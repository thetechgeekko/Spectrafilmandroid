/*
 * Spektrafilm for Android — END-TO-END host parity test for the spectral-domain
 * Gaussian blur (settings.spectral_gaussian_blur) run through the WHOLE scan_film
 * pipeline.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * The spectral blur is the optional blur in compute_hanatos2025_tc_lut: the
 * HANATOS2025 spectra LUT is blurred along its spectral axis (axis 2) with
 * scipy.ndimage.gaussian_filter(sigma=(0,0,sigma)) (SciPy defaults order=0,
 * mode='reflect', truncate=4.0) BEFORE the sensitivity contraction. It therefore
 * changes the filming tc_lut and every downstream tap (film_log_raw /
 * film_density_cmy / final_rgb). The blur lives in the LUT-build path, NOT the
 * per-pixel spatial branch, so this case keeps spatial + grain OFF and is fully
 * deterministic, matching gen_goldens.py's scan_spectral_blur case.
 *
 * It checks film_log_raw + film_density_cmy + final_rgb against the
 * scan_spectral_blur oracle golden bit-exact (max_abs <= 1e-4, rms <= 1e-5), and
 * confirms that turning the blur OFF (everything else equal) changes the output —
 * proving the blur is the sole driver of the difference vs. the golden and is
 * genuinely active end-to-end.
 *
 * Build (host) — full source set, run from the cpp root:
 *   g++ -std=c++17 -O2 -pthread -I <cpp_root> -I <tools/parity> \
 *     tests/test_spectral_blur_e2e.cpp spektra.cpp \
 *     model/*.cpp kernels/*.cpp io/*.cpp profiles/*.cpp \
 *     runtime/*.cpp runtime/stages/*.cpp \
 *     -o /tmp/test_spectral_blur_e2e
 * Run (argv[4] optionally overrides the goldens ROOT for a git worktree):
 *   /tmp/test_spectral_blur_e2e <asset_dir> <scan_spectral_blur_golden_dir> \
 *     <input.f64> [goldens_root]
 */
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "spkvec_io.h"
#include "spektra.h"

// Non-public cache observability/configuration seams from spektra.cpp. They are
// not declared in the public C header; shipping diagnostics uses the stable API.
extern uint64_t spk_test_tc_lut_cache_hits(spk_engine* eng);
extern uint64_t spk_test_tc_lut_cache_misses(spk_engine* eng);
extern uint64_t spk_test_tc_lut_cache_evictions(spk_engine* eng);
extern size_t spk_test_tc_lut_cache_dynamic_bytes(spk_engine* eng);
extern void spk_test_tc_lut_cache_set_dynamic_budget(spk_engine* eng,
                                                     size_t bytes);

namespace {

const char* kAssetDir = "/home/user/spektrafilm/src/spektrafilm/data";
const char* kBlurGoldenDir =
    "/home/user/Spectrafilmandroid/tools/parity/goldens/scan_spectral_blur";
const char* kInputF64 =
    "/home/user/Spectrafilmandroid/engine/spektra-core/src/main/cpp/tests/"
    "scan_portra_input_rgb.f64";

// The non-default sigma the golden was generated with (gen_goldens.py
// scan_spectral_blur). Band-index units, matching scipy.ndimage.gaussian_filter.
constexpr float kBlurSigma = 5.0f;

bool check(const char* label, const float* got, const std::vector<float>& gold) {
    double max_abs = 0.0, sse = 0.0;
    size_t argmax = 0;
    for (size_t i = 0; i < gold.size(); ++i) {
        double d = std::fabs(static_cast<double>(got[i]) -
                             static_cast<double>(gold[i]));
        if (d > max_abs) { max_abs = d; argmax = i; }
        sse += d * d;
    }
    double rms = std::sqrt(sse / static_cast<double>(gold.size()));
    const double tol_max_abs = 1e-4, tol_rms = 1e-5;
    bool pass = (max_abs <= tol_max_abs) && (rms <= tol_rms);
    std::printf("[%s] max_abs=%.6e (tol %.0e) rms=%.6e (tol %.0e) "
                "worst idx=%zu got=%.8f gold=%.8f -> %s\n",
                label, max_abs, tol_max_abs, rms, tol_rms, argmax,
                got[argmax], gold[argmax], pass ? "PASS" : "FAIL");
    return pass;
}

double max_abs_diff(const float* a, const float* b, size_t n) {
    double m = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double d = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        if (d > m) m = d;
    }
    return m;
}

// Build the scan_spectral_blur parameter set: scan_film route, spatial OFF,
// grain/glare/stochastic OFF (the blur is independent of the spatial branch), and
// the spectral Gaussian blur ON with the golden's sigma. Mirrors gen_goldens.py's
// scan_spectral_blur case.
void set_blur_params(spk_params* p) {
    p->film_profile = "kodak_portra_400";
    p->print_profile = "kodak_portra_endura";
    spk_default_params(p);
    p->exposure_compensation_ev = 0.0f;
    p->auto_exposure = 0;
    p->density_curve_gamma = 1.0f;
    p->grain_active = 0;
    p->halation_active = 0;  // spatial branch OFF (deactivate_spatial_effects=True)
    // Spatial effects are per-effect gated (zero = inert); express the
    // oracle's deactivate_spatial_effects by zeroing the nonzero defaults.
    p->dir_diffusion_size_um = 0.0f;
    p->scanner_unsharp[0] = 0.0f;
    p->scanner_unsharp[1] = 0.0f;
    p->dir_couplers_active = 1;
    p->glare_active = 0;
    p->scan_film = 1;
    p->output_color_space = SPK_CS_SRGB;
    p->output_cctf_encoding = 1;
    p->rgb_to_raw_method = SPK_RGB2RAW_HANATOS2025;
    p->preview_max_size = 640;
    // The feature under test: spectral-domain Gaussian blur of the spectra LUT.
    p->spectral_gaussian_blur = kBlurSigma;
}

}  // namespace

int main(int argc, char** argv) {
    std::string asset_dir = argc > 1 ? argv[1] : kAssetDir;
    std::string golden_dir = argc > 2 ? argv[2] : kBlurGoldenDir;
    std::string input_path = argc > 3 ? argv[3] : kInputF64;
    if (argc > 4) {
        std::string root = argv[4];
        golden_dir = root + "/scan_spectral_blur";
    }

    spk_engine* eng = nullptr;
    spk_status st = spk_engine_create(asset_dir.c_str(), &eng);
    if (st != SPK_OK) {
        std::fprintf(stderr, "engine create failed: %s\n", spk_status_str(st));
        return 2;
    }
    // One canonical 192x192x3 f64 tc_lut is ~885 kB (~864 KiB) including key/owner
    // allocation. A 1 MiB dynamic budget therefore makes this e2e exercise a
    // real eviction with only two distinct blur values.
    spk_test_tc_lut_cache_set_dynamic_budget(eng, 1024u * 1024u);
    const uint64_t tc_hits_before = spk_test_tc_lut_cache_hits(eng);
    const uint64_t tc_misses_before = spk_test_tc_lut_cache_misses(eng);

    spkvec::Array gold_rgb = spkvec::read(golden_dir + "/final_rgb.spkvec");
    spkvec::Array gold_logr = spkvec::read(golden_dir + "/film_log_raw.spkvec");
    spkvec::Array gold_cmy = spkvec::read(golden_dir + "/film_density_cmy.spkvec");
    const int height = static_cast<int>(gold_rgb.shape[0]);
    const int width = static_cast<int>(gold_rgb.shape[1]);
    const int npix = width * height;
    std::printf("Image: %dx%dx3 (%d pixels)\n", width, height, npix);

    std::vector<double> rgb64(static_cast<size_t>(npix) * 3);
    {
        std::ifstream in(input_path, std::ios::binary);
        if (!in) { std::fprintf(stderr, "cannot open %s\n", input_path.c_str()); return 2; }
        in.read(reinterpret_cast<char*>(rgb64.data()),
                static_cast<std::streamsize>(rgb64.size() * sizeof(double)));
        if (in.gcount() != static_cast<std::streamsize>(rgb64.size() * sizeof(double))) {
            std::fprintf(stderr, "input size mismatch\n");
            return 2;
        }
    }
    std::vector<float> rgb32(rgb64.begin(), rgb64.end());
    spk_image in_img{rgb32.data(), width, height, SPK_CS_PROPHOTO};

    spk_params p{};
    set_blur_params(&p);

    bool pass_logr = false, pass_cmy = false, pass_rgb = false;

    spk_image tap_logr{};
    st = spk_simulate_tap(eng, &in_img, &p, "film_log_raw", &tap_logr);
    if (st != SPK_OK) {
        std::fprintf(stderr, "tap(film_log_raw) failed: %s\n", spk_status_str(st));
    } else {
        pass_logr = check("spectral_blur_e2e film_log_raw", tap_logr.data, gold_logr.data);
        spk_image_free(&tap_logr);
    }

    spk_image tap_cmy{};
    st = spk_simulate_tap(eng, &in_img, &p, "film_density_cmy", &tap_cmy);
    if (st != SPK_OK) {
        std::fprintf(stderr, "tap(film_density_cmy) failed: %s\n", spk_status_str(st));
    } else {
        pass_cmy = check("spectral_blur_e2e film_density_cmy", tap_cmy.data, gold_cmy.data);
        spk_image_free(&tap_cmy);
    }

    spk_image out{};
    st = spk_simulate(eng, &in_img, &p, &out);
    if (st != SPK_OK) {
        std::fprintf(stderr, "spk_simulate failed: %s\n", spk_status_str(st));
        spk_engine_destroy(eng);
        return 2;
    }
    pass_rgb = check("spectral_blur_e2e final_rgb", out.data, gold_rgb.data);
    const bool cache_engaged =
        spk_test_tc_lut_cache_misses(eng) > tc_misses_before &&
        spk_test_tc_lut_cache_hits(eng) >= tc_hits_before + 2;
    std::printf("[tc_lut cache miss-then-hits] -> %s\n",
                cache_engaged ? "PASS" : "FAIL");

    // --- Sanity: blur OFF (everything else equal) changes the output, proving the
    //     spectral blur is genuinely active end-to-end and is the sole driver of
    //     the difference vs. the golden. ---
    spk_params p_off = p;
    p_off.spectral_gaussian_blur = 0.0f;
    spk_image out_off{};
    double blur_delta = 0.0;
    bool blur_active = false;
    st = spk_simulate(eng, &in_img, &p_off, &out_off);
    if (st != SPK_OK) {
        std::fprintf(stderr, "spk_simulate(blur off) failed: %s\n",
                     spk_status_str(st));
    } else {
        blur_delta = max_abs_diff(out.data, out_off.data,
                                  static_cast<size_t>(npix) * 3);
        blur_active = blur_delta > 1e-4;
        std::printf("[blur on vs off] max_abs = %.6e (must be > 0: the spectral "
                    "Gaussian blur smears the reconstructed spectra) -> %s\n",
                    blur_delta, blur_active ? "BLUR ACTIVE" : "BLUR INERT?!");
        spk_image_free(&out_off);
    }

    // Force the blur=5 entry out with one same-sized blur=6 entry, then rebuild
    // blur=5. The full final render must be byte-identical before/after eviction.
    const uint64_t evictions_before = spk_test_tc_lut_cache_evictions(eng);
    spk_params p_alt = p;
    p_alt.spectral_gaussian_blur = 6.0f;
    spk_image out_alt{};
    bool eviction_happened = false;
    bool eviction_bit_exact = false;
    st = spk_simulate(eng, &in_img, &p_alt, &out_alt);
    if (st == SPK_OK) {
        spk_image_free(&out_alt);
        eviction_happened =
            spk_test_tc_lut_cache_evictions(eng) > evictions_before &&
            spk_test_tc_lut_cache_dynamic_bytes(eng) <= 1024u * 1024u;
        spk_image rebuilt{};
        st = spk_simulate(eng, &in_img, &p, &rebuilt);
        if (st == SPK_OK) {
            const size_t bytes = static_cast<size_t>(npix) * 3 * sizeof(float);
            eviction_bit_exact = std::memcmp(out.data, rebuilt.data, bytes) == 0;
            spk_image_free(&rebuilt);
        }
    }
    std::printf("[tc_lut eviction happened] -> %s\n",
                eviction_happened ? "PASS" : "FAIL");
    std::printf("[tc_lut before==after eviction] -> %s\n",
                eviction_bit_exact ? "PASS (byte-identical)" : "FAIL");
    char cache_json[2048]{};
    const int cache_json_bytes = spk_engine_tc_lut_cache_stats_json(
        eng, cache_json, static_cast<int>(sizeof(cache_json)));
    char too_small[64] = {'x'};
    const bool diagnostics_reject_small_buffer =
        spk_engine_tc_lut_cache_stats_json(
            eng, too_small, static_cast<int>(sizeof(too_small))) == 0 &&
        too_small[0] == '\0';
    const std::string cache_diagnostics = cache_json;
    const bool shipping_diagnostics = cache_json_bytes > 0 &&
        cache_diagnostics.find("\"schema\":\"spk.tc_lut_cache.v1\"") !=
            std::string::npos &&
        cache_diagnostics.find("\"hits\":") != std::string::npos &&
        cache_diagnostics.find("\"misses\":") != std::string::npos &&
        cache_diagnostics.find("\"evictions\":") != std::string::npos &&
        cache_diagnostics.find("\"cache_held_bytes\":") != std::string::npos &&
        cache_diagnostics.find("\"memory_boundary\":\"post-build-cache-residency\"") !=
            std::string::npos;
    std::printf("[shipping tc_lut diagnostics] -> %s\n",
                shipping_diagnostics ? "PASS" : "FAIL");
    std::printf("[shipping diagnostics small buffer] -> %s\n",
                diagnostics_reject_small_buffer ? "PASS" : "FAIL");
    spk_image_free(&out);

    spk_engine_destroy(eng);
    bool all = pass_logr && pass_cmy && pass_rgb && blur_active &&
               cache_engaged && eviction_happened && eviction_bit_exact &&
               shipping_diagnostics && diagnostics_reject_small_buffer;
    std::printf("%s\n", all ? "ALL PASS" : "FAIL");
    return all ? 0 : 1;
}
