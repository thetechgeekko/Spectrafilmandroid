/*
 * Spektrafilm for Android — per-stage/per-filter render timing (DIAGNOSTIC).
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Wall-clock accumulators for the render pipeline's stages and filters, surfaced
 * to logcat once per render so we can see WHERE the interactive latency goes
 * (issue #146/#152 — the device validation found the scan stage is not the
 * preview bottleneck; grain/halation/filming and per-source LUT bakes are).
 *
 * OBSERVATION ONLY — reading the clock and adding a double NEVER changes a
 * rendered pixel, so this is outside the parity contract (the 38-gate suite is
 * unaffected). Slots are written on the ORCHESTRATOR thread only: each timer
 * brackets a whole stage/filter call, and the parallel_for worker fan-out lives
 * *inside* that call, so there is no cross-thread write and no synchronization.
 * A render is serialized end-to-end (the app's SingleFlight; parity tests are
 * single-threaded per case), so the process-global accumulator is reset at the
 * top of each top-level render (run_scan_film / run_print) and read after it
 * returns.
 */
#ifndef SPK_RUNTIME_STAGE_TIMER_H
#define SPK_RUNTIME_STAGE_TIMER_H

#include <chrono>
#include <cstdio>

namespace spk {

enum StageSlot {
    STG_PREPROCESS = 0,   // geometry crop/rescale + AE metering
    STG_TC_LUT,           // filming Hanatos tc_lut build (cold-start heavy)
    STG_PRINT_DIGEST,     // print neutral-CC digest (print route)
    STG_FILMING_EXPOSE,   // expose(): spectral upsample -> camera raw -> tc_lut apply -> log_raw
    STG_HIGHLIGHT_BOOST,  // filming pre-clip highlight boost
    STG_DIFFUSION,        // camera optical diffusion filter (Black Pro-Mist)
    STG_LENS_BLUR,        // camera lens blur (Gaussian)
    STG_HALATION,         // in-emulsion scatter + back-reflection halation
    STG_DEVELOP,          // exposure->density curve interpolation
    STG_DIR_COUPLERS,     // DIR-coupler density correction (spatial)
    STG_GRAIN,            // stochastic AgX grain particle model
    STG_PRINT_EXPOSE,     // enlarger expose + print develop (print route)
    STG_SCAN,             // scan(): whole stage, density -> display RGB (CPU or GPU)
    STG_SCAN_SPATIAL,     // SUB-MEASURE of scan: gamut compress / lens blur / unsharp
    STG_GLARE,            // SUB-MEASURE of scan: viewing-glare FIELD build (print
                          // route). The per-pixel add is folded into the scan loops
                          // and is not separable from them.
    STG_COUNT
};

inline const char* stage_name(int s) {
    switch (s) {
        case STG_PREPROCESS:    return "preprocess";
        case STG_TC_LUT:        return "tc_lut_build";
        case STG_PRINT_DIGEST:  return "print_digest";
        case STG_FILMING_EXPOSE:return "filming_expose";
        case STG_HIGHLIGHT_BOOST:return "highlight_boost";
        case STG_DIFFUSION:     return "camera_diffusion";
        case STG_LENS_BLUR:     return "lens_blur";
        case STG_HALATION:      return "halation";
        case STG_DEVELOP:       return "develop";
        case STG_DIR_COUPLERS:  return "dir_couplers";
        case STG_GRAIN:         return "grain";
        case STG_PRINT_EXPOSE:  return "print_expose";
        case STG_SCAN:          return "scan";
        case STG_SCAN_SPATIAL:  return "scan_spatial";
        case STG_GLARE:         return "glare_field";
        default:                return "?";
    }
}

// Process-global accumulator (ms). An inline function's local static has a
// single instance across the whole program, so this is shared across all TUs.
inline double* stage_timings() {
    static double t[STG_COUNT] = {0};
    return t;
}

inline void stage_timings_reset() {
    double* t = stage_timings();
    for (int i = 0; i < STG_COUNT; ++i) t[i] = 0.0;
}

// Format the non-zero slots as "stage=1.23 other=4.56" into buf; returns the
// count of bytes written (excluding the NUL).
//
// TWO THINGS THE OUTPUT DOES NOT SAY, and both have already misled a reading:
//
//  1. Zero slots are SKIPPED, so a gated-off filter is invisible rather than shown
//     as 0. An export profile taken at default settings therefore says nothing
//     about camera_diffusion (Black Pro-Mist), lens_blur or glare_field — all
//     three default off. Absence here is not evidence of cheapness.
//  2. SUB-MEASURE slots are NESTED inside their parent, so the printed numbers
//     MUST NOT simply be added up. scan_spatial and glare_field both sit inside
//     the scan bracket, so a naive total double-counts them. Summing the line and
//     comparing against the native call's wall clock produced an apparent
//     "timers exceed wall clock by 40-75 ms"; subtracting the nested
//     scan_spatial turns that into the ~363 ms of JNI entry, param marshalling
//     and result allocation that genuinely is outside every stage.
inline int stage_timings_format(char* buf, int cap) {
    const double* t = stage_timings();
    int off = 0;
    for (int i = 0; i < STG_COUNT && off < cap - 1; ++i) {
        if (t[i] <= 0.0) continue;
        int n = std::snprintf(buf + off, static_cast<size_t>(cap - off),
                              "%s%s=%.1f", off ? " " : "", stage_name(i), t[i]);
        if (n < 0) break;
        off += n;
    }
    if (cap > 0) buf[off < cap ? off : cap - 1] = '\0';
    return off;
}

// RAII bracket: adds its lifetime (ms) to `slot` on destruction. Bracket a whole
// stage/filter call on the orchestrator thread.
struct ScopedStage {
    int slot;
    std::chrono::steady_clock::time_point t0;
    explicit ScopedStage(int s)
        : slot(s), t0(std::chrono::steady_clock::now()) {}
    ~ScopedStage() {
        const double dt = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        stage_timings()[slot] += dt;
    }
    ScopedStage(const ScopedStage&) = delete;
    ScopedStage& operator=(const ScopedStage&) = delete;
};

}  // namespace spk

#endif  // SPK_RUNTIME_STAGE_TIMER_H
