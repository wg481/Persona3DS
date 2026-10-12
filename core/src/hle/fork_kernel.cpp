// See fork_kernel.hpp.
#include "p3p3ds/hle/fork_kernel.hpp"
#include "p3p3ds/hle/fork_media.hpp"
#include "p3p3ds/hle/fork_movie.hpp"
#include "p3p3ds/hle/fork_atrac.hpp"

#include "p3p3ds/hle/mpeg.hpp"
#include "p3p3ds/kernel_state.hpp"
#include "p3p3ds/profile.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
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

std::string describe_devctl(const std::string &device, std::uint32_t cmd, std::uint32_t indata, std::uint32_t inlen,
                            std::uint32_t outdata, std::uint32_t outlen) {
    return "sceIoDevctl(\"" + device + "\", " + psprecomp::hex32(cmd) + ", in " + psprecomp::hex32(indata) + "/" +
           std::to_string(inlen) + ", out " + psprecomp::hex32(outdata) + "/" + std::to_string(outlen) + ") is not modelled yet";
}

std::string describe_subintr(std::uint32_t intno, std::uint32_t no, std::uint32_t handler, std::uint32_t arg) {
    return "sceKernelRegisterSubIntrHandler(intno " + std::to_string(intno) + ", no " + std::to_string(no) + ", handler " +
           psprecomp::hex32(handler) + ", arg " + psprecomp::hex32(arg) + ") is not modelled yet";
}

std::uint32_t mpeg_avc_ycbcr_size_mode1(std::uint32_t width, std::uint32_t height) noexcept {
    if ((width & 15u) != 0u || (height & 15u) != 0u || width <= 16u || width > 480u || height == 0u || height > 272u) return 0u;
    return 128u + (width / 2u) * (height / 2u) * 6u;
}

std::string describe_ycbcr_query(std::uint32_t mpeg, std::uint32_t mode, std::uint32_t width, std::uint32_t height,
                                 std::uint32_t result) {
    return "sceMpegAvcQueryYCbCrSize(mpeg " + psprecomp::hex32(mpeg) + ", mode " + std::to_string(mode) + ", " +
           std::to_string(width) + " x " + std::to_string(height) + ", out " + psprecomp::hex32(result) + ") is not modelled yet";
}

std::string describe_ycbcr_init(std::uint32_t mpeg, std::uint32_t mode, std::uint32_t width, std::uint32_t height,
                                std::uint32_t buffer) {
    return "sceMpegAvcInitYCbCr(mpeg " + psprecomp::hex32(mpeg) + ", mode " + std::to_string(mode) + ", " +
           std::to_string(width) + " x " + std::to_string(height) + ", buffer " + psprecomp::hex32(buffer) + ") is not modelled yet";
}

std::string describe_refer_event_flag(std::int32_t uid, std::uint32_t info, std::uint32_t size) {
    return "sceKernelReferEventFlagStatus(uid " + std::to_string(uid) + ", info " + psprecomp::hex32(info) + ", size " +
           std::to_string(size) + ") is not modelled yet";
}

std::string describe_decode_ycbcr(std::uint32_t mpeg, std::uint32_t au, std::uint32_t buffer, std::uint32_t init) {
    return "sceMpegAvcDecodeYCbCr(mpeg " + psprecomp::hex32(mpeg) + ", au " + psprecomp::hex32(au) + ", buffer " +
           psprecomp::hex32(buffer) + ", init " + psprecomp::hex32(init) + ") is not modelled yet";
}

std::string describe_avc_csc(std::uint32_t mpeg, std::uint32_t source, std::uint32_t range, std::uint32_t frame_width,
                             std::uint32_t dest) {
    return "sceMpegAvcCsc(mpeg " + psprecomp::hex32(mpeg) + ", source " + psprecomp::hex32(source) + ", range " +
           psprecomp::hex32(range) + ", frameWidth " + std::to_string(frame_width) + ", dest " + psprecomp::hex32(dest) +
           ") is not modelled yet";
}

std::string describe_terminate_delete(std::int32_t uid, std::uint32_t status) {
    return "sceKernelTerminateDeleteThread(uid " + std::to_string(uid) + ", internal state " + std::to_string(status) +
           ") on a live thread is not modelled yet";
}

std::uint32_t umd_deactivate_result(std::uint32_t mode, const std::string &drive) noexcept {
    return (mode == 1u || mode == 2u) && drive == "disc0:" ? 0u : 0x80010016u;
}

std::string describe_decode_stop_ycbcr(std::uint32_t mpeg, std::uint32_t buffer, std::uint32_t status) {
    return "sceMpegAvcDecodeStopYCbCr(mpeg " + psprecomp::hex32(mpeg) + ", buffer " + psprecomp::hex32(buffer) + ", status " +
           psprecomp::hex32(status) + ") is not modelled yet";
}

std::optional<std::uint32_t> umd_wait_result(std::uint32_t stat, std::uint32_t drive_stat) noexcept {
    if ((stat & kUmdWaitableStats) == 0u) return 0x80010016u;
    if ((stat & drive_stat) != 0u) return 0u;
    return std::nullopt;
}

namespace {
// A loaded PRX (upstream ModuleManager) whose memory contains `address`.
const hle::LoadedModule *module_at(KernelState &kernel, std::uint32_t address) {
    for (const auto &[uid, module] : kernel.modules().modules)
        if (!module.hle_only && address >= module.base && address - module.base < module.size) return &module;
    return nullptr;
}

// What upstream's sceKernelLoadModule does not keep: text size and module_stop.
struct ModuleLayout { std::uint32_t text_size{}, module_stop{}; };
std::optional<ModuleLayout> module_layout(psprecomp::Runtime &rt, KernelState &kernel, const hle::LoadedModule &module) {
    const auto resolved = kernel.io().resolve(module.path);
    const auto source = resolved ? resolved->fs->open(resolved->path) : nullptr;
    if (!source) return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(source->size()));
    (void)source->read(0, bytes.data(), bytes.size());
    const auto image = psprecomp::Elf32Image::from_bytes(bytes, module.path);
    ModuleLayout layout;
    if (!image.segments().empty()) layout.text_size = std::min(image.segments().front().memory_size, module.size);
    const auto info = image.find_module_info(rt.memory(), module.base);
    if (!info) return std::nullopt;
    auto &m = rt.memory();
    for (std::uint32_t entry = info->ent_top; entry + 16u <= info->ent_end;) { // as upstream modulemgr.cpp
        const auto name_ptr = m.load32(entry);
        const auto length = m.load8(entry + 8u), vstubs = m.load8(entry + 9u);
        const auto stubs = m.load16(entry + 10u);
        const auto table = m.load32(entry + 12u);
        for (std::uint32_t i = 0; name_ptr == 0u && i < stubs; ++i)
            if (m.load32(table + 4u * i) == kModuleStopNid) layout.module_stop = m.load32(table + 4u * (stubs + vstubs + i));
        entry += std::max<std::uint32_t>(length, 4u) * 4u;
    }
    return layout;
}
} // namespace

std::optional<std::uint32_t> io_rename_static_result(const std::string &from, const std::string &to) {
    if (from.find_first_of("*?") != std::string::npos || to.find_first_of("*?") != std::string::npos) return 0x80010016u;
    const auto from_colon = from.find(':'), to_colon = to.find(':');
    if (from_colon == std::string::npos || to_colon == std::string::npos) return std::nullopt; // relative
    auto device = [](const std::string &path, std::size_t colon) {
        std::string d = path.substr(0, colon);
        for (auto &ch : d) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return d;
    };
    if (device(from, from_colon) != device(to, to_colon)) return 0x80020322u;
    return std::nullopt;
}

namespace {
VblankSubInterrupts g_vblank_subintr;
KernelState *g_fork_kernel = nullptr;
MsgDialogState g_msgdialog;
std::map<std::int32_t, std::int32_t> g_module_stop_threads; // module uid -> thread running module_stop
std::map<std::int32_t, bool> g_modules_stopped;             // module uid -> module_stop finished
constexpr std::uint64_t kInterruptCheckDispatches = 32u; // dispatch boundaries between vblank checks
constexpr std::uint32_t kVblankInterrupt = 30u;

bool system_subintr(std::int32_t no) noexcept { return (no >= 18 && no <= 20) || (no >= 24 && no <= 26); }

void start_handlers(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx, int from);

void interrupt_boundary(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { (void)dispatch_vblank_subinterrupts(rt, ctx); }

// Runs the next enabled handler at or after `from`; each one chains the next
// from its continuation, the last clears in_handler.
void start_handlers(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx, int from) {
    auto &s = g_vblank_subintr;
    for (int no = from; no < VblankSubInterrupts::kUserSlots; ++no) {
        const auto &slot = s.slot(no);
        if (!slot.enabled || slot.handler == 0u) continue;
        const psprecomp::AllegrexContext resume = ctx;
        g_fork_kernel->threads().call_guest(rt, ctx, resume, slot.handler,
            {static_cast<std::uint32_t>(no), slot.arg}, slot.gp,
            [no](psprecomp::Runtime &r, psprecomp::AllegrexContext &c, std::uint32_t) { start_handlers(r, c, no + 1); });
        // Native callers still on the host stack must unwind: resume from ctx.pc.
        ++psprecomp::g_runtime_thread_switch_generation_fast;
        return;
    }
    s.in_handler = false;
}
} // namespace

void rebuild_umd_state(KernelState &kernel, bool activated, bool keep_callback) {
    auto &umd = kernel.umd();
    const bool present = umd.medium_present();
    const auto callback = umd.registered_callback();
    umd = hle::UmdState{};
    umd.set_medium_present(present);
    if (activated) (void)umd.activate(1u, "disc0:");
    if (keep_callback && callback != 0) (void)umd.register_callback(callback, kernel.threads());
}

std::uint32_t umd_drive_stat(const hle::UmdState &umd) noexcept {
    return !umd.medium_present() ? 0x01u : umd.activation_requested() ? 0x32u : 0x12u; // as umd.cpp sceUmdGetDriveStat
}


std::uint32_t VblankSubInterrupts::register_handler(std::int32_t no, std::uint32_t handler, std::uint32_t arg, std::uint32_t gp) noexcept {
    if (no < 0 || no >= 32) return kInvalid;
    if (no >= kUserSlots) return system_subintr(no) ? kExists : kInvalid;
    auto &slot = slots_[no];
    if (slot.handler != 0u) return kExists;
    slot.handler = handler;
    slot.arg = arg;
    slot.gp = gp;
    return 0u;
}

std::uint32_t VblankSubInterrupts::release(std::int32_t no) noexcept {
    if (no < 0 || no >= 32) return kInvalid;
    if (no >= kUserSlots || slots_[no].handler == 0u) return kNotFound;
    slots_[no] = Slot{};
    return 0u;
}

std::uint32_t VblankSubInterrupts::enable(std::int32_t no) noexcept {
    if (no < 0 || no >= kUserSlots) return kInvalid;
    slots_[no].enabled = true;
    return 0u;
}

bool VblankSubInterrupts::any_runnable() const noexcept {
    for (const auto &slot : slots_)
        if (slot.enabled && slot.handler != 0u) return true;
    return false;
}

VblankSubInterrupts &fork_vblank_subinterrupts() noexcept { return g_vblank_subintr; }

bool dispatch_vblank_subinterrupts(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    auto &s = g_vblank_subintr;
    if (g_fork_kernel == nullptr || s.in_handler || !s.any_runnable()) return false;
    auto &threads = g_fork_kernel->threads();
    const auto vblank = threads.vblank_count();
    if (vblank <= s.last_vblank || threads.current_thread() == nullptr) return false;
    s.last_vblank = vblank;
    s.in_handler = true;
    rt.event("vblank_subintr", {{"vblank", static_cast<std::uint32_t>(vblank)}});
    start_handlers(rt, ctx, 0);
    return true;
}

GeStallTiming &ge_stall_timing() noexcept {
    static GeStallTiming timing;
    return timing;
}

void register_fork_kernel_imports(psprecomp::Runtime &runtime, KernelState &kernel) {
    g_fork_kernel = &kernel;
    // sceGe_user::0xE0D68148 sceGeListUpdateStallAddr: upstream's GeManager::update_stall, timed
    // (ge_stall_timing). Replaces core/src/hle/ge.cpp's registration (fork imports register last).
    runtime.register_hle("sceGe_user", 0xE0D68148u, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &p = host_profile();
        const std::uint64_t start = profile_now(p);
        ctx.set_gpr(2, static_cast<std::uint32_t>(kernel.ge().update_stall(rt, static_cast<int>(ctx.gpr[4]), ctx.gpr[5])));
        auto &t = ge_stall_timing();
        t.units += profile_now(p) - start;
        ++t.calls;
    });
    g_vblank_subintr = VblankSubInterrupts{};
    g_module_stop_threads.clear();
    g_msgdialog = MsgDialogState{};
    g_modules_stopped.clear();
    psprecomp::set_runtime_starvation_hook(&interrupt_boundary, kInterruptCheckDispatches);
    atrac::register_fork_atrac(runtime, kernel); // sceAtrac3plus (fork_atrac.cpp)
    const auto subintr = [&runtime](std::uint32_t nid, auto fn) {
        runtime.register_hle("InterruptManager", nid, [fn](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto intno = static_cast<std::int32_t>(ctx.gpr[4]);
            // uOFW: intrNum >= 68 is invalid; registersub.expected: -2, -1, 68, 69 -> 0x80020065.
            if (intno < 0 || intno >= 68) { ctx.set_gpr(2, VblankSubInterrupts::kInvalid); return; }
            if (ctx.gpr[4] != kVblankInterrupt) { rt.stop(describe_subintr(ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], ctx.gpr[7])); return; }
            ctx.set_gpr(2, fn(ctx, static_cast<std::int32_t>(ctx.gpr[5])));
        });
    };
    // InterruptManager::0xCA04A2B9 - sceKernelRegisterSubIntrHandler
    subintr(0xCA04A2B9u, [](psprecomp::AllegrexContext &ctx, std::int32_t no) {
        return g_vblank_subintr.register_handler(no, ctx.gpr[6], ctx.gpr[7], ctx.gpr[28]);
    });
    // InterruptManager::0xD61E6961 - sceKernelReleaseSubIntrHandler
    subintr(0xD61E6961u, [](psprecomp::AllegrexContext &, std::int32_t no) { return g_vblank_subintr.release(no); });
    // InterruptManager::0xFB8E22EC - sceKernelEnableSubIntr
    subintr(0xFB8E22ECu, [](psprecomp::AllegrexContext &, std::int32_t no) {
        if (g_fork_kernel != nullptr && !g_vblank_subintr.any_runnable())
            g_vblank_subintr.last_vblank = g_fork_kernel->threads().vblank_count(); // first call at the next vblank
        return g_vblank_subintr.enable(no);
    });

    // IoFileMgrForUser::0x54F5FB11 - sceIoDevctl: modelled commands, else report the call.
    runtime.register_hle("IoFileMgrForUser", 0x54F5FB11u, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &m = rt.memory();
        const std::uint32_t dev = ctx.gpr[4], cmd = ctx.gpr[5], indata = ctx.gpr[6], inlen = ctx.gpr[7];
        const std::string device = dev != 0u && m.contains(dev, 1u) ? m.read_c_string(dev, 32u) : std::string("<invalid>");
        if ((device == "ms0:" || device == "fatms0:") && cmd == 0x02425818u && inlen >= 4u && m.contains(indata, 4u)) {
            const std::uint32_t info = m.load32(indata);
            if (m.contains(info, 20u)) {
                using Ms = MemoryStickInfo;
                m.store32(info, Ms::kClusters);       // maxClusters
                m.store32(info + 4u, Ms::kClusters);  // freeClusters
                m.store32(info + 8u, Ms::kClusters);  // maxSectors ([INFERRED] same range as the clusters)
                m.store32(info + 12u, Ms::kSectorSize);
                m.store32(info + 16u, Ms::kSectorCount);
                ctx.set_gpr(2, 0u);
                return;
            }
        }
        const std::uint32_t outdata = ctx.gpr[8], outlen = ctx.gpr[9];
        if (device == "fatms0:" && cmd == 0x02425823u && outlen >= 4u && m.contains(outdata, 4u)) {
            m.store32(outdata, 1u); // FAT ready: a stick is always inserted
            ctx.set_gpr(2, 0u);
            return;
        }
        rt.stop(describe_devctl(device, cmd, indata, inlen, outdata, outlen));
    });


    // sceMpeg::0x211A057C - sceMpegAvcQueryYCbCrSize: mode 1 with valid dimensions, else report the call.
    runtime.register_hle("sceMpeg", 0x211A057Cu, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &m = rt.memory();
        const std::uint32_t mode = ctx.gpr[5], width = ctx.gpr[6], height = ctx.gpr[7], result = ctx.gpr[8];
        const std::uint32_t size = mode == 1u ? mpeg_avc_ycbcr_size_mode1(width, height) : 0u;
        if (size != 0u && m.contains(result, 4u)) {
            m.store32(result, size);
            ctx.set_gpr(2, 0u);
            return;
        }
        rt.stop(describe_ycbcr_query(ctx.gpr[4], mode, width, height, result));
    });

    // sceMpeg::0x67179B1B - sceMpegAvcInitYCbCr: mode 1 with a buffer of the queried size, else report the call.
    runtime.register_hle("sceMpeg", 0x67179B1Bu, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        const std::uint32_t mode = ctx.gpr[5], width = ctx.gpr[6], height = ctx.gpr[7], buffer = ctx.gpr[8];
        const std::uint32_t size = mode == 1u ? mpeg_avc_ycbcr_size_mode1(width, height) : 0u;
        if (size != 0u && rt.memory().contains(buffer, size)) {
            ctx.set_gpr(2, 0u);
            return;
        }
        rt.stop(describe_ycbcr_init(ctx.gpr[4], mode, width, height, buffer));
    });

    // ThreadManForUser::0xA66B0120 - sceKernelReferEventFlagStatus: size 0 or 52, else report the call.
    runtime.register_hle("ThreadManForUser", 0xA66B0120u, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &m = rt.memory();
        const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
        const std::uint32_t info = ctx.gpr[5];
        const auto *flag = kernel.threads().get_event_flag(uid);
        if (flag == nullptr) { ctx.set_gpr(2, 0x8002019Au); return; } // SCE_KERNEL_ERROR_UNKNOWN_EVFID
        const std::uint32_t size = m.contains(info, 4u) ? m.load32(info) : 0xFFFFFFFFu;
        if (size == 0u) { ctx.set_gpr(2, 0u); return; }
        constexpr std::uint32_t kInfoSize = 52u;
        if (size != kInfoSize || !m.contains(info, kInfoSize)) {
            rt.stop(describe_refer_event_flag(uid, info, size));
            return;
        }
        for (std::uint32_t i = 0; i < 32u; ++i)
            m.store8(info + 4u + i, i < flag->name.size() && i < 31u ? static_cast<std::uint8_t>(flag->name[i]) : 0u);
        m.store32(info + 36u, flag->attr);
        m.store32(info + 40u, flag->init);
        m.store32(info + 44u, flag->bits);
        m.store32(info + 48u, static_cast<std::uint32_t>(flag->waiters.size()));
        ctx.set_gpr(2, 0u);
    });

    // sceMpeg::0xF0EB1125 - sceMpegAvcDecodeYCbCr: sceMpegAvcDecode's pipeline, no pixels.
    runtime.register_hle("sceMpeg", 0xF0EB1125u, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &m = rt.memory();
        const std::uint32_t mpeg = ctx.gpr[4], au = ctx.gpr[5], buffer = ctx.gpr[6], init = ctx.gpr[7];
        auto *inst = m.contains(mpeg, 4u) ? kernel.mpeg().find(m.load32(mpeg)) : nullptr;
        if (inst == nullptr || !m.contains(au, 24u) || !m.contains(buffer, 4u) || !m.contains(init, 4u)) {
            rt.stop(describe_decode_ycbcr(mpeg, au, buffer, init));
            return;
        }
        std::uint32_t pictures = 0;
        if (m.load32(au + hle::mpeg::AuSize) != 0u) {
            if (inst->video_pipeline_primed) pictures = 1;
            inst->video_pipeline_primed = true;
            ++inst->video_decoded;
        }
        if (pictures != 0u) hle::media::fork_movies().picture_decoded(inst->data);
        m.store32(au + hle::mpeg::AuSize, 0u);
        m.store32(init, pictures);
        ctx.set_gpr(2, 0u);
    });

    // sceMpeg::0x31BD0272 - sceMpegAvcCsc: the range in 8888 from the converted movie's current
    // picture (fork_movie.hpp), black without one.
    runtime.register_hle("sceMpeg", 0x31BD0272u, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &m = rt.memory();
        const std::uint32_t mpeg = ctx.gpr[4], source = ctx.gpr[5], range = ctx.gpr[6], stride = ctx.gpr[7], dest = ctx.gpr[8];
        const auto report = [&] { rt.stop(describe_avc_csc(mpeg, source, range, stride, dest)); };
        if (!m.contains(mpeg, 4u) || kernel.mpeg().find(m.load32(mpeg)) == nullptr || !m.contains(range, 16u)) return report();
        const std::uint32_t x = m.load32(range), y = m.load32(range + 4u), w = m.load32(range + 8u), h = m.load32(range + 12u);
        if (((x | y | w | h) & 15u) != 0u || w == 0u || h == 0u || w > 480u || h > 272u || x + w > stride || stride > 1024u)
            return report();
        const auto row_address = [&](std::uint32_t row) { return dest + ((y + row) * stride + x) * 4u; };
        for (std::uint32_t row = 0; row < h; ++row)
            if (m.raw_pointer(row_address(row), w * 4u) == nullptr) return report();
        constexpr std::uint32_t kBlack = 0xFF000000u; // opaque, as BlankMpegDecoder
        std::uint32_t pw = 0, ph = 0;
        const std::uint32_t *picture = hle::media::fork_movies().current_picture(m.load32(mpeg), pw, ph);
        const auto copy_start = std::chrono::steady_clock::now();
        if (picture != nullptr && x + w <= pw && y + h <= ph) { // the whole range inside the picture: row copies
            for (std::uint32_t row = 0; row < h; ++row) {
                std::memcpy(m.raw_pointer(row_address(row), w * 4u), picture + (y + row) * pw + x, w * 4u);
                m.notify_vram_write(row_address(row), w * 4u);
            }
            hle::media::note_picture_write(dest, stride); // after the rows: the renderer may read them at once
            hle::media::fork_movies().add_copy_time(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - copy_start).count()));
            ctx.set_gpr(2, 0u);
            return;
        }
        for (std::uint32_t row = 0; row < h; ++row) {
            std::uint8_t *p = m.raw_pointer(row_address(row), w * 4u);
            const std::uint32_t py = y + row;
            for (std::uint32_t i = 0; i < w; ++i) {
                const std::uint32_t px = x + i;
                const std::uint32_t c = picture != nullptr && px < pw && py < ph ? picture[py * pw + px] : kBlack;
                std::memcpy(p + i * 4u, &c, 4u);
            }
            m.notify_vram_write(row_address(row), w * 4u);
        }
        hle::media::note_picture_write(dest, stride);
        ctx.set_gpr(2, 0u);
    });

    // sceMpeg::0x21FF80E4 - sceMpegQueryStreamOffset: as core/src/hle/mpeg.cpp, plus binding the
    // instance to the converted movie whose PSMF header matches (fork_movie.hpp).
    runtime.register_hle("sceMpeg", 0x21FF80E4u, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &m = rt.memory();
        const auto header = ctx.gpr[5], out = ctx.gpr[6];
        if (!m.contains(header, 16u) || m.load32(header) != 0x464D5350u) { ctx.set_gpr(2, hle::mpeg::kErrorInvalidValue); return; } // "PSMF"
        m.store32(out, __builtin_bswap32(m.load32(header + 8u)));
        auto *inst = m.contains(ctx.gpr[4], 4u) ? kernel.mpeg().find(m.load32(ctx.gpr[4])) : nullptr;
        if (inst != nullptr) {
            inst->stream_size = __builtin_bswap32(m.load32(header + 12u));
            auto &movies = hle::media::fork_movies();
            std::string name;
            if (movies.bind(m, inst->data, header, hle::media::fork_media().dir(), &name) != nullptr)
                rt.event("movie_track", {{"handle", inst->data}}, name);
            else if (movies.stats().entries != 0u)
                rt.event("movie_unmatched", {{"handle", inst->data}, {"header", header}});
        }
        ctx.set_gpr(2, 0u);
    });

    // sceMpeg::0x606A4649 - sceMpegDelete: as core/src/hle/mpeg.cpp, plus ending the instance's converted movie.
    runtime.register_hle("sceMpeg", 0x606A4649u, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &m = rt.memory();
        if (auto *inst = m.contains(ctx.gpr[4], 4u) ? kernel.mpeg().find(m.load32(ctx.gpr[4])) : nullptr) {
            rt.event("mpeg_delete", {{"handle", inst->data}, {"video_aus", inst->video_decoded}, {"audio_aus", inst->audio_decoded}});
            hle::media::fork_movies().release(inst->data);
            kernel.mpeg().instances.erase(inst->data);
        }
        ctx.set_gpr(2, 0u);
    });

    // sceMpeg::0x800C44DF - sceMpegAtracDecode: as core/src/hle/mpeg.cpp, with the converted
    // movie's audio (2048 stereo samples per access unit) when there is one.
    runtime.register_hle("sceMpeg", 0x800C44DFu, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &m = rt.memory();
        auto *inst = m.contains(ctx.gpr[4], 4u) ? kernel.mpeg().find(m.load32(ctx.gpr[4])) : nullptr;
        const auto au = ctx.gpr[5], buffer = ctx.gpr[6];
        if (inst == nullptr || !m.contains(au, 24u) || m.load32(au + hle::mpeg::AuSize) == 0u) { ctx.set_gpr(2, hle::mpeg::kErrorAtracNoAu); return; }
        std::uint8_t *out = m.raw_pointer(buffer, 2048u * 4u);
        if (out == nullptr || (reinterpret_cast<std::uintptr_t>(out) & 1u) != 0u ||
            !hle::media::fork_movies().next_audio(inst->data, reinterpret_cast<std::int16_t *>(out)))
            kernel.mpeg().decoder->decode_audio(inst->last_audio, m, buffer);
        ++inst->audio_decoded;
        m.store32(au + hle::mpeg::AuSize, 0u);
        ctx.set_gpr(2, 0u);
    });

    // ThreadManForUser::0x383F7BCC - sceKernelTerminateDeleteThread: dormant threads, else report the call.
    runtime.register_hle("ThreadManForUser", 0x383F7BCCu, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &tm = kernel.threads();
        const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
        const auto *t = tm.get_thread(uid);
        if (uid != 0 && uid != tm.current_thread_id() && t != nullptr && t->status != hle::InternalThreadState::Dormant &&
            t->status != hle::InternalThreadState::Stopped) {
            rt.stop(describe_terminate_delete(uid, static_cast<std::uint32_t>(t->status)));
            return;
        }
        ctx.set_gpr(2, static_cast<std::uint32_t>(tm.delete_thread(uid)));
    });

    // sceUmdUser::0xE83742BA - sceUmdDeactivate
    runtime.register_hle("sceUmdUser", 0xE83742BAu, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &m = rt.memory();
        const std::uint32_t mode = ctx.gpr[4], drive_ptr = ctx.gpr[5];
        std::string drive;
        if (drive_ptr != 0u && m.contains(drive_ptr, 7u)) {
            try { drive = m.read_c_string(drive_ptr, 7u); } catch (const psprecomp::Error &) { drive.clear(); }
        }
        const std::uint32_t result = umd_deactivate_result(mode, drive);
        if (result == 0u) rebuild_umd_state(kernel, false, true);
        ctx.set_gpr(2, result);
        rt.event("umd_deactivate", {{"mode", mode}, {"drive_ptr", drive_ptr}, {"result", result}});
        if (result == 0u && kernel.umd().registered_callback() != 0)
            (void)kernel.threads().notify_callback(rt, kernel.umd().registered_callback(), 0x12u);
    });

    // sceUmdUser::0xBD2BDE07 - sceUmdUnRegisterUMDCallBack
    runtime.register_hle("sceUmdUser", 0xBD2BDE07u, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
        const bool registered = uid != 0 && uid == kernel.umd().registered_callback();
        if (registered) rebuild_umd_state(kernel, kernel.umd().activation_requested(), false);
        const std::uint32_t result = registered ? static_cast<std::uint32_t>(uid) : 0x80010016u;
        ctx.set_gpr(2, result);
        rt.event("umd_callback_unregister", {{"uid", ctx.gpr[4]}, {"result", result}});
    });

    // sceMpeg::0xF2930C9C - sceMpegAvcDecodeStopYCbCr: no pending picture, as sceMpegAvcDecodeStop.
    runtime.register_hle("sceMpeg", 0xF2930C9Cu, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &m = rt.memory();
        const std::uint32_t mpeg = ctx.gpr[4], buffer = ctx.gpr[5], status = ctx.gpr[6];
        if (!m.contains(mpeg, 4u) || kernel.mpeg().find(m.load32(mpeg)) == nullptr || !m.contains(status, 4u)) {
            rt.stop(describe_decode_stop_ycbcr(mpeg, buffer, status));
            return;
        }
        m.store32(status, 0u);
        ctx.set_gpr(2, 0u);
    });

    // sceDisplay::0x4D4E10EC - sceDisplayIsVblank
    runtime.register_hle("sceDisplay", 0x4D4E10ECu, [&kernel](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        ctx.set_gpr(2, display_is_vblank(kernel.threads().now(), hle::ThreadManager::kVblankPeriodUs));
    });

    // sceUmdUser::0x4A9E5E29 - sceUmdWaitDriveStatCB(stat, timeout)
    runtime.register_hle("sceUmdUser", 0x4A9E5E29u, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &tm = kernel.threads();
        const std::uint32_t stat = ctx.gpr[4], timeout = ctx.gpr[5];
        const auto result = umd_wait_result(stat, umd_drive_stat(kernel.umd()));
        rt.event("umd_wait_drive_stat_cb", {{"stat", stat}, {"timeout", timeout}, {"drive", umd_drive_stat(kernel.umd())},
                                            {"blocks", !result.has_value()}});
        if (result) { ctx.set_gpr(2, *result); return; }
        const auto deadline = timeout == 0u ? kNoDeadline : tm.now() + std::max<std::uint64_t>(timeout, kUmdMinWaitUs);
        tm.block_current(rt, ctx, WaitInfo{WaitType::Umd, kUmdWaitObject, deadline, 0u, true});
    });

    // sceUmdUser::0x6AF9B50A - sceUmdCancelWaitDriveStat
    runtime.register_hle("sceUmdUser", 0x6AF9B50Au, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        const auto woken = kernel.threads().wake_host_waiters(rt, WaitType::Umd, kUmdWaitObject, kUmdWaitCanceled);
        rt.event("umd_cancel_wait_drive_stat", {{"woken", static_cast<std::uint32_t>(woken)}});
        ctx.set_gpr(2, 0u);
    });

    // ModuleMgrForUser::0xF0A26395 - sceKernelGetModuleId
    runtime.register_hle("ModuleMgrForUser", 0xF0A26395u, [&kernel](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        const auto *module = module_at(kernel, ctx.gpr[31]);
        ctx.set_gpr(2, module != nullptr ? static_cast<std::uint32_t>(module->uid) : kMainModuleId);
    });

    // ModuleMgrForUser::0xD1FF982A - sceKernelStopModule(uid, args, argp, *status, option)
    runtime.register_hle("ModuleMgrForUser", 0xD1FF982Au, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &tm = kernel.threads();
        const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
        const std::uint32_t status_ptr = ctx.gpr[7], option = ctx.gpr[8];
        auto &modules = kernel.modules().modules;
        const auto it = modules.find(uid);
        if (it == modules.end()) { ctx.set_gpr(2, 0x8002012Eu); return; } // SCE_ERROR_KERNEL_UNKNOWN_MODULE
        auto &module = it->second;
        const auto report = [&](const char *why) {
            rt.stop("sceKernelStopModule(uid " + std::to_string(uid) + ", option " + psprecomp::hex32(option) + ") " + why +
                    " is not modelled yet");
        };
        const auto finish = [&](std::int32_t status) {
            if (status_ptr != 0u && rt.memory().contains(status_ptr, 4u)) rt.memory().store32(status_ptr, static_cast<std::uint32_t>(status));
            g_modules_stopped[uid] = true;
            rt.event("module_stop", {{"uid", static_cast<std::uint32_t>(uid)}, {"status", static_cast<std::uint32_t>(status)}}, module.name);
            ctx.set_gpr(2, 0u);
        };
        if (const auto thread = g_module_stop_threads.find(uid); thread != g_module_stop_threads.end()) {
            const auto *t = tm.get_thread(thread->second); // retried after module_stop's thread ended
            if (t == nullptr || t->status != hle::InternalThreadState::Stopped) return report("while module_stop runs");
            const auto status = t->exit_status;
            (void)tm.delete_thread(thread->second);
            g_module_stop_threads.erase(thread);
            return finish(status);
        }
        if (option != 0u) return report("with an SMOption");
        if (!module.started) return report("on a module that is not started");
        if (g_modules_stopped[uid]) return report("on a stopped module");
        if (module.hle_only) return finish(0);
        const auto layout = module_layout(rt, kernel, module);
        if (!layout) return report("(module file or info not found)");
        if (layout->module_stop == 0u) return finish(0);
        const auto thid = tm.create_thread("SceModmgrStop", layout->module_stop, 0x20, 0x40000u, 0x80000000u, 0u, rt.memory());
        if (thid < 0) { ctx.set_gpr(2, static_cast<std::uint32_t>(thid)); return; }
        g_module_stop_threads[uid] = thid;
        auto caller = ctx;
        const auto started = tm.start_thread(thid, ctx.gpr[5], ctx.gpr[6], rt.memory(), caller, false, module.gp);
        if (started < 0) { g_module_stop_threads.erase(uid); ctx.set_gpr(2, static_cast<std::uint32_t>(started)); return; }
        hle::WaitInfo wait{hle::WaitType::ThreadEnd, thid};
        wait.retry = true; // as upstream's sceKernelStartModule
        tm.block_current(rt, ctx, wait);
    });

    // ModuleMgrForUser::0x2E0911AA - sceKernelUnloadModule(uid)
    runtime.register_hle("ModuleMgrForUser", 0x2E0911AAu, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
        auto &modules = kernel.modules().modules;
        const auto it = modules.find(uid);
        if (it == modules.end()) { ctx.set_gpr(2, 0x8002012Eu); return; }
        auto &module = it->second;
        if (module.started && !g_modules_stopped[uid]) {
            rt.stop("sceKernelUnloadModule(uid " + std::to_string(uid) + ") of a started module that was not stopped is not modelled yet");
            return;
        }
        if (!module.hle_only) {
            const auto layout = module_layout(rt, kernel, module);
            auto &m = rt.memory();
            const std::uint32_t text = layout ? layout->text_size : 0u;
            for (std::uint32_t off = 0; off + 4u <= module.size; off += 4u) m.store32(module.base + off, off < text ? 0x0000004Du : 0xFFFFFFFFu);
            (void)kernel.sysmem().free_partition_memory(module.memory_block);
        }
        rt.event("module_unload", {{"uid", static_cast<std::uint32_t>(uid)}}, module.name);
        g_modules_stopped.erase(uid);
        modules.erase(it);
        ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
    });

    // ModuleMgrForUser::0x8F2DF740 - sceKernelStopUnloadSelfModuleWithStatus(exitStatus, args, argp, *status, option)
    runtime.register_hle("ModuleMgrForUser", 0x8F2DF740u, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        if (const auto *module = module_at(kernel, ctx.gpr[31])) {
            rt.stop("sceKernelStopUnloadSelfModuleWithStatus from module " + module->name + " is not modelled yet");
            return;
        }
        rt.stop("Game exit: the main module called sceKernelStopUnloadSelfModuleWithStatus(" +
                std::to_string(static_cast<std::int32_t>(ctx.gpr[4])) + ")");
    });

    // LoadExecForUser::0x05572A5F - sceKernelExitGame
    runtime.register_hle("LoadExecForUser", 0x05572A5Fu, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &) {
        rt.stop("Game exit: sceKernelExitGame");
    });

    // IoFileMgrForUser::0x779103A0 - sceIoRename(old, new)
    runtime.register_hle("IoFileMgrForUser", 0x779103A0u, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &m = rt.memory();
        const std::uint32_t from_ptr = ctx.gpr[4], to_ptr = ctx.gpr[5];
        if (!m.contains(from_ptr, 1u) || !m.contains(to_ptr, 1u)) { // a PSP crashes on NULL (rename.c)
            rt.stop("sceIoRename(" + psprecomp::hex32(from_ptr) + ", " + psprecomp::hex32(to_ptr) + ") with an invalid path pointer");
            return;
        }
        const auto from = m.read_c_string(from_ptr, 1024u), to = m.read_c_string(to_ptr, 1024u);
        rt.event("io_rename", {}, from + " -> " + to);
        if (const auto result = io_rename_static_result(from, to)) { ctx.set_gpr(2, *result); return; }
        if (from.find(':') != std::string::npos) {
            const auto resolved = kernel.io().resolve(from);
            if (!resolved || !resolved->fs->stat(resolved->path)) { ctx.set_gpr(2, 0x80010002u); return; } // no such source
        }
        rt.stop("sceIoRename(\"" + from + "\", \"" + to + "\") of an existing or relative file is not modelled yet");
    });

    // sceUtility MsgDialog (see fork_kernel.hpp).
    constexpr std::uint32_t kUtilityBadState = 0x80110001u;
    runtime.register_hle("sceUtility", 0x2AD8E239u, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { // InitStart
        auto &m = rt.memory();
        const std::uint32_t p = ctx.gpr[4];
        if (g_msgdialog.status != 0) { ctx.set_gpr(2, kUtilityBadState); return; }
        if (kernel.savedata().status() != 0) { rt.stop("sceUtilityMsgDialogInitStart while the savedata dialog is active is not modelled yet"); return; }
        const std::uint32_t size = m.contains(p, 4u) ? m.load32(p) : 0u;
        if (size < 572u || !m.contains(p, size)) { rt.stop("sceUtilityMsgDialogInitStart(" + psprecomp::hex32(p) + ") with size " + std::to_string(size) + " is not modelled yet"); return; }
        g_msgdialog = MsgDialogState{1, false, false, p, size};
        const std::uint32_t mode = m.load32(p + 52u), error = m.load32(p + 56u), options = m.load32(p + 572u);
        std::string text;
        for (std::uint32_t i = 0; i < 512u; ++i) { const auto ch = m.load8(p + 60u + i); if (ch == 0u) break; text.push_back(static_cast<char>(ch)); }
        rt.event("msgdialog", {{"mode", mode}, {"error", error}, {"options", options}, {"size", size}}, text);
        std::cout << "[MSGDIALOG] mode=" << mode << " error=" << psprecomp::hex32(error) << " options=" << psprecomp::hex32(options)
                  << " text=\"" << text << "\"\n";
        ctx.set_gpr(2, 0u);
    });
    runtime.register_hle("sceUtility", 0x9A1C91D7u, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { // GetStatus
        auto &d = g_msgdialog;
        const auto now = kernel.threads().vblank_count();
        if (d.status == 1) {
            if (d.init_reported) {
                d.status = 2;
                d.visible_since = now;
                if (rt.memory().contains(d.param + 48u, 4u)) rt.memory().store32(d.param + 48u, 0u); // "Result -> 00000000"
            }
            d.init_reported = true;
        } else if (d.status == 2 && now - d.visible_since >= kMsgDialogStallVblanks) {
            rt.stop("sceUtilityMsgDialog stays VISIBLE: no sceUtilityMsgDialogUpdate call in " + std::to_string(kMsgDialogStallVblanks) +
                    " vblanks (the dialog only advances in Update)");
            return;
        } else if (d.status == 4) {
            if (d.shutdown_reported) d.status = 0;
            else { d.shutdown_reported = true; ctx.set_gpr(2, 4u); return; }
        }
        ctx.set_gpr(2, static_cast<std::uint32_t>(d.status));
    });
    runtime.register_hle("sceUtility", 0x95FC253Bu, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { // Update
        auto &d = g_msgdialog;
        ctx.set_gpr(2, 0u);
        if (d.status != 2) return;
        auto &m = rt.memory();
        const auto button = msgdialog_default_button(m.load32(d.param + 572u));
        if (d.size >= 580u) m.store32(d.param + 576u, button);
        m.store32(d.param + 28u, 0u); // base.result [INFERRED]
        rt.event("msgdialog_done", {{"button", button}});
        d.status = 3;
    });
    runtime.register_hle("sceUtility", 0x67AF3428u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { // ShutdownStart
        if (g_msgdialog.status != 3) { ctx.set_gpr(2, 0x80110001u); return; }
        g_msgdialog.status = 4;
        g_msgdialog.shutdown_reported = false;
        ctx.set_gpr(2, 0u);
    });

    // ThreadManForUser::0x30FD48F0 - sceKernelPollEventFlag(evid, bits, mode, *outBits). Upstream
    // registers its poll under 0x30FD7D3A, which no reference knows (PSPSDK ThreadManForUser.S,
    // uOFW threadman exports.exp and PPSSPP all give 0x30FD48F0), so the game's import was
    // unbound (PC run 20 / New 3DS run 10, thread BGM). Same ThreadManager implementation.
    runtime.register_hle("ThreadManForUser", 0x30FD48F0u, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        ctx.set_gpr(2, static_cast<std::uint32_t>(kernel.threads().poll_event_flag(rt.memory(), static_cast<std::int32_t>(ctx.gpr[4]),
                                                                                    ctx.gpr[5], ctx.gpr[6], ctx.gpr[7])));
    });

    // SysMemUserForUser::0xF919F628 - sceKernelTotalFreeMemSize. Upstream's sysmem.cpp registers it
    // under 0xF9100EB9, which no reference knows (PSPSDK SysMemUserForUser.S, PPSSPP, uOFW sysmem
    // exports.exp: 0xF919F628). Persona does not import it; registered so the call is reachable.
    runtime.register_hle("SysMemUserForUser", 0xF919F628u, [&kernel](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        ctx.set_gpr(2, kernel.sysmem().total_free_memory());
    });

    // ThreadManForUser::0x809CE29B - sceKernelExitDeleteThread(status)
    runtime.register_hle("ThreadManForUser", 0x809CE29Bu, [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &tm = kernel.threads();
        const auto uid = tm.current_thread_id();
        if (!tm.exit_current_thread(static_cast<std::int32_t>(ctx.gpr[4]), ctx, rt)) return;
        (void)tm.delete_thread(uid);
    });

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
