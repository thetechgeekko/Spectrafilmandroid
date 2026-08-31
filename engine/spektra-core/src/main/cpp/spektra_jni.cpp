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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <sys/system_properties.h>   // debug.spektra.dumpparams (dump_marshalled_params)
#endif

#include <atomic>

#include "jni_safety.h"
#include "runtime/params_manifest.h"
#include "runtime/stage_timer.h"
#include "spektra.h"

#define JNI(ret, name) extern "C" JNIEXPORT ret JNICALL \
    Java_com_spectrafilm_engine_SpektraEngine_##name

namespace {

spk::jni::AllocationRegistry g_allocations;

// Throw a java.lang.RuntimeException carrying `msg` so a real, specific failure
// reaches Kotlin instead of collapsing to a bare null return (which the facade
// previously surfaced as a misleading "not implemented yet" error). Safe to call
// even if a pending exception already exists (ThrowNew is a no-op then). Returns
// after queuing the throw; the caller must return promptly to the JVM.
void throw_runtime(JNIEnv* env, const char* msg) noexcept {
    if (env->ExceptionCheck()) return;  // don't mask an already-pending exception
    jclass cls = env->FindClass("java/lang/RuntimeException");
    if (cls) {
        env->ThrowNew(cls, msg);
        env->DeleteLocalRef(cls);
    }
}

// Throw a RuntimeException describing an spk_status failure (e.g.
// "spektra: profile not found"). No-op for SPK_OK.
void throw_status(JNIEnv* env, spk_status st) noexcept {
    if (st == SPK_OK) return;
    char msg[768];
    const int prefix = std::snprintf(msg, sizeof(msg), "spektra: %s",
                                      spk_status_str(st));
    if (prefix < 0) return;
    size_t used = static_cast<size_t>(prefix);
    if (used >= sizeof(msg)) used = sizeof(msg) - 1;
    const char* detail = spk_last_error_message();
    if (detail && detail[0] != '\0') {
        const int n = std::snprintf(msg + used, sizeof(msg) - used,
                                    ": %s", detail);
        if (n > 0) used += static_cast<size_t>(n);
        if (used >= sizeof(msg)) msg[sizeof(msg) - 1] = '\0';
    }
    if (st == SPK_ERR_CANCELLED && !env->ExceptionCheck()) {
        jclass cls = env->FindClass("java/util/concurrent/CancellationException");
        if (cls) {
            env->ThrowNew(cls, msg);
            env->DeleteLocalRef(cls);
            return;
        }
        env->ExceptionClear();
    }
    throw_runtime(env, msg);
}

// A C++ exception must NEVER unwind through the extern "C" JNI boundary — that
// is std::terminate/SIGABRT, i.e. a hard native crash instead of a catchable
// Java error. Every allocating entry point below is a function-try-block that
// funnels here: std::bad_alloc (any of the engine's full-resolution
// std::vector allocations failing on a low-memory device) becomes
// java.lang.OutOfMemoryError — catchable, matching the existing output-buffer
// OOM path — and anything else becomes a RuntimeException.
void throw_native_oom(JNIEnv* env) noexcept {
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

void throw_cpp_exception(JNIEnv* env, const std::exception& e) noexcept {
    char msg[768];
    const char* what = e.what();
    std::snprintf(msg, sizeof(msg), "spektra: native error: %s",
                  what ? what : "unknown std::exception");
    throw_runtime(env, msg);
}

void throw_unknown_cpp_exception(JNIEnv* env) noexcept {
    throw_runtime(env, "spektra: unknown native exception");
}

// Resolve the caller-visible range of a direct ByteBuffer. JNI's raw address and
// capacity alone do not enforce Buffer.position()/limit(), so accepting only those
// would let native code read outside a sliced/logically bounded input.
bool direct_float_input(JNIEnv* env, jobject buffer, jint width, jint height,
                        float** out) {
    if (!buffer || !out) return false;
    *out = nullptr;

    std::uint64_t required = 0;
    if (!spk::jni::checked_rgb_f32_bytes(width, height, &required) ||
        required > static_cast<std::uint64_t>(INT32_MAX)) {
        throw_runtime(env, "spektra: invalid or oversized image dimensions");
        return false;
    }
    void* base = env->GetDirectBufferAddress(buffer);
    const jlong capacity = env->GetDirectBufferCapacity(buffer);
    if (!base || capacity < 0) {
        throw_runtime(env, "spektra: input ByteBuffer is not direct");
        return false;
    }

    jclass cls = env->GetObjectClass(buffer);
    if (!cls) return false;
    jmethodID position_method = env->GetMethodID(cls, "position", "()I");
    jmethodID limit_method = env->GetMethodID(cls, "limit", "()I");
    env->DeleteLocalRef(cls);
    if (!position_method || !limit_method) {
        env->ExceptionClear();
        throw_runtime(env, "spektra: cannot inspect input ByteBuffer range");
        return false;
    }
    const jint position = env->CallIntMethod(buffer, position_method);
    const jint limit = env->CallIntMethod(buffer, limit_method);
    if (env->ExceptionCheck()) return false;
    std::uintptr_t address = 0;
    if (!spk::jni::checked_float_buffer_range(
            static_cast<std::int64_t>(capacity), position, limit, required,
            reinterpret_cast<std::uintptr_t>(base), &address)) {
        throw_runtime(env,
            "spektra: input ByteBuffer range is invalid, too small, or unaligned");
        return false;
    }
    *out = reinterpret_cast<float*>(address);
    return true;
}

class JniCancellation {
public:
    JniCancellation(JNIEnv* env, jobject token) : env_(env), token_(token) {}

    bool initialize() {
        if (!token_) return true;
        jclass cls = env_->GetObjectClass(token_);
        if (!cls) return false;
        method_ = env_->GetMethodID(cls, "isCancellationRequested", "()Z");
        env_->DeleteLocalRef(cls);
        if (!method_) {
            env_->ExceptionClear();
            throw_runtime(env_, "spektra: invalid cancellation token");
            return false;
        }
        return true;
    }

    spk_cancel_check callback() const noexcept {
        return token_ ? &JniCancellation::check : nullptr;
    }

    void* context() noexcept { return this; }

private:
    static int check(void* opaque) noexcept {
        auto* self = static_cast<JniCancellation*>(opaque);
        if (!self || !self->token_ || !self->method_) return 0;
        if (self->env_->ExceptionCheck()) return 1;
        const jboolean requested = self->env_->CallBooleanMethod(
            self->token_, self->method_);
        return self->env_->ExceptionCheck() || requested == JNI_TRUE ? 1 : 0;
    }

    JNIEnv* env_;
    jobject token_;
    jmethodID method_ = nullptr;
};

bool timing_automation_enabled() {
#ifdef __ANDROID__
    if (ATrace_isEnabled()) return true;
    char on[PROP_VALUE_MAX] = {0};
    return __system_property_get("debug.spektra.timingjson", on) > 0 &&
           on[0] != '0' && on[0] != '\0';
#else
    return false;
#endif
}

bool render_kind_from_jni(jint code, jboolean preview,
                          spk::RenderTimingKind* out) {
    if (!out) return false;
    switch (code) {
        case 1:
            if (preview) return false;
            *out = spk::RTK_EXPORT;
            return true;
        case 2:
            if (!preview) return false;
            *out = spk::RTK_PREVIEW;
            return true;
        case 3:
            if (preview) return false;
            *out = spk::RTK_MAGNIFIER;
            return true;
        case 4:
            if (!preview) return false;
            *out = spk::RTK_ROI;
            return true;
        case 5:
            if (preview) return false;
            *out = spk::RTK_EXACT_RENDER;
            return true;
        default:
            return false;
    }
}

// Must run only after ScopedRenderTiming publishes. The guard below is declared
// first, then the timing scope, so reverse destruction order makes that true on
// success, every early return, and C++ exception unwinding.
void log_completed_stage_timing(uint64_t expected_render_id) noexcept {
#ifdef __ANDROID__
    if (expected_render_id == 0 ||
        spk_stage_timing_render_id() != expected_render_id) {
        return;
    }
    const spk::StageTimingSnapshot& snapshot = spk::stage_timing_snapshot();
    char timings[512];
    if (spk_stage_timings(timings, sizeof(timings)) > 0) {
        const bool diffusion_ran =
            std::strstr(timings, "camera_diffusion") != nullptr;
        char fallback[48] = {0};
        if (diffusion_ran) {
            std::snprintf(fallback, sizeof(fallback), " fft_fallbacks=%llu",
                          snapshot.fft_fallbacks);
        }
        __android_log_print(ANDROID_LOG_INFO, "Spektra",
                            "stage timings ms [%s id=%llu]: %s%s",
                            spk::render_timing_kind_name(snapshot.kind),
                            static_cast<unsigned long long>(snapshot.render_id),
                            timings, fallback);
    }
    if (timing_automation_enabled()) {
        char json[2048];
        if (spk_stage_timings_json(json, sizeof(json)) > 0) {
            __android_log_print(ANDROID_LOG_INFO, "Spektra",
                                "stage timings json: %s", json);
        }
    }
#else
    (void)expected_render_id;
#endif
}

class StageTimingLogGuard {
public:
    StageTimingLogGuard() = default;
    void expect(uint64_t render_id) { expected_render_id_ = render_id; }
    ~StageTimingLogGuard() { log_completed_stage_timing(expected_render_id_); }

    StageTimingLogGuard(const StageTimingLogGuard&) = delete;
    StageTimingLogGuard& operator=(const StageTimingLogGuard&) = delete;

private:
    uint64_t expected_render_id_ = 0;
};

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
    if (!cls) return nullptr;
    jmethodID m = env->GetMethodID(cls, getter, sig);
    env->DeleteLocalRef(cls);
    if (!m) { env->ExceptionClear(); return nullptr; }
    return env->CallObjectMethod(obj, m);
}

bool read_float(JNIEnv* env, jobject obj, const char* getter, float* out) {
    if (!obj || !out) return false;
    jclass cls = env->GetObjectClass(obj);
    if (!cls) return false;
    jmethodID m = env->GetMethodID(cls, getter, "()F");
    env->DeleteLocalRef(cls);
    if (!m) { env->ExceptionClear(); return false; }
    const jfloat value = env->CallFloatMethod(obj, m);
    if (env->ExceptionCheck()) return false;
    *out = value;
    return true;
}

bool read_bool_i32(JNIEnv* env, jobject obj, const char* getter, int32_t* out) {
    if (!obj || !out) return false;
    jclass cls = env->GetObjectClass(obj);
    if (!cls) return false;
    jmethodID m = env->GetMethodID(cls, getter, "()Z");
    env->DeleteLocalRef(cls);
    if (!m) { env->ExceptionClear(); return false; }
    const jboolean value = env->CallBooleanMethod(obj, m);
    if (env->ExceptionCheck()) return false;
    *out = value == JNI_TRUE ? 1 : 0;
    return true;
}

bool read_int(JNIEnv* env, jobject obj, const char* getter, int32_t* out) {
    if (!obj || !out) return false;
    jclass cls = env->GetObjectClass(obj);
    if (!cls) return false;
    jmethodID m = env->GetMethodID(cls, getter, "()I");
    env->DeleteLocalRef(cls);
    if (!m) { env->ExceptionClear(); return false; }
    const jint value = env->CallIntMethod(obj, m);
    if (env->ExceptionCheck()) return false;
    *out = value;
    return true;
}

// Unbox a java.lang.Float (or Number) into a float.
float unbox_float(JNIEnv* env, jobject boxed) {
    if (!boxed) return 0.0f;
    jclass cls = env->GetObjectClass(boxed);
    if (!cls) return 0.0f;
    jmethodID m = env->GetMethodID(cls, "floatValue", "()F");
    env->DeleteLocalRef(cls);
    if (!m) { env->ExceptionClear(); return 0.0f; }
    return env->CallFloatMethod(boxed, m);
}

// Read one component of a kotlin.Triple/Pair.
//
// THE BUG THIS EXISTS FOR. R8 REMOVES kotlin.Triple.getFirst/getSecond/getThird
// and kotlin.Pair.getFirst/getSecond from the release dex. proguard-rules.pro
// keeps com.spectrafilm.engine.** so GrainParams.getAgxParticleScale() survives
// and returns a real Triple -- but `kotlin.**` was not kept, and NO BYTECODE
// anywhere calls Triple.getFirst. The only caller is this file, by literal
// string, which R8 cannot see. So it shrank them as unreachable.
//
// `-dontobfuscate` does not save you: it prevents RENAMING, not REMOVAL. That
// distinction shipped 19 wrong parameters in every release APK -- every
// Triple/Pair-valued param (grain particle scale, density min, uniformity,
// halation scatter/strength, all four coupler gammas, camera UV/IR filters,
// scanner unsharp, AND crop centre/size) marshalled as 0.0.
//
// Two independent defects made that silent, and both are fixed here:
//   1. the getter vanished  -> fall back to the BACKING FIELD, which R8 keeps
//      (dexdump shows `fields: first, second, third` intact). JNI GetFieldID
//      reaches private fields, so this works without any keep rule.
//   2. failure OVERWROTE the caller's value with 0.0 -- unbox_float(nullptr)
//      returns 0.0f, so a failed read did not leave spk_default_params' seed in
//      place, it destroyed it. Now a component that cannot be read leaves `out`
//      alone and reports, so the default survives and the failure is visible.
jobject tuple_component(JNIEnv* env, jobject t, const char* getter,
                        const char* field) {
    jobject v = call_obj(env, t, getter, "()Ljava/lang/Object;");
    if (v) return v;
    if (env->ExceptionCheck()) return nullptr;
    jclass cls = env->GetObjectClass(t);
    if (!cls) return nullptr;
    jfieldID f = env->GetFieldID(cls, field, "Ljava/lang/Object;");
    env->DeleteLocalRef(cls);
    if (!f) { env->ExceptionClear(); return nullptr; }
    return env->GetObjectField(t, f);
}

// One-time complaint that a tuple could not be read. Rate-limited to once per
// process: this runs per param per render, and a silent wrong number is exactly
// what went unnoticed before -- but a log line per render per param would be its
// own kind of unreadable.
void warn_tuple_unreadable_once(const char* getter) {
#if defined(__ANDROID__)
    static std::atomic<bool> warned{false};
    bool expected = false;
    if (!warned.compare_exchange_strong(expected, true)) return;
    __android_log_print(ANDROID_LOG_ERROR, "Spektra",
        "PARAM MARSHALLING BROKEN: could not read the components of '%s' by "
        "getter OR by field. Every Triple/Pair-valued param is therefore at its "
        "built-in default, not the value you set. This is what R8 shrinking "
        "kotlin.Triple/kotlin.Pair looks like -- check the keep rules.", getter);
#else
    (void)getter;
#endif
}

// Read a kotlin.Triple<Float,Float,Float> into out[3]. Leaves `out` UNTOUCHED if
// the tuple or any component cannot be read, so the caller's default survives.
void read_triple_f(JNIEnv* env, jobject obj, const char* getter, float out[3]) {
    jobject t = call_obj(env, obj, getter, "()Lkotlin/Triple;");
    if (!t) return;
    jobject a = tuple_component(env, t, "getFirst", "first");
    jobject b = tuple_component(env, t, "getSecond", "second");
    jobject c = tuple_component(env, t, "getThird", "third");
    if (a && b && c) {
        out[0] = unbox_float(env, a);
        out[1] = unbox_float(env, b);
        out[2] = unbox_float(env, c);
    } else {
        warn_tuple_unreadable_once(getter);   // out keeps its default
    }
    if (a) env->DeleteLocalRef(a);
    if (b) env->DeleteLocalRef(b);
    if (c) env->DeleteLocalRef(c);
    env->DeleteLocalRef(t);
}

// Read a kotlin.Pair<Float,Float> into out[2]. Same contract as read_triple_f.
void read_pair_f(JNIEnv* env, jobject obj, const char* getter, float out[2]) {
    jobject t = call_obj(env, obj, getter, "()Lkotlin/Pair;");
    if (!t) return;
    jobject a = tuple_component(env, t, "getFirst", "first");
    jobject b = tuple_component(env, t, "getSecond", "second");
    if (a && b) {
        out[0] = unbox_float(env, a);
        out[1] = unbox_float(env, b);
    } else {
        warn_tuple_unreadable_once(getter);   // out keeps its default
    }
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
    std::string enlarger_illuminant;
    std::string input_color_space;
};

bool read_diffusion_family(JNIEnv* env, jobject df, int32_t* out) {
    if (!df || !out) return true;
    jobject value = call_obj(env, df, "getFilterFamily", "()Ljava/lang/String;");
    if (!value) return !env->ExceptionCheck();
    std::string family = jstr(env, static_cast<jstring>(value));
    env->DeleteLocalRef(value);
    if (family.empty()) return true;
    if (family == "glimmerglass") {
        *out = SPK_DIFFUSION_GLIMMERGLASS;
    } else if (family == "black_pro_mist") {
        *out = SPK_DIFFUSION_BLACK_PRO_MIST;
    } else if (family == "pro_mist") {
        *out = SPK_DIFFUSION_PRO_MIST;
    } else if (family == "cinebloom") {
        *out = SPK_DIFFUSION_CINEBLOOM;
    } else {
        throw_runtime(env, "spektra: unsupported diffusion filter family");
        return false;
    }
    return true;
}

// Read a DiffusionFilterParams jobject into the spk_params diffusion-filter
// fields (camera/enlarger share the same struct). `set` writes through a small
// lambda so the same reader serves both prefixes.
bool read_diffusion_filter(JNIEnv* env, jobject df, int32_t* family,
                           int32_t* active, float* strength, float* spatial_scale,
                           float* halo_warmth, float* core_intensity,
                           float* core_size, float* halo_intensity, float* halo_size,
                           float* bloom_intensity, float* bloom_size) {
    if (!df) return true;
    if (!read_diffusion_family(env, df, family)) return false;
    read_bool_i32(env, df, "getActive", active);
    read_float(env, df, "getStrength", strength);
    read_float(env, df, "getSpatialScale", spatial_scale);
    read_float(env, df, "getHaloWarmth", halo_warmth);
    read_float(env, df, "getCoreIntensity", core_intensity);
    read_float(env, df, "getCoreSize", core_size);
    read_float(env, df, "getHaloIntensity", halo_intensity);
    read_float(env, df, "getHaloSize", halo_size);
    read_float(env, df, "getBloomIntensity", bloom_intensity);
    read_float(env, df, "getBloomSize", bloom_size);
    return !env->ExceptionCheck();
}

// Read the packed tone-curve float[] from SpektraParams.toneCurvePacked() into the
// flat spk_params fields. Layout: [active, mN, (x,y)*mN, rN, (x,y)*rN, gN, ..., bN, ...].
// Per-channel counts are clamped to SPK_TONE_MAX_PTS. Absent/short array leaves the
// tone curve OFF (the spk_default_params value), so this is a strict no-op by default.
void read_tone_curve(JNIEnv* env, jobject params, spk_params* out) {
    jobject arrObj = call_obj(env, params, "toneCurvePacked", "()[F");
    if (!arrObj) return;
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

// One-shot-per-render dump of the marshalled spk_params, gated on a system
// property so it is inert in a normal release build. See the call site at the end
// of marshal_params for why this exists.
void dump_marshalled_params(const spk_params* p) {
#if defined(__ANDROID__)
    char on[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.spektra.dumpparams", on) <= 0 || on[0] == '0')
        return;
    __android_log_print(ANDROID_LOG_INFO, "Spektra",
        "params route: scan_film=%d grain_active=%d halation_active=%d "
        "dir_couplers_active=%d glare_active=%d auto_exposure=%d ev_comp=%.4f",
        p->scan_film, p->grain_active, p->halation_active, p->dir_couplers_active,
        p->glare_active, p->auto_exposure, (double)p->exposure_compensation_ev);
    __android_log_print(ANDROID_LOG_INFO, "Spektra",
        "params grain: sublayers=%d n_sub=%d area_um2=%.5f blur=%.5f "
        "density_min=[%.5f %.5f %.5f] uniformity=[%.5f %.5f %.5f]",
        p->grain_sublayers_active, p->grain_n_sub_layers,
        (double)p->grain_particle_area_um2, (double)p->grain_blur,
        (double)p->grain_density_min[0], (double)p->grain_density_min[1],
        (double)p->grain_density_min[2],
        (double)p->grain_uniformity[0], (double)p->grain_uniformity[1],
        (double)p->grain_uniformity[2]);
    __android_log_print(ANDROID_LOG_INFO, "Spektra",
        "params grain2: particle_scale=[%.5f %.5f %.5f] scale_layers=[%.5f %.5f %.5f] "
        "micro=[%.5f %.5f] blur_dye_um=%.5f",
        (double)p->grain_particle_scale[0], (double)p->grain_particle_scale[1],
        (double)p->grain_particle_scale[2],
        (double)p->grain_particle_scale_layers[0],
        (double)p->grain_particle_scale_layers[1],
        (double)p->grain_particle_scale_layers[2],
        (double)p->grain_micro_structure[0], (double)p->grain_micro_structure[1],
        (double)p->grain_blur_dye_clouds_um);
    __android_log_print(ANDROID_LOG_INFO, "Spektra",
        "params scanner: white_corr=%d black_corr=%d white_level=%.5f "
        "black_level=%.5f use_scanner_lut=%d use_enlarger_lut=%d lut_res=%d",
        p->scanner_white_correction, p->scanner_black_correction,
        (double)p->scanner_white_level, (double)p->scanner_black_level,
        p->use_scanner_lut, p->use_enlarger_lut, p->lut_resolution);
    __android_log_print(ANDROID_LOG_INFO, "Spektra",
        "params misc: film=%s print=%s out_cs=%d cctf=%d gamma=%.5f "
        "preview_max=%d gpu_preview=%d gpu_export=%d",
        p->film_profile ? p->film_profile : "(null)",
        p->print_profile ? p->print_profile : "(null)",
        (int)p->output_color_space, p->output_cctf_encoding,
        (double)p->density_curve_gamma, p->preview_max_size,
        p->gpu_preview, p->gpu_export);
#else
    (void)p;
#endif
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
        read_float(env, camera, "getExposureCompensationEv",
                   &out->exposure_compensation_ev);
        read_bool_i32(env, camera, "getAutoExposure", &out->auto_exposure);
        // Forward the metering method string (e.g. "center_weighted"). The owned
        // copy in `store` keeps the pointer valid for the duration of the call;
        // empty/null leaves the spk_default_params value (engine -> center_weighted).
        store->auto_exposure_method = jstr(env,
            static_cast<jstring>(call_obj(env, camera, "getAutoExposureMethod",
                                          "()Ljava/lang/String;")));
        out->auto_exposure_method = store->auto_exposure_method.empty()
            ? nullptr : store->auto_exposure_method.c_str();
        read_float(env, camera, "getLensBlurUm", &out->lens_blur_um);
        read_float(env, camera, "getFilmFormatMm", &out->film_format_mm);
        read_triple_f(env, camera, "getFilterUv", out->camera_filter_uv);
        read_triple_f(env, camera, "getFilterIr", out->camera_filter_ir);
        jobject df = call_obj(env, camera, "getDiffusionFilter",
            "()Lcom/spectrafilm/engine/DiffusionFilterParams;");
        if (!read_diffusion_filter(env, df, &out->camera_diffusion_family,
            &out->camera_diffusion_active,
            &out->camera_diffusion_strength, &out->camera_diffusion_spatial_scale,
            &out->camera_diffusion_halo_warmth, &out->camera_diffusion_core_intensity,
            &out->camera_diffusion_core_size, &out->camera_diffusion_halo_intensity,
            &out->camera_diffusion_halo_size, &out->camera_diffusion_bloom_intensity,
            &out->camera_diffusion_bloom_size)) return false;
        if (df) env->DeleteLocalRef(df);
    }

    // ---- enlarger ----
    if (enlarger) {
        jobject illum = call_obj(env, enlarger, "getIlluminant",
            "()Ljava/lang/String;");
        if (illum) {
            store->enlarger_illuminant = jstr(env, static_cast<jstring>(illum));
            env->DeleteLocalRef(illum);
            if (!store->enlarger_illuminant.empty()) {
                out->enlarger_illuminant = store->enlarger_illuminant.c_str();
            }
        }
        read_float(env, enlarger, "getPrintExposure", &out->print_exposure);
        read_bool_i32(env, enlarger, "getPrintExposureCompensation",
                      &out->print_exposure_compensation);
        read_bool_i32(env, enlarger, "getNormalizePrintExposure",
                      &out->normalize_print_exposure);
        read_float(env, enlarger, "getYFilterShift", &out->y_filter_shift);
        read_float(env, enlarger, "getMFilterShift", &out->m_filter_shift);
        read_float(env, enlarger, "getYFilterNeutral", &out->y_filter_neutral);
        read_float(env, enlarger, "getMFilterNeutral", &out->m_filter_neutral);
        read_float(env, enlarger, "getCFilterNeutral", &out->c_filter_neutral);
        read_float(env, enlarger, "getLensBlur", &out->enlarger_lens_blur);
        read_float(env, enlarger, "getPreflashExposure", &out->preflash_exposure);
        read_float(env, enlarger, "getPreflashYFilterShift",
                   &out->preflash_y_filter_shift);
        read_float(env, enlarger, "getPreflashMFilterShift",
                   &out->preflash_m_filter_shift);
        jobject df = call_obj(env, enlarger, "getDiffusionFilter",
            "()Lcom/spectrafilm/engine/DiffusionFilterParams;");
        if (!read_diffusion_filter(env, df, &out->enlarger_diffusion_family,
            &out->enlarger_diffusion_active,
            &out->enlarger_diffusion_strength, &out->enlarger_diffusion_spatial_scale,
            &out->enlarger_diffusion_halo_warmth, &out->enlarger_diffusion_core_intensity,
            &out->enlarger_diffusion_core_size, &out->enlarger_diffusion_halo_intensity,
            &out->enlarger_diffusion_halo_size, &out->enlarger_diffusion_bloom_intensity,
            &out->enlarger_diffusion_bloom_size)) return false;
        if (df) env->DeleteLocalRef(df);
    }

    // ---- scanner ----
    if (scanner) {
        read_float(env, scanner, "getLensBlur", &out->scanner_lens_blur);
        read_bool_i32(env, scanner, "getWhiteCorrection",
                      &out->scanner_white_correction);
        read_bool_i32(env, scanner, "getBlackCorrection",
                      &out->scanner_black_correction);
        read_float(env, scanner, "getWhiteLevel", &out->scanner_white_level);
        read_float(env, scanner, "getBlackLevel", &out->scanner_black_level);
        read_pair_f(env, scanner, "getUnsharpMask", out->scanner_unsharp);
    }

    // ---- film rendering ----
    if (filmR) {
        read_float(env, filmR, "getDensityCurveGamma", &out->density_curve_gamma);
        jobject grain = call_obj(env, filmR, "getGrain",
            "()Lcom/spectrafilm/engine/GrainParams;");
        jobject halation = call_obj(env, filmR, "getHalation",
            "()Lcom/spectrafilm/engine/HalationParams;");
        jobject dir = call_obj(env, filmR, "getDirCouplers",
            "()Lcom/spectrafilm/engine/DirCouplersParams;");
        jobject glare = call_obj(env, filmR, "getGlare",
            "()Lcom/spectrafilm/engine/GlareParams;");

        if (grain) {
            read_bool_i32(env, grain, "getActive", &out->grain_active);
            read_bool_i32(env, grain, "getSublayersActive",
                          &out->grain_sublayers_active);
            read_float(env, grain, "getAgxParticleAreaUm2",
                       &out->grain_particle_area_um2);
            read_triple_f(env, grain, "getAgxParticleScale", out->grain_particle_scale);
            read_triple_f(env, grain, "getAgxParticleScaleLayers", out->grain_particle_scale_layers);
            read_triple_f(env, grain, "getDensityMin", out->grain_density_min);
            read_triple_f(env, grain, "getUniformity", out->grain_uniformity);
            read_float(env, grain, "getBlur", &out->grain_blur);
            read_float(env, grain, "getBlurDyeCloudsUm",
                       &out->grain_blur_dye_clouds_um);
            read_pair_f(env, grain, "getMicroStructure", out->grain_micro_structure);
            read_int(env, grain, "getNSubLayers", &out->grain_n_sub_layers);
            env->DeleteLocalRef(grain);
        }
        if (halation) {
            read_bool_i32(env, halation, "getActive", &out->halation_active);
            read_float(env, halation, "getScatterAmount",
                       &out->halation_scatter_amount);
            read_float(env, halation, "getScatterSpatialScale",
                       &out->halation_scatter_spatial_scale);
            read_float(env, halation, "getHalationAmount",
                       &out->halation_halation_amount);
            read_float(env, halation, "getHalationSpatialScale",
                       &out->halation_halation_spatial_scale);
            read_triple_f(env, halation, "getScatterCoreUm", out->halation_scatter_core_um);
            read_triple_f(env, halation, "getScatterTailUm", out->halation_scatter_tail_um);
            read_triple_f(env, halation, "getScatterTailWeight", out->halation_scatter_tail_weight);
            read_float(env, halation, "getBoostEv", &out->halation_boost_ev);
            read_float(env, halation, "getBoostRange", &out->halation_boost_range);
            read_float(env, halation, "getProtectEv", &out->halation_protect_ev);
            read_triple_f(env, halation, "getHalationStrength", out->halation_strength);
            read_triple_f(env, halation, "getHalationFirstSigmaUm", out->halation_first_sigma_um);
            read_int(env, halation, "getHalationNBounces",
                     &out->halation_n_bounces);
            read_float(env, halation, "getHalationBounceDecay",
                       &out->halation_bounce_decay);
            read_bool_i32(env, halation, "getHalationRenormalize",
                          &out->halation_renormalize);
            env->DeleteLocalRef(halation);
        }
        if (dir) {
            read_bool_i32(env, dir, "getActive", &out->dir_couplers_active);
            read_float(env, dir, "getAmount", &out->dir_amount);
            read_float(env, dir, "getInhibitionSamelayer",
                       &out->dir_inhibition_samelayer);
            read_float(env, dir, "getInhibitionInterlayer",
                       &out->dir_inhibition_interlayer);
            read_triple_f(env, dir, "getGammaSamelayerRgb", out->dir_gamma_samelayer_rgb);
            read_pair_f(env, dir, "getGammaInterlayerRToGb", out->dir_gamma_interlayer_r_to_gb);
            read_pair_f(env, dir, "getGammaInterlayerGToRb", out->dir_gamma_interlayer_g_to_rb);
            read_pair_f(env, dir, "getGammaInterlayerBToRg", out->dir_gamma_interlayer_b_to_rg);
            read_float(env, dir, "getDiffusionSizeUm", &out->dir_diffusion_size_um);
            read_float(env, dir, "getDiffusionTailUm", &out->dir_diffusion_tail_um);
            read_float(env, dir, "getDiffusionTailWeight",
                       &out->dir_diffusion_tail_weight);
            env->DeleteLocalRef(dir);
        }
        if (glare) {
            read_bool_i32(env, glare, "getActive", &out->glare_active);
            read_float(env, glare, "getPercent", &out->glare_percent);
            read_float(env, glare, "getRoughness", &out->glare_roughness);
            read_float(env, glare, "getBlur", &out->glare_blur);
            env->DeleteLocalRef(glare);
        }
    }

    // ---- print rendering ----
    if (printR) {
        read_float(env, printR, "getDensityCurveGamma",
                   &out->print_density_curve_gamma);
        jobject pglare = call_obj(env, printR, "getGlare",
            "()Lcom/spectrafilm/engine/GlareParams;");
        if (pglare) {
            read_bool_i32(env, pglare, "getActive", &out->print_glare_active);
            read_float(env, pglare, "getPercent", &out->print_glare_percent);
            read_float(env, pglare, "getRoughness", &out->print_glare_roughness);
            read_float(env, pglare, "getBlur", &out->print_glare_blur);
            env->DeleteLocalRef(pglare);
        }
        // OPT-IN s023 print density-curve morph. Absent / active=false -> the
        // engine defaults (off, identity) set in spk_default_params remain.
        jobject pmorph = call_obj(env, printR, "getDensityCurvesMorph",
            "()Lcom/spectrafilm/engine/PrintCurvesMorphParams;");
        if (pmorph) {
            read_bool_i32(env, pmorph, "getActive", &out->print_morph_active);
            read_float(env, pmorph, "getGammaFactor", &out->print_morph_gamma_factor);
            read_float(env, pmorph, "getGammaFactorFast",
                       &out->print_morph_gamma_factor_fast);
            read_float(env, pmorph, "getGammaFactorSlow",
                       &out->print_morph_gamma_factor_slow);
            read_float(env, pmorph, "getGammaFactorRed",
                       &out->print_morph_gamma_factor_red);
            read_float(env, pmorph, "getGammaFactorGreen",
                       &out->print_morph_gamma_factor_green);
            read_float(env, pmorph, "getGammaFactorBlue",
                       &out->print_morph_gamma_factor_blue);
            read_float(env, pmorph, "getDeveloperExhaustion",
                       &out->print_morph_developer_exhaustion);
            env->DeleteLocalRef(pmorph);
        }
    }

    // ---- io ----
    if (io) {
        jobject ics = call_obj(env, io, "getInputColorSpace",
            "()Ljava/lang/String;");
        if (ics) {
            store->input_color_space = jstr(env, static_cast<jstring>(ics));
            env->DeleteLocalRef(ics);
            if (!store->input_color_space.empty()) {
                out->input_color_space = store->input_color_space.c_str();
            }
        }
        read_bool_i32(env, io, "getScanFilm", &out->scan_film);
        read_bool_i32(env, io, "getOutputCctfEncoding",
                      &out->output_cctf_encoding);
        read_bool_i32(env, io, "getInputCctfDecoding",
                      &out->input_cctf_decoding);
        read_bool_i32(env, io, "getCrop", &out->crop);
        read_float(env, io, "getUpscaleFactor", &out->upscale_factor);
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
        read_bool_i32(env, settings, "getApplyHanatos2025AdaptationWindow",
                      &out->apply_hanatos_window);
        read_bool_i32(env, settings, "getApplyHanatos2025AdaptationSurface",
                      &out->apply_hanatos_surface);
        read_float(env, settings, "getSpectralGaussianBlur",
                   &out->spectral_gaussian_blur);
        read_bool_i32(env, settings, "getUseEnlargerLut", &out->use_enlarger_lut);
        read_bool_i32(env, settings, "getUseScannerLut", &out->use_scanner_lut);
        // GPU preview fast-path toggle (GPU M1, #146; default false). Consulted
        // only by spk_simulate_preview — export renders ignore it by design.
        read_bool_i32(env, settings, "getGpuPreview", &out->gpu_preview);
        // Experimental GPU export toggle (#154; default false). Consulted only by
        // spk_simulate (export); preview clears it, tap/bake hard-zero the latch.
        read_bool_i32(env, settings, "getGpuExport", &out->gpu_export);
        read_int(env, settings, "getLutResolution", &out->lut_resolution);
        read_int(env, settings, "getPreviewMaxSize", &out->preview_max_size);
        read_bool_i32(env, settings, "getNeutralPrintFiltersFromDatabase",
                      &out->neutral_print_filters_from_database);
    }

    // Tone curve (top-level packed float[]); OFF by default => no-op.
    read_tone_curve(env, params, out);

    // DIAGNOSTIC: dump what actually crossed the boundary.
    //
    // Exists because a whole route was reported rendering a flat constant and the
    // params were read off the UI, which is not the same thing -- #143 is an entire
    // batch of "params that lie", i.e. controls whose displayed value and marshalled
    // value disagree. Reading a slider is evidence about the slider. This is evidence
    // about the engine's input, which is what a repro needs.
    //
    // Off unless the system property is set, so it costs a getprop per render and
    // nothing else:  adb shell setprop debug.spektra.dumpparams 1
    // Deliberately covers the values a repro has to match: route, the stochastic and
    // spatial gates, grain (whose density_min feeds the GRAIN MODEL at
    // spektra.cpp:763, not only the opt-in LUT domain), the scanner corrections and
    // their levels, and the profiles.
    dump_marshalled_params(out);

    if (camera) env->DeleteLocalRef(camera);
    if (enlarger) env->DeleteLocalRef(enlarger);
    if (scanner) env->DeleteLocalRef(scanner);
    if (filmR) env->DeleteLocalRef(filmR);
    if (printR) env->DeleteLocalRef(printR);
    if (io) env->DeleteLocalRef(io);
    if (settings) env->DeleteLocalRef(settings);
    return !env->ExceptionCheck();
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
} catch (...) {
    throw_unknown_cpp_exception(env);
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
} catch (...) {
    throw_unknown_cpp_exception(env);
    return 0;
}

JNI(void, nativeDestroy)(JNIEnv* env, jobject /*thiz*/, jlong handle) try {
    spk_engine_destroy(reinterpret_cast<spk_engine*>(handle));
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
} catch (...) {
    throw_unknown_cpp_exception(env);
}

JNI(jstring, nativeListProfiles)(JNIEnv* env, jobject /*thiz*/, jlong handle) try {
    spk_engine* eng = reinterpret_cast<spk_engine*>(handle);
    if (!eng) {
        throw_runtime(env, "spektra: engine handle is null");
        return nullptr;
    }
    size_t needed = 0;
    spk_status st = spk_engine_list_profiles(eng, nullptr, 0, &needed);
    if (st != SPK_OK) {
        throw_status(env, st);
        return nullptr;
    }
    if (needed == 0) {
        throw_runtime(env, "spektra: profile list returned an invalid size");
        return nullptr;
    }
    std::vector<char> buf(needed);
    st = spk_engine_list_profiles(eng, buf.data(), buf.size(), &needed);
    if (st != SPK_OK) {
        throw_status(env, st);
        return nullptr;
    }
    return env->NewStringUTF(buf.data());
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
    return nullptr;
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
    return nullptr;
} catch (...) {
    throw_unknown_cpp_exception(env);
    return nullptr;
}

/*
 * nativeSimulate(handle, inBuf, w, h, inCs, paramsObj, preview, renderKind,
 *                cancellation)
 *     -> SimResult(data, width, height, colorSpace, renderId).
 * Reads a direct float ByteBuffer of w*h*3 floats, marshals the params, runs
 * spk_simulate(_preview), and wraps the result into a SimResult with a fresh
 * direct float ByteBuffer.
 */
JNI(jobject, nativeSimulate)(JNIEnv* env, jobject /*thiz*/, jlong handle,
                             jobject inBuf, jint w, jint h, jstring inCs,
                             jobject paramsObj, jboolean preview,
                             jint renderKind, jobject cancellation) try {
    StageTimingLogGuard timing_log;
    spk::RenderTimingKind logical_kind = spk::RTK_UNKNOWN;
    const bool valid_kind = render_kind_from_jni(renderKind, preview, &logical_kind);
    spk::ScopedRenderTiming timing(valid_kind ? logical_kind : spk::RTK_UNKNOWN);
    const uint64_t render_id = timing.render_id();
    timing_log.expect(render_id);
    spk_diffusion_reset_fft_fallbacks();
    if (!valid_kind) {
        timing.finish(SPK_ERR_BAD_ARGS);
        throw_runtime(env, "spektra: invalid render kind for preview/exact route");
        return nullptr;
    }

    spk_engine* eng = reinterpret_cast<spk_engine*>(handle);
    if (!eng) { throw_runtime(env, "spektra: engine handle is null"); return nullptr; }
    if (!inBuf) { throw_runtime(env, "spektra: input ByteBuffer is null"); return nullptr; }
    if (w <= 0 || h <= 0) {
        throw_runtime(env, "spektra: invalid image dimensions");
        return nullptr;
    }

    float* in_data = nullptr;
    if (!direct_float_input(env, inBuf, w, h, &in_data)) return nullptr;

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
    JniCancellation cancellation_probe(env, cancellation);
    if (!cancellation_probe.initialize()) return nullptr;

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
    spk_status st = preview
        ? spk_simulate_preview_cancellable(
              eng, &in_img, &params, &out, cancellation_probe.callback(),
              cancellation_probe.context())
        : spk_simulate_cancellable(
              eng, &in_img, &params, &out, cancellation_probe.callback(),
              cancellation_probe.context());
#ifdef __ANDROID__
    // One-time diagnostic when the GPU preview self-check failed (state 2): the
    // preview silently stays on the CPU path for this process (#146 mandate —
    // fall back, but never silently for the log).
    if (preview) {
        // One-time logs so the toggle is externally verifiable from logcat
        // (#146 on-device validation found silence was ambiguous: only the
        // failure case logged, so "passed" and "never ran" looked identical).
        static std::atomic<bool> gpu_state_logged{false};
        const int gpu_state = spk_gpu_scan_state();
        if (gpu_state != 0 &&
            !gpu_state_logged.exchange(true, std::memory_order_relaxed)) {
            if (gpu_state == 1) {
                __android_log_print(ANDROID_LOG_INFO, "Spektra",
                                    "gpu preview self-check PASSED on this device/driver");
            } else {
                __android_log_print(ANDROID_LOG_WARN, "Spektra",
                                    "gpu preview self-check FAILED on this device/driver; "
                                    "previews stay on the CPU path this session");
            }
        }
        static std::atomic<bool> gpu_engaged_logged{false};
        if (spk_gpu_scan_frames() > 0 &&
            !gpu_engaged_logged.exchange(true, std::memory_order_relaxed)) {
            __android_log_print(ANDROID_LOG_INFO, "Spektra",
                                "gpu scan path ACTIVE (eligible preview frames render on the GPU)");
        }
        // Same pair for the PRINT-EXPOSE offload (perf lab). It is a separate
        // kernel dispatch with its own self-check, and it only runs on the print
        // route, so "scan ACTIVE" says nothing about it — without these two the
        // print offload would be exactly the ambiguous silence #146 called out.
        static std::atomic<bool> gpu_print_state_logged{false};
        const int gpu_print = spk_gpu_print_state();
        if (gpu_print != 0 &&
            !gpu_print_state_logged.exchange(true, std::memory_order_relaxed)) {
            if (gpu_print == 1) {
                __android_log_print(ANDROID_LOG_INFO, "Spektra",
                                    "gpu print-expose self-check PASSED on this device/driver");
            } else {
                __android_log_print(ANDROID_LOG_WARN, "Spektra",
                                    "gpu print-expose self-check FAILED on this device/driver; "
                                    "the print integral stays on the CPU path this session");
            }
        }
        static std::atomic<bool> gpu_print_engaged_logged{false};
        if (spk_gpu_print_frames() > 0 &&
            !gpu_print_engaged_logged.exchange(true, std::memory_order_relaxed)) {
            __android_log_print(ANDROID_LOG_INFO, "Spektra",
                                "gpu print-expose path ACTIVE (print-route frames render "
                                "their spectral integral on the GPU)");
        }
    }
#endif
    if (st != SPK_OK) {
        timing.finish(st);
        throw_status(env, st);
        return nullptr;
    }
    if (!out.data) { throw_runtime(env, "spektra: engine returned no data"); return nullptr; }

    // Hand the output back as a NATIVE (off-heap) direct ByteBuffer rather than a
    // JVM-managed one. `ByteBuffer.allocateDirect` is backed on Android by a
    // non-movable byte[] on the ART managed heap, so a full-res result (~140 MB) plus
    // the equally large RAW input buffer blows the ~256 MB heap-growth limit at export
    // (OutOfMemoryError). Adobe Lightroom's native engine keeps full-res pixels in
    // native memory and never crosses them to the Java heap; we mirror that here by
    // malloc'ing the result and wrapping it with NewDirectByteBuffer. The Kotlin
    // SimResult owns it with the registry-issued token and releases it through
    // the token-aware private JNI bridge below.
    std::uint64_t out_bytes64 = 0;
    if (!spk::jni::checked_rgb_f32_bytes(out.width, out.height, &out_bytes64) ||
        out_bytes64 > static_cast<std::uint64_t>(INT32_MAX) ||
        out_bytes64 > static_cast<std::uint64_t>(
                          std::numeric_limits<std::size_t>::max())) {
        spk_image_free(&out);
        throw_runtime(env, "spektra: invalid or oversized engine output dimensions");
        return nullptr;
    }
    const size_t out_bytes = static_cast<size_t>(out_bytes64);

    void* native_buf = std::malloc(out_bytes);
    if (!native_buf) {
        spk_image_free(&out);
        jclass oom = env->FindClass("java/lang/OutOfMemoryError");
        if (oom) env->ThrowNew(oom, "spektra: failed to allocate native output buffer");
        else throw_runtime(env, "spektra: failed to allocate native output buffer");
        return nullptr;
    }
    if (!spk::jni::copy_bytes_cancellable(
            native_buf, out.data, out_bytes, cancellation_probe.callback(),
            cancellation_probe.context())) {
        std::free(native_buf);
        spk_image_free(&out);
        timing.finish(SPK_ERR_CANCELLED);
        if (!env->ExceptionCheck()) throw_status(env, SPK_ERR_CANCELLED);
        return nullptr;
    }

    // Capture dims BEFORE freeing the engine-side image.
    int out_w = out.width, out_h = out.height, out_cs = out.color_space;
    spk_image_free(&out);  // engine-side copy no longer needed

    jobject outBuf = env->NewDirectByteBuffer(
        native_buf, static_cast<jlong>(out_bytes));
    if (env->ExceptionCheck() || !outBuf) {
        std::free(native_buf);
        if (!env->ExceptionCheck())
            throw_runtime(env, "spektra: failed to wrap native output buffer");
        return nullptr;
    }

    // Register before constructing Kotlin ownership so a constructor failure can
    // release through the same opaque token without a double-free race.
    std::uint64_t allocation_token = 0;
    bool registered = false;
    try {
        registered = g_allocations.add(native_buf, out_bytes, &allocation_token);
    } catch (...) {
        std::free(native_buf);
        throw;
    }
    if (!registered || allocation_token == 0) {
        std::free(native_buf);
        throw_runtime(env, "spektra: failed to register native output buffer");
        return nullptr;
    }

    // Build SimResult(data, width, height, colorSpace, renderId, allocationToken). Capture the id
    // from the LIVE outer scope above; the public completed-snapshot getter still
    // points at the previous render until declaration-order RAII publishes us.
    // NOTE: NewDirectByteBuffer does NOT take ownership — if SimResult is never
    // constructed below, the Kotlin side can't free native_buf, so every failure
    // path from here on must std::free(native_buf) to avoid leaking the result.
    jclass csCls = env->FindClass("com/spectrafilm/engine/ColorSpace");
    if (!csCls) {
        env->ExceptionClear();
        env->DeleteLocalRef(outBuf);
        if (g_allocations.take(native_buf, out_bytes, allocation_token)) std::free(native_buf);
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
        if (g_allocations.take(native_buf, out_bytes, allocation_token)) std::free(native_buf);
        throw_runtime(env, "spektra: SimResult class not found");
        return nullptr;
    }
    jmethodID resCtor = env->GetMethodID(resCls, "<init>",
        "(Ljava/nio/ByteBuffer;IILcom/spectrafilm/engine/ColorSpace;JJ)V");
    jobject result = nullptr;
    if (resCtor) {
        result = env->NewObject(resCls, resCtor, outBuf, out_w, out_h, csObj,
                                static_cast<jlong>(render_id),
                                static_cast<jlong>(allocation_token));
        if (env->ExceptionCheck()) { env->ExceptionClear(); result = nullptr; }
    } else {
        env->ExceptionClear();
    }

    if (csArr) env->DeleteLocalRef(csArr);
    if (csObj) env->DeleteLocalRef(csObj);
    env->DeleteLocalRef(resCls);
    env->DeleteLocalRef(outBuf);
    if (!result) {
        // Kotlin init may already have token-released the buffer; `take` makes
        // this constructor rollback exact-once either way.
        if (g_allocations.take(native_buf, out_bytes, allocation_token)) std::free(native_buf);
        throw_runtime(env, "spektra: failed to construct SimResult");
        return nullptr;
    }
    // Object construction and allocation registration happen after the chunked
    // copy. Poll once more at the publication boundary so cancellation arriving
    // during that JNI work cannot return a stale successful SimResult. A request
    // after this poll is ordered after the native operation's commit point.
    const spk_cancel_check final_cancel_check = cancellation_probe.callback();
    if (final_cancel_check &&
        final_cancel_check(cancellation_probe.context()) != 0) {
        env->DeleteLocalRef(result);
        if (g_allocations.take(native_buf, out_bytes, allocation_token)) std::free(native_buf);
        timing.finish(SPK_ERR_CANCELLED);
        if (!env->ExceptionCheck()) throw_status(env, SPK_ERR_CANCELLED);
        return nullptr;
    }
    timing.finish(SPK_OK);
    return result;
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
    return nullptr;
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
    return nullptr;
} catch (...) {
    throw_unknown_cpp_exception(env);
    return nullptr;
}

/*
 * SimResult.freeDirectBuffer(buf) — release a native (malloc + NewDirectByteBuffer)
 * engine-output buffer only when its opaque allocation token matches. The JNI ABI is
 * `(ByteBuffer, long token)`. Called from
 * SimResult.close(). Named to match the Kotlin
 * @JvmStatic companion method (Java_com_spectrafilm_engine_SimResult_freeDirectBuffer),
 * NOT the SpektraEngine JNI() macro. Foreign, duplicate, stale-token, and wrong-token
 * release attempts are skipped.
 */
extern "C" JNIEXPORT void JNICALL
Java_com_spectrafilm_engine_SimResult_freeDirectBuffer(JNIEnv* env, jclass /*clazz*/,
                                                       jobject buf, jlong token) try {
    if (!buf || token <= 0) return;
    void* p = env->GetDirectBufferAddress(buf);
    const jlong capacity = env->GetDirectBufferCapacity(buf);
    if (!p || capacity <= 0 ||
        static_cast<std::uint64_t>(capacity) >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return;
    }
    const size_t size = static_cast<size_t>(capacity);
    if (g_allocations.take(p, size, static_cast<std::uint64_t>(token))) std::free(p);
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
} catch (...) {
    throw_unknown_cpp_exception(env);
}

/*
 * SimResult.nativeReportRenderOutcome(renderId, outcome) — keyed app-level
 * disposition emitted after native completion. A successful native render may
 * still be superseded/cancelled before Compose publishes it; release benchmark
 * consumers fold the latest event per renderId and exclude discarded work.
 */
extern "C" JNIEXPORT void JNICALL
Java_com_spectrafilm_engine_SimResult_nativeReportRenderOutcome(
    JNIEnv* env, jclass /*clazz*/, jlong render_id, jint outcome_code) try {
    if (render_id <= 0 || !timing_automation_enabled()) return;
    if (outcome_code < static_cast<jint>(spk::ARO_CONSUMED) ||
        outcome_code > static_cast<jint>(spk::ARO_FAILED)) {
        return;
    }
    const auto outcome = static_cast<spk::AppRenderOutcome>(outcome_code);
#ifdef __ANDROID__
    char json[256];
    if (spk::stage_timing_outcome_json_format(
            json, sizeof(json), static_cast<uint64_t>(render_id), outcome) > 0) {
        __android_log_print(ANDROID_LOG_INFO, "Spektra",
                            "render outcome json: %s", json);
    }
    if (ATrace_isEnabled()) {
        char trace_name[96];
        std::snprintf(trace_name, sizeof(trace_name),
                      "spk.render_outcome.%s#%llu",
                      spk::app_render_outcome_name(outcome),
                      static_cast<unsigned long long>(render_id));
        ATrace_beginSection(trace_name);
        ATrace_endSection();
    }
#else
    (void)outcome;
#endif
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
} catch (...) {
    throw_unknown_cpp_exception(env);
}

/*
 * SimResult.allocDirectBuffer(size) -> NativeBufferAllocation or null.
 * An OFF-HEAP direct buffer of `size` bytes, NOT on the ART managed heap (unlike
 * ByteBuffer.allocateDirect, which on Android is a non-movable byte[] counting against the
 * ~256 MB heap-growth limit). Used for large export staging buffers (e.g. the 16-bit
 * TIFF/PNG quantise buffer, ~600 MB at 100 MP). The returned opaque token is
 * required for release. Returns null on bad size or OOM (caller falls back to managed).
 */
extern "C" JNIEXPORT jobject JNICALL
Java_com_spectrafilm_engine_SimResult_allocDirectBuffer(JNIEnv* env, jclass /*clazz*/,
                                                        jlong size) try {
    if (size <= 0 || size > static_cast<jlong>(INT32_MAX) ||
        static_cast<std::uint64_t>(size) >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return nullptr;
    }
    void* p = std::malloc(static_cast<size_t>(size));
    if (!p) return nullptr;
    jobject buf = env->NewDirectByteBuffer(p, size);
    if (!buf) { std::free(p); return nullptr; }  // wrap failed -> don't leak
    std::uint64_t allocation_token = 0;
    bool registered = false;
    try {
        registered = g_allocations.add(p, static_cast<size_t>(size), &allocation_token);
    } catch (...) {
        std::free(p);
        throw;
    }
    if (!registered || allocation_token == 0) {
        std::free(p);
        return nullptr;
    }
    jclass allocation_cls = env->FindClass("com/spectrafilm/engine/NativeBufferAllocation");
    if (!allocation_cls) {
        env->ExceptionClear();
        if (g_allocations.take(p, static_cast<size_t>(size), allocation_token)) std::free(p);
        return nullptr;
    }
    jmethodID allocation_ctor = env->GetMethodID(
        allocation_cls, "<init>", "(Ljava/nio/ByteBuffer;J)V");
    jobject allocation = nullptr;
    if (allocation_ctor) {
        allocation = env->NewObject(allocation_cls, allocation_ctor, buf,
                                    static_cast<jlong>(allocation_token));
    }
    env->DeleteLocalRef(allocation_cls);
    env->DeleteLocalRef(buf);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!allocation) {
        if (g_allocations.take(p, static_cast<size_t>(size), allocation_token)) std::free(p);
        return nullptr;
    }
    return allocation;
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
    return nullptr;
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
    return nullptr;
} catch (...) {
    throw_unknown_cpp_exception(env);
    return nullptr;
}

/*
 * nativeMeterExposureEv(handle, inBuf, w, h, paramsObj, cancellation) -> double EV.
 * Meters the image the way a render would and returns the auto-exposure
 * compensation in EV (linear gain = 2**ev), without rendering. Used by the LUT
 * preview paths, which must supply the exposure gain themselves because a baked
 * 3D LUT carries none (see spk_meter_exposure_ev / spk_bake_cube_lut).
 */
JNI(jdouble, nativeMeterExposureEv)(JNIEnv* env, jobject /*thiz*/, jlong handle,
                                    jobject inBuf, jint w, jint h,
                                    jstring inCs,
                                    jobject paramsObj,
                                    jobject cancellation) try {
    spk_engine* eng = reinterpret_cast<spk_engine*>(handle);
    if (!eng) { throw_runtime(env, "spektra: engine handle is null"); return 0.0; }
    if (!inBuf) { throw_runtime(env, "spektra: input ByteBuffer is null"); return 0.0; }
    if (w <= 0 || h <= 0) {
        throw_runtime(env, "spektra: invalid image dimensions");
        return 0.0;
    }
    float* in_data = nullptr;
    if (!direct_float_input(env, inBuf, w, h, &in_data)) return 0.0;

    // Match nativeSimulate's input contract. Metering feeds the same ProPhoto
    // luminance transform as rendering, so silently dropping LinearImage's tag
    // would produce a plausible but physically wrong exposure value.
    const std::string in_cs = jstr(env, inCs);
    if (!in_cs.empty() && in_cs != "ProPhoto RGB") {
        throw_runtime(env,
            ("spektra: unsupported input color space '" + in_cs +
             "' (the engine accepts linear ProPhoto RGB only)").c_str());
        return 0.0;
    }

    spk_params params;
    ParamStorage store;
    if (!marshal_params(env, paramsObj, &params, &store)) {
        throw_runtime(env, "spektra: failed to marshal params");
        return 0.0;
    }
    JniCancellation cancellation_probe(env, cancellation);
    if (!cancellation_probe.initialize()) return 0.0;

    spk_image img{in_data, w, h, static_cast<int32_t>(SPK_CS_PROPHOTO)};
    double ev = 0.0;
    spk_status st = spk_meter_exposure_ev_cancellable(
        eng, &img, &params, &ev, cancellation_probe.callback(),
        cancellation_probe.context());
    if (st != SPK_OK) { throw_status(env, st); return 0.0; }
    return static_cast<jdouble>(ev);
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
    return 0.0;
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
    return 0.0;
} catch (...) {
    throw_unknown_cpp_exception(env);
    return 0.0;
}

/*
 * nativeBakeCubeLut(handle, paramsObj, size, shaper, cancellation)
 *     -> LutBakeResult or null.
 * Marshals the params (reusing marshal_params), bakes a size^3 3D LUT of the
 * current film look, and returns its text plus the logical render id. The bake
 * forces all spatial /
 * stochastic effects off (see spk_bake_cube_lut). Sized in two passes: first to
 * learn the required buffer length, then to fill it. Both passes are nested in
 * one outer timing/log context, and the sizing query is successful.
 */
JNI(jobject, nativeBakeCubeLut)(JNIEnv* env, jobject /*thiz*/, jlong handle,
                                jobject paramsObj, jint size, jint shaper,
                                jobject cancellation) try {
    StageTimingLogGuard timing_log;
    spk::ScopedRenderTiming timing(spk::RTK_LUT_BAKE);
    const uint64_t render_id = timing.render_id();
    timing_log.expect(render_id);

    spk_engine* eng = reinterpret_cast<spk_engine*>(handle);
    if (!eng) { throw_runtime(env, "spektra: engine handle is null"); return nullptr; }

    spk_params params;
    ParamStorage store;
    if (!marshal_params(env, paramsObj, &params, &store)) {
        throw_runtime(env, "spektra: failed to marshal params");
        return nullptr;
    }
    JniCancellation cancellation_probe(env, cancellation);
    if (!cancellation_probe.initialize()) return nullptr;

    size_t needed = 0;
    spk_status st = spk_bake_cube_lut_cancellable(
        eng, &params, size, shaper, nullptr, 0, &needed,
        cancellation_probe.callback(), cancellation_probe.context());
    if (st != SPK_OK || needed == 0) {
        const spk_status failure = st == SPK_OK ? SPK_ERR_INTERNAL : st;
        timing.finish(failure);
        throw_status(env, failure);
        return nullptr;
    }

    std::vector<char> buf(needed);
    st = spk_bake_cube_lut_cancellable(
        eng, &params, size, shaper, buf.data(), buf.size(), &needed,
        cancellation_probe.callback(), cancellation_probe.context());
    if (st != SPK_OK) {
        timing.finish(st);
        throw_status(env, st);
        return nullptr;
    }

    jstring text = env->NewStringUTF(buf.data());
    if (!text) {
        if (!env->ExceptionCheck()) {
            throw_runtime(env, "spektra: failed to construct LUT text");
        }
        return nullptr;
    }
    jclass result_cls =
        env->FindClass("com/spectrafilm/engine/LutBakeResult");
    if (!result_cls) {
        env->ExceptionClear();
        env->DeleteLocalRef(text);
        throw_runtime(env, "spektra: LutBakeResult class not found");
        return nullptr;
    }
    jmethodID ctor = env->GetMethodID(
        result_cls, "<init>", "(Ljava/lang/String;J)V");
    jobject result = nullptr;
    if (ctor) {
        result = env->NewObject(result_cls, ctor, text,
                                static_cast<jlong>(render_id));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            result = nullptr;
        }
    } else {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(result_cls);
    env->DeleteLocalRef(text);
    if (!result) {
        throw_runtime(env, "spektra: failed to construct LutBakeResult");
        return nullptr;
    }
    timing.finish(SPK_OK);
    return result;
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
    return nullptr;
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
    return nullptr;
} catch (...) {
    throw_unknown_cpp_exception(env);
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
JNI(void, nativeSetBigCores)(JNIEnv* env, jclass /*clazz*/, jint mode) try {
    spk_set_big_cores(static_cast<int>(mode));
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
} catch (...) {
    throw_unknown_cpp_exception(env);
}

JNI(jint, nativeBigCoreCount)(JNIEnv* env, jclass /*clazz*/) try {
    return static_cast<jint>(spk_big_core_count());
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
    return 0;
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
    return 0;
} catch (...) {
    throw_unknown_cpp_exception(env);
    return 0;
}

// Instrumentation seam for the actual jobject -> spk_params bridge. Keeping the
// observation behind JNI catches getter names/signatures and tuple/member loss.
// The engine instrumentation APK is not R8-minified; shipping-R8 reachability is
// covered separately by the release integration gate. It also catches
// appended selector wiring, and default survival—coverage a direct spk_params
// host test cannot provide.
JNI(jstring, nativeDebugMarshalledParams)(JNIEnv* env, jclass /*clazz*/,
                                          jobject paramsObj) try {
    spk_params params;
    ParamStorage store;
    if (!marshal_params(env, paramsObj, &params, &store)) {
        if (!env->ExceptionCheck()) {
            throw_runtime(env, "spektra: failed to marshal params");
        }
        return nullptr;
    }
    // Full named inventory, deliberately independent of ABI layout/padding.
    // The device test builds expected values from the Kotlin tree, not from an
    // observed JNI digest, so a getter already missing today cannot be blessed.
    const std::string manifest = spk::params_manifest::named_inventory(params);
    return env->NewStringUTF(manifest.c_str());
} catch (const std::bad_alloc&) {
    throw_native_oom(env);
    return nullptr;
} catch (const std::exception& e) {
    throw_cpp_exception(env, e);
    return nullptr;
} catch (...) {
    throw_unknown_cpp_exception(env);
    return nullptr;
}
