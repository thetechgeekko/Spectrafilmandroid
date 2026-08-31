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
 * spektrafilm. Ports the parts of spektrafilm/profiles/io.py the scanning stage
 * consumes: info.type / info.viewing_illuminant and data.{wavelengths,
 * channel_density, base_density, density_curves}. Other fields parsed by the
 * full pipeline (log_sensitivity, log_exposure, ...) are loaded lazily as the
 * port grows; the scanning milestone only needs the subset below.
 */
#ifndef SPK_PROFILES_PROFILE_H
#define SPK_PROFILES_PROFILE_H

#include <string>
#include <vector>

namespace spk {

struct ViewingIlluminant;

// Mirrors the spektrafilm Profile fields used downstream. Spectral arrays are
// stored on the profile's own wavelength grid (the bundled profiles already use
// the 380..780 @5nm / 81-sample working shape, matching spektra.h). NaN entries
// in the JSON (`null`) are preserved as NaN so density_to_light zeroes them,
// exactly as the Python pipeline does.
struct Profile {
    // info
    std::string type;                 // "negative" | "positive"
    std::string stock;                // e.g. "kodak_portra_400" (info.stock)
    std::string viewing_illuminant;   // e.g. "D50"
    // Stable pointer into model/color_output's immutable exact-ID registry.
    // Loaded profiles resolve this before any spectral payload is accepted;
    // direct unit fixtures may leave it null and render paths will fail closed
    // through the same registry using viewing_illuminant.
    const ViewingIlluminant* resolved_viewing_illuminant = nullptr;
    std::string reference_illuminant;
    // Halation preset tags (info.use / info.antihalation). Drive the digested
    // halation sigma_h / strength via params_builder._HALATION_PRESETS. Default to
    // the "still"/"strong" still-film negative baseline if absent.
    std::string use = "still";          // "still" | "cine"
    std::string antihalation = "strong"; // "strong" | "weak" | "no"

    // data
    std::vector<float> wavelengths;        // (S,)
    std::vector<float> channel_density;     // (S*3,) row-major [s*3 + k] (CMY dye)
    std::vector<float> base_density;        // (S,)
    std::vector<float> density_curves;      // (N*3,) row-major [n*3 + k]
    // Per-sublayer density characteristic curves (grain sublayer path). Shape
    // (N, 3layer, 3ch) row-major: index n*9 + layer*3 + ch. Empty if the JSON
    // omits data.density_curves_layers (then the sublayer grain path is unusable
    // and callers fall back to the non-sublayer path). Matches the indexing of
    // density_curves.cpp::interp_density_cmy_layers.
    std::vector<float> density_curves_layers;  // (N*9,) row-major [n*9 + L*3 + k]
    // Parametric density-curve model (data.density_curves_model, 'cdfs'): a sum of
    // amplitude-weighted Gaussian CDFs per channel/sub-layer. centers/amplitudes/
    // sigmas are each (3ch, n_layers) flattened row-major [c*n_layers + i]. Consumed
    // ONLY by the opt-in print-curve morph (model/morph_curves); empty / n_layers==0
    // when the JSON omits it (then the morph is unavailable and the stored
    // density_curves table is used, as in the default path).
    std::string dc_model_type;                  // e.g. "cdfs"
    int dc_model_n_layers = 0;                  // sub-layers per channel (0 = absent)
    std::vector<double> dc_model_centers;       // (3*n_layers,) [c*n_layers + i]
    std::vector<double> dc_model_amplitudes;    // (3*n_layers,)
    std::vector<double> dc_model_sigmas;        // (3*n_layers,)
    int n_samples = 0;                      // S
    int n_density_pts = 0;                  // N

    // Filming-stage fields (loaded for the rgb->raw->density port). Optional in
    // the JSON; empty vectors mean "not present" (the scanning milestone did not
    // need them).
    std::vector<float> log_sensitivity;     // (S*3,) row-major [s*3 + k]
    std::vector<float> log_exposure;        // (N,) shared log-exposure axis
    // hanatos2025 spectral band-pass window params (erf4 model):
    // [c_uv, sigma_uv, c_ir, sigma_ir]. Empty if absent.
    std::vector<double> window_params;
    // hanatos2025 log-exposure-correction surface params (poly4 model): a
    // (3, surface_params_cols) matrix flattened row-major as [ch*cols + k], one
    // degree-4 2D polynomial per channel (coefficient c0 is unused by poly2d_deg4).
    // Empty if absent. Used only when apply_hanatos2025_adaptation_surface is on.
    std::vector<double> surface_params;     // (3*cols,) row-major
    int surface_params_cols = 0;            // per-channel coeff count (15 for poly4)

    bool is_negative() const { return type == "negative"; }
    bool is_positive() const { return type == "positive"; }
};

// Load a profile from a JSON file path (the bundled spektrafilm profile format).
// Throws std::runtime_error on parse/validation failure.
Profile load_profile_file(const std::string& json_path);

// Parse a profile from an in-memory JSON string (for asset-backed loading on
// Android, where the file lives in the APK asset store).
Profile load_profile_string(const std::string& json_text);

}  // namespace spk

#endif  // SPK_PROFILES_PROFILE_H
