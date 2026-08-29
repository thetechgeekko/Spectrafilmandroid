/*
 * Spektrafilm for Android — JNI bridge.
 * GPLv3. Bridges com.spectrafilm.engine.SpektraEngine (Kotlin) to the spektra C API.
 *
 * Design notes:
 *  - Image buffers cross the boundary as direct java.nio.ByteBuffer (float32 RGB) to
 *    avoid per-pixel JNI traffic; only width/height/colorspace are passed as ints.
 *  - The native engine handle is stored as a Kotlin Long and passed back in.
 *  - SpektraParams (a nested Kotlin data class tree) is marshalled into the flat
 *    spk_params struct by reading the high-traffic fields the scan_film route needs.
 *    Field/method IDs are cached on first use.
 *  - The output is wrapped into a com.spectrafilm.engine.SimResult holding a direct
 *    float ByteBuffer plus width/height/ColorSpace.
 */
#include <jni.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/asset_manager_jni.h>
#include <android/log.h>
#endif

#include "spektra.h"

#define JNI(ret, name) extern "C" JNIEXPORT ret JNICALL \
    Java_com_spectrafilm_engine_SpektraEngine_##name

namespace {

// Throw a java.lang.RuntimeException carrying `msg` so a real, specific failure
// reaches Kotlin instead of collapsing to a bare null return (which the facade
// previously surfaced as a misleading "not implemented yet" error). Safe to call
// even if a pending exception already exists (ThrowNew is a no-op then). Returns
// after queuing the throw; the caller must return promptly to the JVM.
void throw_runtime(JNIEnv* env, const char* msg) {
    if (env->ExceptionCheck()) return;  // don't mask an already-pending exception
    jclass cls = env->FindClass("java/lang/RuntimeException");
    if (cls) {
        env->ThrowNew(cls, msg);
        env->DeleteLocalRef(cls);
    }
}

// Throw a RuntimeException describing an spk_status failure (e.g.
// "spektra: profile not found"). No-op for SPK_OK.
void throw_status(JNIEnv* env, spk_status st) {
    if (st == SPK_OK) return;
    std::string msg = std::string("spektra: ") + spk_status_str(st);
    throw_runtime(env, msg.c_str());
}

// A C++ exception must NEVER unwind through the extern "C" JNI boundary — that
// is std::terminate/SIGABRT, i.e. a hard native crash instead of a catchable
// Java error. Every allocating entry point below is a function-try-block that
// funnels here: std::bad_alloc (any of the engine's full-resolution
// std::vector allocations failing on a low-memory device) becomes
// java.lang.OutOfMemoryError — catchable, matching the existing output-buffer
// OOM path — and anything else becomes a RuntimeException.
void throw_native_oom(JNIEnv* env) {
    if (env->ExceptionCheck()) return;
    jclass oom = env->FindClass("java/lang/OutOfMemoryError");
    if (oom) {
        env->ThrowNew(oom, "spektra: native allocation failed (image too large "
                           "for available memory)");
        env->DeleteLocalRef(oom);
    } else {
        throw_runtime(env, "spektra: native allocation failed");
    }
}

void throw_cpp_exception(JNIEnv* env, const std::exception& e) {
    std::string msg = std::string("spektra: native error: ") + e.what();
    throw_runtime(env, msg.c_str());
}

// Read a jstring into a std::string (empty on null).
std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

// Call a no-arg getter on `obj`, returning the (Object) result, or null.
jobject call_obj(JNIEnv* env, jobject obj, const char* getter, const char* sig) {
    if (!obj) return nullptr;
    jclass cls = env->GetObjectClass(obj);
    jmethodID m = env->GetMethodID(cls, getter, sig);
    env->DeleteLocalRef(cls);
    if (!m) { env->ExceptionClear(); return nullptr; }
    return env->CallObjectMethod(obj, m);
}

float call_float(JNIEnv* env, jobject obj, const char* getter) {
    if (!obj) return 0.0f;
    jclass cls = env->GetObjectClass(obj);
    jmethodID m = env->GetMethodID(cls, getter, "()F");
    env->DeleteLocalRef(cls);
    if (!m) { env->ExceptionClear(); return 0.0f; }
    return env->CallFloatMethod(obj, m);
}

bool call_bool(JNIEnv* env, jobject obj, const char* getter) {
    if (!obj) return false;
    jclass cls = env->GetObjectClass(obj);
    jmethodID m = env->GetMethodID(cls, getter, "()Z");
    env->DeleteLocalRef(cls);
    if (!m) { env->ExceptionClear(); return false; }
    return env->CallBooleanMethod(obj, m) == JNI_TRUE;
}

int call_int(JNIEnv* env, jobject obj, const char* getter) {
    if (!obj) return 0;
    jclass cls = env->GetObjectClass(obj);
    jmethodID m = env->GetMethodID(cls, getter, "()I");
    env->DeleteLocalRef(cls);
    if (!m) { env->ExceptionClear(); return 0; }
    return env->CallIntMethod(obj, m);
}

// Unbox a java.lang.Float (or Number) into a float.
float unbox_float(JNIEnv* env, jobject boxed) {
    if (!boxed) return 0.0f;
    jclass cls = env->GetObjectClass(boxed);
    jmethodID m = env->GetMethodID(cls, "floatValue", "()F");
    env->DeleteLocalRef(cls);
    if (!m) { env->ExceptionClear(); return 0.0f; }
    return env->CallFloatMethod(boxed, m);
}

// Read a kotlin.Triple<Float,Float,Float> via getFirst/getSecond/getThird (each
// returns java.lang.Object -> java.lang.Float). Writes the three components to
// out[3]. Leaves out untouched on null (keeps the default already there).
void read_triple_f(JNIEnv* env, jobject obj, const char* getter, float out[3]) {
    jobject t = call_obj(env, obj, getter, "()Lkotlin/Triple;");
    if (!t) return;
    jobject a = call_obj(env, t, "getFirst", "()Ljava/lang/Object;");
    jobject b = call_obj(env, t, "getSecond", "()Ljava/lang/Object;");
    jobject c = call_obj(env, t, "getThird", "()Ljava/lang/Object;");
    out[0] = unbox_float(env, a);
    out[1] = unbox_float(env, b);
    out[2] = unbox_float(env, c);
    if (a) env->DeleteLocalRef(a);
    if (b) env->DeleteLocalRef(b);
    if (c) env->DeleteLocalRef(c);
    env->DeleteLocalRef(t);
}

// Read a kotlin.Pair<Float,Float> via getFirst/getSecond. Writes to out[2].
void read_pair_f(JNIEnv* env, jobject obj, const char* getter, float out[2]) {
    jobject t = call_obj(env, obj, getter, "()Lkotlin/Pair;");
    if (!t) return;
    jobject a = call_obj(env, t, "getFirst", "()Ljava/lang/Object;");
    jobject b = call_obj(env, t, "getSecond", "()Ljava/lang/Object;");
    out[0] = unbox_float(env, a);
    out[1] = unbox_float(env, b);
    if (a) env->DeleteLocalRef(a);
    if (b) env->DeleteLocalRef(b);
    env->DeleteLocalRef(t);
}

// Map a Kotlin ColorSpace enum (com.spectrafilm.engine.ColorSpace) to spk_color_space
// via its ordinal(). Ordinals match: SRGB=0, ADOBE_RGB=1, PROPHOTO=2, ... LINEAR_SRGB=5.
spk_color_space enum_ordinal_cs(JNIEnv* env, jobject e) {
    if (!e) return SPK_CS_SRGB;
    jclass cls = env->GetObjectClass(e);
    jmethodID m = env->GetMethodID(cls, "ordinal", "()I");
    env->DeleteLocalRef(cls);
    if (!m) { env->ExceptionClear(); return SPK_CS_SRGB; }
    return static_cast<spk_color_space>(env->CallIntMethod(e, m));
}

spk_rgb2raw enum_ordinal_rgb2raw(JNIEnv* env, jobject e) {
    if (!e) return SPK_RGB2RAW_HANATOS2025;
    jclass cls = env->GetObjectClass(e);
    jmethodID m = env->GetMethodID(cls, "ordinal", "()I");
    env->DeleteLocalRef(cls);
    if (!m) { env->ExceptionClear(); return SPK_RGB2RAW_HANATOS2025; }
    return static_cast<spk_rgb2raw>(env->CallIntMethod(e, m));
}

// Marshal a SpektraParams jobject into a flat spk_params. The returned struct
// holds pointers into the std::string storage parameters, which must outlive use.
struct ParamStorage {
    std::string film_profile;
    std::string print_profile;
    std::string auto_exposure_method;
};

// Read a DiffusionFilterParams jobject into the spk_params diffusion-filter
// fields (camera/enlarger share the same struct). `set` writes through a small
// lambda so the same reader serves both prefixes.
void read_diffusion_filter(JNIEnv* env, jobject df,
                           int32_t* active, float* strength, float* spatial_scale,
                           float* halo_warmth, float* core_intensity,
                           float* core_size, float* halo_intensity, float* halo_size,
                           float* bloom_intensity, float* bloom_size) {
    if (!df) return;
    *active = call_bool(env, df, "getActive") ? 1 : 0;
    *strength = call_float(env, df, "getStrength");
    *spatial_scale = call_float(env, df, "getSpatialScale");
    *halo_warmth = call_float(env, df, "getHaloWarmth");
    *core_intensity = call_float(env, df, "getCoreIntensity");
    *core_size = call_float(env, df, "getCoreSize");
    *halo_intensity = call_float(env, df, "getHaloIntensity");
    *halo_size = call_float(env, df, "getHaloSize");
    *bloom_intensity = call_float(env, df, "getBloomIntensity");
    *bloom_size = call_float(env, df, "getBloomSize");
}

// Read the packed tone-curve float[] from SpektraParams.toneCurvePacked() into the
// flat spk_params fields. Layout: [active, mN, (x,y)*mN, rN, (x,y)*rN, gN, ..., bN, ...].
// Per-channel counts are clamped to SPK_TONE_MAX_PTS. Absent/short array leaves the
// tone curve OFF (the spk_default_params value), so this is a strict no-op by default.
void read_tone_curve(JNIEnv* env, jobject params, spk_params* out) {
    jobject arrObj = call_obj(env, params, "toneCurvePacked", "()[F");
    if (!arrObj) { env->ExceptionClear(); return; }
    jfloatArray arr = static_cast<jfloatArray>(arrObj);
    jsize len = env->GetArrayLength(arr);
    if (len < 1) { env->DeleteLocalRef(arrObj); return; }
    std::vector<float> v(static_cast<size_t>(len));
    env->GetFloatArrayRegion(arr, 0, len, v.data());
    env->DeleteLocalRef(arrObj);

    size_t i = 0;
    out->tone_curve_active = (v[i++] != 0.0f) ? 1 : 0;
    auto read_channel = [&](int32_t* n, float* xs, float* ys) {
        *n = 0;
        if (i >= v.size()) return;
        int cnt = static_cast<int>(v[i++]);
        if (cnt < 0) cnt = 0;
        int kept = cnt > SPK_TONE_MAX_PTS ? SPK_TONE_MAX_PTS : cnt;
        for (int k = 0; k < cnt; ++k) {
            if (i + 1 >= v.size()) break;
            float x = v[i++], y = v[i++];
            if (k < kept) { xs[k] = x; ys[k] = y; }
        }
        *n = kept;
    };
    read_channel(&out->tone_curve_master_n, out->tone_curve_master_x, out->tone_curve_master_y);
    for (int c = 0; c < 3; ++c) {
        read_channel(&out->tone_curve_rgb_n[c], out->tone_curve_rgb_x[c], out->tone_curve_rgb_y[c]);
    }
}

// Read a Kotlin enum's ordinal() as an int, returning `def` for a null/failed read.
// Used for the gamut-compression selectors, which spk_params stores as int32 ordinals
// mirroring model/gamut_compression.h's enum-class values (OutputGamutCompress /
// InputGamutCompress). The default sentinel (0) keeps the render path byte-identical.
int enum_ordinal_int(JNIEnv* env, jobject e, int def) {
    if (!e) return def;
    jclass cls = env->GetObjectClass(e);
    jmethodID m = env->GetMethodID(cls, "ordinal", "()I");
    env->DeleteLocalRef(cls);
    if (!m) { env->ExceptionClear(); return def; }
    return static_cast<int>(env->CallIntMethod(e, m));
}

bool marshal_params(JNIEnv* env, jobject params, spk_params* out, ParamStorage* store) {
    // Top-level: filmProfile / printProfile (String), and the nested objects.
    store->film_profile = jstr(env,
        static_cast<jstring>(call_obj(env, params, "getFilmProfile",
                                      "()Ljava/lang/String;")));
    store->print_profile = jstr(env,
        static_cast<jstring>(call_obj(env, params, "getPrintProfile",
                                      "()Ljava/lang/String;")));
    out->film_profile = store->film_profile.c_str();
    out->print_profile = store->print_profile.c_str();

    // Seed every field with the physical defaults so unread/absent fields keep
    // the parity-reproducing defaults; the jobject getters override below.
    spk_default_params(out);
    if (!params) return true;

    jobject camera   = call_obj(env, params, "getCamera",
        "()Lcom/spectrafilm/engine/CameraParams;");
    jobject enlarger = call_obj(env, params, "getEnlarger",
        "()Lcom/spectrafilm/engine/EnlargerParams;");
    jobject scanner  = call_obj(env, params, "getScanner",
        "()Lcom/spectrafilm/engine/ScannerParams;");
    jobject filmR    = call_obj(env, params, "getFilmRender",
        "()Lcom/spectrafilm/engine/FilmRenderingParams;");
    jobject printR   = call_obj(env, params, "getPrintRender",
        "()Lcom/spectrafilm/engine/PrintRenderingParams;");
    jobject io       = call_obj(env, params, "getIo",
        "()Lcom/spectrafilm/engine/IoParams;");
    jobject settings = call_obj(env, params, "getSettings",
        "()Lcom/spectrafilm/engine/SettingsParams;");

    // ---- camera ----
    if (camera) {
        out->exposure_compensation_ev = call_float(env, camera, "getExposureCompensationEv");
        out->auto_exposure = call_bool(env, camera, "getAutoExposure") ? 1 : 0;
        // Forward the metering method string (e.g. "center_weighted"). The owned
        // copy in `store` keeps the pointer valid for the duration of the call;
        // empty/null leaves the spk_default_params value (engine -> center_weighted).
        store->auto_exposure_method = jstr(env,
            static_cast<jstring>(call_obj(env, camera, "getAutoExposureMethod",
                                          "()Ljava/lang/String;")));
        out->auto_exposure_method = store->auto_exposure_method.empty()
            ? nullptr : store->auto_exposure_method.c_str();
        out->lens_blur_um = call_float(env, camera, "getLensBlurUm");
        out->film_format_mm = call_float(env, camera, "getFilmFormatMm");
        read_triple_f(env, camera, "getFilterUv", out->camera_filter_uv);
        read_triple_f(env, camera, "getFilterIr", out->camera_filter_ir);
        jobject df = call_obj(env, camera, "getDiffusionFilter",
            "()Lcom/spectrafilm/engine/DiffusionFilterParams;");
        read_diffusion_filter(env, df, &out->camera_diffusion_active,
            &out->camera_diffusion_strength, &out->camera_diffusion_spatial_scale,
            &out->camera_diffusion_halo_warmth, &out->camera_diffusion_core_intensity,
            &out->camera_diffusion_core_size, &out->camera_diffusion_halo_intensity,
            &out->camera_diffusion_halo_size, &out->camera_diffusion_bloom_intensity,
            &out->camera_diffusion_bloom_size);
        if (df) env->DeleteLocalRef(df);
    }

    // ---- enlarger ----
    if (enlarger) {
        out->print_exposure = call_float(env, enlarger, "getPrintExposure");
        out->print_exposure_compensation =
            call_bool(env, enlarger, "getPrintExposureCompensation") ? 1 : 0;
        out->normalize_print_exposure =
            call_bool(env, enlarger, "getNormalizePrintExposure") ? 1 : 0;
        out->y_filter_shift = call_float(env, enlarger, "getYFilterShift");
        out->m_filter_shift = call_float(env, enlarger, "getMFilterShift");
        out->y_filter_neutral = call_float(env, enlarger, "getYFilterNeutral");
        out->m_filter_neutral = call_float(env, enlarger, "getMFilterNeutral");
        out->c_filter_neutral = call_float(env, enlarger, "getCFilterNeutral");
        out->enlarger_lens_blur = call_float(env, enlarger, "getLensBlur");
        out->preflash_exposure = call_float(env, enlarger, "getPreflashExposure");
        out->preflash_y_filter_shift = call_float(env, enlarger, "getPreflashYFilterShift");
        out->preflash_m_filter_shift = call_float(env, enlarger, "getPreflashMFilterShift");
        jobject df = call_obj(env, enlarger, "getDiffusionFilter",
            "()Lcom/spectrafilm/engine/DiffusionFilterParams;");
        read_diffusion_filter(env, df, &out->enlarger_diffusion_active,
            &out->enlarger_diffusion_strength, &out->enlarger_diffusion_spatial_scale,
            &out->enlarger_diffusion_halo_warmth, &out->enlarger_diffusion_core_intensity,
            &out->enlarger_diffusion_core_size, &out->enlarger_diffusion_halo_intensity,
            &out->enlarger_diffusion_halo_size, &out->enlarger_diffusion_bloom_intensity,
            &out->enlarger_diffusion_bloom_size);
        if (df) env->DeleteLocalRef(df);
    }

    // ---- scanner ----
    if (scanner) {
        out->scanner_lens_blur = call_float(env, scanner, "getLensBlur");
        out->scanner_white_correction = call_bool(env, scanner, "getWhiteCorrection") ? 1 : 0;
        out->scanner_black_correction = call_bool(env, scanner, "getBlackCorrection") ? 1 : 0;
        out->scanner_white_level = call_float(env, scanner, "getWhiteLevel");
        out->scanner_black_level = call_float(env, scanner, "getBlackLevel");
        read_pair_f(env, scanner, "getUnsharpMask", out->scanner_unsharp);
    }

    // ---- film rendering ----
    if (filmR) {
        out->density_curve_gamma = call_float(env, filmR, "getDensityCurveGamma");
        jobject grain = call_obj(env, filmR, "getGrain",
            "()Lcom/spectrafilm/engine/GrainParams;");
        jobject halation = call_obj(env, filmR, "getHalation",
            "()Lcom/spectrafilm/engine/HalationParams;");
        jobject dir = call_obj(env, filmR, "getDirCouplers",
            "()Lcom/spectrafilm/engine/DirCouplersParams;");
        jobject glare = call_obj(env, filmR, "getGlare",
            "()Lcom/spectrafilm/engine/GlareParams;");

        if (grain) {
            out->grain_active = call_bool(env, grain, "getActive") ? 1 : 0;
            out->grain_sublayers_active = call_bool(env, grain, "getSublayersActive") ? 1 : 0;
            out->grain_particle_area_um2 = call_float(env, grain, "getAgxParticleAreaUm2");
            read_triple_f(env, grain, "getAgxParticleScale", out->grain_particle_scale);
            read_triple_f(env, grain, "getAgxParticleScaleLayers", out->grain_particle_scale_layers);
            read_triple_f(env, grain, "getDensityMin", out->grain_density_min);
            read_triple_f(env, grain, "getUniformity", out->grain_uniformity);
            out->grain_blur = call_float(env, grain, "getBlur");
            out->grain_blur_dye_clouds_um = call_float(env, grain, "getBlurDyeCloudsUm");
            read_pair_f(env, grain, "getMicroStructure", out->grain_micro_structure);
            out->grain_n_sub_layers = call_int(env, grain, "getNSubLayers");
            env->DeleteLocalRef(grain);
        }
        if (halation) {
            out->halation_active = call_bool(env, halation, "getActive") ? 1 : 0;
            out->halation_scatter_amount = call_float(env, halation, "getScatterAmount");
            out->halation_scatter_spatial_scale = call_float(env, halation, "getScatterSpatialScale");
            out->halation_halation_amount = call_float(env, halation, "getHalationAmount");
            out->halation_halation_spatial_scale = call_float(env, halation, "getHalationSpatialScale");
            read_triple_f(env, halation, "getScatterCoreUm", out->halation_scatter_core_um);
            read_triple_f(env, halation, "getScatterTailUm", out->halation_scatter_tail_um);
            read_triple_f(env, halation, "getScatterTailWeight", out->halation_scatter_tail_weight);
            out->halation_boost_ev = call_float(env, halation, "getBoostEv");
            out->halation_boost_range = call_float(env, halation, "getBoostRange");
            out->halation_protect_ev = call_float(env, halation, "getProtectEv");
            read_triple_f(env, halation, "getHalationStrength", out->halation_strength);
            read_triple_f(env, halation, "getHalationFirstSigmaUm", out->halation_first_sigma_um);
            out->halation_n_bounces = call_int(env, halation, "getHalationNBounces");
            out->halation_bounce_decay = call_float(env, halation, "getHalationBounceDecay");
            out->halation_renormalize = call_bool(env, halation, "getHalationRenormalize") ? 1 : 0;
            env->DeleteLocalRef(halation);
        }
        if (dir) {
            out->dir_couplers_active = call_bool(env, dir, "getActive") ? 1 : 0;
            out->dir_amount = call_float(env, dir, "getAmount");
            out->dir_inhibition_samelayer = call_float(env, dir, "getInhibitionSamelayer");
            out->dir_inhibition_interlayer = call_float(env, dir, "getInhibitionInterlayer");
            read_triple_f(env, dir, "getGammaSamelayerRgb", out->dir_gamma_samelayer_rgb);
            read_pair_f(env, dir, "getGammaInterlayerRToGb", out->dir_gamma_interlayer_r_to_gb);
            read_pair_f(env, dir, "getGammaInterlayerGToRb", out->dir_gamma_interlayer_g_to_rb);
            read_pair_f(env, dir, "getGammaInterlayerBToRg", out->dir_gamma_interlayer_b_to_rg);
            out->dir_diffusion_size_um = call_float(env, dir, "getDiffusionSizeUm");
            out->dir_diffusion_tail_um = call_float(env, dir, "getDiffusionTailUm");
            out->dir_diffusion_tail_weight = call_float(env, dir, "getDiffusionTailWeight");
            env->DeleteLocalRef(dir);
        }
        if (glare) {
            out->glare_active = call_bool(env, glare, "getActive") ? 1 : 0;
            out->glare_percent = call_float(env, glare, "getPercent");
            out->glare_roughness = call_float(env, glare, "getRoughness");
            out->glare_blur = call_float(env, glare, "getBlur");
            env->DeleteLocalRef(glare);
        }
    }

    // ---- print rendering ----
    if (printR) {
        out->print_density_curve_gamma = call_float(env, printR, "getDensityCurveGamma");
        jobject pglare = call_obj(env, printR, "getGlare",
            "()Lcom/spectrafilm/engine/GlareParams;");
        if (pglare) {
            out->print_glare_active = call_bool(env, pglare, "getActive") ? 1 : 0;
            out->print_glare_percent = call_float(env, pglare, "getPercent");
            out->print_glare_roughness = call_float(env, pglare, "getRoughness");
            out->print_glare_blur = call_float(env, pglare, "getBlur");
            env->DeleteLocalRef(pglare);
        }
        // OPT-IN s023 print density-curve morph. Absent / active=false -> the
        // engine defaults (off, identity) set in spk_default_params remain.
        jobject pmorph = call_obj(env, printR, "getDensityCurvesMorph",
            "()Lcom/spectrafilm/engine/PrintCurvesMorphParams;");
        if (pmorph) {
            out->print_morph_active = call_bool(env, pmorph, "getActive") ? 1 : 0;
            out->print_morph_gamma_factor = call_float(env, pmorph, "getGammaFactor");
            out->print_morph_gamma_factor_fast = call_float(env, pmorph, "getGammaFactorFast");
            out->print_morph_gamma_factor_slow = call_float(env, pmorph, "getGammaFactorSlow");
            out->print_morph_gamma_factor_red = call_float(env, pmorph, "getGammaFactorRed");
            out->print_morph_gamma_factor_green = call_float(env, pmorph, "getGammaFactorGreen");
            out->print_morph_gamma_factor_blue = call_float(env, pmorph, "getGammaFactorBlue");
            out->print_morph_developer_exhaustion = call_float(env, pmorph, "getDeveloperExhaustion");
            env->DeleteLocalRef(pmorph);
        }
    }

    // ---- io ----
    if (io) {
        out->scan_film = call_bool(env, io, "getScanFilm") ? 1 : 0;
        out->output_cctf_encoding = call_bool(env, io, "getOutputCctfEncoding") ? 1 : 0;
        out->input_cctf_decoding = call_bool(env, io, "getInputCctfDecoding") ? 1 : 0;
        out->crop = call_bool(env, io, "getCrop") ? 1 : 0;
        out->upscale_factor = call_float(env, io, "getUpscaleFactor");
        read_pair_f(env, io, "getCropCenter", out->crop_center);
        read_pair_f(env, io, "getCropSize", out->crop_size);
        jobject ocs = call_obj(env, io, "getOutputColorSpace",
            "()Lcom/spectrafilm/engine/ColorSpace;");
        out->output_color_space = enum_ordinal_cs(env, ocs);
        if (ocs) env->DeleteLocalRef(ocs);
        // OPT-IN gamut compression selectors. Ordinals mirror model/gamut_compression.h;
        // the default sentinels (output kLegacyClip=0 / input kOff=0) leave the render
        // path byte-identical, so an absent/old IoParams keeps the spk_default_params 0s.
        jobject ogc = call_obj(env, io, "getOutputGamutCompress",
            "()Lcom/spectrafilm/engine/OutputGamutCompress;");
        out->output_gamut_compress = enum_ordinal_int(env, ogc, 0);
        if (ogc) env->DeleteLocalRef(ogc);
        jobject igc = call_obj(env, io, "getInputGamutCompress",
            "()Lcom/spectrafilm/engine/InputGamutCompress;");
        out->input_gamut_compress = enum_ordinal_int(env, igc, 0);
        if (igc) env->DeleteLocalRef(igc);
    }

    // ---- settings ----
    if (settings) {
        jobject m = call_obj(env, settings, "getRgbToRawMethod",
            "()Lcom/spectrafilm/engine/Rgb2Raw;");
        out->rgb_to_raw_method = enum_ordinal_rgb2raw(env, m);
        if (m) env->DeleteLocalRef(m);
        out->apply_hanatos_window =
            call_bool(env, settings, "getApplyHanatos2025AdaptationWindow") ? 1 : 0;
        out->apply_hanatos_surface =
            call_bool(env, settings, "getApplyHanatos2025AdaptationSurface") ? 1 : 0;
        out->spectral_gaussian_blur = call_float(env, settings, "getSpectralGaussianBlur");
        out->use_enlarger_lut = call_bool(env, settings, "getUseEnlargerLut") ? 1 : 0;
        out->use_scanner_lut = call_bool(env, settings, "getUseScannerLut") ? 1 : 0;
        // GPU preview fast-path toggle (GPU M1, #146; default false). Consulted
        // only by spk_simulate_preview — export renders ignore it by design.
        out->gpu_preview = call_bool(env, settings, "getGpuPreview") ? 1 : 0;
        // Experimental GPU export toggle (#154; default false). Consulted only by
        // spk_simulate (export); preview clears it, tap/bake hard-zero the latch.
        out->gpu_export = call_bool(env, settings, "getGpuExport") ? 1 : 0;
        out->lut_resolution = call_int(env, settings, "getLutResolution");
        out->preview_max_size = call_int(env, settings, "getPreviewMaxSize");
        out->neutral_print_filters_from_database =
            call_bool(env, settings, "getNeutralPrintFiltersFromDatabase") ? 1 : 0;
    }

    // Tone curve (top-level packed float[]); OFF by default => no-op.
    read_tone_curve(env, params, out);

    if (camera) env->DeleteLocalRef(camera);
    if (enlarger) env->DeleteLocalRef(enlarger);
    if (scanner) env->DeleteLocalRef(scanner);
    if (filmR) env->DeleteLocalRef(filmR);
    if (printR) env->DeleteLocalRef(printR);
    if (io) env->DeleteLocalRef(io);
    if (settings) env->DeleteLocalRef(settings);
    return true;
}

}  // namespace

JNI(jlong, nativeCreate)(JNIEnv* env, jobject /*thiz*/, jstring assetDir) try {
    std::string dir = jstr(env, assetDir);
    if (dir.empty()) {
        // AAssetManager path not yet wired; require an extracted dir.
        throw_runtime(env, "spektra: assetDir is null/empty (extracted asset "
                           "directory required)");
        return 0;
    }
    spk_engine* eng = nullptr;
    spk_status st = spk_engine_create(dir.c_str(), &eng);
    if (st != SPK_OK) { throw_status(env, st); return 0; }
    return reinterpret_cast<jlong>(eng);
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
    return 0;
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
    return 0;
}

/*
 * nativeCreateFromAssets(assetManager) -> engine handle (Long).
 * Builds an engine that reads its bundled assets directly from the APK via the
 * app's AssetManager (no on-device extraction). The Kotlin side must keep the
 * AssetManager referenced for the engine's lifetime — AAssetManager_fromJava
 * returns a pointer valid only while the Java AssetManager is alive. Returns 0
 * (with a thrown RuntimeException) on failure so the Kotlin side can fall back to
 * the extract-then-create path.
 */
JNI(jlong, nativeCreateFromAssets)(JNIEnv* env, jobject /*thiz*/,
                                   jobject assetManager) try {
#ifdef __ANDROID__
    if (!assetManager) {
        throw_runtime(env, "spektra: assetManager is null");
        return 0;
    }
    AAssetManager* mgr = AAssetManager_fromJava(env, assetManager);
    if (!mgr) {
        throw_runtime(env, "spektra: AAssetManager_fromJava returned null");
        return 0;
    }
    spk_engine* eng = nullptr;
    spk_status st = spk_engine_create_asset_manager(mgr, &eng);
    if (st != SPK_OK) { throw_status(env, st); return 0; }
    return reinterpret_cast<jlong>(eng);
#else
    (void)assetManager;
    throw_runtime(env, "spektra: AAssetManager mode unavailable (not Android)");
    return 0;
#endif
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
    return 0;
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
    return 0;
}

JNI(void, nativeDestroy)(JNIEnv* /*env*/, jobject /*thiz*/, jlong handle) {
    spk_engine_destroy(reinterpret_cast<spk_engine*>(handle));
}

JNI(jstring, nativeListProfiles)(JNIEnv* env, jobject /*thiz*/, jlong handle) try {
    spk_engine* eng = reinterpret_cast<spk_engine*>(handle);
    if (!eng) return env->NewStringUTF("");
    size_t needed = 0;
    spk_engine_list_profiles(eng, nullptr, 0, &needed);
    if (needed == 0) return env->NewStringUTF("");
    std::vector<char> buf(needed);
    if (spk_engine_list_profiles(eng, buf.data(), buf.size(), &needed) != SPK_OK)
        return env->NewStringUTF("");
    return env->NewStringUTF(buf.data());
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
    return nullptr;
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
    return nullptr;
}

/*
 * nativeSimulate(handle, inBuf, w, h, inCs, paramsObj, preview) -> SimResult.
 * Reads a direct float ByteBuffer of w*h*3 floats, marshals the params, runs
 * spk_simulate(_preview), and wraps the result into a SimResult with a fresh
 * direct float ByteBuffer.
 */
JNI(jobject, nativeSimulate)(JNIEnv* env, jobject /*thiz*/, jlong handle,
                             jobject inBuf, jint w, jint h, jstring inCs,
                             jobject paramsObj, jboolean preview) try {
    spk_engine* eng = reinterpret_cast<spk_engine*>(handle);
    if (!eng) { throw_runtime(env, "spektra: engine handle is null"); return nullptr; }
    if (!inBuf) { throw_runtime(env, "spektra: input ByteBuffer is null"); return nullptr; }
    if (w <= 0 || h <= 0) {
        throw_runtime(env, "spektra: invalid image dimensions");
        return nullptr;
    }

    float* in_data = static_cast<float*>(env->GetDirectBufferAddress(inBuf));
    if (!in_data) {
        throw_runtime(env, "spektra: input ByteBuffer is not direct");
        return nullptr;
    }
    // Reject buffers too small for w*h*3 float32 to avoid out-of-bounds reads.
    // The required byte count is computed in 64-bit to avoid overflow.
    const jlong cap = env->GetDirectBufferCapacity(inBuf);
    const int64_t need_bytes =
        static_cast<int64_t>(w) * h * 3 * static_cast<int64_t>(sizeof(float));
    if (cap < 0 || static_cast<int64_t>(cap) < need_bytes) {
        throw_runtime(env, "spektra: input ByteBuffer capacity too small for "
                           "width*height*3*4 bytes");
        return nullptr;
    }

    // Honor the buffer's colour-space tag. The engine ingests linear ProPhoto RGB
    // only (runtime InputColorSpace has a single value, kProPhotoRGB); every decode
    // path (LibRaw raw_decoder, the platform/photo bitmap decoder, the synthetic
    // image) already emits ProPhoto. Validating here means a non-ProPhoto buffer
    // fails loudly instead of being silently re-interpreted through the ProPhoto
    // primaries — the exact failure mode that mis-rendered native RAW decodes when
    // the decoder still tagged its output "ACES2065-1". Empty/null tag => legacy
    // default (treated as ProPhoto).
    {
        const std::string in_cs = jstr(env, inCs);
        if (!in_cs.empty() && in_cs != "ProPhoto RGB") {
            throw_runtime(env,
                ("spektra: unsupported input color space '" + in_cs +
                 "' (the engine accepts linear ProPhoto RGB only)").c_str());
            return nullptr;
        }
    }

    spk_params params;
    ParamStorage store;
    if (!marshal_params(env, paramsObj, &params, &store)) {
        throw_runtime(env, "spektra: failed to marshal params");
        return nullptr;
    }

    // Input is linear ProPhoto RGB (validated above); the engine's filming stage
    // interprets it as such (FilmingParams::input_color_space == kProPhotoRGB).
    spk_image in_img{in_data, w, h, static_cast<int>(SPK_CS_PROPHOTO)};
    spk_image out{};
    // One-shot memo opt-out (EXPORT_FASTPATH item 2): the app's non-preview
    // renders are exports and magnifier crops — their memo key can never be
    // re-used, so the full-buffer key hashing + result copies (hundreds of ms
    // + ~2x full-res buffers held in the engine at 12 MP) are pure overhead.
    // The memos are transparent (byte-identical hit or miss), so this cannot
    // change output pixels. Preview renders keep the memos — that is what
    // they exist for.
    if (!preview) params.disable_buffer_memos = 1;
    spk_status st = preview ? spk_simulate_preview(eng, &in_img, &params, &out)
                            : spk_simulate(eng, &in_img, &params, &out);
#ifdef __ANDROID__
    // One-time diagnostic when the GPU preview self-check failed (state 2): the
    // preview silently stays on the CPU path for this process (#146 mandate —
    // fall back, but never silently for the log).
    if (preview) {
        // One-time logs so the toggle is externally verifiable from logcat
        // (#146 on-device validation found silence was ambiguous: only the
        // failure case logged, so "passed" and "never ran" looked identical).
        static bool gpu_state_logged = false;
        const int gpu_state = spk_gpu_scan_state();
        if (!gpu_state_logged && gpu_state != 0) {
            gpu_state_logged = true;
            if (gpu_state == 1) {
                __android_log_print(ANDROID_LOG_INFO, "Spektra",
                                    "gpu preview self-check PASSED on this device/driver");
            } else {
                __android_log_print(ANDROID_LOG_WARN, "Spektra",
                                    "gpu preview self-check FAILED on this device/driver; "
                                    "previews stay on the CPU path this session");
            }
        }
        static bool gpu_engaged_logged = false;
        if (!gpu_engaged_logged && spk_gpu_scan_frames() > 0) {
            gpu_engaged_logged = true;
            __android_log_print(ANDROID_LOG_INFO, "Spektra",
                                "gpu scan path ACTIVE (eligible preview frames render on the GPU)");
        }
        // Same pair for the PRINT-EXPOSE offload (perf lab). It is a separate
        // kernel dispatch with its own self-check, and it only runs on the print
        // route, so "scan ACTIVE" says nothing about it — without these two the
        // print offload would be exactly the ambiguous silence #146 called out.
        static bool gpu_print_state_logged = false;
        const int gpu_print = spk_gpu_print_state();
        if (!gpu_print_state_logged && gpu_print != 0) {
            gpu_print_state_logged = true;
            if (gpu_print == 1) {
                __android_log_print(ANDROID_LOG_INFO, "Spektra",
                                    "gpu print-expose self-check PASSED on this device/driver");
            } else {
                __android_log_print(ANDROID_LOG_WARN, "Spektra",
                                    "gpu print-expose self-check FAILED on this device/driver; "
                                    "the print integral stays on the CPU path this session");
            }
        }
        static bool gpu_print_engaged_logged = false;
        if (!gpu_print_engaged_logged && spk_gpu_print_frames() > 0) {
            gpu_print_engaged_logged = true;
            __android_log_print(ANDROID_LOG_INFO, "Spektra",
                                "gpu print-expose path ACTIVE (print-route frames render "
                                "their spectral integral on the GPU)");
        }
    }
    // Per-stage/per-filter timing of this render (#146/#152): one line so the
    // owner can see where the latency goes (grain/halation/filming vs scan, and
    // the cold-start LUT bakes). Empty only if nothing ran.
    {
        char tbuf[512];
        if (spk_stage_timings(tbuf, sizeof(tbuf)) > 0) {
            __android_log_print(ANDROID_LOG_INFO, "Spektra",
                                "stage timings ms [%s]: %s",
                                preview ? "preview" : "export", tbuf);
        }
    }
#endif
    if (st != SPK_OK) { throw_status(env, st); return nullptr; }
    if (!out.data) { throw_runtime(env, "spektra: engine returned no data"); return nullptr; }

    // Hand the output back as a NATIVE (off-heap) direct ByteBuffer rather than a
    // JVM-managed one. `ByteBuffer.allocateDirect` is backed on Android by a
    // non-movable byte[] on the ART managed heap, so a full-res result (~140 MB) plus
    // the equally large RAW input buffer blows the ~256 MB heap-growth limit at export
    // (OutOfMemoryError). Adobe Lightroom's native engine keeps full-res pixels in
    // native memory and never crosses them to the Java heap; we mirror that here by
    // malloc'ing the result and wrapping it with NewDirectByteBuffer. The Kotlin
    // SimResult owns it and frees it via SimResult.freeDirectBuffer (below).
    const size_t n = static_cast<size_t>(out.width) * out.height * 3;

    // Guard against >2 GiB: NewDirectByteBuffer takes a jlong capacity, but the Kotlin
    // side reads it through a jint-indexed FloatBuffer, so keep the existing 2 GiB cap.
    // Computed in int64 to avoid its own overflow. (Security review F2.)
    const int64_t out_bytes = static_cast<int64_t>(n) * static_cast<int64_t>(sizeof(float));
    if (out_bytes > static_cast<int64_t>(INT32_MAX)) {
        spk_image_free(&out);
        throw_runtime(env, "spektra: output image too large for a direct ByteBuffer (>2 GiB)");
        return nullptr;
    }

    void* native_buf = std::malloc(static_cast<size_t>(out_bytes));
    if (!native_buf) {
        spk_image_free(&out);
        jclass oom = env->FindClass("java/lang/OutOfMemoryError");
        if (oom) env->ThrowNew(oom, "spektra: failed to allocate native output buffer");
        else throw_runtime(env, "spektra: failed to allocate native output buffer");
        return nullptr;
    }
    std::memcpy(native_buf, out.data, static_cast<size_t>(out_bytes));

    // Capture dims BEFORE freeing the engine-side image.
    int out_w = out.width, out_h = out.height, out_cs = out.color_space;
    spk_image_free(&out);  // engine-side copy no longer needed

    jobject outBuf = env->NewDirectByteBuffer(native_buf, out_bytes);
    if (env->ExceptionCheck() || !outBuf) {
        std::free(native_buf);
        if (!env->ExceptionCheck())
            throw_runtime(env, "spektra: failed to wrap native output buffer");
        return nullptr;
    }

    // Build the SimResult(data: ByteBuffer, width: Int, height: Int, colorSpace: ColorSpace).
    // NOTE: NewDirectByteBuffer does NOT take ownership — if SimResult is never
    // constructed below, the Kotlin side can't free native_buf, so every failure
    // path from here on must std::free(native_buf) to avoid leaking the result.
    jclass csCls = env->FindClass("com/spectrafilm/engine/ColorSpace");
    if (!csCls) {
        env->ExceptionClear();
        env->DeleteLocalRef(outBuf);
        std::free(native_buf);
        throw_runtime(env, "spektra: ColorSpace class not found");
        return nullptr;
    }
    jmethodID csValues = env->GetStaticMethodID(csCls, "values",
        "()[Lcom/spectrafilm/engine/ColorSpace;");
    jobjectArray csArr = nullptr;
    if (csValues) {
        csArr = static_cast<jobjectArray>(
            env->CallStaticObjectMethod(csCls, csValues));
        if (env->ExceptionCheck()) { env->ExceptionClear(); csArr = nullptr; }
    } else {
        env->ExceptionClear();
    }
    int ord = out_cs;
    int len = csArr ? env->GetArrayLength(csArr) : 0;
    if (ord < 0 || ord >= len) ord = 0;
    jobject csObj = csArr ? env->GetObjectArrayElement(csArr, ord) : nullptr;
    env->DeleteLocalRef(csCls);

    jclass resCls = env->FindClass("com/spectrafilm/engine/SimResult");
    if (!resCls) {
        env->ExceptionClear();
        if (csArr) env->DeleteLocalRef(csArr);
        if (csObj) env->DeleteLocalRef(csObj);
        env->DeleteLocalRef(outBuf);
        std::free(native_buf);
        throw_runtime(env, "spektra: SimResult class not found");
        return nullptr;
    }
    jmethodID resCtor = env->GetMethodID(resCls, "<init>",
        "(Ljava/nio/ByteBuffer;IILcom/spectrafilm/engine/ColorSpace;)V");
    jobject result = nullptr;
    if (resCtor) {
        result = env->NewObject(resCls, resCtor, outBuf, out_w, out_h, csObj);
        if (env->ExceptionCheck()) { env->ExceptionClear(); result = nullptr; }
    } else {
        env->ExceptionClear();
    }

    if (csArr) env->DeleteLocalRef(csArr);
    if (csObj) env->DeleteLocalRef(csObj);
    env->DeleteLocalRef(resCls);
    env->DeleteLocalRef(outBuf);
    if (!result) {
        // SimResult construction failed: nothing on the Kotlin side will free the
        // native result buffer, so release it here.
        std::free(native_buf);
        throw_runtime(env, "spektra: failed to construct SimResult");
        return nullptr;
    }
    return result;
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
    return nullptr;
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
    return nullptr;
}

/*
 * SimResult.freeDirectBuffer(buf) — release a native (malloc + NewDirectByteBuffer)
 * engine-output buffer. Called from SimResult.close(). Named to match the Kotlin
 * @JvmStatic companion method (Java_com_spectrafilm_engine_SimResult_freeDirectBuffer),
 * NOT the SpektraEngine JNI() macro. free(nullptr) is a no-op, and freeing a buffer
 * whose address can't be resolved is skipped.
 */
extern "C" JNIEXPORT void JNICALL
Java_com_spectrafilm_engine_SimResult_freeDirectBuffer(JNIEnv* env, jclass /*clazz*/,
                                                       jobject buf) {
    if (!buf) return;
    void* p = env->GetDirectBufferAddress(buf);
    if (p) std::free(p);
}

/*
 * SimResult.allocDirectBuffer(size) -> ByteBuffer (malloc + NewDirectByteBuffer) or null.
 * An OFF-HEAP direct buffer of `size` bytes, NOT on the ART managed heap (unlike
 * ByteBuffer.allocateDirect, which on Android is a non-movable byte[] counting against the
 * ~256 MB heap-growth limit). Used for large export staging buffers (e.g. the 16-bit
 * TIFF/PNG quantise buffer, ~600 MB at 100 MP). The caller MUST free it with
 * freeDirectBuffer(). Returns null on bad size or OOM (caller falls back to managed).
 */
extern "C" JNIEXPORT jobject JNICALL
Java_com_spectrafilm_engine_SimResult_allocDirectBuffer(JNIEnv* env, jclass /*clazz*/,
                                                        jlong size) {
    if (size <= 0) return nullptr;
    void* p = std::malloc(static_cast<size_t>(size));
    if (!p) return nullptr;
    jobject buf = env->NewDirectByteBuffer(p, size);
    if (!buf) { std::free(p); return nullptr; }  // wrap failed -> don't leak
    return buf;
}

/*
 * nativeMeterExposureEv(handle, inBuf, w, h, paramsObj) -> double EV.
 * Meters the image the way a render would and returns the auto-exposure
 * compensation in EV (linear gain = 2**ev), without rendering. Used by the LUT
 * preview paths, which must supply the exposure gain themselves because a baked
 * 3D LUT carries none (see spk_meter_exposure_ev / spk_bake_cube_lut).
 */
JNI(jdouble, nativeMeterExposureEv)(JNIEnv* env, jobject /*thiz*/, jlong handle,
                                    jobject inBuf, jint w, jint h,
                                    jobject paramsObj) try {
    spk_engine* eng = reinterpret_cast<spk_engine*>(handle);
    if (!eng) { throw_runtime(env, "spektra: engine handle is null"); return 0.0; }
    if (!inBuf) { throw_runtime(env, "spektra: input ByteBuffer is null"); return 0.0; }
    if (w <= 0 || h <= 0) {
        throw_runtime(env, "spektra: invalid image dimensions");
        return 0.0;
    }
    float* in_data = static_cast<float*>(env->GetDirectBufferAddress(inBuf));
    if (!in_data) {
        throw_runtime(env, "spektra: input ByteBuffer is not direct");
        return 0.0;
    }
    // Same 64-bit capacity guard as nativeSimulate: reject anything too small for
    // w*h*3 float32 rather than reading out of bounds.
    const jlong cap = env->GetDirectBufferCapacity(inBuf);
    const int64_t need_bytes =
        static_cast<int64_t>(w) * h * 3 * static_cast<int64_t>(sizeof(float));
    if (cap < 0 || static_cast<int64_t>(cap) < need_bytes) {
        throw_runtime(env, "spektra: input ByteBuffer capacity too small for "
                           "width*height*3*4 bytes");
        return 0.0;
    }

    spk_params params;
    ParamStorage store;
    if (!marshal_params(env, paramsObj, &params, &store)) {
        throw_runtime(env, "spektra: failed to marshal params");
        return 0.0;
    }

    spk_image img{in_data, w, h, static_cast<int32_t>(SPK_CS_PROPHOTO)};
    double ev = 0.0;
    spk_status st = spk_meter_exposure_ev(eng, &img, &params, &ev);
    if (st != SPK_OK) { throw_status(env, st); return 0.0; }
    return static_cast<jdouble>(ev);
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
    return 0.0;
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
    return 0.0;
}

/*
 * nativeBakeCubeLut(handle, paramsObj, size) -> String (.cube text) or null.
 * Marshals the params (reusing marshal_params), bakes a size^3 3D LUT of the
 * current film look, and returns the .cube text. The bake forces all spatial /
 * stochastic effects off (see spk_bake_cube_lut). Sized in two passes: first to
 * learn the required buffer length, then to fill it.
 */
JNI(jstring, nativeBakeCubeLut)(JNIEnv* env, jobject /*thiz*/, jlong handle,
                                jobject paramsObj, jint size, jint shaper) try {
    spk_engine* eng = reinterpret_cast<spk_engine*>(handle);
    if (!eng) { throw_runtime(env, "spektra: engine handle is null"); return nullptr; }

    spk_params params;
    ParamStorage store;
    if (!marshal_params(env, paramsObj, &params, &store)) {
        throw_runtime(env, "spektra: failed to marshal params");
        return nullptr;
    }

    size_t needed = 0;
    spk_bake_cube_lut(eng, &params, size, shaper, nullptr, 0, &needed);
    // The sizing pass returns SPK_ERR_BAD_ARGS (null buffer) but still sets
    // `needed`; needed==0 means a real failure (bad profile, internal) — surface it.
    if (needed == 0) {
        // Re-run to capture the actual status for a meaningful message.
        spk_status probe = spk_bake_cube_lut(eng, &params, size, shaper, nullptr, 0, &needed);
        throw_status(env, probe == SPK_OK ? SPK_ERR_INTERNAL : probe);
        return nullptr;
    }

    std::vector<char> buf(needed);
    spk_status st = spk_bake_cube_lut(eng, &params, size, shaper, buf.data(), buf.size(),
                                      &needed);
    if (st != SPK_OK) { throw_status(env, st); return nullptr; }
    return env->NewStringUTF(buf.data());
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
    return nullptr;
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
    return nullptr;
}

/*
 * Big-core pinning (perf-lab, issue #117). The engine gates this on the
 * SPK_BIG_CORES env var, which an Android app cannot set for its own process —
 * the JVM is already up by the time any Kotlin runs. These two forward the
 * setting so the win is reachable from the shipping build.
 *
 * Instance-less (@JvmStatic in the companion): pinning is a process-wide policy
 * for the render pool, not per-engine state.
 */
JNI(void, nativeSetBigCores)(JNIEnv* /*env*/, jclass /*clazz*/, jint mode) {
    spk_set_big_cores(static_cast<int>(mode));
}

JNI(jint, nativeBigCoreCount)(JNIEnv* /*env*/, jclass /*clazz*/) {
    return static_cast<jint>(spk_big_core_count());
}
