#pragma once
// Persona3DS fork: one background thread on the New 3DS system core (core 1),
// for work that may wait (movie decode-ahead, fork_movie.hpp
// set_background_executor). Included by main.cpp only.
// [VERIFIED] libctru include/3ds/thread.h (threadCreate): "Processor #1 is the
// system core. If APT_SetAppCpuTimeLimit is used, it is possible to create a
// single thread on this core." The application gets the percentage it sets of
// that core (30 % here, as probe_cores in main.cpp); the system runs the rest.
// One task at a time: post() returns false while a task runs, and the caller
// then does the work itself.
#include <3ds.h>

#include <atomic>

namespace p3p3ds::n3ds {

class Core1Worker {
public:
    bool start(s32 priority) {
        if (thread_ != nullptr) return true;
        if (R_FAILED(APT_SetAppCpuTimeLimit(30))) return false;
        LightEvent_Init(&wake_, RESET_ONESHOT);
        thread_ = threadCreate(&Core1Worker::entry, this, 32u << 10, priority, 1, false);
        return thread_ != nullptr;
    }
    bool post(void (*task)(void *), void *arg) {
        if (thread_ == nullptr || busy_.exchange(true, std::memory_order_acq_rel)) return false;
        task_ = task;
        arg_ = arg;
        LightEvent_Signal(&wake_);
        return true;
    }
    void stop() {
        if (thread_ == nullptr) return;
        quit_.store(true, std::memory_order_release);
        LightEvent_Signal(&wake_);
        threadJoin(thread_, U64_MAX);
        threadFree(thread_);
        thread_ = nullptr;
    }

private:
    static void entry(void *self) {
        auto &w = *static_cast<Core1Worker *>(self);
        for (;;) {
            LightEvent_Wait(&w.wake_);
            if (w.busy_.load(std::memory_order_acquire) && w.task_ != nullptr) { // a posted task runs before a stop
                auto *task = w.task_;
                void *arg = w.arg_;
                w.task_ = nullptr;
                task(arg);
                w.busy_.store(false, std::memory_order_release);
            }
            if (w.quit_.load(std::memory_order_acquire)) return;
        }
    }
    Thread thread_{};
    LightEvent wake_{};
    std::atomic<bool> busy_{false}, quit_{false};
    void (*task_)(void *){};
    void *arg_{};
};

} // namespace p3p3ds::n3ds
