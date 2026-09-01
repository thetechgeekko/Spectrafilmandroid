/*
 * Spektrafilm for Android — native engine: deterministic parallel-for.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm.
 *
 * A minimal fork-join helper for the engine's embarrassingly-parallel maps:
 * the per-pixel stages (expose / scan / print_expose), the DIR-coupler and
 * density-curve interpolations, the spatial kernels (Gaussian/exponential
 * filters, diffusion convolution — parallel_for_weighted below), the 3D-LUT
 * apply, and the element-wise mix/copy passes around them. The range
 * [begin, end) is split into contiguous, disjoint chunks whose boundaries
 * depend ONLY on (count, threads) — never on thread scheduling. Because each
 * item is computed independently and written to a disjoint output location,
 * the result is BIT-IDENTICAL to the serial loop for any thread count, which
 * preserves the bit-exact parity gate.
 *
 * Stochastic grain does NOT use parallel_for: model/grain.cpp runs its own
 * fixed-block scheme (deterministic per-block seeding + a dynamic
 * atomic-counter worker pool) so the seeded RNG stays reproducible per block
 * regardless of which worker runs it — gated by test_parallel scenario 5.
 */
#ifndef SPK_KERNELS_PARALLEL_H
#define SPK_KERNELS_PARALLEL_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// OPT-IN Intel oneTBB backend (CMake SPK_USE_TBB, default OFF). When enabled the
// per-pixel stages are scheduled through tbb::parallel_for instead of the hand-rolled
// fork-join below. The chunk boundaries are still the SAME pure function of
// (count, threads) and a simple_partitioner is used, so the result stays
// BIT-IDENTICAL across thread counts — the thread-invariance the parity gate requires.
// Default builds (no TBB) are unchanged and carry no dependency.
#ifdef SPK_USE_TBB
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/simple_partitioner.h>
#endif

namespace spk {

// Worker count for parallel_for. Honours the SPK_NUM_THREADS environment
// override (clamped to >= 1) when set; otherwise std::thread::hardware_concurrency()
// (falling back to 1 when the platform reports 0). Read fresh each call so tests
// can vary it via setenv to assert thread-count invariance.
int parallel_num_threads();

// OPT-IN big.LITTLE affinity (perf-lab). Android schedulers are free to park the
// render pool on efficiency cores, and a fork-join is only as fast as its slowest
// chunk: one worker on a 2.0 GHz little core stalls the join for everyone, so the
// whole map runs at little-core speed no matter how many big cores are idle. This
// pins the calling thread to the cores whose cpuinfo_max_freq is at least
// SPK_BIG_CORE_RATIO (default 0.80) of the fastest core's. Spawned workers INHERIT
// the mask on Linux, so one syscall per thread covers the pool.
//
// OFF unless SPK_BIG_CORES is 1/on/true — an untouched build is bit-for-bit and
// scheduler-for-scheduler what it was.
//
// OUTPUT IS UNAFFECTED BY CONSTRUCTION: affinity moves *where* a chunk runs, never
// what it computes. Chunk boundaries stay a pure function of (count, nthreads), and
// when the mask changes the worker count the thread-invariance contract already
// covers it — that is exactly what test_parallel asserts.
//
// Idempotent per thread (a thread_local latch), so calling it on every parallel_for
// costs one predicted branch after the first.
void parallel_pin_to_big_cores();

// Number of cores the ratio classifies as "big", or 0 when detection failed, the
// platform has no affinity API, or pinning is currently off. Diagnostic /
// worker-count capping.
int parallel_big_core_count();

// Programmatic override of the SPK_BIG_CORES env gate, so the app can offer this
// as a setting: the engine is loaded into a running process that cannot setenv
// its own pre-main environment. mode is 1 (on), 0 (off), or -1 (defer to the env
// var, the default).
//
// Safe to call between renders, including mid-session:
//   * the sysfs topology probe is cached, so a toggle costs no I/O;
//   * each toggle bumps a generation counter, and a thread whose latch predates
//     the current generation re-applies the mask on its next parallel_for, so
//     turning the setting OFF really unpins (the mask captured before the first
//     pin is restored) rather than leaving the pool stuck on the big cluster.
//
// OUTPUT IS UNAFFECTED: this changes only which cores run a chunk and how many
// workers split it, and every worker count is byte-identical by the chunking
// contract above — which is what test_parallel asserts.
void parallel_set_big_cores(int mode);

// Minimum pixels per worker. Below this the range runs serially to avoid thread
// spawn overhead dominating (e.g. small preview renders).
constexpr int kParallelMinChunk = 8192;

// Effective minimum chunk. Honours the SPK_PARALLEL_MIN_CHUNK environment override
// (clamped to >= 1) when set; otherwise kParallelMinChunk. Unset — the shipping
// default — is byte-identical to the previous behaviour.
//
// Why this exists: a fixture SMALLER than the minimum chunk collapses to exactly one
// chunk at every thread count, so parallel_for takes the serial path and a
// thread-invariance test over it cannot observe a chunking bug — the gate passes
// vacuously. The 64x64 (4096-pixel) parity fixture is such a case against the 8192
// default. Setting this override lets the test force genuine multi-chunk execution
// on the existing fixture, so 1-vs-N really compares split work against serial work.
int parallel_min_chunk();

// Cooperative cancellation for long native maps. The callback is deliberately
// owned and invoked by the thread that constructs the scope. Android's JNI
// adapter holds that caller thread's JNIEnv, so render-pool workers may observe
// only the local atomic stop flag and must never invoke the callback themselves.
using ParallelCancelCheck = int (*)(void*);

class ParallelCancellationScope {
public:
    ParallelCancellationScope(ParallelCancelCheck check, void* context) noexcept;
    ~ParallelCancellationScope();

    ParallelCancellationScope(const ParallelCancellationScope&) = delete;
    ParallelCancellationScope& operator=(const ParallelCancellationScope&) = delete;

private:
    ParallelCancelCheck check_;
    void* context_;
    bool cancelled_ = false;
    ParallelCancellationScope* previous_ = nullptr;

    friend bool parallel_cancellation_active() noexcept;
    friend bool parallel_cancellation_latched() noexcept;
    friend bool parallel_cancellation_requested() noexcept;
    friend bool parallel_cancellation_poll(ParallelCancelCheck, void*) noexcept;
};

// Marker thrown only on the scope-owning thread after every worker has joined.
// The run_* orchestration boundary catches it and maps it to SPK_ERR_CANCELLED,
// so partially-filled stage buffers can never flow into dependent arithmetic.
struct ParallelCancelled final {};

bool parallel_cancellation_active() noexcept;
bool parallel_cancellation_latched() noexcept;
bool parallel_cancellation_requested() noexcept;
bool parallel_cancellation_poll(ParallelCancelCheck check,
                                void* context) noexcept;

// At most this many declared pixel-equivalents are completed by the caller
// between callback polls. Weighted maps clamp to one row/column when a single
// unit itself exceeds this value.
constexpr int kCancellationPollWork = 1024;

namespace detail {

// std::thread terminates the process when an exception escapes its entry point.
// Capture the first failure, ask sibling chunks not yet started to stop, join the
// whole pool, and only then rethrow on the dispatch owner.
class ParallelFailureState final {
public:
    void capture_current() noexcept {
        stop_.store(true, std::memory_order_release);
        const std::exception_ptr captured = std::current_exception();
        while (lock_.test_and_set(std::memory_order_acquire)) {}
        if (!failure_) failure_ = captured;
        lock_.clear(std::memory_order_release);
    }

    bool stop_requested() const noexcept {
        return stop_.load(std::memory_order_acquire);
    }

    void rethrow_if_captured() const {
        // Called only by the owner after every successfully-created worker has
        // joined, so no writer can still race this read.
        if (failure_) std::rethrow_exception(failure_);
    }

private:
    std::atomic<bool> stop_{false};
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    std::exception_ptr failure_;
};

// Dispatch [begin, end) as ceil-divided chunks across nthreads workers (the
// caller has already resolved and clamped nthreads to >= 2). Chunk boundaries
// are a pure function of (count, nthreads) — never of thread scheduling.
template <typename Body>
void parallel_dispatch(int begin, int end, int nthreads, const Body& body) {
    const int count = end - begin;
    // Ceil-divide so the chunk boundaries are fixed by (count, nthreads) alone.
    const int chunk = (count + nthreads - 1) / nthreads;

    // OPT-IN (SPK_BIG_CORES): keep the pool off the efficiency cluster. No-op
    // unless enabled, and never affects chunk boundaries or arithmetic.
    parallel_pin_to_big_cores();

#ifdef SPK_USE_TBB
    // oneTBB backend: schedule the SAME fixed chunks via tbb::parallel_for with a
    // simple_partitioner (no dynamic re-splitting), so each chunk maps 1:1 to a fixed
    // [cb, ce) and the output is identical regardless of how TBB distributes them.
    const int nchunks = (count + chunk - 1) / chunk;
    tbb::parallel_for(
        tbb::blocked_range<int>(0, nchunks, 1),
        [&](const tbb::blocked_range<int>& r) {
            for (int t = r.begin(); t < r.end(); ++t) {
                const int cb = begin + t * chunk;
                if (cb >= end) continue;
                const int ce = std::min(cb + chunk, end);
                body(cb, ce);
            }
        },
        tbb::simple_partitioner());
    return;
#else
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(nthreads - 1));
    ParallelFailureState failure;
    try {
        for (int t = 1; t < nthreads; ++t) {
            if (failure.stop_requested()) break;
            const int cb = begin + t * chunk;
            if (cb >= end) break;
            const int ce = std::min(cb + chunk, end);
            workers.emplace_back([&body, &failure, cb, ce]() {
                if (failure.stop_requested()) return;
                try {
                    body(cb, ce);
                } catch (...) {
                    failure.capture_current();
                }
            });
        }
    } catch (...) {
        // Includes std::thread construction/system failures after earlier
        // workers have already launched.
        failure.capture_current();
    }
    // The calling thread runs the first chunk while the workers run theirs.
    if (!failure.stop_requested()) {
        try {
            body(begin, std::min(begin + chunk, end));
        } catch (...) {
            failure.capture_current();
        }
    }
    for (auto& w : workers) w.join();
    failure.rethrow_if_captured();
#endif  // SPK_USE_TBB
}

// Cancellation-aware dispatch. The null-callback route never enters here and
// therefore retains parallel_dispatch above byte-for-byte. With a callback,
// every fixed worker range is subdivided into bounded deterministic blocks.
// Workers only read `stop`; callback polling remains on the invoking thread.
template <typename Body>
void parallel_dispatch_cancellable(int begin, int end, int nthreads,
                                   int block_items, const Body& body) {
    if (block_items < 1) block_items = 1;
    if (parallel_cancellation_requested()) throw ParallelCancelled{};

    const auto run_blocks = [&](int cb, int ce, std::atomic<bool>* stop,
                                bool poll) {
        for (int b = cb; b < ce; b += block_items) {
            if (stop && stop->load(std::memory_order_relaxed)) return;
            if (poll && parallel_cancellation_requested()) {
                if (stop) stop->store(true, std::memory_order_relaxed);
                return;
            }
            const int be = std::min(b + block_items, ce);
            body(b, be);
        }
    };

    if (nthreads <= 1) {
        run_blocks(begin, end, nullptr, true);
        if (parallel_cancellation_latched()) throw ParallelCancelled{};
        return;
    }

    const int count = end - begin;
    const int chunk = (count + nthreads - 1) / nthreads;
    std::atomic<bool> stop{false};
    std::atomic<int> workers_left{0};
    ParallelFailureState failure;
    std::mutex wait_mutex;
    std::condition_variable wait_cv;
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(nthreads - 1));
    for (int t = 1; t < nthreads; ++t) {
        const int cb = begin + t * chunk;
        if (cb >= end) break;
        const int ce = std::min(cb + chunk, end);
        workers_left.fetch_add(1, std::memory_order_relaxed);
        try {
            workers.emplace_back([&, cb, ce]() {
                try {
                    run_blocks(cb, ce, &stop, false);
                } catch (...) {
                    failure.capture_current();
                    stop.store(true, std::memory_order_release);
                }
                workers_left.fetch_sub(1, std::memory_order_release);
                wait_cv.notify_one();
            });
        } catch (...) {
            workers_left.fetch_sub(1, std::memory_order_relaxed);
            failure.capture_current();
            stop.store(true, std::memory_order_release);
            break;
        }
    }

    bool cancelled = false;
    if (!failure.stop_requested()) {
        try {
            run_blocks(begin, std::min(begin + chunk, end), &stop, true);
        } catch (const ParallelCancelled&) {
            // A nested owner-thread map may surface the same marker. Stop this
            // dispatch too, join its workers, then rethrow below.
            cancelled = true;
            stop.store(true, std::memory_order_relaxed);
        } catch (...) {
            failure.capture_current();
            stop.store(true, std::memory_order_release);
        }
    }

    // Normally equal-sized chunks finish together. If a worker has a heavier
    // row/column, keep sampling from the owner thread while it drains instead
    // of blocking in join with a stale JNI cancellation signal.
    while (!stop.load(std::memory_order_relaxed) &&
           workers_left.load(std::memory_order_acquire) > 0) {
        if (parallel_cancellation_requested()) {
            cancelled = true;
            stop.store(true, std::memory_order_relaxed);
            break;
        }
        std::unique_lock<std::mutex> lock(wait_mutex);
        wait_cv.wait_for(lock, std::chrono::milliseconds(2), [&]() {
            return workers_left.load(std::memory_order_acquire) == 0;
        });
    }
    for (auto& worker : workers) worker.join();
    failure.rethrow_if_captured();
    if (cancelled || parallel_cancellation_latched()) throw ParallelCancelled{};
}

// Dynamic worker pool for stages whose independently-seeded work units have
// highly variable cost (currently film grain). The caller owns the atomic work
// queue; this helper owns every thread lifetime and the exception boundary.
//
// With an active cancellation scope the owner thread remains an orchestrator so
// only it samples the thread-affine callback. Otherwise the owner participates
// in the work exactly like the fixed-chunk dispatcher. In both modes a worker
// exception or a partial std::thread construction failure stops the remaining
// work, joins every successfully-created thread, then rethrows on the owner.
// `thread_factory` is injectable solely so the host regression can prove the
// partial-construction path without exhausting process thread resources.
template <typename Worker, typename ThreadFactory>
void parallel_dispatch_dynamic_with_factory(int nthreads, const Worker& worker,
                                            const ThreadFactory& thread_factory) {
    if (nthreads < 1) nthreads = 1;
    const bool cancellable = parallel_cancellation_active();
    if (!cancellable && nthreads == 1) {
        worker(nullptr);
        return;
    }
    if (cancellable && parallel_cancellation_requested()) {
        throw ParallelCancelled{};
    }

    parallel_pin_to_big_cores();
    std::atomic<bool> stop{false};
    std::atomic<int> workers_left{0};
    ParallelFailureState failure;
    std::mutex wait_mutex;
    std::condition_variable wait_cv;
    std::vector<std::thread> workers;
    const int background_workers = cancellable ? nthreads : nthreads - 1;
    workers.reserve(static_cast<size_t>(background_workers));

    for (int t = 0; t < background_workers; ++t) {
        workers_left.fetch_add(1, std::memory_order_relaxed);
        try {
            workers.emplace_back(thread_factory([&]() {
                try {
                    worker(&stop);
                } catch (...) {
                    failure.capture_current();
                    stop.store(true, std::memory_order_release);
                }
                workers_left.fetch_sub(1, std::memory_order_release);
                wait_cv.notify_one();
            }));
        } catch (...) {
            workers_left.fetch_sub(1, std::memory_order_relaxed);
            failure.capture_current();
            stop.store(true, std::memory_order_release);
            break;
        }
    }

    bool cancelled = false;
    if (cancellable) {
        while (!stop.load(std::memory_order_acquire) &&
               workers_left.load(std::memory_order_acquire) > 0) {
            if (parallel_cancellation_requested()) {
                cancelled = true;
                stop.store(true, std::memory_order_release);
                break;
            }
            std::unique_lock<std::mutex> lock(wait_mutex);
            wait_cv.wait_for(lock, std::chrono::milliseconds(2), [&]() {
                return workers_left.load(std::memory_order_acquire) == 0 ||
                       stop.load(std::memory_order_acquire);
            });
        }
    } else if (!failure.stop_requested()) {
        try {
            worker(&stop);
        } catch (...) {
            failure.capture_current();
            stop.store(true, std::memory_order_release);
        }
    }

    for (auto& thread : workers) thread.join();
    failure.rethrow_if_captured();
    if (cancelled || (cancellable && parallel_cancellation_latched())) {
        throw ParallelCancelled{};
    }
}

template <typename Worker>
void parallel_dispatch_dynamic(int nthreads, const Worker& worker) {
    const auto thread_factory = [](auto&& entry) {
        return std::thread(std::forward<decltype(entry)>(entry));
    };
    parallel_dispatch_dynamic_with_factory(nthreads, worker, thread_factory);
}

}  // namespace detail

// Split [begin, end) into up to parallel_num_threads() contiguous, disjoint
// chunks and run body(chunk_begin, chunk_end) for each — workers on their own
// threads, the first chunk on the calling thread. Chunk boundaries are a pure
// function of (count, threads), so for a body that writes only disjoint outputs
// the result is independent of thread count and scheduling.
//
// body MUST be free of cross-iteration shared mutable state.
template <typename Body>
void parallel_for(int begin, int end, const Body& body) {
    const int count = end - begin;
    if (count <= 0) return;

    int nthreads = parallel_num_threads();
    if (nthreads > 1) {
        const int min_chunk = parallel_min_chunk();
        const int max_by_work = (count + min_chunk - 1) / min_chunk;
        nthreads = std::min(nthreads, max_by_work < 1 ? 1 : max_by_work);
    }
    if (parallel_cancellation_active()) {
        detail::parallel_dispatch_cancellable(
            begin, end, nthreads, kCancellationPollWork, body);
        return;
    }
    if (nthreads <= 1) {
        body(begin, end);
        return;
    }
    detail::parallel_dispatch(begin, end, nthreads, body);
}

// parallel_for for index ranges whose items are HEAVIER than one pixel — a row
// of an image pass, a column of an IIR sweep, a row of a direct convolution.
// `unit_work` is the number of pixel-equivalents each index covers (e.g. the
// image width for a row-parallel pass), so the serial-below-min-chunk clamp
// measures the real work: clamping by the bare item count would collapse a
// few-thousand-row image to a single chunk and silently serialize it. Chunk
// boundaries remain a pure function of (count, threads) exactly as in
// parallel_for, so a body whose items are computed independently with disjoint
// writes stays byte-identical for any worker count.
template <typename Body>
void parallel_for_weighted(int begin, int end, long long unit_work,
                           const Body& body) {
    const int count = end - begin;
    if (count <= 0) return;

    int nthreads = parallel_num_threads();
    if (nthreads > 1) {
        const long long uw = unit_work < 1 ? 1 : unit_work;
        const long long min_chunk = parallel_min_chunk();
        const long long total_work = static_cast<long long>(count) * uw;
        const long long max_by_work = (total_work + min_chunk - 1) / min_chunk;
        if (max_by_work < static_cast<long long>(nthreads)) {
            nthreads = static_cast<int>(max_by_work < 1 ? 1 : max_by_work);
        }
    }
    if (parallel_cancellation_active()) {
        const long long uw = unit_work < 1 ? 1 : unit_work;
        const long long block = kCancellationPollWork / uw;
        detail::parallel_dispatch_cancellable(
            begin, end, nthreads,
            static_cast<int>(block < 1 ? 1 : block), body);
        return;
    }
    if (nthreads <= 1) {
        body(begin, end);
        return;
    }
    detail::parallel_dispatch(begin, end, nthreads, body);
}

}  // namespace spk

#endif  // SPK_KERNELS_PARALLEL_H
