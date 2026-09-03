/*
 * Spektrafilm for Android — host test for spk_bake_cube_lut.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * Bakes a 17^3 3D LUT for the default scan_film look and validates the emitted
 * .cube text:
 *   - header: LUT_3D_SIZE 17, TITLE, DOMAIN_MIN/MAX present
 *   - exactly N^3 RGB data lines
 *   - values sane (finite, within a generous [-0.01, 1.5] band)
 *   - non-degenerate: the LUT is neither the identity map nor a constant.
 * Then property-tests the INPUT SHAPER (shaper=1, sRGB): corner agreement with
 * the unshaped bake, a real overall difference (the argument is not ignored),
 * and per-entry agreement with a fine linear reference bake sampled at the
 * shaped coordinates — see the "shaper" block at the bottom.
 *
 * Build (host):
 *   g++ -std=c++17 -O2 -I <cpp_root> -I <tools/parity> \
 *     tests/test_bake_lut.cpp spektra.cpp \
 *     runtime/stages/filming.cpp runtime/stages/scanning.cpp \
 *     runtime/stages/printing.cpp runtime/params.cpp runtime/print_digest.cpp \
 *     model/couplers.cpp model/density_curves.cpp model/color_output.cpp \
 *     model/emulsion.cpp model/conversions.cpp model/spectral.cpp \
 *     model/color_filters.cpp model/grain.cpp model/diffusion.cpp model/glare.cpp \
 *     kernels/spectral_upsampling.cpp kernels/interp.cpp kernels/gaussian.cpp \
 *     kernels/exponential_filter.cpp kernels/stats.cpp \
 *     io/npy_lut.cpp profiles/profile.cpp \
 *     -o /tmp/test_bake_lut
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "spektra.h"

namespace {

const char* kAssetDir = "/home/user/spektrafilm/src/spektrafilm/data";

// Extract the integer value following a header token, or -1 if absent.
int header_int(const std::string& text, const std::string& token) {
    size_t pos = text.find(token);
    if (pos == std::string::npos) return -1;
    return std::atoi(text.c_str() + pos + token.size());
}

// Bake one .cube via the size-then-fill protocol; empty string on failure.
std::string bake_text(spk_engine* eng, const spk_params* p, int n,
                      int32_t shaper) {
    size_t needed = 0;
    spk_bake_cube_lut(eng, p, n, shaper, nullptr, 0, &needed);
    if (needed == 0) return std::string();
    std::vector<char> buf(needed);
    spk_status st = spk_bake_cube_lut(eng, p, n, shaper, buf.data(), buf.size(),
                                      &needed);
    if (st != SPK_OK) return std::string();
    return std::string(buf.data());
}

// Parse the N^3 "%f %f %f" data lines exactly the way main()'s block does:
// skip empty/'#' lines and header keyword lines (which begin with a letter).
std::vector<float> parse_cube_data(const std::string& text) {
    std::vector<float> data;
    std::istringstream iss(text);
    std::string ln;
    while (std::getline(iss, ln)) {
        if (ln.empty() || ln[0] == '#') continue;
        char c = ln[0];
        if (!(c == '-' || c == '.' || (c >= '0' && c <= '9'))) continue;
        float r, g, b;
        if (std::sscanf(ln.c_str(), "%f %f %f", &r, &g, &b) == 3) {
            data.push_back(r);
            data.push_back(g);
            data.push_back(b);
        }
    }
    return data;
}

// Lattice index -> [0,1] coordinate (exact: n-1 = 8 or 64 are powers of two).
double lincoord(int i, int n) { return static_cast<double>(i) / (n - 1); }

// sRGB EOTF — MUST mirror spektra.cpp's shaper_to_linear (shaper id 1) exactly:
// it reconstructs the linear input a shaped lattice coordinate stands for.
float srgb_eotf(float e) {
    if (e <= 0.04045f) return e / 12.92f;
    return static_cast<float>(std::pow((e + 0.055f) / 1.055f, 2.4));
}

// Trilinear lookup of channel `c` in a linear-domain n^3 cube (blue-fastest
// order, as emitted) at coordinate (x,y,z) in [0,1]^3.
double trilinear(const std::vector<float>& data, int n, double x, double y,
                 double z, int c) {
    auto axis = [&](double v, int* i0, double* f) {
        double u = v * (n - 1);
        if (u < 0) u = 0;
        if (u > n - 1) u = n - 1;
        int i = static_cast<int>(u);
        if (i > n - 2) i = n - 2;
        *i0 = i;
        *f = u - i;
    };
    int xi, yi, zi;
    double xf, yf, zf;
    axis(x, &xi, &xf);
    axis(y, &yi, &yf);
    axis(z, &zi, &zf);
    // Entry (ri,gi,bi) lives at flat index (ri*n + gi)*n + bi (blue-fastest).
    auto at = [&](int ri, int gi, int bi) -> double {
        return data[(static_cast<size_t>(ri) * n + gi) * n * 3 +
                    static_cast<size_t>(bi) * 3 + c];
    };
    double acc = 0.0;
    for (int dr = 0; dr < 2; ++dr)
        for (int dg = 0; dg < 2; ++dg)
            for (int db = 0; db < 2; ++db) {
                double w = (dr ? xf : 1.0 - xf) * (dg ? yf : 1.0 - yf) *
                           (db ? zf : 1.0 - zf);
                acc += w * at(xi + dr, yi + dg, zi + db);
            }
    return acc;
}

}  // namespace

int main(int argc, char** argv) {
    std::string asset_dir = argc > 1 ? argv[1] : kAssetDir;
    const int N = 17;

    spk_engine* eng = nullptr;
    spk_status st = spk_engine_create(asset_dir.c_str(), &eng);
    if (st != SPK_OK) {
        std::fprintf(stderr, "engine create failed: %s\n", spk_status_str(st));
        std::printf("FAIL\n");
        return 2;
    }

    spk_params p{};
    p.film_profile = "kodak_portra_400";
    p.print_profile = "kodak_portra_endura";
    spk_default_params(&p);
    p.scan_film = 1;                       // negative scan route
    p.auto_exposure = 0;
    p.output_color_space = SPK_CS_SRGB;
    p.output_cctf_encoding = 1;
    // Deliberately leave the stochastic/spatial toggles ON in params to prove the
    // bake forces them off internally (must still succeed & be deterministic).
    p.grain_active = 1;
    p.halation_active = 1;
    p.glare_active = 1;

    bool ok = true;

    // --- Pass 1: size the buffer (null out_text). ---------------------------
    size_t needed = 0;
    st = spk_bake_cube_lut(eng, &p, N, 0, nullptr, 0, &needed);
    if (st != SPK_OK || needed == 0) {
        std::fprintf(stderr, "sizing pass failed (needed=%zu st=%s)\n",
                     needed, spk_status_str(st));
        ok = false;
    }

    // --- Pass 2: actually bake. ---------------------------------------------
    std::vector<char> buf(needed > 0 ? needed : 1);
    st = spk_bake_cube_lut(eng, &p, N, 0, buf.data(), buf.size(), &needed);
    if (st != SPK_OK) {
        std::fprintf(stderr, "bake failed: %s\n", spk_status_str(st));
        spk_engine_destroy(eng);
        std::printf("FAIL\n");
        return 1;
    }

    std::string text(buf.data());

    // --- Determinism: a second bake must be byte-identical (no stochastics). -
    {
        size_t need2 = 0;
        spk_bake_cube_lut(eng, &p, N, 0, nullptr, 0, &need2);
        std::vector<char> buf2(need2);
        spk_bake_cube_lut(eng, &p, N, 0, buf2.data(), buf2.size(), &need2);
        if (std::string(buf2.data()) != text) {
            std::fprintf(stderr, "bake is non-deterministic across runs\n");
            ok = false;
        } else {
            std::printf("[determinism] two bakes byte-identical -> PASS\n");
        }
    }

    spk_engine_destroy(eng);

    // --- Header checks. ------------------------------------------------------
    int lut_size = header_int(text, "LUT_3D_SIZE ");
    bool has_title = text.find("TITLE") != std::string::npos;
    bool has_dmin = text.find("DOMAIN_MIN") != std::string::npos;
    bool has_dmax = text.find("DOMAIN_MAX") != std::string::npos;
    bool has_input_doc = text.find("linear ProPhoto") != std::string::npos;
    bool has_excluded_doc = text.find("EXCLUDED") != std::string::npos;
    std::printf("[header] LUT_3D_SIZE=%d TITLE=%d DOMAIN_MIN=%d DOMAIN_MAX=%d "
                "input_doc=%d excluded_doc=%d\n",
                lut_size, has_title, has_dmin, has_dmax, has_input_doc,
                has_excluded_doc);
    if (lut_size != N || !has_title || !has_dmin || !has_dmax ||
        !has_input_doc || !has_excluded_doc) {
        std::fprintf(stderr, "header check failed\n");
        ok = false;
    }

    // --- Parse the N^3 data lines (skip '#' comments and header keywords). ---
    const size_t expected = static_cast<size_t>(N) * N * N;
    std::vector<float> data;  // flat RGB
    data.reserve(expected * 3);
    {
        std::istringstream iss(text);
        std::string ln;
        while (std::getline(iss, ln)) {
            if (ln.empty() || ln[0] == '#') continue;
            // header keyword lines begin with a letter (TITLE/LUT_3D_SIZE/DOMAIN_*)
            char c = ln[0];
            if (!(c == '-' || c == '.' || (c >= '0' && c <= '9'))) continue;
            float r, g, b;
            if (std::sscanf(ln.c_str(), "%f %f %f", &r, &g, &b) == 3) {
                data.push_back(r);
                data.push_back(g);
                data.push_back(b);
            }
        }
    }
    std::printf("[data] parsed %zu triples (expected %zu)\n",
                data.size() / 3, expected);
    if (data.size() != expected * 3) {
        std::fprintf(stderr, "data-line count mismatch\n");
        ok = false;
    }

    // --- Sanity + non-degeneracy. -------------------------------------------
    if (data.size() == expected * 3) {
        // Reconstruct the identity lattice in the SAME blue-fastest order the
        // baker uses, so we can compare and prove the look is not the identity.
        double max_dev_from_identity = 0.0;
        double min_v = 1e9, max_v = -1e9;
        bool finite = true;
        size_t idx = 0;
        const float denom = static_cast<float>(N - 1);
        for (int ri = 0; ri < N; ++ri) {
            float rv = ri / denom;
            for (int gi = 0; gi < N; ++gi) {
                float gv = gi / denom;
                for (int bi = 0; bi < N; ++bi) {
                    float bv = bi / denom;
                    float or_ = data[idx * 3 + 0];
                    float og = data[idx * 3 + 1];
                    float ob = data[idx * 3 + 2];
                    if (!std::isfinite(or_) || !std::isfinite(og) || !std::isfinite(ob))
                        finite = false;
                    for (float v : {or_, og, ob}) {
                        if (v < min_v) min_v = v;
                        if (v > max_v) max_v = v;
                    }
                    max_dev_from_identity = std::fmax(max_dev_from_identity,
                        std::fmax(std::fabs(or_ - rv),
                                  std::fmax(std::fabs(og - gv), std::fabs(ob - bv))));
                    ++idx;
                }
            }
        }
        // Variance across the table (per channel) proves it is not constant.
        double mean[3] = {0, 0, 0};
        for (size_t i = 0; i < expected; ++i)
            for (int c = 0; c < 3; ++c) mean[c] += data[i * 3 + c];
        for (int c = 0; c < 3; ++c) mean[c] /= expected;
        double var = 0.0;
        for (size_t i = 0; i < expected; ++i)
            for (int c = 0; c < 3; ++c) {
                double d = data[i * 3 + c] - mean[c];
                var += d * d;
            }
        var /= (expected * 3);

        std::printf("[values] finite=%d range=[%.4f,%.4f] "
                    "max_dev_from_identity=%.4f variance=%.6f\n",
                    finite, min_v, max_v, max_dev_from_identity, var);

        if (!finite) { std::fprintf(stderr, "non-finite values\n"); ok = false; }
        if (min_v < -0.01 || max_v > 1.5) {
            std::fprintf(stderr, "values out of sane band\n"); ok = false;
        }
        if (max_dev_from_identity < 1e-3) {
            std::fprintf(stderr, "LUT is (near) identity — film look not applied\n");
            ok = false;
        }
        if (var < 1e-5) {
            std::fprintf(stderr, "LUT is (near) constant\n"); ok = false;
        }
    }

    // Print the header snippet for the record.
    {
        size_t cut = 0;
        for (int i = 0; i < 18 && cut != std::string::npos; ++i)
            cut = text.find('\n', cut + 1);
        std::printf("---- .cube header ----\n%s----\n",
                    text.substr(0, cut == std::string::npos ? text.size() : cut + 1).c_str());
    }

    // --- INPUT SHAPER property case (shaper=1, sRGB; no goldens). -----------
    //
    // The shaped lattice entry at index i per axis is the pipeline evaluated at
    // the linear coordinate eotf(i/(n-1)) (spektra.cpp shaper_to_linear), and
    // the bake is pointwise (spatial/stochastic effects + AE forced off), so
    // three properties must hold, each guarding a distinct failure mode:
    //  (i)  CORNERS: eotf(0)=0 and eotf(1)=1 exactly (0/12.92 and (1.055/1.055)
    //       ^2.4 are exact in float), so the all-0 and all-1 entries of a
    //       shaped bake must equal the unshaped bake's byte-for-byte (compared
    //       as the parsed float triples, which the fixed "%.6f" format maps
    //       1:1 onto the emitted bytes). Catches a shaper that warps the
    //       endpoints (domain drift).
    //  (ii) DIFFERENCE: away from the corners eotf(x) != x, so the shaped bake
    //       must differ from the unshaped one overall. Catches the historical
    //       bug class of a silently ignored argument.
    // (iii) CONSISTENCY: a shaped entry approximates trilinear interpolation of
    //       a FINE linear reference bake (N=65) at (eotf(ri/8), eotf(gi/8),
    //       eotf(bi/8)) — same pipeline, so the residual is only trilinear
    //       interpolation error of the 65-grid. Catches a shaper applied to
    //       the wrong domain / axis order / wrong transfer curve, all of which
    //       displace samples by far more than interpolation error.
    {
        const int NS = 9;    // small shaped/linear probe lattice
        const int NR = 65;   // fine linear reference (9's grid nests in 65's)
        spk_engine* eng2 = nullptr;
        st = spk_engine_create(asset_dir.c_str(), &eng2);
        if (st != SPK_OK) {
            std::fprintf(stderr, "engine2 create failed: %s\n", spk_status_str(st));
            std::printf("FAIL\n");
            return 2;
        }
        const std::string lin_text = bake_text(eng2, &p, NS, /*shaper=*/0);
        const std::string shp_text = bake_text(eng2, &p, NS, /*shaper=*/1);
        const std::string ref_text = bake_text(eng2, &p, NR, /*shaper=*/0);
        spk_engine_destroy(eng2);

        std::vector<float> lin = parse_cube_data(lin_text);
        std::vector<float> shp = parse_cube_data(shp_text);
        std::vector<float> ref = parse_cube_data(ref_text);
        const size_t ns3 = static_cast<size_t>(NS) * NS * NS;
        const size_t nr3 = static_cast<size_t>(NR) * NR * NR;
        if (lin.size() != ns3 * 3 || shp.size() != ns3 * 3 ||
            ref.size() != nr3 * 3) {
            std::fprintf(stderr,
                         "[shaper] bake/parse size mismatch: lin=%zu shp=%zu "
                         "ref=%zu\n", lin.size(), shp.size(), ref.size());
            ok = false;
        } else {
            // (i) corner byte-equality (entry 0 and entry NS^3-1).
            const size_t last = (ns3 - 1) * 3;
            const bool c0 = std::memcmp(&lin[0], &shp[0], 3 * sizeof(float)) == 0;
            const bool c1 =
                std::memcmp(&lin[last], &shp[last], 3 * sizeof(float)) == 0;
            std::printf("[shaper] corner (0,0,0) byte-equal=%d corner (1,1,1) "
                        "byte-equal=%d -> %s\n", c0, c1,
                        (c0 && c1) ? "PASS" : "FAIL");
            if (!c0 || !c1) ok = false;

            // (ii) overall difference.
            double max_diff = 0.0;
            for (size_t i = 0; i < ns3 * 3; ++i) {
                double d = std::fabs(static_cast<double>(shp[i]) - lin[i]);
                if (d > max_diff) max_diff = d;
            }
            std::printf("[shaper] max |shaped-linear| = %.4f (require > 0.01) "
                        "-> %s\n", max_diff, max_diff > 0.01 ? "PASS" : "FAIL");
            if (!(max_diff > 0.01)) ok = false;

            // Self-check of the reference machinery: the NS=9 linear lattice
            // coordinates i/8 = 8i/64 land EXACTLY on the NR=65 grid, so
            // trilinear(ref) there must reproduce the linear bake up to the
            // "%.6f" print quantization (5e-7 per value; weights sum to 1, so
            // the interpolated quantization error cannot exceed it, plus 5e-7
            // for the probe's own print). Bound 2e-6 = that 1e-6 with 2x
            // margin. A wrong axis order / flat index in trilinear() would
            // blow this up immediately, so (iii) below can be trusted.
            double max_lin_err = 0.0;
            // (iii) shaped entries vs reference at the shaped coordinates.
            double max_shp_err = 0.0;
            int worst[4] = {0, 0, 0, 0};
            int over_1e3 = 0, over_5e3 = 0, over_1e2 = 0;
            for (int ri = 0; ri < NS; ++ri) {
                const double rl = lincoord(ri, NS), rs = srgb_eotf(
                    static_cast<float>(rl));
                for (int gi = 0; gi < NS; ++gi) {
                    const double gl = lincoord(gi, NS), gs = srgb_eotf(
                        static_cast<float>(gl));
                    for (int bi = 0; bi < NS; ++bi) {
                        const double bl = lincoord(bi, NS), bs = srgb_eotf(
                            static_cast<float>(bl));
                        const size_t at =
                            ((static_cast<size_t>(ri) * NS + gi) * NS + bi) * 3;
                        for (int c = 0; c < 3; ++c) {
                            double e1 = std::fabs(
                                trilinear(ref, NR, rl, gl, bl, c) - lin[at + c]);
                            if (e1 > max_lin_err) max_lin_err = e1;
                            double e2 = std::fabs(
                                trilinear(ref, NR, rs, gs, bs, c) - shp[at + c]);
                            if (e2 > 1e-3) ++over_1e3;
                            if (e2 > 5e-3) ++over_5e3;
                            if (e2 > 1e-2) ++over_1e2;
                            if (e2 > max_shp_err) {
                                max_shp_err = e2;
                                worst[0] = ri; worst[1] = gi;
                                worst[2] = bi; worst[3] = c;
                            }
                        }
                    }
                }
            }
            std::printf("[shaper] on-grid linear self-check max err = %.3e "
                        "(require <= 2e-6) -> %s\n", max_lin_err,
                        max_lin_err <= 2e-6 ? "PASS" : "FAIL");
            if (!(max_lin_err <= 2e-6)) ok = false;

            // TOLERANCE for (iii): the residual is pure trilinear interpolation
            // error of the 65-grid at off-grid points (the on-grid self-check
            // above is EXACT, so the machinery contributes nothing). Measured
            // on this pipeline (portra scan route, sRGB-encoded output):
            // max err 2.61e-2, with only 12 of 2187 values above 1e-2, all in
            // the darkest reference cells (linear input < ~0.06) where the
            // negative's toe and the output CCTF bend hardest between
            // 1/64-spaced samples. Bound 6e-2 = observed max with ~2.3x margin
            // for profile/toolchain drift. What that still catches: an axis
            // swap, a double-encode, or OETF-instead-of-EOTF displace
            // mid-lattice samples by 0.1..0.5 in input and O(0.1) in output —
            // far above 6e-2. Honest limit: a near-miss curve (pure gamma 2.2
            // vs sRGB piecewise) displaces dark inputs by <= ~3e-3 and is NOT
            // distinguishable from interpolation error at this reference
            // density; the corner check (i) doesn't pin it either (any sane
            // shaper fixes 0 and 1). Tightening that would need a much finer
            // reference bake (error ~ h^2, cost ~ n^3) for little gate value.
            const double kTol = 6e-2;
            std::printf("[shaper] shaped-vs-reference max err = %.3e at lattice "
                        "(%d,%d,%d) ch=%d; count>1e-3: %d  >5e-3: %d  >1e-2: %d "
                        "(of %zu)\n",
                        max_shp_err, worst[0], worst[1], worst[2], worst[3],
                        over_1e3, over_5e3, over_1e2, ns3 * 3);
            std::printf("[shaper] shaped-vs-reference (require <= %.0e) -> %s\n",
                        kTol, max_shp_err <= kTol ? "PASS" : "FAIL");
            if (!(max_shp_err <= kTol)) ok = false;
        }
    }

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
