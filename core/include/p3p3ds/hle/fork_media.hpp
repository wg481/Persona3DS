#pragma once
// Converted media (Persona3DS fork): music the game would decode with its
// ATRAC codec, converted ahead of time by tools/convert_media.py with the
// maintainer's ffmpeg into IMA-ADPCM WAV files plus an index. The runtime
// recognizes a track by a hash of the start of the .at3 file the game hands
// to sceAtrac (header and first frames), then serves PCM from the converted
// file at the sample position the ATRAC library is decoding.
//
// Index (<media dir>/index.txt), one entry per line, '#' starts a comment:
//   atrac <prefix bytes> <FNV-1a 64 of those bytes, hex> <sample offset> <file>
// The sample offset is added to the ATRAC library's raw sample position
// (decodePos) to get the position in the converted file.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace psprecomp { class GuestMemory; }

namespace p3p3ds::hle::media {

[[nodiscard]] std::uint64_t fnv1a64(const std::uint8_t *data, std::size_t size) noexcept;

// A WAV file with IMA-ADPCM data (format 0x11, as written by ffmpeg
// adpcm_ima_wav), read block by block.
class ImaTrack {
public:
    [[nodiscard]] static std::shared_ptr<ImaTrack> open(const std::string &path);
    ~ImaTrack();
    ImaTrack(const ImaTrack &) = delete;
    ImaTrack &operator=(const ImaTrack &) = delete;

    // Writes `count` stereo S16 frames from sample `position` (negative or past
    // the end: silence). A mono file is written to both channels.
    void read_stereo(std::int64_t position, std::uint32_t count, std::int16_t *out);
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::uint64_t samples() const noexcept { return total_; }

private:
    ImaTrack() = default;
    bool decode_block(std::uint64_t block);
    std::FILE *file_{};
    std::uint32_t channels_{}, block_align_{}, per_block_{};
    std::uint64_t data_off_{}, data_size_{}, total_{};
    std::uint64_t cached_block_{~0ull};
    std::vector<std::uint8_t> raw_;
    std::vector<std::int16_t> pcm_; // per_block_ frames, interleaved
};

struct AtracEntry {
    std::uint32_t prefix{};
    std::uint64_t hash{};
    std::int32_t offset{};
    std::string file;
};

struct Stats {
    std::uint32_t entries{}, matched{}, unmatched{}, open_failures{};
};

class Library {
public:
    // Loads <dir>/index.txt; a missing index means no converted media.
    void set_dir(const std::string &dir);
    void add_atrac_entry(AtracEntry entry) { atrac_.push_back(std::move(entry)); ++stats_.entries; }
    // The converted track whose prefix hash matches the `size` bytes at `buffer`, or null.
    std::shared_ptr<ImaTrack> find_atrac(psprecomp::GuestMemory &m, std::uint32_t buffer, std::uint32_t size,
                                         std::int32_t &offset, std::string *name = nullptr);
    [[nodiscard]] const Stats &stats() const noexcept { return stats_; }
    [[nodiscard]] const std::string &dir() const noexcept { return dir_; }

private:
    std::string dir_;
    std::vector<AtracEntry> atrac_;
    Stats stats_{};
};

[[nodiscard]] Library &fork_media() noexcept;

} // namespace p3p3ds::hle::media
