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

PspDateTime psp_date_time(std::uint64_t unix_microseconds) noexcept {
    const std::uint64_t seconds = unix_microseconds / 1000000u;
    const std::uint64_t days = seconds / 86400u, rest = seconds % 86400u;
    // Proleptic Gregorian civil date from days since 1970-01-01 (H. Hinnant's
    // civil_from_days, for non-negative day counts).
    const std::uint64_t z = days + 719468u, era = z / 146097u, doe = z - era * 146097u;
    const std::uint64_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const std::uint64_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u), mp = (5u * doy + 2u) / 153u;
    const std::uint64_t day = doy - (153u * mp + 2u) / 5u + 1u, month = mp < 10u ? mp + 3u : mp - 9u;
    const std::uint64_t year = yoe + era * 400u + (month <= 2u ? 1u : 0u);
    return PspDateTime{static_cast<std::uint16_t>(year), static_cast<std::uint16_t>(month), static_cast<std::uint16_t>(day),
                       static_cast<std::uint16_t>(rest / 3600u), static_cast<std::uint16_t>(rest / 60u % 60u),
                       static_cast<std::uint16_t>(rest % 60u), static_cast<std::uint32_t>(unix_microseconds % 1000000u)};
}

namespace {
bool leap_year(std::uint32_t y) noexcept { return (y % 4u == 0u && y % 100u != 0u) || y % 400u == 0u; }

// Days since 0001-01-01 (H. Hinnant's days_from_civil, shifted from 1970).
std::uint64_t days_since_year_one(std::uint32_t y, std::uint32_t m, std::uint32_t d) noexcept {
    y -= m <= 2u ? 1u : 0u;
    const std::uint64_t era = y / 400u, yoe = y - era * 400u;
    const std::uint64_t doy = (153u * (m > 2u ? m - 3u : m + 9u) + 2u) / 5u + d - 1u;
    const std::uint64_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097u + doe - 306u; // 0000-03-01 based count minus the days to 0001-01-01
}
} // namespace

bool psp_date_time_valid(const PspDateTime &t) noexcept {
    static constexpr std::uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (t.year < 1u || t.year > 9999u || t.month < 1u || t.month > 12u) return false;
    const std::uint32_t days = kDays[t.month - 1u] + (t.month == 2u && leap_year(t.year) ? 1u : 0u);
    return t.day >= 1u && t.day <= days && t.hour < 24u && t.minute < 60u && t.second < 60u && t.microsecond < 1000000u;
}

std::uint64_t psp_tick(const PspDateTime &t) noexcept {
    const std::uint64_t seconds = days_since_year_one(t.year, t.month, t.day) * 86400u + t.hour * 3600u + t.minute * 60u + t.second;
    return seconds * 1000000u + t.microsecond;
}

void register_fork_kernel_imports(psprecomp::Runtime &runtime, KernelState &kernel) {
    // sceRtc::0x6FF40ACC - sceRtcGetTick
    runtime.register_hle("sceRtc", 0x6FF40ACCu, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        const std::uint32_t date = ctx.gpr[4], tick = ctx.gpr[5]; // $a0, $a1
        if ((tick & 0x80000000u) != 0u) { ctx.set_gpr(2, 0x80000023u); return; }
        auto &m = rt.memory();
        if (!m.contains(date, 16u) || !m.contains(tick, 8u)) { // a real PSP faults
            rt.stop("sceRtcGetTick: date " + psprecomp::hex32(date) + " or tick " + psprecomp::hex32(tick) + " is not guest memory");
            return;
        }
        const PspDateTime t{m.load16(date), m.load16(date + 2u), m.load16(date + 4u), m.load16(date + 6u),
                            m.load16(date + 8u), m.load16(date + 10u), m.load32(date + 12u)};
        if (!psp_date_time_valid(t)) { ctx.set_gpr(2, 0x800001FEu); return; }
        const auto value = psp_tick(t);
        m.store32(tick, static_cast<std::uint32_t>(value));
        m.store32(tick + 4u, static_cast<std::uint32_t>(value >> 32));
        ctx.set_gpr(2, 0u);
    });


    // sceRtc::0xE7C27D1B - sceRtcGetCurrentClockLocalTime
    runtime.register_hle("sceRtc", 0xE7C27D1Bu, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        const std::uint32_t out = ctx.gpr[4]; // $a0
        if ((out & 0x80000000u) != 0u) { ctx.set_gpr(2, 0x80000023u); return; }
        if (!rt.memory().contains(out, 16u)) { // a real PSP faults (rtc.c: "NULL ... Crash")
            rt.stop("sceRtcGetCurrentClockLocalTime: output " + psprecomp::hex32(out) + " is not guest memory");
            return;
        }
        const auto t = psp_date_time(kForkRtcEpochSeconds * 1000000u + kernel.threads().now());
        auto &m = rt.memory();
        m.store16(out, t.year); m.store16(out + 2u, t.month); m.store16(out + 4u, t.day);
        m.store16(out + 6u, t.hour); m.store16(out + 8u, t.minute); m.store16(out + 10u, t.second);
        m.store32(out + 12u, t.microsecond);
        ctx.set_gpr(2, 0u);
    });


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
