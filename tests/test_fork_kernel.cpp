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
    std::cout << "fork kernel checks failures=" << failures << "\n";
    return failures ? 1 : 0;
}
