#pragma once
// Persona3DS fork: converted-movie decoding with the New 3DS hardware H.264
// decoder (MVDSTD), behind fork_movie.hpp's VideoDecoder. Included by
// main.cpp only (keeps the runner's object list unchanged for the builder).
// Sequence from devkitPro 3ds-examples mvd/source/main.c and libctru
// include/3ds/services/mvd.h: NAL units are fed with a 3-byte 00 00 01 prefix
// from linear memory, then the picture is rendered to a 16-bit buffer.
// Output type MVD_OUTPUT_BGR565: in libctru's naming that is red in bits
// 11-15 (the layout lut_ below expects). [VERIFIED] devkitPro 3ds-examples
// mvd/source/main.c decodes H.264 with MVD_OUTPUT_BGR565 straight into a
// GSP_RGB565_OES framebuffer (red in the high bits). MVD_OUTPUT_RGB565 gave
// red and blue swapped on the maintainer's New 3DS (blue faces in an FMV).
// [VERIFIED] maintainer's New 3DS: mvd:STD is reachable from the .3dsx and
// the work buffer from mvdstdCalculateBufferSize (3,063 KiB at level 3.0).
#include "p3p3ds/hle/fork_movie.hpp"

#include <3ds.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace p3p3ds::n3ds {

class MvdDecoder final : public hle::media::VideoDecoder {
public:
    MvdDecoder(std::uint32_t width, std::uint32_t height) : width_(width), height_(height) {
#ifdef MVD_CALC_WITH_LEVEL_FLAG_ENABLE_CALC // libctru with mvdstdCalculateBufferSize (newer than some installs)
        MVDSTD_CalculateWorkBufSizeConfig calc{};
        calc.level.enable = 1;
        calc.level.flag = MVD_CALC_WITH_LEVEL_FLAG_ENABLE_CALC;
        calc.level.level = MVD_H264_LEVEL_3_0; // tools/convert_media.py encodes level 3.0
        calc.width = width;
        calc.height = height;
        if (R_FAILED(mvdstdCalculateBufferSize(&calc, &workbuf_))) workbuf_ = 0u;
#endif
        if (workbuf_ == 0u) workbuf_ = MVD_DEFAULT_WORKBUF_SIZE;
        in_ = static_cast<std::uint8_t *>(linearMemAlign(kInBytes, 0x40));
        out_ = static_cast<std::uint16_t *>(linearMemAlign(width * height * 2u, 0x40));
        if (in_ == nullptr || out_ == nullptr) { status_ = "mvd: no linear memory for buffers"; return; }
        init_ = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565, workbuf_, nullptr); // see the header comment
        char text[96];
        // Persona3DS: linear memory left after the init (-1: libctru's linearAlloc of the work buffer failed)
        std::snprintf(text, sizeof text, "mvd init %08lx work %lu KiB, linear free %lu KiB", static_cast<unsigned long>(init_),
                      static_cast<unsigned long>(workbuf_ / 1024u), static_cast<unsigned long>(linearSpaceFree() / 1024u));
        status_ = text;
        if (R_FAILED(init_)) return;
        mvdstdGenerateDefaultConfig(&config_, width, height, width, height, nullptr, reinterpret_cast<u32 *>(out_), reinterpret_cast<u32 *>(out_));
        ready_ = true;
    }
    ~MvdDecoder() override {
        if (ready_) mvdstdExit();
        if (in_ != nullptr) linearFree(in_);
        if (out_ != nullptr) linearFree(out_);
    }

    bool decode(const std::uint8_t *au, std::size_t size, std::uint32_t *rgba, std::uint32_t width, std::uint32_t height) override {
        if (!ready_) return false;
        bool slice = false;
        const u64 hw_start = svcGetSystemTick(); // Persona3DS: hardware (process + render) vs CPU conversion time
        const auto start_code = [&](std::size_t from) {
            for (std::size_t k = from; k + 3 <= size; ++k)
                if (au[k] == 0 && au[k + 1] == 0 && au[k + 2] == 1) return k;
            return size;
        };
        for (std::size_t i = start_code(0); i + 3 < size;) {
            // NAL unit from its 00 00 01 start code to the next one (minus the extra zero of a 4-byte code).
            const std::size_t next = start_code(i + 3);
            const std::size_t end = next < size && au[next - 1] == 0 ? next - 1 : next;
            const std::uint32_t type = au[i + 3] & 0x1Fu;
            const std::size_t length = end - i;
            if (type != 9u && type != 6u && length <= kInBytes) { // not AUD / SEI
                std::memcpy(in_, au + i, length);
                GSPGPU_FlushDataCache(in_, length);
                if (length > max_nal_) max_nal_ = static_cast<std::uint32_t>(length);
                // P3P3DS: a unit the decoder did not take whole (INCOMPLETEPROCESSING, `remaining_size` bytes
                // left) is handed in again from where it stopped. Measured on the New 3DS (docs/CURRENT_STATE.md
                // 2026-10-14): the opening's first picture (the 12,936-byte IDR slice) came back with all of
                // it left; skipping it left the following pictures without a reference until the next
                // keyframe (the washed-out start).
                std::size_t left = length;
                for (int attempt = 0;; ++attempt) {
                    MVDSTD_ProcessNALUnitOut out{};
                    const Result r = mvdstdProcessVideoFrame(in_, left, 0, &out);
                    if (!MVD_CHECKNALUPROC_SUCCESS(r)) { note_error("process", r); return false; }
                    if (r != MVD_STATUS_INCOMPLETEPROCESSING) {
                        if (attempt != 0) ++resent_ok_;
                        break;
                    }
                    if (incomplete_++ == 0u) first_incomplete_ = decoded_;
                    if (out.remaining_size > max_remaining_) max_remaining_ = out.remaining_size;
                    if (out.remaining_size == 0u || out.remaining_size > left || attempt >= 7) break;
                    if (out.remaining_size < left) std::memmove(in_, in_ + (left - out.remaining_size), out.remaining_size);
                    left = out.remaining_size;
                    GSPGPU_FlushDataCache(in_, left);
                    ++resent_;
                }
                if (type == 1u || type == 5u) slice = true;
            }
            i = next;
        }
        if (!slice) return false;
        const Result r = mvdstdRenderVideoFrame(&config_, true);
        if (r != MVD_STATUS_OK) { note_error("render", r); return false; }
        GSPGPU_InvalidateDataCache(out_, width_ * height_ * 2u);
        const u64 conv_start = svcGetSystemTick();
        hw_ticks_ += conv_start - hw_start;
        const std::uint32_t w = width < width_ ? width : width_, h = height < height_ ? height : height_;
        if (lut_.empty()) { // red-high 565 (MVD_OUTPUT_BGR565) -> PSP 8888, one lookup per pixel (256 KiB)
            lut_.resize(65536);
            for (std::uint32_t v = 0; v < 65536u; ++v) {
                const std::uint32_t r5 = v >> 11, g6 = (v >> 5) & 63u, b5 = v & 31u;
                lut_[v] = 0xFF000000u | ((b5 << 3 | b5 >> 2) << 16) | ((g6 << 2 | g6 >> 4) << 8) | (r5 << 3 | r5 >> 2);
            }
        }
        for (std::uint32_t y = 0; y < h; ++y) {
            const std::uint16_t *src = out_ + y * width_;
            std::uint32_t *dst = rgba + y * width;
            for (std::uint32_t x = 0; x < w; ++x) dst[x] = lut_[src[x]];
        }
        conv_ticks_ += svcGetSystemTick() - conv_start;
        ++timed_;
        ++decoded_;
        return true;
    }
    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] std::string status() const override {
        if (timed_ == 0u) return status_;
        char text[160]; // per picture: MVD process + render (includes waiting for the decoder), RGB565 -> 8888 table
        const double ms = 1000.0 / static_cast<double>(SYSCLOCK_ARM11) / static_cast<double>(timed_);
        std::snprintf(text, sizeof text, "; hw %.2f conv %.2f ms; nal max %lu B, incomplete %lu (first pic %lu, left max %lu B), resent %lu ok %lu",
                      static_cast<double>(hw_ticks_) * ms, static_cast<double>(conv_ticks_) * ms, static_cast<unsigned long>(max_nal_),
                      static_cast<unsigned long>(incomplete_), static_cast<unsigned long>(first_incomplete_),
                      static_cast<unsigned long>(max_remaining_), static_cast<unsigned long>(resent_), static_cast<unsigned long>(resent_ok_));
        return status_ + text;
    }

private:
    static constexpr std::size_t kInBytes = 0x80000;
    void note_error(const char *what, Result r) {
        char text[96];
        std::snprintf(text, sizeof text, "mvd %s %08lx (work %lu KiB)", what, static_cast<unsigned long>(r),
                      static_cast<unsigned long>(workbuf_ / 1024u));
        status_ = text;
    }
    std::uint32_t width_, height_;
    u32 workbuf_{};
    std::uint8_t *in_{};
    std::uint16_t *out_{};
    MVDSTD_Config config_{};
    Result init_{-1};
    bool ready_{};
    std::string status_;
    std::vector<std::uint32_t> lut_;
    u64 hw_ticks_{}, conv_ticks_{};
    std::uint64_t timed_{};
    std::uint32_t max_nal_{}, incomplete_{}, first_incomplete_{}, max_remaining_{}, decoded_{}, resent_{}, resent_ok_{}; // P3P3DS
};

// Persona3DS: make_room frees linear memory (the GPU renderer's unused textures) and is called once when
// the decoder could not start: mvdstdInit's 3 MiB work buffer comes from linear memory, which textures fill
// during play (hospital cutscene: "mvd init ffffffff", the movie stayed black; docs/CURRENT_STATE.md 2026-10-14).
inline void install_mvd_decoder(std::function<void()> make_room = {}) {
    hle::media::set_video_decoder_factory([make_room](std::uint32_t w, std::uint32_t h) -> std::unique_ptr<hle::media::VideoDecoder> {
        auto decoder = std::make_unique<MvdDecoder>(w, h);
        if (!decoder->ready() && make_room) {
            decoder.reset(); // its buffers and the MVD service go first
            make_room();
            decoder = std::make_unique<MvdDecoder>(w, h);
        }
        return decoder;
    });
}

} // namespace p3p3ds::n3ds
