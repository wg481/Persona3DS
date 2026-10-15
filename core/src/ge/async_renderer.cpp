// GE back end on a worker thread: see async_renderer.hpp.
//
// The queue is a single-producer / single-consumer byte ring. Records are
// [Header][payload] rounded to 8 bytes; a Wrap record sends the reader back
// to the start when a record does not fit before the end. head_ (producer)
// and tail_ (consumer) are published with release / read with acquire, so
// everything written before a record -- its payload, and guest memory the
// game wrote before the GE command -- is visible to the worker.
#include "p3p3ds/ge/async_renderer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace p3p3ds::ge {
namespace {

// P3P3DS: a State record is [StateDelta][count x (index, value)]: the 32-bit words of GeRegisters
// but the CLUT (sent on its own, only when it changed) that differ from the worker's copy. Word
// index: reg 0..255, bone 256..351, world 352..363, view 364..375, tgen 376..387, proj 388..403.
// It replaced a full 1.6 KB snapshot copied twice per state change (school dungeon: one per draw,
// about 15k/s, the main core's `ge` share; docs/CURRENT_STATE.md 2026-10-13).
struct StateDelta {
    std::uint64_t state_version;
    std::uint32_t count, pad;
};
constexpr std::uint32_t kBoneAt = 256u, kWorldAt = kBoneAt + 96u, kViewAt = kWorldAt + 12u, kTgenAt = kViewAt + 12u,
                        kProjAt = kTgenAt + 12u, kStateEnd = kProjAt + 16u;
static_assert(kBoneAt == GeRegisters::kWordBone && kWorldAt == GeRegisters::kWordWorld && kViewAt == GeRegisters::kWordView &&
              kTgenAt == GeRegisters::kWordTgen && kProjAt == GeRegisters::kWordProj && kStateEnd == GeRegisters::kWords);

std::uint32_t get_word(const GeRegisters &r, std::uint32_t index) {
    if (index < kBoneAt) return r.reg[index];
    const float *f = index < kWorldAt ? &r.bone[index - kBoneAt]
                   : index < kViewAt  ? &r.world[index - kWorldAt]
                   : index < kTgenAt  ? &r.view[index - kViewAt]
                   : index < kProjAt  ? &r.tgen[index - kTgenAt]
                                      : &r.proj[index - kProjAt];
    std::uint32_t w;
    std::memcpy(&w, f, 4u);
    return w;
}

// Appends (base + i, word) for every word of `now` that differs from `sent`, and updates `sent`.
template <class T, std::size_t N>
std::uint32_t diff_words(const std::array<T, N> &now, std::uint32_t *sent, std::uint32_t base, std::uint32_t *out, std::uint32_t n) {
    static_assert(sizeof(T) == 4u);
    const auto *cur = reinterpret_cast<const std::uint8_t *>(now.data());
    for (std::uint32_t i = 0; i < N; i += 8u) {
        const std::uint32_t len = std::min<std::uint32_t>(8u, static_cast<std::uint32_t>(N) - i);
        if (std::memcmp(cur + i * 4u, sent + i, len * 4u) == 0) continue;
        for (std::uint32_t j = i; j < i + len; ++j) {
            std::uint32_t w;
            std::memcpy(&w, cur + j * 4u, 4u);
            if (w == sent[j]) continue;
            sent[j] = w;
            out[2u * n] = base + j;
            out[2u * n + 1u] = w;
            ++n;
        }
    }
    return n;
}

void set_word(GeRegisters &r, std::uint32_t index, std::uint32_t value) {
    if (index < kBoneAt) { r.reg[index] = value; return; }
    float *f = index < kWorldAt ? &r.bone[index - kBoneAt]
             : index < kViewAt  ? &r.world[index - kWorldAt]
             : index < kTgenAt  ? &r.view[index - kViewAt]
             : index < kProjAt  ? &r.tgen[index - kTgenAt]
                                : &r.proj[index - kProjAt];
    std::memcpy(f, &value, 4u);
}
struct ClutPayload {
    std::array<std::uint32_t, 256> clut;
    std::uint32_t clut_words;
    std::uint32_t pad;
    std::uint64_t clut_hash;
};
struct DrawPayload {
    psprecomp::GuestMemory *memory;
    std::uint32_t prim, count, vertex, index;
};
struct TransferPayload {
    psprecomp::GuestMemory *memory;
};
struct CallPayload {
    std::function<void(GeRenderer &)> *fn;
};
struct TaskPayload { // P3P3DS: AsyncRenderer::post_task
    AsyncRenderer::TaskFn fn;
    void *ctx, *p;
    std::uint32_t kind, a, b;
};

constexpr std::uint32_t round8(std::size_t n) { return static_cast<std::uint32_t>((n + 7u) & ~std::size_t{7}); }
constexpr std::size_t kMinQueue = 64u << 10;

} // namespace

AsyncRenderer::AsyncRenderer(std::unique_ptr<GeRenderer> inner, std::size_t queue_bytes, WorkerHooks hooks, bool drain_on_sync)
    : inner_(std::move(inner)), hooks_(std::move(hooks)), drain_on_sync_(drain_on_sync) {
    if (!hooks_.start) return;
    ring_.resize(round8(std::max(queue_bytes, kMinQueue)));
    threaded_ = hooks_.start(&AsyncRenderer::worker_entry, this);
    if (!threaded_) {
        ring_.clear();
        ring_.shrink_to_fit();
    }
}

AsyncRenderer::~AsyncRenderer() {
    if (!threaded_) return;
    push(Type::Stop, nullptr, 0);
    drain();
    if (hooks_.join) hooks_.join();
}

void AsyncRenderer::worker_entry(void *self) { static_cast<AsyncRenderer *>(self)->worker_loop(); }

void AsyncRenderer::worker_loop() {
    const auto cap = static_cast<std::uint32_t>(ring_.size());
    bool busy = false;
    std::uint64_t busy_start = 0;
    for (;;) {
        std::uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) {
            if (busy) { // busy time is taken per stretch of work, not per record
                stats_.busy.fetch_add(now() - busy_start, std::memory_order_relaxed);
                busy = false;
            }
            pause();
            continue;
        }
        if (!busy) {
            busy = true;
            busy_start = now();
        }
        Header header;
        std::memcpy(&header, &ring_[t], sizeof header);
        const std::uint8_t *payload = &ring_[t] + sizeof(Header);
        bool stop = false;
        switch (header.type) {
        case Type::Wrap:
            tail_.store(0u, std::memory_order_release);
            continue;
        case Type::State: {
            StateDelta s; // P3P3DS: changed words only
            std::memcpy(&s, payload, sizeof s);
            const std::uint8_t *words = payload + sizeof s;
            for (std::uint32_t k = 0; k < s.count; ++k) {
                std::uint32_t pair[2];
                std::memcpy(pair, words + k * 8u, sizeof pair);
                set_word(worker_regs_, pair[0], pair[1]);
            }
            worker_regs_.state_version = s.state_version;
            break;
        }
        case Type::Clut: {
            ClutPayload c;
            std::memcpy(&c, payload, sizeof c);
            worker_regs_.clut = c.clut;
            worker_regs_.clut_words = c.clut_words;
            worker_regs_.clut_hash = c.clut_hash;
            break;
        }
        case Type::Draw: {
            DrawPayload d;
            std::memcpy(&d, payload, sizeof d);
            inner_->draw(*d.memory, worker_regs_, static_cast<Prim>(d.prim), d.count, d.vertex, d.index);
            break;
        }
        case Type::Transfer: {
            TransferPayload d;
            std::memcpy(&d, payload, sizeof d);
            inner_->transfer(*d.memory, worker_regs_);
            break;
        }
        case Type::Sync:
            inner_->sync();
            break;
        case Type::Call: {
            CallPayload c;
            std::memcpy(&c, payload, sizeof c);
            (*c.fn)(*inner_);
            delete c.fn;
            break;
        }
        case Type::Task: { // P3P3DS
            TaskPayload k;
            std::memcpy(&k, payload, sizeof k);
            k.fn(k.ctx, k.kind, k.a, k.b, k.p);
            break;
        }
        case Type::Stop:
            stop = true;
            break;
        }
        t += header.bytes;
        if (t == cap) t = 0;
        if (stop) stats_.busy.fetch_add(now() - busy_start, std::memory_order_relaxed);
        tail_.store(t, std::memory_order_release);
        if (stop) return;
    }
}

// Room for one record of `bytes` (a multiple of 8, far below the queue size);
// waits while the worker has not freed enough. One byte always stays free, so
// head == tail means empty.
std::uint8_t *AsyncRenderer::reserve(std::uint32_t bytes) {
    const auto cap = static_cast<std::uint32_t>(ring_.size());
    const std::uint32_t h = head_.load(std::memory_order_relaxed);
    std::uint64_t wait_start = 0;
    bool waited = false;
    for (;;) {
        const std::uint32_t t = tail_.load(std::memory_order_acquire);
        const std::uint32_t free = (t + cap - h - 1u) % cap;
        if (h + bytes <= cap) {
            if (free >= bytes) {
                pending_ = (h + bytes) % cap;
                break;
            }
        } else if (free >= (cap - h) + bytes) {
            const Header wrap{Type::Wrap, cap - h};
            std::memcpy(&ring_[h], &wrap, sizeof wrap);
            pending_ = bytes;
            if (waited) stats_.full_wait += now() - wait_start;
            return ring_.data();
        }
        if (!waited) {
            waited = true;
            wait_start = now();
        }
        pause();
    }
    if (waited) stats_.full_wait += now() - wait_start;
    return &ring_[h];
}

void AsyncRenderer::publish() { head_.store(pending_, std::memory_order_release); }

void AsyncRenderer::push(Type type, const void *payload, std::uint32_t payload_bytes) {
    const std::uint32_t bytes = round8(sizeof(Header) + payload_bytes);
    std::uint8_t *at = reserve(bytes);
    const Header header{type, bytes};
    std::memcpy(at, &header, sizeof header);
    if (payload_bytes != 0u) std::memcpy(at + sizeof header, payload, payload_bytes);
    publish();
    ++stats_.records;
}

// The worker's register copy follows the game's only when the render state
// changed (state_version; CLUT loads change it too), the CLUT only when its
// hash changed.
void AsyncRenderer::push_state(const GeRegisters &regs) {
    // P3P3DS: GeManager leaves state_version when a word is rewritten with its value or is list control
    // (docs/CURRENT_STATE.md); such words still reach the worker's copy while any is marked.
    if (state_sent_ && regs.state_version == sent_version_) {
        std::uint32_t any = 0u;
        if (regs.dirty_tracked) for (const std::uint32_t d : regs.dirty) any |= d;
        if (any == 0u) return;
    }
    // P3P3DS: only the words that differ from the worker's copy (sent_state_, all zero like
    // worker_regs_ at the start), behind a StateDelta header in delta_.
    static_assert(kStateWords == kStateEnd && sizeof(StateDelta) == 16u);
    std::uint32_t *sent = sent_state_.data(), *out = delta_.data() + 4u;
    std::uint32_t n = 0;
    if (regs.dirty_tracked) { // the words GeManager marked as written: no pass over all 1.6 KB
        for (std::uint32_t k = 0; k < regs.dirty.size(); ++k) {
            for (std::uint32_t bits = regs.dirty[k]; bits != 0u; bits &= bits - 1u) {
                const std::uint32_t i = k * 32u + static_cast<std::uint32_t>(std::countr_zero(bits));
                const std::uint32_t w = get_word(regs, i);
                sent[i] = w;
                out[2u * n] = i;
                out[2u * n + 1u] = w;
                ++n;
            }
            regs.dirty[k] = 0u;
        }
    } else {
        n = diff_words(regs.reg, sent, 0u, out, 0u);
        n = diff_words(regs.bone, sent + kBoneAt, kBoneAt, out, n);
        n = diff_words(regs.world, sent + kWorldAt, kWorldAt, out, n);
        n = diff_words(regs.view, sent + kViewAt, kViewAt, out, n);
        n = diff_words(regs.tgen, sent + kTgenAt, kTgenAt, out, n);
        n = diff_words(regs.proj, sent + kProjAt, kProjAt, out, n);
    }
    const StateDelta head{regs.state_version, n, 0u};
    std::memcpy(delta_.data(), &head, sizeof head);
    push(Type::State, delta_.data(), static_cast<std::uint32_t>(sizeof head) + n * 8u);
    state_sent_ = true;
    sent_version_ = regs.state_version;
    ++stats_.states;
    if (clut_sent_ && regs.clut_hash == sent_clut_hash_) return;
    ClutPayload c;
    c.clut = regs.clut;
    c.clut_words = regs.clut_words;
    c.pad = 0;
    c.clut_hash = regs.clut_hash;
    push(Type::Clut, &c, sizeof c);
    clut_sent_ = true;
    sent_clut_hash_ = regs.clut_hash;
    ++stats_.cluts;
}

void AsyncRenderer::draw(psprecomp::GuestMemory &memory, const GeRegisters &regs, Prim prim, std::uint32_t count,
                         std::uint32_t vertex_address, std::uint32_t index_address) {
    if (!threaded_) {
        inner_->draw(memory, regs, prim, count, vertex_address, index_address);
        return;
    }
    push_state(regs);
    const DrawPayload d{&memory, static_cast<std::uint32_t>(prim), count, vertex_address, index_address};
    push(Type::Draw, &d, sizeof d);
    ++stats_.draws;
}

void AsyncRenderer::transfer(psprecomp::GuestMemory &memory, const GeRegisters &regs) {
    if (!threaded_) {
        inner_->transfer(memory, regs);
        return;
    }
    push_state(regs);
    const TransferPayload d{&memory};
    push(Type::Transfer, &d, sizeof d);
}

void AsyncRenderer::sync() {
    if (!threaded_) {
        inner_->sync();
        return;
    }
    push(Type::Sync, nullptr, 0);
    if (drain_on_sync_) drain();
}

void AsyncRenderer::drain() {
    if (!threaded_) return;
    if (tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_relaxed)) return;
    ++stats_.drains;
    const std::uint64_t start = now();
    while (tail_.load(std::memory_order_acquire) != head_.load(std::memory_order_relaxed)) pause();
    stats_.drain_wait += now() - start;
}

void AsyncRenderer::post(std::function<void(GeRenderer &)> fn) {
    if (!threaded_) {
        fn(*inner_);
        return;
    }
    const CallPayload c{new std::function<void(GeRenderer &)>(std::move(fn))};
    push(Type::Call, &c, sizeof c);
    ++stats_.calls;
}

// P3P3DS: see the header.
void AsyncRenderer::post_task(TaskFn fn, void *ctx, std::uint32_t kind, std::uint32_t a, std::uint32_t b, void *p) {
    if (!threaded_) {
        fn(ctx, kind, a, b, p);
        return;
    }
    const TaskPayload k{fn, ctx, p, kind, a, b};
    push(Type::Task, &k, sizeof k);
}

} // namespace p3p3ds::ge
