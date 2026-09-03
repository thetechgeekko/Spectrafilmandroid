/*
 * Test-only canonical inventory for the Kotlin -> JNI -> spk_params boundary.
 *
 * Values are emitted field-by-field instead of hashing the object representation:
 * pointer width, padding and endianness therefore cannot make arm64 and x86_64
 * disagree.  Strings are UTF-8 hex, floats are their IEEE-754 binary32 bits, and
 * all integral fields are signed decimal.  The instrumentation test constructs
 * its expected inventory directly from the Kotlin parameter tree, so an omitted
 * JNI getter or assignment cannot be blessed by observing this output.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "spektra.h"

namespace spk::params_manifest {

inline void append_prefix(std::string* out, const char* name) {
    out->append(name);
    out->push_back('=');
}

inline void append_i32(std::string* out, const char* name, int32_t value) {
    append_prefix(out, name);
    out->push_back('i');
    out->append(std::to_string(value));
    out->push_back('\n');
}

inline void append_f32(std::string* out, const char* name, float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    char encoded[10];
    const int n = std::snprintf(encoded, sizeof(encoded), "f%08x", bits);
    if (n != 9) return;
    append_prefix(out, name);
    out->append(encoded, static_cast<size_t>(n));
    out->push_back('\n');
}

inline void append_string(std::string* out, const char* name, const char* value) {
    append_prefix(out, name);
    if (!value) {
        out->append("null\n");
        return;
    }
    out->push_back('s');
    constexpr char kHex[] = "0123456789abcdef";
    for (const auto* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        out->push_back(kHex[*p >> 4]);
        out->push_back(kHex[*p & 0x0f]);
    }
    out->push_back('\n');
}

inline void append_f32_array(std::string* out, const char* name,
                             const float* values, size_t count) {
    char indexed[96];
    for (size_t i = 0; i < count; ++i) {
        const int n = std::snprintf(indexed, sizeof(indexed), "%s[%zu]", name, i);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(indexed)) return;
        append_f32(out, indexed, values[i]);
    }
}

inline void append_i32_array(std::string* out, const char* name,
                             const int32_t* values, size_t count) {
    char indexed[96];
    for (size_t i = 0; i < count; ++i) {
        const int n = std::snprintf(indexed, sizeof(indexed), "%s[%zu]", name, i);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(indexed)) return;
        append_i32(out, indexed, values[i]);
    }
}

inline std::string named_inventory(const spk_params& p) {
    std::string out("schema=spk_params_named_v2\n");
    out.reserve(12288);

#define SPK_MANIFEST_I32(field) append_i32(&out, #field, static_cast<int32_t>(p.field))
#define SPK_MANIFEST_F32(field) append_f32(&out, #field, p.field)
#define SPK_MANIFEST_STR(field) append_string(&out, #field, p.field)
#define SPK_MANIFEST_F32_ARRAY(field, count) \
    append_f32_array(&out, #field, p.field, count)
#define SPK_MANIFEST_I32_ARRAY(field, count) \
    append_i32_array(&out, #field, p.field, count)

    SPK_MANIFEST_STR(film_profile);
    SPK_MANIFEST_STR(print_profile);
    SPK_MANIFEST_F32(exposure_compensation_ev);
    SPK_MANIFEST_I32(auto_exposure);
    SPK_MANIFEST_F32(lens_blur_um);
    SPK_MANIFEST_F32(film_format_mm);
    SPK_MANIFEST_STR(auto_exposure_method);

    SPK_MANIFEST_F32(y_filter_shift);
    SPK_MANIFEST_F32(m_filter_shift);
    SPK_MANIFEST_F32(preflash_exposure);
    SPK_MANIFEST_I32(normalize_print_exposure);

    SPK_MANIFEST_F32(density_curve_gamma);
    SPK_MANIFEST_I32(grain_active);
    SPK_MANIFEST_I32(halation_active);
    SPK_MANIFEST_I32(dir_couplers_active);
    SPK_MANIFEST_I32(glare_active);

    SPK_MANIFEST_I32(scan_film);
    SPK_MANIFEST_I32(output_color_space);
    SPK_MANIFEST_I32(output_cctf_encoding);
    SPK_MANIFEST_I32(rgb_to_raw_method);
    SPK_MANIFEST_I32(preview_max_size);

    SPK_MANIFEST_F32_ARRAY(camera_filter_uv, 3);
    SPK_MANIFEST_F32_ARRAY(camera_filter_ir, 3);
    SPK_MANIFEST_I32(camera_diffusion_active);
    SPK_MANIFEST_F32(camera_diffusion_strength);
    SPK_MANIFEST_F32(camera_diffusion_spatial_scale);
    SPK_MANIFEST_F32(camera_diffusion_halo_warmth);
    SPK_MANIFEST_F32(camera_diffusion_core_intensity);
    SPK_MANIFEST_F32(camera_diffusion_core_size);
    SPK_MANIFEST_F32(camera_diffusion_halo_intensity);
    SPK_MANIFEST_F32(camera_diffusion_halo_size);
    SPK_MANIFEST_F32(camera_diffusion_bloom_intensity);
    SPK_MANIFEST_F32(camera_diffusion_bloom_size);

    SPK_MANIFEST_F32(print_exposure);
    SPK_MANIFEST_I32(print_exposure_compensation);
    SPK_MANIFEST_F32(y_filter_neutral);
    SPK_MANIFEST_F32(m_filter_neutral);
    SPK_MANIFEST_F32(c_filter_neutral);
    SPK_MANIFEST_F32(enlarger_lens_blur);
    SPK_MANIFEST_F32(preflash_y_filter_shift);
    SPK_MANIFEST_F32(preflash_m_filter_shift);
    SPK_MANIFEST_I32(enlarger_diffusion_active);
    SPK_MANIFEST_F32(enlarger_diffusion_strength);
    SPK_MANIFEST_F32(enlarger_diffusion_spatial_scale);
    SPK_MANIFEST_F32(enlarger_diffusion_halo_warmth);
    SPK_MANIFEST_F32(enlarger_diffusion_core_intensity);
    SPK_MANIFEST_F32(enlarger_diffusion_core_size);
    SPK_MANIFEST_F32(enlarger_diffusion_halo_intensity);
    SPK_MANIFEST_F32(enlarger_diffusion_halo_size);
    SPK_MANIFEST_F32(enlarger_diffusion_bloom_intensity);
    SPK_MANIFEST_F32(enlarger_diffusion_bloom_size);

    SPK_MANIFEST_F32(scanner_lens_blur);
    SPK_MANIFEST_F32_ARRAY(scanner_unsharp, 2);
    SPK_MANIFEST_I32(scanner_white_correction);
    SPK_MANIFEST_I32(scanner_black_correction);
    SPK_MANIFEST_F32(scanner_white_level);
    SPK_MANIFEST_F32(scanner_black_level);

    SPK_MANIFEST_I32(grain_sublayers_active);
    SPK_MANIFEST_F32(grain_particle_area_um2);
    SPK_MANIFEST_F32_ARRAY(grain_particle_scale, 3);
    SPK_MANIFEST_F32_ARRAY(grain_particle_scale_layers, 3);
    SPK_MANIFEST_F32_ARRAY(grain_density_min, 3);
    SPK_MANIFEST_F32_ARRAY(grain_uniformity, 3);
    SPK_MANIFEST_F32(grain_blur);
    SPK_MANIFEST_F32(grain_blur_dye_clouds_um);
    SPK_MANIFEST_F32_ARRAY(grain_micro_structure, 2);
    SPK_MANIFEST_I32(grain_n_sub_layers);

    SPK_MANIFEST_F32(halation_scatter_amount);
    SPK_MANIFEST_F32(halation_scatter_spatial_scale);
    SPK_MANIFEST_F32(halation_halation_amount);
    SPK_MANIFEST_F32(halation_halation_spatial_scale);
    SPK_MANIFEST_F32_ARRAY(halation_scatter_core_um, 3);
    SPK_MANIFEST_F32_ARRAY(halation_scatter_tail_um, 3);
    SPK_MANIFEST_F32_ARRAY(halation_scatter_tail_weight, 3);
    SPK_MANIFEST_F32(halation_boost_ev);
    SPK_MANIFEST_F32(halation_boost_range);
    SPK_MANIFEST_F32(halation_protect_ev);
    SPK_MANIFEST_F32_ARRAY(halation_strength, 3);
    SPK_MANIFEST_F32_ARRAY(halation_first_sigma_um, 3);
    SPK_MANIFEST_I32(halation_n_bounces);
    SPK_MANIFEST_F32(halation_bounce_decay);
    SPK_MANIFEST_I32(halation_renormalize);

    SPK_MANIFEST_F32(dir_amount);
    SPK_MANIFEST_F32(dir_inhibition_samelayer);
    SPK_MANIFEST_F32(dir_inhibition_interlayer);
    SPK_MANIFEST_F32_ARRAY(dir_gamma_samelayer_rgb, 3);
    SPK_MANIFEST_F32_ARRAY(dir_gamma_interlayer_r_to_gb, 2);
    SPK_MANIFEST_F32_ARRAY(dir_gamma_interlayer_g_to_rb, 2);
    SPK_MANIFEST_F32_ARRAY(dir_gamma_interlayer_b_to_rg, 2);
    SPK_MANIFEST_F32(dir_diffusion_size_um);
    SPK_MANIFEST_F32(dir_diffusion_tail_um);
    SPK_MANIFEST_F32(dir_diffusion_tail_weight);

    SPK_MANIFEST_F32(glare_percent);
    SPK_MANIFEST_F32(glare_roughness);
    SPK_MANIFEST_F32(glare_blur);
    SPK_MANIFEST_I32(print_glare_active);
    SPK_MANIFEST_F32(print_glare_percent);
    SPK_MANIFEST_F32(print_glare_roughness);
    SPK_MANIFEST_F32(print_glare_blur);
    SPK_MANIFEST_F32(print_density_curve_gamma);

    SPK_MANIFEST_I32(print_morph_active);
    SPK_MANIFEST_F32(print_morph_gamma_factor);
    SPK_MANIFEST_F32(print_morph_gamma_factor_fast);
    SPK_MANIFEST_F32(print_morph_gamma_factor_slow);
    SPK_MANIFEST_F32(print_morph_gamma_factor_red);
    SPK_MANIFEST_F32(print_morph_gamma_factor_green);
    SPK_MANIFEST_F32(print_morph_gamma_factor_blue);
    SPK_MANIFEST_F32(print_morph_developer_exhaustion);

    SPK_MANIFEST_I32(input_cctf_decoding);
    SPK_MANIFEST_I32(crop);
    SPK_MANIFEST_F32_ARRAY(crop_center, 2);
    SPK_MANIFEST_F32_ARRAY(crop_size, 2);
    SPK_MANIFEST_F32(upscale_factor);

    SPK_MANIFEST_I32(apply_hanatos_window);
    SPK_MANIFEST_I32(apply_hanatos_surface);
    SPK_MANIFEST_F32(spectral_gaussian_blur);
    SPK_MANIFEST_I32(use_enlarger_lut);
    SPK_MANIFEST_I32(use_scanner_lut);
    SPK_MANIFEST_I32(lut_resolution);
    SPK_MANIFEST_I32(neutral_print_filters_from_database);
    SPK_MANIFEST_I32(gpu_preview);
    SPK_MANIFEST_I32(gpu_export);
    SPK_MANIFEST_I32(allow_gpu_scan);

    SPK_MANIFEST_I32(output_gamut_compress);
    SPK_MANIFEST_I32(input_gamut_compress);

    SPK_MANIFEST_I32(tone_curve_active);
    SPK_MANIFEST_I32(tone_curve_master_n);
    SPK_MANIFEST_F32_ARRAY(tone_curve_master_x, SPK_TONE_MAX_PTS);
    SPK_MANIFEST_F32_ARRAY(tone_curve_master_y, SPK_TONE_MAX_PTS);
    SPK_MANIFEST_I32_ARRAY(tone_curve_rgb_n, 3);
    for (size_t channel = 0; channel < 3; ++channel) {
        char name[96];
        std::snprintf(name, sizeof(name), "tone_curve_rgb_x[%zu]", channel);
        append_f32_array(&out, name, p.tone_curve_rgb_x[channel], SPK_TONE_MAX_PTS);
        std::snprintf(name, sizeof(name), "tone_curve_rgb_y[%zu]", channel);
        append_f32_array(&out, name, p.tone_curve_rgb_y[channel], SPK_TONE_MAX_PTS);
    }

    SPK_MANIFEST_I32(disable_buffer_memos);
    SPK_MANIFEST_STR(enlarger_illuminant);
    SPK_MANIFEST_STR(input_color_space);
    SPK_MANIFEST_I32(camera_diffusion_family);
    SPK_MANIFEST_I32(enlarger_diffusion_family);

#undef SPK_MANIFEST_I32
#undef SPK_MANIFEST_F32
#undef SPK_MANIFEST_STR
#undef SPK_MANIFEST_F32_ARRAY
#undef SPK_MANIFEST_I32_ARRAY

    return out;
}

// The manifest intentionally ends at the ABI tail. Appending a field without
// extending named_inventory must make every native build fail loudly.
static_assert(offsetof(spk_params, enlarger_diffusion_family) +
                  sizeof(((spk_params*)nullptr)->enlarger_diffusion_family) ==
              sizeof(spk_params));

}  // namespace spk::params_manifest
