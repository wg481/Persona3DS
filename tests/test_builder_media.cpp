// P3P3DS: Builder media conversion (platform/pc/builder/media.cpp), the C++ port of tools/convert_media.py.
// Known answers for the helpers; with ffmpeg and ffprobe on PATH and pspautotests fetched
// (tools/fetch_references.py), an end-to-end conversion of one .at3 and one .pmf, and a second run that
// converts nothing because the files exist.
//   test_builder_media                 unit checks (+ end-to-end when possible)
//   test_builder_media <in> <out>      convert folder <in> into <out> (parity runs against the Python tool)
//   test_builder_media --oma <pmf> <out>  write the movie's ATRAC3plus stream as OMA (parity runs)
#include "media.hpp"
#include "p3p3ds/vfs.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace p3p3ds::builder;

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

std::string quote(const fs::path &p) { return "\"" + p.string() + "\""; }

media::Tools system_tools(const fs::path &temp) {
    media::Tools t;
    t.ffmpeg = "ffmpeg";
    t.ffprobe = "ffprobe";
    t.temp = temp;
    t.run = [](const std::vector<fs::path> &argv, const fs::path &log) {
        std::string cmd;
        for (const auto &a : argv) cmd += (cmd.empty() ? "" : " ") + quote(a);
        cmd += " > " + quote(log) + " 2>&1";
        return std::system(cmd.c_str());
    };
    t.log = [](const std::string &s) { std::printf("%s\n", s.c_str()); };
    return t;
}

bool have_tool(const char *name) {
#ifdef _WIN32
    const std::string cmd = std::string(name) + " -version > NUL 2>&1";
#else
    const std::string cmd = std::string(name) + " -version > /dev/null 2>&1";
#endif
    return std::system(cmd.c_str()) == 0;
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 4 && std::string(argv[1]) == "--oma") { // parity runs: the OMA the movie audio step extracts
        std::ifstream in(argv[2], std::ios::binary);
        const std::vector<std::uint8_t> pmf((std::istreambuf_iterator<char>(in)), {});
        const auto oma = media::psmf_audio_oma(pmf);
        std::ofstream(argv[3], std::ios::binary).write(reinterpret_cast<const char *>(oma.data()), static_cast<std::streamsize>(oma.size()));
        return 0;
    }
    if (argc == 3) {
        const auto t = system_tools(fs::path(argv[2]) / ".." / "media_tmp");
        p3p3ds::vfs::HostFileSystem input(argv[1]);
        const auto r = media::convert(input, "", argv[2], t);
        std::printf("tracks %u movies %u converted %u\n", r.tracks, r.movies, r.converted);
        return 0;
    }

    // FNV-1a 64 reference values (offset basis; "a").
    check(media::fnv1a64(nullptr, 0) == 0xCBF29CE484222325ull, "fnv1a64 empty");
    check(media::fnv1a64(reinterpret_cast<const std::uint8_t *>("a"), 1) == 0xAF63DC4C8601EC8Cull, "fnv1a64 a");

    // Access units start at access unit delimiters (NAL type 9), with the zero byte of a 4-byte start code.
    {
        const std::vector<std::uint8_t> s = {0, 0, 0, 1, 0x09, 0xF0, 0, 0, 1, 0x65, 0xAA, 0, 0, 0, 1, 0x09, 0xF0, 0, 0, 1, 0x41, 0xBB};
        const auto aus = media::h264_access_units(s);
        check(aus.size() == 2 && aus[0] == std::pair<std::uint32_t, std::uint32_t>{0, 11} && aus[1] == std::pair<std::uint32_t, std::uint32_t>{11, 11},
              "h264 access units");
    }

    // No PSMF: no audio.
    check(media::psmf_audio_oma({'X', 'Y', 'Z', 'W'}).empty(), "psmf_audio_oma rejects non-PSMF");

#ifdef P3P_SOURCE_DIR
    const fs::path refs = fs::path(P3P_SOURCE_DIR) / "references/pspautotests/tests";
    const fs::path at3 = refs / "audio/atrac/sample.at3", pmf = refs / "video/pmf/test.pmf";
    if (fs::exists(at3)) {
        std::ifstream in(at3, std::ios::binary);
        const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), {});
        const auto info = media::parse_at3(data);
        check(info.frame_samples == 2048 && info.block_align > 0, "parse_at3 sample.at3 is ATRAC3plus");
        bool threw = false;
        try { (void)media::parse_at3({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'A', 'V', 'I', ' '}); } catch (const std::exception &) { threw = true; }
        check(threw, "parse_at3 rejects a non-WAVE RIFF");
    }
    if (fs::exists(at3) && fs::exists(pmf) && have_tool("ffmpeg") && have_tool("ffprobe")) {
        const fs::path root = fs::temp_directory_path() / "p3p3ds_builder_media_test";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "in/SOUND");
        fs::create_directories(root / "in/MOVIE");
        fs::copy_file(at3, root / "in/SOUND/BGM.AT3");
        fs::copy_file(pmf, root / "in/MOVIE/OPEN.PMF");
        const auto t = system_tools(root / "tmp");
        p3p3ds::vfs::HostFileSystem input(root / "in");
        const auto first = media::convert(input, "", root / "out", t);
        check(first.tracks == 1 && first.movies == 1 && first.converted == 2, "end-to-end: one track and one movie converted");
        std::ifstream idx(root / "out/index.txt");
        std::stringstream text;
        text << idx.rdbuf();
        const std::string index = text.str();
        check(index.find("\natrac ") != std::string::npos && index.find("SOUND_BGM.wav  # SOUND/BGM.AT3") != std::string::npos, "index has the track");
        check(index.find("\nmovie 2048 ") != std::string::npos && index.find("MOVIE_OPEN.h264 MOVIE_OPEN.idx") != std::string::npos, "index has the movie");
        check(fs::file_size(root / "out/MOVIE_OPEN.idx") > 20, "frame index written");
        const auto second = media::convert(input, "", root / "out", t);
        check(second.converted == 0 && second.tracks == 1 && second.movies == 1, "second run converts nothing");
        fs::remove_all(root, ec);
    } else {
        std::printf("end-to-end media test skipped (needs ffmpeg, ffprobe and references/pspautotests)\n");
    }
#endif
    std::printf(failures ? "%d failure(s)\n" : "builder media: all checks passed\n", failures);
    return failures ? 1 : 0;
}
