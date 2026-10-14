// Converted movies for the Persona3DS fork: see fork_movie.hpp.
#include "p3p3ds/hle/fork_movie.hpp"

#include "p3p3ds/hle/fork_media.hpp"
#include "psprecomp/guest_memory.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>

namespace p3p3ds::hle::media {
namespace {

VideoDecoderFactory g_factory;
Movies g_movies;

struct PictureWrite {
    std::uint32_t address{}, stride{};
    std::uint64_t generation{}; // 0: free slot
    std::uint32_t rows{};       // rows written from row 0 (0: unknown)
};
std::mutex g_writes_lock;
std::array<PictureWrite, 4> g_writes{}; // Persona double-buffers its movie texture: 2 addresses
std::uint64_t g_last_generation = 0;
constexpr std::uint32_t kPsmfPrefixMax = 0x800u; // the PSMF header the game reads before the stream

std::uint32_t le32(const std::uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24; }

} // namespace

void set_video_decoder_factory(VideoDecoderFactory factory) { g_factory = std::move(factory); }

std::shared_ptr<MovieTrack> MovieTrack::open(const std::string &dir, const std::string &video, const std::string &frames,
                                             const std::string &audio) {
    std::shared_ptr<MovieTrack> t(new MovieTrack());
    std::FILE *idx = std::fopen((dir + "/" + frames).c_str(), "rb");
    if (idx == nullptr) return nullptr;
    std::uint8_t head[20];
    const bool ok = std::fread(head, 1, 20, idx) == 20 && std::memcmp(head, "P3MV", 4) == 0 && le32(head + 4) == 1u;
    if (ok) {
        t->width_ = le32(head + 8);
        t->height_ = le32(head + 12);
        t->aus_.resize(le32(head + 16));
        for (auto &au : t->aus_) {
            std::uint8_t e[8];
            if (std::fread(e, 1, 8, idx) != 8) { t->aus_.clear(); break; }
            au = {le32(e), le32(e + 4)};
        }
    }
    std::fclose(idx);
    if (!ok || t->aus_.empty() || t->width_ == 0u || t->height_ == 0u || t->width_ > 1024u || t->height_ > 1024u) return nullptr;
    t->file_ = std::fopen((dir + "/" + video).c_str(), "rb");
    if (t->file_ == nullptr) return nullptr;
    std::setvbuf(t->file_, nullptr, _IONBF, 0); // read_unit does its own read-ahead
    if (!audio.empty() && audio != "-") t->audio_ = ImaTrack::open(dir + "/" + audio);
    t->rgba_.assign(static_cast<std::size_t>(t->width_) * t->height_, 0xFF000000u);
    if (g_factory) t->decoder_ = g_factory(t->width_, t->height_);
    return t;
}

MovieTrack::~MovieTrack() {
    if (file_ != nullptr) std::fclose(file_);
}

// The bytes of one access unit: from the read-ahead chunk, refilled from `offset` when the unit is not
// inside it (one SD read per kReadChunk of stream instead of a seek and read per picture); a unit
// larger than the chunk is read on its own.
const std::uint8_t *MovieTrack::read_unit(std::uint32_t offset, std::uint32_t size) {
    if (offset >= chunk_offset_ && offset + static_cast<std::uint64_t>(size) <= chunk_offset_ + chunk_.size())
        return chunk_.data() + (offset - chunk_offset_);
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> &dst = size > kReadChunk ? au_ : chunk_;
    dst.resize(size > kReadChunk ? size : kReadChunk);
    std::size_t got = 0;
    if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) == 0) got = std::fread(dst.data(), 1, dst.size(), file_);
    read_us_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    dst.resize(got);
    if (&dst == &chunk_) chunk_offset_ = offset;
    return got >= size ? dst.data() : nullptr;
}

bool MovieTrack::decode_next() {
    if (next_ >= aus_.size()) return false;
    const auto [offset, size] = aus_[next_++];
    const std::uint8_t *au = read_unit(offset, size);
    if (au == nullptr) return false;
    ++units_;
    if (!decoder_->decode(au, size, rgba_.data(), width_, height_)) return false;
    ++pictures_;
    return true;
}

const std::uint32_t *MovieTrack::picture(std::uint32_t n) {
    if (decoder_ == nullptr) return nullptr;
    if (n >= aus_.size()) n = static_cast<std::uint32_t>(aus_.size() - 1u); // the game ran past the converted stream: hold its last picture
    if (static_cast<std::int64_t>(n) == shown_) return rgba_.data();
    if (static_cast<std::int64_t>(n) < shown_) return rgba_.data(); // [INFERRED] pictures only move forward; keep the last one
    // One access unit is one picture (Baseline, no B-frames): decode up to n.
    while (static_cast<std::int64_t>(pictures_) <= static_cast<std::int64_t>(n))
        if (!decode_next()) return shown_ >= 0 ? rgba_.data() : nullptr;
    shown_ = n;
    return rgba_.data();
}

std::shared_ptr<MovieTrack> Movies::bind(psprecomp::GuestMemory &m, std::uint32_t handle, std::uint32_t header, const std::string &dir,
                                         std::string *name) {
    // [INFERRED] one movie at a time: a new stream ends the previous ones (and frees their decoder,
    // the 3DS hardware decoder being a single session).
    playing_.clear();
    forget_picture_writes();
    if (entries_.empty()) return nullptr;
    for (const auto &e : entries_) {
        if (e.prefix > kPsmfPrefixMax) continue;
        const std::uint8_t *p = m.raw_pointer(header, e.prefix);
        if (p == nullptr || fnv1a64(p, e.prefix) != e.hash) continue;
        auto track = MovieTrack::open(dir, e.video, e.frames, e.audio);
        if (track == nullptr) { ++stats_.decode_failures; return nullptr; }
        ++stats_.matched;
        decoder_status_ = track->decoder() != nullptr ? track->decoder()->status() : "no decoder";
        if (name != nullptr) *name = e.video;
        playing_[handle] = Playing{track};
        return track;
    }
    ++stats_.unmatched;
    return nullptr;
}

void Movies::picture_decoded(std::uint32_t handle) {
    if (auto it = playing_.find(handle); it != playing_.end()) ++it->second.pictures;
}

const std::uint32_t *Movies::current_picture(std::uint32_t handle, std::uint32_t &width, std::uint32_t &height) {
    auto it = playing_.find(handle);
    if (it == playing_.end() || it->second.pictures == 0u) return nullptr;
    const auto start = std::chrono::steady_clock::now();
    const std::uint64_t units = it->second.track->units_decoded(), read_us = it->second.track->read_us();
    const auto *p = it->second.track->picture(it->second.pictures - 1u);
    stats_.units += it->second.track->units_decoded() - units;
    stats_.read_us += it->second.track->read_us() - read_us;
    stats_.decode_us += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    if (const auto *d = it->second.track->decoder()) decoder_status_ = d->status();
    if (p == nullptr) { ++stats_.decode_failures; return nullptr; }
    ++stats_.pictures;
    width = it->second.track->width();
    height = it->second.track->height();
    return p;
}

bool Movies::next_audio(std::uint32_t handle, std::int16_t *out) {
    auto it = playing_.find(handle);
    if (it == playing_.end() || it->second.track->audio() == nullptr) return false;
    it->second.track->audio()->read_stereo(static_cast<std::int64_t>(it->second.audio_units++) * 2048, 2048, out);
    ++stats_.audio_units;
    return true;
}

Movies &fork_movies() noexcept { return g_movies; }

void note_picture_write(std::uint32_t address, std::uint32_t stride, std::uint32_t rows) {
    address = psprecomp::GuestMemory::canonical(address);
    const std::lock_guard<std::mutex> lock(g_writes_lock);
    PictureWrite *slot = nullptr;
    for (auto &w : g_writes)
        if (w.generation != 0u && w.address == address) { slot = &w; break; }
    if (slot == nullptr) // a free slot, else the least recently written
        for (auto &w : g_writes)
            if (slot == nullptr || w.generation < slot->generation) slot = &w;
    *slot = {address, stride, ++g_last_generation, rows};
}

bool picture_write_generation(std::uint32_t address, std::uint32_t stride, std::uint64_t &generation, std::uint32_t *rows) {
    address = psprecomp::GuestMemory::canonical(address);
    const std::lock_guard<std::mutex> lock(g_writes_lock);
    for (const auto &w : g_writes)
        if (w.generation != 0u && w.address == address && w.stride == stride) {
            generation = w.generation;
            if (rows != nullptr) *rows = w.rows;
            return true;
        }
    return false;
}

void forget_picture_writes() {
    const std::lock_guard<std::mutex> lock(g_writes_lock);
    g_writes = {};
}

} // namespace p3p3ds::hle::media
