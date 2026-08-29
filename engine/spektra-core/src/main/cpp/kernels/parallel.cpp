/*
 * Spektrafilm for Android — native engine: deterministic parallel-for (worker
 * count resolution). GPLv3. Film modeling powered by spektrafilm (GPLv3).
 */
#include "kernels/parallel.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

namespace spk {

namespace {

// Env flag helper: 1/on/true/yes enable, anything else (including unset) disable.
bool env_enabled(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    return std::strcmp(v, "1") == 0 || std::strcmp(v, "on") == 0 ||
           std::strcmp(v, "true") == 0 || std::strcmp(v, "yes") == 0;
}

// Programmatic big-core override (parallel_set_big_cores). -1 defers to the env
// var, which is the default and keeps every existing host/CI invocation exactly
// as it was. Relaxed ordering is enough: a stale read costs at most one render
// on the previous policy, and the policy never affects the arithmetic.
std::atomic<int> g_big_cores_mode{-1};

// Bumped on every policy change. A thread whose pin latch predates the current
// value re-applies the mask, which is what makes turning the setting OFF at
// runtime actually unpin an already-pinned worker.
std::atomic<unsigned> g_big_cores_generation{0};

// Current policy: the programmatic override when set, else the env var.
bool big_cores_enabled() {
    const int mode = g_big_cores_mode.load(std::memory_order_relaxed);
    if (mode >= 0) return mode != 0;
    return env_enabled("SPK_BIG_CORES");
}

#if defined(__linux__)
// Read one unsigned from a sysfs file; 0 when the file is absent or unreadable
// (a kernel without cpufreq, or a core that is offline right now).
unsigned read_uint_file(const char* path) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) return 0;
    unsigned long v = 0;
    const int got = std::fscanf(f, "%lu", &v);
    std::fclose(f);
    return got == 1 ? static_cast<unsigned>(v) : 0u;
}
#endif

// Classify cores by cpuinfo_max_freq and build the "big" mask. Computed once:
// the topology cannot change under us, and this is sysfs I/O we must not do per
// render. Returns the number of big cores; fills `mask` when non-null.
int detect_big_cores(
#if defined(__linux__)
    cpu_set_t* mask
#else
    void* mask
#endif
) {
#if defined(__linux__)
    // The ratio is a knob so the on-device run can sweep it: 0.80 keeps prime +
    // performance on the usual 3-cluster ARM parts and drops the efficiency
    // cluster; 1.0 would keep only the prime core.
    double ratio = 0.80;
    if (const char* rv = std::getenv("SPK_BIG_CORE_RATIO")) {
        const double r = std::atof(rv);
        if (r > 0.0 && r <= 1.0) ratio = r;
    }
    const int ncpu = static_cast<int>(std::thread::hardware_concurrency());
    if (ncpu <= 1) return 0;

    std::vector<unsigned> freq(static_cast<size_t>(ncpu), 0u);
    unsigned fmax = 0;
    char path[128];
    for (int c = 0; c < ncpu; ++c) {
        std::snprintf(path, sizeof(path),
                      "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        freq[static_cast<size_t>(c)] = read_uint_file(path);
        if (freq[static_cast<size_t>(c)] > fmax) fmax = freq[static_cast<size_t>(c)];
    }
    // No cpufreq data at all (common on x86 hosts and emulators) — every core is
    // equal as far as we can tell, so there is no "big" set to pin to.
    if (fmax == 0) return 0;

    const unsigned threshold = static_cast<unsigned>(ratio * fmax);
    int nbig = 0;
    if (mask) CPU_ZERO(mask);
    for (int c = 0; c < ncpu; ++c) {
        if (freq[static_cast<size_t>(c)] >= threshold) {
            if (mask) CPU_SET(c, mask);
            ++nbig;
        }
    }
    // Pinning to every core is the same as not pinning; report 0 so callers can
    // skip the syscall and the worker-count cap.
    return nbig == ncpu ? 0 : nbig;
#else
    (void)mask;
    return 0;
#endif
}

}  // namespace

int parallel_big_core_count() {
    // The sysfs probe is cached because the topology cannot change under us; the
    // POLICY is read live so a runtime toggle takes effect without a restart.
    // (Previously the probe was folded into the policy in one `static const`,
    // which pinned the answer to whatever the env said on the very first call.)
    if (!big_cores_enabled()) return 0;
    static const int n = detect_big_cores(nullptr);
    return n;
}

void parallel_set_big_cores(int mode) {
    const int m = mode < 0 ? -1 : (mode != 0 ? 1 : 0);
    const int prev = g_big_cores_mode.exchange(m, std::memory_order_relaxed);
    // Only a real change costs a re-pin; a settings screen that rewrites the same
    // value on every recomposition must not force a syscall per render.
    if (prev != m) g_big_cores_generation.fetch_add(1u, std::memory_order_relaxed);
}

void parallel_pin_to_big_cores() {
#if defined(__linux__)
    // One latch per thread, versioned by the policy generation: workers spawned
    // after this inherit the mask, so the pool is covered by the orchestrator's
    // single call, and a thread that somehow starts its own parallel_for pays one
    // syscall and no more. Storing the generation (rather than a bare bool) is
    // what lets a mid-session toggle re-apply: a thread pinned under generation N
    // re-evaluates once the counter moves to N+1.
    static thread_local unsigned applied = 0u;  // 0 = never applied
    const unsigned gen = g_big_cores_generation.load(std::memory_order_relaxed) + 1u;
    if (applied == gen) return;
    applied = gen;

    // The mask this thread had before we first touched it, so that turning the
    // setting OFF restores the scheduler's own placement instead of leaving the
    // pool stuck on the big cluster. Captured lazily: workers are spawned fresh
    // per fork-join, so probing affinity on threads we are never going to pin
    // would put two syscalls on the DEFAULT (feature-off) path of every render.
    static thread_local cpu_set_t original;
    static thread_local bool restorable = false;  // captured AND since pinned

    if (!big_cores_enabled()) {
        if (restorable) {
            (void)sched_setaffinity(0, sizeof(original), &original);
            restorable = false;
        }
        return;
    }
    cpu_set_t mask;
    if (detect_big_cores(&mask) <= 0) return;
    const bool captured = sched_getaffinity(0, sizeof(original), &original) == 0;
    // A failure here is not an error worth propagating: the render is still
    // correct on whatever cores the scheduler picks, just possibly slower.
    if (sched_setaffinity(0, sizeof(mask), &mask) == 0 && captured) restorable = true;
#endif
}

int parallel_num_threads() {
    // Explicit override (tests, or a host that wants to pin the worker count).
    if (const char* env = std::getenv("SPK_NUM_THREADS")) {
        const int n = std::atoi(env);
        if (n >= 1) return n;
    }
    // With big-core pinning on, sizing the pool to hardware_concurrency() would
    // oversubscribe the mask — more workers than runnable cores, so chunks queue
    // behind each other and the join waits for two serial chunks instead of one.
    // Cap to the big-core count. Output is unchanged: the chunk split is a pure
    // function of (count, nthreads) and every worker count is byte-identical.
    const int nbig = parallel_big_core_count();
    if (nbig > 0) return nbig;
    const unsigned hw = std::thread::hardware_concurrency();
    return hw == 0 ? 1 : static_cast<int>(hw);
}

int parallel_min_chunk() {
    // Test-only override. Unset (the shipping default) yields kParallelMinChunk, so
    // production behaviour — and therefore every golden — is unchanged.
    if (const char* env = std::getenv("SPK_PARALLEL_MIN_CHUNK")) {
        const int n = std::atoi(env);
        if (n >= 1) return n;
    }
    return kParallelMinChunk;
}

}  // namespace spk
