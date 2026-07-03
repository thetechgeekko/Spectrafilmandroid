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
 * A minimal fork-join helper for the engine's embarrassingly-parallel per-pixel
 * stages (expose / scan / print_expose). The range [begin, end) is split into
 * contiguous, disjoint chunks whose boundaries depend ONLY on (count, threads) —
 * never on thread scheduling. Because each pixel is computed independently and
 * written to a disjoint output location, the result is BIT-IDENTICAL to the
 * serial loop for any thread count, which preserves the bit-exact parity gate.
 *
 * NOT for stochastic stages (grain): those walk a seeded RNG in pixel order and
 * must stay serial to remain reproducible.
 */
#ifndef SPK_KERNELS_PARALLEL_H
#define SPK_KERNELS_PARALLEL_H

#include <algorithm>
#include <atomic>
#include <thread>
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

// Minimum pixels per worker. Below this the range runs serially to avoid thread
// spawn overhead dominating (e.g. small preview renders).
constexpr int kParallelMinChunk = 8192;

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
        const int max_by_work = (count + kParallelMinChunk - 1) / kParallelMinChunk;
        nthreads = std::min(nthreads, max_by_work < 1 ? 1 : max_by_work);
    }
    if (nthreads <= 1) {
        body(begin, end);
        return;
    }

    // Ceil-divide so the chunk boundaries are fixed by (count, nthreads) alone.
    const int chunk = (count + nthreads - 1) / nthreads;

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
    for (int t = 1; t < nthreads; ++t) {
        const int cb = begin + t * chunk;
        if (cb >= end) break;
        const int ce = std::min(cb + chunk, end);
        workers.emplace_back([&body, cb, ce]() { body(cb, ce); });
    }
    // The calling thread runs the first chunk while the workers run theirs.
    body(begin, std::min(begin + chunk, end));
    for (auto& w : workers) w.join();
#endif  // SPK_USE_TBB
}

// Run body(i) for each i in [0, ntasks) across up to parallel_num_threads()
// threads, pulling task indices off a shared atomic counter (dynamic load
// balancing — the extra task when ntasks doesn't divide the worker count lands
// on whichever thread frees up first). Unlike parallel_for there is NO min-chunk
// gate: this is for a SMALL number of COARSE, heavyweight tasks (e.g. the grain
// RNG streams), where per-thread spawn cost is negligible.
//
// DETERMINISM CONTRACT: task-to-thread assignment must not affect the result, so
// the caller MUST have each task write only DISJOINT outputs; any cross-task
// reduction is the caller's job to do serially AFTER this returns. Given that,
// the output is byte-identical for any thread count — the thread-invariance the
// parity gate requires. body MUST be free of cross-task shared mutable state.
template <typename Body>
void parallel_tasks(int ntasks, const Body& body) {
    if (ntasks <= 0) return;
    int nthreads = std::min(parallel_num_threads(), ntasks);
    if (nthreads <= 1) {
        for (int i = 0; i < ntasks; ++i) body(i);
        return;
    }
    std::atomic<int> next{0};
    auto worker = [&]() {
        int i;
        while ((i = next.fetch_add(1, std::memory_order_relaxed)) < ntasks)
            body(i);
    };
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(nthreads - 1));
    for (int t = 1; t < nthreads; ++t) workers.emplace_back(worker);
    worker();  // the calling thread participates
    for (auto& w : workers) w.join();
}

}  // namespace spk

#endif  // SPK_KERNELS_PARALLEL_H
