// Converted media for the Persona3DS fork: see fork_media.hpp.
#include "p3p3ds/hle/fork_media.hpp"

#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace p3p3ds::hle::media {
namespace {

// IMA ADPCM step tables (IMA Digital Audio Focus and Technical Working Group,
// "Recommended Practices for Enhancing Digital Audio Compatibility", 1992).
constexpr std::int16_t kStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr std::int8_t kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

std::uint32_t le32(const std::uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24; }
std::uint16_t le16(const std::uint8_t *p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }

struct ImaState {
    int predictor, index;
    std::int16_t step(int nibble) {
        const int s = kStep[index];
        int diff = s >> 3;
        if (nibble & 1) diff += s >> 2;
        if (nibble & 2) diff += s >> 1;
        if (nibble & 4) diff += s;
        predictor = std::clamp(nibble & 8 ? predictor - diff : predictor + diff, -32768, 32767);
        index = std::clamp(index + kIndex[nibble], 0, 88);
        return static_cast<std::int16_t>(predictor);
    }
};

Library g_media;

} // namespace

std::uint64_t fnv1a64(const std::uint8_t *data, std::size_t size) noexcept {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
    return h;
}

std::shared_ptr<ImaTrack> ImaTrack::open(const std::string &path) {
    std::shared_ptr<ImaTrack> t(new ImaTrack());
    t->file_ = std::fopen(path.c_str(), "rb");
    if (t->file_ == nullptr) return nullptr;
    std::uint8_t head[12];
    if (std::fread(head, 1, 12, t->file_) != 12 || std::memcmp(head, "RIFF", 4) != 0 || std::memcmp(head + 8, "WAVE", 4) != 0)
        return nullptr;
    std::uint64_t pos = 12, fact = 0;
    bool fmt = false;
    for (;;) {
        std::uint8_t ck[8];
        if (std::fseek(t->file_, static_cast<long>(pos), SEEK_SET) != 0 || std::fread(ck, 1, 8, t->file_) != 8) return nullptr;
        const std::uint32_t size = le32(ck + 4);
        if (std::memcmp(ck, "fmt ", 4) == 0) {
            std::uint8_t f[16];
            if (size < 16 || std::fread(f, 1, 16, t->file_) != 16) return nullptr;
            if (le16(f) != 0x11u) return nullptr; // WAVE_FORMAT_IMA_ADPCM
            t->channels_ = le16(f + 2);
            t->block_align_ = le16(f + 12);
            if ((t->channels_ != 1u && t->channels_ != 2u) || t->block_align_ <= 4u * t->channels_ ||
                (t->block_align_ - 4u * t->channels_) % (4u * t->channels_) != 0u)
                return nullptr;
            t->per_block_ = 1u + (t->block_align_ - 4u * t->channels_) * 2u / t->channels_;
            fmt = true;
        } else if (std::memcmp(ck, "fact", 4) == 0 && size >= 4) {
            std::uint8_t f[4];
            if (std::fread(f, 1, 4, t->file_) != 4) return nullptr;
            fact = le32(f);
        } else if (std::memcmp(ck, "data", 4) == 0) {
            if (!fmt) return nullptr;
            t->data_off_ = pos + 8;
            t->data_size_ = size;
            break;
        }
        pos += 8u + size + (size & 1u);
    }
    const std::uint64_t blocks = t->data_size_ / t->block_align_;
    t->total_ = fact != 0 ? std::min<std::uint64_t>(fact, blocks * t->per_block_) : blocks * t->per_block_;
    t->raw_.resize(t->block_align_);
    t->pcm_.resize(static_cast<std::size_t>(t->per_block_) * t->channels_);
    return t;
}

ImaTrack::~ImaTrack() {
    if (file_ != nullptr) std::fclose(file_);
}

bool ImaTrack::decode_block(std::uint64_t block) {
    if (block == cached_block_) return true;
    cached_block_ = ~0ull;
    if (std::fseek(file_, static_cast<long>(data_off_ + block * block_align_), SEEK_SET) != 0 ||
        std::fread(raw_.data(), 1, block_align_, file_) != block_align_)
        return false;
    const std::uint32_t ch = channels_;
    ImaState s[2]{};
    for (std::uint32_t c = 0; c < ch; ++c) {
        s[c].predictor = static_cast<std::int16_t>(le16(&raw_[c * 4u]));
        s[c].index = std::clamp<int>(raw_[c * 4u + 2u], 0, 88);
        pcm_[c] = static_cast<std::int16_t>(s[c].predictor);
    }
    // Then groups of 4 bytes (8 samples, low nibble first) per channel, channels alternating.
    const std::uint8_t *p = &raw_[4u * ch];
    for (std::uint32_t group = 0; 1u + group * 8u < per_block_; ++group)
        for (std::uint32_t c = 0; c < ch; ++c)
            for (std::uint32_t b = 0; b < 4u; ++b, ++p) {
                const std::uint32_t frame = 1u + group * 8u + b * 2u;
                pcm_[frame * ch + c] = s[c].step(*p & 0x0F);
                pcm_[(frame + 1u) * ch + c] = s[c].step(*p >> 4);
            }
    cached_block_ = block;
    return true;
}

void ImaTrack::read_stereo(std::int64_t position, std::uint32_t count, std::int16_t *out) {
    for (std::uint32_t n = 0; n < count;) {
        const std::int64_t at = position + n;
        if (at < 0 || static_cast<std::uint64_t>(at) >= total_ || !decode_block(static_cast<std::uint64_t>(at) / per_block_)) {
            out[2u * n] = out[2u * n + 1u] = 0;
            ++n;
            continue;
        }
        const std::uint32_t first = static_cast<std::uint32_t>(static_cast<std::uint64_t>(at) % per_block_);
        const std::uint64_t left = std::min<std::uint64_t>(per_block_ - first, total_ - static_cast<std::uint64_t>(at));
        const std::uint32_t take = static_cast<std::uint32_t>(std::min<std::uint64_t>(left, count - n));
        for (std::uint32_t k = 0; k < take; ++k) {
            const std::int16_t *f = &pcm_[static_cast<std::size_t>(first + k) * channels_];
            out[2u * (n + k)] = f[0];
            out[2u * (n + k) + 1u] = channels_ == 2u ? f[1] : f[0];
        }
        n += take;
    }
}

void Library::set_dir(const std::string &dir) {
    dir_ = dir;
    atrac_.clear();
    stats_ = {};
    std::ifstream in(dir + "/index.txt");
    std::string line;
    while (std::getline(in, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        std::istringstream fields(line);
        std::string kind, hex;
        AtracEntry e;
        if (!(fields >> kind) || kind != "atrac") continue;
        if (!(fields >> e.prefix >> hex >> e.offset)) continue;
        std::getline(fields >> std::ws, e.file);
        while (!e.file.empty() && (e.file.back() == '\r' || e.file.back() == ' ')) e.file.pop_back();
        if (e.prefix == 0u || e.file.empty()) continue;
        e.hash = std::stoull(hex, nullptr, 16);
        add_atrac_entry(std::move(e));
    }
}

std::shared_ptr<ImaTrack> Library::find_atrac(psprecomp::GuestMemory &m, std::uint32_t buffer, std::uint32_t size,
                                              std::int32_t &offset, std::string *name) {
    if (atrac_.empty()) return nullptr;
    for (const auto &e : atrac_) {
        if (e.prefix > size) continue;
        const std::uint8_t *p = m.raw_pointer(buffer, e.prefix);
        if (p == nullptr || fnv1a64(p, e.prefix) != e.hash) continue;
        auto track = ImaTrack::open(dir_ + "/" + e.file);
        if (track == nullptr) {
            ++stats_.open_failures;
            return nullptr;
        }
        ++stats_.matched;
        offset = e.offset;
        if (name != nullptr) *name = e.file;
        return track;
    }
    ++stats_.unmatched;
    return nullptr;
}

Library &fork_media() noexcept { return g_media; }

} // namespace p3p3ds::hle::media
