#pragma once
// Converted movies (Persona3DS fork). tools/convert_media.py re-encodes each
// PSMF movie with the user's ffmpeg into H.264 Baseline (Annex B, one access
// unit per picture, no B-frames) plus a frame index, and its ATRAC3plus audio
// into IMA-ADPCM. The runtime recognizes a movie by a hash of the PSMF header
// the game passes to sceMpegQueryStreamOffset, then serves picture N (in
// display order) at the game's N-th sceMpegAvcCsc and audio access unit K
// (2048 samples) at its K-th sceMpegAtracDecode, instead of black and silence.
// Decoding is behind VideoDecoder: the PC runner uses libavcodec when it is
// available, the New 3DS its hardware decoder (MVD); without one the picture
// stays black.
//
// Index entry (<media dir>/index.txt, see fork_media.hpp):
//   movie <prefix bytes> <FNV-1a 64 of the PSMF prefix, hex> <video .h264> <frames .idx> <audio .wav or ->
// Frame index (.idx): "P3MV", then u32 version 1, width, height, count, and
// count x (u32 offset, u32 size) of each access unit in the .h264 file.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace psprecomp { class GuestMemory; }

namespace p3p3ds::hle::media {

class ImaTrack;
void forget_picture_writes(); // below

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    // Decodes one Annex B access unit; returns true when it produced a
    // picture, written as width x height PSP 8888 pixels (R, G, B, A bytes).
    virtual bool decode(const std::uint8_t *au, std::size_t size, std::uint32_t *rgba, std::uint32_t width, std::uint32_t height) = 0;
    [[nodiscard]] virtual std::string status() const { return {}; }
};
using VideoDecoderFactory = std::function<std::unique_ptr<VideoDecoder>(std::uint32_t width, std::uint32_t height)>;
// Set by the platform (PC: libavcodec if built with it; 3DS: MVD). Without
// one, movies keep black pictures but still get their converted audio.
void set_video_decoder_factory(VideoDecoderFactory factory);
// P3P3DS: runs a task on another thread (3DS: core 1), false when it cannot. With one, a track decodes
// the access unit after the one the game asked for while the game runs (MovieTrack::kick_ahead);
// the decoder is then used by one thread at a time (the game's thread waits for the task first).
using BackgroundExecutor = std::function<bool(void (*task)(void *), void *arg)>;
void set_background_executor(BackgroundExecutor executor);

class MovieTrack {
public:
    [[nodiscard]] static std::shared_ptr<MovieTrack> open(const std::string &dir, const std::string &video, const std::string &frames,
                                                          const std::string &audio);
    ~MovieTrack();
    MovieTrack(const MovieTrack &) = delete;
    MovieTrack &operator=(const MovieTrack &) = delete;

    // Picture `n` (display order), or null when it cannot be decoded.
    const std::uint32_t *picture(std::uint32_t n);
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t frames() const noexcept { return static_cast<std::uint32_t>(aus_.size()); }
    [[nodiscard]] const std::shared_ptr<ImaTrack> &audio() const noexcept { return audio_; }
    [[nodiscard]] const VideoDecoder *decoder() const noexcept { return decoder_.get(); }
    [[nodiscard]] std::uint64_t units_decoded() const noexcept { return units_; }
    [[nodiscard]] std::uint64_t read_us() const noexcept { return read_us_; }
    [[nodiscard]] std::uint64_t ahead_used() const noexcept { return ahead_used_; } // P3P3DS: pictures decoded ahead
    // P3P3DS: the decode-ahead task is running (it uses the decoder: do not read its status meanwhile).
    [[nodiscard]] bool ahead_busy() const noexcept { return ahead_state_.load(std::memory_order_acquire) == 1; }
    static constexpr std::size_t kReadChunk = 256u * 1024u; // read-ahead of the stream (units are read in order)

private:
    MovieTrack() = default;
    bool decode_next();
    // P3P3DS: decode-ahead (set_background_executor). State 0 idle, 1 running, 2 done, 3 failed;
    // the task owns decoder_, file_, chunk_ and ahead_ while it is 1.
    void kick_ahead();
    int wait_ahead();
    static void ahead_task(void *self);
    std::vector<std::uint32_t> ahead_;
    std::atomic<int> ahead_state_{0};
    std::uint32_t ahead_unit_{};
    std::uint64_t ahead_read_us_{}, ahead_used_{};
    const std::uint8_t *read_unit(std::uint32_t offset, std::uint32_t size, std::uint64_t &read_us); // P3P3DS: read_us per thread
    std::FILE *file_{};
    std::uint32_t width_{}, height_{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> aus_;
    std::shared_ptr<ImaTrack> audio_;
    std::unique_ptr<VideoDecoder> decoder_;
    std::vector<std::uint8_t> au_;
    std::vector<std::uint32_t> rgba_;
    std::uint32_t next_{};      // next access unit to decode
    std::int64_t shown_{-1};    // picture index held in rgba_
    std::uint32_t pictures_{};  // pictures produced since the start
    std::vector<std::uint8_t> chunk_; // stream bytes [chunk_offset_, chunk_offset_ + chunk_.size())
    std::uint64_t chunk_offset_{};
    std::uint64_t units_{}, read_us_{};
};

struct MovieEntry {
    std::uint32_t prefix{};
    std::uint64_t hash{};
    std::string video, frames, audio;
};

struct MovieStats {
    std::uint32_t entries{}, matched{}, unmatched{}, pictures{}, audio_units{}, decode_failures{};
    std::uint64_t decode_us{}, copy_us{}; // host time decoding pictures / copying them in sceMpegAvcCsc
    std::uint64_t units{}, read_us{};     // access units decoded (several per picture when the game skips some), time reading them
};

// Movie state of the game's sceMpeg instances, keyed by the mpeg handle.
class Movies {
public:
    void add_entry(MovieEntry e) { entries_.push_back(std::move(e)); ++stats_.entries; }
    void clear() { entries_.clear(); playing_.clear(); stats_ = {}; forget_picture_writes(); }
    // sceMpegQueryStreamOffset: (re)binds `handle` to the movie whose PSMF prefix matches.
    std::shared_ptr<MovieTrack> bind(psprecomp::GuestMemory &m, std::uint32_t handle, std::uint32_t header, const std::string &dir,
                                     std::string *name = nullptr);
    // sceMpegAvcDecodeYCbCr produced a picture for `handle`.
    void picture_decoded(std::uint32_t handle);
    // sceMpegAvcCsc: the current picture of `handle`, or null.
    const std::uint32_t *current_picture(std::uint32_t handle, std::uint32_t &width, std::uint32_t &height);
    // sceMpegAtracDecode: writes the next 2048 stereo samples of `handle` to `out`; false without converted audio.
    bool next_audio(std::uint32_t handle, std::int16_t *out);
    // sceMpegDelete: the instance's movie ends (its decoder is released).
    void release(std::uint32_t handle) { playing_.erase(handle); forget_picture_writes(); }
    [[nodiscard]] bool playing() const noexcept { return !playing_.empty(); }
    [[nodiscard]] const MovieStats &stats() const noexcept { return stats_; }
    void add_copy_time(std::uint64_t us) noexcept { stats_.copy_us += us; }
    [[nodiscard]] const std::string &decoder_status() const noexcept { return decoder_status_; }

private:
    struct Playing {
        std::shared_ptr<MovieTrack> track;
        std::uint32_t pictures{}, audio_units{};
    };
    std::vector<MovieEntry> entries_;
    std::map<std::uint32_t, Playing> playing_;
    MovieStats stats_{};
    std::string decoder_status_;
};

[[nodiscard]] Movies &fork_movies() noexcept;

// Movie pictures written into guest memory (sceMpegAvcCsc), for the renderer: instead of hashing a
// whole movie texture every frame to see whether it changed, it compares a write generation.
// note_picture_write is called on the game's thread after a picture's rows were written (address of
// the first texel of row 0, row stride in texels); picture_write_generation may be called from
// another thread (the 3DS GE worker) and gives the generation of the last write at exactly that
// address and stride. Generations only grow, across movies; forget_picture_writes (a movie ends or a
// new one starts) drops every address, so their textures go back to content hashing.
// rows: how many rows from row 0 hold the picture (0: unknown); the renderer uploads only those
// (Persona: 272 rows of a 512-row texture, docs/CURRENT_STATE.md 2026-10-14).
void note_picture_write(std::uint32_t address, std::uint32_t stride, std::uint32_t rows = 0u);
[[nodiscard]] bool picture_write_generation(std::uint32_t address, std::uint32_t stride, std::uint64_t &generation,
                                            std::uint32_t *rows = nullptr);
void forget_picture_writes();

} // namespace p3p3ds::hle::media
