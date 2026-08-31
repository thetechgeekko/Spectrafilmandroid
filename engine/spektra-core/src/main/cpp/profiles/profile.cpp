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

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "json_min.h"
#include "model/color_output.h"

namespace spk {

namespace {

// Read a JSON array of numbers into a flat float vector. JSON null -> NaN.
std::vector<float> read_vec(const json::Value& v) {
    if (!v.is_array()) throw std::runtime_error("Profile: expected numeric array");
    std::vector<float> out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        out.push_back(static_cast<float>(v[i].as_number()));
    }
    return out;
}

// Read a JSON (N x 3) array of numbers into a flat row-major float vector.
// Returns the row count via *rows. JSON null -> NaN.
std::vector<float> read_matrix3(const json::Value& v, int* rows) {
    if (!v.is_array()) throw std::runtime_error("Profile: expected matrix array");
    std::vector<float> out;
    out.reserve(v.size() * 3);
    for (size_t i = 0; i < v.size(); ++i) {
        const json::Value& row = v[i];
        if (!row.is_array() || row.size() != 3)
            throw std::runtime_error("Profile: expected (N,3) matrix row");
        out.push_back(static_cast<float>(row[0].as_number()));
        out.push_back(static_cast<float>(row[1].as_number()));
        out.push_back(static_cast<float>(row[2].as_number()));
    }
    *rows = static_cast<int>(v.size());
    return out;
}

// Read a JSON (N x 3 x 3) tensor into a flat row-major float vector with index
// n*9 + layer*3 + ch (matching density_curves.cpp::interp_density_cmy_layers).
// Returns the outer count via *rows. JSON null -> NaN.
std::vector<float> read_tensor33(const json::Value& v, int* rows) {
    if (!v.is_array()) throw std::runtime_error("Profile: expected tensor array");
    std::vector<float> out;
    out.reserve(v.size() * 9);
    for (size_t i = 0; i < v.size(); ++i) {
        const json::Value& layers = v[i];
        if (!layers.is_array() || layers.size() != 3)
            throw std::runtime_error("Profile: expected (N,3,3) tensor layer dim");
        for (size_t L = 0; L < 3; ++L) {
            const json::Value& row = layers[L];
            if (!row.is_array() || row.size() != 3)
                throw std::runtime_error("Profile: expected (N,3,3) tensor channel dim");
            out.push_back(static_cast<float>(row[0].as_number()));
            out.push_back(static_cast<float>(row[1].as_number()));
            out.push_back(static_cast<float>(row[2].as_number()));
        }
    }
    *rows = static_cast<int>(v.size());
    return out;
}

}  // namespace

Profile load_profile_string(const std::string& json_text) {
    json::ValuePtr root = json::parse(json_text);
    const json::Value& r = *root;

    const json::Value& info = r.at("info");
    const json::Value& data = r.at("data");

    Profile p;
    p.type = info.at("type").as_string();
    if (info.has("stock")) p.stock = info.at("stock").as_string();
    if (info.has("use")) p.use = info.at("use").as_string();
    if (info.has("antihalation")) p.antihalation = info.at("antihalation").as_string();
    if (!info.has("viewing_illuminant")) {
        throw std::runtime_error(
            "Profile: invalid info.viewing_illuminant '<missing>'");
    }
    const json::Value& viewing_value = info.at("viewing_illuminant");
    if (!viewing_value.is_string()) {
        throw std::runtime_error(
            "Profile: invalid info.viewing_illuminant '<non-string>'");
    }
    p.viewing_illuminant = viewing_value.as_string();
    p.resolved_viewing_illuminant =
        &require_viewing_illuminant(p.viewing_illuminant);
    if (info.has("reference_illuminant"))
        p.reference_illuminant = info.at("reference_illuminant").as_string();

    p.wavelengths = read_vec(data.at("wavelengths"));
    p.n_samples = static_cast<int>(p.wavelengths.size());

    int cd_rows = 0;
    p.channel_density = read_matrix3(data.at("channel_density"), &cd_rows);
    p.base_density = read_vec(data.at("base_density"));

    int dc_rows = 0;
    p.density_curves = read_matrix3(data.at("density_curves"), &dc_rows);
    p.n_density_pts = dc_rows;

    // Per-sublayer density curves (grain sublayer path). Optional; present in the
    // bundled film profiles as data.density_curves_layers with shape (N,3,3).
    if (data.has("density_curves_layers")) {
        const json::Value& dcl = data.at("density_curves_layers");
        if (dcl.is_array() && dcl.size() > 0) {
            int dcl_rows = 0;
            p.density_curves_layers = read_tensor33(dcl, &dcl_rows);
            if (dcl_rows != p.n_density_pts)
                throw std::runtime_error(
                    "Profile: density_curves_layers rows != density_curves rows");
        }
    }

    // Filming-stage fields (optional; present in the bundled film profiles).
    if (data.has("log_sensitivity")) {
        int ls_rows = 0;
        p.log_sensitivity = read_matrix3(data.at("log_sensitivity"), &ls_rows);
        if (ls_rows != p.n_samples)
            throw std::runtime_error("Profile: log_sensitivity rows != wavelengths");
    }
    if (data.has("log_exposure")) {
        p.log_exposure = read_vec(data.at("log_exposure"));
        if (static_cast<int>(p.log_exposure.size()) != p.n_density_pts)
            throw std::runtime_error("Profile: log_exposure size != density_curves rows");
    }
    // Parametric density-curve model (optional; present in the bundled profiles as
    // data.density_curves_model). centers/amplitudes/sigmas are each (3, n_layers).
    // Stored at double precision (the morph runs in float64 like the oracle).
    if (data.has("density_curves_model")) {
        const json::Value& m = data.at("density_curves_model");
        if (m.has("model_type")) p.dc_model_type = m.at("model_type").as_string();
        // Read a (3, n_layers) matrix flattened row-major [c*n_layers + i]; returns
        // n_layers (and requires exactly 3 channel rows of equal length).
        auto read_cas = [](const json::Value& v, std::vector<double>* out) -> int {
            if (!v.is_array() || v.size() != 3)
                throw std::runtime_error("Profile: density_curves_model expects 3 channels");
            int n_layers = -1;
            for (size_t c = 0; c < 3; ++c) {
                const json::Value& row = v[c];
                if (!row.is_array())
                    throw std::runtime_error("Profile: density_curves_model channel not array");
                if (n_layers < 0) n_layers = static_cast<int>(row.size());
                else if (static_cast<int>(row.size()) != n_layers)
                    throw std::runtime_error("Profile: density_curves_model ragged layers");
                for (size_t i = 0; i < row.size(); ++i)
                    out->push_back(row[i].as_number());
            }
            return n_layers;
        };
        if (m.has("centers") && m.has("amplitudes") && m.has("sigmas")) {
            const int nl_c = read_cas(m.at("centers"), &p.dc_model_centers);
            const int nl_a = read_cas(m.at("amplitudes"), &p.dc_model_amplitudes);
            const int nl_s = read_cas(m.at("sigmas"), &p.dc_model_sigmas);
            if (nl_c != nl_a || nl_a != nl_s)
                throw std::runtime_error("Profile: density_curves_model layer count mismatch");
            p.dc_model_n_layers = nl_c;
        }
    }
    if (data.has("hanatos2025_adaptation_window_params")) {
        const json::Value& wp = data.at("hanatos2025_adaptation_window_params");
        if (!wp.is_array())
            throw std::runtime_error("Profile: window_params expected array");
        for (size_t i = 0; i < wp.size(); ++i)
            p.window_params.push_back(wp[i].as_number());
    }
    // hanatos2025 log-exposure-correction surface params: a (3, cols) matrix, one
    // degree-4 2D polynomial per channel. Mirrors profiles/io.py
    // hanatos2025_adaptation_surface_params (np.asarray -> _empty_matrix if absent).
    if (data.has("hanatos2025_adaptation_surface_params")) {
        const json::Value& sp = data.at("hanatos2025_adaptation_surface_params");
        if (sp.is_array() && sp.size() > 0) {
            if (sp.size() != 3)
                throw std::runtime_error("Profile: surface_params expected 3 rows");
            int cols = -1;
            for (size_t r = 0; r < sp.size(); ++r) {
                const json::Value& row = sp[r];
                if (!row.is_array())
                    throw std::runtime_error("Profile: surface_params row expected array");
                if (cols < 0) cols = static_cast<int>(row.size());
                else if (static_cast<int>(row.size()) != cols)
                    throw std::runtime_error("Profile: surface_params ragged rows");
                for (size_t k = 0; k < row.size(); ++k)
                    p.surface_params.push_back(row[k].as_number());
            }
            p.surface_params_cols = cols;
        }
    }

    // Validation mirroring profiles/io.py::_validate_profile (subset).
    if (cd_rows != p.n_samples)
        throw std::runtime_error("Profile: channel_density rows != wavelengths");
    if (static_cast<int>(p.base_density.size()) != p.n_samples)
        throw std::runtime_error("Profile: base_density size != wavelengths");

    return p;
}

Profile load_profile_file(const std::string& json_path) {
    std::ifstream in(json_path, std::ios::binary);
    if (!in) throw std::runtime_error("Profile: cannot open " + json_path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return load_profile_string(ss.str());
}

}  // namespace spk
