/*
 * Spektrafilm for Android — host test for the GPU preview fast-path (GPU M1, #146).
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * Separate GPU gate (not in the 39-test engine-parity table): exercising the real Vulkan branch
 * needs an ICD, which CI runners don't guarantee. Locally, run it under
 * SwiftShader (Chromium bundles one):
 *   VK_ICD_FILENAMES=<...>/vk_swiftshader_icd.json /tmp/test_gpu_host <asset_dir>
 * WITHOUT an ICD (or when built without SPK_ENABLE_VULKAN) the test still
 * passes: it then proves the fallback path — gpu_preview=1 must be a strict
 * no-op (byte-identical to gpu_preview=0) when no GPU is available.
 *
 * What it proves, per route (scan_film, D50 print, and Kodak 2383/2393 K75P
 * print), per kernel class:
 *  - LINEAR kernel (production defaults: scanner unsharp (0.7, 0.7) is ON, so
 *    interactive previews hit the linear kernel + CPU plane/encode tail).
 *  - FUSED kernel (sharpening zeroed: the full-chain sRGB shader path).
 *  1. LAW (#149): spk_simulate (EXPORT) with gpu_preview=1 is BYTE-IDENTICAL to
 *     the same export with gpu_preview=0 — the toggle cannot reach export.
 *  2. The GPU preview render is within the oracle tolerance band (max_abs <=
 *     1e-4) of the CPU EXPORT render on the same pixels (input chosen small
 *     enough that spk_simulate_preview skips the downscale, so grids match).
 *     For scale: the forced scanner LUT is profile/domain dependent at LUT17:
 *     the locked D50 case is <=5e-5, while K75P 2383/2393 are about
 *     0.0040/0.0073 vs direct. GPU preview bypasses that LUT when it engages.
 *  3. Warm-host determinism: repeated GPU preview renders are byte-identical.
 *  4. The one-time self-check ran and passed (spk_gpu_scan_state() == 1) when a
 *     GPU is present; K75P runs first so that global check cannot be satisfied
 *     only by legacy D50 tables. Without a GPU the state stays 0 and nothing
 *     engaged.
 *
 * Build (host) — full source set + -DSPK_ENABLE_VULKAN=1 -lvulkan, from the cpp
 * root (see CLAUDE.md for the base compile line; SRC already includes gpu/).
 */
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "gpu/tests/pointwise_chain_oracle.h"
#include "gpu/vulkan_compute.h"
#include "profiles/json_min.h"
#include "runtime/stage_timer.h"
#include "spektra.h"

// Host-only test seams intentionally kept out of the public/JNI ABI.
void spk_test_pointwise_set_fault(int fault);
double spk_test_pointwise_direct_gain(void);
size_t spk_test_last_asset_max_bytes(void);
size_t spk_test_last_asset_bytes_allocated(void);
uint64_t spk_test_film_cache_hits(spk_engine* eng);
uint64_t spk_test_film_cache_misses(spk_engine* eng);
uint64_t spk_test_print_density_cache_hits(spk_engine* eng);
uint64_t spk_test_print_density_cache_misses(spk_engine* eng);
uint64_t spk_test_pointwise_cache_hits(spk_engine* eng);
uint64_t spk_test_pointwise_cache_misses(spk_engine* eng);
uint64_t spk_test_pointwise_cache_generation(spk_engine* eng);

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) g_fail = 1;
}

double max_abs(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        double d = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        if (d > m) m = d;
    }
    return m;
}

bool bytes_eq(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// Deterministic synthetic linear-RGB input covering shadows..highlights.
std::vector<float> make_input(int w, int h) {
    std::vector<float> v(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t i = (static_cast<size_t>(y) * w + x) * 3;
            v[i + 0] = 0.02f + 0.9f * x / (w - 1);
            v[i + 1] = 0.02f + 0.9f * y / (h - 1);
            v[i + 2] = 0.05f + 0.8f * ((x + y) % 17) / 16.0f;
        }
    return v;
}

const int W = 64, H = 48;
std::vector<float> g_input;

bool render(spk_engine* eng, const spk_params& q, bool preview,
            std::vector<float>* out) {
    spk_image in{g_input.data(), W, H, 0};
    spk_image o{};
    spk_status st = preview ? spk_simulate_preview(eng, &in, &q, &o)
                            : spk_simulate(eng, &in, &q, &o);
    if (st != SPK_OK) {
        std::printf("FAIL: render st=%d detail=%s\n", st,
                    spk_last_error_message());
        g_fail = 1;
        return false;
    }
    out->assign(o.data, o.data + static_cast<size_t>(o.width) * o.height * 3);
    spk_image_free(&o);
    return true;
}

// One kernel-class check set on one route. `label` names route+class.
void run_case(spk_engine* eng, const spk_params& base, const char* label,
              bool gpu_present) {
    spk_params cpu_p = base;   // gpu_preview = 0
    spk_params gpu_p = base;
    gpu_p.gpu_preview = 1;

    std::vector<float> exp_a, exp_b, prev_cpu, g1, g2, g3;
    if (!render(eng, cpu_p, false, &exp_a)) return;
    if (!render(eng, gpu_p, false, &exp_b)) return;
    check(bytes_eq(exp_a, exp_b),
          std::string(label) + ": EXPORT ignores gpu_preview (byte-identical)");

    if (!render(eng, cpu_p, true, &prev_cpu)) return;
    if (!render(eng, gpu_p, true, &g1)) return;
    if (!render(eng, gpu_p, true, &g2)) return;
    if (!render(eng, gpu_p, true, &g3)) return;
    check(bytes_eq(g1, g2) && bytes_eq(g1, g3),
          std::string(label) + ": GPU preview x3 byte-identical (warm host)");

    const double cpu_band = max_abs(prev_cpu, exp_a);
    const double gpu_band = max_abs(g1, exp_a);
    std::printf("info %s: preview-vs-export max_abs cpu(LUT)=%.3e gpu=%.3e\n",
                label, cpu_band, gpu_band);

    if (gpu_present) {
        // Bar: within the oracle tolerance of the export, OR no worse than the
        // existing CPU preview. The second arm matters on the print route,
        // where the preview's forced ENLARGER LUT (~1e-4, pre-existing and
        // GPU-independent) dominates the preview-vs-export distance for CPU
        // and GPU alike.
        check(gpu_band <= 1e-4 || gpu_band <= cpu_band,
              std::string(label) +
                  ": GPU preview within 1e-4 of export or beats the CPU preview");
        check(!bytes_eq(g1, prev_cpu),
              std::string(label) + ": GPU actually engaged (differs from LUT preview)");
    } else {
        check(bytes_eq(prev_cpu, g1),
              std::string(label) + ": no GPU => gpu_preview is a byte-identical no-op");
    }

    // EXPERIMENTAL GPU export (#154): the export path also offloads under
    // gpu_export. The CPU export (exp_a) is the oracle-verified reference.
    spk_params exp_gpu_p = base;
    exp_gpu_p.gpu_export = 1;
    std::vector<float> exp_gpu;
    if (!render(eng, exp_gpu_p, false, &exp_gpu)) return;
    if (gpu_present) {
        const double exp_band = max_abs(exp_gpu, exp_a);
        std::printf("info %s: GPU-export-vs-CPU-export max_abs=%.3e\n", label, exp_band);
        check(exp_band <= 1e-4,
              std::string(label) + ": GPU export within 1e-4 of CPU export");
        check(!bytes_eq(exp_gpu, exp_a),
              std::string(label) + ": GPU export actually engaged");
        // gpu_export must NOT leak into the preview path (independent toggles).
        spk_params prev_exp_p = base;
        prev_exp_p.gpu_export = 1;  // gpu_preview stays 0
        std::vector<float> prev_exp;
        if (render(eng, prev_exp_p, true, &prev_exp))
            check(bytes_eq(prev_exp, prev_cpu),
                  std::string(label) + ": gpu_export does not affect the preview path");
    } else {
        check(bytes_eq(exp_gpu, exp_a),
              std::string(label) + ": no GPU => gpu_export is a byte-identical no-op");
    }
}

void run_all(spk_engine* eng, bool gpu_present) {
    // The scanner self-check is process-global. Run a K75P profile first so a
    // hard-wired D50 GPU table fails before it can arm the global state. Keep
    // both affected Kodak profiles in the matrix, then cover the legacy scan
    // and print routes.
    const struct {
        int scan_film;
        const char* print_profile;
        const char* label;
    } routes[] = {
        {0, "kodak_2383", "print/kodak_2383/K75P"},
        {0, "kodak_2393", "print/kodak_2393/K75P"},
        {1, "kodak_portra_endura", "scan/kodak_portra/D50"},
        {0, "kodak_portra_endura", "print/kodak_portra_endura/D50"},
    };

    for (const auto& route : routes) {
        spk_params p;
        p.film_profile = "kodak_portra_400";
        p.print_profile = route.print_profile;
        spk_default_params(&p);
        p.scan_film = route.scan_film;
        p.preview_max_size = 640;  // > longest edge => preview on the same grid

        // Production defaults: scanner unsharp (0.7, 0.7) ON -> LINEAR kernel.
        run_case(eng, p, (std::string(route.label) + "/linear").c_str(),
                 gpu_present);
        // Sharpening off -> FUSED (full-chain sRGB) kernel.
        spk_params pf = p;
        pf.scanner_unsharp[0] = 0.0f;
        pf.scanner_unsharp[1] = 0.0f;
        run_case(eng, pf, (std::string(route.label) + "/fused").c_str(),
                 gpu_present);
    }
}

double rms_error(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size())
        return std::numeric_limits<double>::infinity();
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double difference =
            static_cast<double>(a[i]) - static_cast<double>(b[i]);
        sum += difference * difference;
    }
    return std::sqrt(sum / static_cast<double>(a.size()));
}

void run_product_pointwise_route_contract(spk_engine* eng,
                                          const char* asset_dir,
                                          bool gpu_present) {
    spk_params p{};
    p.film_profile = "kodak_portra_400";
    p.print_profile = "kodak_portra_endura";
    spk_default_params(&p);
    p.scan_film = 0;
    p.gpu_export = 0;
    p.grain_active = 0;
    p.halation_active = 0;
    p.glare_active = 0;
    p.print_glare_active = 0;
    p.dir_diffusion_size_um = 0.0f;
    p.dir_diffusion_tail_um = 0.0f;
    p.dir_diffusion_tail_weight = 0.0f;
    p.scanner_unsharp[0] = 0.0f;
    p.scanner_unsharp[1] = 0.0f;

    spk_params gpu_p = p;
    gpu_p.gpu_export = 1;
    std::vector<float> cpu_output;
    if (!render(eng, p, false, &cpu_output)) return;
    const uint64_t film_hits_before = spk_test_film_cache_hits(eng);
    const uint64_t film_misses_before = spk_test_film_cache_misses(eng);
    const uint64_t print_hits_before = spk_test_print_density_cache_hits(eng);
    const uint64_t print_misses_before =
        spk_test_print_density_cache_misses(eng);

    std::vector<float> output;
    if (!render(eng, gpu_p, false, &output)) return;
    char json[4096]{};
    const int json_size = spk::stage_timings_json_format(json, sizeof(json));
    check(json_size > 0 && std::strstr(json, "\"gpu_pointwise\":{") != nullptr,
          "product pointwise route publishes render-local diagnostics");
    const spk::GpuPointwiseTimingSnapshot& pointwise =
        spk::stage_timing_snapshot().gpu_pointwise;
    check(pointwise.requested && pointwise.attempted == gpu_present,
          "eligible product route requests the resident chain and attempts only when available");
    if (gpu_present) {
        const double materialized_max = max_abs(output, cpu_output);
        const double materialized_rms = rms_error(output, cpu_output);
        std::printf(
            "info: product materialized CPU-vs-GPU max_abs=%.9g rms=%.9g\n",
            materialized_max, materialized_rms);
        check(pointwise.engaged && std::strcmp(pointwise.reason, "none") == 0 &&
                  pointwise.dispatches == 3 && pointwise.input_uploads == 1 &&
                  pointwise.final_readbacks == 1 &&
                  pointwise.interstage_host_bytes == 0,
              "eligible product route completes one resident 3/1/1/0 DAG");
        check(materialized_max <= 1e-4 && materialized_rms <= 1e-5,
              "materialized f64-input product route stays inside the CPU oracle tolerance");
        check(std::strcmp(pointwise.self_test_state, "ran_pass") == 0 &&
                  pointwise.self_test_chain_runs == 3 &&
                  pointwise.self_test_dispatches == 9 &&
                  pointwise.self_test_input_uploads == 3 &&
                  pointwise.self_test_final_readbacks == 3 &&
                  pointwise.self_test_interstage_host_bytes == 0,
              "cold product render reports the separate 3-run numeric self-test");
        const spk::StageTimingSnapshot first_snapshot =
            spk::stage_timing_snapshot();
        check(first_snapshot.stages_ms[spk::STG_FILMING_EXPOSE] == 0.0 &&
                  first_snapshot.stages_ms[spk::STG_DEVELOP] == 0.0 &&
                  first_snapshot.stages_ms[spk::STG_PRINT_EXPOSE] == 0.0 &&
                  first_snapshot.stages_ms[spk::STG_SCAN] == 0.0,
              "self-test CPU oracle does not contaminate product stage slots");
        check(spk_test_film_cache_hits(eng) == film_hits_before &&
                  spk_test_film_cache_misses(eng) == film_misses_before &&
                  spk_test_print_density_cache_hits(eng) == print_hits_before &&
                  spk_test_print_density_cache_misses(eng) ==
                      print_misses_before,
              "resident success bypasses film and print-density memos");
    } else {
        check(!pointwise.attempted && !pointwise.engaged &&
                  pointwise.dispatches == 0 &&
                   pointwise.input_uploads == 0 &&
                   pointwise.final_readbacks == 0 &&
                   std::strcmp(pointwise.reason, "unavailable") == 0,
               "unavailable product route reports an explicit fallback");
        check(bytes_eq(output, cpu_output),
              "unavailable product route preserves exact CPU output");
    }

    const uint64_t first_generation =
        spk_test_pointwise_cache_generation(eng);
    const uint64_t hits_before_repeat = spk_test_pointwise_cache_hits(eng);
    std::vector<float> repeated;
    if (!render(eng, gpu_p, false, &repeated)) return;
    const spk::GpuPointwiseTimingSnapshot repeated_pointwise =
        spk::stage_timing_snapshot().gpu_pointwise;
    if (gpu_present) {
        check(bytes_eq(output, repeated),
              "product route is byte-identical across warm repeats");
        check(first_generation != 0 &&
                  spk_test_pointwise_cache_generation(eng) ==
                      first_generation &&
                  spk_test_pointwise_cache_hits(eng) > hits_before_repeat,
              "same static tables hit the prepared cache with a stable generation");
        check(std::strcmp(repeated_pointwise.self_test_state,
                          "cached_pass") == 0 &&
                  repeated_pointwise.self_test_chain_runs == 0 &&
                  repeated_pointwise.self_test_dispatches == 0,
              "warm render reports a cached numeric capability verdict");
    }

    spk_params blocked = gpu_p;
    blocked.grain_active = 1;
    std::vector<float> blocked_output;
    if (!render(eng, blocked, false, &blocked_output)) return;
    const spk::GpuPointwiseTimingSnapshot& blocked_pointwise =
        spk::stage_timing_snapshot().gpu_pointwise;
    check(blocked_pointwise.requested && !blocked_pointwise.attempted &&
              !blocked_pointwise.engaged &&
              std::strcmp(blocked_pointwise.reason, "grain") == 0 &&
              blocked_pointwise.dispatches == 0 &&
              blocked_pointwise.input_uploads == 0 &&
              blocked_pointwise.final_readbacks == 0,
          "ineligible effect reports a zero-work product fallback reason");

    if (!gpu_present) return;

    // The direct form reads caller f32 and folds the non-unity AE gain exactly
    // once into both the product request and keyed capability oracle.
    spk_params direct_cpu = p;
    direct_cpu.disable_buffer_memos = 1;
    direct_cpu.auto_exposure = 1;
    spk_params direct_gpu = direct_cpu;
    direct_gpu.gpu_export = 1;
    std::vector<float> direct_reference, direct_first, direct_repeat;
    if (!render(eng, direct_cpu, false, &direct_reference) ||
        !render(eng, direct_gpu, false, &direct_first) ||
        !render(eng, direct_gpu, false, &direct_repeat)) {
        return;
    }
    const double direct_max = max_abs(direct_first, direct_reference);
    const double direct_rms = rms_error(direct_first, direct_reference);
    std::printf("info: product direct CPU-vs-GPU max_abs=%.9g rms=%.9g gain=%.9g\n",
                direct_max, direct_rms, spk_test_pointwise_direct_gain());
    check(std::isfinite(spk_test_pointwise_direct_gain()) &&
              std::fabs(spk_test_pointwise_direct_gain() - 1.0) > 1e-6,
          "direct f32 product fixture exercises a non-unity input gain");
    check(direct_max <= 1e-4 && direct_rms <= 1e-5,
          "direct f32+gain product route stays inside the CPU oracle tolerance");
    check(bytes_eq(direct_first, direct_repeat),
          "direct f32+gain product route is byte-identical when warm");

    // Tone is deliberately the one CPU post-pass after a successful resident
    // chain. It must preserve the same public numerical contract.
    spk_params tone_cpu = p;
    tone_cpu.tone_curve_active = 1;
    tone_cpu.tone_curve_master_n = 3;
    tone_cpu.tone_curve_master_x[0] = 0.0f;
    tone_cpu.tone_curve_master_x[1] = 0.5f;
    tone_cpu.tone_curve_master_x[2] = 1.0f;
    tone_cpu.tone_curve_master_y[0] = 0.0f;
    tone_cpu.tone_curve_master_y[1] = 0.32f;
    tone_cpu.tone_curve_master_y[2] = 1.0f;
    spk_params tone_gpu = tone_cpu;
    tone_gpu.gpu_export = 1;
    std::vector<float> tone_reference, tone_first, tone_repeat;
    if (!render(eng, tone_cpu, false, &tone_reference) ||
        !render(eng, tone_gpu, false, &tone_first) ||
        !render(eng, tone_gpu, false, &tone_repeat)) {
        return;
    }
    const double tone_max = max_abs(tone_first, tone_reference);
    const double tone_rms = rms_error(tone_first, tone_reference);
    std::printf("info: product tone CPU-vs-GPU max_abs=%.9g rms=%.9g\n",
                tone_max, tone_rms);
    check(tone_max <= 1e-4 && tone_rms <= 1e-5,
          "active tone post-pass stays inside the CPU oracle tolerance");
    check(bytes_eq(tone_first, tone_repeat),
          "active tone product route is byte-identical when warm");

    // Successful direct GPU output supersedes both spectral LUT approximations.
    spk_params lut_gpu = gpu_p;
    lut_gpu.use_enlarger_lut = 1;
    lut_gpu.use_scanner_lut = 1;
    std::vector<float> lut_output;
    if (!render(eng, lut_gpu, false, &lut_output)) return;
    const spk::GpuPointwiseTimingSnapshot lut_pointwise =
        spk::stage_timing_snapshot().gpu_pointwise;
    check(lut_pointwise.engaged &&
              std::strcmp(lut_pointwise.reason, "none") == 0,
          "resident route supersedes enabled enlarger/scanner LUTs");

    // A table-affecting profile parameter must allocate a fresh opaque token,
    // rerun qualification, and visibly change the result.
    spk_params changed_cpu = p;
    changed_cpu.density_curve_gamma = 0.87f;
    spk_params changed_gpu = changed_cpu;
    changed_gpu.gpu_export = 1;
    std::vector<float> changed_reference, changed_output;
    if (!render(eng, changed_cpu, false, &changed_reference) ||
        !render(eng, changed_gpu, false, &changed_output)) {
        return;
    }
    const uint64_t changed_generation =
        spk_test_pointwise_cache_generation(eng);
    check(changed_generation != 0 && changed_generation != first_generation,
          "changed static table receives a new opaque prepared generation");
    check(!bytes_eq(changed_output, output),
          "changed static table changes the product output");
    check(max_abs(changed_output, changed_reference) <= 1e-4 &&
              rms_error(changed_output, changed_reference) <= 1e-5,
          "changed-table product route remains inside the CPU oracle tolerance");
    check(std::strcmp(spk::stage_timing_snapshot()
                          .gpu_pointwise.self_test_state,
                      "ran_pass") == 0,
          "changed table invalidates and reruns numeric qualification");

    // Rewarm the base key, then force the caller-sized scratch allocation to
    // fail. The public call must complete through exact CPU fallback.
    std::vector<float> base_rewarmed;
    if (!render(eng, gpu_p, false, &base_rewarmed)) return;
    std::vector<float> fallback_reference;
    if (!render(eng, p, false, &fallback_reference)) return;
    spk_test_pointwise_set_fault(1);
    std::vector<float> allocation_fallback;
    const bool allocation_rendered =
        render(eng, gpu_p, false, &allocation_fallback);
    spk_test_pointwise_set_fault(0);
    if (!allocation_rendered) return;
    const spk::GpuPointwiseTimingSnapshot allocation_pointwise =
        spk::stage_timing_snapshot().gpu_pointwise;
    check(!allocation_pointwise.attempted && !allocation_pointwise.engaged &&
              std::strcmp(allocation_pointwise.reason,
                          "allocation_failed") == 0,
          "scratch allocation failure fails closed before the product dispatch");
    check(bytes_eq(allocation_fallback, fallback_reference),
          "scratch allocation failure returns exact CPU fallback pixels");

    const uint64_t legacy_scan_frames_before = spk_gpu_scan_frames();
    const uint64_t legacy_print_frames_before = spk_gpu_print_frames();
    spk_test_pointwise_set_fault(5);
    std::vector<float> dispatch_fallback;
    const bool dispatch_rendered =
        render(eng, gpu_p, false, &dispatch_fallback);
    spk_test_pointwise_set_fault(0);
    if (!dispatch_rendered) return;
    const spk::GpuPointwiseTimingSnapshot dispatch_pointwise =
        spk::stage_timing_snapshot().gpu_pointwise;
    check(dispatch_pointwise.attempted && !dispatch_pointwise.engaged &&
              std::strcmp(dispatch_pointwise.reason, "dispatch-failed") == 0,
          "resident dispatch failure publishes an explicit attempted fallback");
    check(bytes_eq(dispatch_fallback, fallback_reference),
          "resident dispatch failure returns exact CPU fallback pixels");
    check(spk_gpu_scan_frames() == legacy_scan_frames_before &&
              spk_gpu_print_frames() == legacy_print_frames_before,
          "full-chain failure disables both legacy partial GPU stages for the frame");

    // Deterministic host seams hit each cancellation window. Public output must
    // remain empty; the completed post-dispatch counters remain observable.
    const auto run_cancel_fault = [&](int fault, const char* reason,
                                      bool after_dispatch) {
        spk_image in{g_input.data(), W, H, 0};
        spk_image cancelled{};
        spk_test_pointwise_set_fault(fault);
        const spk_status status = spk_simulate_cancellable(
            eng, &in, &gpu_p, &cancelled, nullptr, nullptr);
        spk_test_pointwise_set_fault(0);
        const spk::GpuPointwiseTimingSnapshot timing =
            spk::stage_timing_snapshot().gpu_pointwise;
        check(status == SPK_ERR_CANCELLED && cancelled.data == nullptr &&
                  cancelled.width == 0 && cancelled.height == 0,
              std::string(reason) + " leaves public output empty");
        check(std::strcmp(timing.reason, reason) == 0 && !timing.engaged &&
                  timing.attempted == after_dispatch &&
                  timing.dispatches == (after_dispatch ? 3u : 0u) &&
                  timing.input_uploads == (after_dispatch ? 1u : 0u) &&
                  timing.final_readbacks == (after_dispatch ? 1u : 0u),
              std::string(reason) + " publishes the correct render-local counters");
        spk_image_free(&cancelled);
    };
    run_cancel_fault(2, "cancelled_before_prepare", false);
    run_cancel_fault(3, "cancelled_before_dispatch", false);
    run_cancel_fault(4, "cancelled_after_dispatch", true);

    // Process-global opaque generation uniqueness prevents the lower-level
    // process-global table cache from reusing stale bytes across engine lifetimes.
    spk_engine* second = nullptr;
    spk_engine* third = nullptr;
    uint64_t second_generation = 0;
    uint64_t third_generation = 0;
    if (spk_engine_create(asset_dir, &second) == SPK_OK) {
        std::vector<float> second_output;
        if (render(second, gpu_p, false, &second_output))
            second_generation = spk_test_pointwise_cache_generation(second);
        spk_engine_destroy(second);
    }
    if (spk_engine_create(asset_dir, &third) == SPK_OK) {
        std::vector<float> third_output;
        if (render(third, gpu_p, false, &third_output))
            third_generation = spk_test_pointwise_cache_generation(third);
        spk_engine_destroy(third);
    }
    check(second_generation != 0 && third_generation != 0 &&
              second_generation != third_generation &&
              second_generation != first_generation &&
              third_generation != first_generation,
          "separate and recreated engines receive process-global unique generations");
}

void run_neutral_filter_failure_contract(const char* asset_dir) {
    spk_engine* eng = nullptr;
    if (spk_engine_create(asset_dir, &eng) != SPK_OK) {
        check(false, "neutral-filter failure fixture creates an isolated engine");
        return;
    }

    spk_params database{};
    database.film_profile = "kodak_portra_400";
    database.print_profile = "kodak_portra_endura";
    spk_default_params(&database);
    database.allow_gpu_scan = 0;
    database.gpu_preview = 0;
    database.gpu_export = 0;
    database.disable_buffer_memos = 1;

    spk_params schema_zero = database;
    schema_zero.neutral_print_filters_from_database = 0;
    schema_zero.c_filter_neutral = 0.0f;
    schema_zero.m_filter_neutral = 0.0f;
    schema_zero.y_filter_neutral = 0.0f;

    std::vector<float> database_output;
    std::vector<float> schema_zero_output;
    std::vector<float> resolver_failure_output;
    const bool valid_rendered = render(eng, database, false, &database_output);
    const bool zero_rendered = render(eng, schema_zero, false, &schema_zero_output);
    spk_test_pointwise_set_fault(6);  // simulate failure-atomic resolver=false
    const bool failure_rendered =
        render(eng, database, false, &resolver_failure_output);
    spk_test_pointwise_set_fault(0);

    if (valid_rendered && zero_rendered && failure_rendered) {
        check(!bytes_eq(database_output, schema_zero_output),
              "neutral-filter failure fixture distinguishes database CC from schema zero");
        check(bytes_eq(resolver_failure_output, schema_zero_output),
              "neutral-filter resolver failure preserves the schema-zero fallback");
    }
    spk_engine_destroy(eng);
}

void run_engine_asset_cap_contract(const char* asset_dir) {
#ifdef __ANDROID__
    (void)asset_dir;
#else
    namespace fs = std::filesystem;
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("spektra_engine_asset_cap_" + std::to_string(nonce));
    try {
        fs::create_directories(root / "profiles");
        fs::create_directories(root / "filters");
        fs::create_directories(root / "luts" / "spectral_upsampling");
        fs::copy_file(fs::path(asset_dir) / "profiles" /
                          "kodak_portra_400.json",
                      root / "profiles" / "kodak_portra_400.json");
        fs::copy_file(fs::path(asset_dir) / "profiles" /
                          "kodak_portra_endura.json",
                      root / "profiles" / "kodak_portra_endura.json");
        fs::copy_file(fs::path(asset_dir) / "luts" / "spectral_upsampling" /
                          "irradiance_xy_tc.npy",
                      root / "luts" / "spectral_upsampling" /
                          "irradiance_xy_tc.npy");
        const std::string oversized(spk::json::kMaxInputBytes + 1u, ' ');
        {
            std::ofstream out(root / "profiles" / "oversized.json",
                              std::ios::binary | std::ios::trunc);
            out.write(oversized.data(),
                      static_cast<std::streamsize>(oversized.size()));
            if (!out) throw std::runtime_error("cannot write oversized profile fixture");
        }
        {
            std::ofstream out(root / "filters" /
                                  "neutral_print_filters.json",
                              std::ios::binary | std::ios::trunc);
            out.write(oversized.data(),
                      static_cast<std::streamsize>(oversized.size()));
            if (!out) throw std::runtime_error("cannot write oversized neutral fixture");
        }
    } catch (const std::exception& e) {
        std::printf("FAIL: engine asset-cap fixture setup: %s\n", e.what());
        g_fail = 1;
        std::error_code ignored;
        fs::remove_all(root, ignored);
        return;
    }

    spk_engine* eng = nullptr;
    if (spk_engine_create(root.string().c_str(), &eng) != SPK_OK) {
        check(false, "engine asset-cap fixture creates an isolated engine");
        std::error_code ignored;
        fs::remove_all(root, ignored);
        return;
    }

    spk_params oversized_profile{};
    oversized_profile.film_profile = "oversized";
    oversized_profile.print_profile = "kodak_portra_endura";
    spk_default_params(&oversized_profile);
    oversized_profile.scan_film = 1;
    oversized_profile.allow_gpu_scan = 0;
    oversized_profile.disable_buffer_memos = 1;
    spk_image input{g_input.data(), W, H, 0};
    spk_image rejected{};
    const spk_status profile_status =
        spk_simulate(eng, &input, &oversized_profile, &rejected);
    check(profile_status == SPK_ERR_PROFILE_INVALID && rejected.data == nullptr &&
              std::strstr(spk_last_error_message(), "exceeds") != nullptr,
          "engine reports an existing cap+1 profile as bounded invalid content");
    check(spk_test_last_asset_max_bytes() == spk::json::kMaxInputBytes &&
              spk_test_last_asset_bytes_allocated() == 0,
          "cap+1 profile is rejected before its payload allocation");
    spk_image_free(&rejected);

    spk_params missing_profile = oversized_profile;
    missing_profile.film_profile = "definitely_missing_profile";
    spk_image missing{};
    const spk_status missing_status =
        spk_simulate(eng, &input, &missing_profile, &missing);
    check(missing_status == SPK_ERR_PROFILE_NOT_FOUND && missing.data == nullptr,
          "bounded profile reads still distinguish a missing asset from invalid content");
    spk_image_free(&missing);

    spk_params schema_zero{};
    schema_zero.film_profile = "kodak_portra_400";
    schema_zero.print_profile = "kodak_portra_endura";
    spk_default_params(&schema_zero);
    schema_zero.allow_gpu_scan = 0;
    schema_zero.disable_buffer_memos = 1;
    schema_zero.neutral_print_filters_from_database = 0;
    schema_zero.c_filter_neutral = 0.0f;
    schema_zero.m_filter_neutral = 0.0f;
    schema_zero.y_filter_neutral = 0.0f;
    spk_params oversized_neutral = schema_zero;
    oversized_neutral.neutral_print_filters_from_database = 1;
    std::vector<float> zero_output;
    std::vector<float> oversized_output;
    const bool zero_rendered = render(eng, schema_zero, false, &zero_output);
    const bool oversized_rendered =
        render(eng, oversized_neutral, false, &oversized_output);
    if (zero_rendered && oversized_rendered) {
        check(bytes_eq(zero_output, oversized_output),
              "cap+1 neutral database preserves the schema-zero fallback");
        check(spk_test_last_asset_max_bytes() == spk::json::kMaxInputBytes &&
                  spk_test_last_asset_bytes_allocated() == 0,
              "cap+1 neutral database is rejected before its payload allocation");
    }

    spk_engine_destroy(eng);
    std::error_code ignored;
    fs::remove_all(root, ignored);
#endif
}

void run_pointwise_chain_contract(bool gpu_present) {
    using namespace spk::gpu;

    constexpr uint32_t kLegacyOneDimensionalLimit = 65535u * 64u;
    const PointwiseDispatchGrid boundary =
        plan_pointwise_dispatch(kLegacyOneDimensionalLimit, 65535u, 65535u);
    check(boundary.valid && boundary.groups_x == 65535u && boundary.groups_y == 1u,
          "pointwise grid covers the legacy 1D dispatch boundary");
    const PointwiseDispatchGrid over_boundary =
        plan_pointwise_dispatch(kLegacyOneDimensionalLimit + 1u, 65535u, 65535u);
    check(over_boundary.valid && over_boundary.groups_x == 65535u &&
              over_boundary.groups_y == 2u &&
              over_boundary.group_row_stride_pixels == kLegacyOneDimensionalLimit,
          "pointwise grid crosses 4,194,240 pixels in one 2D dispatch");
    const PointwiseDispatchGrid twelve_mp =
        plan_pointwise_dispatch(12000000u, 65535u, 65535u);
    const PointwiseDispatchGrid fifty_mp =
        plan_pointwise_dispatch(50000000u, 65535u, 65535u);
    const PointwiseDispatchGrid two_hundred_mp =
        plan_pointwise_dispatch(200000000u, 65535u, 65535u);
    check(twelve_mp.valid && twelve_mp.groups_x == 65535u && twelve_mp.groups_y == 3u,
          "pointwise grid plans a 12 MP frame without host allocation");
    check(fifty_mp.valid && fifty_mp.groups_x == 65535u && fifty_mp.groups_y == 12u,
          "pointwise grid plans a 50 MP frame without host allocation");
    check(two_hundred_mp.valid && two_hundred_mp.groups_x == 65535u &&
              two_hundred_mp.groups_y == 48u,
          "pointwise grid plans a future 200 MP frame without host allocation");
    const PointwiseDispatchGrid uint32_frame =
        plan_pointwise_dispatch(UINT32_MAX, UINT32_MAX, UINT32_MAX);
    check(uint32_frame.valid && uint32_frame.groups_x == 67108863u &&
              uint32_frame.groups_y == 2u && uint32_frame.total_groups == 67108864u &&
              uint32_frame.group_row_stride_pixels == UINT64_C(4294967232),
          "pointwise grid keeps the shader's uint32 flat index overflow-free");
    check(static_cast<uint64_t>(uint32_frame.groups_x) * uint32_frame.groups_y >
                  uint32_frame.total_groups &&
              (static_cast<uint64_t>(uint32_frame.total_groups) - 1u) * 64u ==
                  UINT64_C(4294967232),
          "pointwise grid exposes padded groups for the pre-multiply shader guard");
    check(!plan_pointwise_dispatch(65u, 1u, 1u).valid,
          "pointwise grid rejects a device dispatch-limit overflow");

    constexpr uint32_t kPixels = 2;
    constexpr uint32_t kCurvePoints = 5;
    const float input[kPixels * 3] = {
        0.07f, 0.38f, 0.91f,
        0.44f, 0.63f, 0.14f,
    };
    const float tc_lut[2 * 2 * 3] = {
        0.13f, 0.21f, 0.34f, 0.55f, 0.09f, 0.27f,
        0.18f, 0.63f, 0.41f, 0.77f, 0.46f, 0.12f,
    };
    const float develop_axis[kCurvePoints * 3] = {
        -8.0f, -7.4f, -6.8f, -3.5f, -3.0f, -2.4f, -1.0f, -0.4f, 0.1f,
        0.8f,  1.3f,  1.9f,  4.0f,  4.6f,  5.2f,
    };
    const float develop_curve[kCurvePoints * 3] = {
        0.03f, 0.07f, 0.12f, 0.19f, 0.25f, 0.31f, 0.52f, 0.61f, 0.73f,
        1.08f, 1.21f, 1.37f, 1.92f, 2.13f, 2.36f,
    };
    const float dir_axis[kCurvePoints * 3] = {
        -7.7f, -7.0f, -6.4f, -3.2f, -2.7f, -2.0f, -0.8f, -0.2f, 0.4f,
        1.0f,  1.6f,  2.2f,  4.3f,  4.9f,  5.6f,
    };
    const float dir_curve[kCurvePoints * 3] = {
        0.02f, 0.05f, 0.09f, 0.16f, 0.22f, 0.29f, 0.48f, 0.58f, 0.69f,
        1.02f, 1.18f, 1.32f, 1.85f, 2.06f, 2.29f,
    };
    const float paper_axis[kCurvePoints * 3] = {
        -6.9f, -6.3f, -5.8f, -3.0f, -2.5f, -1.9f, -0.7f, -0.1f, 0.5f,
        1.1f,  1.7f,  2.4f,  4.6f,  5.1f,  5.8f,
    };
    const float paper_curve[kCurvePoints * 3] = {
        0.04f, 0.08f, 0.13f, 0.21f, 0.27f, 0.34f, 0.56f, 0.66f, 0.78f,
        1.12f, 1.28f, 1.45f, 2.01f, 2.24f, 2.49f,
    };
    float dye[81 * 3]{};
    float scan_dye[81 * 3]{};
    float isens[81 * 3]{};
    float icmf[81 * 3]{};
    for (size_t band = 0; band < 81; ++band) {
        const float t = static_cast<float>(band + 1u) / 81.0f;
        dye[band * 3] = 0.002f + 0.006f * t;
        dye[band * 3 + 1] = 0.003f + 0.004f * t * t;
        dye[band * 3 + 2] = 0.0015f + 0.007f * (1.0f - t);
        scan_dye[band * 3] = 0.004f + 0.003f * (1.0f - t);
        scan_dye[band * 3 + 1] = 0.001f + 0.008f * t * t;
        scan_dye[band * 3 + 2] = 0.0025f + 0.005f * t;
        isens[band * 3] = (0.60f + 0.40f * t) / 81.0f;
        isens[band * 3 + 1] = (0.72f - 0.31f * t) / 81.0f;
        isens[band * 3 + 2] = (0.48f + 0.23f * t * t) / 81.0f;
        icmf[band * 3] = (0.51f + 0.36f * t) / 81.0f;
        icmf[band * 3 + 1] = (0.79f - 0.27f * t) / 81.0f;
        icmf[band * 3 + 2] = (0.43f + 0.29f * t * t) / 81.0f;
    }

    PointwiseChainRequest request{};
    request.input_rgb = input;
    request.input_component_count = kPixels * 3;
    request.pixel_count = kPixels;
    request.static_table_key = UINT64_C(0x1480000000000001);
    request.film.tc_lut = {tc_lut, 2 * 2 * 3};
    request.film.tc_edge = 2;
    request.film.develop_axis = {develop_axis, kCurvePoints * 3};
    request.film.develop_curve = {develop_curve, kCurvePoints * 3};
    request.film.dir_axis = {dir_axis, kCurvePoints * 3};
    request.film.dir_curve = {dir_curve, kCurvePoints * 3};
    request.film.curve_points = kCurvePoints;
    request.film.exposure_multiplier = 0.83f;
    request.film.coupler_shift = 0.17f;
    const float coupler_matrix[9] = {
        0.041f, 0.012f, 0.007f, 0.009f, 0.053f,
        0.015f, 0.004f, 0.018f, 0.047f,
    };
    std::memcpy(request.film.coupler_matrix, coupler_matrix,
                sizeof(coupler_matrix));
    request.print.dye = {dye, 81 * 3};
    request.print.illuminant_sensitivity = {isens, 81 * 3};
    request.print.paper_axis = {paper_axis, kCurvePoints * 3};
    request.print.paper_curve = {paper_curve, kCurvePoints * 3};
    request.print.curve_points = kCurvePoints;
    request.print.midgray = 0.91f;
    request.print.exposure_multiplier = 1.13f;
    request.print.preflash[0] = 0.011f;
    request.print.preflash[1] = 0.023f;
    request.print.preflash[2] = 0.006f;
    request.scan.dye = {scan_dye, 81 * 3};
    request.scan.illuminant_cmf = {icmf, 81 * 3};
    const float xyz_to_rgb[9] = {
        1.07f, 0.08f, 0.02f, 0.04f, 0.92f,
        0.07f, 0.03f, 0.11f, 0.79f,
    };
    std::memcpy(request.scan.xyz_to_rgb, xyz_to_rgb, sizeof(xyz_to_rgb));

    float invalid_rgb[kPixels * 3];
    for (float& v : invalid_rgb) v = 123.0f;
    PointwiseChainOutput invalid_output{invalid_rgb, kPixels * 3};
    PointwiseChainDiagnostics invalid_diagnostics{};
    PointwiseChainRequest bad_count = request;
    --bad_count.film.develop_axis.count;
    check(!render_pointwise_chain(bad_count, &invalid_output, &invalid_diagnostics),
          "pointwise chain rejects a truncated curve axis");
    bool sentinel_unchanged = true;
    for (float v : invalid_rgb) sentinel_unchanged = sentinel_unchanged && v == 123.0f;
    check(sentinel_unchanged,
          "pointwise invalid request leaves the caller output untouched");

    float nonfinite_axis[kCurvePoints * 3];
    std::memcpy(nonfinite_axis, develop_axis, sizeof(develop_axis));
    nonfinite_axis[2] = std::numeric_limits<float>::quiet_NaN();
    PointwiseChainRequest bad_axis = request;
    bad_axis.film.develop_axis = {nonfinite_axis, kCurvePoints * 3};
    check(!render_pointwise_chain(bad_axis, &invalid_output, &invalid_diagnostics),
          "pointwise chain rejects a non-finite folded curve axis");
    sentinel_unchanged = true;
    for (float v : invalid_rgb) sentinel_unchanged = sentinel_unchanged && v == 123.0f;
    check(sentinel_unchanged,
          "pointwise non-finite table failure leaves caller output untouched");

    float descending_axis[kCurvePoints * 3];
    std::memcpy(descending_axis, develop_axis, sizeof(develop_axis));
    descending_axis[3] = -9.0f;
    PointwiseChainRequest bad_order = request;
    bad_order.film.develop_axis = {descending_axis, kCurvePoints * 3};
    check(!render_pointwise_chain(bad_order, &invalid_output, &invalid_diagnostics),
          "pointwise chain rejects a non-increasing curve axis");
    sentinel_unchanged = true;
    for (float v : invalid_rgb) sentinel_unchanged = sentinel_unchanged && v == 123.0f;
    check(sentinel_unchanged,
          "pointwise invalid axis ordering leaves caller output untouched");

    float cold_rgb[kPixels * 3]{};
    PointwiseChainOutput cold_output{cold_rgb, kPixels * 3};
    PointwiseChainDiagnostics cold{};
    const bool cold_ok = render_pointwise_chain(request, &cold_output, &cold);
    if (!gpu_present) {
        check(!cold_ok, "pointwise chain fails closed without Vulkan");
        check(!cold.engaged && cold.dispatches == 0 && cold.input_uploads == 0 &&
                  cold.final_readbacks == 0 && cold.interstage_host_bytes == 0,
              "pointwise fallback reports zero GPU work");
        check(cold.fallback_reason != PointwiseFallbackReason::none,
              "pointwise fallback exposes a reason");
        return;
    }

    check(cold_ok && cold.engaged, "pointwise filming->printing->scan chain engages");
    check(cold.dispatches == 3 && cold.input_uploads == 1 &&
              cold.final_readbacks == 1 && cold.interstage_host_bytes == 0,
          "pointwise chain stays resident between exactly three dispatches");
    check(cold.pipeline_creates == 3 && cold.buffer_allocations > 0 &&
              cold.static_upload_bytes > 0,
          "cold pointwise chain creates pipelines, buffers, and static tables");
    bool finite = true;
    for (float v : cold_rgb) finite = finite && std::isfinite(v);
    check(finite, "pointwise chain explicitly contains non-finite input");
    double cold_oracle[kPixels * 3]{};
    spk::gpu::test::render_pointwise_chain_f64(request, cold_oracle);
    const spk::gpu::test::PointwiseOracleError cold_error =
        spk::gpu::test::pointwise_oracle_error(cold_rgb, cold_oracle, kPixels * 3);
    std::printf("info: pointwise f64 oracle max_abs=%.9g rms=%.9g\n",
                cold_error.max_abs, cold_error.rms);
    check(cold_error.max_abs <= 2e-5 && cold_error.rms <= 5e-6,
          "pointwise combined chain stays inside the f64 oracle tolerance");

    float nan_input[kPixels * 3];
    std::memcpy(nan_input, input, sizeof(input));
    nan_input[3] = std::numeric_limits<float>::quiet_NaN();
    PointwiseChainRequest nan_request = request;
    nan_request.input_rgb = nan_input;
    float nan_rgb[kPixels * 3]{};
    PointwiseChainOutput nan_output{nan_rgb, kPixels * 3};
    PointwiseChainDiagnostics nan_diagnostics{};
    const bool nan_ok =
        render_pointwise_chain(nan_request, &nan_output, &nan_diagnostics);
    bool nan_finite = true;
    for (float value : nan_rgb) nan_finite = nan_finite && std::isfinite(value);
    check(nan_ok && nan_diagnostics.engaged && nan_diagnostics.dispatches == 3 &&
              nan_diagnostics.input_uploads == 1 &&
              nan_diagnostics.final_readbacks == 1 &&
              nan_diagnostics.interstage_host_bytes == 0 && nan_finite,
          "pointwise NaN input is contained in a separate resident render");

    float warm_rgb[kPixels * 3]{};
    PointwiseChainOutput warm_output{warm_rgb, kPixels * 3};
    PointwiseChainDiagnostics warm{};
    const bool warm_ok = render_pointwise_chain(request, &warm_output, &warm);
    check(warm_ok && warm.engaged &&
              std::memcmp(cold_rgb, warm_rgb, sizeof(cold_rgb)) == 0,
          "warm pointwise chain is byte-deterministic");
    check(warm.dispatches == 3 && warm.input_uploads == 1 &&
              warm.final_readbacks == 1 && warm.interstage_host_bytes == 0,
          "warm pointwise chain preserves the resident DAG contract");
    check(warm.pipeline_creates == 0 && warm.buffer_allocations == 0 &&
              warm.static_upload_bytes == 0,
          "warm pointwise chain reuses pipelines, buffers, and static tables");

    float changed_tc_lut[2 * 2 * 3];
    for (size_t i = 0; i < 2 * 2 * 3; ++i) {
        changed_tc_lut[i] = tc_lut[i] * (0.91f + 0.01f * static_cast<float>(i)) +
                            0.017f;
    }
    PointwiseChainRequest changed_request = request;
    changed_request.static_table_key = UINT64_C(0x1480000000001001);
    changed_request.film.tc_lut = {changed_tc_lut, 2 * 2 * 3};
    float changed_rgb[kPixels * 3]{};
    PointwiseChainOutput changed_output{changed_rgb, kPixels * 3};
    PointwiseChainDiagnostics changed_diagnostics{};
    const bool changed_ok =
        render_pointwise_chain(changed_request, &changed_output, &changed_diagnostics);
    check(changed_ok && changed_diagnostics.engaged &&
              changed_diagnostics.dispatches == 3 &&
              changed_diagnostics.input_uploads == 1 &&
              changed_diagnostics.final_readbacks == 1 &&
              changed_diagnostics.interstage_host_bytes == 0,
          "changed table key completes the same resident DAG");
    check(changed_diagnostics.pipeline_creates == 0 &&
              changed_diagnostics.buffer_allocations == 0 &&
              changed_diagnostics.static_upload_bytes > 0,
          "changed table key re-uploads static bytes without rebuilding resources");
    check(std::memcmp(cold_rgb, changed_rgb, sizeof(cold_rgb)) != 0,
          "changed static table produces changed output");
    double changed_oracle[kPixels * 3]{};
    spk::gpu::test::render_pointwise_chain_f64(changed_request, changed_oracle);
    const spk::gpu::test::PointwiseOracleError changed_error =
        spk::gpu::test::pointwise_oracle_error(changed_rgb, changed_oracle,
                                               kPixels * 3);
    std::printf("info: changed-table f64 oracle max_abs=%.9g rms=%.9g\n",
                changed_error.max_abs, changed_error.rms);
    check(changed_error.max_abs <= 2e-5 && changed_error.rms <= 5e-6,
          "changed-table chain stays inside the f64 oracle tolerance");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <asset_dir>\n", argv[0]); return 2; }
    spk_engine* eng = nullptr;
    if (spk_engine_create(argv[1], &eng) != SPK_OK) {
        std::fprintf(stderr, "engine create failed from %s\n", argv[1]);
        return 2;
    }
    g_input = make_input(W, H);

    run_pointwise_chain_contract(spk::gpu::available());

    // Product-seam contract for #148 Phase B. This deliberately runs before the
    // broader legacy partial-GPU matrix so diagnostics describe this render.
    run_product_pointwise_route_contract(eng, argv[1],
                                         spk::gpu::available());
    run_neutral_filter_failure_contract(argv[1]);
    run_engine_asset_cap_contract(argv[1]);

    // First pass assumes a GPU; if nothing engaged (state stays 0), rerun all
    // assertions in fallback mode instead.
    run_all(eng, /*gpu_present=*/true);
    int st = spk_gpu_scan_state();
    if (st == 0) {
        std::printf("info: no GPU engaged (state=0) — validating fallback law only\n");
        g_fail = 0;
        run_all(eng, /*gpu_present=*/false);
        check(spk_gpu_scan_state() == 0, "self-check never ran without a GPU");
    } else {
        check(st == 1, "self-check passed (state == 1)");
        check(spk_gpu_scan_frames() > 0,
              "frames counter engaged (spk_gpu_scan_frames > 0)");
        // PRINT-EXPOSE offload (perf lab): the print route above ran with
        // allow_gpu on, so its integral must have engaged too. Without these the
        // print numbers printed above would still pass on a silent CPU fallback,
        // which is exactly the failure mode worth catching — the offload is only
        // interesting if it is actually running.
        std::printf("info: gpu print state=%d frames=%llu\n", spk_gpu_print_state(),
                    static_cast<unsigned long long>(spk_gpu_print_frames()));
        check(spk_gpu_print_state() == 1,
              "print-expose self-check passed (spk_gpu_print_state == 1)");
        check(spk_gpu_print_frames() > 0,
              "print-expose offload engaged (spk_gpu_print_frames > 0)");
    }
    if (st == 0) {
        check(spk_gpu_scan_frames() == 0,
              "frames counter stays 0 without a GPU");
        check(spk_gpu_print_frames() == 0,
              "print-expose offload never engaged without a GPU");
    }

    std::printf(g_fail ? "test_gpu_host: FAIL\n" : "test_gpu_host: ALL OK\n");
    return g_fail;
}
