#pragma once
// Persona3DS fork: sound output on the 3DS DSP (NDSP). Until now the 3DS
// runner had no audio backend (docs/3DS_PLATFORM.md: "no audio"); the game's
// sceAudio output (sound effects, converted music and movie audio) was mixed
// for nobody. Included by main.cpp only.
//
// The kernel's PCM sink (hle/audio.hpp) hands every accepted buffer with its
// start on the virtual clock. Buffers are mixed into a ring indexed by virtual
// sample number; once the virtual clock has passed a block's end it is final
// and copied to an NDSP wave buffer: a channel's buffer starts at or after the
// virtual time it is submitted (core/src/hle/audio.cpp AudioState::output:
// start = max(now, play_end)), so no data can still arrive for it. At most
// kMaxQueued blocks wait in the DSP queue. The play cursor only advances when
// the clock does: when the game runs slower than real time the sound has gaps
// instead of drifting, and when the cursor falls more than kMaxBehind behind
// the clock it skips forward (sound in step with the picture). Needs sdmc:/3ds/dspfirm.cdc (DSP
// firmware dumped by the user, as for every homebrew with sound).
#include <3ds.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace p3p3ds::n3ds {

class NdspOutput {
public:
    static constexpr std::uint32_t kRate = 44100u;
    static constexpr std::uint32_t kBlock = 1024u;      // frames per NDSP wave buffer (23 ms)
    static constexpr std::uint32_t kBuffers = 8u;       // 186 ms queued at most
    // Persona3DS: blocks are final by the virtual clock (pump). Earlier rule: final once data had
    // arrived kLeadFrames past the block; 2048 dropped a third of the PCM as late, 8192 (186 ms) none,
    // but with up to kBuffers queued the sound lagged the picture by up to about 370 ms (maintainer:
    // "audio a little desynchronized").
    static constexpr std::uint32_t kMaxQueued = 4u;            // blocks in the DSP queue (93 ms)
    static constexpr std::uint32_t kMaxBehind = 8u * kBlock;   // cursor more than 186 ms behind the clock: skip
    static constexpr std::uint32_t kRingFrames = 1u << 16; // 1.49 s

    NdspOutput() {
        init_ = ndspInit();
        if (R_FAILED(init_)) return;
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspChnReset(0);
        ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
        ndspChnSetRate(0, static_cast<float>(kRate));
        ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
        float mix[12] = {1.0f, 1.0f};
        ndspChnSetMix(0, mix);
        pcm_ = static_cast<std::int16_t *>(linearAlloc(kBuffers * kBlock * 4u));
        if (pcm_ == nullptr) { ndspExit(); init_ = -1; return; }
        std::memset(wave_, 0, sizeof wave_);
        for (std::uint32_t i = 0; i < kBuffers; ++i) {
            wave_[i].data_pcm16 = pcm_ + i * kBlock * 2u;
            wave_[i].nsamples = kBlock;
            wave_[i].status = NDSP_WBUF_FREE;
        }
        ring_.assign(static_cast<std::size_t>(kRingFrames) * 2u, 0);
        ready_ = true;
    }
    ~NdspOutput() {
        if (!ready_) return;
        ndspChnWaveBufClear(0);
        ndspExit();
        linearFree(pcm_);
    }
    NdspOutput(const NdspOutput &) = delete;
    NdspOutput &operator=(const NdspOutput &) = delete;

    // The kernel's PCM sink (main thread, inside sceAudio calls).
    void add(std::uint64_t start_us, const std::vector<std::int16_t> &stereo) {
        if (!ready_) return;
        const std::uint64_t start = start_us * kRate / 1000000u, frames = stereo.size() / 2u;
        if (!started_) { play_ = start; started_ = true; }
        if (start + frames > newest_) newest_ = start + frames;
        // Persona3DS: falling behind the clock is handled in pump(). Here only data that would not fit
        // the ring (more than kRingFrames past the cursor) is dropped, counted as overflow: the earlier
        // rule (skip when the newest buffer is 0.7 s past the cursor) also fired for one large buffer
        // submitted ahead and threw away sound not yet played.
        for (std::uint64_t f = 0; f < frames; ++f) {
            const std::uint64_t at = start + f;
            if (at < play_) { ++late_; continue; }
            if (at >= play_ + kRingFrames) { ++overflow_; continue; }
            const std::size_t i = static_cast<std::size_t>(at % kRingFrames) * 2u;
            ring_[i] += stereo[2u * f];
            ring_[i + 1u] += stereo[2u * f + 1u];
        }
    }

    // Once per vblank on the main thread, with the virtual clock: queue every block the clock has
    // passed, up to kMaxQueued in the DSP queue.
    void pump(std::uint64_t now_us) {
        if (!ready_ || !started_) return;
        const std::uint64_t now = now_us * kRate / 1000000u;
        if (now > play_ + kMaxBehind) { // far behind the picture (slow stretch, stalls): jump to it
            const std::uint64_t to = now - kBlock;
            for (std::uint64_t f = play_; f < to && f < play_ + kRingFrames; ++f) {
                const std::size_t i = static_cast<std::size_t>(f % kRingFrames) * 2u;
                ring_[i] = ring_[i + 1u] = 0;
            }
            skipped_ += to - play_;
            play_ = to;
        }
        std::uint32_t in_flight = 0;
        for (const auto &b : wave_) in_flight += b.status == NDSP_WBUF_QUEUED || b.status == NDSP_WBUF_PLAYING ? 1u : 0u;
        // An empty queue (start, or after a gap) refills only once two blocks are final: a one-block
        // cushion against the vblank (16.7 ms) and block (23.2 ms) rhythms meeting badly.
        if (in_flight == 0u && play_ + 2u * kBlock > now) return;
        for (std::uint32_t n = 0; n < kBuffers; ++n) {
            if (play_ + kBlock > now) return;            // the block is not over on the virtual clock yet
            if (in_flight >= kMaxQueued) return;         // enough queued
            ndspWaveBuf &w = wave_[next_];
            if (w.status != NDSP_WBUF_FREE && w.status != NDSP_WBUF_DONE) return; // all queued
            if (queued_ != 0u && !ndspChnIsPlaying(0)) ++gaps_;
            std::int16_t *out = w.data_pcm16;
            for (std::uint32_t f = 0; f < kBlock; ++f) {
                const std::size_t i = static_cast<std::size_t>((play_ + f) % kRingFrames) * 2u;
                out[2u * f] = clamp16(ring_[i]);
                out[2u * f + 1u] = clamp16(ring_[i + 1u]);
                ring_[i] = ring_[i + 1u] = 0;
            }
            DSP_FlushDataCache(out, kBlock * 4u);
            ndspChnWaveBufAdd(0, &w);
            play_ += kBlock;
            ++queued_;
            ++in_flight;
            next_ = (next_ + 1u) % kBuffers;
        }
    }

    [[nodiscard]] std::string status() const {
        char text[128];
        if (!ready_)
            std::snprintf(text, sizeof text, "ndsp init %08lx (needs sdmc:/3ds/dspfirm.cdc)", static_cast<unsigned long>(init_));
        else
            std::snprintf(text, sizeof text, "ndsp ok, %lu blocks, %llu late, %llu skipped, %llu overflow, %lu gaps", static_cast<unsigned long>(queued_),
                          static_cast<unsigned long long>(late_), static_cast<unsigned long long>(skipped_),
                          static_cast<unsigned long long>(overflow_), static_cast<unsigned long>(gaps_));
        return text;
    }

private:
    static std::int16_t clamp16(std::int32_t v) { return static_cast<std::int16_t>(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
    Result init_{-1};
    bool ready_{}, started_{};
    std::int16_t *pcm_{};
    ndspWaveBuf wave_[kBuffers];
    std::vector<std::int32_t> ring_;
    std::uint64_t play_{}, newest_{}, late_{}, skipped_{}, overflow_{};
    std::uint32_t next_{}, queued_{}, gaps_{};
};

} // namespace p3p3ds::n3ds
