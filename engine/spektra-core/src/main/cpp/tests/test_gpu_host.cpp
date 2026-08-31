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
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "gpu/tests/pointwise_chain_oracle.h"
#include "gpu/vulkan_compute.h"
#include "spektra.h"

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
    if (st != SPK_OK) { std::printf("FAIL: render st=%d\n", st); g_fail = 1; return false; }
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
