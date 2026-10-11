#pragma once
// Persona3DS fork: converted-movie decoding with the New 3DS hardware H.264
// decoder (MVDSTD), behind fork_movie.hpp's VideoDecoder. Included by
// main.cpp only (keeps the runner's object list unchanged for the builder).
// Sequence from devkitPro 3ds-examples mvd/source/main.c and libctru
// include/3ds/services/mvd.h: NAL units are fed with a 3-byte 00 00 01 prefix
// from linear memory, then the picture is rendered to an RGB565 buffer.
// [UNVERIFIED] on hardware: access to mvd:STD from a .3dsx, the work-buffer
// size from mvdstdCalculateBufferSize, and the RGB565 channel order.
#include "p3p3ds/hle/fork_movie.hpp"

#include <3ds.h>

#include <cstdio>
#include <cstring>
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
        init_ = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_RGB565, workbuf_, nullptr);
        char text[96];
        std::snprintf(text, sizeof text, "mvd init %08lx work %lu KiB", static_cast<unsigned long>(init_),
                      static_cast<unsigned long>(workbuf_ / 1024u));
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
                const Result r = mvdstdProcessVideoFrame(in_, length, 0, nullptr);
                if (!MVD_CHECKNALUPROC_SUCCESS(r)) { note_error("process", r); return false; }
                if (type == 1u || type == 5u) slice = true;
            }
            i = next;
        }
        if (!slice) return false;
        const Result r = mvdstdRenderVideoFrame(&config_, true);
        if (r != MVD_STATUS_OK) { note_error("render", r); return false; }
        GSPGPU_InvalidateDataCache(out_, width_ * height_ * 2u);
        const std::uint32_t w = width < width_ ? width : width_, h = height < height_ ? height : height_;
        if (lut_.empty()) { // RGB565 -> PSP 8888, one lookup per pixel (256 KiB)
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
        return true;
    }
    [[nodiscard]] std::string status() const override { return status_; }

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
};

inline void install_mvd_decoder() {
    hle::media::set_video_decoder_factory([](std::uint32_t w, std::uint32_t h) { return std::make_unique<MvdDecoder>(w, h); });
}

} // namespace p3p3ds::n3ds
