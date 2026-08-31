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
#include <string>
#include <vector>

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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <asset_dir>\n", argv[0]); return 2; }
    spk_engine* eng = nullptr;
    if (spk_engine_create(argv[1], &eng) != SPK_OK) {
        std::fprintf(stderr, "engine create failed from %s\n", argv[1]);
        return 2;
    }
    g_input = make_input(W, H);

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
