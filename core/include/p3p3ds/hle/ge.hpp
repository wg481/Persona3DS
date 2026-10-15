#pragma once
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <map>
#include <deque>
#include <string>
#include <memory>
#include <vector>
#include "p3p3ds/ge/renderer.hpp"
namespace psprecomp { class Runtime; class GuestMemory; struct AllegrexContext; }
namespace p3p3ds::ge { class AsyncRenderer; }
namespace p3p3ds { class KernelState; }
namespace p3p3ds::hle {
enum class GeStatus : std::uint32_t { Completed=0, Queued=1, Running=2, Stalled=3, Paused=4 };
struct GeListInfo {
    std::uint32_t id{}, list_address{}, pc{}, stall_address{}, opt_param{};
    std::uint32_t context_address{}, stack_count{}, stack_address{};
    std::int32_t callback_id{-1};
    GeStatus status{GeStatus::Queued};
    std::uint64_t commands{}, completions{};
    std::uint32_t previous{}, finish_token{};
    std::vector<std::pair<std::uint32_t,std::uint32_t>> call_stack; // (return pc, offset)
    bool bbox_failed{};
};
struct GeState {
    std::array<std::uint32_t,256> registers{};
    std::array<std::array<std::uint32_t,128>,5> matrices{};
    std::array<unsigned,5> matrix_index{};
    std::uint32_t vertex{}, index{}, offset{};
    std::uint32_t relative(std::uint32_t arg) const { return ((((registers[0x10]&0xF0000)<<8)|arg)+offset)&0x0FFFFFFF; }
    std::uint32_t color_address() const { return 0x04000000u | (registers[0x9C]&0x1FFFF0); }
    std::uint32_t depth_address() const { return 0x04000000u | (registers[0x9E]&0x1FFFF0); }
    unsigned color_stride() const { return registers[0x9D]&0x7FC; }
    unsigned depth_stride() const { return registers[0x9F]&0x7FC; }
    unsigned format() const { return registers[0xD2]&3; }
};
struct GeCallback { std::uint32_t signal{}, signal_arg{}, finish{}, finish_arg{}; };
class GeManager {
public:
    GeManager() = default;
    GeManager(const GeManager &) = delete;
    GeManager &operator=(const GeManager &) = delete;
    ~GeManager(); // P3P3DS: waits for display lists still running on the GE worker
    static constexpr std::uint32_t kEdramBase=0x04000000u, kEdramSize=0x00200000u;
    std::uint32_t edram_base() const { return kEdramBase; }
    std::uint32_t edram_size() const { return kEdramSize; }
    std::int32_t enqueue_list(psprecomp::Runtime &, std::uint32_t, std::uint32_t, int, std::uint32_t, bool head=false);
    std::int32_t update_stall(psprecomp::Runtime &, int, std::uint32_t);
    std::int32_t sync(psprecomp::Runtime &, int id, int mode, bool all=false);
    void pump(psprecomp::Runtime &);
    std::int32_t set_callback(psprecomp::GuestMemory &, std::uint32_t);
    std::int32_t unset_callback(int);
    const GeListInfo *find(int id) const;
    const GeListInfo &last_list() const { return lists_.rbegin()->second; }
    std::uint64_t enqueue_count() const { return enqueued_; }
    // Completed lists kept for sceGeListSync/status queries; older ones are
    // dropped (the PSP recycles its display-list IDs). [INFERRED] 64 slots.
    static constexpr std::size_t kKeptLists = 64;
    const GeState &state() const { return state_; }
    const std::map<int,GeListInfo> &lists() const { return lists_; }
    bool writer_observed() const { return writer_observed_; }
    std::uint32_t writer_pc() const { return writer_pc_; }
    void report() const;
    void set_renderer(std::unique_ptr<ge::GeRenderer> renderer) { async_=nullptr; renderer_ = std::move(renderer); }
    // P3P3DS: display lists run on the GE worker thread (New 3DS core 2), as the PSP GE runs them
    // beside the CPU. `async` is the threaded renderer set with set_renderer (null: lists run on the
    // game's thread, as before). The game's thread then only queues work (enqueue, stall updates);
    // what must run there -- list completion and its finish callback, SIGNAL, errors -- the worker
    // leaves at the list's pc and service() runs it. Everything else of GeManager is touched by the
    // game's thread only while the worker is idle (quiesce). docs/CURRENT_STATE.md 2026-10-13.
    void set_async_executor(psprecomp::Runtime &rt, ge::AsyncRenderer *async);
    bool async_execution() const { return async_ != nullptr; }
    bool async_pending() const { return async_pending_.load(std::memory_order_acquire); }
    void service(psprecomp::Runtime &rt); // game's thread: run what the worker left, if anything
    void quiesce(psprecomp::Runtime &rt); // game's thread: worker idle, nothing left for this thread
    // Debug fast-forward (PC runner --render-from): display lists still run
    // (state, vertex pointers, callbacks), but no pixels are produced.
    bool skip_rasterization{};
    // Debug (PC runner --draw-log): one line per PRIM with its state and
    // screen bounds while set; null in normal runs.
    std::FILE *draw_log{};
    // Per-PRIM census of the GE state the game uses (feature -> draw count),
    // to prioritise renderer work by observed workload. A dozen string-keyed
    // map updates per draw: off in the production runtime (3DS).
#if defined(PSPRECOMP_NO_FRONTIER_DIAGNOSTICS)
    bool census{false};
#else
    bool census{true};
#endif
    const std::map<std::string, std::uint64_t> &feature_counts() const { return features_; }
    ge::GeRenderer &renderer() { return *renderer_; }
    const ge::GeRegisters &registers() const { return regs_; }
private:
    ge::GeRegisters regs_;
    std::unique_ptr<ge::GeRenderer> renderer_{std::make_unique<ge::SoftwareRenderer>()};
    void execute(psprecomp::Runtime &, GeListInfo &, std::uint32_t word);
    GeState state_;
    std::map<int,GeListInfo> lists_;
    std::map<std::string, std::uint64_t> features_;
    void count_features(std::uint32_t prim_type);
    void log_draw(psprecomp::GuestMemory &mem, std::uint32_t prim_type, std::uint32_t count);
    std::map<int,GeCallback> callbacks_;
    std::deque<int> queue_;
    int next_id_{1}, next_callback_{0};
    std::uint64_t enqueued_{};
    bool writer_observed_{};
    std::uint64_t enqueue_end_{}; // profile clock when the last enqueue returned (0: none pending)
    std::uint32_t writer_pc_{};
    void deliver_finish_callback(psprecomp::Runtime &, const GeListInfo &, std::uint32_t end_pc);
    // P3P3DS: worker execution (set_async_executor)
    void pump_impl(psprecomp::Runtime &, bool worker);
    bool worker_needs_main(const GeListInfo &, std::uint32_t word) const;
    static void worker_task(void *self, std::uint32_t kind, std::uint32_t a, std::uint32_t b, void *p);
    ge::AsyncRenderer *async_{};
    psprecomp::Runtime *async_rt_{};
    ge::GeRenderer *active_{}; // the renderer execute() calls: renderer_, or the worker's own
    std::atomic<bool> async_pending_{};
    // P3P3DS: the newest stall update not yet taken by the worker, (id << 32) | stall, 0: none. Updates of the
    // same list while it waits replace it instead of queueing another task (update_stall, worker_task kind 4).
    std::atomic<std::uint64_t> stall_slot_{};
    bool in_service_{};
};
void register_ge_module(psprecomp::Runtime &, KernelState &);
} // namespace p3p3ds::hle
