// Converted media (core/src/hle/fork_media.cpp): IMA-ADPCM WAV reading, the
// media index, track lookup by prefix hash, and sceAtrac serving converted
// samples at its decode position. The ATRAC part uses pspautotests'
// sample.at3 and is skipped without the reference clone.
#include "p3p3ds/hle/fork_atrac.hpp"
#include "p3p3ds/hle/fork_media.hpp"
#include "p3p3ds/hle/fork_movie.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace media = p3p3ds::hle::media;

namespace {
int failures = 0;
const std::filesystem::path kTmp = std::filesystem::path(P3P_SOURCE_DIR) / ".tmp/test_fork_media";
const std::filesystem::path kAt3 = std::filesystem::path(P3P_SOURCE_DIR) / "references/pspautotests/tests/audio/atrac/sample.at3";

void check(bool ok, const std::string &what) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << what << "\n"; }
}

void put16(std::vector<std::uint8_t> &v, std::uint32_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }
void put32(std::vector<std::uint8_t> &v, std::uint32_t x) { put16(v, x & 0xFFFF); put16(v, x >> 16); }
void tag(std::vector<std::uint8_t> &v, const char *t) { v.insert(v.end(), t, t + 4); }

// IMA-ADPCM WAV with `blocks` blocks of 24 bytes (17 frames each).
// Block b: left predictor 1000*(b+1), right -1000*(b+1), step index 0, all
// nibbles 0, except stereo block 0 whose first left byte is 0x07.
std::vector<std::uint8_t> ima_wav(std::uint32_t channels, std::uint32_t blocks, std::uint32_t fact) {
    const std::uint32_t align = channels == 2 ? 24u : 12u;
    std::vector<std::uint8_t> data;
    for (std::uint32_t b = 0; b < blocks; ++b) {
        put16(data, static_cast<std::uint16_t>(static_cast<std::int16_t>(1000 * (b + 1)))); data.push_back(0); data.push_back(0);
        if (channels == 2) { put16(data, static_cast<std::uint16_t>(static_cast<std::int16_t>(-1000 * static_cast<int>(b + 1)))); data.push_back(0); data.push_back(0); }
        const std::size_t first = data.size();
        data.resize(data.size() + align - 4u * channels, 0);
        if (b == 0 && channels == 2) data[first] = 0x07;
    }
    std::vector<std::uint8_t> v;
    tag(v, "RIFF"); put32(v, 0); tag(v, "WAVE");
    tag(v, "fmt "); put32(v, 20); put16(v, 0x11); put16(v, channels); put32(v, 44100); put32(v, 44100 * align / 17);
    put16(v, align); put16(v, 4); put16(v, 2); put16(v, 17);
    tag(v, "fact"); put32(v, 4); put32(v, fact);
    tag(v, "data"); put32(v, static_cast<std::uint32_t>(data.size()));
    v.insert(v.end(), data.begin(), data.end());
    const auto riff = static_cast<std::uint32_t>(v.size() - 8);
    for (int k = 0; k < 4; ++k) v[4 + k] = static_cast<std::uint8_t>(riff >> (8 * k));
    return v;
}

void write(const std::filesystem::path &p, const std::vector<std::uint8_t> &bytes) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void test_fnv() {
    const std::uint8_t a = 'a';
    // FNV-1a 64-bit test vector (Fowler/Noll/Vo reference): "a" -> af63dc4c8601ec8c.
    check(media::fnv1a64(&a, 1) == 0xAF63DC4C8601EC8Cull, "fnv1a64(\"a\")");
    check(media::fnv1a64(nullptr, 0) == 0xCBF29CE484222325ull, "fnv1a64 of nothing is the offset basis");
}

void test_ima() {
    write(kTmp / "stereo.wav", ima_wav(2, 3, 46));
    write(kTmp / "mono.wav", ima_wav(1, 2, 34));
    auto t = media::ImaTrack::open((kTmp / "stereo.wav").string());
    check(t != nullptr, "open stereo IMA WAV");
    if (t == nullptr) return;
    check(t->channels() == 2 && t->samples() == 46, "stereo: 2 channels, 46 samples (fact)");
    std::int16_t s[2 * 4];
    t->read_stereo(0, 3, s);
    // Header sample, then nibble 7 at index 0 (step 7): diff = 0 (7>>3) + 1 (7>>2) + 3 (7>>1) + 7 = 11
    // -> 1011, index 0 + 8 = 8.
    // Nibble 0 at index 8 (step 16): diff = 2 -> 1013, index 7.
    check(s[0] == 1000 && s[1] == -1000, "block 0 frame 0 is the header predictor");
    check(s[2] == 1011 && s[4] == 1013, "block 0 left nibbles 7 then 0: 1011, 1013");
    check(s[3] == -1000 && s[5] == -1000, "block 0 right nibbles 0 at index 0: unchanged");
    t->read_stereo(17, 2, s);
    check(s[0] == 2000 && s[1] == -2000 && s[2] == 2000, "block 1 starts at frame 17");
    t->read_stereo(44, 4, s);
    check(s[0] == 3000 && s[2] == 3000 && s[4] == 0 && s[6] == 0, "frames 44, 45 valid; 46 and later silent");
    t->read_stereo(-2, 3, s);
    check(s[0] == 0 && s[2] == 0 && s[4] == 1000, "negative positions are silent");
    auto mono = media::ImaTrack::open((kTmp / "mono.wav").string());
    check(mono != nullptr && mono->channels() == 1, "open mono IMA WAV");
    if (mono != nullptr) {
        mono->read_stereo(17, 1, s);
        check(s[0] == 2000 && s[1] == 2000, "mono duplicated to both channels");
    }
    check(media::ImaTrack::open((kTmp / "missing.wav").string()) == nullptr, "missing file: null");
}

void test_index_and_atrac() {
    psprecomp::Runtime rt;
    auto &m = rt.memory();
    const std::vector<std::uint8_t> head = {'R', 'I', 'F', 'F', 1, 2, 3, 4, 'W', 'A', 'V', 'E'};
    {
        std::ofstream idx(kTmp / "index.txt");
        idx << "# test index\n";
        idx << "atrac 12 " << std::hex << media::fnv1a64(head.data(), head.size()) << std::dec << " -5 stereo.wav\n";
        idx << "bogus line\n";
        idx << "atrac 12 0 0 \n"; // no file: ignored
    }
    auto &lib = media::fork_media();
    lib.set_dir(kTmp.string());
    check(lib.stats().entries == 1, "index: one valid entry");
    const std::uint32_t at = 0x08A00000u;
    for (std::size_t k = 0; k < head.size(); ++k) m.store8(at + static_cast<std::uint32_t>(k), head[k]);
    std::int32_t offset = 0;
    std::string name;
    check(lib.find_atrac(m, at, 12, offset, &name) != nullptr && offset == -5 && name == "stereo.wav", "prefix hash match");
    check(lib.find_atrac(m, at, 11, offset) == nullptr, "fewer bytes than the prefix: no match");
    m.store8(at + 5, 9);
    check(lib.find_atrac(m, at, 12, offset) == nullptr, "different bytes: no match");

    if (!std::filesystem::exists(kAt3)) { std::cout << "ATRAC part skipped: pspautotests clone absent\n"; return; }
    std::ifstream in(kAt3, std::ios::binary);
    const std::vector<std::uint8_t> at3{std::istreambuf_iterator<char>(in), {}};
    write(kTmp / "long.wav", ima_wav(2, 1000, 17000));
    const std::uint32_t prefix = 1024;
    {
        std::ofstream idx(kTmp / "index.txt");
        idx << "atrac " << prefix << " " << std::hex << media::fnv1a64(at3.data(), prefix) << std::dec << " -2000 long.wav\n";
    }
    lib.set_dir(kTmp.string());
    const std::uint32_t buf = 0x08A00000u, out = 0x08C00000u;
    for (std::size_t k = 0; k < at3.size(); ++k) m.store8(buf + static_cast<std::uint32_t>(k), at3[k]);
    p3p3ds::hle::atrac::Library atrac;
    const auto size = static_cast<std::uint32_t>(at3.size());
    const int id = atrac.set_halfway_buffer_and_get_id(m, buf, size, size);
    check(id >= 0 && atrac.track_name(id) == "long.wav", "sample.at3 attaches the converted track");
    auto track = media::ImaTrack::open((kTmp / "long.wav").string());
    for (int call = 0; call < 3 && id >= 0; ++call) {
        const std::uint32_t pos = atrac.info(id)->decodePos;
        std::uint32_t samples = 0, finish = 0;
        std::int32_t remain = 0;
        m.zero(out, 0x4000);
        check(atrac.decode_data(m, id, out, &samples, &finish, &remain) == 0 && samples != 0, "decode_data succeeds");
        std::vector<std::int16_t> want(2u * samples);
        track->read_stereo(static_cast<std::int64_t>(pos) - 2000, samples, want.data());
        bool same = true;
        for (std::uint32_t k = 0; k < 2u * samples; ++k)
            same = same && static_cast<std::int16_t>(m.load16(out + 2u * k)) == want[k];
        check(same, "decoded samples come from the track at decodePos + offset (call " + std::to_string(call) + ")");
    }
    check(atrac.release_atrac_id(id) == 0 && atrac.track_name(id).empty(), "release forgets the track");
    // Without a match the output stays silence.
    lib.set_dir((kTmp / "none").string());
    const int id2 = atrac.set_halfway_buffer_and_get_id(m, buf, size, size);
    std::uint32_t samples = 0, finish = 0;
    std::int32_t remain = 0;
    m.store16(out, 0x1234);
    check(atrac.decode_data(m, id2, out, &samples, &finish, &remain) == 0 && m.load16(out) == 0, "no index: silence");
}

// Fake decoder: every access unit is a picture whose pixels all equal the unit's last byte;
// an access unit of one byte 0xEE produces no picture.
class FakeDecoder final : public media::VideoDecoder {
public:
    bool decode(const std::uint8_t *au, std::size_t size, std::uint32_t *rgba, std::uint32_t width, std::uint32_t height) override {
        if (size == 1 && au[0] == 0xEE) return false;
        for (std::uint32_t k = 0; k < width * height; ++k) rgba[k] = au[size - 1];
        return true;
    }
};

void test_movies() {
    // 5 access units of 3 bytes: "AU" + index.
    std::vector<std::uint8_t> video;
    std::vector<std::uint8_t> idx = {'P', '3', 'M', 'V'};
    for (std::uint32_t v : {1u, 16u, 8u, 5u}) put32(idx, v);
    for (std::uint32_t k = 0; k < 5; ++k) {
        put32(idx, static_cast<std::uint32_t>(video.size()));
        put32(idx, 3);
        video.insert(video.end(), {'A', 'U', static_cast<std::uint8_t>(10 + k)});
    }
    write(kTmp / "mv.h264", video);
    write(kTmp / "mv.idx", idx);
    write(kTmp / "mv.wav", ima_wav(2, 300, 300 * 17));
    std::vector<std::uint8_t> psmf(0x800, 0);
    psmf[0] = 'P'; psmf[1] = 'S'; psmf[2] = 'M'; psmf[3] = 'F'; psmf[100] = 7;
    {
        std::ofstream index(kTmp / "index.txt");
        index << "movie 2048 " << std::hex << media::fnv1a64(psmf.data(), psmf.size()) << std::dec << " mv.h264 mv.idx mv.wav\n";
        index << "movie 2048 0 missing.h264 missing.idx -\n";
    }
    media::set_video_decoder_factory([](std::uint32_t, std::uint32_t) { return std::make_unique<FakeDecoder>(); });
    auto &lib = media::fork_media();
    lib.set_dir(kTmp.string());
    auto &movies = media::fork_movies();
    check(movies.stats().entries == 2, "index: two movie entries");
    psprecomp::Runtime rt;
    auto &m = rt.memory();
    const std::uint32_t header = 0x08A00000u, handle = 0x09000000u;
    for (std::size_t k = 0; k < psmf.size(); ++k) m.store8(header + static_cast<std::uint32_t>(k), psmf[k]);
    std::string name;
    check(movies.bind(m, handle, header, lib.dir(), &name) != nullptr && name == "mv.h264", "PSMF header selects the movie");
    std::uint32_t w = 0, h = 0;
    check(movies.current_picture(handle, w, h) == nullptr, "no picture before the first decoded one");
    movies.picture_decoded(handle);
    const std::uint32_t *p = movies.current_picture(handle, w, h);
    check(p != nullptr && w == 16 && h == 8 && p[0] == 10u, "first picture is access unit 0");
    movies.picture_decoded(handle);
    movies.picture_decoded(handle);
    p = movies.current_picture(handle, w, h);
    check(p != nullptr && p[5] == 12u, "third picture is access unit 2 (unit 1 decoded on the way)");
    check(movies.current_picture(handle, w, h)[0] == 12u, "same picture again without decoding");
    for (int k = 0; k < 5; ++k) movies.picture_decoded(handle);
    p = movies.current_picture(handle, w, h);
    check(p != nullptr && p[0] == 14u, "past the last unit: the last picture stays");
    std::vector<std::int16_t> pcm(4096);
    check(movies.next_audio(handle, pcm.data()) && pcm[0] == 1000 && pcm[17 * 2] == 2000, "audio unit 0: samples 0..2047");
    check(movies.next_audio(handle, pcm.data()) && pcm[0] == static_cast<std::int16_t>(1000 * (2048 / 17 + 1)), "audio unit 1 starts at sample 2048");
    check(movies.stats().audio_units == 2, "audio units counted");
    movies.release(handle);
    check(!movies.playing() && movies.current_picture(handle, w, h) == nullptr, "release ends the movie");
    m.store8(header + 100, 8);
    check(movies.bind(m, handle, header, lib.dir()) == nullptr && movies.stats().unmatched == 1, "other header: unmatched");
    media::set_video_decoder_factory(nullptr);
    m.store8(header + 100, 7);
    check(movies.bind(m, handle, header, lib.dir()) != nullptr, "bind without a decoder");
    movies.picture_decoded(handle);
    check(movies.current_picture(handle, w, h) == nullptr, "no decoder: no picture (black)");
    check(movies.next_audio(handle, pcm.data()), "no decoder: the audio still plays");
}

// Read-ahead of the converted stream: 300 access units of 2,000-2,999 bytes (about 750 KB, several
// read chunks) plus one unit larger than a chunk; every picture must come from its own unit.
void test_movie_read_ahead() {
    std::vector<std::uint8_t> video;
    std::vector<std::uint8_t> idx = {'P', '3', 'M', 'V'};
    constexpr std::uint32_t kUnits = 301, kBig = 150;
    for (std::uint32_t v : {1u, 16u, 8u, kUnits}) put32(idx, v);
    for (std::uint32_t k = 0; k < kUnits; ++k) {
        const std::uint32_t size = k == kBig ? static_cast<std::uint32_t>(media::MovieTrack::kReadChunk) + 1000u : 2000u + (k * 37u) % 1000u;
        put32(idx, static_cast<std::uint32_t>(video.size()));
        put32(idx, size);
        for (std::uint32_t b = 0; b + 1 < size; ++b) video.push_back(static_cast<std::uint8_t>(b * 7u + k));
        video.push_back(static_cast<std::uint8_t>(k % 251u)); // the fake decoder's picture value
    }
    write(kTmp / "ra.h264", video);
    write(kTmp / "ra.idx", idx);
    media::set_video_decoder_factory([](std::uint32_t, std::uint32_t) { return std::make_unique<FakeDecoder>(); });
    auto track = media::MovieTrack::open(kTmp.string(), "ra.h264", "ra.idx", "-");
    check(track != nullptr && track->frames() == kUnits, "read-ahead movie opens with 301 units");
    bool all = track != nullptr;
    for (std::uint32_t k = 0; all && k < kUnits; ++k) {
        const std::uint32_t *p = track->picture(k);
        all = p != nullptr && p[0] == k % 251u && p[127] == k % 251u;
        if (!all) check(false, "picture " + std::to_string(k) + " comes from its own unit");
    }
    check(all, "all 301 pictures come from their own units (chunk boundaries and a unit larger than a chunk)");
    check(track != nullptr && track->units_decoded() == kUnits, "one unit decoded per picture when none is skipped");
    auto skip = media::MovieTrack::open(kTmp.string(), "ra.h264", "ra.idx", "-");
    const std::uint32_t *p = skip != nullptr ? skip->picture(9) : nullptr;
    check(p != nullptr && p[0] == 9u && skip->units_decoded() == 10u, "skipping to picture 9 decodes units 0..9");
    media::set_video_decoder_factory(nullptr);
}

// Write generations of movie pictures (sceMpegAvcCsc -> 3DS renderer).
void test_picture_writes() {
    media::forget_picture_writes();
    std::uint64_t g = 0, a1 = 0, a2 = 0, b1 = 0;
    check(!media::picture_write_generation(0x08F58010u, 512u, g), "no write yet: no generation");
    media::note_picture_write(0x08F58010u, 512u);
    check(media::picture_write_generation(0x08F58010u, 512u, a1) && a1 != 0u, "written address has a generation");
    check(media::picture_write_generation(0x48F58010u, 512u, g) && g == a1, "uncached mirror is the same address");
    check(!media::picture_write_generation(0x08F58010u, 480u, g), "other stride: no generation");
    check(!media::picture_write_generation(0x08F58020u, 512u, g), "other address: no generation");
    media::note_picture_write(0x08ED0010u, 512u); // Persona's second movie texture
    check(media::picture_write_generation(0x08ED0010u, 512u, b1) && b1 > a1, "second buffer: newer generation");
    check(media::picture_write_generation(0x08F58010u, 512u, g) && g == a1, "first buffer unchanged by the second");
    media::note_picture_write(0x08F58010u, 512u);
    check(media::picture_write_generation(0x08F58010u, 512u, a2) && a2 > b1, "rewrite: generation grows");
    for (std::uint32_t k = 0; k < 4; ++k) media::note_picture_write(0x09000000u + k * 0x100000u, 512u);
    check(!media::picture_write_generation(0x08ED0010u, 512u, g), "four newer addresses evict the oldest");
    media::forget_picture_writes();
    check(!media::picture_write_generation(0x09000000u, 512u, g), "forget drops every address");
    media::note_picture_write(0x08F58010u, 512u);
    check(media::picture_write_generation(0x08F58010u, 512u, g) && g > a2, "generations keep growing after forget");
    media::fork_movies().release(0x12345678u);
    check(!media::picture_write_generation(0x08F58010u, 512u, g), "a movie ending (sceMpegDelete) forgets the writes");
}

} // namespace

int main() {
    std::filesystem::create_directories(kTmp);
    test_fnv();
    test_ima();
    test_index_and_atrac();
    test_movies();
    test_movie_read_ahead();
    test_picture_writes();
    std::cout << "fork media failures=" << failures << "\n";
    return failures ? 1 : 0;
}
