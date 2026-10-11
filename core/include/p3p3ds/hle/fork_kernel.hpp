#pragma once
// Kernel imports added by the Persona3DS fork for games other than P3P.
// Kept in their own files (plus one registration call in hle_modules.cpp) so
// that upstream P3P3DS changes keep merging; drop an entry here if upstream
// implements the same NID.
#include <cstdint>
#include <string>

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

// sceRtc::0xE7C27D1B
// int sceRtcGetCurrentClockLocalTime(ScePspDateTime *time);
// uOFW src/kd/rtc/rtc.c: a kernel-space pointer from user mode returns
// 0x80000023 (SCE_ERROR_PRIV_REQUIRED); otherwise the current tick plus the
// system time-zone offset is written as ScePspDateTime (PSPSDK psptypes.h:
// u16 year, month, day, hour, minute, second; u32 microsecond = 16 bytes)
// and 0 is returned. pspautotests rtc/rtc.expected: "Normal: 00000000" and a
// valid date and time. The clock is the virtual clock on the same fixed
// epoch as sceKernelLibcTime (core/src/hle/utils.cpp: 2008-01-01 00:00 UTC,
// [INFERRED] for determinism) with time-zone offset 0 [INFERRED].
struct PspDateTime {
    std::uint16_t year, month, day, hour, minute, second;
    std::uint32_t microsecond;
};
inline constexpr std::uint64_t kForkRtcEpochSeconds = 1199145600u; // = utils.cpp kEpochSeconds
[[nodiscard]] PspDateTime psp_date_time(std::uint64_t unix_microseconds) noexcept;

// sceRtc::0x6FF40ACC
// int sceRtcGetTick(const ScePspDateTime *date, u64 *tick);
// tick = microseconds since 0001-01-01 00:00 (proleptic Gregorian).
// pspautotests rtc/convert.expected (hardware): 0010-01-01 -> 283996800000000,
// 9998-01-01 -> 315474825600000000, 9999-01-01 -> 315506361600000000, years 0
// and 10000 -> 0x800001FE with *tick unchanged; convert.c: 1970-01-01 is
// 62135596800000000. Validity follows sceRtcCheckValid (rtc/lookup.expected:
// year, month, day, hour, minute, second, microsecond each checked);
// [INFERRED] any invalid field gives the same 0x800001FE. uOFW rtc.c: a
// kernel-space tick pointer from user mode returns 0x80000023.
[[nodiscard]] bool psp_date_time_valid(const PspDateTime &t) noexcept;
[[nodiscard]] std::uint64_t psp_tick(const PspDateTime &t) noexcept; // valid dates only

// IoFileMgrForUser::0x54F5FB11
// int sceIoDevctl(const char *dev, unsigned int cmd, void *indata, int inlen,
//                 void *outdata, int outlen);   (uOFW src/kd/iofilemgr/iofilemgr.c)
// Device-specific: what a command does depends on the device driver. Only
// commands observed in Persona are modelled; any other call stops the run and
// names the device, command and buffers (AGENTS.md section 6: the observed
// call first, then its semantics).
//
// ms0: / fatms0: 0x02425818 (PSPSDK pspiofilemgr_devctl.h SCE_PR_GETDEV):
// indata holds a pointer to SceDevInf {maxClusters, freeClusters,
// maxSectors, sectorSize, sectorCount} (5 x u32), which the driver fills;
// returns 0. pspautotests mstick/mstick.expected (hardware): sectorSize 512,
// sectorCount 64; cluster counts between 256 MB and 128 GB worth. Persona
// (run 9, 0x088188E8..0x08818938) computes sectorSize * sectorCount *
// freeClusters / 1024 and branches when it is under 256 (KiB).
//
// fatms0: 0x02425823 (run 10: out = 4-byte stack word, no input): "memory
// stick FAT ready / device assigned". No uOFW, PSPSDK or pspautotests source;
// [INFERRED] from the behaviour references (AGENTS.md section 4, read only):
// PPSSPP Core/HLE/sceIo.cpp and JPCSP IoFileMgrForUser.java both write 1
// (stick inserted and ready) to the output word and return 0. They disagree
// on errors (PPSSPP ignores outLen and delays the result by 23.5 ms; JPCSP
// needs outLen >= 4); neither case is observed, so only outLen >= 4 with a
// valid address is modelled, without the delay.
struct MemoryStickInfo {
    // [INFERRED] a fixed 1 GB (SI) stick, all free: deterministic, inside the
    // hardware test's range, and small enough that Persona's 32-bit product
    // (512 * 64 * freeClusters) does not overflow.
    static constexpr std::uint32_t kClusters = 1000000000u / (512u * 64u);
    static constexpr std::uint32_t kSectorSize = 512u, kSectorCount = 64u;
};
[[nodiscard]] std::string describe_devctl(const std::string &device, std::uint32_t cmd, std::uint32_t indata,
                                          std::uint32_t inlen, std::uint32_t outdata, std::uint32_t outlen);

// InterruptManager::0xCA04A2B9
// int sceKernelRegisterSubIntrHandler(int intno, int no, void *handler, void *arg);
// (uOFW src/kd/interruptman exports). Reached on the New 3DS (first run,
// 591 frames) but not yet on the PC. Running a guest handler on every
// interrupt is a scheduler feature; until Persona's arguments are observed
// the call stops the run and names them.
[[nodiscard]] std::string describe_subintr(std::uint32_t intno, std::uint32_t no, std::uint32_t handler, std::uint32_t arg);

void register_fork_kernel_imports(psprecomp::Runtime &runtime, KernelState &kernel);

} // namespace p3p3ds::hle
