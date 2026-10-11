// sceAtrac3plus model (core/src/hle/fork_atrac.cpp) replayed against the
// hardware transcripts in references/pspautotests/tests/audio/atrac: each
// test below performs the same calls as the pspautotests program of the same
// name, on the same .at3 files, and prints the same lines; they are compared
// with the program's .expected (the "[x]"/"[r]" markers and blank lines are
// ignored). Lines of the expected files that print the library's internal
// context (_sceAtracGetContextAddress) are not reproduced; those programs are
// not replayed. Skipped without the reference clone.
#include "p3p3ds/hle/fork_atrac.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using p3p3ds::hle::atrac::Library;

namespace {
int failures = 0;
const std::filesystem::path kDir = std::filesystem::path(P3P_SOURCE_DIR) / "references/pspautotests/tests/audio/atrac";

std::vector<std::string> g_out;
std::string g_raw; // schedf-style output, split into lines when compared (stream.cpp)
void raw(const char *format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    g_raw += text;
}
void line(const char *format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    std::string s(text);
    while (!s.empty() && (s.back() == '\n')) s.pop_back();
    g_out.push_back(s);
}

std::vector<std::string> expected_lines(const std::filesystem::path &file) {
    std::ifstream in(file);
    std::vector<std::string> lines;
    std::string s;
    while (std::getline(in, s)) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
        if (s.size() >= 4 && s[0] == '[' && s[2] == ']' && s[3] == ' ') s.erase(0, 4);
        if (!s.empty()) lines.push_back(s);
    }
    return lines;
}

void compare(const char *name) {
    const auto want = expected_lines(kDir / (std::string(name) + ".expected"));
    std::vector<std::string> got;
    for (auto &s : g_out) if (!s.empty()) got.push_back(s);
    const std::size_t n = std::max(want.size(), got.size());
    int bad = 0;
    for (std::size_t k = 0; k < n; ++k) {
        const std::string w = k < want.size() ? want[k] : "<missing>";
        const std::string g = k < got.size() ? got[k] : "<missing>";
        if (w != g && bad++ < 8) std::cerr << name << ".expected:" << k + 1 << "\n  want: " << w << "\n  got:  " << g << "\n";
    }
    if (bad) { ++failures; std::cerr << name << ": " << bad << " differing lines\n"; }
    else std::cout << name << ": " << got.size() << " lines match\n";
    g_out.clear();
}

// Guest memory with the test files.
struct Env {
    psprecomp::Runtime rt;
    psprecomp::GuestMemory &m = rt.memory();
    Library lib;
    std::uint32_t next = 0x08A00000u;
    std::uint32_t alloc(std::uint32_t size) { const auto at = next; next += (size + 0x3Fu) & ~0x3Fu; return at; }
    std::uint32_t load(const std::vector<std::uint8_t> &bytes) {
        const auto at = alloc(static_cast<std::uint32_t>(bytes.size()));
        for (std::size_t k = 0; k < bytes.size(); ++k) m.store8(at + static_cast<std::uint32_t>(k), bytes[k]);
        return at;
    }
};
std::vector<std::uint8_t> read_file(const char *name) {
    std::ifstream in(kDir / name, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// shared.cpp CreateLoopedAtracFrom: adds a smpl chunk with one loop.
std::vector<std::uint8_t> looped(const std::vector<std::uint8_t> &at3, std::uint32_t loopStart, std::uint32_t loopEnd, std::uint32_t numLoops = 1) {
    constexpr std::uint32_t kExtra = 44 + 24, kDataStart = 88;
    std::vector<std::uint8_t> out(at3.size() + kExtra);
    std::copy(at3.begin(), at3.begin() + kDataStart, out.begin());
    auto put = [&](std::size_t at, std::uint32_t v) { for (int b = 0; b < 4; ++b) out[at + b] = static_cast<std::uint8_t>(v >> (8 * b)); };
    std::uint32_t riff = 0;
    for (int b = 0; b < 4; ++b) riff |= static_cast<std::uint32_t>(out[4 + b]) << (8 * b);
    put(4, riff + kExtra);
    const std::uint32_t words[] = {0x6C706D73u, 36 + 24, 0, 0, 22676, 60, 0, 0, 0, numLoops, 0x18, 0, 0, loopStart, loopEnd, 0, 0};
    for (std::size_t k = 0; k < std::size(words); ++k) put(kDataStart + 4 * k, words[k]);
    std::copy(at3.begin() + kDataStart, at3.end(), out.begin() + kDataStart + kExtra);
    return out;
}

// ---- setdata.cpp -----------------------------------------------------------
void test_setdata() {
    Env e;
    auto at3 = read_file("sample.at3");
    const auto buf = e.load(at3);
    const auto size = static_cast<std::uint32_t>(at3.size());
    const int atracID = e.lib.get_atrac_id(0x1001), plusID = e.lib.get_atrac_id(0x1000);
    line("IDs:");
    line("  ATRAC3: %08x", e.lib.set_halfway_buffer(e.m, atracID, buf, size, size));
    line("  ATRAC3+: %08x", e.lib.set_halfway_buffer(e.m, plusID, buf, size, size));
    line("  Unallocated (1): %08x", e.lib.set_halfway_buffer(e.m, 1, buf, size, size));
    line("  Unallocated (4): %08x", e.lib.set_halfway_buffer(e.m, 4, buf, size, size));
    line("  -1: %08x", e.lib.set_halfway_buffer(e.m, -1, buf, size, size));
    line("Buffer:");
    line("  Zero length: %08x", e.lib.set_halfway_buffer(e.m, plusID, buf, 0, 0));
    e.m.zero(buf, size);
    line("  Zeroed data: %08x", e.lib.set_halfway_buffer(e.m, plusID, buf, size, size));
    line("sceAtracSetDataAndGetID:");
    int result = e.lib.set_halfway_buffer_and_get_id(e.m, buf, size, size);
    if (result >= 0) e.lib.release_atrac_id(result);
    line("  Zeroed data: %08x", result);
    for (std::size_t k = 0; k < at3.size(); ++k) e.m.store8(buf + static_cast<std::uint32_t>(k), at3[k]);
    line("  * Reloaded ATRAC3+ test file");
    result = e.lib.set_halfway_buffer_and_get_id(e.m, buf, size, size);
    if (result >= 0) e.lib.release_atrac_id(result);
    line("  Normal: %08x", result);
    compare("setdata");
}

// ---- decode.cpp ------------------------------------------------------------
void test_decode() {
    Env e;
    const auto at3 = read_file("sample.at3");
    const auto buf = e.load(at3);
    const int atracID = e.lib.set_halfway_buffer_and_get_id(e.m, buf, static_cast<std::uint32_t>(at3.size()), static_cast<std::uint32_t>(at3.size()));
    const auto samples = e.alloc(65536 * 2);
    auto decode = [&](const char *title, int id, std::uint32_t out) {
        std::uint32_t count = 0x1337, ended = 0x1337;
        std::int32_t remaining = 0x1337;
        const auto r = e.lib.decode_data(e.m, id, out, &count, &ended, &remaining);
        line("%s: %08x (count=%x, ended=%x, remaining=%x)", title, r, count, ended, static_cast<std::uint32_t>(remaining));
    };
    line("IDs:");
    decode("  Normal", atracID, samples);
    decode("  Unallocated (1)", 1, samples);
    decode("  Unallocated (4)", 4, samples);
    decode("  -1", -1, samples);
    line("Missing parameters:");
    decode("  NULL output", atracID, 0);
    e.lib.reset_play_position(e.m, atracID, 0, 0, 0);
    decode("  Normal", atracID, samples);
    line("Drain:");
    int i = 0;
    std::int32_t result = 0;
    for (; i < 1000; ++i) {
        std::uint32_t count, ended;
        std::int32_t remaining;
        result = e.lib.decode_data(e.m, atracID, samples, &count, &ended, &remaining);
        if (result != 0) break;
    }
    line("  Drained in %d calls: %08x", i, static_cast<std::uint32_t>(result));
    compare("decode");
}

// ---- getsoundsample.cpp ----------------------------------------------------
void test_getsoundsample() {
    Env e;
    const auto at3 = read_file("sample.at3");
    const auto buf = e.load(at3);
    const auto size = static_cast<std::uint32_t>(at3.size());
    auto test = [&](const char *title, int id) {
        std::int32_t end = -1337, ls = -1337, le = -1337;
        const auto r = e.lib.get_sound_sample(id, &end, &ls, &le);
        if (r >= 0) line("%s: OK (%08x), end=%08x, loopStart=%08x, loopEnd=%08x", title, r, end, ls, le);
        else if (id < 0) line("%s: Error (%08x, %08x), end=%08x, loopStart=%08x, loopEnd=%08x", title, id, r, end, ls, le);
        else line("%s: Failed (%08x), end=%08x, loopStart=%08x, loopEnd=%08x", title, r, end, ls, le);
    };
    int atracID = e.lib.set_halfway_buffer_and_get_id(e.m, buf, size, size);
    const int atracID2 = e.lib.get_atrac_id(0x1000);
    line("Atrac IDs:");
    test("  Normal", atracID);
    test("  Invalid ID", -1);
    test("  Unused ID", 1);
    line("Other states:");
    test("  No data", atracID2);
    e.lib.set_state_for_test(atracID, 8);
    test("  State 8", atracID);
    e.lib.set_state_for_test(atracID, 16);
    test("  State 16", atracID);
    e.lib.release_atrac_id(atracID);
    e.lib.release_atrac_id(atracID2);
    e.lib.set_state_for_test(atracID, -1); // release refuses state 16; the next ID must be free again
    line("Loop");
    struct { const char *title; std::uint32_t start, end; } loops[] = {{"  Loop full", 2048, 249548}, {"  Loop start", 2048, 8192}, {"  Loop zero", 0, 2048}};
    for (const auto &l : loops) {
        const auto file = looped(at3, l.start, l.end);
        const auto lbuf = e.load(file);
        atracID = e.lib.set_halfway_buffer_and_get_id(e.m, lbuf, static_cast<std::uint32_t>(file.size()), static_cast<std::uint32_t>(file.size()));
        test(l.title, atracID);
        e.lib.release_atrac_id(atracID);
    }
    compare("getsoundsample");
}

// ---- addstreamdata.cpp -----------------------------------------------------
void test_addstreamdata() {
    Env e;
    const auto at3 = read_file("sample.at3");
    const auto buf = e.load(at3);
    const auto size = static_cast<std::uint32_t>(at3.size());
    const int fullID = e.lib.set_halfway_buffer_and_get_id(e.m, buf, size, size);
    const int halfID = e.lib.set_halfway_buffer_and_get_id(e.m, buf, size / 2, size);
    const int emptyID = e.lib.get_atrac_id(0x1000);
    auto add = [&](const char *title, int id, std::uint32_t bytes) { line("%s: %08x", title, e.lib.add_stream_data(id, bytes)); };
    line("Atrac IDs:");
    add("  Invalid", -1, 0x100);
    add("  Empty", emptyID, 0x100);
    add("  Fully buffered", fullID, 0x100);
    add("  Half buffered", halfID, 0x100);
    line("Bytes:");
    add("  Zero", halfID, 0);
    add("  One", halfID, 1);
    add("  Size", halfID, size);
    add("  Remaining", halfID, size / 2 - 0x101);
    add("  Zero after remaining", halfID, 0);
    compare("addstreamdata");
}

// ---- getremainframe.cpp ----------------------------------------------------
void test_getremainframe() {
    Env e;
    const auto at3 = read_file("sample.at3");
    const auto size = static_cast<std::uint32_t>(at3.size());
    auto test = [&](const char *title, int id) {
        std::int32_t remain = -1337;
        const auto r = e.lib.get_remain_frame(id, &remain);
        if (r >= 0) line("%s: OK (%08x), remainFrame=%d", title, r, remain);
        else if (id < 0) line("%s: Invalid (%08x, %08x), remainFrame=%d", title, id, r, remain);
        else line("%s: Failed (%08x), remainFrame=%d", title, r, remain);
    };
    const auto input = e.alloc(size + 68);
    auto fill = [&](const std::vector<std::uint8_t> &src) { for (std::size_t k = 0; k < src.size(); ++k) e.m.store8(input + static_cast<std::uint32_t>(k), src[k]); };
    auto drain = [&](int id, const std::vector<std::uint8_t> &file) {
        std::uint32_t samples, end = 0;
        std::int32_t remain;
        do {
            const auto res = e.lib.decode_data(e.m, id, 0, &samples, &end, &remain);
            if (res != 0) { line("** ERROR: %08x", res); break; }
            if (remain == 0) {
                std::uint32_t ptr, avail, off;
                if (e.lib.get_stream_data_info(id, &ptr, &avail, &off) == 0) {
                    for (std::uint32_t k = 0; k < avail; ++k) e.m.store8(ptr + k, file[off + k]);
                    e.lib.add_stream_data(id, avail);
                }
            }
        } while (end == 0);
    };
    fill(at3);
    int atracID = e.lib.set_halfway_buffer_and_get_id(e.m, input, size, size);
    line("Atrac IDs:");
    test("  Normal", atracID);
    test("  Invalid ID", -1);
    test("  Unused ID", 2);
    e.lib.release_atrac_id(atracID);

    atracID = e.lib.set_halfway_buffer_and_get_id(e.m, input, size, size);
    const int atracID2 = e.lib.get_atrac_id(0x1000);
    line("Other states:");
    test("  No data", atracID2);
    e.lib.set_state_for_test(atracID, 8);
    test("  State 8", atracID);
    e.lib.set_state_for_test(atracID, 16);
    test("  State 16", atracID);
    e.lib.set_state_for_test(atracID, 2);
    e.lib.release_atrac_id(atracID);
    e.lib.release_atrac_id(atracID2);

    line("Buffers:");
    atracID = e.lib.set_halfway_buffer_and_get_id(e.m, input, size, size);
    test("  Full buffer", atracID);
    e.lib.release_atrac_id(atracID);
    for (std::uint32_t half : {472u, 473u, 847u, 848u, 1975u, 1976u}) {
        char title[128];
        std::snprintf(title, sizeof title, "  Half buffer size = %u", half);
        atracID = e.lib.set_halfway_buffer_and_get_id(e.m, input, half, size);
        test(title, atracID);
        e.lib.release_atrac_id(atracID);
    }
    atracID = e.lib.set_halfway_buffer_and_get_id(e.m, input, size / 2, size / 2);
    test("  Streaming buffer", atracID);
    e.lib.release_atrac_id(atracID);
    fill(at3);
    atracID = e.lib.set_halfway_buffer_and_get_id(e.m, input, size / 2, size / 2);
    p3p3ds::hle::atrac::BufferInfo info{};
    e.lib.get_buffer_info_for_resetting(e.m, atracID, 4096, &info);
    for (std::uint32_t k = 0; k < info.minWriteByteFirstBuf; ++k) e.m.store8(input + k, at3[info.readPositionFirstBuf + k]);
    e.lib.reset_play_position(e.m, atracID, 4096, info.minWriteByteFirstBuf, 0);
    test("  Streaming buffer after seek + min", atracID);
    e.lib.release_atrac_id(atracID);

    const auto loop = looped(at3, 2048, 249548);
    line("Drained remain values:");
    fill(at3);
    atracID = e.lib.set_halfway_buffer_and_get_id(e.m, input, size, size);
    drain(atracID, at3);
    test("  Full buffer", atracID);
    e.lib.release_atrac_id(atracID);
    fill(at3);
    atracID = e.lib.set_halfway_buffer_and_get_id(e.m, input, size / 2, size);
    drain(atracID, at3);
    test("  Half buffer", atracID);
    e.lib.release_atrac_id(atracID);
    fill(at3);
    atracID = e.lib.set_halfway_buffer_and_get_id(e.m, input, size / 2, size / 2);
    drain(atracID, at3);
    test("  Streamed without loop", atracID);
    e.lib.release_atrac_id(atracID);
    fill(loop);
    atracID = e.lib.set_halfway_buffer_and_get_id(e.m, input, size / 2, size / 2);
    e.lib.set_loop_num(atracID, 1);
    drain(atracID, loop);
    test("  Streamed with loop", atracID);
    e.lib.release_atrac_id(atracID);
    compare("getremainframe");
}

// ---- second/*.cpp: streaming with a loop and a second buffer ---------------
// SetDataAndGetID on the first half of a file (as Persona streams), after
// sceAtracReinit(0, 3).
struct HalfFile { std::vector<std::uint8_t> file; std::uint32_t buffer{}; int id{}; };
HalfFile set_half(Env &e, const std::vector<std::uint8_t> &file) {
    HalfFile h{file};
    const auto half = static_cast<std::uint32_t>(file.size() / 2);
    h.buffer = e.alloc(half);
    for (std::uint32_t k = 0; k < half; ++k) e.m.store8(h.buffer + k, file[k]);
    h.id = e.lib.set_halfway_buffer_and_get_id(e.m, h.buffer, half, half);
    return h;
}

void test_second_getinfo() {
    Env e;
    e.lib.reinit(0, 3);
    const auto at3 = read_file("sample.at3");
    const auto buf = e.load(at3);
    const auto size = static_cast<std::uint32_t>(at3.size());
    auto info = [&](const char *title, int id) {
        std::uint32_t pos = static_cast<std::uint32_t>(-1337), bytes = static_cast<std::uint32_t>(-1337);
        const auto r = e.lib.get_second_buffer_info(id, &pos, &bytes);
        if (id >= 0) line(r >= 0 ? "%s: OK (%08x) pos=%08x, size=%08x" : "%s: Failed (%08x) pos=%08x, size=%08x", title, r, pos, bytes);
        else line("%s: Invalid (%08x, %08x) pos=%08x, size=%08x", title, id, r, pos, bytes);
    };
    auto info_file = [&](const char *title, const std::vector<std::uint8_t> &file) {
        const auto h = set_half(e, file);
        info(title, h.id);
        e.lib.release_atrac_id(h.id);
    };
    int atracID = e.lib.set_halfway_buffer_and_get_id(e.m, buf, size, size);
    line("Atrac IDs:");
    info("  Normal", atracID);
    info("  Invalid ID", -1);
    info("  Unused ID", 1);
    line("Other states:");
    const int without = e.lib.get_atrac_id(0x1000);
    info("  No data", without);
    e.lib.release_atrac_id(without);
    e.lib.set_state_for_test(atracID, 8);
    info("  State 8", atracID);
    e.lib.set_state_for_test(atracID, 16);
    info("  State 16", atracID);
    e.lib.set_state_for_test(atracID, 1);
    e.lib.release_atrac_id(atracID);
    line("Stream types:");
    const int halfway = e.lib.set_halfway_buffer_and_get_id(e.m, buf, size / 2, size);
    info("  Halfway", atracID);
    e.lib.release_atrac_id(halfway);
    info_file("  No loop", at3);
    info_file("  End loop", looped(at3, 2048, 249548));
    info_file("  Middle loop", looped(at3, 2048, 200000));
    info_file("  Late loop", looped(at3, 200000, 249548));
    line("Loop end cutoff:");
    info_file("  200335", looped(at3, 2048, 200335));
    info_file("  200336", looped(at3, 2048, 200336));
    line("With second buffer:");
    const auto h = set_half(e, looped(at3, 2048, 200000));
    std::uint32_t off = 0, bytes = 0;
    e.lib.get_second_buffer_info(h.id, &off, &bytes);
    const auto second = e.alloc(bytes);
    line("  Set buffer: %08x", e.lib.set_second_buffer(h.id, second, bytes));
    info("  With second buffer", h.id);
    e.lib.release_atrac_id(h.id);
    compare("second/getinfo");
}

void test_second_needed() {
    Env e;
    e.lib.reinit(0, 3);
    const auto at3 = read_file("sample.at3");
    const auto buf = e.load(at3);
    const auto size = static_cast<std::uint32_t>(at3.size());
    auto needed = [&](const char *title, int id) {
        const auto r = e.lib.is_second_buffer_needed(id);
        if (id >= 0) { if (r >= 0) line("%s: %d", title, r); else line("%s: Failed (%08x)", title, r); }
        else line("%s: Invalid (%08x, %08x)", title, id, r);
    };
    auto needed_file = [&](const char *title, const std::vector<std::uint8_t> &file) {
        const auto h = set_half(e, file);
        needed(title, h.id);
        e.lib.release_atrac_id(h.id);
    };
    int atracID = e.lib.set_halfway_buffer_and_get_id(e.m, buf, size, size);
    line("Atrac IDs:");
    needed("  Normal", atracID);
    needed("  Invalid ID", -1);
    needed("  Unused ID", 1);
    line("Other states:");
    const int without = e.lib.get_atrac_id(0x1000);
    needed("  No data", without);
    e.lib.release_atrac_id(without);
    e.lib.set_state_for_test(atracID, 8);
    needed("  State 8", atracID);
    e.lib.set_state_for_test(atracID, 16);
    needed("  State 16", atracID);
    e.lib.set_state_for_test(atracID, 1);
    e.lib.release_atrac_id(atracID);
    line("Stream types:");
    const int halfway = e.lib.set_halfway_buffer_and_get_id(e.m, buf, size / 2, size);
    needed("  Halfway", atracID);
    e.lib.release_atrac_id(halfway);
    needed_file("  No loop", at3);
    needed_file("  End loop", looped(at3, 2048, 249548));
    needed_file("  Middle loop", looped(at3, 2048, 200000));
    needed_file("  Late loop", looped(at3, 200000, 249548));
    line("With second buffer:");
    const auto h = set_half(e, looped(at3, 2048, 200000));
    std::uint32_t off = 0, bytes = 0;
    e.lib.get_second_buffer_info(h.id, &off, &bytes);
    line("  Set buffer: %08x", e.lib.set_second_buffer(h.id, e.alloc(bytes), bytes));
    needed("  Is needed with buffer", h.id);
    e.lib.release_atrac_id(h.id);
    compare("second/needed");
}

void test_second_setbuffer() {
    Env e;
    e.lib.reinit(0, 3);
    const auto at3 = read_file("sample.at3");
    const auto buf = e.load(at3);
    const auto size = static_cast<std::uint32_t>(at3.size());
    const auto second = e.alloc(size + 0x1000);
    auto set = [&](const char *title, int id, std::uint32_t ptr, std::uint32_t bytes) {
        const auto r = e.lib.set_second_buffer(id, ptr, bytes);
        if (id >= 0) line(r >= 0 ? "%s: OK (%08x)" : "%s: Failed (%08x)", title, r);
        else line("%s: Invalid (%08x, %08x)", title, id, r);
    };
    auto set_file = [&](const char *title, const std::vector<std::uint8_t> &file, std::uint32_t ptr, std::uint32_t bytes) {
        const auto h = set_half(e, file);
        set(title, h.id, ptr, bytes);
        e.lib.release_atrac_id(h.id);
    };
    int atracID = e.lib.set_halfway_buffer_and_get_id(e.m, buf, size, size);
    line("Atrac IDs:");
    set("  Normal", atracID, second, 0);
    set("  Invalid ID", -1, second, 0);
    set("  Unused ID", 1, second, 0);
    line("Other states:");
    const int without = e.lib.get_atrac_id(0x1000);
    set("  No data", without, second, 0);
    e.lib.release_atrac_id(without);
    e.lib.set_state_for_test(atracID, 8);
    set("  State 8", atracID, second, 0);
    e.lib.set_state_for_test(atracID, 16);
    set("  State 16", atracID, second, 0);
    e.lib.set_state_for_test(atracID, 1);
    e.lib.release_atrac_id(atracID);
    line("Stream types:");
    const int halfway = e.lib.set_halfway_buffer_and_get_id(e.m, buf, size / 2, size);
    set("  Halfway", atracID, second, 0x1000);
    e.lib.release_atrac_id(halfway);
    set_file("  No loop", at3, second, 0x1000);
    set_file("  End loop", looped(at3, 2048, 249548), second, 0x1000);
    set_file("  Middle loop", looped(at3, 2048, 200000), second, 0x1000);
    set_file("  Late loop", looped(at3, 200000, 249548), second, 0x1000);
    line("Stream pointers:");
    set_file("  Valid address", looped(at3, 2048, 200000), second, 0x1000);
    set_file("  Invalid address", looped(at3, 2048, 200000), 0, 0x1000);
    line("Sizes:");
    set_file("  Size 0x467", looped(at3, 2048, 200000), second, 376 * 3 - 1);
    set_file("  Size 0x468", looped(at3, 2048, 200000), second, 376 * 3);
    set_file("  Larger than file", looped(at3, 2048, 200000), second, size + 0x2000);
    set_file("  0x177 with one frame", looped(at3, 2048, 247500), second, 375);
    set_file("  0x178 with one frame", looped(at3, 2048, 247500), second, 376);
    set_file("  Zero near end", looped(at3, 2048, 249540), second, 0);
    line("With second buffer:");
    const auto h = set_half(e, looped(at3, 2048, 200000));
    std::uint32_t off = 0, bytes = 0;
    e.lib.get_second_buffer_info(h.id, &off, &bytes);
    const auto sb = e.alloc(bytes);
    line("  Set buffer: %08x", e.lib.set_second_buffer(h.id, sb, bytes));
    set("  Set again", h.id, sb, bytes);
    set("  Set to NULL", h.id, 0, bytes);
    e.lib.release_atrac_id(h.id);
    compare("second/setbuffer");
}

// shared.h schedfResetBuffer: pointers inside the buffer print as p+offset.
void reset_buffer_lines(const p3p3ds::hle::atrac::BufferInfo &b, std::uint32_t base) {
    auto one = [&](const char *tag, std::uint32_t ptr, std::uint32_t writable, std::uint32_t min, std::uint32_t file) {
        const std::int32_t diff = static_cast<std::int32_t>(ptr - base);
        if (diff >= 0 && diff < 0x10000) line("   %s: write=p+0x%x, writable=%08x, min=%08x, file=%08x", tag, diff, writable, min, file);
        else line("   %s: write=0x%x, writable=%08x, min=%08x, file=%08x", tag, ptr, writable, min, file);
    };
    one("#1", b.writePositionFirstBuf, b.writableByteFirstBuf, b.minWriteByteFirstBuf, b.readPositionFirstBuf);
    if (b.readPositionSecondBuf != 0u) one("#2", b.writePositionSecondBuf, b.writableByteSecondBuf, b.minWriteByteSecondBuf, b.readPositionSecondBuf);
}
p3p3ds::hle::atrac::BufferInfo filled_cc() {
    p3p3ds::hle::atrac::BufferInfo b;
    b.writePositionFirstBuf = b.writableByteFirstBuf = b.minWriteByteFirstBuf = b.readPositionFirstBuf = 0xCCCCCCCCu;
    b.writePositionSecondBuf = b.writableByteSecondBuf = b.minWriteByteSecondBuf = b.readPositionSecondBuf = 0xCCCCCCCCu;
    return b;
}

void test_second_resetting() {
    Env e;
    const auto at3 = read_file("sample.at3");
    const auto file = looped(at3, 2048, 200000);
    const auto h = set_half(e, file);
    if (e.lib.is_second_buffer_needed(h.id) != 1) { line("FATAL: Does not need second buffer."); compare("second/resetting"); return; }
    auto info_funcs = [&]() {
        const std::pair<const char *, std::uint32_t> cases[] = {{"  Reset to start", 0}, {"  Reset before loop", 200000 - 2048 * 5}, {"  Reset to loop", 200000},
                                                                {"  Reset end of loop frame", 200335 + 368}, {"  Reset after loop", 200335 + 369}, {"  Reset to end", 247500}};
        for (const auto &[title, sample] : cases) {
            auto b = filled_cc();
            line("%s: %08x", title, e.lib.get_buffer_info_for_resetting(e.m, h.id, sample, &b));
            reset_buffer_lines(b, h.buffer);
        }
    };
    auto pos_funcs = [&]() {
        const std::pair<const char *, std::uint32_t> cases[] = {{"  Play at start", 0}, {"  Play before loop", 200000 - 2048 * 5}, {"  Play at loop", 200000},
                                                                {"  Play at end of loop frame", 200335 + 368}, {"  Play after loop", 200335 + 369}, {"  Play at end", 247500}};
        for (const auto &[title, sample] : cases) {
            p3p3ds::hle::atrac::BufferInfo b{};
            const auto r = e.lib.get_buffer_info_for_resetting(e.m, h.id, sample, &b);
            line("  %08x: Reset info for sample %d", r, sample);
            reset_buffer_lines(b, h.buffer);
            if (r == 0) {
                for (std::uint32_t k = 0; k < b.minWriteByteFirstBuf; ++k) e.m.store8(b.writePositionFirstBuf + k, file[b.readPositionFirstBuf + k]);
                line("%s: %08x", title, e.lib.reset_play_position(e.m, h.id, sample, b.minWriteByteFirstBuf, 0));
            } else {
                line("%s: %08x", title, e.lib.reset_play_position(e.m, h.id, sample, 0, 0));
            }
        }
    };
    line("Reset info without second buffer:");
    info_funcs();
    line("Reset pos without second buffer:");
    pos_funcs();
    std::uint32_t off = 0, bytes = 0;
    e.lib.get_second_buffer_info(h.id, &off, &bytes);
    const auto second = e.alloc(bytes);
    for (std::uint32_t k = 0; k < bytes; ++k) e.m.store8(second + k, file[off + k]);
    e.lib.set_second_buffer(h.id, second, 368 * 5);
    line("Reset info with partial second buffer:");
    info_funcs();
    line("Reset pos with partial second buffer:");
    pos_funcs();
    e.lib.set_second_buffer(h.id, second, bytes);
    line("Reset info with full second buffer:");
    info_funcs();
    line("Reset pos with full second buffer:");
    pos_funcs();
    e.lib.release_atrac_id(h.id);
    compare("second/resetting");
}

// shared.cpp LogAtracContext from the model's state (the unk[] words are not kept).
void log_context(Env &e, int id, std::uint32_t buffer, std::uint32_t second, bool full) {
    const auto *i = e.lib.info(id);
    if (i == nullptr || i->state <= 0) { line("Context not yet available for atracID %d", id); return; }
    if (full) {
        line("dataOff: %08x sampleSize: %04x codec: %04x channels: %d", i->dataOff, i->sampleSize, i->codec, i->numChan);
        line("endSample: %08x loopStart: %08x loopEnd: %08x", i->endSample, i->loopStart, i->loopEnd);
        line("bufferByte: %08x secondBufferByte: %08x", i->bufferByte, i->secondBufferByte);
    }
    line("decodePos: %08x curBuffer: %d state: %d framesToSkip: %d", i->decodePos, i->unk22, i->state, i->numFrame);
    line("curFileOff: %08x fileDataEnd: %08x loopNum: %d", i->curOff, i->dataEnd, i->loopNum);
    line("streamDataByte: %08x streamOff: %08x streamOff2: %08x codec_err: %03xX", i->streamDataByte, i->unk48, i->unk52,
         static_cast<std::uint32_t>(i->codecErr) >> 4);
    const std::uint32_t bufOff = i->buffer == 0u ? 0u : i->buffer - buffer;
    const std::uint32_t secondOff = second == 0u ? 0xabcdabcdu : (i->secondBuffer == 0u ? 0u : i->secondBuffer - second);
    line("buffer(offset): %08x secondBuffer(offset): %08x firstValidSample: %04x", bufOff, secondOff, static_cast<std::uint32_t>(i->samplesPerChan));
}

// ---- resetting.cpp ---------------------------------------------------------
void test_resetting() {
    Env e;
    const auto at3 = read_file("sample.at3");
    const auto size = static_cast<std::uint32_t>(at3.size());
    const auto buf = e.load(at3);
    const auto data = e.alloc(16384 * 2);
    auto info = [&](const char *title, int id, std::uint32_t sample) {
        auto b = filled_cc();
        line("%s: %08x", title, e.lib.get_buffer_info_for_resetting(e.m, id, sample, &b));
        reset_buffer_lines(b, buf);
    };
    auto decode = [&](int id) {
        std::uint32_t count, finish;
        std::int32_t remain;
        line("  Decode: %08x", e.lib.decode_data(e.m, id, data, &count, &finish, &remain));
    };
    auto reload = [&]() { for (std::uint32_t k = 0; k < size; ++k) e.m.store8(buf + k, at3[k]); };
    int atracID = e.lib.set_halfway_buffer_and_get_id(e.m, buf, size / 2, size / 2);
    line("IDs:");
    info("  Unallocated", 4, 0);
    info("  Invalid", -1, 0);
    info("  Valid", atracID, 0);
    line("Sample values:");
    const std::int32_t samples[] = {0, 1, 1679, 1680, 2046, 2047, 0x1000, 0x47ff, 0x10000, 247499, 247500, 247501, 0x100000, 0x1000000, 0x7FFFFFFF, -1, -2, -2048, -4096};
    for (const auto sample : samples) {
        char title[64];
        std::snprintf(title, sizeof title, "  %08x", static_cast<std::uint32_t>(sample));
        info(title, atracID, static_cast<std::uint32_t>(sample));
    }
    line("Other states:");
    const int without = e.lib.get_atrac_id(0x1000);
    info("  No data", without, 0);
    e.lib.release_atrac_id(without);
    e.lib.set_state_for_test(atracID, 8);
    info("  State 8", atracID, 0);
    e.lib.set_state_for_test(atracID, 16);
    info("  State 16", atracID, 0);
    e.lib.set_state_for_test(atracID, 1);
    e.lib.release_atrac_id(atracID);

    reload();
    atracID = e.lib.set_halfway_buffer_and_get_id(e.m, buf, size, size);
    line("Entire buffer:");
    info("  At start -> 0", atracID, 0);
    decode(atracID);
    info("  After decode 1 -> 0", atracID, 0);
    info("  After decode 1 -> 65536", atracID, 65536);
    decode(atracID);
    info("  After decode 2 -> 65536", atracID, 65536);
    info("  After decode 2 -> 196608", atracID, 196608);
    decode(atracID);
    info("  After decode 3 -> 196608", atracID, 196608);
    e.lib.release_atrac_id(atracID);

    reload();
    atracID = e.lib.set_halfway_buffer_and_get_id(e.m, buf, size / 2, size);
    line("Half buffer:");
    info("  At start -> 0", atracID, 0);
    decode(atracID);
    info("  After decode 1 -> 0", atracID, 0);
    info("  After decode 1 -> 65536", atracID, 65536);
    decode(atracID);
    info("  After decode 2 -> 65536", atracID, 65536);
    log_context(e, atracID, buf, 0, true);
    info("  After decode 2 -> 196608", atracID, 196608);
    log_context(e, atracID, buf, 0, true);
    decode(atracID);
    log_context(e, atracID, buf, 0, true);
    info("  After decode 3 -> 196608", atracID, 196608);
    log_context(e, atracID, buf, 0, true);
    e.lib.release_atrac_id(atracID);

    reload();
    atracID = e.lib.set_halfway_buffer_and_get_id(e.m, buf, size / 2, size / 2);
    line("Streamed buffer:");
    info("  At start -> 0", atracID, 0);
    decode(atracID);
    info("  After decode 1 -> 0", atracID, 0);
    info("  After decode 1 -> 65536", atracID, 65536);
    decode(atracID);
    info("  After decode 2 -> 65536", atracID, 65536);
    info("  After decode 2 -> 196608", atracID, 196608);
    decode(atracID);
    info("  After decode 3 -> 196608", atracID, 196608);
    compare("resetting");
}

// ---- resetpos.cpp ----------------------------------------------------------
// On hardware `input` was allocated 0xB510 bytes after the file data, and
// offsets are printed relative to the file data: the same layout is used.
void test_resetpos() {
    Env e;
    const auto at3 = read_file("sample.at3");
    const auto size = static_cast<std::uint32_t>(at3.size());
    const auto file = e.load(at3);
    const auto input = file + 0xB510u;
    e.next = input + size + 0x40u;
    auto copy_in = [&](std::uint32_t bytes) { for (std::uint32_t k = 0; k < bytes; ++k) e.m.store8(input + k, at3[k]); };
    auto log_reset = [&](int id, std::int32_t sample, const p3p3ds::hle::atrac::BufferInfo &b) { // shared.cpp LogResetBuffer
        line("%08x=sceAtracGetBufferInfoForResetting(%d):", static_cast<std::uint32_t>(id), sample);
        const std::uint32_t w1 = b.writePositionFirstBuf == 0xCCCCCCCCu ? 0xCCCCCCCCu : b.writePositionFirstBuf - file;
        const std::uint32_t w2 = b.writePositionSecondBuf == 0xCCCCCCCCu ? 0xCCCCCCCCu : b.writePositionSecondBuf - file;
        line("  first : writeOffset: %08x writableBytes: %08x minWriteBytes: %08x filePos: %08x", w1, b.writableByteFirstBuf, b.minWriteByteFirstBuf, b.readPositionFirstBuf);
        line("  second: writeOffset: %08x writableBytes: %08x minWriteBytes: %08x filePos: %08x", w2, b.writableByteSecondBuf, b.minWriteByteSecondBuf, b.readPositionSecondBuf);
    };
    auto direct = [&](const char *title, int id, std::uint32_t sample, std::uint32_t first, std::uint32_t second) {
        line("%s: %08x", title, e.lib.reset_play_position(e.m, id, sample, first, second));
    };
    auto simple = [&](const char *title, int id, std::int32_t sample) {
        auto b = filled_cc();
        const auto r = e.lib.get_buffer_info_for_resetting(e.m, id, static_cast<std::uint32_t>(sample), &b);
        log_reset(id, sample, b);
        if (r == 0) {
            for (std::uint32_t k = 0; k < b.minWriteByteFirstBuf; ++k) { // outside the file: whatever memory held (0 here)
                const std::uint32_t at = b.readPositionFirstBuf + k;
                e.m.store8(b.writePositionFirstBuf + k, at < at3.size() ? at3[at] : 0u);
            }
            log_context(e, id, file, 0, true);
            direct(title, id, static_cast<std::uint32_t>(sample), b.minWriteByteFirstBuf, 0);
        } else {
            direct(title, id, static_cast<std::uint32_t>(sample), 0, 0);
        }
    };
    auto sizes = [&](int id) {
        for (const std::int32_t bytes : {-1, 0, 0x100, 0x1000, 0x100000}) {
            char title[128];
            std::snprintf(title, sizeof title, "  First size = %08x", static_cast<std::uint32_t>(bytes));
            direct(title, id, 0, static_cast<std::uint32_t>(bytes), 0);
            std::snprintf(title, sizeof title, "  Second size = %08x", static_cast<std::uint32_t>(bytes));
            direct(title, id, 0, 0, static_cast<std::uint32_t>(bytes));
        }
    };
    copy_in(size / 2);
    int atracID = e.lib.set_halfway_buffer_and_get_id(e.m, input, size / 2, size / 2);
    line("IDs:");
    simple("  Unallocated", 4, 0);
    simple("  Invalid", -1, 0);
    simple("  Valid", atracID, 0);
    line("Sample values:");
    for (const std::int32_t sample : {0, 1, 2046, 2047, 0x1000, 0x47ff, 0x10000, 247499, 247500, 247501, 0x100000, 0x1000000, 0x7FFFFFFF, -1, -2, -2048, -4096}) {
        log_context(e, atracID, file, 0, true);
        char title[128];
        std::snprintf(title, sizeof title, "  %08x", static_cast<std::uint32_t>(sample));
        simple(title, atracID, sample);
    }
    log_context(e, atracID, file, 0, true);
    line("Other states:");
    const int without = e.lib.get_atrac_id(0x1000);
    simple("  No data", without, 0);
    e.lib.release_atrac_id(without);
    e.lib.set_state_for_test(atracID, 8);
    simple("  State 8", atracID, 0);
    e.lib.set_state_for_test(atracID, 16);
    simple("  State 16", atracID, 0);
    e.lib.set_state_for_test(atracID, 1);
    e.lib.release_atrac_id(atracID);

    copy_in(size);
    atracID = e.lib.set_halfway_buffer_and_get_id(e.m, input, size, size);
    line("Entire buffer:");
    simple("  Sample = 0", atracID, 0);
    simple("  Sample = 65536", atracID, 65536);
    simple("  Sample = 196608", atracID, 196608);
    sizes(atracID);
    e.lib.release_atrac_id(atracID);

    copy_in(size / 2);
    atracID = e.lib.set_halfway_buffer_and_get_id(e.m, input, size / 2, size);
    line("Half buffer:");
    simple("  Sample = 0", atracID, 0);
    simple("  Sample = 65536", atracID, 65536);
    simple("  Sample = 196608", atracID, 196608);
    sizes(atracID);
    e.lib.release_atrac_id(atracID);

    copy_in(size / 2);
    atracID = e.lib.set_halfway_buffer_and_get_id(e.m, input, size / 2, size / 2);
    line("Streamed buffer:");
    simple("  Sample = 0", atracID, 0);
    simple("  Sample = 65536", atracID, 65536);
    simple("  Sample = 196608", atracID, 196608);
    sizes(atracID);
    compare("resetpos");
}

// ---- stream.cpp ------------------------------------------------------------
// Each RunAtracTest run is compared on its own (sections start with a line of
// 60 '='). The ATRAC_TEST_CORRUPT run needs a real decoder (it overwrites
// frames with 0xFF and the hardware codec reports errors), so it is reported
// but not counted.
enum StreamMode { kFull = 1, kHalfway = 2, kStream = 3, kHalfwayStream = 4, kMask = 7, kCorrupt = 0x100, kDontRefill = 0x200,
                  kResetEarly = 0x400, kResetLate = 0x800, kResetReloadAll = 0x1000 };

struct GuestFile { // shared.h Atrac3File: host bytes read into guest memory
    const std::vector<std::uint8_t> &data;
    std::uint32_t pos = 0;
    void seek(std::uint32_t p) { pos = p; }
    std::uint32_t tell() const { return pos; }
    int read(Env &e, std::uint32_t to, std::uint32_t size) {
        if (pos + size > data.size()) {
            raw("Read past the end! pos=%d readSize=%d size_=%d\n", pos, size, static_cast<int>(data.size()));
            size = static_cast<std::uint32_t>(data.size()) - pos;
        }
        for (std::uint32_t k = 0; k < size; ++k) e.m.store8(to + k, data[pos + k]);
        pos += size;
        return static_cast<int>(size);
    }
};

void hex_dump16(Env &e, std::uint32_t at) {
    std::uint8_t b[16];
    for (std::uint32_t k = 0; k < 16u; ++k) b[k] = e.m.load8(at + k);
    raw("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

void log_context_raw(Env &e, int id, std::uint32_t buffer, std::uint32_t second, bool full) {
    log_context(e, id, buffer, second, full);
    for (auto &s : g_out) { g_raw += s; g_raw += '\n'; }
    g_out.clear();
}

void log_reset_raw(std::uint32_t result, std::int32_t sample, const p3p3ds::hle::atrac::BufferInfo &b, std::uint32_t base) {
    raw("%08x=sceAtracGetBufferInfoForResetting(%d):\n", result, sample);
    const std::uint32_t w1 = b.writePositionFirstBuf == 0xCCCCCCCCu ? 0xCCCCCCCCu : b.writePositionFirstBuf - base;
    const std::uint32_t w2 = b.writePositionSecondBuf == 0xCCCCCCCCu ? 0xCCCCCCCCu : b.writePositionSecondBuf - base;
    raw("  first : writeOffset: %08x writableBytes: %08x minWriteBytes: %08x filePos: %08x\n", w1, b.writableByteFirstBuf, b.minWriteByteFirstBuf, b.readPositionFirstBuf);
    raw("  second: writeOffset: %08x writableBytes: %08x minWriteBytes: %08x filePos: %08x\n", w2, b.writableByteSecondBuf, b.minWriteByteSecondBuf, b.readPositionSecondBuf);
}

void run_atrac_test(Env &e, const std::vector<std::uint8_t> &bytes, int mode, int requestedBufSize, int minRemain, int loopCount) {
    GuestFile file{bytes};
    const char *modeName = (mode & kMask) == kFull ? "full" : (mode & kMask) == kHalfway ? "halfway" : (mode & kMask) == kStream ? "stream" : "N/A";
    raw("============================================================\n");
    raw("AtracTest: 'unk', mode %s, buffer size %08x, min remain %d:\n", modeName, requestedBufSize, minRemain);
    const int at3Size = static_cast<int>(bytes.size());
    std::uint32_t fileSize = 0;
    for (int b = 0; b < 4; ++b) fileSize |= static_cast<std::uint32_t>(bytes[4 + b]) << (8 * b);
    raw("filesize (according to header) = 0x%08x\n", fileSize);
    {
        const auto hdr = e.alloc(16);
        for (std::uint32_t k = 0; k < 16u; ++k) e.m.store8(hdr + k, bytes[k]);
        hex_dump16(e, hdr);
    }
    raw("at3size = 0x%08x\n", at3Size);
    int bufSize = 0, loadBytes = 0;
    switch (mode & kMask) {
    case kStream: bufSize = loadBytes = requestedBufSize; raw("Streaming. buf_size: %08x\n", bufSize); break;
    case kHalfway:
        raw("Halfway: Creating a buffer that fits the full %08x bytes, but partially filled.\n", at3Size);
        bufSize = at3Size;
        loadBytes = std::min(requestedBufSize, at3Size);
        break;
    case kHalfwayStream:
        if (requestedBufSize >= at3Size) {
            raw("UNINTENDED: Creating a buffer that fits the full %08x bytes, but partially filled.\n", at3Size);
            bufSize = at3Size;
            loadBytes = requestedBufSize;
        } else {
            raw("Creating a streaming buffer that's partially filled.\n");
            bufSize = requestedBufSize;
            loadBytes = 0x800;
        }
        break;
    case kFull: raw("Full: Creating a buffer that fits all the %08x bytes.\n", at3Size); bufSize = loadBytes = at3Size; break;
    }
    const auto data = e.alloc(static_cast<std::uint32_t>(bufSize));
    constexpr int kDecodeSize = 32 * 1024, kDecBuffers = 2;
    const auto decode = e.alloc(kDecodeSize * kDecBuffers);
    e.m.zero(data, static_cast<std::uint32_t>(bufSize));
    (void)file.read(e, data, static_cast<std::uint32_t>(loadBytes));
    if (mode & kCorrupt) {
        raw("Corrupting the data.\n");
        int at = loadBytes - 0x200;
        if (at > 0x4000) at = 0x4000;
        for (int k = 0; k < 0x200; ++k) e.m.store8(data + static_cast<std::uint32_t>(at + k), 0xFF);
    }
    // sceUtilityLoadModule: the test never unloads, so later runs see 0x80111102 (already loaded).
    static bool modulesLoaded = false;
    if (!modulesLoaded) raw("Audio modules: OK\n");
    else raw("Audio modules: Failed %08x %08x\n", 0x80111102u, 0x80111102u);
    modulesLoaded = true;
    raw("Header: %c%c%c%c\n", e.m.load8(data), e.m.load8(data + 1), e.m.load8(data + 2), e.m.load8(data + 3));
    raw("at3_size: %d bufSize: %08x. minRemain: %d.\n", at3Size, bufSize, minRemain);
    int atracID;
    if ((mode & kMask) == kStream || (mode & kMask) == kFull) {
        atracID = e.lib.set_halfway_buffer_and_get_id(e.m, data, static_cast<std::uint32_t>(loadBytes), static_cast<std::uint32_t>(loadBytes));
        if (atracID < 0) { raw("sceAtracSetDataAndGetID: Failed %08x\n", atracID); return; }
        raw("sceAtracSetDataAndGetID: OK, size=%08x (file_size: %08x)\n", loadBytes, at3Size);
    } else {
        atracID = e.lib.set_halfway_buffer_and_get_id(e.m, data, static_cast<std::uint32_t>(loadBytes), static_cast<std::uint32_t>(bufSize));
        if (atracID < 0) { raw("sceAtracSetHalfwayBufferAndGetID: Failed %08x\n", atracID); return; }
        raw("sceAtracSetHalfwayBufferAndGetID: OK, load=%08x buf=%08x file_size: %08x\n", loadBytes, bufSize, at3Size);
    }
    hex_dump16(e, data);
    log_context_raw(e, atracID, data, 0, true);
    std::int32_t endSample, loopStart, loopEnd;
    std::int32_t result = e.lib.get_sound_sample(atracID, &endSample, &loopStart, &loopEnd);
    raw("%08x=sceAtracGetSoundSample: endSample:%08x, loopStart:%08x, loopEnd:%08x\n", result, endSample, loopStart, loopEnd);
    std::uint32_t value;
    result = e.lib.get_bitrate(atracID, &value);
    raw("%08x=sceAtracGetBitrate: %d\n", result, value);
    result = e.lib.get_channel(atracID, &value);
    raw("%08x=sceAtracGetChannel: %d\n", result, value);
    result = e.lib.get_output_channel(atracID, &value);
    raw("%08x=sceAtracGetOutputChannel: %d\n", result, value);
    std::int32_t loopNum = static_cast<std::int32_t>(0xcccccccc);
    std::uint32_t loopStatus = 0xcccccccc;
    result = e.lib.get_loop_status(atracID, &loopNum, &loopStatus);
    raw("%08x=sceAtracGetLoopStatus(%d, %d)\n", result, loopNum, static_cast<std::int32_t>(loopStatus));
    if (loopCount >= -1) {
        result = e.lib.set_loop_num(atracID, loopCount);
        raw("%08x=sceAtracSetLoopNum(%d)\n", result, loopCount);
    }
    std::uint32_t maxSamples = 0;
    result = e.lib.get_max_sample(atracID, &maxSamples);
    raw("%08x=sceAtracGetMaxSample: %d (%08x)\n", result, maxSamples, maxSamples);
    std::uint32_t nextSamples = 0;
    result = e.lib.get_next_sample(atracID, &nextSamples);
    raw("%08x=sceAtracGetNextSample: %d (%08x)\n", result, nextSamples, nextSamples);
    std::uint32_t second = 0, secondPos = 0, secondBytes = 0;
    result = e.lib.get_second_buffer_info(atracID, &secondPos, &secondBytes);
    raw("%08x=sceAtracGetSecondBufferInfo: %u, %u\n", result, secondPos, secondBytes);
    if (result == 0) {
        raw("Filling up a second data buffer according to specifications.\n");
        second = e.alloc(secondBytes);
        e.m.zero(second, secondBytes);
        file.seek(secondPos);
        if (file.read(e, second, secondBytes) != static_cast<int>(secondBytes)) raw("File read for second buffer failed.\n");
        file.seek(0);
        e.lib.set_second_buffer(atracID, second, secondBytes);
    } else {
        e.lib.set_second_buffer(atracID, 0, 0);
    }
    std::int32_t remainFrame = static_cast<std::int32_t>(0xcccccccc);
    e.lib.get_remain_frame(atracID, &remainFrame);
    raw("sceAtracGetRemainFrame(): %d\n\n", remainFrame);
    std::uint32_t writePtr = 0, bytesToRead = 0, readFileOffset = 0;
    result = e.lib.get_stream_data_info(atracID, &writePtr, &bytesToRead, &readFileOffset);
    raw("%i=sceAtracGetStreamDataInfo: %d (offset), %d, %d (%08x %08x %08x)\n", result, static_cast<int>(writePtr - data), bytesToRead, readFileOffset,
        writePtr - data, bytesToRead, readFileOffset);
    if (mode & kResetEarly) {
        raw("==========================\n");
        constexpr int kSeek = 0x3000;
        raw("Option enabled: Resetting the buffer position to %d.\n", kSeek);
        auto info = filled_cc();
        result = e.lib.get_buffer_info_for_resetting(e.m, atracID, kSeek, &info);
        log_reset_raw(static_cast<std::uint32_t>(result), kSeek, info, data);
        int toWrite = static_cast<int>(info.writableByteFirstBuf);
        if (!(mode & kResetReloadAll) && toWrite > requestedBufSize) toWrite = requestedBufSize;
        raw("Performing actions to reset the buffer - fread of %d/%d bytes from %d in file, to offset %d\n", toWrite, info.writableByteFirstBuf,
            info.readPositionFirstBuf, static_cast<int>(info.writePositionFirstBuf - data));
        file.seek(info.readPositionFirstBuf);
        const int written1 = file.read(e, info.writePositionFirstBuf, static_cast<std::uint32_t>(toWrite));
        log_context_raw(e, atracID, data, second, true);
        result = e.lib.reset_play_position(e.m, atracID, kSeek, static_cast<std::uint32_t>(written1), 0);
        raw("%08x=sceAtracResetPlayPosition(%d, %d, %d)\n", result, kSeek, written1, 0);
    }
    bool first = true, quit = false;
    int count = 0, end = 3, decIndex = 0;
    std::uint32_t samples = 0;
    while (!(quit && end == 0)) {
        if (quit) --end;
        log_context_raw(e, atracID, data, second, first);
        const auto frame = decode + static_cast<std::uint32_t>(kDecodeSize * decIndex);
        e.lib.get_loop_status(atracID, &loopNum, &loopStatus);
        std::uint32_t nextPos = 0;
        result = e.lib.get_next_decode_position(atracID, &nextPos);
        const std::int32_t result2 = e.lib.get_next_sample(atracID, &nextSamples);
        raw("%08x=sceAtracGetNextDecodePosition: pos=%d (%08x) (loopnum %d status %d)\n", result, nextPos, nextPos, loopNum, static_cast<std::int32_t>(loopStatus));
        raw("%08x=sceAtracGetNextSample: samples=%d (%80x)", result2, nextSamples, nextSamples);
        std::uint32_t finish = 0;
        result = e.lib.decode_data(e.m, atracID, frame, &samples, &finish, &remainFrame);
        if (finish) { raw("Finish flag hit, stopping soon. result=%08x\n", result); quit = true; }
        else raw("(no finish flag) result=%08x\n", result);
        if (result) {
            raw("%08x=sceAtracDecodeData error: samples: %08x, finish: %08x, remainFrame: %d\n", result, samples, finish, remainFrame);
            quit = true;
        } else {
            raw("%08x=sceAtracDecodeData: samples: %08x, finish: %08x, remainFrame: %d\n", result, samples, finish, remainFrame);
        }
        if (first && samples < maxSamples) raw("Deglitching first frame\n");
        raw("========\n");
        result = e.lib.get_stream_data_info(atracID, &writePtr, &bytesToRead, &readFileOffset);
        raw("%i=sceAtracGetStreamDataInfo: %d (off), %d, %d (%08x %08x %08x)\n", result, static_cast<int>(writePtr - data), bytesToRead, readFileOffset,
            writePtr - data, bytesToRead, readFileOffset);
        if (remainFrame >= 0 && remainFrame < minRemain && bytesToRead > 0 && (mode & kDontRefill) == 0) {
            const int filePos = static_cast<int>(file.tell());
            if ((mode & kMask) == kHalfway) bytesToRead = std::min<std::uint32_t>(bytesToRead, static_cast<std::uint32_t>(requestedBufSize));
            if (static_cast<int>(readFileOffset) != filePos) {
                raw("Calling fread (%d) (!!!! predicted to be at %d, is at %d). Seeking.\n", bytesToRead, readFileOffset, filePos);
                file.seek(readFileOffset);
            } else {
                raw("Calling fread (%d) (at file offset %d)\n", bytesToRead, readFileOffset);
            }
            const int bytesRead = file.read(e, writePtr, bytesToRead);
            if (bytesRead != static_cast<int>(bytesToRead)) { raw("!!!! fread error: %d != %d\n", bytesRead, bytesToRead); return; }
            log_context_raw(e, atracID, data, second, first);
            result = e.lib.add_stream_data(atracID, bytesToRead);
            if (result) { raw("%08x=sceAtracAddStreamData(%d) error\n", result, bytesToRead); return; }
            raw("%08x=sceAtracAddStreamData: %08x\n", result, bytesToRead);
            raw("========\n");
            result = e.lib.get_stream_data_info(atracID, &writePtr, &bytesToRead, &readFileOffset);
            raw("%i=sceAtracGetStreamDataInfo: %d (off), %d, %d (%08x %08x %08x)\n", result, static_cast<int>(writePtr - data), bytesToRead, readFileOffset,
                writePtr - data, bytesToRead, readFileOffset);
        }
        std::int32_t internalError = 0;
        result = e.lib.get_internal_error_info(atracID, &internalError);
        if (internalError != 0 || result != 0) raw("%08x=sceAtracGetInternalErrorInfo(): %08x\n", result, internalError);
        first = false;
        if (++decIndex == kDecBuffers) decIndex = 0;
        ++count;
    }
    loopNum = static_cast<std::int32_t>(0xcccccccc);
    loopStatus = 0xcccccccc;
    result = e.lib.get_loop_status(atracID, &loopNum, &loopStatus);
    raw("(end) %08x=sceAtracGetLoopStatus(%d, %08x)\n", result, loopNum, loopStatus);
    result = e.lib.get_next_sample(atracID, &nextSamples);
    raw("(end) %08x=sceAtracGetNextSample: %d (%08x)\n", result, nextSamples, nextSamples);
    for (const std::int32_t offset : {-2048, -10000, 0, 1, 2047, 2048, 2049, 4096, 16384, 20000, 40000, 1000000000}) { // LogResetBufferInfo
        auto info = filled_cc();
        const auto r = e.lib.get_buffer_info_for_resetting(e.m, atracID, static_cast<std::uint32_t>(offset), &info);
        log_reset_raw(static_cast<std::uint32_t>(r), offset, info, data);
    }
    log_context_raw(e, atracID, data, second, true);
    if (end) raw("reached end of file\n");
    raw("sceAtracReleaseAtracID: %08X\n\n", e.lib.release_atrac_id(atracID));
    raw("Done! req=%d\n", requestedBufSize);
}

std::vector<std::vector<std::string>> sections(const std::string &text) {
    std::vector<std::vector<std::string>> out(1);
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string s = text.substr(start, end - start);
        while (!s.empty() && s.back() == '\r') s.pop_back();
        if (s == std::string(60, '=')) out.emplace_back();
        if (!s.empty()) out.back().push_back(s);
        start = end + 1;
    }
    return out;
}

void test_stream() {
    Env e;
    const auto at3 = read_file("sample.at3");
    raw("file size: %d\n", static_cast<int>(at3.size()));
    run_atrac_test(e, at3, kHalfwayStream | kResetEarly, 32 * 1024, 10, 0);
    run_atrac_test(e, at3, kHalfwayStream, 29 * 1024, 10, 0);
    run_atrac_test(e, at3, kHalfway, 0x4300, 10, 0);
    const auto loop1 = looped(at3, 2048 + 2048 * 10 + 256, 249548, 1);
    const auto loop2 = looped(at3, 2048 + 2048 * 10 + 256, 100000, 1);
    run_atrac_test(e, loop1, kStream, 0x4000, 32, 1);
    run_atrac_test(e, loop2, kStream, 0x4000, 32, 2);
    run_atrac_test(e, at3, kStream, 0x4500, 10, 0);
    run_atrac_test(e, at3, kStream | kCorrupt, 0x3700, 10, 0);
    run_atrac_test(e, at3, kStream | kDontRefill, 0x4300, 10, 0);

    std::ifstream in(kDir / "stream.expected");
    const std::string want_text{std::istreambuf_iterator<char>(in), {}};
    const auto want = sections(want_text), got = sections(g_raw);
    g_raw.clear();
    if (want.size() != got.size()) { ++failures; std::cerr << "stream: " << got.size() << " sections, expected " << want.size() << "\n"; return; }
    for (std::size_t k = 0; k < want.size(); ++k) {
        const bool corrupt = k == 7; // ATRAC_TEST_CORRUPT run
        std::size_t bad = 0, firstBad = 0;
        const std::size_t n = std::max(want[k].size(), got[k].size());
        for (std::size_t j = 0; j < n; ++j) {
            const std::string w = j < want[k].size() ? want[k][j] : "<missing>", g = j < got[k].size() ? got[k][j] : "<missing>";
            if (w != g) {
                if (bad == 0) firstBad = j;
                if (bad < 6 && !corrupt) std::cerr << "stream run " << k << " line " << j + 1 << "\n  want: " << w << "\n  got:  " << g << "\n";
                ++bad;
            }
        }
        if (corrupt) {
            std::cout << "stream run " << k << " (corrupt data, needs a real decoder): " << (bad ? firstBad : want[k].size()) << " of "
                      << want[k].size() << " lines match before the first difference\n";
        } else if (bad) {
            ++failures;
            std::cerr << "stream run " << k << ": " << bad << " differing lines\n";
        } else {
            std::cout << "stream run " << k << ": " << got[k].size() << " lines match\n";
        }
    }
}
} // namespace

int main() {
    if (!std::filesystem::exists(kDir / "sample.at3")) { std::cout << "SKIP: pspautotests clone absent\n"; return 77; }
    test_setdata();
    test_decode();
    test_getsoundsample();
    test_addstreamdata();
    test_getremainframe();
    test_second_getinfo();
    test_second_needed();
    test_second_setbuffer();
    test_second_resetting();
    test_resetting();
    test_resetpos();
    test_stream();
    std::cout << "atrac replay failures=" << failures << "\n";
    return failures ? 1 : 0;
}
