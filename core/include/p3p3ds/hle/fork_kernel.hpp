#pragma once
// Kernel imports added by the Persona3DS fork for games other than P3P.
// Kept in their own files (plus one registration call in hle_modules.cpp) so
// that upstream P3P3DS changes keep merging; drop an entry here if upstream
// implements the same NID.
#include <cstdint>

namespace psprecomp {
class Runtime;
}

namespace p3p3ds {
class KernelState;
}

namespace p3p3ds::hle {

// SysMemUserForUser::0x91DE343C
// int sceKernelSetCompiledSdkVersion500_550(int sdkVersion);
// uOFW src/kd/sysmem/misc.c (sceKernelSetCompiledSdkVersion500_550): accepts
// a version whose upper 16 bits are 0x0500 or 0x0505, sets the "SDK version
// set" game-info flag (0x1000) and returns 0. Any other version resets the
// hardware and never returns. Here: the accepted case records the version
// (as the 600_620 handler does) and returns true; otherwise nothing is
// recorded and false is returned so the caller can stop the run.
[[nodiscard]] bool sceKernelSetCompiledSdkVersion500_550(KernelState &kernel, std::uint32_t version) noexcept;

// scePower::0x04B7766E
// int scePowerRegisterCallback(int slot, SceUID cbid);
// pspautotests tests/power/power.c + power.expected (hardware): slot -1 takes
// the first free of 16 slots and returns it (all used: 0x80000022); slot
// 0..15 returns 0 (occupied: 0x80000020); slot 16..31 0x80000023; any other
// slot 0x80000102; an unknown callback 0x80000100. Registering notifies the
// callback once (registering one callback in two slots notifies it twice,
// delivered as one call with count 2); the hardware shows bit 0x80
// (PSP_POWER_CB_BATTERY_EXIST) set in the power info argument.
class PowerCallbackSlots {
public:
    static constexpr int kSlots = 16;
    // Power info delivered on registration. [INFERRED] AC power + battery
    // present + 100 %, flags from PSPSDK psppower.h; the hardware test only
    // shows bit 0x80 (it masks the rest).
    static constexpr std::uint32_t kPowerInfo = 0x00001000u | 0x00000080u | 100u;
    // Returns the guest result; on success *notify is the callback to notify.
    std::int32_t register_callback(std::int32_t slot, std::int32_t cbid, bool cbid_valid, std::int32_t *notify) noexcept;

private:
    std::int32_t slots_[kSlots]{}; // callback UID, 0 = free
};

// sceCtrl::0x6A2774F3
// int sceCtrlSetSamplingCycle(unsigned int cycle);
// uOFW src/kd/ctrl/ctrl.c (sceCtrlSetSamplingCycle): 0 = sample at every
// vblank, 5555..20000 = sample every `cycle` microseconds from a timer; both
// return the previous cycle. Any other value returns 0x800001FE
// (SCE_ERROR_INVALID_VALUE) and changes nothing. pspautotests
// ctrl/vblank.expected: sceCtrlSetSamplingCycle(0) on a fresh boot returns 0.
// The runtime samples once per vblank only (ctrl.cpp), so a timer cycle is
// reported as unsupported instead of being accepted silently.
enum class SamplingCycleResult { Ok, Invalid, TimerNotModelled };
class CtrlSamplingCycle {
public:
    static constexpr std::uint32_t kInvalidValue = 0x800001FEu;
    SamplingCycleResult set(std::uint32_t cycle, std::uint32_t *previous) noexcept;
    [[nodiscard]] std::uint32_t cycle() const noexcept { return cycle_; }

private:
    std::uint32_t cycle_{}; // 0: vblank sampling (boot state)
};

void register_fork_kernel_imports(psprecomp::Runtime &runtime, KernelState &kernel);

} // namespace p3p3ds::hle
