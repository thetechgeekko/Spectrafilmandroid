// SPDX-FileCopyrightText: 2026 Spektrafilm Android contributors
// SPDX-License-Identifier: GPL-3.0-only
// Small f64 reference for the focused resident filming -> printing -> scan gate.
// Keep this test helper compiled without -ffast-math: it intentionally evaluates
// the production shaders' operation order against the same fp32 inputs/tables in
// IEEE double precision.
#ifndef SPK_GPU_TESTS_POINTWISE_CHAIN_ORACLE_H
#define SPK_GPU_TESTS_POINTWISE_CHAIN_ORACLE_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "gpu/vulkan_compute.h"

namespace spk::gpu::test {

struct PointwiseOracleError {
    double max_abs = 0.0;
    double rms = 0.0;
};

namespace detail {

inline double finite_or(double value, double fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

inline double clamp01(double value) noexcept {
    return std::min(std::max(value, 0.0), 1.0);
}

inline double mitchell(double t) noexcept {
    constexpr double b = 1.0 / 3.0;
    constexpr double c = 1.0 / 3.0;
    const double x = std::fabs(t);
    if (x < 1.0) {
        return ((12.0 - 9.0 * b - 6.0 * c) * x * x * x +
                (-18.0 + 12.0 * b + 6.0 * c) * x * x + (6.0 - 2.0 * b)) /
               6.0;
    }
    if (x < 2.0) {
        return ((-b - 6.0 * c) * x * x * x +
                (6.0 * b + 30.0 * c) * x * x +
                (-12.0 * b - 48.0 * c) * x + (8.0 * b + 24.0 * c)) /
               6.0;
    }
    return 0.0;
}

inline int mirror_index(int index, int edge) noexcept {
    if (index < 0) return -index;
    if (index >= edge) return 2 * (edge - 1) - index;
    return index;
}

inline double cubic_base_fraction(double coordinate, int edge, int& base) noexcept {
    coordinate = std::min(std::max(coordinate, 0.0), static_cast<double>(edge - 1));
    if (coordinate >= static_cast<double>(edge - 1)) {
        base = edge - 2;
        return 1.0;
    }
    base = std::clamp(static_cast<int>(std::floor(coordinate)), 0, edge - 2);
    return coordinate - static_cast<double>(base);
}

inline double interpolate(double x, const PointwiseTableSpan& axis,
                          const PointwiseTableSpan& curve, uint32_t points,
                          uint32_t channel) noexcept {
    const double first = axis.data[channel];
    const double last = axis.data[(points - 1u) * 3u + channel];
    if (x <= first) return curve.data[channel];
    if (x >= last) return curve.data[(points - 1u) * 3u + channel];
    uint32_t low = 0;
    uint32_t high = points - 1u;
    while (high - low > 1u) {
        const uint32_t middle = low + (high - low) / 2u;
        if (x < axis.data[middle * 3u + channel]) {
            high = middle;
        } else {
            low = middle;
        }
    }
    const double x0 = axis.data[low * 3u + channel];
    const double x1 = axis.data[high * 3u + channel];
    const double y0 = curve.data[low * 3u + channel];
    const double y1 = curve.data[high * 3u + channel];
    return y0 + (x - x0) / (x1 - x0) * (y1 - y0);
}

inline double srgb_cctf(double value) noexcept {
    return value <= 0.0031308
               ? 12.92 * value
               : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

}  // namespace detail

inline void render_pointwise_chain_f64(const PointwiseChainRequest& request,
                                       double* output_rgb) noexcept {
    // These are deliberately float constants: GLSL rounds them to fp32 before
    // the f64 reference upcasts, matching the validated M2 oracle method.
    constexpr float kProPhotoToXyzD55[9] = {
        0.7815775876144749f,    0.12427353211547089f,  0.05084064074531416f,
        0.28106991658512925f,   0.7111246050020191f,   0.0078043503519031375f,
        0.0008785229438793953f, 0.0012166783269637077f, 0.9190442562432091f,
    };
    constexpr uint32_t kBands = 81;
    const uint32_t edge = request.film.tc_edge;

    for (uint32_t pixel = 0; pixel < request.pixel_count; ++pixel) {
        const double r = detail::finite_or(request.input_rgb[pixel * 3u], 0.0);
        const double g = detail::finite_or(request.input_rgb[pixel * 3u + 1u], 0.0);
        const double b = detail::finite_or(request.input_rgb[pixel * 3u + 2u], 0.0);
        const double x = static_cast<double>(kProPhotoToXyzD55[0]) * r +
                         static_cast<double>(kProPhotoToXyzD55[1]) * g +
                         static_cast<double>(kProPhotoToXyzD55[2]) * b;
        const double y = static_cast<double>(kProPhotoToXyzD55[3]) * r +
                         static_cast<double>(kProPhotoToXyzD55[4]) * g +
                         static_cast<double>(kProPhotoToXyzD55[5]) * b;
        const double z = static_cast<double>(kProPhotoToXyzD55[6]) * r +
                         static_cast<double>(kProPhotoToXyzD55[7]) * g +
                         static_cast<double>(kProPhotoToXyzD55[8]) * b;
        const double brightness = x + y + z;
        const double denominator = std::max(brightness, 1e-10);
        const double chroma_x = detail::clamp01(x / denominator);
        const double chroma_y = detail::clamp01(y / denominator);
        const double quad_y = detail::clamp01(
            chroma_y / std::max(1.0 - chroma_x, 1e-10));
        const double quad_x = detail::clamp01((1.0 - chroma_x) * (1.0 - chroma_x));

        int x_base = 0;
        int y_base = 0;
        const double scale = static_cast<double>(edge - 1u);
        const double x_fraction =
            detail::cubic_base_fraction(quad_x * scale, static_cast<int>(edge), x_base);
        const double y_fraction =
            detail::cubic_base_fraction(quad_y * scale, static_cast<int>(edge), y_base);
        const double wx[4] = {detail::mitchell(x_fraction + 1.0),
                              detail::mitchell(x_fraction),
                              detail::mitchell(x_fraction - 1.0),
                              detail::mitchell(x_fraction - 2.0)};
        const double wy[4] = {detail::mitchell(y_fraction + 1.0),
                              detail::mitchell(y_fraction),
                              detail::mitchell(y_fraction - 1.0),
                              detail::mitchell(y_fraction - 2.0)};
        double raw[3]{};
        double weight_sum = 0.0;
        for (int i = 0; i < 4; ++i) {
            const int xi = detail::mirror_index(x_base - 1 + i, static_cast<int>(edge));
            for (int j = 0; j < 4; ++j) {
                const int yj =
                    detail::mirror_index(y_base - 1 + j, static_cast<int>(edge));
                const double weight = wx[i] * wy[j];
                weight_sum += weight;
                const size_t cell =
                    (static_cast<size_t>(xi) * edge + static_cast<size_t>(yj)) * 3u;
                for (size_t channel = 0; channel < 3; ++channel) {
                    raw[channel] += weight * request.film.tc_lut.data[cell + channel];
                }
            }
        }
        for (double& value : raw) value /= weight_sum;

        double log_raw[3]{};
        double developed[3]{};
        double silver[3]{};
        for (uint32_t channel = 0; channel < 3; ++channel) {
            log_raw[channel] = std::log10(
                std::max(raw[channel] * brightness * request.film.exposure_multiplier,
                         0.0) +
                1e-10);
            developed[channel] = detail::interpolate(
                log_raw[channel], request.film.develop_axis,
                request.film.develop_curve, request.film.curve_points, channel);
            silver[channel] = developed[channel] + request.film.coupler_shift *
                                                       developed[channel] * developed[channel];
        }
        double film_density[3]{};
        for (uint32_t channel = 0; channel < 3; ++channel) {
            const double inhibition =
                silver[0] * request.film.coupler_matrix[channel] +
                silver[1] * request.film.coupler_matrix[3u + channel] +
                silver[2] * request.film.coupler_matrix[6u + channel];
            film_density[channel] = detail::interpolate(
                log_raw[channel] - inhibition, request.film.dir_axis,
                request.film.dir_curve, request.film.curve_points, channel);
        }

        double print_exposure[3]{};
        for (uint32_t band = 0; band < kBands; ++band) {
            const size_t base = static_cast<size_t>(band) * 3u;
            double density = 0.0;
            for (size_t channel = 0; channel < 3; ++channel) {
                density += film_density[channel] * request.print.dye.data[base + channel];
            }
            const double transmittance = std::pow(10.0, -density);
            for (size_t channel = 0; channel < 3; ++channel) {
                print_exposure[channel] +=
                    transmittance *
                    request.print.illuminant_sensitivity.data[base + channel];
            }
        }
        double print_density[3]{};
        for (uint32_t channel = 0; channel < 3; ++channel) {
            const double exposed = print_exposure[channel] * request.print.midgray +
                                   request.print.preflash[channel];
            double log_exposure = std::log10(std::max(exposed, 0.0) + 1e-10);
            const double round_trip =
                std::pow(10.0, log_exposure) * request.print.exposure_multiplier;
            log_exposure = std::log10(std::max(round_trip, 0.0) + 1e-10);
            print_density[channel] = detail::interpolate(
                log_exposure, request.print.paper_axis, request.print.paper_curve,
                request.print.curve_points, channel);
        }

        double xyz[3]{};
        for (uint32_t band = 0; band < kBands; ++band) {
            const size_t base = static_cast<size_t>(band) * 3u;
            double density = 0.0;
            for (size_t channel = 0; channel < 3; ++channel) {
                density += print_density[channel] * request.scan.dye.data[base + channel];
            }
            const double transmittance = std::pow(10.0, -density);
            for (size_t channel = 0; channel < 3; ++channel) {
                xyz[channel] +=
                    transmittance * request.scan.illuminant_cmf.data[base + channel];
            }
        }
        for (size_t channel = 0; channel < 3; ++channel) {
            const size_t row = channel * 3u;
            const double linear = request.scan.xyz_to_rgb[row] * xyz[0] +
                                  request.scan.xyz_to_rgb[row + 1u] * xyz[1] +
                                  request.scan.xyz_to_rgb[row + 2u] * xyz[2];
            output_rgb[pixel * 3u + channel] = detail::clamp01(
                detail::finite_or(detail::srgb_cctf(linear), 0.0));
        }
    }
}

inline PointwiseOracleError pointwise_oracle_error(const float* actual,
                                                    const double* expected,
                                                    size_t component_count) noexcept {
    PointwiseOracleError error{};
    double squared_sum = 0.0;
    for (size_t i = 0; i < component_count; ++i) {
        const double delta = std::fabs(static_cast<double>(actual[i]) - expected[i]);
        error.max_abs = std::max(error.max_abs, delta);
        squared_sum += delta * delta;
    }
    error.rms = std::sqrt(squared_sum / static_cast<double>(component_count));
    return error;
}

}  // namespace spk::gpu::test

#endif  // SPK_GPU_TESTS_POINTWISE_CHAIN_ORACLE_H
