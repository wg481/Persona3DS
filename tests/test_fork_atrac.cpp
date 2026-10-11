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
    std::cout << "atrac replay failures=" << failures << "\n";
    return failures ? 1 : 0;
}
