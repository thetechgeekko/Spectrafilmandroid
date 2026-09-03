/*
 * Spektrafilm for Android — host test for the spectral 3D-LUT memo.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * Gates kernels/lut3d_cache.{h,cpp}: the engine-level memo that stops scan()'s
 * scanner LUT and print_expose()'s enlarger LUT from being rebuilt on every
 * call. spk_simulate_preview forces BOTH LUTs on, so before the memo every
 * interactive frame paid two steps^3 sweeps of 81-band spectral integrals to
 * reproduce a LUT that had not changed.
 *
 * The memo is only sound if a cached LUT is indistinguishable from a rebuilt
 * one, so this test compares against a FRESH ENGINE (which can only ever build
 * cold) and demands BYTE-IDENTICAL output — not a tolerance band. The LUT paths'
 * accuracy against the direct spectral evaluation stays the business of
 * test_scanner_lut_e2e / test_enlarger_lut_e2e / test_lut_accel; what is at
 * stake here is purely "memo == rebuild".
 *
 * Assertions, on BOTH routes (scan_film and print), LUTs on at resolution 17:
 *  1. WARM == COLD. A shared engine rendering a param set must be byte-identical
 *     to a fresh engine rendering the same params — the first time and on every
 *     repeat.
 *  2. THE MEMO ENGAGES. Repeating a render whose LUT inputs are unchanged must
 *     produce cache HITS and no new MISS. (The probe param is print_exposure /
 *     output_color_space: each busts the downstream buffer memos so the LUT is
 *     actually fetched again, but neither feeds a LUT key.)
 *  3. NO STALE REUSE. Perturbing each LUT-relevant param ONE AT A TIME — every
 *     input folded into either key: the film and print profiles, the enlarger
 *     filter shifts and neutral CC, exposure compensation, the preflash, the
 *     grain density floor and lut_resolution — must register a MISS and must
 *     still match a fresh engine byte-for-byte. A key that omitted any of them
 *     would return a stale LUT here and the byte compare would catch it.
 *  4. THREAD INVARIANCE. The memo must not introduce a thread-count dependence:
 *     the same render at SPK_NUM_THREADS 1 and 8 stays byte-identical (the gate
 *     test_parallel applies to the non-LUT path).
 *
 * Build (host) — full source set, from the cpp root:
 *   g++ -std=c++17 -O2 -pthread -I. tests/test_lut_cache_e2e.cpp <SRC> -o /tmp/test_lut_cache_e2e
 * Run:
 *   /tmp/test_lut_cache_e2e <asset_dir> <input.f64>
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "kernels/lut3d_cache.h"
#include "model/color_output.h"
#include "profiles/profile.h"
#include "runtime/stages/scanning.h"
#include "spektra.h"

// Host-only memo counters (spektra.cpp, #ifndef __ANDROID__). Forward-declared
// exactly as the other host tests do — no header / ABI change.
extern uint64_t spk_test_lut_cache_hits(spk_engine* eng);
extern uint64_t spk_test_lut_cache_misses(spk_engine* eng);

namespace {

const char* kAssetDir = "/home/user/spektrafilm/src/spektrafilm/data";
const char* kInputF64 =
    "/home/user/Spectrafilmandroid/engine/spektra-core/src/main/cpp/tests/"
    "scan_portra_input_rgb.f64";

int g_fail = 0;

void check(bool ok, const char* what) {
    std::printf("[lut_cache_e2e] %-52s -> %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) g_fail = 1;
}

// Deterministic params with BOTH spectral LUTs on at the preview resolution —
// the configuration spk_simulate_preview forces and the one the memo serves.
spk_params base_params(int scan_film) {
    spk_params p{};
    p.film_profile = "kodak_portra_400";
    p.print_profile = "kodak_portra_endura";
    spk_default_params(&p);
    p.exposure_compensation_ev = 0.0f;
    p.auto_exposure = 0;
    p.density_curve_gamma = 1.0f;
    p.grain_active = 0;     // stochastic off -> deterministic, memos active
    p.halation_active = 0;
    // Spatial effects are per-effect gated (zero = inert); express the oracle's
    // deactivate_spatial_effects by zeroing the nonzero defaults.
    p.dir_diffusion_size_um = 0.0f;
    p.lens_blur_um = 0.0f;
    p.camera_diffusion_active = 0;
    p.enlarger_diffusion_active = 0;
    p.scanner_lens_blur = 0.0f;
    p.scanner_unsharp[0] = 0.0f;
    p.scanner_unsharp[1] = 0.0f;
    p.dir_couplers_active = 1;
    p.glare_active = 0;
    p.print_glare_active = 0;
    p.scan_film = scan_film;
    p.output_color_space = SPK_CS_SRGB;
    p.output_cctf_encoding = 1;
    p.rgb_to_raw_method = SPK_RGB2RAW_HANATOS2025;
    p.use_scanner_lut = 1;
    p.use_enlarger_lut = 1;
    p.lut_resolution = 17;
    return p;
}

bool render(spk_engine* eng, const spk_image& in, const spk_params& p,
            std::vector<float>* out_rgb) {
    spk_image out{};
    spk_status st = spk_simulate(eng, &in, &p, &out);
    if (st != SPK_OK) {
        std::fprintf(stderr, "spk_simulate failed: %s\n", spk_status_str(st));
        return false;
    }
    const size_t n = static_cast<size_t>(out.width) * out.height * 3;
    out_rgb->assign(out.data, out.data + n);
    spk_image_free(&out);
    return true;
}

// Render `p` on a brand-new engine, which has no memo to hit and must build the
// LUT cold — the reference every warm render is compared against.
bool render_cold(const std::string& asset_dir, const spk_image& in,
                 const spk_params& p, std::vector<float>* out_rgb) {
    spk_engine* fresh = nullptr;
    if (spk_engine_create(asset_dir.c_str(), &fresh) != SPK_OK) {
        std::fprintf(stderr, "fresh engine create failed\n");
        return false;
    }
    bool ok = render(fresh, in, p, out_rgb);
    spk_engine_destroy(fresh);
    return ok;
}

bool bit_identical(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string asset_dir = argc > 1 ? argv[1] : kAssetDir;
    std::string input_path = argc > 2 ? argv[2] : kInputF64;

    // The committed fixture is the 64x64 scan_portra input (float64 RGB).
    const int width = 64, height = 64;
    const size_t n = static_cast<size_t>(width) * height * 3;
    std::vector<double> rgb64(n);
    {
        std::ifstream in(input_path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "cannot open %s\n", input_path.c_str());
            return 2;
        }
        in.read(reinterpret_cast<char*>(rgb64.data()),
                static_cast<std::streamsize>(n * sizeof(double)));
        if (in.gcount() != static_cast<std::streamsize>(n * sizeof(double))) {
            std::fprintf(stderr, "input size mismatch\n");
            return 2;
        }
    }
    std::vector<float> rgb32(rgb64.begin(), rgb64.end());
    spk_image in_img{rgb32.data(), width, height, SPK_CS_PROPHOTO};
    std::printf("Image: %dx%dx3, LUTs on @ resolution 17\n", width, height);

    spk_engine* eng = nullptr;
    if (spk_engine_create(asset_dir.c_str(), &eng) != SPK_OK) {
        std::fprintf(stderr, "engine create failed\n");
        return 2;
    }

    for (int scan_film = 1; scan_film >= 0; --scan_film) {
        const char* route = scan_film ? "scan_film" : "print";
        spk_params p = base_params(scan_film);

        // --- 1. WARM == COLD, on the first render and on a repeat ------------
        std::vector<float> cold, warm1, warm2;
        if (!render_cold(asset_dir, in_img, p, &cold)) return 2;
        if (!render(eng, in_img, p, &warm1)) return 2;
        if (!render(eng, in_img, p, &warm2)) return 2;
        check(bit_identical(warm1, cold),
              (std::string(route) + ": first warm render == cold").c_str());
        check(bit_identical(warm2, cold),
              (std::string(route) + ": repeat render == cold").c_str());

        // --- 2. THE MEMO ENGAGES --------------------------------------------
        // print_exposure (print route) and the output space (scan route) each
        // bust the film/print buffer memos downstream of the LUT, forcing the
        // LUT to be fetched again, while feeding NEITHER LUT key. So the fetch
        // must hit, and no new build may happen.
        const uint64_t miss_before = spk_test_lut_cache_misses(eng);
        const uint64_t hit_before = spk_test_lut_cache_hits(eng);
        for (int i = 1; i <= 3; ++i) {
            spk_params q = p;
            if (scan_film) {
                q.output_color_space =
                    (i % 2) ? SPK_CS_ADOBE_RGB : SPK_CS_REC2020;
            } else {
                q.print_exposure = 1.0f + 0.01f * static_cast<float>(i);
            }
            std::vector<float> warm, ref;
            if (!render(eng, in_img, q, &warm)) return 2;
            if (!render_cold(asset_dir, in_img, q, &ref)) return 2;
            check(bit_identical(warm, ref),
                  (std::string(route) + ": non-LUT param edit == cold").c_str());
        }
        const uint64_t new_hits = spk_test_lut_cache_hits(eng) - hit_before;
        const uint64_t new_misses = spk_test_lut_cache_misses(eng) - miss_before;
        std::printf("[lut_cache_e2e] %s: LUT fetches over 3 non-LUT edits: "
                    "%llu hit / %llu miss\n", route,
                    static_cast<unsigned long long>(new_hits),
                    static_cast<unsigned long long>(new_misses));
        check(new_hits > 0,
              (std::string(route) + ": memo engaged (hits > 0)").c_str());
        check(new_misses == 0,
              (std::string(route) + ": no rebuild on a non-LUT edit").c_str());

        // --- 3. NO STALE REUSE ----------------------------------------------
        // Every input folded into either LUT key, perturbed one at a time. Each
        // must MISS (a new LUT is genuinely required) and must still match a
        // cold render exactly — an under-specified key would silently serve the
        // previous LUT and diverge here.
        struct Perturb { const char* name; void (*apply)(spk_params*); };
        const Perturb perturbs[] = {
            {"film_profile", [](spk_params* q) { q->film_profile = "fujifilm_pro_400h"; }},
            {"print_profile", [](spk_params* q) {
                 q->print_profile = "fujifilm_crystal_archive_typeii"; }},
            {"lut_resolution", [](spk_params* q) { q->lut_resolution = 23; }},
            {"y_filter_shift", [](spk_params* q) { q->y_filter_shift = 7.5f; }},
            {"m_filter_shift", [](spk_params* q) { q->m_filter_shift = -4.0f; }},
            {"y_filter_neutral", [](spk_params* q) {
                 q->neutral_print_filters_from_database = 0;
                 q->y_filter_neutral = 42.0f; }},
            {"exposure_compensation_ev", [](spk_params* q) {
                 q->exposure_compensation_ev = 1.25f; }},
            {"normalize_print_exposure", [](spk_params* q) {
                 q->exposure_compensation_ev = 1.25f;
                 q->normalize_print_exposure = 0; }},
            {"preflash_exposure", [](spk_params* q) { q->preflash_exposure = 0.35f; }},
            {"preflash_m_filter_shift", [](spk_params* q) {
                 q->preflash_exposure = 0.35f;
                 q->preflash_m_filter_shift = 6.0f; }},
            {"grain_density_min", [](spk_params* q) {
                 q->grain_density_min[0] = 0.11f;
                 q->grain_density_min[1] = 0.13f;
                 q->grain_density_min[2] = 0.17f; }},
        };
        for (const Perturb& pb : perturbs) {
            spk_params q = p;
            pb.apply(&q);
            const uint64_t m0 = spk_test_lut_cache_misses(eng);
            std::vector<float> warm, ref;
            if (!render(eng, in_img, q, &warm)) return 2;
            if (!render_cold(asset_dir, in_img, q, &ref)) return 2;
            const bool missed = spk_test_lut_cache_misses(eng) > m0;
            const bool same = bit_identical(warm, ref);
            std::printf("[lut_cache_e2e] %-9s perturb %-24s rebuilt=%d "
                        "identical=%d -> %s\n", route, pb.name, missed ? 1 : 0,
                        same ? 1 : 0, same ? "PASS" : "FAIL");
            if (!same) g_fail = 1;
            // A perturbation that does not reach either LUT (e.g. an enlarger
            // param on the scan_film route) legitimately does NOT rebuild; the
            // byte compare above is the gate that matters. Rebuilds are only
            // REQUIRED where the param does feed a key, asserted below.
            const bool feeds_a_key =
                scan_film ? (std::strcmp(pb.name, "film_profile") == 0 ||
                             std::strcmp(pb.name, "lut_resolution") == 0)
                          : true;
            if (feeds_a_key && !missed) {
                std::printf("[lut_cache_e2e] %s: %s feeds a LUT key but did not "
                            "rebuild -> FAIL\n", route, pb.name);
                g_fail = 1;
            }
        }

        // --- 4. THREAD INVARIANCE -------------------------------------------
        // A memo must not make the result depend on the worker count. Compare
        // 1-worker and 8-worker renders through the SAME warm cache.
        std::vector<float> t1, t8;
        setenv("SPK_NUM_THREADS", "1", 1);
        if (!render(eng, in_img, p, &t1)) return 2;
        setenv("SPK_NUM_THREADS", "8", 1);
        if (!render(eng, in_img, p, &t8)) return 2;
        unsetenv("SPK_NUM_THREADS");
        check(bit_identical(t1, t8),
              (std::string(route) + ": 1 vs 8 workers byte-identical").c_str());
        check(bit_identical(t1, cold),
              (std::string(route) + ": 1-worker warm == cold").c_str());
    }

    // --- 5. VIEWING-WHITE CACHE ISOLATION ----------------------------------
    // Keep every profile table and density sample byte-identical, changing ONLY
    // the resolved viewing illuminant. The second call must miss; otherwise a
    // K75P stock can silently reuse a D50 scanner LUT (or vice versa). Returning
    // to K75P must hit and reproduce the first K75P result byte-for-byte.
    {
        spk::Profile k75p = spk::load_profile_file(
            asset_dir + "/profiles/kodak_2383.json");
        spk::Profile d50 = k75p;
        d50.viewing_illuminant = "D50";
        d50.resolved_viewing_illuminant =
            spk::find_viewing_illuminant("D50");

        constexpr int w = 4, h = 4;
        std::vector<float> density(static_cast<size_t>(w) * h * 3);
        for (int p = 0; p < w * h; ++p) {
            density[static_cast<size_t>(p) * 3 + 0] = 0.05f + 0.07f * p;
            density[static_cast<size_t>(p) * 3 + 1] = 0.11f + 0.05f * p;
            density[static_cast<size_t>(p) * 3 + 2] = 0.17f + 0.03f * p;
        }

        spk::Lut3DCache cache;
        spk::ScanningParams q;
        q.scan_film = false;
        q.use_lut = true;
        q.lut_resolution = 17;
        q.lut_cache = &cache;
        std::vector<float> k75p_a(density.size()), d50_out(density.size()),
            k75p_b(density.size());

        spk::scan(k75p, q, density.data(), w, h, k75p_a.data());
        const uint64_t misses_after_k75p = cache.misses();
        spk::scan(d50, q, density.data(), w, h, d50_out.data());
        const bool white_changed_key = cache.misses() > misses_after_k75p;
        const uint64_t hits_before_return = cache.hits();
        spk::scan(k75p, q, density.data(), w, h, k75p_b.data());
        const bool warm_hit = cache.hits() > hits_before_return;
        const bool warm_exact = bit_identical(k75p_a, k75p_b);

        double white_delta = 0.0;
        for (size_t i = 0; i < k75p_a.size(); ++i) {
            const double d = std::fabs(static_cast<double>(k75p_a[i]) -
                                       static_cast<double>(d50_out[i]));
            if (d > white_delta) white_delta = d;
        }
        const bool discriminating = white_delta > 1e-3;
        const bool pass = white_changed_key && warm_hit && warm_exact &&
                          discriminating;
        std::printf("[lut_cache_e2e] viewing-only D50/K75P: miss=%d "
                    "return-hit=%d warm-exact=%d max-delta=%.6e -> %s\n",
                    white_changed_key ? 1 : 0, warm_hit ? 1 : 0,
                    warm_exact ? 1 : 0, white_delta, pass ? "PASS" : "FAIL");
        if (!pass) g_fail = 1;
    }

    std::printf("[lut_cache_e2e] final memo state: %llu hits / %llu misses\n",
                static_cast<unsigned long long>(spk_test_lut_cache_hits(eng)),
                static_cast<unsigned long long>(spk_test_lut_cache_misses(eng)));
    spk_engine_destroy(eng);
    std::printf("%s\n", g_fail ? "FAIL" : "ALL PASS");
    return g_fail;
}
