// See fork_kernel.hpp.
#include "p3p3ds/hle/fork_kernel.hpp"

#include "p3p3ds/kernel_state.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <memory>
#include <string>

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

SamplingCycleResult CtrlSamplingCycle::set(std::uint32_t cycle, std::uint32_t *previous) noexcept {
    if (cycle != 0u && (cycle < 5555u || cycle > 20000u)) return SamplingCycleResult::Invalid;
    if (cycle != 0u) return SamplingCycleResult::TimerNotModelled;
    *previous = cycle_;
    cycle_ = cycle;
    return SamplingCycleResult::Ok;
}

void register_fork_kernel_imports(psprecomp::Runtime &runtime, KernelState &kernel) {
    // sceCtrl::0x6A2774F3 - sceCtrlSetSamplingCycle
    auto sampling = std::make_shared<CtrlSamplingCycle>();
    runtime.register_hle("sceCtrl", 0x6A2774F3u, [sampling](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        const std::uint32_t cycle = ctx.gpr[4]; // $a0
        std::uint32_t previous = 0;
        switch (sampling->set(cycle, &previous)) {
        case SamplingCycleResult::Ok: ctx.set_gpr(2, previous); break;
        case SamplingCycleResult::Invalid: ctx.set_gpr(2, CtrlSamplingCycle::kInvalidValue); break;
        case SamplingCycleResult::TimerNotModelled:
            rt.stop("sceCtrlSetSamplingCycle: timer sampling every " + std::to_string(cycle) +
                    " us is not modelled (input is sampled per vblank)");
            break;
        }
    });


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
