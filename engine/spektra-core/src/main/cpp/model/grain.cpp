/*
 * Spektrafilm for Android — native engine: film grain (AgX particle model).
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Implements model/grain.h. Mirrors grain.py::layer_particle_model
 * (poisson_binomial) + apply_grain_to_density.
 *
 * Verification is statistical (see tests/test_grain.cpp): the model is
 * mean-preserving and injects noise whose per-channel std matches the Python
 * oracle in magnitude. Exact element-wise parity is impossible because the C++
 * std::mt19937 RNG stream differs from numpy/Numba — only the *distributions*
 * (Poisson, Binomial) and the algorithm structure are matched.
 */
#include "model/grain.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "kernels/gaussian.h"
#include "kernels/parallel.h"
#include "kernels/stats.h"

namespace spk {

void layer_particle_model(const float* density, int npix, int width, int height,
                          double density_max, double n_particles_per_pixel,
                          double grain_uniformity, uint64_t seed, float* out) {
    layer_particle_model(density, npix, width, height, density_max,
                         n_particles_per_pixel, grain_uniformity, seed,
                         /*blur_particle=*/0.0, out);
}

void layer_particle_model(const float* density, int npix, int width, int height,
                          double density_max, double n_particles_per_pixel,
                          double grain_uniformity, uint64_t seed,
                          double blur_particle, float* out) {
    const double od_particle = density_max / n_particles_per_pixel;
    StatsRng rng(seed);
    for (int i = 0; i < npix; ++i) {
        // probability_of_development = clip(density/density_max, 1e-6, 1-1e-6)
        double p = static_cast<double>(density[i]) / density_max;
        if (p < 1e-6) p = 1e-6;
        else if (p > 1.0 - 1e-6) p = 1.0 - 1e-6;

        double saturation = 1.0 - p * grain_uniformity * (1.0 - 1e-6);
        double lam = n_particles_per_pixel / saturation;
        int64_t seeds = fast_poisson_one(lam, rng);
        int64_t dev = fast_binomial_one(seeds, p, rng);
        out[i] = static_cast<float>(static_cast<double>(dev) * od_particle * saturation);
    }

    // Per-particle dye-cloud blur: grain.py uses
    //   grain = fast_gaussian_filter(grain, blur_particle*sqrt(od_particle))
    // unconditionally when blur_particle > 0 (no >0.4 sigma threshold here).
    if (blur_particle > 0.0) {
        double sigma = blur_particle * std::sqrt(od_particle);
        gaussian_blur_plane(out, width, height, static_cast<float>(sigma));
    }
}

void add_micro_structure(float* inout, int npix, int width, int height,
                         const double micro_structure[2], double pixel_size_um,
                         uint64_t seed) {
    // add_micro_structure(density_cmy_out, micro_structure, pixel_size_um).
    double blur_pixel = micro_structure[0] / pixel_size_um;
    double sigma = micro_structure[1] * 0.001 / pixel_size_um;  // nm -> µm -> px
    if (sigma <= 0.05) return;

    // clumping = fast_lognormal_from_mean_std(ones, ones*sigma) — independent per
    // pixel AND per channel (np.ones_like(density_cmy_out) is (H,W,3)). One
    // deterministic stream produces the whole (npix,3) clumping field.
    std::vector<float> clumping(static_cast<size_t>(npix) * 3);
    StatsRng rng(seed);
    for (size_t i = 0; i < clumping.size(); ++i)
        clumping[i] = static_cast<float>(
            fast_lognormal_from_mean_std_one(1.0, sigma, rng));

    if (blur_pixel > 0.4) {
        gaussian_blur(clumping.data(), width, height, 3,
                      static_cast<float>(blur_pixel));
    }

    for (size_t i = 0; i < clumping.size(); ++i)
        inout[i] = static_cast<float>(static_cast<double>(inout[i]) * clumping[i]);
}

void apply_grain_to_density(const float* density_cmy, int npix, int width,
                            int height, double pixel_size_um,
                            const GrainParams& grain, float* out) {
    if (!grain.active) {
        if (out != density_cmy)
            std::copy(density_cmy, density_cmy + static_cast<size_t>(npix) * 3, out);
        return;
    }

    const double pixel_area_um2 = pixel_size_um * pixel_size_um;
    const int n_sub = grain.n_sub_layers > 1 ? grain.n_sub_layers : 1;

    double density_max[3], n_ppp[3];
    for (int c = 0; c < 3; ++c) {
        density_max[c] = grain.density_max_curves[c] + grain.density_min[c];
        double particle_area = grain.agx_particle_area_um2 * grain.agx_particle_scale[c];
        n_ppp[c] = pixel_area_um2 / particle_area;
        if (n_sub > 1) n_ppp[c] /= n_sub;
    }

    // Accumulator per channel-interleaved pixel (the Python code does
    // density_cmy += density_min before sampling, then out -= density_min after).
    std::vector<double> acc(static_cast<size_t>(npix) * 3, 0.0);

    // The 3×n_sub (channel × sublayer) calls are INDEPENDENT seeded RNG streams,
    // so run them concurrently — each into its own private output plane
    // (index k = c*n_sub + sl). The RNG walk WITHIN a call stays serial. We then
    // accumulate serially in canonical (c, sl) order, so the float add-order — and
    // the output — is byte-identical to the serial loop for any thread count. See
    // apply_grain_to_density_layers below / kernels/parallel.h for the contract.
    const int ntasks = 3 * n_sub;
    std::vector<float> planes(static_cast<size_t>(npix) * ntasks);
    parallel_tasks(ntasks, [&](int k) {
        const int c = k / n_sub, sl = k % n_sub;
        std::vector<float> shifted(static_cast<size_t>(npix));  // task-private
        // density_cmy[:,c] + density_min[c]
        for (int i = 0; i < npix; ++i)
            shifted[i] = static_cast<float>(
                static_cast<double>(density_cmy[i * 3 + c]) + grain.density_min[c]);
        uint64_t seed = static_cast<uint64_t>(grain.seed_base[c] + sl * 10 +
                                              grain.seed_offset);
        layer_particle_model(shifted.data(), npix, width, height,
                             density_max[c], n_ppp[c], grain.uniformity[c],
                             seed, &planes[static_cast<size_t>(k) * npix]);
    });
    for (int c = 0; c < 3; ++c) {
        for (int sl = 0; sl < n_sub; ++sl) {
            const float* lo = &planes[static_cast<size_t>(c * n_sub + sl) * npix];
            for (int i = 0; i < npix; ++i)
                acc[static_cast<size_t>(i) * 3 + c] += lo[i];
        }
        // /= n_sub_layers, then -= density_min[c]
        for (int i = 0; i < npix; ++i) {
            double v = acc[static_cast<size_t>(i) * 3 + c] / n_sub - grain.density_min[c];
            out[i * 3 + c] = static_cast<float>(v);
        }
    }

    // Final per-channel Gaussian blur (sigma in pixels). Python threshold: > 0.4.
    if (grain.blur > 0.4) {
        gaussian_blur(out, width, height, 3, static_cast<float>(grain.blur));
    }
}

void apply_grain_to_density_layers(const float* density_cmy_layers, int npix,
                                   int width, int height,
                                   const double* density_max_layers,
                                   double pixel_size_um, const GrainParams& grain,
                                   float* out) {
    // density_max_total[c] = sum over sublayers of density_max_layers[sl,c].
    double dmax_total[3] = {0, 0, 0};
    for (int sl = 0; sl < 3; ++sl)
        for (int c = 0; c < 3; ++c)
            dmax_total[c] += density_max_layers[sl * 3 + c];

    // Per-sublayer/channel derived quantities. `frac` (= density_max_fractions)
    // is consumed inline below for dmin_layers and n_ppp, so it is not stored.
    double dmin_layers[3][3];   // density_min_layers[sl][c]
    double dmax_lay[3][3];      // density_max_layers + density_min_layers
    double n_ppp[3][3];         // n_particles_per_pixel[sl][c]
    const double pixel_area_um2 = pixel_size_um * pixel_size_um;
    for (int sl = 0; sl < 3; ++sl) {
        for (int c = 0; c < 3; ++c) {
            double frac = density_max_layers[sl * 3 + c] / dmax_total[c];
            dmin_layers[sl][c] = frac * grain.density_min[c];
            dmax_lay[sl][c] = density_max_layers[sl * 3 + c] + dmin_layers[sl][c];
            double particle_area = grain.agx_particle_area_um2 *
                                   grain.agx_particle_scale[c] *
                                   grain.agx_particle_scale_layers[sl];
            n_ppp[sl][c] = pixel_area_um2 * frac / particle_area;
        }
    }

    // Accumulator per channel-interleaved pixel.
    std::vector<double> acc(static_cast<size_t>(npix) * 3, 0.0);

    // Each (channel, sublayer) is an INDEPENDENT seeded RNG stream, so the 9
    // layer_particle_model calls run concurrently — each into its own private
    // output plane (index k = c*3 + sl). The RNG walk WITHIN a call stays serial
    // (mt19937 consumed in raster order with data-dependent draw counts, so it
    // cannot be split across pixels bit-exactly). We then accumulate serially in
    // canonical (c, sl) order, so the float add-order — and therefore the output —
    // is byte-identical to the serial loop for any thread count. Grain is only
    // statistically matched to the oracle and excluded from the byte-exact
    // goldens; its parity gate is thread-invariance, which this preserves (see
    // kernels/parallel.h and tests/test_parallel.cpp).
    std::vector<float> planes(static_cast<size_t>(npix) * 9);
    parallel_tasks(9, [&](int k) {
        const int c = k / 3, sl = k % 3;
        std::vector<float> shifted(static_cast<size_t>(npix));  // task-private
        // density_cmy_layers[:,sl,c] += density_min_layers[sl,c]
        for (int i = 0; i < npix; ++i) {
            float v = density_cmy_layers[static_cast<size_t>(i) * 9 + sl * 3 + c];
            shifted[i] = static_cast<float>(static_cast<double>(v) +
                                            dmin_layers[sl][c]);
        }
        uint64_t seed = static_cast<uint64_t>(grain.seed_base[c] + sl * 10 +
                                              grain.seed_offset);
        layer_particle_model(shifted.data(), npix, width, height,
                             dmax_lay[sl][c], n_ppp[sl][c], grain.uniformity[c],
                             seed, grain.blur_dye_clouds_um,
                             &planes[static_cast<size_t>(k) * npix]);
    });
    for (int c = 0; c < 3; ++c) {
        for (int sl = 0; sl < 3; ++sl) {
            const float* lo = &planes[static_cast<size_t>(c * 3 + sl) * npix];
            for (int i = 0; i < npix; ++i)
                acc[static_cast<size_t>(i) * 3 + c] += lo[i];
        }
    }

    for (size_t i = 0; i < acc.size(); ++i)
        out[i] = static_cast<float>(acc[i]);

    // micro-structure clumping (operates on the accumulated grain, before the
    // density_min subtraction and final blur — matching grain.py order).
    uint64_t micro_seed = static_cast<uint64_t>(777 + grain.seed_offset);
    add_micro_structure(out, npix, width, height, grain.micro_structure,
                        pixel_size_um, micro_seed);

    // density_cmy_out -= density_min
    for (int i = 0; i < npix; ++i)
        for (int c = 0; c < 3; ++c)
            out[i * 3 + c] = static_cast<float>(static_cast<double>(out[i * 3 + c]) -
                                                grain.density_min[c]);

    // Final per-channel Gaussian blur. NOTE: the layers path threshold is > 0
    // (grain.py: `if grain_blur>0`), unlike the non-sublayer path's > 0.4.
    if (grain.blur > 0.0) {
        gaussian_blur(out, width, height, 3, static_cast<float>(grain.blur));
    }
}

}  // namespace spk
