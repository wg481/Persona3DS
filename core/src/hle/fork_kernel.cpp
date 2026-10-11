// See fork_kernel.hpp.
#include "p3p3ds/hle/fork_kernel.hpp"

#include "p3p3ds/kernel_state.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <memory>

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

std::int32_t PowerCallbackSlots::register_callback(std::int32_t slot, std::int32_t cbid, bool cbid_valid,
                                                  std::int32_t *notify) noexcept {
    // [INFERRED] check order: slot range first, then the callback (the
    // hardware test only passes an invalid callback with a valid slot).
    if (slot < -1 || slot >= 32) return static_cast<std::int32_t>(0x80000102u);
    if (slot >= kSlots) return static_cast<std::int32_t>(0x80000023u);
    if (!cbid_valid) return static_cast<std::int32_t>(0x80000100u);
    std::int32_t result = 0;
    if (slot == -1) {
        slot = 0;
        while (slot < kSlots && slots_[slot] != 0) ++slot;
        if (slot == kSlots) return static_cast<std::int32_t>(0x80000022u);
        result = slot;
    } else if (slots_[slot] != 0) {
        return static_cast<std::int32_t>(0x80000020u);
    }
    slots_[slot] = cbid;
    *notify = cbid;
    return result;
}

void register_fork_kernel_imports(psprecomp::Runtime &runtime, KernelState &kernel) {
    // scePower::0x04B7766E - scePowerRegisterCallback
    auto power = std::make_shared<PowerCallbackSlots>();
    runtime.register_hle("scePower", 0x04B7766Eu,
        [&kernel, power](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto slot = static_cast<std::int32_t>(ctx.gpr[4]); // $a0
            const auto cbid = static_cast<std::int32_t>(ctx.gpr[5]); // $a1
            std::int32_t notify = 0;
            const auto result = power->register_callback(slot, cbid, kernel.threads().get_callback(cbid) != nullptr, &notify);
            if (notify != 0) (void)kernel.threads().notify_callback(rt, notify, PowerCallbackSlots::kPowerInfo);
            ctx.set_gpr(2, static_cast<std::uint32_t>(result));
        });

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
