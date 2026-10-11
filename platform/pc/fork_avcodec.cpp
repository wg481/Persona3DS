// Persona3DS fork: converted-movie decoding on the PC runner with the user's
// libavcodec (fork_movie.hpp VideoDecoder). Built only when CMake finds
// libavcodec through pkg-config (P3P_HAVE_AVCODEC); otherwise movies stay black.
#include "p3p3ds/hle/fork_movie.hpp"

#include <algorithm>
#include <memory>
#include <string>

#ifdef P3P_HAVE_AVCODEC
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

namespace {

std::uint8_t clamp8(int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); }

class AvcodecDecoder final : public p3p3ds::hle::media::VideoDecoder {
public:
    AvcodecDecoder() {
        if (const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264)) {
            ctx_ = avcodec_alloc_context3(codec);
            ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
            ctx_->thread_count = 1;
            if (avcodec_open2(ctx_, codec, nullptr) < 0) avcodec_free_context(&ctx_);
        }
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
    }
    ~AvcodecDecoder() override {
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        avcodec_free_context(&ctx_);
    }
    bool decode(const std::uint8_t *au, std::size_t size, std::uint32_t *rgba, std::uint32_t width, std::uint32_t height) override {
        if (ctx_ == nullptr) return false;
        packet_->data = const_cast<std::uint8_t *>(au);
        packet_->size = static_cast<int>(size);
        if (avcodec_send_packet(ctx_, packet_) < 0) return false;
        if (avcodec_receive_frame(ctx_, frame_) < 0) return false;
        if (frame_->format != AV_PIX_FMT_YUV420P && frame_->format != AV_PIX_FMT_YUVJ420P) return false;
        // BT.601 limited range to RGB (the PSP's movies and x264's default), written as PSP 8888.
        const std::uint32_t w = std::min<std::uint32_t>(width, static_cast<std::uint32_t>(frame_->width));
        const std::uint32_t h = std::min<std::uint32_t>(height, static_cast<std::uint32_t>(frame_->height));
        for (std::uint32_t y = 0; y < h; ++y) {
            const std::uint8_t *Y = frame_->data[0] + y * frame_->linesize[0];
            const std::uint8_t *U = frame_->data[1] + (y / 2) * frame_->linesize[1];
            const std::uint8_t *V = frame_->data[2] + (y / 2) * frame_->linesize[2];
            for (std::uint32_t x = 0; x < w; ++x) {
                const int c = (Y[x] - 16) * 298, d = U[x / 2] - 128, e = V[x / 2] - 128;
                const std::uint32_t r = clamp8((c + 409 * e + 128) >> 8), g = clamp8((c - 100 * d - 208 * e + 128) >> 8),
                                    b = clamp8((c + 516 * d + 128) >> 8);
                rgba[y * width + x] = 0xFF000000u | b << 16 | g << 8 | r;
            }
        }
        av_frame_unref(frame_);
        return true;
    }
    [[nodiscard]] std::string status() const override { return ctx_ != nullptr ? "libavcodec" : "libavcodec: no H.264 decoder"; }

private:
    AVCodecContext *ctx_{};
    AVFrame *frame_{};
    AVPacket *packet_{};
};

} // namespace

namespace p3p3ds::pc {
bool install_movie_decoder() {
    p3p3ds::hle::media::set_video_decoder_factory([](std::uint32_t, std::uint32_t) { return std::make_unique<AvcodecDecoder>(); });
    return true;
}
} // namespace p3p3ds::pc
#else
namespace p3p3ds::pc {
bool install_movie_decoder() { return false; }
} // namespace p3p3ds::pc
#endif
