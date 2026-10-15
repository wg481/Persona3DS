#pragma once
// Kernel imports added by the Persona3DS fork for games other than P3P.
// Kept in their own files (plus one registration call in hle_modules.cpp) so
// that upstream P3P3DS changes keep merging; drop an entry here if upstream
// implements the same NID.
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace psprecomp {
class Runtime;
struct AllegrexContext;
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

// sceMpeg 0x211A057C sceMpegAvcQueryYCbCrSize(mpeg, mode, width, height, *size)
// (signature: pspautotests video/mpeg/playertiming.c). Persona (PC run 13,
// New 3DS run 3) calls it with mode 1 after sceMpegRegistStream. No hardware
// .expected exists; [INFERRED] from the behaviour references (AGENTS.md
// section 4; read, not copied), which agree for mode 1: width and height
// multiples of 16, width <= 480, height <= 272, size = 128 + (width / 2) *
// (height / 2) * 6 (480 x 272: 195,968), return 0 (PPSSPP sceMpeg.cpp; JPCSP
// sceMpeg.java, with its two plane sizes summing to the same value). They
// disagree on width <= 16 or height 0, on the other modes and on writing
// *size on error, so those calls stop the run and name their arguments.
// The mpeg handle is not checked [INFERRED: Persona passes the one it created].
[[nodiscard]] std::uint32_t mpeg_avc_ycbcr_size_mode1(std::uint32_t width, std::uint32_t height) noexcept; // 0 = not modelled
[[nodiscard]] std::string describe_ycbcr_query(std::uint32_t mpeg, std::uint32_t mode, std::uint32_t width,
                                               std::uint32_t height, std::uint32_t result);

// sceMpeg 0x67179B1B sceMpegAvcInitYCbCr(mpeg, mode, width, height, buffer)
// (signature: playertiming.c). Persona (PC run 14, New 3DS run 4) calls it
// in a loop from 0x088117F0, once per buffer of a table, with the same
// mpeg, mode 1 and dimensions as sceMpegAvcQueryYCbCrSize; any non-zero
// result leaves the loop. No hardware .expected exists. The behaviour
// references agree on returning 0 for mode 1 and a valid buffer, and
// disagree on side effects: PPSSPP writes nothing; JPCSP clears a 128-byte
// header and stores plane pointers at buffer + 16..44. Modelled: mode 1
// with dimensions sceMpegAvcQueryYCbCrSize accepts and a buffer of that size
// in guest memory -> 0, buffer untouched [INFERRED]. If a later YCbCr call
// fails on the buffer contents, the JPCSP header is the first suspect.
// Anything else stops the run and names its arguments.
[[nodiscard]] std::string describe_ycbcr_init(std::uint32_t mpeg, std::uint32_t mode, std::uint32_t width,
                                              std::uint32_t height, std::uint32_t buffer);

// ThreadManForUser 0xA66B0120 sceKernelReferEventFlagStatus(evid, info).
// Persona (PC run 15, New 3DS run 5) calls it from 0x088146BC in its movie
// player, with info->size = 52 written just before. uOFW has not reversed it;
// results [VERIFIED] pspautotests threads/events/refer + create (hardware):
// unknown, NULL or deleted id -> 0x8002019A; size 0 -> 0 with nothing
// written; any other size -> 0 and size becomes 52; with size 52 the info is
// {size, name[32] (31 chars + NUL), attr, initPattern, currentPattern,
// numWaitThreads}. How much is written for sizes 1..51 or above 52 is not
// shown by the test, so those (and an info not in guest memory, which
// crashes a PSP) stop the run and name the call.
[[nodiscard]] std::string describe_refer_event_flag(std::int32_t uid, std::uint32_t info, std::uint32_t size);

// sceMpeg 0xF0EB1125 sceMpegAvcDecodeYCbCr(mpeg, au, buffer, init).
// Persona (PC run 16, New 3DS run 6) calls it from 0x08813DB8 with the
// SceMpeg pointer, its AVC AU, a pointer to a word holding one of the
// sceMpegAvcInitYCbCr buffers (stored in the delay slot) and $sp as *init.
// No hardware .expected exists for the YCbCr path. Modelled [INFERRED] like
// upstream's sceMpegAvcDecode, whose pipeline is [VERIFIED] by
// video/mpeg/basic.expected, on the same MpegInstance (KernelState::mpeg()):
// with a non-empty AU the first decode after create/flush reports no picture
// and every later one reports 1; the AU size is cleared; returns 0. The
// decoder is the upstream placeholder, so no YCbCr pixels are written; the
// conversion call (sceMpegAvcCsc) produces the black picture. PPSSPP
// (read, not copied) instead writes *init = 1 for newer libraries; not
// observed here. An unknown handle or an AU, buffer pointer or init outside
// guest memory stops the run and names the call.
[[nodiscard]] std::string describe_decode_ycbcr(std::uint32_t mpeg, std::uint32_t au, std::uint32_t buffer,
                                                std::uint32_t init);

// sceMpeg 0x31BD0272 sceMpegAvcCsc(mpeg, source, range, frameWidth, dest).
// Persona (PC run 17, New 3DS run 7) calls it on displayThread from
// 0x088139C8: the SceMpeg pointer, one YCbCr buffer, range = $sp holding
// {0, 0, width, height} (the same words as the size query), frameWidth 512,
// dest = a VRAM base + index * 0x88000 (the double-buffered framebuffer).
// No hardware .expected exists. [INFERRED] from the behaviour references
// (read, not copied): PPSSPP converts the range into dest in the mpeg's
// pixel mode (default 8888; Persona never calls sceMpegAvcDecodeMode and
// displays an 8888 framebuffer with stride 512) and returns 0; JPCSP
// requires x, y, width and height to be multiples of 16 (0x806101FE
// otherwise) and reads the plane pointers from the sceMpegAvcInitYCbCr
// header. The upstream decoder is a black placeholder, so the range is
// filled with opaque black (0xFF000000, as BlankMpegDecoder does) and the
// YCbCr buffer is not read. Neither reference's CSC delay is modelled.
// Unmodelled forms (unknown handle, range not multiples of 16 or empty,
// frameWidth 0 or smaller than x + width, dest rows outside guest memory)
// stop the run and name the call.
[[nodiscard]] std::string describe_avc_csc(std::uint32_t mpeg, std::uint32_t source, std::uint32_t range,
                                           std::uint32_t frame_width, std::uint32_t dest);

// ThreadManForUser 0x383F7BCC sceKernelTerminateDeleteThread(thid).
// Persona (PC run 18, New 3DS run 8) calls it from 0x088153D4 after the
// intro movie, on its player threads, which have already exited. Results
// [VERIFIED] pspautotests threads/threads/terminate.expected (hardware):
// -1, invalid or deleted -> 0x80020198; 0 or the current thread ->
// 0x80020197; created (never started) or finished -> 0 and deleted. These
// are upstream's ThreadManager::delete_thread results for a dormant thread.
// Ready, waiting and suspended threads also give 0 on hardware, but that
// terminates a live thread (its waiters wake with 0x800201AC); not observed,
// so those stop the run and name the call.
[[nodiscard]] std::string describe_terminate_delete(std::int32_t uid, std::uint32_t status);

// sceUmdUser 0xE83742BA sceUmdDeactivate(mode, drive). Persona (PC run 19,
// New 3DS run 9) calls it from 0x0881528C after the intro movie with mode 2
// and the "disc0:" string it gave sceUmdActivate (0x08A65314), then cleans
// up its UMD callback. Results [VERIFIED] pspautotests umd/api/api.expected
// (mode 19, -1, NULL drive, kernel pointer -> 0x80010016) and
// umd/callbacks/umd.expected (mode 1, "disc0:" -> 0, the registered UMD
// callback is notified again); mode and drive are validated like upstream's
// UmdState::activate (modes 1/2, exactly "disc0:"). The notification
// argument 0x12 (MEDIA_IN|READY) is [INFERRED] (the test only shows it is
// non-zero; see umd.cpp). Gap [INFERRED]: upstream's UmdState cannot clear
// its activation flag from fork code, so sceUmdGetDriveStat still reports
// READABLE (0x32) afterwards.
[[nodiscard]] std::uint32_t umd_deactivate_result(std::uint32_t mode, const std::string &drive) noexcept;

// sceUmdUser 0xBD2BDE07 sceUmdUnRegisterUMDCallBack(cbid). Added ahead of
// the game reaching it (maintainer's request, 2026-10-11): the code after
// Persona's sceUmdDeactivate ([INFERRED] stub 0x089813EC, called from
// 0x088152A0 with $a0 = the UMD callback id at 0x08A67E50) unregisters the
// callback before deleting it; $v0 is not read there. Results [VERIFIED]
// pspautotests umd/api/api.expected (the registered callback -> its own id;
// a semaphore -> 0x80010016) and umd/callbacks/umd.expected, umd/wait (a
// second unregister and -1 fail with 0x80010016). uOFW mediaman.c returns 0
// instead for SDK >= 3.00 [UNVERIFIED which firmware]; the hardware value
// is used.
//
// Fork-owned UMD state: upstream's UmdState has no deactivate/unregister,
// so sceUmdDeactivate and sceUmdUnRegisterUMDCallBack rebuild it through
// its public API (medium presence kept; activation and callback kept or
// dropped). sceUmdGetDriveStat (umd.cpp) then reports 0x12 after a
// deactivate, and upstream's sceUmdActivate no longer notifies an
// unregistered callback.
//
// sceUmdUser 0x4A9E5E29 sceUmdWaitDriveStatCB(stat, timeout) and
// 0x6AF9B50A sceUmdCancelWaitDriveStat(). Imported by Persona; added ahead
// of the game reaching them. [VERIFIED] pspautotests umd/api (stat 0, 0x04,
// 0x40, 0x80, 0xFFFFFFC4 -> 0x80010016: no bit of 0x3B set) and umd/wait
// (any requested bit present -> 0: 0xFF matches after a deactivate; READABLE
// after a deactivate times out with 0x800201A8; timeout 0 waits until
// sceUmdCancelWaitDriveStat, which returns 0, also with no waiters, and
// wakes the waiter with 0x800201A9; timeouts under ~250 us take ~250 us).
// The drive stat is umd.cpp's sceUmdGetDriveStat value. Not modelled
// [INFERRED: not observed]: a waiter woken early by another thread's
// sceUmdActivate, callbacks delivered when the wait is satisfied at once,
// and a cancel issued from a callback of the waiting thread.
inline constexpr std::uint32_t kUmdWaitableStats = 0x3Bu;     // NOT_PRESENT|PRESENT|CHANGED|INITING|INITED|READY
inline constexpr std::uint32_t kUmdWaitCanceled = 0x800201A9u; // SCE_KERNEL_ERROR_WAIT_CANCEL
inline constexpr std::int32_t kUmdWaitObject = 0x554D44;       // host wait object for drive-stat waiters ("UMD")
inline constexpr std::uint64_t kUmdMinWaitUs = 250u;
[[nodiscard]] std::optional<std::uint32_t> umd_wait_result(std::uint32_t stat, std::uint32_t drive_stat) noexcept; // nullopt: blocks

// sceMpeg 0xF2930C9C sceMpegAvcDecodeStopYCbCr(mpeg, buffer, status).
// Imported by Persona; added ahead of the game reaching it. As upstream's
// sceMpegAvcDecodeStop and PPSSPP (read, not copied): *status = 0 (no
// pending picture), return 0 [INFERRED]; JPCSP instead flushes one pending
// picture into *buffer, which the placeholder decoder does not have. An
// unknown handle or a status outside guest memory stops and names the call.
[[nodiscard]] std::string describe_decode_stop_ycbcr(std::uint32_t mpeg, std::uint32_t buffer, std::uint32_t status);

// sceDisplay 0x4D4E10EC sceDisplayIsVblank(). Imported by Persona; added
// ahead of the game reaching it. [VERIFIED] pspautotests display/isstate and
// vblanklen (hardware): 1 from vblank start for about 731.5 us (measured
// 730-740 us), then 0 until the next start; the frame is 16,680-16,690 us.
// Upstream's virtual clock starts each vblank at a multiple of
// ThreadManager::kVblankPeriodUs, so: (now % period) < kVblankLengthUs.
// Persona3DS profiling: time spent inside sceGeListUpdateStallAddr's GE work
// (GeManager::update_stall: running the newly appended display-list commands),
// in host_profile().clock units, without the HLE call's own entry/exit cost.
// The first dungeon calls it about 10,500 times a second.
struct GeStallTiming {
    std::uint64_t calls{}, units{};
    // Opcodes of the commands each call releases, read from the list's current
    // pc up to the new stall address (the list's own words; sub-lists reached
    // through CALL are not followed, the CALLs themselves are counted).
    std::array<std::uint64_t, 256> opcodes{};
    std::uint64_t scanned{};
    // Batching (set_ge_stall_batching): update_stall runs ("flushes"), and why: the
    // batch reached its cap, another import was called, another list was updated,
    // or the update could not be deferred (unknown/completed list, unaligned).
    std::uint64_t flushes{}, by_cap{}, by_import{}, by_list{}, direct{};
    std::uint64_t merged{}; // P3P3DS: updates with lists on the GE worker that replaced a pending one (no task)
    // Timing (units) and the opcode census (opcodes, scanned) only when set: each costs timer reads
    // (kernel calls on the New 3DS) or a scan per update. Calls and flushes are always counted.
    bool detail{};
};

// Persona3DS: sceGeListUpdateStallAddr batching. With max_updates > 0, a valid
// stall update only records the new stall address; the GE runs the released
// commands (GeManager::update_stall) once max_updates updates are pending, before
// any other HLE import (pre-import hook), or when another list is updated. Every
// import therefore sees the GE as if each update had run at once; only updates
// with nothing but game code between them are run together. 0 (the default)
// runs every update at once, as upstream. The New 3DS runner turns it on.
void set_ge_stall_batching(unsigned max_updates) noexcept;
// Runs the pending stall update, if any (also used by tests).
void flush_ge_stall(psprecomp::Runtime &rt);
// The `count` opcodes most often released per call so far, as "op:per-call"
// pairs, e.g. "C4:1.0 04:1.0 0A:0.9".
[[nodiscard]] std::string ge_stall_top_opcodes(std::size_t count);
[[nodiscard]] GeStallTiming &ge_stall_timing() noexcept;

inline constexpr std::uint64_t kVblankLengthUs = 732u;
[[nodiscard]] constexpr std::uint32_t display_is_vblank(std::uint64_t now_us, std::uint64_t period_us) noexcept {
    return now_us % period_us < kVblankLengthUs ? 1u : 0u;
}

// ModuleMgrForUser 0xF0A26395 sceKernelGetModuleId, 0xD1FF982A
// sceKernelStopModule, 0x2E0911AA sceKernelUnloadModule, 0x8F2DF740
// sceKernelStopUnloadSelfModuleWithStatus; LoadExecForUser 0x05572A5F
// sceKernelExitGame. Imported by Persona; added ahead of the game reaching
// them. Contract: uOFW src/kd/modulemgr (queryModule.c, start-stopModule.c)
// and pspautotests modules/startoptions, modules/unloadfill (hardware).
//  GetModuleId: the module containing the caller's return address (uOFW);
//    a PRX loaded by upstream's sceKernelLoadModule gives its id, anything
//    else the main module, id 1 as upstream's sceKernelGetModuleIdByAddress
//    [INFERRED].
//  StopModule(uid, args, argp, *status, option): module_stop (system export
//    NID 0xCEE8593C) runs on its own thread, priority 0x20, stack 0x40000
//    (startoptions.expected), and the call returns 0 with *status = its exit
//    status (unloadfill.expected); memory is unchanged. Without module_stop
//    (or an HLE-only module) *status = 0 and 0 [INFERRED, as upstream's
//    sceKernelStartModule without module_start]. Unknown id 0x8002012E.
//  UnloadModule(uid): returns >= 0 (unloadfill.expected; the id, uOFW);
//    the text is filled with `break` (0x0000004D) and data/bss with
//    0xFFFFFFFF, then the memory is freed. EBOOT stubs bound to its exports
//    are not unbound [INFERRED: not observed].
//  StopUnloadSelfModuleWithStatus from the main module, and ExitGame: the
//    game is quitting; the run stops with a message that says so.
//  Not modelled, stop and name the call: an SMOption, stopping a module
//  that is not started or already stopped, unloading a started module that
//  was not stopped, StopUnloadSelf from a loaded PRX.
inline constexpr std::uint32_t kMainModuleId = 1u;
inline constexpr std::uint32_t kModuleStopNid = 0xCEE8593Cu;

// IoFileMgrForUser 0x779103A0 sceIoRename(old, new). Imported by Persona;
// added ahead of the game reaching it. [VERIFIED] pspautotests
// io/file/rename.expected (hardware): a wildcard (* or ?) in either path ->
// 0x80010016; different devices -> 0x80020322; a missing source ->
// 0x80010002; an existing target, a missing target directory or the same
// path -> 0x80010011; otherwise 0. Upstream's IoFileMgr has no writable
// files (sceIoWrite to a file returns a bad descriptor), so only the error
// cases that need no write are modelled; renaming an existing file, and
// relative paths (no current directory), stop the run and name the call.
[[nodiscard]] std::optional<std::uint32_t> io_rename_static_result(const std::string &from, const std::string &to); // nullopt: needs the files

// sceUtility MsgDialog: 0x2AD8E239 InitStart(params), 0x9A1C91D7
// GetStatus, 0x95FC253B Update(n), 0x67AF3428 ShutdownStart. Imported by
// Persona; added ahead of the game reaching them. Params: PSPSDK
// psputility_msgdialog.h (base 48 bytes, unknown/result at 48, mode 52,
// errorValue 56, message[512] 60, options 572, buttonPressed 576; sizes 572
// / 580 / 708). [VERIFIED] pspautotests utility/msgdialog/dialog.expected
// (hardware): InitStart 0; status 1 (INIT), then 2 (VISIBLE) with the word
// at 48 set to 0; 3 (QUIT) when the dialog ends; ShutdownStart 0 and status
// 4 (FINISHED) once, then 0. The hardware's fade-in (~34 frames) and fade-out
// (~27) are compressed as upstream's savedata dialog does (INIT and FINISHED
// reported once each; done on the first Update while VISIBLE). There is no
// dialog UI yet: the dialog confirms its default button at once
// [INFERRED, as PPSSPP's confirm]: buttonPressed (sizes >= 580) = 1 (YES/OK),
// or 2 (NO) for a yes/no dialog (options 0x10) whose default is No (options
// 0x100). The message, mode, error value and options are logged
// (`msgdialog` event and console) so the run shows what the game said.
// Out-of-sequence calls return 0x80110001 [INFERRED, as upstream savedata];
// InitStart while the savedata dialog is active stops and names the call.
// Persona's EBOOT has no direct call to sceUtilityMsgDialogUpdate
// (tools/p1_callsites.py, 2026-10-11: 0 jal sites; it may be called
// indirectly). A dialog only advances in Update, so a dialog still VISIBLE
// kMsgDialogStallVblanks after it became visible, with no Update call, stops
// the run with a message instead of letting the game poll forever.
inline constexpr std::uint64_t kMsgDialogStallVblanks = 600u; // 10 s
struct MsgDialogState {
    int status{};                         // 0 none, 1 init, 2 visible, 3 quit, 4 finished
    bool init_reported{}, shutdown_reported{};
    std::uint32_t param{}, size{};
    std::uint64_t visible_since{};        // vblank count when VISIBLE was first reported
};
inline constexpr std::uint32_t kMsgDialogOptionYesNo = 0x10u, kMsgDialogOptionDefaultNo = 0x100u;
[[nodiscard]] constexpr std::uint32_t msgdialog_default_button(std::uint32_t options) noexcept {
    return (options & kMsgDialogOptionYesNo) != 0u && (options & kMsgDialogOptionDefaultNo) != 0u ? 2u : 1u;
}

// ThreadManForUser 0x809CE29B sceKernelExitDeleteThread(status). Persona
// (PC run 21, New 3DS run 11) calls it from 0x0881128C to end its BGM thread
// (status 1). Upstream's ThreadManager::exit_current_thread (threads waiting
// for the end wake with the exit status, the next thread is dispatched),
// then delete_thread on the exited thread: [VERIFIED] pspautotests
// threads/threads/threadend.expected "Exit deleted": a later wait on it gives
// 0x80020198 (unknown thread). Waiters at the time of the exit get the exit
// status [INFERRED: PPSSPP __KernelDeleteThread; no hardware case].

// InterruptManager 0xCA04A2B9 sceKernelRegisterSubIntrHandler(intno, no, handler, arg),
// 0xFB8E22EC sceKernelEnableSubIntr(intno, no), 0xD61E6961
// sceKernelReleaseSubIntrHandler(intno, no) (uOFW src/kd/interruptman).
// Persona (PC run 12, New 3DS run 2): intno 30 (vblank), no 0, handler
// 0x0881203C, arg 0x090DF380. Only intno 30 is modelled; intno < 0 or >= 68
// returns 0x80020065 (uOFW, registersub.expected), another valid intno stops
// the run and names the call.
// Results [VERIFIED] pspautotests intr/registersub, releasesub, enablesub
// (hardware), intno 30:
//  register: no 0..15 -> 0 (a NULL handler registers but leaves the slot
//    free); occupied by a non-NULL handler -> 0x80020067; no 18-20, 24-26
//    -> 0x80020067 (system handlers); any other no -> 0x80020065.
//  release: no 0..15 with a non-NULL handler -> 0 (and disables it); 0..15
//    without one, 16..31 -> 0x80020068; any other no -> 0x80020065.
//  enable: no 0..15 -> 0 even without a handler (a handler registered later
//    runs); invalid no -> 0x80020065. [INFERRED] 16..31 -> 0x80020065 (uOFW:
//    user mode may not touch system sub-interrupts).
//  An enabled sub-interrupt with a handler runs handler(no, arg) once per
//  vblank ("* handler(00000001, deadbeef)" after a 17 ms delay); a released
//  handler does not run again until re-enabled.
class VblankSubInterrupts {
public:
    static constexpr std::uint32_t kInvalid = 0x80020065u, kExists = 0x80020067u, kNotFound = 0x80020068u;
    static constexpr int kUserSlots = 16;
    struct Slot { std::uint32_t handler{}, arg{}, gp{}; bool enabled{}; };
    std::uint32_t register_handler(std::int32_t no, std::uint32_t handler, std::uint32_t arg, std::uint32_t gp) noexcept;
    std::uint32_t release(std::int32_t no) noexcept;
    std::uint32_t enable(std::int32_t no) noexcept;
    [[nodiscard]] const Slot &slot(int no) const noexcept { return slots_[no]; }
    [[nodiscard]] bool any_runnable() const noexcept;
    std::uint64_t last_vblank{};   // vblank count whose handlers have run
    bool in_handler{};             // a handler is running (no nesting)

private:
    Slot slots_[kUserSlots]{};
};

// Runs the enabled vblank handlers if a vblank passed since they last ran:
// from a dispatch boundary (runtime starvation hook), as a guest call on the
// current thread that resumes at ctx.pc. Missed vblanks (all threads
// waiting, no dispatches) collapse into one call [INFERRED]; the handler may
// be preempted by threads its HLE calls wake [INFERRED: on the PSP it would
// finish first]. Returns true if a handler was started.
bool dispatch_vblank_subinterrupts(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx);
[[nodiscard]] VblankSubInterrupts &fork_vblank_subinterrupts() noexcept;

[[nodiscard]] std::string describe_subintr(std::uint32_t intno, std::uint32_t no, std::uint32_t handler, std::uint32_t arg);

void register_fork_kernel_imports(psprecomp::Runtime &runtime, KernelState &kernel);

} // namespace p3p3ds::hle
