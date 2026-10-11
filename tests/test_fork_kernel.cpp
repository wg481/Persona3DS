// Kernel imports added by the Persona3DS fork (core/src/hle/fork_kernel.cpp),
// checked against uOFW's semantics and dispatched through the runtime as the
// game reaches them.
#include "p3p3ds/hle/fork_kernel.hpp"
#include "p3p3ds/hle/hle_modules.hpp"
#include "p3p3ds/kernel_state.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "p3p3ds/vfs.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <iostream>
#include <string>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << "\n"; ++failures; } } while (0)

// SysMemUserForUser::0x91DE343C through the import table, as Persona's
// module start calls it (first import executed, maintainer's run 2026-10-11).
struct Call {
    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    Call() { p3p3ds::hle::register_all_hle_modules(rt, kernel); }
    std::uint32_t operator()(std::uint32_t version) {
        rt.cpu().set_gpr(4, version);
        rt.cpu().set_gpr(2, 0xDEADBEEFu);
        rt.invoke_import("SysMemUserForUser", 0x91DE343Cu, rt.cpu());
        return rt.cpu().gpr[2];
    }
};
// scePower::0x04B7766E through the import table, replaying the parts of
// pspautotests tests/power/power.c that do not need scePowerUnregisterCallback
// (results from power.expected, recorded on hardware).
struct Power {
    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    Power() {
        p3p3ds::hle::register_all_hle_modules(rt, kernel);
        kernel.threads().init_root_thread("root", 0x08804000u, 0x09FFFF00u, 0u);
    }
    std::int32_t cb(const char *name, std::uint32_t arg) { return kernel.threads().create_callback(name, 0x08900000u, arg); }
    std::uint32_t reg(std::int32_t slot, std::int32_t cbid) {
        rt.cpu().set_gpr(4, static_cast<std::uint32_t>(slot));
        rt.cpu().set_gpr(5, static_cast<std::uint32_t>(cbid));
        rt.invoke_import("scePower", 0x04B7766Eu, rt.cpu());
        return rt.cpu().gpr[2];
    }
    const p3p3ds::hle::CallbackObject &object(std::int32_t uid) { return *kernel.threads().get_callback(uid); }
};

void test_power_register_callback() {
    { // Section 1: auto slots 0, 1, 2; the callback registered twice is notified twice.
        Power p;
        const auto cb1 = p.cb("powerHandler", 0x1234u), cb2 = p.cb("powerHandler2", 0x4567u);
        CHECK(p.reg(-1, cb1) == 0u);
        CHECK(p.reg(-1, cb2) == 1u);
        CHECK(p.reg(-1, cb2) == 2u);
        CHECK(!p.rt.stopped());
        CHECK(p.object(cb1).notify_count == 1); // "powerHandler called: 00000001, 00000080"
        CHECK(p.object(cb2).notify_count == 2); // "powerHandler2 called: 00000002, 00000080"
        CHECK((p.object(cb1).notify_arg & 0x80u) != 0u);
        CHECK((p.object(cb2).notify_arg & 0x80u) != 0u);
    }
    { // Section 3: explicit slot and errors.
        Power p;
        const auto cb1 = p.cb("powerHandler", 0x1234u), cb2 = p.cb("powerHandler2", 0x4567u);
        CHECK(p.reg(0, cb1) == 0u);           // Normal
        CHECK(p.reg(0, 0) == 0x80000100u);    // Invalid CB
        CHECK(p.reg(-4, cb1) == 0x80000102u); // Too low slot
        CHECK(p.reg(31, cb1) == 0x80000023u); // Too high (31)
        CHECK(p.reg(32, cb1) == 0x80000102u); // Too high (32)
        CHECK(p.reg(0, cb1) == 0x80000020u);  // Twice (same)
        CHECK(p.reg(0, cb2) == 0x80000020u);  // Twice (different)
        CHECK(p.object(cb1).notify_count == 1);
        CHECK(p.object(cb2).notify_count == 0);
    }
    { // Section 4: 16 slots, then 0x80000022.
        Power p;
        const auto cb2 = p.cb("powerHandler2", 0x4567u);
        for (std::uint32_t i = 0; i < 16; ++i) CHECK(p.reg(-1, cb2) == i);
        CHECK(p.reg(-1, cb2) == 0x80000022u);
    }
}
// sceCtrl::0x6A2774F3 through the import table (uOFW ctrl.c semantics).
void test_ctrl_sampling_cycle() {
    struct Ctrl {
        psprecomp::Runtime rt;
        p3p3ds::KernelState kernel;
        Ctrl() { p3p3ds::hle::register_all_hle_modules(rt, kernel); }
        std::uint32_t set(std::uint32_t cycle) {
            rt.cpu().set_gpr(4, cycle);
            rt.cpu().set_gpr(2, 0xDEADBEEFu);
            rt.invoke_import("sceCtrl", 0x6A2774F3u, rt.cpu());
            return rt.cpu().gpr[2];
        }
    };
    {
        Ctrl c;
        CHECK(c.set(0u) == 0u); // pspautotests ctrl/vblank.expected: "sceCtrlSetSamplingCycle: 00000000"
        CHECK(c.set(0u) == 0u);
        CHECK(c.set(1u) == 0x800001FEu);
        CHECK(c.set(5554u) == 0x800001FEu);
        CHECK(c.set(20001u) == 0x800001FEu);
        CHECK(c.set(0xFFFFFFFFu) == 0x800001FEu);
        CHECK(!c.rt.stopped());
    }
    for (const std::uint32_t cycle : {5555u, 16666u, 20000u}) {
        Ctrl c;
        (void)c.set(cycle);
        CHECK(c.rt.stopped());
        CHECK(c.rt.stop_reason().find("sceCtrlSetSamplingCycle") != std::string::npos);
    }
}
// sceRtc::0xE7C27D1B (uOFW rtc.c; pspautotests rtc/rtc.expected).
void test_rtc_local_time() {
    using p3p3ds::hle::psp_date_time;
    { const auto t = psp_date_time(0u); CHECK(t.year == 1970 && t.month == 1 && t.day == 1 && t.hour == 0 && t.second == 0); }
    { // 2008 is a leap year: day 60 after 2008-01-01 is March 1st.
        const auto t = psp_date_time((p3p3ds::hle::kForkRtcEpochSeconds + 60u * 86400u + 3661u) * 1000000u + 500000u);
        CHECK(t.year == 2008 && t.month == 3 && t.day == 1 && t.hour == 1 && t.minute == 1 && t.second == 1 && t.microsecond == 500000u);
    }
    { const auto t = psp_date_time((p3p3ds::hle::kForkRtcEpochSeconds + 59u * 86400u) * 1000000u); CHECK(t.month == 2 && t.day == 29); }
    { const auto t = psp_date_time(951782400ull * 1000000u); CHECK(t.year == 2000 && t.month == 2 && t.day == 29); } // 2000-02-29 00:00 UTC

    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    constexpr std::uint32_t kOut = 0x08B00000u;
    rt.memory().store32(kOut + 12u, 0xFFFFFFFFu);
    rt.cpu().set_gpr(4, kOut);
    rt.invoke_import("sceRtc", 0xE7C27D1Bu, rt.cpu());
    CHECK(!rt.stopped());
    CHECK(rt.cpu().gpr[2] == 0u); // "Normal: 00000000"
    // At virtual time 0: the fixed epoch 2008-01-01 00:00:00.000000 (checkPspTime: year > 1980, valid date/time).
    CHECK(rt.memory().load16(kOut) == 2008u && rt.memory().load16(kOut + 2u) == 1u && rt.memory().load16(kOut + 4u) == 1u);
    CHECK(rt.memory().load16(kOut + 6u) == 0u && rt.memory().load16(kOut + 8u) == 0u && rt.memory().load16(kOut + 10u) == 0u);
    CHECK(rt.memory().load32(kOut + 12u) == 0u);
    rt.cpu().set_gpr(4, 0x88000000u); // kernel address from user mode
    rt.invoke_import("sceRtc", 0xE7C27D1Bu, rt.cpu());
    CHECK(rt.cpu().gpr[2] == 0x80000023u);
    rt.cpu().set_gpr(4, 0u);
    rt.invoke_import("sceRtc", 0xE7C27D1Bu, rt.cpu());
    CHECK(rt.stopped());
}
// sceRtc::0x6FF40ACC with pspautotests rtc/convert.expected values (hardware).
void test_rtc_get_tick() {
    using p3p3ds::hle::PspDateTime;
    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    constexpr std::uint32_t kDate = 0x08B00000u, kTick = 0x08B00020u;
    auto &m = rt.memory();
    const auto get_tick = [&](PspDateTime t, std::uint64_t *out) {
        m.store16(kDate, t.year); m.store16(kDate + 2u, t.month); m.store16(kDate + 4u, t.day);
        m.store16(kDate + 6u, t.hour); m.store16(kDate + 8u, t.minute); m.store16(kDate + 10u, t.second);
        m.store32(kDate + 12u, t.microsecond);
        rt.cpu().set_gpr(4, kDate);
        rt.cpu().set_gpr(5, kTick);
        rt.invoke_import("sceRtc", 0x6FF40ACCu, rt.cpu());
        *out = m.load32(kTick) | (static_cast<std::uint64_t>(m.load32(kTick + 4u)) << 32);
        return rt.cpu().gpr[2];
    };
    std::uint64_t tick = 0;
    CHECK(get_tick({10, 1, 1, 0, 0, 0, 0}, &tick) == 0u && tick == 283996800000000ull);      // "Normal"
    CHECK(get_tick({9998, 1, 1, 0, 0, 0, 0}, &tick) == 0u && tick == 315474825600000000ull); // "Bad date" (valid)
    CHECK(get_tick({0, 1, 1, 0, 0, 0, 0}, &tick) == 0x800001FEu && tick == 315474825600000000ull); // "Min year", unchanged
    CHECK(get_tick({9999, 1, 1, 0, 0, 0, 0}, &tick) == 0u && tick == 315506361600000000ull); // "Max year"
    CHECK(get_tick({10000, 1, 1, 0, 0, 0, 0}, &tick) == 0x800001FEu && tick == 315506361600000000ull); // "Year overflow"
    CHECK(get_tick({1970, 1, 1, 0, 0, 0, 0}, &tick) == 0u && tick == 62135596800000000ull); // convert.c constant
    CHECK(get_tick({2008, 2, 29, 23, 59, 59, 999999}, &tick) == 0u);
    CHECK(get_tick({2009, 2, 29, 0, 0, 0, 0}, &tick) == 0x800001FEu); // not a leap year
    CHECK(get_tick({2012, 13, 1, 0, 0, 0, 0}, &tick) == 0x800001FEu);
    CHECK(get_tick({2012, 9, 20, 24, 0, 0, 0}, &tick) == 0x800001FEu);
    CHECK(get_tick({2012, 9, 20, 0, 0, 0, 1000000}, &tick) == 0x800001FEu);
    CHECK(!rt.stopped());
    // The clock and the tick agree: the fixed epoch 2008-01-01 is 1199145600 s after 1970.
    CHECK(get_tick(p3p3ds::hle::psp_date_time(p3p3ds::hle::kForkRtcEpochSeconds * 1000000u), &tick) == 0u &&
          tick == 62135596800000000ull + p3p3ds::hle::kForkRtcEpochSeconds * 1000000u);
    rt.cpu().set_gpr(4, kDate);
    rt.cpu().set_gpr(5, 0x88000000u);
    rt.invoke_import("sceRtc", 0x6FF40ACCu, rt.cpu());
    CHECK(rt.cpu().gpr[2] == 0x80000023u);
}
// IoFileMgrForUser::0x54F5FB11: not modelled; the stop names the call (Persona run 8).
void test_devctl_reports_call() {
    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    constexpr std::uint32_t kName = 0x08B00000u;
    const char name[] = "mscmhc0:";
    for (std::uint32_t i = 0; i < sizeof name; ++i) rt.memory().store8(kName + i, static_cast<std::uint8_t>(name[i]));
    rt.cpu().set_gpr(4, kName); rt.cpu().set_gpr(5, 0x02025806u); rt.cpu().set_gpr(6, 0u);
    rt.cpu().set_gpr(7, 0u); rt.cpu().set_gpr(8, 0x08B00100u); rt.cpu().set_gpr(9, 4u);
    rt.invoke_import("IoFileMgrForUser", 0x54F5FB11u, rt.cpu());
    CHECK(rt.stopped());
    CHECK(rt.stop_reason() == "sceIoDevctl(\"mscmhc0:\", 0x02025806, in 0x00000000/0, out 0x08B00100/4) is not modelled yet");
}

// InterruptManager sub-interrupts, intno 30 (vblank): results from
// pspautotests intr/registersub, releasesub, enablesub (hardware).
struct Intr {
    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    Intr() {
        p3p3ds::hle::register_all_hle_modules(rt, kernel);
        kernel.threads().init_root_thread("root", 0x08804000u, 0x09FFFF00u, 0x08AFD790u);
    }
    std::uint32_t call(std::uint32_t nid, std::uint32_t intno, std::uint32_t no, std::uint32_t handler = 0u, std::uint32_t arg = 0u) {
        rt.cpu().set_gpr(4, intno); rt.cpu().set_gpr(5, no); rt.cpu().set_gpr(6, handler); rt.cpu().set_gpr(7, arg);
        rt.invoke_import("InterruptManager", nid, rt.cpu());
        return rt.cpu().gpr[2];
    }
    std::uint32_t reg(std::int32_t no, std::uint32_t h, std::uint32_t arg = 0xDEADBEEFu) { return call(0xCA04A2B9u, 30u, static_cast<std::uint32_t>(no), h, arg); }
    std::uint32_t rel(std::int32_t no) { return call(0xD61E6961u, 30u, static_cast<std::uint32_t>(no)); }
    std::uint32_t en(std::int32_t no) { return call(0xFB8E22ECu, 30u, static_cast<std::uint32_t>(no)); }
    void advance_vblank() { for (int i = 0; i < 16700; ++i) kernel.threads().on_syscall_return(rt, rt.cpu()); }
};

void test_subintr_results() {
    constexpr std::uint32_t kH = 0x0881203Cu;
    { // registersub.expected "sub interrupts" (register, then release)
        Intr t;
        for (std::int32_t no = -2; no < 70; ++no) {
            const auto r = t.reg(no, kH);
            (void)t.rel(no);
            const bool sys = (no >= 18 && no <= 20) || (no >= 24 && no <= 26);
            CHECK(r == (no >= 0 && no <= 15 ? 0u : sys ? 0x80020067u : 0x80020065u));
        }
        CHECK(t.reg(1, 0u) == 0u);              // NULL handler
        CHECK(t.reg(1, 0u) == 0u);              // Twice NULL
        CHECK(t.reg(1, kH) == 0u);              // Twice not NULL
        CHECK(t.reg(1, kH) == 0x80020067u);     // Again not NULL
        CHECK(t.reg(1, 0u) == 0x80020067u);     // NULL afterward
    }
    { // releasesub.expected
        Intr t;
        for (std::int32_t no = -2; no < 70; ++no) {
            (void)t.reg(no, kH);
            const auto r = t.rel(no);
            CHECK(r == (no >= 0 && no <= 15 ? 0u : no >= 16 && no <= 31 ? 0x80020068u : 0x80020065u));
        }
        (void)t.reg(1, 0u);
        CHECK(t.rel(1) == 0x80020068u);         // NULL handler
        CHECK(t.rel(1) == 0x80020068u);         // Twice NULL
        (void)t.reg(1, kH);
        CHECK(t.rel(1) == 0u);                  // Not NULL
        CHECK(t.rel(1) == 0x80020068u);         // Twice not NULL
    }
    { // enablesub.expected
        Intr t;
        (void)t.reg(1, kH);
        CHECK(t.call(0xFB8E22ECu, 70u, 1u) == 0x80020065u);           // Invalid intr
        CHECK(t.call(0xCA04A2B9u, 0xFFFFFFFEu, 0u, 0x0881203Cu) == 0x80020065u); // Interrupt -2
        CHECK(!t.rt.stopped());
        (void)t.call(0xCA04A2B9u, 25u, 0u, 0x0881203Cu);              // valid, not modelled: reported
        CHECK(t.rt.stopped() && t.rt.stop_reason().find("intno 25") != std::string::npos);
    }
    {
        Intr t;
        (void)t.reg(1, kH);
        CHECK(t.en(70) == 0x80020065u);         // Invalid subintr
        CHECK(t.en(1) == 0u);                   // Registered
        CHECK(t.en(1) == 0u);                   // Twice
        CHECK(t.en(3) == 0u);                   // Unregistered
        CHECK(!t.rt.stopped());
    }
}

void test_subintr_dispatch() {
    using TM = p3p3ds::hle::ThreadManager;
    Intr t;
    auto &ctx = t.rt.cpu();
    // Persona's call (PC run 12): intno 30, no 0, handler 0x0881203C, arg 0x090DF380.
    ctx.set_gpr(28, 0x08AFD790u);
    CHECK(t.reg(0, 0x0881203Cu, 0x090DF380u) == 0u);
    CHECK(t.en(0) == 0u);
    CHECK(t.en(3) == 0u);                        // enabled before its handler exists
    CHECK(t.reg(3, 0x08900000u, 0x1234u) == 0u); // ... runs once registered
    CHECK(t.en(5) == 0u);                        // enabled without a handler: never runs
    ctx.pc = 0x08810000u;
    ctx.set_gpr(29, 0x09FFF000u);
    CHECK(!p3p3ds::hle::dispatch_vblank_subinterrupts(t.rt, ctx)); // no vblank yet
    t.advance_vblank();
    ctx.pc = 0x08810000u;
    const auto generation = psprecomp::runtime_thread_switch_generation();
    CHECK(p3p3ds::hle::dispatch_vblank_subinterrupts(t.rt, ctx));
    CHECK(psprecomp::runtime_thread_switch_generation() != generation); // native callers unwind
    CHECK(ctx.pc == 0x0881203Cu && ctx.gpr[4] == 0u && ctx.gpr[5] == 0x090DF380u);
    CHECK(ctx.gpr[28] == 0x08AFD790u && ctx.gpr[31] == TM::kGuestCallReturnSentinel);
    CHECK(!p3p3ds::hle::dispatch_vblank_subinterrupts(t.rt, ctx)); // no nesting
    t.kernel.threads().guest_call_returned(t.rt, ctx);               // handler 0 returns
    CHECK(ctx.pc == 0x08900000u && ctx.gpr[4] == 3u && ctx.gpr[5] == 0x1234u);
    t.kernel.threads().guest_call_returned(t.rt, ctx);               // handler 3 returns
    CHECK(ctx.pc == 0x08810000u && ctx.gpr[29] == 0x09FFF000u);      // interrupted code resumes
    CHECK(!p3p3ds::hle::fork_vblank_subinterrupts().in_handler);
    CHECK(!p3p3ds::hle::dispatch_vblank_subinterrupts(t.rt, ctx));  // once per vblank
    // Released while enabled: does not run again (enablesub "Released while enabled").
    CHECK(t.rel(0) == 0u && t.rel(3) == 0u);
    CHECK(t.reg(0, 0x0881203Cu, 0x090DF380u) == 0u);
    t.advance_vblank();
    CHECK(!p3p3ds::hle::dispatch_vblank_subinterrupts(t.rt, ctx));
    CHECK(!t.rt.stopped());
}

// fatms0: 0x02425823 as Persona calls it (run 10): out = a stack word, outlen 4.
void test_devctl_fat_ready() {
    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    auto &m = rt.memory();
    constexpr std::uint32_t kName = 0x08B00000u, kOut = 0x09FEF9F0u;
    const char name[] = "fatms0:";
    for (std::uint32_t i = 0; i < sizeof name; ++i) m.store8(kName + i, static_cast<std::uint8_t>(name[i]));
    m.store32(kOut, 0xDEADBEEFu);
    rt.cpu().set_gpr(4, kName); rt.cpu().set_gpr(5, 0x02425823u); rt.cpu().set_gpr(6, 0u);
    rt.cpu().set_gpr(7, 0u); rt.cpu().set_gpr(8, kOut); rt.cpu().set_gpr(9, 4u);
    rt.invoke_import("IoFileMgrForUser", 0x54F5FB11u, rt.cpu());
    CHECK(!rt.stopped());
    CHECK(rt.cpu().gpr[2] == 0u);
    CHECK(m.load32(kOut) == 1u);
    rt.cpu().set_gpr(9, 0u); // not observed: still reported, not guessed
    rt.invoke_import("IoFileMgrForUser", 0x54F5FB11u, rt.cpu());
    CHECK(rt.stopped());
}

// ms0: 0x02425818 (SCE_PR_GETDEV) as Persona calls it (run 9): indata = a
// stack word holding a pointer to the SceDevInf right after it.
void test_devctl_ms_free_space() {
    for (const char *name : {"ms0:", "fatms0:"}) {
        psprecomp::Runtime rt;
        p3p3ds::KernelState kernel;
        p3p3ds::hle::register_all_hle_modules(rt, kernel);
        auto &m = rt.memory();
        constexpr std::uint32_t kName = 0x08B00000u, kSp = 0x09FEF9E0u;
        for (std::uint32_t i = 0; name[i] != 0; ++i) m.store8(kName + i, static_cast<std::uint8_t>(name[i]));
        m.store8(kName + static_cast<std::uint32_t>(std::char_traits<char>::length(name)), 0u);
        m.store32(kSp, kSp + 4u);
        rt.cpu().set_gpr(4, kName); rt.cpu().set_gpr(5, 0x02425818u); rt.cpu().set_gpr(6, kSp);
        rt.cpu().set_gpr(7, 4u); rt.cpu().set_gpr(8, 0u); rt.cpu().set_gpr(9, 0u);
        rt.invoke_import("IoFileMgrForUser", 0x54F5FB11u, rt.cpu());
        CHECK(!rt.stopped());
        CHECK(rt.cpu().gpr[2] == 0u);
        const std::uint32_t max = m.load32(kSp + 4u), free = m.load32(kSp + 8u), sectors = m.load32(kSp + 12u);
        const std::uint32_t size = m.load32(kSp + 16u), count = m.load32(kSp + 20u);
        CHECK(size == 512u && count == 64u); // mstick.expected
        constexpr std::uint32_t kMin = (256u * 1000u * 1000u) / (512u * 64u), kMax = 128000000000ull / (512u * 64u) - 1u;
        CHECK(max >= kMin && max <= kMax && free >= kMin && free <= kMax && sectors >= kMin && sectors <= kMax);
        // Persona's check (0x08818918..0x08818938): 32-bit sectorSize * sectorCount * freeClusters >> 10 >= 256.
        const std::uint32_t kib = (size * count * free) >> 10;
        CHECK(kib >= 256u);
        CHECK(static_cast<std::uint64_t>(size) * count * free < 0x100000000ull); // no 32-bit overflow in the game's mult
    }
}

// sceMpegAvcQueryYCbCrSize as Persona calls it (run 13: $a0 = 0x08CC268C,
// mode 1, out = $sp); PPSSPP and JPCSP agree on mode 1 (see fork_kernel.hpp).
void test_mpeg_ycbcr_size() {
    using p3p3ds::hle::mpeg_avc_ycbcr_size_mode1;
    CHECK(mpeg_avc_ycbcr_size_mode1(480u, 272u) == 195968u);  // 128 + 240 * 136 * 6
    CHECK(mpeg_avc_ycbcr_size_mode1(464u, 256u) == 128u + 232u * 128u * 6u);
    CHECK(mpeg_avc_ycbcr_size_mode1(496u, 272u) == 0u);       // too wide
    CHECK(mpeg_avc_ycbcr_size_mode1(480u, 288u) == 0u);       // too tall
    CHECK(mpeg_avc_ycbcr_size_mode1(480u, 270u) == 0u);       // not a multiple of 16
    CHECK(mpeg_avc_ycbcr_size_mode1(16u, 272u) == 0u);        // references disagree
    CHECK(mpeg_avc_ycbcr_size_mode1(480u, 0u) == 0u);

    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    auto &m = rt.memory();
    constexpr std::uint32_t kOut = 0x09FEF8F0u;
    m.store32(kOut, 0xDEADBEEFu);
    rt.cpu().set_gpr(4, 0x08CC268Cu); rt.cpu().set_gpr(5, 1u); rt.cpu().set_gpr(6, 480u);
    rt.cpu().set_gpr(7, 272u); rt.cpu().set_gpr(8, kOut);
    rt.invoke_import("sceMpeg", 0x211A057Cu, rt.cpu());
    CHECK(!rt.stopped());
    CHECK(rt.cpu().gpr[2] == 0u);
    CHECK(m.load32(kOut) == 195968u);
    rt.cpu().set_gpr(5, 2u); // other modes are not observed: reported, not guessed
    rt.invoke_import("sceMpeg", 0x211A057Cu, rt.cpu());
    CHECK(rt.stopped());
    CHECK(rt.stop_reason().find("sceMpegAvcQueryYCbCrSize(mpeg 0x08CC268C, mode 2, 480 x 272") != std::string::npos);
}

// sceMpegAvcInitYCbCr as Persona calls it (run 14, loop at 0x088117F0).
void test_mpeg_ycbcr_init() {
    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    auto &m = rt.memory();
    constexpr std::uint32_t kBuffer = 0x09000000u;
    m.store32(kBuffer + 16u, 0x12345678u);
    auto init = [&](std::uint32_t mode, std::uint32_t width, std::uint32_t height, std::uint32_t buffer) {
        rt.cpu().set_gpr(4, 0x08CC268Cu); rt.cpu().set_gpr(5, mode); rt.cpu().set_gpr(6, width);
        rt.cpu().set_gpr(7, height); rt.cpu().set_gpr(8, buffer); rt.cpu().set_gpr(2, 0xDEADBEEFu);
        rt.invoke_import("sceMpeg", 0x67179B1Bu, rt.cpu());
        return rt.cpu().gpr[2];
    };
    CHECK(init(1u, 480u, 272u, kBuffer) == 0u);
    CHECK(!rt.stopped());
    CHECK(m.load32(kBuffer + 16u) == 0x12345678u); // untouched (PPSSPP behaviour)
    (void)init(1u, 480u, 272u, 0u);                 // invalid buffer: reported
    CHECK(rt.stopped());
    CHECK(rt.stop_reason().find("sceMpegAvcInitYCbCr(mpeg 0x08CC268C, mode 1, 480 x 272, buffer 0x00000000") != std::string::npos);
}

// sceKernelReferEventFlagStatus: pspautotests threads/events/refer and
// create (hardware), and Persona's call (run 15: info->size = 52).
void test_refer_event_flag() {
    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    auto &m = rt.memory();
    constexpr std::uint32_t kInfo = 0x09FEF800u;
    auto refer = [&](std::int32_t uid, std::uint32_t size) {
        m.store32(kInfo, size);
        rt.cpu().set_gpr(4, static_cast<std::uint32_t>(uid)); rt.cpu().set_gpr(5, kInfo);
        rt.invoke_import("ThreadManForUser", 0xA66B0120u, rt.cpu());
        return rt.cpu().gpr[2];
    };
    const auto flag = kernel.threads().create_event_flag("create2", 0x200u, 1u | 8u | 32u);
    for (std::uint32_t i = 4; i < 52u; i += 4) m.store32(kInfo + i, 0xCCCCCCCCu);
    CHECK(refer(flag, 52u) == 0u);
    CHECK(m.load32(kInfo) == 52u);
    CHECK(m.read_c_string(kInfo + 4u, 32u) == "create2");
    CHECK(m.load32(kInfo + 4u + 28u) == 0u); // name padded with zeros
    CHECK(m.load32(kInfo + 36u) == 0x200u);
    CHECK(m.load32(kInfo + 40u) == 0x29u && m.load32(kInfo + 44u) == 0x29u);
    CHECK(m.load32(kInfo + 48u) == 0u);
    CHECK(!rt.stopped());
    // "Long name": 31 characters are kept.
    const auto lng = kernel.threads().create_event_flag("12345678901234567890123456789012", 0u, 0u);
    CHECK(refer(lng, 52u) == 0u);
    CHECK(m.read_c_string(kInfo + 4u, 32u) == "1234567890123456789012345678901");
    // refer.expected: size 0 -> result 0, size stays 0.
    m.store32(kInfo + 36u, 0xDEADBEEFu);
    CHECK(refer(flag, 0u) == 0u);
    CHECK(m.load32(kInfo) == 0u && m.load32(kInfo + 36u) == 0xDEADBEEFu);
    // NULL / Invalid / Deleted -> 0x8002019A.
    CHECK(refer(0, 52u) == 0x8002019Au);
    CHECK(refer(static_cast<std::int32_t>(0xDEADBEEFu), 52u) == 0x8002019Au);
    CHECK(!rt.stopped());
    // Sizes other than 0 and 52 are not modelled: reported.
    (void)refer(flag, 8u);
    CHECK(rt.stopped());
    CHECK(rt.stop_reason().find("sceKernelReferEventFlagStatus(uid " + std::to_string(flag) + ", info 0x09FEF800, size 8)") != std::string::npos);
}

// sceMpegAvcDecodeYCbCr on an instance made by upstream's sceMpegCreate, as
// Persona calls it (run 16): first decode no picture, then one per AU.
void test_mpeg_decode_ycbcr() {
    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    auto &m = rt.memory();
    constexpr std::uint32_t kMpeg = 0x08CC268Cu, kData = 0x08A00000u, kRb = 0x08CC26ECu, kAu = 0x08B00000u;
    constexpr std::uint32_t kBufPtr = 0x08CC25ECu, kInit = 0x09FEF800u;
    rt.cpu().set_gpr(4, kMpeg); rt.cpu().set_gpr(5, kData); rt.cpu().set_gpr(6, 0x10000u); rt.cpu().set_gpr(7, kRb);
    rt.invoke_import("sceMpeg", 0xD8C5F121u, rt.cpu()); // sceMpegCreate
    CHECK(rt.cpu().gpr[2] == 0u);
    m.store32(kBufPtr, 0x09000000u);
    auto decode = [&](std::uint32_t au_size) {
        m.store32(kAu + 20u, au_size);
        m.store32(kInit, 0xDEADBEEFu);
        rt.cpu().set_gpr(4, kMpeg); rt.cpu().set_gpr(5, kAu); rt.cpu().set_gpr(6, kBufPtr); rt.cpu().set_gpr(7, kInit);
        rt.invoke_import("sceMpeg", 0xF0EB1125u, rt.cpu());
        return rt.cpu().gpr[2];
    };
    CHECK(decode(0x1234u) == 0u && m.load32(kInit) == 0u && m.load32(kAu + 20u) == 0u); // pipeline delay
    CHECK(decode(0x1234u) == 0u && m.load32(kInit) == 1u && m.load32(kAu + 20u) == 0u);
    CHECK(decode(0u) == 0u && m.load32(kInit) == 0u);                                   // empty AU
    CHECK(kernel.mpeg().find(kData)->video_decoded == 2u);
    CHECK(!rt.stopped());
    m.store32(kMpeg, 0x12345678u); // unknown handle: reported
    (void)decode(0x1234u);
    CHECK(rt.stopped());
    CHECK(rt.stop_reason().find("sceMpegAvcDecodeYCbCr(mpeg 0x08CC268C") != std::string::npos);
}

// sceMpegAvcCsc as Persona calls it (run 17, displayThread): range {0, 0,
// 480, 272} on the stack, frameWidth 512, dest = the second framebuffer.
void test_mpeg_avc_csc() {
    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    auto &m = rt.memory();
    constexpr std::uint32_t kMpeg = 0x08CC268Cu, kData = 0x08A00000u, kRb = 0x08CC26ECu, kRange = 0x09FAE500u;
    constexpr std::uint32_t kDest = 0x04088000u;
    rt.cpu().set_gpr(4, kMpeg); rt.cpu().set_gpr(5, kData); rt.cpu().set_gpr(6, 0x10000u); rt.cpu().set_gpr(7, kRb);
    rt.invoke_import("sceMpeg", 0xD8C5F121u, rt.cpu()); // sceMpegCreate
    CHECK(rt.cpu().gpr[2] == 0u);
    auto csc = [&](std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
        m.store32(kRange, x); m.store32(kRange + 4u, y); m.store32(kRange + 8u, w); m.store32(kRange + 12u, h);
        rt.cpu().set_gpr(4, kMpeg); rt.cpu().set_gpr(5, 0x09000000u); rt.cpu().set_gpr(6, kRange);
        rt.cpu().set_gpr(7, 512u); rt.cpu().set_gpr(8, kDest); rt.cpu().set_gpr(2, 0xDEADBEEFu);
        rt.invoke_import("sceMpeg", 0x31BD0272u, rt.cpu());
        return rt.cpu().gpr[2];
    };
    for (std::uint32_t y = 0; y < 272u; y += 271u)
        for (std::uint32_t x : {0u, 479u, 480u}) m.store32(kDest + (y * 512u + x) * 4u, 0x12345678u);
    CHECK(csc(0u, 0u, 480u, 272u) == 0u);
    CHECK(!rt.stopped());
    CHECK(m.load32(kDest) == 0xFF000000u && m.load32(kDest + 479u * 4u) == 0xFF000000u);
    CHECK(m.load32(kDest + (271u * 512u + 479u) * 4u) == 0xFF000000u);
    CHECK(m.load32(kDest + 480u * 4u) == 0x12345678u);                // outside the range: untouched
    CHECK(m.load32(kDest + (271u * 512u + 480u) * 4u) == 0x12345678u);
    (void)csc(0u, 0u, 480u, 270u); // not a multiple of 16: reported
    CHECK(rt.stopped());
    CHECK(rt.stop_reason().find("sceMpegAvcCsc(mpeg 0x08CC268C, source 0x09000000, range 0x09FAE500, frameWidth 512") != std::string::npos);
}

// sceKernelTerminateDeleteThread: pspautotests threads/threads/terminate
// (hardware), and Persona's call on its exited movie threads (run 18).
void test_terminate_delete_thread() {
    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    auto &tm = kernel.threads();
    const auto root = tm.init_root_thread("root", 0x08804000u, 0x09FFFF00u, 0u);
    auto call = [&](std::int32_t uid) {
        rt.cpu().set_gpr(4, static_cast<std::uint32_t>(uid));
        rt.invoke_import("ThreadManForUser", 0x383F7BCCu, rt.cpu());
        return rt.cpu().gpr[2];
    };
    const auto created = tm.create_thread("created", 0x08900000u, 0x20, 0x1000u, 0u, 0u, rt.memory());
    CHECK(call(created) == 0u);                          // Created: 00000000
    CHECK(tm.get_thread(created) == nullptr);
    CHECK(call(created) == 0x80020198u);                 // Deleted: 80020198
    const auto finished = tm.create_thread("readThread", 0x0881329Cu, 64, 0x800u, 0u, 0u, rt.memory());
    tm.get_thread(finished)->status = p3p3ds::hle::InternalThreadState::Stopped; // as after sceKernelExitThread
    CHECK(call(finished) == 0u);                         // Finished: 00000000
    CHECK(tm.get_thread(finished) == nullptr);
    CHECK(call(-1) == 0x80020198u && call(0x7FFFFFFF) == 0x80020198u); // -1, Invalid
    CHECK(call(0) == 0x80020197u && call(root) == 0x80020197u);         // Zero, Current
    CHECK(!rt.stopped());
    const auto ready = tm.create_thread("ready", 0x08900000u, 0x40, 0x1000u, 0u, 0u, rt.memory());
    tm.get_thread(ready)->status = p3p3ds::hle::InternalThreadState::Ready;
    (void)call(ready);                                   // live thread: reported
    CHECK(rt.stopped());
    CHECK(rt.stop_reason().find("sceKernelTerminateDeleteThread(uid " + std::to_string(ready)) != std::string::npos);
    CHECK(tm.get_thread(ready) != nullptr);
}

// sceUmdDeactivate: pspautotests umd/api and umd/callbacks (hardware), and
// Persona's call (run 19: mode 2, "disc0:").
void test_umd_deactivate() {
    using p3p3ds::hle::umd_deactivate_result;
    CHECK(umd_deactivate_result(2u, "disc0:") == 0u && umd_deactivate_result(1u, "disc0:") == 0u);
    CHECK(umd_deactivate_result(19u, "disc0:") == 0x80010016u);
    CHECK(umd_deactivate_result(0xFFFFFFFFu, "disc0:") == 0x80010016u);
    CHECK(umd_deactivate_result(2u, "") == 0x80010016u && umd_deactivate_result(1u, "umd0:") == 0x80010016u);

    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    kernel.threads().init_root_thread("root", 0x08804000u, 0x09FFFF00u, 0u);
    auto &m = rt.memory();
    constexpr std::uint32_t kDrive = 0x08A65314u;
    const char name[] = "disc0:";
    for (std::uint32_t i = 0; i < sizeof name; ++i) m.store8(kDrive + i, static_cast<std::uint8_t>(name[i]));
    const auto cb = kernel.threads().create_callback("umdHandler", 0x08900000u, 0x1234u);
    CHECK(kernel.umd().register_callback(cb, kernel.threads()) == 0u);
    auto deactivate = [&](std::uint32_t mode, std::uint32_t drive) {
        rt.cpu().set_gpr(4, mode); rt.cpu().set_gpr(5, drive);
        rt.invoke_import("sceUmdUser", 0xE83742BAu, rt.cpu());
        return rt.cpu().gpr[2];
    };
    CHECK(deactivate(2u, kDrive) == 0u);
    CHECK(kernel.threads().get_callback(cb)->notify_count == 1); // "umdHandler called" after sceUmdDeactivate
    CHECK(deactivate(2u, 0u) == 0x80010016u);                     // "Mode 2, NULL"
    CHECK(deactivate(19u, kDrive) == 0x80010016u);                // "Mode 19"
    CHECK(kernel.threads().get_callback(cb)->notify_count == 1);
    CHECK(!rt.stopped());
}

// sceUmdUnRegisterUMDCallBack: pspautotests umd/api and umd/callbacks
// (hardware), as Persona uses it after sceUmdDeactivate.
void test_umd_unregister_callback() {
    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    kernel.threads().init_root_thread("root", 0x08804000u, 0x09FFFF00u, 0u);
    auto &m = rt.memory();
    auto unregister = [&](std::int32_t uid) {
        rt.cpu().set_gpr(4, static_cast<std::uint32_t>(uid));
        rt.invoke_import("sceUmdUser", 0xBD2BDE07u, rt.cpu());
        return rt.cpu().gpr[2];
    };
    const auto sema = kernel.threads().create_sema("sema", 0u, 0, 1);
    const auto cb = kernel.threads().create_callback("umdHandler", 0x08900000u, 0x1234u);
    CHECK(unregister(cb) == 0x80010016u);                 // nothing registered yet
    CHECK(kernel.umd().register_callback(cb, kernel.threads()) == 0u);
    CHECK(unregister(sema) == 0x80010016u);               // "Unregister semaphore: 80010016"
    CHECK(unregister(cb) == static_cast<std::uint32_t>(cb)); // "Unregister callback": result == cb
    CHECK(kernel.umd().registered_callback() == 0);
    CHECK(static_cast<std::int32_t>(unregister(cb)) < 0); // umd.expected: the second one fails
    // sceUmdDeactivate no longer notifies the unregistered callback.
    constexpr std::uint32_t kDrive = 0x08A65314u;
    const char name[] = "disc0:";
    for (std::uint32_t i = 0; i < sizeof name; ++i) m.store8(kDrive + i, static_cast<std::uint8_t>(name[i]));
    rt.cpu().set_gpr(4, 2u); rt.cpu().set_gpr(5, kDrive);
    rt.invoke_import("sceUmdUser", 0xE83742BAu, rt.cpu());
    CHECK(rt.cpu().gpr[2] == 0u);
    CHECK(kernel.threads().get_callback(cb)->notify_count == 0);
    CHECK(!rt.stopped());
}

// sceMpegAvcDecodeStopYCbCr on a created instance: *status = 0, result 0.
void test_mpeg_decode_stop_ycbcr() {
    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    auto &m = rt.memory();
    constexpr std::uint32_t kMpeg = 0x08CC268Cu, kData = 0x08A00000u, kRb = 0x08CC26ECu, kStatus = 0x09FEF800u;
    rt.cpu().set_gpr(4, kMpeg); rt.cpu().set_gpr(5, kData); rt.cpu().set_gpr(6, 0x10000u); rt.cpu().set_gpr(7, kRb);
    rt.invoke_import("sceMpeg", 0xD8C5F121u, rt.cpu()); // sceMpegCreate
    m.store32(kStatus, 0xDEADBEEFu);
    rt.cpu().set_gpr(4, kMpeg); rt.cpu().set_gpr(5, 0x08CC25ECu); rt.cpu().set_gpr(6, kStatus);
    rt.invoke_import("sceMpeg", 0xF2930C9Cu, rt.cpu());
    CHECK(rt.cpu().gpr[2] == 0u && m.load32(kStatus) == 0u && !rt.stopped());
    rt.cpu().set_gpr(4, 0x08B00000u); // no handle stored there: reported
    rt.invoke_import("sceMpeg", 0xF2930C9Cu, rt.cpu());
    CHECK(rt.stopped());
}

// sceDisplayIsVblank: isstate.expected / vblanklen.expected timing.
void test_display_is_vblank() {
    using p3p3ds::hle::display_is_vblank;
    constexpr std::uint64_t kPeriod = p3p3ds::hle::ThreadManager::kVblankPeriodUs;
    CHECK(display_is_vblank(0u, kPeriod) == 1u);                 // "After start: vblank=1"
    CHECK(display_is_vblank(5u * kPeriod + 631u, kPeriod) == 1u); // during vsync (631.5 us)
    CHECK(display_is_vblank(5u * kPeriod + 731u, kPeriod) == 1u);
    CHECK(display_is_vblank(5u * kPeriod + 740u, kPeriod) == 0u); // vblanklen: 730-740 us
    CHECK(display_is_vblank(5u * kPeriod + 1000u, kPeriod) == 0u); // "After vblank end: vblank=0"
    CHECK(display_is_vblank(6u * kPeriod - 1u, kPeriod) == 0u);

    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    rt.invoke_import("sceDisplay", 0x4D4E10ECu, rt.cpu());
    CHECK(rt.cpu().gpr[2] == 1u && !rt.stopped()); // clock at 0: vblank start
}

// UMD drive-stat waits: pspautotests umd/api and umd/wait (hardware), and
// the fork-owned activation state after sceUmdDeactivate.
void test_umd_wait_drive_stat() {
    using p3p3ds::hle::umd_wait_result;
    for (std::uint32_t bad : {0u, 0x04u, 0x40u, 0x80u, 0xFFFFFFC4u}) CHECK(umd_wait_result(bad, 0x32u) == 0x80010016u);
    CHECK(umd_wait_result(0x20u, 0x32u) == 0u);           // readable after activate
    CHECK(umd_wait_result(0x02u, 0x32u) == 0u);           // "sceUmdWaitDriveStat(0x02): 0"
    CHECK(umd_wait_result(0xFFu, 0x12u) == 0u);           // 0xFF after a deactivate: 0
    CHECK(!umd_wait_result(0x20u, 0x12u).has_value());    // READABLE after a deactivate: waits

    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    auto &tm = kernel.threads();
    tm.init_root_thread("root", 0x08804000u, 0x09FFFF00u, 0u);
    kernel.umd().set_medium_present(true);
    auto &m = rt.memory();
    constexpr std::uint32_t kDrive = 0x08A65314u;
    const char name[] = "disc0:";
    for (std::uint32_t i = 0; i < sizeof name; ++i) m.store8(kDrive + i, static_cast<std::uint8_t>(name[i]));
    auto call = [&](std::uint32_t nid, std::uint32_t a0, std::uint32_t a1) {
        rt.cpu().set_gpr(4, a0); rt.cpu().set_gpr(5, a1); rt.cpu().set_gpr(2, 0xDEADBEEFu);
        rt.invoke_import("sceUmdUser", nid, rt.cpu());
        return rt.cpu().gpr[2];
    };
    CHECK(call(0xC6183D47u, 1u, kDrive) == 0u);           // sceUmdActivate (upstream)
    CHECK(call(0x6B4A146Cu, 0u, 0u) == 0x32u);            // sceUmdGetDriveStat
    CHECK(call(0x4A9E5E29u, 0x20u, 10000u) == 0u);
    CHECK(call(0xE83742BAu, 1u, kDrive) == 0u);           // sceUmdDeactivate
    CHECK(call(0x6B4A146Cu, 0u, 0u) == 0x12u);            // no longer readable
    CHECK(kernel.umd().medium_present());
    CHECK(call(0x4A9E5E29u, 0xFFu, 10000u) == 0u);
    CHECK(call(0x4A9E5E29u, 0x04u, 10000u) == 0x80010016u);
    CHECK(call(0x6AF9B50Au, 0u, 0u) == 0u);               // "No waiters: 00000000"
    // READABLE after a deactivate waits: alone, the thread times out
    // ("sceUmdWaitDriveStatCB: 800201A8") after the full timeout.
    const auto before = tm.now();
    CHECK(call(0x4A9E5E29u, 0x20u, 10000u) == 0x800201A8u);
    CHECK(tm.now() - before >= 10000u);
    // Timeout 0 waits until sceUmdCancelWaitDriveStat from another thread (800201A9).
    const auto root = tm.current_thread_id();
    const auto canceler = tm.create_thread("canceler", 0x08900000u, 0x20, 0x1000u, 0u, 0u, m);
    CHECK(tm.start_thread(canceler, 0u, 0u, m, rt.cpu(), false) == 0);
    (void)call(0x4A9E5E29u, 0x20u, 0u);
    CHECK(tm.get_thread(root)->status == p3p3ds::hle::InternalThreadState::Waiting);
    CHECK(tm.get_thread(root)->wait.type == p3p3ds::hle::WaitType::Umd);
    CHECK(tm.current_thread_id() == canceler);
    CHECK(call(0x6AF9B50Au, 0u, 0u) == 0u);
    CHECK(tm.get_thread(root)->status != p3p3ds::hle::InternalThreadState::Waiting);
    CHECK(tm.get_thread(root)->context.gpr[2] == 0x800201A9u);
    CHECK(!rt.stopped());
    CHECK(call(0xC6183D47u, 1u, kDrive) == 0u);           // activate again
    CHECK(call(0x6B4A146Cu, 0u, 0u) == 0x32u);
}

// Module manager group on pspautotests modules/startoptions/child.prx (an
// unencrypted PRX with module_start and module_stop), loaded by upstream's
// sceKernelLoadModule. Skipped without the reference clone.
void test_module_stop_unload() {
    const std::filesystem::path dir = std::filesystem::path(P3P_SOURCE_DIR) / "references/pspautotests/tests/modules/startoptions";
    if (!std::filesystem::exists(dir / "child.prx")) { std::cout << "module test skipped (no pspautotests clone)\n"; return; }
    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    auto &tm = kernel.threads();
    tm.init_root_thread("root", 0x08804000u, 0x09FFFF00u, 0u);
    kernel.io().mount("host0:", std::make_shared<p3p3ds::vfs::HostFileSystem>(dir));
    auto &m = rt.memory();
    auto call = [&](const char *lib, std::uint32_t nid, std::uint32_t a0, std::uint32_t a1 = 0, std::uint32_t a2 = 0,
                    std::uint32_t a3 = 0, std::uint32_t t0 = 0, std::uint32_t ra = 0x08804200u) {
        auto &c = rt.cpu();
        c.gpr[4] = a0; c.gpr[5] = a1; c.gpr[6] = a2; c.gpr[7] = a3; c.gpr[8] = t0; c.gpr[31] = ra; c.gpr[2] = 0xDEADBEEFu;
        rt.invoke_import(lib, nid, c);
        return c.gpr[2];
    };
    constexpr std::uint32_t kPath = 0x08900000u, kStatus = 0x08900100u;
    const char path[] = "host0:/child.prx";
    for (std::uint32_t i = 0; i < sizeof path; ++i) m.store8(kPath + i, static_cast<std::uint8_t>(path[i]));
    const auto uid = static_cast<std::int32_t>(call("ModuleMgrForUser", 0x977DE386u, kPath)); // sceKernelLoadModule
    CHECK(uid > 0);
    auto &module = kernel.modules().modules.at(uid);
    CHECK(call("ModuleMgrForUser", 0xF0A26395u, 0u, 0u, 0u, 0u, 0u, module.base + 0x10u) == static_cast<std::uint32_t>(uid));
    CHECK(call("ModuleMgrForUser", 0xF0A26395u, 0u) == 1u); // the main module, as sceKernelGetModuleIdByAddress
    CHECK(call("ModuleMgrForUser", 0xD1FF982Au, 0x7777u, 0u, 0u, kStatus) == 0x8002012Eu);
    module.started = true; // module_start ran (upstream sceKernelStartModule; needs the dispatch loop)
    const auto root = tm.current_thread_id();
    (void)call("ModuleMgrForUser", 0xD1FF982Au, static_cast<std::uint32_t>(uid), 0u, 0u, kStatus);
    CHECK(tm.get_thread(root)->status == p3p3ds::hle::InternalThreadState::Waiting);
    std::int32_t stopper = 0;
    for (std::int32_t id = 1; id < 64; ++id)
        if (const auto *t = tm.get_thread(id); t != nullptr && t->name == "SceModmgrStop") stopper = id;
    CHECK(stopper != 0);
    if (stopper == 0) return;
    const auto *t = tm.get_thread(stopper);
    CHECK(t->current_priority == 0x20 && t->stack_size >= 0x40000u); // startoptions.expected: stop thread
    CHECK(t->entry_pc >= module.base && t->entry_pc < module.base + module.size);
    const std::uint32_t text_word = m.load32(module.base);
    CHECK(text_word != 0x0000004Du);
    tm.get_thread(stopper)->status = p3p3ds::hle::InternalThreadState::Stopped; // module_stop returned 5
    tm.get_thread(stopper)->exit_status = 5;
    tm.get_thread(root)->status = p3p3ds::hle::InternalThreadState::Running;
    (void)tm.switch_to(root, rt.cpu());
    CHECK(call("ModuleMgrForUser", 0xD1FF982Au, static_cast<std::uint32_t>(uid), 0u, 0u, kStatus) == 0u); // retried call
    CHECK(m.load32(kStatus) == 5u);
    CHECK(tm.get_thread(stopper) == nullptr);
    CHECK(m.load32(module.base) == text_word); // unloadfill.expected: stop leaves memory unchanged
    const auto base = module.base, size = module.size;
    CHECK(call("ModuleMgrForUser", 0x2E0911AAu, static_cast<std::uint32_t>(uid)) == static_cast<std::uint32_t>(uid));
    CHECK(m.load32(base) == 0x0000004Du);         // text: break
    CHECK(m.load32(base + size - 4u) == 0xFFFFFFFFu); // bss
    CHECK(kernel.modules().modules.count(uid) == 0u);
    CHECK(call("ModuleMgrForUser", 0x2E0911AAu, static_cast<std::uint32_t>(uid)) == 0x8002012Eu);
    CHECK(!rt.stopped());
    (void)call("ModuleMgrForUser", 0x8F2DF740u, 0u); // from the main module: game exit
    CHECK(rt.stopped() && rt.stop_reason().find("Game exit") != std::string::npos);
}

// sceKernelExitGame stops the run as a game exit.
void test_exit_game() {
    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    rt.invoke_import("LoadExecForUser", 0x05572A5Fu, rt.cpu());
    CHECK(rt.stopped() && rt.stop_reason() == "Game exit: sceKernelExitGame");
}

// sceIoRename: pspautotests io/file/rename.expected error cases.
void test_io_rename() {
    using p3p3ds::hle::io_rename_static_result;
    CHECK(io_rename_static_result("ms0:/__iorename_test*.txt", "ms0:/a.txt") == 0x80010016u); // wild -> rel
    CHECK(io_rename_static_result("ms0:/a.txt", "ms0:/__iorename_test?.txt") == 0x80010016u);
    CHECK(io_rename_static_result("ms0:/a.txt", "host0:/a.txt") == 0x80020322u);            // cross device
    CHECK(!io_rename_static_result("ms0:/a.txt", "ms0:/b.txt").has_value());

    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    const auto dir = std::filesystem::path(P3P_SOURCE_DIR) / "tests";
    kernel.io().mount("ms0:", std::make_shared<p3p3ds::vfs::HostFileSystem>(dir));
    auto &m = rt.memory();
    auto rename = [&](const char *from, const char *to) {
        for (std::uint32_t i = 0;; ++i) { m.store8(0x08900000u + i, static_cast<std::uint8_t>(from[i])); if (!from[i]) break; }
        for (std::uint32_t i = 0;; ++i) { m.store8(0x08900200u + i, static_cast<std::uint8_t>(to[i])); if (!to[i]) break; }
        rt.cpu().set_gpr(4, 0x08900000u); rt.cpu().set_gpr(5, 0x08900200u);
        rt.invoke_import("IoFileMgrForUser", 0x779103A0u, rt.cpu());
        return rt.cpu().gpr[2];
    };
    CHECK(rename("ms0:/_DOES_NOT_EXIST/x.txt", "ms0:/y.txt") == 0x80010002u); // bad src path
    CHECK(rename("ms0:/__iorename_test3.txt", "ms0:/a.txt") == 0x80010002u);  // bad src file
    CHECK(rename("ms0:/a.txt", "host0:/a.txt") == 0x80020322u);
    CHECK(!rt.stopped());
    (void)rename("ms0:/test_fork_kernel.cpp", "ms0:/renamed.cpp"); // existing file: not modelled
    CHECK(rt.stopped());
    CHECK(std::filesystem::exists(dir / "test_fork_kernel.cpp"));
}

// MsgDialog: pspautotests utility/msgdialog/dialog.expected status sequence,
// and the default button the dialog confirms without a UI.
void test_msgdialog() {
    using p3p3ds::hle::msgdialog_default_button;
    CHECK(msgdialog_default_button(0x01u) == 1u);          // OK dialog
    CHECK(msgdialog_default_button(0x11u) == 1u);          // yes/no, default yes
    CHECK(msgdialog_default_button(0x111u) == 2u);         // yes/no, default no
    CHECK(msgdialog_default_button(0x100u) == 1u);         // default-no without yes/no buttons

    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    auto &m = rt.memory();
    constexpr std::uint32_t kParams = 0x08A00000u;
    auto call = [&](std::uint32_t nid, std::uint32_t a0 = 0u) {
        rt.cpu().set_gpr(4, a0); rt.cpu().set_gpr(2, 0xDEADBEEFu);
        rt.invoke_import("sceUtility", nid, rt.cpu());
        return rt.cpu().gpr[2];
    };
    for (std::uint32_t i = 0; i < 708u; i += 4u) m.store32(kParams + i, 0u);
    m.store32(kParams, 580u);           // V2
    m.store32(kParams + 48u, 0x1337u);  // dialog->result in the test
    m.store32(kParams + 52u, 1u);       // text mode
    const char text[] = "Save data is corrupted.";
    for (std::uint32_t i = 0; i < sizeof text; ++i) m.store8(kParams + 60u + i, static_cast<std::uint8_t>(text[i]));
    m.store32(kParams + 572u, 0x111u);  // yes/no, default no
    CHECK(call(0x9A1C91D7u) == 0u);     // nothing running
    CHECK(call(0x67AF3428u) == 0x80110001u);
    CHECK(call(0x2AD8E239u, kParams) == 0u);        // "InitStart: 00000000"
    CHECK(call(0x2AD8E239u, kParams) == 0x80110001u);
    CHECK(call(0x9A1C91D7u) == 1u);                 // "GetStatus: 00000001"
    CHECK(m.load32(kParams + 48u) == 0x1337u);
    CHECK(call(0x9A1C91D7u) == 2u);                 // VISIBLE
    CHECK(m.load32(kParams + 48u) == 0u);           // "Result -> 00000000"
    (void)call(0x95FC253Bu, 1u);                    // Update: confirms the default
    CHECK(m.load32(kParams + 576u) == 2u);
    CHECK(call(0x9A1C91D7u) == 3u);                 // QUIT
    CHECK(call(0x67AF3428u) == 0u);                 // "ShutdownStart: 00000000"
    CHECK(call(0x9A1C91D7u) == 4u);                 // FINISHED once
    CHECK(call(0x9A1C91D7u) == 0u);
    CHECK(!rt.stopped());
    // A dialog polled while VISIBLE for 600 vblanks without Update stops the run.
    CHECK(call(0x2AD8E239u, kParams) == 0u);
    CHECK(call(0x9A1C91D7u) == 1u);
    CHECK(call(0x9A1C91D7u) == 2u);
    for (int k = 0; k < 10200000 && !rt.stopped(); ++k) {
        if (k % 1000 == 0) (void)call(0x9A1C91D7u);
        kernel.threads().on_syscall_return(rt, rt.cpu());
    }
    CHECK(rt.stopped());
    CHECK(rt.stop_reason().find("sceUtilityMsgDialog stays VISIBLE") != std::string::npos);
}

// sceKernelPollEventFlag under its real NID (0x30FD48F0): pspautotests
// threads/events/poll.expected, and Persona's thread BGM call (run 20:
// bits 1, mode 0x21 = OR | CLEAR (PSPSDK pspthreadman.h), no out pointer).
void test_poll_event_flag_nid() {
    psprecomp::Runtime rt;
    p3p3ds::KernelState kernel;
    p3p3ds::hle::register_all_hle_modules(rt, kernel);
    auto &m = rt.memory();
    auto poll = [&](std::int32_t id, std::uint32_t bits, std::uint32_t mode, std::uint32_t out) {
        rt.cpu().set_gpr(4, static_cast<std::uint32_t>(id)); rt.cpu().set_gpr(5, bits);
        rt.cpu().set_gpr(6, mode); rt.cpu().set_gpr(7, out);
        rt.invoke_import("ThreadManForUser", 0x30FD48F0u, rt.cpu());
        return rt.cpu().gpr[2];
    };
    const auto flag = kernel.threads().create_event_flag("poll1", 0u, 0xFFFFFFFFu);
    constexpr std::uint32_t kOut = 0x08900000u;
    m.store32(kOut, 0xDEADBEEFu);
    CHECK(poll(flag, 0u, 0x20u, kOut) == 0x800201B1u && m.load32(kOut) == 0xDEADBEEFu); // "Clear 0x00000000: Failed (800201B1)"
    CHECK(poll(flag, 1u, 0x21u, 0u) == 0u);                                           // Persona's call, no out bits
    CHECK(kernel.threads().get_event_flag(flag)->bits == 0xFFFFFFFEu);                // "Clear/Or 0x00000001": cur=FFFFFFFE
    CHECK(poll(flag, 1u, 0x21u, 0u) == 0x800201AFu);                                  // bit 0 now clear: EVF_COND
    CHECK(poll(0, 1u, 0x01u, 0u) == 0x8002019Au);                                     // unknown id
    CHECK(!rt.stopped());
    // sceKernelTotalFreeMemSize under its real NID returns upstream's value.
    rt.invoke_import("SysMemUserForUser", 0xF919F628u, rt.cpu());
    CHECK(rt.cpu().gpr[2] == kernel.sysmem().total_free_memory() && !rt.stopped());
}
} // namespace

int main() {
    // uOFW src/kd/sysmem/misc.c: accepted when (ver & 0xFFFF0000) is 0x05000000 or 0x05050000.
    for (const std::uint32_t version : {0x05000000u, 0x05000010u, 0x0500FFFFu, 0x05050000u, 0x05050010u}) {
        p3p3ds::KernelState kernel;
        CHECK(p3p3ds::hle::sceKernelSetCompiledSdkVersion500_550(kernel, version));
        CHECK(kernel.has_compiled_sdk_version());
        CHECK(kernel.compiled_sdk_version() == version);
        CHECK((kernel.system_flags() & 0x1000u) != 0u);

        Call call;
        CHECK(call(version) == 0u);
        CHECK(!call.rt.stopped());
        CHECK(call.kernel.compiled_sdk_version() == version);
    }
    // Anything else: the PSP resets; nothing is recorded and the run stops.
    for (const std::uint32_t version : {0u, 0x04020000u, 0x05010000u, 0x05070000u, 0x06020010u, 0x15000000u}) {
        p3p3ds::KernelState kernel;
        CHECK(!p3p3ds::hle::sceKernelSetCompiledSdkVersion500_550(kernel, version));
        CHECK(!kernel.has_compiled_sdk_version());
        CHECK((kernel.system_flags() & 0x1000u) == 0u);

        Call call;
        (void)call(version);
        CHECK(call.rt.stopped());
        CHECK(call.rt.stop_reason().find("sceKernelSetCompiledSdkVersion500_550") != std::string::npos);
        CHECK(!call.kernel.has_compiled_sdk_version());
    }
    test_power_register_callback();
    test_ctrl_sampling_cycle();
    test_rtc_local_time();
    test_rtc_get_tick();
    test_devctl_reports_call();
    test_devctl_ms_free_space();
    test_devctl_fat_ready();
    test_subintr_results();
    test_subintr_dispatch();
    test_mpeg_ycbcr_size();
    test_mpeg_ycbcr_init();
    test_refer_event_flag();
    test_mpeg_decode_ycbcr();
    test_mpeg_avc_csc();
    test_terminate_delete_thread();
    test_umd_deactivate();
    test_umd_unregister_callback();
    test_mpeg_decode_stop_ycbcr();
    test_display_is_vblank();
    test_umd_wait_drive_stat();
    test_module_stop_unload();
    test_exit_game();
    test_io_rename();
    test_msgdialog();
    test_poll_event_flag_nid();
    std::cout << "fork kernel checks failures=" << failures << "\n";
    return failures ? 1 : 0;
}
