/*
 * Spektrafilm for Android — host parity test for the optical diffusion filter.
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
 * Exercises model/diffusion.h::apply_diffusion_filter_um for a NON-default
 * diffusion-filter setting (active=True, family=black_pro_mist, strength=0.5,
 * spatial_scale=1.0, halo_warmth=0.0). Reads the deterministic float64 input
 * irradiance fixture, runs the native diffusion filter (double internals,
 * float32 on store), and compares to the oracle golden produced by
 * tools/parity/gen_diffusion_golden.py, reporting max_abs / rms and PASS/FAIL
 * against the manifest tolerances (max_abs <= 1e-4, rms <= 1e-5).
 *
 * The golden/input paths default to the REPO-ROOT
 * /home/user/Spectrafilmandroid/tools/parity/goldens/diffusion_bpm so the test
 * runs under CI's engine-parity job; argv[1] optionally overrides the goldens
 * ROOT (the dir containing diffusion_bpm/) for a git-worktree run before the
 * golden lands in the repo.
 *
 * Build (host) — from the cpp root:
 *   g++ -std=c++17 -O2 -I. -I <tools/parity> \
 *     tests/test_diffusion.cpp model/diffusion.cpp kernels/exponential_filter.cpp \
 *     -o /tmp/test_diffusion
 * Run:
 *   /tmp/test_diffusion [goldens_root]
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#include "spkvec_io.h"

#include "model/diffusion.h"
#include "runtime/memory_budget.h"

namespace {

const char* kGoldensRoot =
    "/home/user/Spectrafilmandroid/tools/parity/goldens";

// Must match tools/parity/gen_diffusion_golden.py.
constexpr int kSize = 64;
constexpr double kFilmFormatMm = 35.0;

}  // namespace

int main(int argc, char** argv) {
    std::string root = (argc > 1) ? argv[1] : kGoldensRoot;
    std::string case_dir = root + "/diffusion_bpm";
    std::string input_path = case_dir + "/input_rgb.f64";
    std::string golden_path = case_dir + "/diffusion_out.spkvec";

    const int w = kSize, h = kSize;
    const int npix = w * h;
    const size_t n = static_cast<size_t>(npix) * 3;

    // Load the float64 input irradiance fixture (C-contiguous (h,w,3) doubles).
    std::vector<double> raw(n);
    {
        std::ifstream in(input_path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "cannot open input %s\n", input_path.c_str());
            return 2;
        }
        in.read(reinterpret_cast<char*>(raw.data()),
                static_cast<std::streamsize>(n * sizeof(double)));
        if (in.gcount() != static_cast<std::streamsize>(n * sizeof(double))) {
            std::fprintf(stderr, "input size mismatch (%s)\n", input_path.c_str());
            return 2;
        }
    }

    spkvec::Array gold = spkvec::read(golden_path);
    if (gold.data.size() != n) {
        std::fprintf(stderr, "golden size mismatch: got %zu, expected %zu\n",
                     gold.data.size(), n);
        return 2;
    }

    // NON-default diffusion-filter setting under test (mirrors the generator).
    spk::DiffusionFilterParams df;
    df.active = true;
    df.family = spk::DiffusionFamily::kBlackProMist;
    df.strength = 0.5;
    df.spatial_scale = 1.0;
    df.halo_warmth = 0.0;
    const double pixel_size_um = kFilmFormatMm * 1000.0 / static_cast<double>(kSize);

    spk::apply_diffusion_filter_um(raw.data(), w, h, df, pixel_size_um);

    // Store float32 (matching the parity convention), then compare to the golden.
    double max_abs = 0.0, sse = 0.0;
    size_t argmax = 0;
    for (size_t i = 0; i < n; ++i) {
        float got = static_cast<float>(raw[i]);
        double d = std::fabs(static_cast<double>(got) -
                             static_cast<double>(gold.data[i]));
        if (d > max_abs) { max_abs = d; argmax = i; }
        sse += d * d;
    }
    double rms = std::sqrt(sse / static_cast<double>(n));
    const double tol_max_abs = 1e-4, tol_rms = 1e-5;
    bool pass = (max_abs <= tol_max_abs) && (rms <= tol_rms);

    std::printf("Image: %dx%dx3 (%d pixels)  pixel_size_um=%.6f\n", w, h, npix,
                pixel_size_um);
    std::printf("[diffusion_bpm] max_abs = %.6e (tol %.0e)\n", max_abs, tol_max_abs);
    std::printf("[diffusion_bpm] rms     = %.6e (tol %.0e)\n", rms, tol_rms);
    std::printf("[diffusion_bpm] worst idx=%zu: got=%.8f golden=%.8f -> %s\n",
                argmax, static_cast<double>(static_cast<float>(raw[argmax])),
                static_cast<double>(gold.data[argmax]), pass ? "PASS" : "FAIL");
    // #160: the FFT's scratch is admitted through the process memory budget, and a
    // refusal must surface as OOM. It must NOT quietly fall through to the direct
    // O(w*h*ks^2) loop -- that loop is ~10.9 hours at 12 MP, so turning memory
    // pressure into it would be far worse than failing the render.
    {
        auto& budget = spk::memory::process_memory_budget();
        const std::uint64_t restore = budget.limit_bytes();
        budget.set_limit_bytes(1024);            // below the smallest transform's scratch
        std::vector<double> starved(n, 0.25);
        bool denied = false;
        try {
            spk::apply_diffusion_filter_um(starved.data(), w, h, df, pixel_size_um);
        } catch (const std::bad_alloc&) {
            denied = true;
        }
        budget.set_limit_bytes(restore);
        std::printf("[diffusion_bpm] budget denial -> %s\n",
                    denied ? "PASS (bad_alloc)" : "FAIL (silent fallback?)");
        pass = pass && denied;
    }

    std::printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
