/*
 * Spektrafilm for Android — persisted app settings. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * A thin SharedPreferences-backed store for app-level preferences that are NOT part of
 * the per-image SpektraParams tree: the default output color space, preview resolution,
 * default film/print profiles, export format + quality, theme, and the onboarding
 * "seen" flag. ParamsState owns the render parameters; this owns the app's defaults and
 * chrome. Values are read/written eagerly (commit/apply) so they survive process death.
 *
 * applyDefaultsTo() seeds a fresh ParamsState with the user's saved defaults on launch.
 */
package com.spectrafilm.app

import android.content.Context
import android.content.SharedPreferences
import com.spectrafilm.engine.ColorSpace

/** App theme preference. */
enum class ThemeMode(val display: String) {
    SYSTEM("System default"),
    LIGHT("Light"),
    DARK("Dark"),
}

/**
 * Persisted application settings. Construct lazily via [AppSettings.from] (which caches
 * a single instance per process). All setters write through to SharedPreferences.
 */
class AppSettings private constructor(private val prefs: SharedPreferences) {

    var seenOnboarding: Boolean
        get() = prefs.getBoolean(KEY_SEEN_ONBOARDING, false)
        set(v) { prefs.edit().putBoolean(KEY_SEEN_ONBOARDING, v).apply() }

    /** Whether the one-time editor coach-mark overlay (tap-to-edit / compare / 100%) was shown. */
    var seenEditorCoach: Boolean
        get() = prefs.getBoolean(KEY_SEEN_EDITOR_COACH, false)
        set(v) { prefs.edit().putBoolean(KEY_SEEN_EDITOR_COACH, v).apply() }

    /**
     * GPU LUT preview (default OFF — opt-in). When on, the FIT view renders the current
     * look instantly by GPU-sampling the engine's baked 3D LUT instead of a ~1s CPU render
     * per edit. EXPORT is always the exact CPU engine — GPU is preview-only, never the
     * parity path.
     *
     * DEFAULT OFF: the current GLSurfaceView-based surface churns (continuous buffer
     * re-allocation / dropped frames) when the editor's preview area resizes during panel
     * animations, and can grow over the bottom controls — observed on SM-S948W as a frame
     * hang plus a hidden export button. Being reimplemented on a resize-friendly surface
     * (TextureView) before it can be safe to default on. Enable here to try it meanwhile.
     */
    var gpuPreview: Boolean
        get() = prefs.getBoolean(KEY_GPU_PREVIEW, false)
        set(v) { prefs.edit().putBoolean(KEY_GPU_PREVIEW, v).apply() }

    /**
     * Pin the render pool to the device's performance cores.
     *
     * MEASURED A REGRESSION ON A WHOLE RENDER. Kept as a pref-only experiment
     * switch; there is deliberately no Settings UI for it.
     *
     * The microbenchmark said 1.51x (SM-S948W, halation path, 1024x768: 71.86 ms
     * unpinned vs 47.58 ms pinned), and capping the pool without pinning did NOT
     * reproduce it, so core placement really was the mechanism THERE. It did not
     * survive the end-to-end pipeline. A 12.5 MP export on the same device:
     *
     * ```
     *   OFF  median 14476 ms          ON  median 20458 ms      1.41x SLOWER
     *   grain          3620 -> 8612 ms  (2.38x) = 83% of the regression
     *   filming_expose  177 ->  391 ms  (2.20x)
     *   dir_couplers   1349 -> 1466 ms  (1.09x) — essentially immune
     *   halation       2055 -> 2159 ms  (1.05x) — essentially immune
     * ```
     *
     * Why: pinning also caps the pool to the big-core count (parallel.cpp), so ON is
     * 2 workers on 2 prime cores against OFF's 8 workers across all 8 — 9.48 GHz of
     * aggregate clock against 31.26 GHz. A 1.31x per-core clock edge cannot cover a
     * 3.3x compute deficit. The microbenchmark was one spatial filter on a small
     * fixture, where thread-spawn overhead dominates and 2 workers legitimately beat
     * 8; a full-resolution render is nothing like that.
     *
     * Output is unaffected either way — affinity moves only WHERE a chunk runs, and
     * every worker count is byte-identical under the parity gate's thread-invariance
     * contract. That half held up. Default OFF, and it should stay off.
     */
    var bigCores: Boolean
        get() = prefs.getBoolean(KEY_BIG_CORES, false)
        set(v) { prefs.edit().putBoolean(KEY_BIG_CORES, v).apply() }

    /**
     * One-time cleanup of the ORIGINAL `big_cores` key.
     *
     * When the Settings row was removed, nothing wrote that key any more but
     * EngineHolder still read and honoured it — so anyone who had ever flipped the
     * switch was left permanently 1.41x slow, with no UI to discover it and no way
     * to turn it off. The key above is deliberately a NEW name, so a stale value can
     * never be honoured again; this drops the old one so it does not linger.
     */
    fun clearLegacyBigCores() {
        if (prefs.contains(LEGACY_KEY_BIG_CORES)) {
            prefs.edit().remove(LEGACY_KEY_BIG_CORES).apply()
            Diag.i("cleared legacy big_cores pref (feature was measured 1.41x slower)")
        }
    }

    /**
     * GPU ENGINE preview (Vulkan, GPU M1 #146) — distinct from [gpuPreview] (the
     * GLES LUT loupe overlay above): the film simulation itself runs its scan
     * stage on the GPU for interactive previews (~2e-6 from the CPU chain,
     * tighter than the preview's 3D LUT at ~5e-5 — see PR #145). Preview-only:
     * export always renders on the exact CPU engine. Guarded by a one-time
     * on-device self-check with automatic CPU fallback. Default OFF until the
     * on-device validation round (#147 session) signs it off.
     */
    var gpuEngine: Boolean
        get() = prefs.getBoolean(KEY_GPU_ENGINE, false)
        set(v) { prefs.edit().putBoolean(KEY_GPU_ENGINE, v).apply() }

    /**
     * EXPERIMENTAL GPU export (Vulkan, #154 — GPU M4 seed / #149 option B):
     * the film simulation's scan stage runs on the GPU for FULL-RESOLUTION
     * exports, not just previews. "Oracle-verified on your device" — the same
     * on-device self-check gates it, and the CPU engine stays the ground truth
     * and automatic fallback. Default OFF; a plain export is byte-identical to
     * the CPU path. Independent of [gpuEngine] (the preview toggle).
     */
    var gpuExportEngine: Boolean
        get() = prefs.getBoolean(KEY_GPU_EXPORT, false)
        set(v) { prefs.edit().putBoolean(KEY_GPU_EXPORT, v).apply() }

    /**
     * PROGRESSIVE LADDER rung (perf lab). Longest edge of the live DRAFT render that
     * runs while a slider is being dragged, before the crisp settle pass lands.
     *
     * Was the fixed constant [DRAFT_RENDER_MAX_PX]; made a setting so the coarse/fine
     * trade can be swept on a real device instead of guessed. Lower tracks the finger
     * more closely; higher makes the live frame a better preview of the settle result.
     * Clamped to a band where the step-down is still meaningful — below 128 the frame
     * is too coarse to judge a colour edit by, and above 512 the draft costs enough
     * that it stops being a draft.
     *
     * Defaults to [DRAFT_RENDER_MAX_PX], so an untouched install renders as before.
     */
    var draftRenderMaxPx: Int
        get() = prefs.getInt(KEY_DRAFT_MAX_PX, DRAFT_RENDER_MAX_PX).coerceIn(128, 512)
        set(v) { prefs.edit().putInt(KEY_DRAFT_MAX_PX, v.coerceIn(128, 512)).apply() }

    var theme: ThemeMode
        get() = runCatching { ThemeMode.valueOf(prefs.getString(KEY_THEME, ThemeMode.SYSTEM.name)!!) }
            .getOrDefault(ThemeMode.SYSTEM)
        set(v) { prefs.edit().putString(KEY_THEME, v.name).apply() }

    var defaultOutputColorSpace: ColorSpace
        get() = runCatching { ColorSpace.valueOf(prefs.getString(KEY_OUTPUT_CS, ColorSpace.SRGB.name)!!) }
            .getOrDefault(ColorSpace.SRGB)
        set(v) { prefs.edit().putString(KEY_OUTPUT_CS, v.name).apply() }

    var previewMaxSize: Int
        get() = prefs.getInt(KEY_PREVIEW_MAX, 640)
        set(v) { prefs.edit().putInt(KEY_PREVIEW_MAX, v).apply() }

    var defaultFilmProfile: String
        get() = prefs.getString(KEY_FILM, "") ?: ""
        set(v) { prefs.edit().putString(KEY_FILM, v).apply() }

    var defaultPrintProfile: String
        get() = prefs.getString(KEY_PRINT, "") ?: ""
        set(v) { prefs.edit().putString(KEY_PRINT, v).apply() }

    var exportFormat: ExportFormat
        get() = runCatching { ExportFormat.valueOf(prefs.getString(KEY_EXPORT_FMT, ExportFormat.PNG.name)!!) }
            .getOrDefault(ExportFormat.PNG)
        set(v) { prefs.edit().putString(KEY_EXPORT_FMT, v.name).apply() }

    var exportQuality: Int
        get() = prefs.getInt(KEY_EXPORT_Q, 95).coerceIn(1, 100)
        set(v) { prefs.edit().putInt(KEY_EXPORT_Q, v.coerceIn(1, 100)).apply() }

    /**
     * Whether to preserve GPS/location EXIF tags when copying source metadata into
     * exported images. Defaults to FALSE (strip location) — the privacy-safe default
     * for a shareable photo app (security review F3). All other EXIF (camera/lens/
     * exposure/date) is always copied; this toggle only governs the GPS block.
     */
    var exportKeepGps: Boolean
        get() = prefs.getBoolean(KEY_EXPORT_KEEP_GPS, false)
        set(v) { prefs.edit().putBoolean(KEY_EXPORT_KEEP_GPS, v).apply() }

    /**
     * Seed a freshly constructed [state] with the saved app defaults. Profile defaults
     * are only applied when they appear in [availableProfiles] (so a stale id can't make
     * the picker show a profile the engine doesn't know).
     */
    fun applyDefaultsTo(state: ParamsState, availableProfiles: List<String>) {
        state.outputColorSpace = defaultOutputColorSpace
        state.previewMaxSize = previewMaxSize
        state.gpuEngine = gpuEngine
        state.gpuExport = gpuExportEngine
        if (defaultFilmProfile.isNotBlank() && defaultFilmProfile in availableProfiles) {
            state.filmProfile = defaultFilmProfile
        }
        if (defaultPrintProfile.isNotBlank() && defaultPrintProfile in availableProfiles) {
            state.printProfile = defaultPrintProfile
        }
    }

    companion object {
        private const val PREFS_NAME = "spectrafilm_settings"
        private const val KEY_SEEN_ONBOARDING = "seen_onboarding"
        private const val KEY_SEEN_EDITOR_COACH = "seen_editor_coach"
        private const val KEY_GPU_PREVIEW = "gpu_preview"
        private const val KEY_GPU_ENGINE = "gpu_engine_preview"
        private const val KEY_GPU_EXPORT = "gpu_engine_export"
        private const val KEY_DRAFT_MAX_PX = "draft_render_max_px"
        // Renamed from "big_cores". The old key must never be read again — see
        // clearLegacyBigCores(). This one exists only so an A/B can still be driven
        // from a shell without a UI; it is not a product setting.
        private const val KEY_BIG_CORES = "big_cores_experiment"
        private const val LEGACY_KEY_BIG_CORES = "big_cores"
        private const val KEY_THEME = "theme"
        private const val KEY_OUTPUT_CS = "output_color_space"
        private const val KEY_PREVIEW_MAX = "preview_max_size"
        private const val KEY_FILM = "default_film_profile"
        private const val KEY_PRINT = "default_print_profile"
        private const val KEY_EXPORT_FMT = "export_format"
        private const val KEY_EXPORT_Q = "export_quality"
        private const val KEY_EXPORT_KEEP_GPS = "export_keep_gps"

        @Volatile private var instance: AppSettings? = null

        fun from(ctx: Context): AppSettings =
            instance ?: synchronized(this) {
                instance ?: AppSettings(
                    ctx.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE),
                ).also { instance = it }
            }
    }
}
