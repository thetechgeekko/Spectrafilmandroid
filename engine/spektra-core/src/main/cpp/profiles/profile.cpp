/*
 * Spektrafilm for Android — native engine: film/print Profile struct + loader.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm.
 */
#include "profile.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "json_min.h"
#include "model/color_output.h"

namespace spk {

namespace {

constexpr size_t kSpectralRows = 81;
constexpr size_t kDensityRows = 256;
constexpr size_t kChannels = 3;
constexpr size_t kWindowParams = 4;
constexpr size_t kSurfaceRows = 3;
constexpr size_t kSurfaceCols = 15;

enum class NullPolicy { Reject, PreserveAsNan };

[[noreturn]] void profile_error(const std::string& field,
                                const std::string& problem) {
    throw std::runtime_error("Profile: " + field + " " + problem);
}

size_t checked_product(size_t a, size_t b, const std::string& field) {
    if (a != 0 && b > std::numeric_limits<size_t>::max() / a)
        profile_error(field, "size overflow");
    return a * b;
}

void require_size(const json::Value& value, size_t expected,
                  const std::string& field) {
    if (!value.is_array()) profile_error(field, "must be an array");
    if (value.size() != expected)
        profile_error(field, "must contain exactly " + std::to_string(expected) +
                                 " elements");
}

double read_number(const json::Value& value, const std::string& field,
                   NullPolicy null_policy, bool require_float_range) {
    if (!value.is_number()) profile_error(field, "must be numeric");
    if (value.is_null_number) {
        if (null_policy == NullPolicy::PreserveAsNan)
            return std::numeric_limits<double>::quiet_NaN();
        profile_error(field, "must not be null");
    }
    const double number = value.number;
    if (!std::isfinite(number)) profile_error(field, "must be finite");
    if (require_float_range &&
        std::fabs(number) > static_cast<double>(std::numeric_limits<float>::max()))
        profile_error(field, "is outside finite float range");
    return number;
}

float read_float(const json::Value& value, const std::string& field,
                 NullPolicy null_policy = NullPolicy::Reject) {
    const double number = read_number(value, field, null_policy,
                                      /*require_float_range=*/true);
    if (std::isnan(number)) return std::numeric_limits<float>::quiet_NaN();
    const float result = static_cast<float>(number);
    if (!std::isfinite(result)) profile_error(field, "is outside finite float range");
    return result;
}

std::vector<float> read_vec(const json::Value& value, size_t expected,
                            const std::string& field,
                            NullPolicy null_policy = NullPolicy::Reject) {
    require_size(value, expected, field);
    std::vector<float> out;
    out.reserve(expected);
    for (size_t i = 0; i < expected; ++i)
        out.push_back(read_float(value[i], field + "[" + std::to_string(i) + "]",
                                 null_policy));
    return out;
}

void validate_vec(const json::Value& value, size_t expected,
                  const std::string& field, NullPolicy null_policy) {
    require_size(value, expected, field);
    for (size_t i = 0; i < expected; ++i) {
        (void)read_float(value[i], field + "[" + std::to_string(i) + "]",
                         null_policy);
    }
}

std::vector<float> read_matrix3(const json::Value& value, size_t expected_rows,
                                const std::string& field,
                                NullPolicy null_policy = NullPolicy::Reject) {
    require_size(value, expected_rows, field);
    const size_t elements = checked_product(expected_rows, kChannels, field);
    std::vector<float> out;
    out.reserve(elements);
    for (size_t row = 0; row < expected_rows; ++row) {
        const std::string row_field = field + "[" + std::to_string(row) + "]";
        require_size(value[row], kChannels, row_field);
        for (size_t channel = 0; channel < kChannels; ++channel) {
            out.push_back(read_float(
                value[row][channel],
                row_field + "[" + std::to_string(channel) + "]", null_policy));
        }
    }
    return out;
}

std::vector<float> read_tensor33(const json::Value& value, size_t expected_rows,
                                 const std::string& field) {
    require_size(value, expected_rows, field);
    const size_t elements = checked_product(expected_rows, 9, field);
    std::vector<float> out;
    out.reserve(elements);
    for (size_t row = 0; row < expected_rows; ++row) {
        const std::string row_field = field + "[" + std::to_string(row) + "]";
        require_size(value[row], kChannels, row_field);
        for (size_t layer = 0; layer < kChannels; ++layer) {
            const std::string layer_field =
                row_field + "[" + std::to_string(layer) + "]";
            require_size(value[row][layer], kChannels, layer_field);
            for (size_t channel = 0; channel < kChannels; ++channel) {
                out.push_back(read_float(
                    value[row][layer][channel],
                    layer_field + "[" + std::to_string(channel) + "]"));
            }
        }
    }
    return out;
}

std::vector<double> read_fixed_double_matrix(const json::Value& value,
                                              size_t rows, size_t cols,
                                              const std::string& field) {
    require_size(value, rows, field);
    const size_t elements = checked_product(rows, cols, field);
    std::vector<double> out;
    out.reserve(elements);
    for (size_t row = 0; row < rows; ++row) {
        const std::string row_field = field + "[" + std::to_string(row) + "]";
        require_size(value[row], cols, row_field);
        for (size_t col = 0; col < cols; ++col) {
            out.push_back(read_number(
                value[row][col], row_field + "[" + std::to_string(col) + "]",
                NullPolicy::Reject, /*require_float_range=*/true));
        }
    }
    return out;
}

size_t require_rectangular_matrix(const json::Value& value, size_t rows,
                                  const std::string& field) {
    require_size(value, rows, field);
    if (!value[0].is_array())
        profile_error(field + "[0]", "must be an array");
    const size_t cols = value[0].size();
    for (size_t row = 0; row < rows; ++row) {
        require_size(value[row], cols,
                     field + "[" + std::to_string(row) + "]");
    }
    return cols;
}

const std::string& require_string(const json::Value& object, const char* key,
                                  const std::string& field) {
    const json::Value& value = object.at(key);
    if (!value.is_string()) profile_error(field, "must be a string");
    return value.as_string();
}

bool is_one_of(std::string_view value,
               std::initializer_list<std::string_view> allowed) {
    for (const std::string_view candidate : allowed) {
        if (value == candidate) return true;
    }
    return false;
}

void require_allowlisted(const std::string& value, const std::string& field,
                         std::initializer_list<std::string_view> allowed) {
    if (!is_one_of(value, allowed))
        profile_error(field, "has unsupported value '" + value + "'");
}

}  // namespace

Profile load_profile_string(const std::string& json_text) {
    const json::ValuePtr root = json::parse(json_text);
    const json::Value& r = *root;
    if (!r.is_object()) profile_error("root", "must be an object");

    const json::Value& info = r.at("info");
    const json::Value& data = r.at("data");
    if (!info.is_object()) profile_error("info", "must be an object");
    if (!data.is_object()) profile_error("data", "must be an object");
    if (r.has("metadata") && !r.at("metadata").is_object())
        profile_error("metadata", "must be an object when present");

    Profile p;
    p.type = require_string(info, "type", "info.type");
    require_allowlisted(p.type, "info.type", {"negative", "positive"});

    // Keep the pre-V1 diagnostic markers and their precedence consumed by the
    // C/JNI status seam. New V1 schema checks must not mask these diagnostics.
    if (!info.has("viewing_illuminant"))
        throw std::runtime_error(
            "Profile: invalid info.viewing_illuminant '<missing>'");
    const json::Value& viewing = info.at("viewing_illuminant");
    if (!viewing.is_string())
        throw std::runtime_error(
            "Profile: invalid info.viewing_illuminant '<non-string>'");
    p.viewing_illuminant = viewing.as_string();
    require_allowlisted(p.viewing_illuminant, "info.viewing_illuminant",
                        {"D50", "K75P"});
    p.resolved_viewing_illuminant =
        &require_viewing_illuminant(p.viewing_illuminant);

    const std::string support =
        require_string(info, "support", "info.support");
    require_allowlisted(support, "info.support", {"film", "paper"});

    // Upstream also defines a "bw" channel model, but this engine's fixed V1
    // matrices and all downstream dye operations are explicitly three-channel
    // CMY. Reject unsupported monochrome payloads instead of interpreting them
    // as color data.
    const std::string channel_model =
        require_string(info, "channel_model", "info.channel_model");
    require_allowlisted(channel_model, "info.channel_model", {"color"});

    if (info.has("stock")) p.stock = require_string(info, "stock", "info.stock");
    if (info.has("use")) {
        p.use = require_string(info, "use", "info.use");
        require_allowlisted(p.use, "info.use", {"still", "cine"});
    }
    if (info.has("antihalation")) {
        p.antihalation =
            require_string(info, "antihalation", "info.antihalation");
        require_allowlisted(p.antihalation, "info.antihalation",
                            {"strong", "weak", "no"});
    }

    const std::string stage = require_string(info, "stage", "info.stage");
    require_allowlisted(stage, "info.stage", {"filming", "printing"});

    p.reference_illuminant =
        require_string(info, "reference_illuminant", "info.reference_illuminant");
    require_allowlisted(p.reference_illuminant, "info.reference_illuminant",
                        {"D55", "T", "TH-KG3"});

    // Unknown root/info/data/model keys are intentionally accepted and ignored.
    // This is the V1 forward-compatibility policy: metadata may evolve, while
    // every recognized field below remains shape- and value-strict.
    p.wavelengths = read_vec(data.at("wavelengths"), kSpectralRows,
                             "data.wavelengths");
    p.n_samples = static_cast<int>(kSpectralRows);
    p.channel_density = read_matrix3(
        data.at("channel_density"), kSpectralRows, "data.channel_density",
        NullPolicy::PreserveAsNan);
    p.base_density = read_vec(data.at("base_density"), kSpectralRows,
                              "data.base_density", NullPolicy::PreserveAsNan);
    // Required by upstream profile validation even though the current Android
    // pipeline does not consume the cached midscale trace directly.
    validate_vec(data.at("midscale_neutral_density"), kSpectralRows,
                 "data.midscale_neutral_density", NullPolicy::PreserveAsNan);
    p.density_curves = read_matrix3(data.at("density_curves"), kDensityRows,
                                    "data.density_curves");
    p.n_density_pts = static_cast<int>(kDensityRows);

    if (data.has("density_curves_layers")) {
        const json::Value& layers = data.at("density_curves_layers");
        // Upstream serializes the optional tensor's unavailable/default state
        // as [], equivalent to omission. Any non-empty payload is fixed-shape.
        if (!(layers.is_array() && layers.size() == 0)) {
            p.density_curves_layers = read_tensor33(
                layers, kDensityRows, "data.density_curves_layers");
        }
    }
    p.log_sensitivity = read_matrix3(
        data.at("log_sensitivity"), kSpectralRows, "data.log_sensitivity",
        NullPolicy::PreserveAsNan);
    p.log_exposure = read_vec(data.at("log_exposure"), kDensityRows,
                              "data.log_exposure");

    if (data.has("density_curves_model")) {
        const json::Value& model = data.at("density_curves_model");
        if (!model.is_object())
            profile_error("data.density_curves_model", "must be an object");
        p.dc_model_type = require_string(
            model, "model_type", "data.density_curves_model.model_type");
        require_allowlisted(p.dc_model_type,
                            "data.density_curves_model.model_type", {"cdfs"});
        const json::Value& centers = model.at("centers");
        const json::Value& amplitudes = model.at("amplitudes");
        const json::Value& sigmas = model.at("sigmas");
        const size_t center_layers = require_rectangular_matrix(
            centers, kChannels, "data.density_curves_model.centers");
        const size_t amplitude_layers = require_rectangular_matrix(
            amplitudes, kChannels, "data.density_curves_model.amplitudes");
        const size_t sigma_layers = require_rectangular_matrix(
            sigmas, kChannels, "data.density_curves_model.sigmas");
        if (center_layers != amplitude_layers || center_layers != sigma_layers)
            profile_error("data.density_curves_model",
                          "matrices must have equal layer counts");
        if (center_layers > static_cast<size_t>(std::numeric_limits<int>::max()))
            profile_error("data.density_curves_model", "layer count is too large");
        p.dc_model_centers = read_fixed_double_matrix(
            centers, kChannels, center_layers,
            "data.density_curves_model.centers");
        p.dc_model_amplitudes = read_fixed_double_matrix(
            amplitudes, kChannels, center_layers,
            "data.density_curves_model.amplitudes");
        p.dc_model_sigmas = read_fixed_double_matrix(
            sigmas, kChannels, center_layers,
            "data.density_curves_model.sigmas");
        p.dc_model_n_layers = static_cast<int>(center_layers);
    }

    if (data.has("hanatos2025_adaptation_window_params")) {
        const json::Value& window =
            data.at("hanatos2025_adaptation_window_params");
        if (!window.is_array())
            profile_error("data.hanatos2025_adaptation_window_params",
                          "must be an array");
        if (window.size() != 0 && window.size() != kWindowParams)
            profile_error("data.hanatos2025_adaptation_window_params",
                          "must be empty or contain exactly 4 elements");
        p.window_params.reserve(window.size());
        for (size_t i = 0; i < window.size(); ++i) {
            p.window_params.push_back(read_number(
                window[i], "data.hanatos2025_adaptation_window_params[" +
                               std::to_string(i) + "]",
                NullPolicy::Reject, /*require_float_range=*/true));
        }
    }

    if (data.has("hanatos2025_adaptation_surface_params")) {
        const json::Value& surface =
            data.at("hanatos2025_adaptation_surface_params");
        if (!surface.is_array())
            profile_error("data.hanatos2025_adaptation_surface_params",
                          "must be an array");
        if (surface.size() != 0) {
            p.surface_params = read_fixed_double_matrix(
                surface, kSurfaceRows, kSurfaceCols,
                "data.hanatos2025_adaptation_surface_params");
            p.surface_params_cols = static_cast<int>(kSurfaceCols);
        }
    }

    return p;
}

Profile load_profile_file(const std::string& json_path) {
    std::ifstream in(json_path, std::ios::binary);
    if (!in) throw std::runtime_error("Profile: cannot open " + json_path);

    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (end < 0) throw std::runtime_error("Profile: cannot size " + json_path);
    if (static_cast<uintmax_t>(end) > json::kMaxInputBytes)
        throw std::runtime_error("Profile: file exceeds 1 MiB limit");
    const size_t size = static_cast<size_t>(end);

    std::string text(size, '\0');
    in.seekg(0, std::ios::beg);
    if (size != 0) {
        in.read(text.data(), static_cast<std::streamsize>(size));
        if (!in || static_cast<size_t>(in.gcount()) != size)
            throw std::runtime_error("Profile: cannot read " + json_path);
    }
    if (in.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Profile: file changed while reading " + json_path);
    return load_profile_string(text);
}

}  // namespace spk
