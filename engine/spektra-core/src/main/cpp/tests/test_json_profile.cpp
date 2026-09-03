/*
 * Spektrafilm for Android — hostile JSON/profile loader regression tests.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "profiles/json_min.h"
#include "profiles/profile.h"
#include "runtime/print_digest.h"

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    std::printf("%s: %s\n", label, condition ? "OK" : "FAIL");
    if (!condition) ++failures;
}

bool rejects_json(const std::string& text) {
    try {
        (void)spk::json::parse(text);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

bool accepts_json(const std::string& text) {
    try {
        (void)spk::json::parse(text);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string json_array(size_t count, const std::string& value) {
    std::string text = "[";
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) text += ',';
        text += value;
    }
    text += ']';
    return text;
}

std::string json_object(size_t count) {
    std::string text = "{";
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) text += ',';
        text += "\"k" + std::to_string(i) + "\":0";
    }
    text += '}';
    return text;
}

std::string node_budget_graph(size_t leaf_nodes) {
    std::string text = "[";
    bool first = true;
    while (leaf_nodes != 0) {
        const size_t chunk =
            std::min(leaf_nodes, spk::json::kMaxArrayElements);
        if (!first) text += ',';
        text += json_array(chunk, "0");
        first = false;
        leaf_nodes -= chunk;
    }
    text += ']';
    return text;
}

std::string repeated_array(size_t count, const std::string& value) {
    return json_array(count, value);
}

std::string matrix(size_t rows, size_t cols, const std::string& value) {
    return json_array(rows, repeated_array(cols, value));
}

std::string tensor(size_t rows, size_t layers, size_t channels,
                   const std::string& value) {
    return json_array(rows, matrix(layers, channels, value));
}

struct ProfileFixture {
    size_t spectral_rows = 81;
    size_t spectral_cols = 3;
    size_t density_rows = 256;
    size_t density_layer_rows = 256;
    size_t density_layer_layers = 3;
    size_t density_layer_channels = 3;
    size_t model_channels = 3;
    size_t model_center_layers = 3;
    size_t model_amplitude_layers = 3;
    size_t model_sigma_layers = 3;
    size_t midscale_rows = 81;
    size_t sensitivity_rows = 81;
    size_t sensitivity_cols = 3;
    size_t exposure_rows = 256;
    size_t window_values = 4;
    size_t surface_rows = 3;
    size_t surface_cols = 15;
    std::string type = "negative";
    std::string support_json = "\"film\"";
    std::string stage = "filming";
    std::string channel_model_json = "\"color\"";
    std::string viewing = "D50";
    std::string reference = "D55";
    std::string wavelength_value = "380";
    std::string channel_value = "0.1";
    std::string base_value = "0.01";
    std::string curve_value = "0.2";
    std::string midscale_value = "0.1";
    std::string midscale_json;
    std::string log_sensitivity_value = "-1.0";
    std::string log_exposure_value = "-2.0";
    std::string metadata_json =
        "{\"future_note\":\"preserved-compatible\"}";
    bool include_metadata = true;
    bool include_support = true;
    bool include_channel_model = true;
    bool include_density_layers = true;
    bool empty_density_layers = false;
    bool include_midscale = true;
    bool ragged_model_centers = false;
    bool include_log_sensitivity = true;
    bool include_log_exposure = true;
};

std::string make_profile(const ProfileFixture& f = {}) {
    std::string centers = matrix(f.model_channels, f.model_center_layers, "0.5");
    if (f.ragged_model_centers) {
        centers = "[" + repeated_array(f.model_center_layers, "0.5") + "," +
                  repeated_array(f.model_center_layers + 1, "0.5") + "," +
                  repeated_array(f.model_center_layers, "0.5") + "]";
    }
    const std::string amplitudes =
        matrix(f.model_channels, f.model_amplitude_layers, "0.5");
    const std::string sigmas =
        matrix(f.model_channels, f.model_sigma_layers, "0.5");
    std::string text = "{";
    if (f.include_metadata) text += "\"metadata\":" + f.metadata_json + ",";
    text += "\"future_root\":true,\"info\":{\"stock\":\"fixture\",\"type\":\"" +
            f.type + "\"";
    if (f.include_support) text += ",\"support\":" + f.support_json;
    text += ",\"stage\":\"" + f.stage +
        "\",\"viewing_illuminant\":\"" + f.viewing +
        "\",\"reference_illuminant\":\"" + f.reference +
        "\",\"use\":\"still\",\"antihalation\":\"strong\"";
    if (f.include_channel_model)
        text += ",\"channel_model\":" + f.channel_model_json;
    text += ",\"future_info\":17},\"data\":{";
    text += "\"wavelengths\":" + repeated_array(f.spectral_rows, f.wavelength_value);
    text += ",\"channel_density\":" +
            matrix(f.spectral_rows, f.spectral_cols, f.channel_value);
    text += ",\"base_density\":" + repeated_array(f.spectral_rows, f.base_value);
    if (f.include_midscale) {
        text += ",\"midscale_neutral_density\":";
        text += f.midscale_json.empty()
                    ? repeated_array(f.midscale_rows, f.midscale_value)
                    : f.midscale_json;
    }
    text += ",\"density_curves\":" + matrix(f.density_rows, 3, f.curve_value);
    if (f.include_density_layers) {
        text += ",\"density_curves_layers\":";
        text += f.empty_density_layers
                    ? "[]"
                    : tensor(f.density_layer_rows, f.density_layer_layers,
                             f.density_layer_channels, "0.2");
    }
    if (f.include_log_sensitivity) {
        text += ",\"log_sensitivity\":" +
                matrix(f.sensitivity_rows, f.sensitivity_cols,
                       f.log_sensitivity_value);
    }
    if (f.include_log_exposure) {
        text += ",\"log_exposure\":" +
                repeated_array(f.exposure_rows, f.log_exposure_value);
    }
    text += ",\"density_curves_model\":{\"model_type\":\"cdfs\","
            "\"centers\":" + centers + ",\"amplitudes\":" + amplitudes +
            ",\"sigmas\":" + sigmas + "}";
    text += ",\"hanatos2025_adaptation_window_params\":" +
            repeated_array(f.window_values, "1.0");
    text += ",\"hanatos2025_adaptation_surface_params\":" +
            matrix(f.surface_rows, f.surface_cols, "0.0");
    text += ",\"future_data\":{\"source\":\"fixture\"}}}";
    return text;
}

bool rejects_profile(const std::string& text) {
    try {
        (void)spk::load_profile_string(text);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

bool neutral_failure_is_atomic(const std::string& text) {
    double cc[3] = {101.0, 202.0, 303.0};
    try {
        const bool found = spk::resolve_neutral_cc_string(
            text, "paper", "illum", "film", cc);
        return !found && cc[0] == 101.0 && cc[1] == 202.0 && cc[2] == 303.0;
    } catch (...) {
        return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <profiles-dir> <neutral-json>\n", argv[0]);
        return 2;
    }
    check(rejects_json("{\"a\":1,\"\\u0061\":2}"),
          "json rejects decoded-equivalent duplicate keys");
    check(rejects_json("+1"), "json rejects leading plus");
    check(rejects_json("1e309"), "json rejects non-finite number");
    check(rejects_json("01") && rejects_json("1.") && rejects_json("1e") &&
              rejects_json("--1"),
          "json enforces RFC number grammar");
    check(rejects_json(std::string("1") + std::string(128, '0')),
          "json rejects numeric token above 128 bytes");
    check(accepts_json(std::string("1") + std::string(127, '0')),
          "json accepts finite numeric token at 128 bytes");
    check(rejects_json(json_array(513, "0")),
          "json rejects array above 512 elements");
    check(accepts_json(json_array(512, "0")),
          "json accepts array at 512 elements");
    check(rejects_json(json_object(65)), "json rejects object above 64 members");
    check(accepts_json(json_object(64)), "json accepts object at 64 members");
    check(rejects_json("[[[[[[[[0]]]]]]]]"), "json rejects depth above 8");
    check(accepts_json("[[[[[[[0]]]]]]]"), "json accepts depth at 8");

    // Root array + 32 child arrays + the requested leaves gives an exact node
    // budget without colliding with the per-array ceiling.
    constexpr size_t kNodeBudgetChildArrays = 32;
    constexpr size_t kLeavesAtNodeBudget =
        spk::json::kMaxNodes - 1 - kNodeBudgetChildArrays;
    check(accepts_json(node_budget_graph(kLeavesAtNodeBudget)),
          "json accepts graph at 16384 nodes");
    check(rejects_json(node_budget_graph(kLeavesAtNodeBudget + 1)),
          "json rejects graph at 16385 nodes");

    check(rejects_json("\"" + std::string(4097, 'a') + "\""),
          "json rejects decoded string above 4096 bytes");
    check(accepts_json("\"" + std::string(4096, 'a') + "\""),
          "json accepts decoded string at 4096 bytes");
    std::string utf8_at_limit = "\"";
    for (size_t i = 0; i < 2048; ++i) utf8_at_limit += "\xc3\xa9";
    utf8_at_limit += '"';
    std::string utf8_above_limit = utf8_at_limit;
    utf8_above_limit.insert(utf8_above_limit.size() - 1, "\xc3\xa9");
    check(accepts_json(utf8_at_limit) && rejects_json(utf8_above_limit),
          "json enforces decoded byte limit across multi-byte UTF-8");
    std::string input_at_limit = "0";
    input_at_limit.resize(spk::json::kMaxInputBytes, ' ');
    check(accepts_json(input_at_limit), "json accepts valid input at 1 MiB");
    input_at_limit.push_back(' ');
    check(rejects_json(input_at_limit), "json rejects input at 1 MiB plus one");
    check(rejects_json("\"line\nfeed\"") && rejects_json("\"\\u12xz\"") &&
              rejects_json("\"\\ud800\"") && rejects_json("\"\\udc00\""),
          "json rejects controls and malformed Unicode escapes");
    check(accepts_json("\"\\ud83d\\ude00\"") && accepts_json("\"é\""),
          "json accepts paired surrogate and valid UTF-8");
    std::string invalid_utf8 = "\"";
    invalid_utf8.push_back(static_cast<char>(0xc0));
    invalid_utf8.push_back(static_cast<char>(0xaf));
    invalid_utf8 += '"';
    check(rejects_json(invalid_utf8), "json rejects invalid raw UTF-8");
    check(rejects_json("{} garbage"), "json rejects trailing data");

    try {
        const spk::Profile p = spk::load_profile_string(make_profile());
        check(p.n_samples == 81 && p.n_density_pts == 256 &&
                  p.channel_density.size() == 81u * 3u &&
                  p.density_curves_layers.size() == 256u * 9u &&
                  p.dc_model_n_layers == 3 && p.window_params.size() == 4u &&
                  p.surface_params.size() == 3u * 15u,
              "profile accepts V1 shape and ignores forward metadata");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "valid fixture rejected: %s\n", e.what());
        check(false, "profile accepts V1 shape and ignores forward metadata");
    }

    ProfileFixture bad;
    bad.include_metadata = false;
    check(!rejects_profile(make_profile(bad)),
          "profile accepts omitted metadata object");
    bad = {};
    bad.metadata_json = "17";
    check(rejects_profile(make_profile(bad)),
          "profile rejects present non-object metadata");
    bad = {};
    bad.spectral_rows = 80;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-under spectral rows");
    bad = {};
    bad.spectral_rows = 82;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-over spectral rows");
    bad = {};
    bad.density_rows = 255;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-under density rows");
    bad = {};
    bad.density_rows = 257;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-over density rows");
    bad = {};
    bad.include_density_layers = false;
    try {
        const spk::Profile omitted = spk::load_profile_string(make_profile(bad));
        check(omitted.density_curves_layers.empty(),
              "profile treats omitted layer curves as unavailable");
    } catch (...) {
        check(false, "profile treats omitted layer curves as unavailable");
    }
    bad = {};
    bad.empty_density_layers = true;
    try {
        const spk::Profile empty = spk::load_profile_string(make_profile(bad));
        check(empty.density_curves_layers.empty(),
              "profile treats empty layer curves as unavailable");
    } catch (...) {
        check(false, "profile treats empty layer curves as unavailable");
    }
    bad = {};
    bad.density_layer_rows = 255;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-under layer-curve rows");
    bad = {};
    bad.density_layer_rows = 257;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-over layer-curve rows");
    bad = {};
    bad.density_layer_layers = 2;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-under layer-curve layers");
    bad = {};
    bad.density_layer_layers = 4;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-over layer-curve layers");
    bad = {};
    bad.density_layer_channels = 2;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-under layer-curve channels");
    bad = {};
    bad.density_layer_channels = 4;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-over layer-curve channels");
    bad = {};
    bad.spectral_cols = 2;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-under spectral channels");
    bad = {};
    bad.spectral_cols = 4;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-over spectral channels");
    bad = {};
    bad.include_midscale = false;
    check(rejects_profile(make_profile(bad)),
          "profile requires data.midscale_neutral_density");
    bad = {};
    bad.midscale_json = "{}";
    check(rejects_profile(make_profile(bad)),
          "profile requires array midscale neutral density");
    bad = {};
    bad.midscale_rows = 80;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-under midscale neutral density");
    bad = {};
    bad.midscale_rows = 82;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-over midscale neutral density");
    bad = {};
    bad.midscale_value = "null";
    check(!rejects_profile(make_profile(bad)),
          "profile accepts nullable midscale neutral density");

    bool dynamic_models_accept = true;
    for (const size_t layers : {size_t{0}, size_t{2}, size_t{3}, size_t{4},
                                spk::json::kMaxArrayElements}) {
        ProfileFixture dynamic;
        dynamic.model_center_layers = layers;
        dynamic.model_amplitude_layers = layers;
        dynamic.model_sigma_layers = layers;
        try {
            const spk::Profile model =
                spk::load_profile_string(make_profile(dynamic));
            dynamic_models_accept =
                dynamic_models_accept &&
                model.dc_model_n_layers == static_cast<int>(layers) &&
                model.dc_model_centers.size() == 3u * layers &&
                model.dc_model_amplitudes.size() == 3u * layers &&
                model.dc_model_sigmas.size() == 3u * layers;
        } catch (...) {
            dynamic_models_accept = false;
        }
    }
    check(dynamic_models_accept,
          "profile accepts bounded equal-width dynamic curve models");
    bad = {};
    bad.model_center_layers = spk::json::kMaxArrayElements + 1;
    bad.model_amplitude_layers = spk::json::kMaxArrayElements + 1;
    bad.model_sigma_layers = spk::json::kMaxArrayElements + 1;
    check(rejects_profile(make_profile(bad)),
          "profile rejects curve-model layers above parser ceiling");
    bad = {};
    bad.model_center_layers = 2;
    bad.model_amplitude_layers = 3;
    bad.model_sigma_layers = 2;
    check(rejects_profile(make_profile(bad)),
          "profile rejects mismatched curve-model widths");
    bad = {};
    bad.model_center_layers = 2;
    bad.model_amplitude_layers = 2;
    bad.model_sigma_layers = 2;
    bad.ragged_model_centers = true;
    check(rejects_profile(make_profile(bad)),
          "profile rejects ragged curve-model rows");
    bad = {};
    bad.model_channels = 2;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-under curve-model channels");
    bad = {};
    bad.model_channels = 4;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-over curve-model channels");
    bad = {};
    bad.window_values = 3;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-under window parameters");
    bad = {};
    bad.window_values = 5;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-over window parameters");
    bad = {};
    bad.surface_cols = 14;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-under surface columns");
    bad = {};
    bad.surface_cols = 16;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-over surface columns");
    bad = {};
    bad.surface_rows = 2;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-under surface rows");
    bad = {};
    bad.surface_rows = 4;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-over surface rows");

    bool allowlists_reject = true;
    for (const auto& field : std::vector<std::pair<const char*, ProfileFixture>>{
             {"type", [] { ProfileFixture x; x.type = "paper"; return x; }()},
             {"stage", [] { ProfileFixture x; x.stage = "preview"; return x; }()},
             {"viewing", [] { ProfileFixture x; x.viewing = "D65"; return x; }()},
             {"reference", [] { ProfileFixture x; x.reference = "D65"; return x; }()},
         }) {
        (void)field.first;
        allowlists_reject = allowlists_reject && rejects_profile(make_profile(field.second));
    }
    check(allowlists_reject, "profile enforces type, stage, and illuminant allowlists");

    bad = {};
    bad.include_support = false;
    check(rejects_profile(make_profile(bad)), "profile requires info.support");
    bad = {};
    bad.support_json = "17";
    check(rejects_profile(make_profile(bad)), "profile requires string info.support");
    bad = {};
    bad.support_json = "\"plate\"";
    check(rejects_profile(make_profile(bad)),
          "profile rejects unsupported info.support");
    bad = {};
    bad.support_json = "\"paper\"";
    bad.stage = "printing";
    check(!rejects_profile(make_profile(bad)),
          "profile accepts supported paper/color metadata");
    bad = {};
    bad.include_channel_model = false;
    check(rejects_profile(make_profile(bad)), "profile requires info.channel_model");
    bad = {};
    bad.channel_model_json = "17";
    check(rejects_profile(make_profile(bad)),
          "profile requires string info.channel_model");
    bad = {};
    bad.channel_model_json = "\"bw\"";
    check(rejects_profile(make_profile(bad)),
          "profile rejects unsupported monochrome channel model");
    bad = {};
    bad.channel_model_json = "\"spectral\"";
    check(rejects_profile(make_profile(bad)),
          "profile rejects unknown channel model");

    bad = {};
    bad.include_log_sensitivity = false;
    check(rejects_profile(make_profile(bad)),
          "profile requires data.log_sensitivity");
    bad = {};
    bad.sensitivity_rows = 80;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-under log-sensitivity rows");
    bad = {};
    bad.sensitivity_rows = 82;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-over log-sensitivity rows");
    bad = {};
    bad.sensitivity_cols = 2;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-under log-sensitivity channels");
    bad = {};
    bad.sensitivity_cols = 4;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-over log-sensitivity channels");
    bad = {};
    bad.include_log_exposure = false;
    check(rejects_profile(make_profile(bad)),
          "profile requires data.log_exposure");
    bad = {};
    bad.exposure_rows = 255;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-under log-exposure rows");
    bad = {};
    bad.exposure_rows = 257;
    check(rejects_profile(make_profile(bad)),
          "profile rejects one-over log-exposure rows");

    bad = {};
    bad.wavelength_value = "3.5e38";
    check(rejects_profile(make_profile(bad)), "profile rejects values outside float range");
    bad = {};
    bad.curve_value = "null";
    check(rejects_profile(make_profile(bad)), "profile rejects null in finite curve fields");
    bad = {};
    bad.log_exposure_value = "null";
    check(rejects_profile(make_profile(bad)),
          "profile rejects null in finite exposure-axis fields");
    bad = {};
    bad.channel_value = "null";
    bad.base_value = "null";
    try {
        const spk::Profile nullable = spk::load_profile_string(make_profile(bad));
        check(std::isnan(nullable.channel_density[0]) &&
                  std::isnan(nullable.base_density[0]),
              "profile preserves schema-allowed spectral nulls as NaN");
    } catch (...) {
        check(false, "profile preserves schema-allowed spectral nulls as NaN");
    }
    bad = {};
    bad.log_sensitivity_value = "null";
    try {
        const spk::Profile nullable = spk::load_profile_string(make_profile(bad));
        check(nullable.log_sensitivity.size() == 81u * 3u &&
                  std::isnan(nullable.log_sensitivity.front()) &&
                  std::isnan(nullable.log_sensitivity.back()),
              "profile preserves upstream log-sensitivity nulls as NaN");
    } catch (...) {
        check(false, "profile preserves upstream log-sensitivity nulls as NaN");
    }

    size_t loaded_profiles = 0;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(argv[1])) {
            if (entry.path().extension() != ".json") continue;
            const spk::Profile p = spk::load_profile_file(entry.path().string());
            if (p.n_samples != 81 || p.n_density_pts != 256) break;
            ++loaded_profiles;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bundled profile rejected: %s\n", e.what());
    }
    check(loaded_profiles == 28, "all 28 bundled profiles load under V1 policy");

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path bounded_profile =
        std::filesystem::temp_directory_path() /
        ("spektra_profile_boundary_" + std::to_string(nonce) + ".json");
    std::string profile_at_limit = make_profile();
    check(profile_at_limit.size() < spk::json::kMaxInputBytes,
          "profile boundary fixture fits below parser ceiling");
    profile_at_limit.resize(spk::json::kMaxInputBytes, ' ');
    {
        std::ofstream out(bounded_profile, std::ios::binary | std::ios::trunc);
        out << profile_at_limit;
    }
    bool exact_profile_loaded = false;
    try {
        const spk::Profile exact = spk::load_profile_file(bounded_profile.string());
        exact_profile_loaded = exact.n_samples == 81 && exact.n_density_pts == 256;
    } catch (const std::exception&) {
    }
    profile_at_limit.push_back(' ');
    {
        std::ofstream out(bounded_profile, std::ios::binary | std::ios::trunc);
        out << profile_at_limit;
    }
    bool oversized_rejected = false;
    try {
        (void)spk::load_profile_file(bounded_profile.string());
    } catch (const std::exception&) {
        oversized_rejected = true;
    }
    check(exact_profile_loaded, "profile file accepts valid payload at 1 MiB");
    check(oversized_rejected, "profile file rejects payload at 1 MiB plus one");

    const std::filesystem::path bounded_neutral =
        std::filesystem::temp_directory_path() /
        ("spektra_neutral_boundary_" + std::to_string(nonce) + ".json");
    std::string neutral_at_limit =
        "{\"paper\":{\"illum\":{\"film\":[1,2,3]}}}";
    neutral_at_limit.resize(spk::json::kMaxInputBytes, ' ');
    {
        std::ofstream out(bounded_neutral, std::ios::binary | std::ios::trunc);
        out << neutral_at_limit;
    }
    double exact_cc[3] = {-1.0, -1.0, -1.0};
    const bool exact_neutral_found = spk::resolve_neutral_cc(
        bounded_neutral.string(), "paper", "illum", "film", exact_cc);
    neutral_at_limit.push_back(' ');
    {
        std::ofstream out(bounded_neutral, std::ios::binary | std::ios::trunc);
        out << neutral_at_limit;
    }
    double oversized_cc[3] = {7.0, 8.0, 9.0};
    const bool oversized_neutral_found = spk::resolve_neutral_cc(
        bounded_neutral.string(), "paper", "illum", "film", oversized_cc);
    std::error_code remove_error;
    std::filesystem::remove(bounded_profile, remove_error);
    std::filesystem::remove(bounded_neutral, remove_error);
    check(exact_neutral_found && exact_cc[0] == 1.0 && exact_cc[1] == 2.0 &&
              exact_cc[2] == 3.0,
          "neutral-filter file accepts valid payload at 1 MiB");
    check(!oversized_neutral_found && oversized_cc[0] == 7.0 &&
              oversized_cc[1] == 8.0 && oversized_cc[2] == 9.0,
          "neutral-filter file rejects 1 MiB plus one atomically");

    double cc[3] = {-1.0, -1.0, -1.0};
    const bool neutral_found = spk::resolve_neutral_cc_string(
        "{\"paper\":{\"illum\":{\"film\":[1,2,3]}}}",
        "paper", "illum", "film", cc);
    check(neutral_found && cc[0] == 1.0 && cc[1] == 2.0 && cc[2] == 3.0,
          "neutral filter commits one exact finite triple");
    check(neutral_failure_is_atomic("{broken") &&
              neutral_failure_is_atomic("{}") &&
              neutral_failure_is_atomic("{\"paper\":0}") &&
              neutral_failure_is_atomic(
                  "{\"paper\":{\"illum\":{\"film\":[1,null,3]}}}") &&
              neutral_failure_is_atomic(
                  "{\"paper\":{\"illum\":{\"film\":[1,1e309,3]}}}") &&
              neutral_failure_is_atomic(
                  "{\"paper\":{\"illum\":{\"film\":[1,2]}}}") &&
              neutral_failure_is_atomic(
                  std::string(spk::json::kMaxInputBytes + 1, ' ')),
          "neutral filter failures preserve caller sentinels");

    double bundled_cc[3] = {-1.0, -1.0, -1.0};
    const bool bundled_found = spk::resolve_neutral_cc(
        argv[2], "kodak_portra_endura", "TH-KG3", "kodak_portra_400", bundled_cc);
    check(bundled_found && bundled_cc[0] == 0.0 &&
              std::fabs(bundled_cc[1] - 51.43162770877449) < 1e-12 &&
              std::fabs(bundled_cc[2] - 55.26070894686862) < 1e-12,
          "bundled neutral-filter database resolves unchanged");

    double missing_cc[3] = {11.0, 22.0, 33.0};
    const bool missing_found = spk::resolve_neutral_cc(
        bounded_profile.string() + ".missing", "paper", "illum", "film",
        missing_cc);
    check(!missing_found && missing_cc[0] == 11.0 && missing_cc[1] == 22.0 &&
              missing_cc[2] == 33.0,
          "missing neutral-filter file preserves caller sentinels");

    if (failures != 0) {
        std::fprintf(stderr, "test_json_profile: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("test_json_profile: ALL OK");
    return 0;
}
