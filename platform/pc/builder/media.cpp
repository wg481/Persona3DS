// See media.hpp. Each function follows the function of the same name in tools/convert_media.py.
#include "media.hpp"
#include "psp_eboot.hpp" // sha256_hex

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace p3p3ds::builder::media {
namespace {

constexpr const char *kIndexHeader =
    "# Persona3DS converted media (tools/convert_media.py); format: core/include/p3p3ds/hle/fork_media.hpp\n"
    "# atrac <prefix bytes> <FNV-1a 64 of the .at3 prefix> <sample offset> <file>\n"
    "# movie <prefix bytes> <FNV-1a 64 of the PSMF header> <video .h264> <frames .idx> <audio .wav or ->\n";
constexpr std::size_t kMaxPrefix = 0x10000; // [UNVERIFIED] the game hands sceAtrac at least this much of a track
constexpr std::size_t kPsmfPrefix = 0x800;  // the PSMF header the game reads

std::uint32_t le16(const std::vector<std::uint8_t> &d, std::size_t at) { return static_cast<std::uint32_t>(d[at] | (d[at + 1] << 8)); }
std::uint32_t le32(const std::vector<std::uint8_t> &d, std::size_t at) {
    return static_cast<std::uint32_t>(d[at]) | (static_cast<std::uint32_t>(d[at + 1]) << 8) | (static_cast<std::uint32_t>(d[at + 2]) << 16) |
           (static_cast<std::uint32_t>(d[at + 3]) << 24);
}
std::uint32_t be16(const std::vector<std::uint8_t> &d, std::size_t at) { return static_cast<std::uint32_t>((d[at] << 8) | d[at + 1]); }
std::uint32_t be32(const std::vector<std::uint8_t> &d, std::size_t at) {
    return (static_cast<std::uint32_t>(d[at]) << 24) | (static_cast<std::uint32_t>(d[at + 1]) << 16) | (static_cast<std::uint32_t>(d[at + 2]) << 8) | d[at + 3];
}
void put32(std::vector<std::uint8_t> &d, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) d[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}
void append32(std::vector<std::uint8_t> &d, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::vector<std::uint8_t> read_file(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + p.string());
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), {});
}
void write_file(const fs::path &p, const std::vector<std::uint8_t> &d) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(d.data()), static_cast<std::streamsize>(d.size()));
    if (!out) throw std::runtime_error("cannot write " + p.string());
}
std::vector<std::uint8_t> read_source(const vfs::Source &src, std::size_t limit = SIZE_MAX) {
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(src.size(), limit));
    std::vector<std::uint8_t> d(n);
    std::size_t done = 0;
    while (done < n) {
        const std::size_t got = src.read(done, d.data() + done, n - done);
        if (got == 0) break;
        done += got;
    }
    d.resize(done);
    return d;
}
std::string hex16(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}
std::string lower(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string suffix_lower(const std::string &rel) {
    const auto slash = rel.rfind('/');
    const auto dot = rel.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return lower(rel.substr(dot));
}
// rel without its suffix, '/' replaced by '_' (convert_media.py: rel.replace("/", "_").rsplit(".", 1)[0]).
std::string flat_base(const std::string &rel) {
    std::string s = rel;
    std::replace(s.begin(), s.end(), '/', '_');
    const auto dot = s.rfind('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
}

// Every file under root, as '/'-separated paths relative to root, in path order (component by component).
void walk(const vfs::FileSystem &fsys, const std::string &dir, const std::string &rel, std::vector<std::string> &out) {
    const auto entries = fsys.list(dir);
    if (!entries) return;
    for (const auto &e : *entries) {
        const std::string child_rel = rel.empty() ? e.name : rel + "/" + e.name;
        const std::string child = dir.empty() ? e.name : dir + "/" + e.name;
        if (e.directory) walk(fsys, child, child_rel, out);
        else out.push_back(child_rel);
    }
}
bool path_less(const std::string &a, const std::string &b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const auto ea = a.find('/', i), eb = b.find('/', j);
        const std::string pa = a.substr(i, ea == std::string::npos ? std::string::npos : ea - i);
        const std::string pb = b.substr(j, eb == std::string::npos ? std::string::npos : eb - j);
        if (pa != pb) return pa < pb;
        if (ea == std::string::npos || eb == std::string::npos) return ea == std::string::npos && eb != std::string::npos;
        i = ea + 1;
        j = eb + 1;
    }
    return a.size() < b.size();
}

struct Layout { std::int64_t block_samples{}; std::optional<std::size_t> fact_at; };
Layout ima_layout(const std::vector<std::uint8_t> &data) {
    std::size_t pos = 12;
    std::optional<std::size_t> fact_at;
    std::uint32_t align = 0, channels = 0;
    while (pos + 8 <= data.size()) {
        const std::string ck(reinterpret_cast<const char *>(&data[pos]), 4);
        const std::uint32_t size = le32(data, pos + 4);
        if (ck == "fmt ") {
            channels = le16(data, pos + 10);
            align = le16(data, pos + 20);
        } else if (ck == "fact") {
            fact_at = pos + 8;
        } else if (ck == "data") {
            if (!channels || align <= 4 * channels) throw std::runtime_error("not an IMA-ADPCM WAV");
            const std::int64_t per_block = 1 + static_cast<std::int64_t>(align - 4 * channels) * 2 / channels;
            return {static_cast<std::int64_t>(size / align) * per_block, fact_at};
        }
        pos += 8 + size + (size & 1u);
    }
    throw std::runtime_error("no data chunk");
}

void write_frame_index(const fs::path &path, std::uint32_t width, std::uint32_t height,
                       const std::vector<std::pair<std::uint32_t, std::uint32_t>> &aus) {
    std::vector<std::uint8_t> out = {'P', '3', 'M', 'V'};
    append32(out, 1);
    append32(out, width);
    append32(out, height);
    append32(out, static_cast<std::uint32_t>(aus.size()));
    for (const auto &[offset, size] : aus) { append32(out, offset); append32(out, size); }
    write_file(path, out);
}

struct Runner {
    const Tools &t;
    unsigned serial{};
    // true when the tool exited with 0; the tool's output stays in the returned log path.
    bool run(const std::vector<fs::path> &argv, fs::path *log_out = nullptr) {
        const fs::path log = t.temp / ("tool" + std::to_string(serial++ % 4) + ".log");
        const int code = t.run(argv, log);
        if (log_out) *log_out = log;
        return code == 0;
    }
    bool ffmpeg(std::vector<fs::path> args) {
        std::vector<fs::path> argv = {t.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y"};
        argv.insert(argv.end(), args.begin(), args.end());
        return run(argv);
    }
};

std::string tool_error(const fs::path &log) {
    std::ifstream in(log);
    std::string line, last;
    while (std::getline(in, line))
        if (!line.empty()) last = line;
    return last.empty() ? "the tool failed" : last;
}

} // namespace

std::uint64_t fnv1a64(const std::uint8_t *data, std::size_t size) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
    return h;
}

At3Info parse_at3(const std::vector<std::uint8_t> &data) {
    if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error("not a RIFF WAVE file");
    std::size_t pos = 12;
    std::uint32_t block_align = 0, frame = 0;
    while (pos + 8 <= data.size()) {
        const std::string ck(reinterpret_cast<const char *>(&data[pos]), 4);
        const std::uint32_t size = le32(data, pos + 4);
        if (ck == "fmt ") {
            if (pos + 22 > data.size()) throw std::runtime_error("truncated fmt chunk");
            const std::uint32_t fmt_code = le16(data, pos + 8);
            block_align = le16(data, pos + 20);
            frame = fmt_code == 0x270 ? 1024 : fmt_code == 0xFFFE ? 2048 : 0;
            if (frame == 0) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "not ATRAC3/ATRAC3plus (format 0x%X)", fmt_code);
                throw std::runtime_error(buf);
            }
        } else if (ck == "data") {
            if (block_align == 0) throw std::runtime_error("data chunk before fmt");
            return At3Info{static_cast<std::uint32_t>(pos + 8), size, block_align, frame};
        }
        pos += 8 + size + (size & 1u);
    }
    throw std::runtime_error("no data chunk");
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> h264_access_units(const std::vector<std::uint8_t> &data) {
    std::vector<std::size_t> starts;
    auto find = [&](std::size_t from) -> std::size_t {
        for (std::size_t i = from; i + 3 <= data.size(); ++i)
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return i;
        return std::string::npos;
    };
    std::size_t i = find(0);
    while (i != std::string::npos && i + 3 < data.size()) {
        if ((data[i + 3] & 0x1F) == 9) starts.push_back(i > 0 && data[i - 1] == 0 ? i - 1 : i);
        i = find(i + 3);
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    for (std::size_t k = 0; k < starts.size(); ++k) {
        const std::size_t end = k + 1 < starts.size() ? starts[k + 1] : data.size();
        out.emplace_back(static_cast<std::uint32_t>(starts[k]), static_cast<std::uint32_t>(end - starts[k]));
    }
    return out;
}

std::vector<std::uint8_t> psmf_audio_oma(const std::vector<std::uint8_t> &pmf, std::uint8_t channel) {
    if (pmf.size() < 16 || std::memcmp(pmf.data(), "PSMF", 4) != 0) return {};
    std::vector<std::uint8_t> es;
    std::size_t p = be32(pmf, 8);
    while (p + 2048 <= pmf.size()) {
        const std::size_t pk = p;
        std::size_t q = 0;
        p += 2048;
        while (q + 6 <= 2048 && pmf[pk + q] == 0 && pmf[pk + q + 1] == 0 && pmf[pk + q + 2] == 1) {
            const std::uint8_t sid = pmf[pk + q + 3];
            if (sid == 0xBA) {
                if (q + 14 > 2048) break;
                q += 14 + (pmf[pk + q + 13] & 7u);
                continue;
            }
            if (sid == 0xB9) break;
            const std::size_t length = be16(pmf, pk + q + 4);
            const std::size_t body = q + 6, next = q + 6 + length;
            if (sid == 0xBD && length >= 3 && body + 3 <= 2048) {
                const std::size_t payload = body + 3 + pmf[pk + body + 2];
                if (payload + 4 <= next && payload < 2048 && pmf[pk + payload] == channel)
                    for (std::size_t b = payload + 4; b < std::min<std::size_t>(next, 2048); ++b) es.push_back(pmf[pk + b]);
            }
            q = next;
        }
    }
    std::vector<std::pair<std::size_t, std::size_t>> frames;
    std::size_t i = 0;
    while (i + 8 <= es.size() && es[i] == 0x0F && es[i + 1] == 0xD0) {
        const std::size_t size = (static_cast<std::size_t>((es[i + 2] & 3u) << 8) | es[i + 3]) * 8 + 8;
        if (i + 8 + size > es.size()) break;
        frames.emplace_back(i + 8, size);
        i += 8 + size;
    }
    if (frames.empty()) return {};
    std::vector<std::uint8_t> out(96, 0);
    out[0] = 'E'; out[1] = 'A'; out[2] = '3';
    out[5] = 96;
    out[6] = 0xFF; out[7] = 0xFF;
    out[32] = 1; // not encrypted, ATRAC3plus
    out[34] = es[2];
    out[35] = es[3];
    for (const auto &[at, size] : frames) out.insert(out.end(), es.begin() + static_cast<std::ptrdiff_t>(at), es.begin() + static_cast<std::ptrdiff_t>(at + size));
    return out;
}

std::pair<std::int64_t, std::int64_t> finish_wav(const fs::path &path, std::int64_t raw_samples) {
    auto data = read_file(path);
    const Layout layout = ima_layout(data);
    if (raw_samples < 0) raw_samples = layout.block_samples;
    const std::int64_t deficit = raw_samples - layout.block_samples;
    const std::int64_t offset = deficit > 0 ? -deficit : 0;
    const std::int64_t samples = std::min(raw_samples + offset, layout.block_samples);
    if (layout.fact_at) {
        put32(data, *layout.fact_at, static_cast<std::uint32_t>(samples));
        write_file(path, data);
    }
    return {offset, samples};
}

Result convert(const vfs::FileSystem &input, const std::string &root, const fs::path &output, const Tools &tools) {
    std::vector<std::string> files;
    walk(input, root, "", files);
    std::sort(files.begin(), files.end(), path_less);
    std::vector<std::string> movies, music;
    for (const auto &f : files) {
        const auto ext = suffix_lower(f);
        if (ext == ".pmf") movies.push_back(f);
        else if (ext == ".at3") music.push_back(f);
    }
    auto source_path = [&](const std::string &rel) { return root.empty() ? rel : root + "/" + rel; };
    auto log = [&](const std::string &s) { if (tools.log) tools.log(s); };
    fs::create_directories(output);
    fs::create_directories(tools.temp);
    Runner runner{tools};
    Result result;
    const unsigned total = static_cast<unsigned>(movies.size() + music.size());
    unsigned done = 0;

    // ---- Movies (convert_movies) --------------------------------------------------------------
    std::map<std::string, std::vector<std::uint8_t>> heads;
    for (const auto &rel : movies) {
        const auto src = input.open(source_path(rel));
        heads[rel] = src ? read_source(*src, kPsmfPrefix) : std::vector<std::uint8_t>{};
    }
    std::string movie_lines;
    for (const auto &rel : movies) {
        if (tools.progress) tools.progress(done, total, rel);
        ++done;
        const auto &head = heads[rel];
        if (head.size() < 4 || std::memcmp(head.data(), "PSMF", 4) != 0) {
            log("  skip " + rel + ": not a PSMF movie");
            continue;
        }
        for (const auto &other : movies)
            if (other != rel && heads[other] == head) {
                log("  warning " + rel + ": its " + std::to_string(head.size()) + "-byte header equals another movie's; the first one listed wins");
                break;
            }
        const std::string base = flat_base(rel), video = base + ".h264", frames = base + ".idx", audio = base + ".wav";
        std::vector<std::uint8_t> pmf;
        const fs::path temp_in = tools.temp / "movie.pmf";
        auto load = [&]() -> const std::vector<std::uint8_t> & {
            if (pmf.empty()) {
                const auto src = input.open(source_path(rel));
                if (!src) throw std::runtime_error("cannot open " + rel);
                pmf = read_source(*src);
                write_file(temp_in, pmf);
            }
            return pmf;
        };
        try {
            if (!fs::exists(output / video) || !fs::exists(output / frames)) {
                load();
                if (!runner.ffmpeg({"-i", temp_in, "-an", "-c:v", "libx264", "-profile:v", "baseline", "-level", "3.0", "-bf", "0",
                                    "-pix_fmt", "yuv420p", "-preset", "medium", "-crf", "21", "-x264-params",
                                    "aud=1:slices=1:sliced-threads=0:ref=1", "-fps_mode", "passthrough", "-f", "h264", output / video}))
                    throw std::runtime_error("ffmpeg could not convert the video");
                fs::path probe_log;
                if (!runner.run({tools.ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "csv=p=0",
                                 output / video},
                                &probe_log))
                    throw std::runtime_error("ffprobe failed: " + tool_error(probe_log));
                unsigned width = 0, height = 0;
                {
                    std::ifstream in(probe_log);
                    std::string line;
                    while (std::getline(in, line) && std::sscanf(line.c_str(), "%u,%u", &width, &height) != 2) {}
                }
                if (!width || !height) throw std::runtime_error("ffprobe gave no picture size");
                const auto aus = h264_access_units(read_file(output / video));
                if (aus.empty()) throw std::runtime_error("no access unit delimiters in the converted stream");
                write_frame_index(output / frames, width, height, aus);
                ++result.converted;
            }
            if (!fs::exists(output / audio)) {
                try {
                    const auto oma = psmf_audio_oma(load());
                    if (oma.empty()) throw std::runtime_error("no ATRAC3plus stream");
                    const fs::path tmp = fs::path(output / audio).replace_extension(".oma");
                    write_file(tmp, oma);
                    const bool ok = runner.ffmpeg({"-f", "oma", "-i", tmp, "-c:a", "adpcm_ima_wav", output / audio});
                    std::error_code ec;
                    fs::remove(tmp, ec);
                    if (!ok) throw std::runtime_error("ffmpeg could not convert the audio");
                    finish_wav(output / audio, -1);
                } catch (const std::exception &e) {
                    log("  " + rel + ": no audio (" + e.what() + ")");
                    std::error_code ec;
                    fs::remove(output / audio, ec);
                }
            }
        } catch (const std::exception &e) {
            log("  skip " + rel + ": conversion failed (" + e.what() + ")");
            std::error_code ec;
            fs::remove(output / video, ec);
            fs::remove(output / frames, ec);
            fs::remove(temp_in, ec);
            continue;
        }
        std::error_code ec;
        fs::remove(temp_in, ec);
        const auto index = read_file(output / frames);
        const std::uint32_t count = index.size() >= 20 ? le32(index, 16) : 0u;
        const std::string audio_field = fs::exists(output / audio) ? audio : "-";
        movie_lines += "movie " + std::to_string(head.size()) + " " + hex16(fnv1a64(head.data(), head.size())) + " " + video + " " + frames + " " +
                       audio_field + "  # " + rel + "\n";
        log("  " + rel + " -> " + video + "  " + std::to_string(count) + " pictures, audio " + audio_field);
        ++result.movies;
    }

    // ---- Music (convert) ----------------------------------------------------------------------
    struct Track { std::string rel; std::vector<std::uint8_t> data; At3Info info; std::size_t prefix{}; std::string output; };
    std::vector<Track> tracks;
    for (const auto &rel : music) {
        const auto src = input.open(source_path(rel));
        if (!src) continue;
        Track t{rel, read_source(*src), {}, 0, {}};
        try {
            t.info = parse_at3(t.data);
        } catch (const std::exception &e) {
            log("  skip " + rel + ": " + e.what());
            continue;
        }
        tracks.push_back(std::move(t));
    }
    // choose_prefixes: smallest prefix (header + whole ATRAC frames, doubling) that tells each file apart.
    for (auto &t : tracks) {
        std::size_t n = std::min<std::size_t>(t.info.data_off + t.info.block_align, t.data.size());
        // Python compares o[:n] == t[:n]: a shorter file only clashes when it equals t's first n bytes.
        auto clash_py = [&](std::size_t len) {
            for (const auto &o : tracks) {
                if (o.data == t.data) continue;
                const std::size_t on = std::min(len, o.data.size()), tn = std::min(len, t.data.size());
                if (on == tn && std::equal(o.data.begin(), o.data.begin() + static_cast<std::ptrdiff_t>(on), t.data.begin())) return true;
            }
            return false;
        };
        while (clash_py(n) && n < t.data.size()) n = std::min(n * 2, t.data.size());
        t.prefix = n;
    }
    std::string lines = kIndexHeader;
    std::map<std::string, std::string> converted_by_digest;
    for (auto &t : tracks) {
        if (tools.progress) tools.progress(done, total, t.rel);
        ++done;
        const std::string digest = sha256_hex(t.data);
        if (const auto it = converted_by_digest.find(digest); it != converted_by_digest.end()) {
            t.output = it->second;
        } else {
            t.output = flat_base(t.rel) + ".wav";
            const fs::path dst = output / t.output;
            if (!fs::exists(dst)) {
                const fs::path temp_in = tools.temp / "track.at3";
                write_file(temp_in, t.data);
                const bool ok = runner.ffmpeg({"-i", temp_in, "-c:a", "adpcm_ima_wav", dst});
                std::error_code ec;
                fs::remove(temp_in, ec);
                if (!ok) {
                    log("  skip " + t.rel + ": conversion failed");
                    fs::remove(dst, ec);
                    continue;
                }
                ++result.converted;
            }
            converted_by_digest[digest] = t.output;
        }
        const auto [offset, samples] = finish_wav(output / t.output, static_cast<std::int64_t>(t.info.raw_samples()));
        if (t.prefix > kMaxPrefix)
            log("  warning " + t.rel + ": needs a " + std::to_string(t.prefix) + "-byte prefix to be told apart; the game may hand over less");
        lines += "atrac " + std::to_string(t.prefix) + " " + hex16(fnv1a64(t.data.data(), t.prefix)) + " " + std::to_string(offset) + " " + t.output + "  # " + t.rel + "\n";
        log("  " + t.rel + " -> " + t.output + "  prefix " + std::to_string(t.prefix) + "  samples " + std::to_string(samples) + " (raw " +
            std::to_string(t.info.raw_samples()) + ", offset " + std::to_string(offset) + ")");
        ++result.tracks;
    }
    lines += movie_lines;
    {
        std::ofstream out(output / "index.txt", std::ios::binary | std::ios::trunc);
        out << lines;
        if (!out) throw std::runtime_error("cannot write " + (output / "index.txt").string());
    }
    if (tools.progress) tools.progress(total, total, "");
    return result;
}

} // namespace p3p3ds::builder::media
