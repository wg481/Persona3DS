// Kernel imports added by the Persona3DS fork (core/src/hle/fork_kernel.cpp),
// checked against uOFW's semantics and dispatched through the runtime as the
// game reaches them.
#include "p3p3ds/hle/fork_kernel.hpp"
#include "p3p3ds/hle/hle_modules.hpp"
#include "p3p3ds/kernel_state.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <iostream>

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
    std::cout << "fork kernel checks failures=" << failures << "\n";
    return failures ? 1 : 0;
}
