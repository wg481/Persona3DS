// See fork_kernel.hpp.
#include "p3p3ds/hle/fork_kernel.hpp"

#include "p3p3ds/kernel_state.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

namespace p3p3ds::hle {

bool sceKernelSetCompiledSdkVersion500_550(KernelState &kernel, std::uint32_t version) noexcept {
    switch (version & 0xFFFF0000u) {
    case 0x05000000u:
    case 0x05050000u:
        kernel.set_compiled_sdk_version(version);
        return true;
    default:
        return false;
    }
}

void register_fork_kernel_imports(psprecomp::Runtime &runtime, KernelState &kernel) {
    // SysMemUserForUser::0x91DE343C - sceKernelSetCompiledSdkVersion500_550
    runtime.register_hle("SysMemUserForUser", 0x91DE343Cu,
        [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t version = ctx.gpr[4]; // $a0
            if (!sceKernelSetCompiledSdkVersion500_550(kernel, version)) {
                // Real firmware resets the console here (uOFW misc.c).
                rt.stop("sceKernelSetCompiledSdkVersion500_550: version " + psprecomp::hex32(version) +
                        " is not 5.00/5.50 (the PSP would reset)");
                return;
            }
            ctx.set_gpr(2, 0u); // $v0 = 0 (SCE_KERNEL_ERROR_OK)
        });
}

} // namespace p3p3ds::hle
