/*
 * Spektrafilm for Android — app entry. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Lightroom-mobile-style editor: an edge-to-edge near-black canvas with a pinned live
 * preview, a transparent top bar (back/close + export/save + Settings gear + About "?"),
 * an inline adjustment panel that slides up when a category is chosen, and a horizontally
 * scrollable bottom category bar mapping each spektrafilm GUI section (Input, RAW WB,
 * Simulation, Grain, Preflash, Halation, Couplers, Glare, Experimental, Display, Presets,
 * Source) to one icon. Edits rebuild an immutable SpektraParams and trigger a debounced,
 * downscaled preview render. Export renders full-resolution behind a blocking mask then
 * saves to the gallery. A preview rotate button rotates the decoded source so both the
 * preview AND the export reflect the orientation. RAW/DNG import (LibRaw -> ACES2065-1 ->
 * linear ProPhoto RGB), the sRGB photo picker, the synthetic demo image, named JSON presets, and non-destructive
 * per-image recipe auto-save/restore are all preserved.
 */
package com.spectrafilm.app

import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.spring
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.animation.expandVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.lifecycleScope
import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.InputGamutCompress
import com.spectrafilm.engine.LinearImage
import com.spectrafilm.engine.OutputGamutCompress
import com.spectrafilm.engine.Rgb2Raw
import com.spectrafilm.engine.SpektraEngine
import com.spectrafilm.libraw.DecodeStatus
import com.spectrafilm.libraw.RawDecodeException
import com.spectrafilm.libraw.WhiteBalance
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Which kind of source image is loaded. */
private enum class SourceKind { DEMO, PHOTO, RAW }

/** Top-level navigation destinations. */
private enum class Screen { EDITOR, SETTINGS, ABOUT, CURVES_FILM, CURVES_PRINT, DIAGNOSTICS }

/** Adjustment categories shown in the bottom bar; each maps to an existing section. */
private enum class Category(val label: String) {
    SOURCE("Source"),
    PRESETS("Presets"),
    SIMULATION("Simulation"),
    INPUT("Input"),
    RAW_WB("White Bal"),
    GRAIN("Grain"),
    HALATION("Halation"),
    GLARE("Glare"),
    COUPLERS("Couplers"),
    PREFLASH("Preflash"),
    EXPERIMENTAL("Experimental"),
    TONE_CURVE("Tone Curve"),
    MASKS("Masks"),
    DISPLAY("Display"),
}

// Neutral parameter defaults (a fresh ParamsState) — source for slider
// double-tap-to-reset targets.
private val PARAM_DEFAULTS = ParamsState()

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        // Wide-gamut compositing so the color-managed preview bitmaps (tagged Adobe/ProPhoto/Rec2020
        // per the output space) are not clamped to sRGB at composition on wide-gamut panels. No-op on
        // sRGB displays. API 26+; minSdk is 24.
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
            window.colorMode = android.content.pm.ActivityInfo.COLOR_MODE_WIDE_COLOR_GAMUT
        }
        // Persist the last fatal stack trace so the in-app Diagnostics screen can show it
        // after a restart (no permission needed; chains to the platform handler).
        Diagnostics.installCrashHandler(this)
        val settings = AppSettings.from(this)
        setContent {
            var themeMode by remember { mutableStateOf(settings.theme) }
            val dark = when (themeMode) {
                ThemeMode.SYSTEM -> isSystemInDarkTheme()
                ThemeMode.LIGHT -> false
                ThemeMode.DARK -> true
            }
            MaterialTheme(colorScheme = if (dark) darkColorScheme() else lightColorScheme()) {
                AppRoot(settings = settings, onThemeChanged = { themeMode = it })
            }
        }
    }

    /** Hosts onboarding + top-level navigation around the editor. */
    @Composable
    private fun AppRoot(settings: AppSettings, onThemeChanged: (ThemeMode) -> Unit) {
        var showOnboarding by remember { mutableStateOf(!settings.seenOnboarding) }
        // One-time editor coach marks, shown once onboarding is out of the way.
        var showEditorCoach by remember { mutableStateOf(!settings.seenEditorCoach) }
        var screen by remember { mutableStateOf(Screen.EDITOR) }
        // Hoisted here (not inside EditorScreen) so the open adjustment category survives a
        // round-trip to Settings/About and back — you return to where you were editing,
        // Lightroom-style, instead of a collapsed panel.
        val editorCategory = remember { mutableStateOf<Category?>(null) }
        val ctx = LocalContext.current

        // Catalog-grouped profile options for the Settings default-profile pickers.
        var settingsFilmGroups by remember { mutableStateOf<List<DropdownGroup>>(emptyList()) }
        var settingsPrintGroups by remember { mutableStateOf<List<DropdownGroup>>(emptyList()) }

        // Profile IDs and names remembered for the Curves screens.
        var curvesFilmId by remember { mutableStateOf("") }
        var curvesFilmName by remember { mutableStateOf("") }
        var curvesPrintId by remember { mutableStateOf("") }
        var curvesPrintName by remember { mutableStateOf("") }

        // Back from a pushed sub-screen returns to the editor (root).
        BackHandler(enabled = screen != Screen.EDITOR) { screen = Screen.EDITOR }

        val screenState = rememberSaveableStateHolder()

        Box(
            Modifier
                .fillMaxSize()
                .background(SpectraIcons.nearBlackCanvas),
        ) {
            // Each destination keeps its own rememberSaveable bucket, keyed by screen.
            // Without this the `when` below simply drops EditorScreen out of composition
            // when the user opens Settings, discarding sourceUri/sourceKind/sourceName/
            // rotation with it — so coming back from Settings silently landed on the
            // synthetic demo image instead of the photo being edited. Those four are
            // declared rememberSaveable precisely so source identity is durable; the nav
            // swap was defeating that, and it also made any in-app A/B of a render
            // setting impossible, since reaching the toggle destroyed the subject.
            screenState.SaveableStateProvider(screen) {
                when (screen) {
                    Screen.EDITOR -> EditorScreen(
                        settings = settings,
                        activeCategoryState = editorCategory,
                        onOpenSettings = { screen = Screen.SETTINGS },
                        onOpenAbout = { screen = Screen.ABOUT },
                        onProfileGroups = { f, p -> settingsFilmGroups = f; settingsPrintGroups = p },
                        onOpenFilmCurves = { id, name ->
                            curvesFilmId = id; curvesFilmName = name; screen = Screen.CURVES_FILM
                        },
                        onOpenPrintCurves = { id, name ->
                            curvesPrintId = id; curvesPrintName = name; screen = Screen.CURVES_PRINT
                        },
                    )
                    Screen.SETTINGS -> NavScaffold("Settings", onBack = { screen = Screen.EDITOR }) {
                        SettingsScreen(
                            settings = settings,
                            filmGroups = settingsFilmGroups,
                            printGroups = settingsPrintGroups,
                            onThemeChanged = onThemeChanged,
                            onShowOnboarding = { showOnboarding = true; screen = Screen.EDITOR },
                            onOpenDiagnostics = { screen = Screen.DIAGNOSTICS },
                        )
                    }
                    Screen.DIAGNOSTICS -> NavScaffold("Diagnostics", onBack = { screen = Screen.SETTINGS }) {
                        DiagnosticsScreen()
                    }
                    Screen.ABOUT -> NavScaffold("About", onBack = { screen = Screen.EDITOR }) {
                        AboutScreen()
                    }
                    Screen.CURVES_FILM -> ProfileCurvesScreen(
                        profileId = curvesFilmId,
                        displayName = curvesFilmName,
                        onBack = { screen = Screen.EDITOR },
                    )
                    Screen.CURVES_PRINT -> ProfileCurvesScreen(
                        profileId = curvesPrintId,
                        displayName = curvesPrintName,
                        onBack = { screen = Screen.EDITOR },
                    )
                }
            }

            if (showOnboarding && screen == Screen.EDITOR) {
                WelcomeFlow(
                    onFinish = { settings.seenOnboarding = true; showOnboarding = false },
                    onOpenSettings = {
                        settings.seenOnboarding = true; showOnboarding = false; screen = Screen.SETTINGS
                    },
                    onReportIssue = { Links.open(ctx, Links.NEW_ISSUE) },
                )
            }

            // Editor coach marks: after onboarding, the first time the editor is shown.
            if (!showOnboarding && showEditorCoach && screen == Screen.EDITOR) {
                EditorCoachOverlay(
                    onDismiss = { settings.seenEditorCoach = true; showEditorCoach = false },
                )
            }
        }
    }

    /** A simple back-arrow top bar wrapping a sub-screen (inset for status bar). */
    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    private fun NavScaffold(title: String, onBack: () -> Unit, content: @Composable () -> Unit) {
        Surface(color = MaterialTheme.colorScheme.background, modifier = Modifier.fillMaxSize()) {
            Column(
                Modifier
                    .fillMaxSize()
                    .windowInsetsPadding(WindowInsets.systemBars),
            ) {
                TopAppBar(
                    title = { Text(title) },
                    navigationIcon = {
                        TextButton(onClick = onBack) { Text("Back") }
                    },
                )
                Box(Modifier.weight(1f)) { content() }
            }
        }
    }

    @Composable
    private fun EditorScreen(
        settings: AppSettings,
        activeCategoryState: MutableState<Category?>,
        onOpenSettings: () -> Unit,
        onOpenAbout: () -> Unit,
        onProfileGroups: (List<DropdownGroup>, List<DropdownGroup>) -> Unit,
        onOpenFilmCurves: (id: String, name: String) -> Unit,
        onOpenPrintCurves: (id: String, name: String) -> Unit,
    ) {
        val ctx = LocalContext.current.applicationContext
        val scope = lifecycleScope

        var engine by remember { mutableStateOf<SpektraEngine?>(null) }
        var profiles by remember { mutableStateOf<List<String>>(emptyList()) }
        val state = remember { ParamsState() }

        // PERF: decoded proxy-source cache. The interactive preview path re-uses this decoded
        // LinearImage across look/film param edits instead of re-running loadSource() (LibRaw
        // RAW decode or bitmap decode + sRGB→ProPhoto linearization + EXIF/manual rotation) on
        // every previewTick. Keyed by the decode-affecting inputs only (URI + kind + RAW WB/
        // temp/tint + manual rotation + target edge); any change to one of those invalidates it
        // (the key mismatches → fresh decode). See DecodedSourceCache for the read-only proof
        // that the same buffer can be re-fed to the engine without a defensive copy.
        // EXPORT does NOT use this cache — it always decodes fresh at EXPORT_MAX_EDGE_PX.
        val sourceCache = remember { DecodedSourceCache() }
        // Retained-result grade cache: grade-only edits (saturation/vibrance/gamut/
        // masks) re-grade the last settle render's pristine engine output in pure
        // Kotlin — zero native work. Keyed by engine params + decode key + edge.
        val gradeCache = remember { GradeCache() }
        DisposableEffect(Unit) { onDispose { sourceCache.invalidate() } }

        // PERF/OOM: a SECOND single-entry cache for the MAX_EDGE_PX whole-image proxy used by the
        // Lightroom zoom (renderRoi) and the 100% magnifier. Previously both decoded fresh on every
        // gesture settle — each a ~1s LibRaw decode + a 36MB managed-heap LinearImage (1500×2000×3×
        // 4B) — so rapid pan/zoom stacked several before GC reclaimed them and the ART heap OOM'd
        // ("Failed to allocate a 36000019 byte allocation"). Keeping the 2048px proxy resident (and
        // reusing it for every crop) removes both the OOM pileup and the per-gesture decode latency,
        // which is what makes zoom-pan feel Lightroom-smooth. It is a SEPARATE instance from
        // sourceCache so the 640px preview proxy and the 2048px zoom proxy don't evict each other
        // (the single-entry cache is keyed by maxEdge — sharing one would thrash both back to a
        // fresh decode every render).
        val zoomSourceCache = remember { DecodedSourceCache() }
        // Single-flight guards: a cancelled zoom/preview render's still-running native decode is
        // reused by the next gesture instead of triggering an overlapping re-decode (battery).
        val zoomDecodeFlight = remember { SingleFlight<LinearImage>() }
        val previewDecodeFlight = remember { SingleFlight<LinearImage>() }
        DisposableEffect(Unit) { onDispose { zoomSourceCache.invalidate() } }

        // bundled catalog (friendly stock names + grouping) and built-in presets
        var builtInGroups by remember { mutableStateOf<Map<String, List<BuiltInPreset>>>(emptyMap()) }
        var catalogReady by remember { mutableStateOf(false) }

        // image / result state. The source identity (uri/kind/name) and the manual
        // rotation survive process death via rememberSaveable so a recreated Activity
        // restores the loaded image instead of silently dropping the user back on the
        // demo image. Uri is Parcelable and the enums are Serializable, so the default
        // autoSaver stores them. The decode + recipe-restore re-run off the restored
        // source via the post-init render (previewTick) and the recipe-open effect.
        var sourceUri by rememberSaveable { mutableStateOf<Uri?>(null) }
        var sourceKind by rememberSaveable { mutableStateOf(SourceKind.DEMO) }
        var sourceName by rememberSaveable { mutableStateOf("synthetic demo image") }
        var preview by remember { mutableStateOf<Bitmap?>(null) }
        var beforePreview by remember { mutableStateOf<Bitmap?>(null) }
        var status by remember { mutableStateOf("initializing…") }
        var previewBusy by remember { mutableStateOf(false) }
        var decoding by remember { mutableStateOf(false) }
        var lastRenderMs by remember { mutableStateOf<Long?>(null) }
        var renderErr by remember { mutableStateOf<String?>(null) }
        var exporting by remember { mutableStateOf(false) }
        var exportDone by remember { mutableStateOf(false) }
        // Lightroom-style export sheet: per-export format / quality / size / colour / name, instead
        // of the global Settings defaults. Seeded from Settings and remembered back on export.
        var showExportSheet by remember { mutableStateOf(false) }
        var exportOptions by remember {
            mutableStateOf(
                ExportOptions(settings.exportFormat, settings.exportQuality, ExportSize.FULL, 2048, ""),
            )
        }
        var exportKeepGps by remember { mutableStateOf(settings.exportKeepGps) }
        var previewTick by remember { mutableIntStateOf(0) }
        // Slider drag tracking (Lightroom's ICBSliderTrackingBegin/End): true while a slider is
        // being dragged, so the live DRAFT pass runs only during a drag and a discrete edit
        // (switch/dropdown) goes straight to the crisp settle render — no draft flicker, and the
        // continuous-draft cost is bounded to actual drags. Remembered so its identity is stable
        // across recompositions; provided to sliders via LocalSliderInteraction below.
        val interacting = remember { mutableStateOf(false) }
        val sliderInteraction = remember {
            SliderInteraction(
                onChange = { interacting.value = true },
                onFinished = { interacting.value = false },
            )
        }

        // source rotation (applied to the decoded LinearImage -> preview AND export).
        // This is the user's MANUAL step only; the EXIF baseline is derived fresh per load
        // (see loadSource) and is NOT persisted in the recipe.
        var rotation by rememberSaveable { mutableStateOf(SourceRotation.NONE) }

        // Set by loadSource when a compressed (lossy/JPEG-XL) DNG fell back to the platform
        // ImageDecoder. Drives a one-shot snackbar (a render path can't show UI directly).
        var dngFallbackNotice by remember { mutableStateOf(false) }

        // LUT export
        var bakingLut by remember { mutableStateOf(false) }
        var pendingLutText by remember { mutableStateOf<String?>(null) }

        // viewer modes
        var compareMode by remember { mutableStateOf(false) }
        var showHistogram by remember { mutableStateOf(false) }

        // Experimental GPU LUT preview (Settings → Experimental, default OFF). When on we
        // keep the latest linear proxy + a baked 3D LUT of the current look; PreviewRegion
        // GPU-samples them instead of the CPU bitmap. Read at composition — toggling in
        // Settings applies on the next return to the editor.
        val gpuEnabled = settings.gpuPreview
        var gpuProxy by remember { mutableStateOf<LinearImage?>(null) }
        var gpuLut by remember { mutableStateOf<CubeLut?>(null) }
        // Auto-exposure gain (2^ev) for the GPU path: the LUT is baked AE-off at unity
        // gain, so the shader multiplies this in before the lookup (see LutGpuPreview).
        var gpuGain by remember { mutableFloatStateOf(1f) }

        // interactive crop overlay (Lightroom-style); hosts on top of everything.
        var cropOverlayOpen by remember { mutableStateOf(false) }
        // draw-on-the-preview mask geometry editor (positions the selected mask on the photo).
        var maskOverlayOpen by remember { mutableStateOf(false) }
        var maskEditIndex by remember { mutableStateOf(0) }
        // eyedropper: sample a color- or luminance-range mask's target by tapping the photo.
        var sampleOverlayOpen by remember { mutableStateOf(false) }
        var sampleMaskIndex by remember { mutableStateOf(0) }
        var sampleLuminanceMode by remember { mutableStateOf(false) }
        var sampleWbMode by remember { mutableStateOf(false) }  // gray-point WB eyedropper (no mask)

        // 100% grain magnifier
        var magnifierOpen by remember { mutableStateOf(false) }
        var magnifierBitmap by remember { mutableStateOf<Bitmap?>(null) }
        var magnifierRendering by remember { mutableStateOf(false) }
        var magnifierStatus by remember { mutableStateOf("") }
        // Tracks the in-flight magnifier render so a rapid re-tap can cancel the previous
        // request (last-tap-wins) instead of racing N full-res renders into magnifierBitmap.
        // Held in a remember box (not Compose state — we never read it during composition).
        val magnifierJobRef = remember { mutableStateOf<kotlinx.coroutines.Job?>(null) }

        // Lightroom-style zoom: the sharp render of the currently-visible region (rendered at
        // ~screen resolution from a native-pixel crop), overlaid on the scaled proxy.
        var roiOverlay by remember { mutableStateOf<RoiOverlay?>(null) }
        val roiJobRef = remember { mutableStateOf<kotlinx.coroutines.Job?>(null) }
        // Cancel any in-flight ROI / magnifier render job when the editor leaves composition
        // (navigation), so a superseded job doesn't keep rendering into a gone Composable.
        DisposableEffect(Unit) {
            onDispose {
                magnifierJobRef.value?.cancel()
                roiJobRef.value?.cancel()
            }
        }
        // Recycle a superseded ROI bitmap once it has left composition (safe — not mid-draw).
        DisposableEffect(roiOverlay) {
            val current = roiOverlay
            onDispose { current?.bitmap?.let { if (!it.isRecycled) it.recycle() } }
        }

        // presets
        var presetList by remember { mutableStateOf<List<String>>(emptyList()) }
        var presetName by remember { mutableStateOf("") }
        var selectedPreset by remember { mutableStateOf("") }

        // Preset "amount" (Lightroom-style): the look active just before a preset was
        // applied (base) and the full applied preset, kept as flat-schema JSON so the
        // amount slider can cross-fade between them. presetAmount is the live mix; null
        // base/full means no preset has been applied since load (slider hidden).
        var presetBaseJson by remember { mutableStateOf<String?>(null) }
        var presetFullJson by remember { mutableStateOf<String?>(null) }
        var presetAmount by remember { mutableFloatStateOf(1f) }

        // Copy/paste settings (Lightroom-style, backlog #10): a session clipboard holding
        // a full params snapshot, so a look dialed on one image can be pasted onto another.
        // Lives in EditorScreen scope: it survives switching the source, but NOT a round-trip
        // to Settings/About (EditorScreen leaves composition there and the remember is dropped).
        var settingsClipboard by remember { mutableStateOf<String?>(null) }

        // Capture the pre-apply look, run [apply], then snapshot the full preset and arm
        // the amount slider at 100%. Used by both built-in and saved-preset apply paths.
        fun applyWithAmount(apply: () -> Unit) {
            val base = runCatching { Presets.toJsonString(state) }.getOrNull()
            apply()
            val full = runCatching { Presets.toJsonString(state) }.getOrNull()
            if (base != null && full != null) {
                presetBaseJson = base; presetFullJson = full; presetAmount = 1f
            }
        }

        // active adjustment category (null = panel closed); hoisted to AppRoot so it
        // survives navigation to Settings/About and back.
        var activeCategory by activeCategoryState

        fun refreshPresets() { presetList = Presets.list(ctx) }

        // --- non-destructive recipe (sidecar) layer ---
        var recipeReady by remember { mutableStateOf(false) }
        var hasRecipe by remember { mutableStateOf(false) }
        var defaultsJson by remember { mutableStateOf<String?>(null) }
        val snackbarHost = remember { SnackbarHostState() }
        val recipeKey = remember(sourceUri) { Recipes.keyFor(sourceUri) }

        // --- double-back-to-exit on the root editor ---
        var backArmed by remember { mutableStateOf(false) }

        // --- in-session undo / redo edit history ---
        // A snapshot = the full preset/recipe JSON (Presets.toJsonString — covers every
        // ParamsState field incl. raw WB/temp/tint) PLUS the editor-local manual rotation.
        val editHistory = remember { EditHistory() }
        // The last SETTLED snapshot we committed. Capture coalescing pushes THIS (the
        // pre-edit state) onto undo when a newer settled state differs (see the capture
        // effect below), so one slider drag (which settles once) == one undo step.
        var committedSnapshot by remember { mutableStateOf<EditSnapshot?>(null) }
        // Set true while we are programmatically restoring a snapshot (undo/redo). The
        // capture effect skips one settle cycle so the restore is NOT recorded as a new
        // edit — without this the restored state would push itself and we'd never escape.
        var restoring by remember { mutableStateOf(false) }

        // Build a snapshot of the CURRENT live editing state (+ manual rotation).
        fun snapshotNow(): EditSnapshot =
            EditSnapshot(Presets.toJsonString(state), rotation.degrees)

        // Restore a snapshot into the live state: decode params (shared preset schema),
        // re-apply rotation, then bump previewTick — mirrors the recipe restore-on-open
        // path. `restoring` guards the capture effect against recording this mutation.
        fun applySnapshot(snap: EditSnapshot) {
            restoring = true
            runCatching { Presets.decode(org.json.JSONObject(snap.paramsJson), state) }
            rotation = SourceRotation.fromDegrees(snap.rotationDegrees)
            committedSnapshot = snap
            previewTick++
        }

        fun doUndo() {
            val target = editHistory.undo(snapshotNow()) ?: return
            applySnapshot(target)
            status = "undo"
        }

        fun doRedo() {
            val target = editHistory.redo(snapshotNow()) ?: return
            applySnapshot(target)
            status = "redo"
        }

        // One-time engine init. The engine is a process-scoped singleton (see
        // EngineHolder): immutable + thread-safe after construction, so it is reused
        // across Activity recreations instead of being re-created and leaked on every
        // configuration change. EngineHolder also handles the AAssetManager-then-extract
        // fallback. Created off the main thread (asset wiring is heavy on first call).
        LaunchedEffect(Unit) {
            withContext(Dispatchers.IO) {
                val e = EngineHolder.get(ctx)
                val list = runCatching { e.listProfiles() }.getOrDefault(emptyList())
                StockCatalog.stocks(ctx) // warm the catalog cache
                val presetGroups = runCatching { BuiltInPresets.grouped(ctx) }.getOrDefault(emptyMap())
                withContext(Dispatchers.Main) {
                    engine = e
                    profiles = list
                    settings.applyDefaultsTo(state, list)
                    if (list.isNotEmpty()) {
                        if (state.filmProfile !in list) state.filmProfile = list.first()
                        if (state.printProfile !in list) state.printProfile = list.first()
                    }
                    builtInGroups = presetGroups
                    catalogReady = true
                    refreshPresets()
                    status = "ready · ${list.size} profiles"
                    defaultsJson = Presets.toJsonString(state)
                    recipeReady = true
                    previewTick++
                }
            }
        }

        // --- Non-destructive recipe: restore-on-open ---
        var lastRestoredKey by remember { mutableStateOf<String?>(null) }
        LaunchedEffect(recipeKey, recipeReady) {
            if (!recipeReady) return@LaunchedEffect
            if (recipeKey == null) {
                hasRecipe = false
                // Switched to a keyless source (the demo image): still drop cross-image
                // history and re-baseline once for the demo's current state.
                if (lastRestoredKey != null) {
                    lastRestoredKey = null
                    editHistory.clear()
                    committedSnapshot = null
                    restoring = true
                }
                return@LaunchedEffect
            }
            if (recipeKey == lastRestoredKey) return@LaunchedEffect
            lastRestoredKey = recipeKey
            // New source: undo must never cross images. Drop the history and re-baseline so
            // the just-restored/default state for THIS image is the empty-history baseline
            // (canUndo=false). `restoring` makes the next capture settle adopt the new
            // baseline without recording it as an edit.
            editHistory.clear()
            committedSnapshot = null
            restoring = true
            val restored = runCatching { Recipes.load(ctx, recipeKey, state) }.getOrDefault(false)
            hasRecipe = restored
            if (restored) {
                // Restore the persisted manual rotation (EXIF baseline is re-derived on load).
                rotation = runCatching { Recipes.loadRotation(ctx, recipeKey) }
                    .getOrDefault(SourceRotation.NONE)
                previewTick++
                snackbarHost.currentSnackbarData?.dismiss()
                snackbarHost.showSnackbar(
                    message = "Restored saved edit for this image",
                    withDismissAction = true,
                )
            } else {
                Recipes.resetToDefaults(state, settings, profiles)
                rotation = SourceRotation.NONE
                previewTick++
            }
        }

        // One-shot snackbar when a compressed DNG fell back to the system decoder.
        LaunchedEffect(dngFallbackNotice) {
            if (dngFallbackNotice) {
                snackbarHost.currentSnackbarData?.dismiss()
                snackbarHost.showSnackbar(
                    message = "DNG imported via system decoder (display-referred)",
                    withDismissAction = true,
                )
                dngFallbackNotice = false
            }
        }

        // The export runs under a foreground service, and on API 33+ that service's
        // ongoing notification is silently suppressed unless POST_NOTIFICATIONS is
        // granted. The manifest has declared the permission since the service landed,
        // but nothing ever requested it — so on a real install the export notification
        // never appeared at all. Asked in context at the first export, and never
        // blocking: the service's kill-resistance (oom_score_adj 50 vs 700) works
        // whether or not the notification can be drawn.
        val notificationPermission = rememberLauncherForActivityResult(
            ActivityResultContracts.RequestPermission()
        ) { /* granted or denied, the export proceeds either way */ }

        // --- source pickers ---
        val photoPicker = rememberLauncherForActivityResult(
            ActivityResultContracts.PickVisualMedia()
        ) { uri ->
            if (uri != null) {
                sourceUri = uri; sourceKind = SourceKind.PHOTO; sourceName = "picked photo"
                rotation = SourceRotation.NONE
                status = "photo selected"; previewTick++
            }
        }
        val rawPicker = rememberLauncherForActivityResult(
            ActivityResultContracts.OpenDocument()
        ) { uri ->
            if (uri != null) {
                val name = uri.lastPathSegment ?: "raw"
                if (McrawContainer.isMcrawFileName(name)) {
                    // MotionCam RAW-video container: recognized (see McrawContainer /
                    // docs/RESEARCH_MCRAW.md) but native frame decode isn't wired yet, so
                    // don't set a RAW source that would fail — tell the user instead.
                    status = "MotionCam .mcraw clips aren't supported yet (RAW-video import in progress)"
                    scope.launch {
                        snackbarHost.currentSnackbarData?.dismiss()
                        snackbarHost.showSnackbar("MotionCam .mcraw import is coming — single RAW/DNG works today")
                    }
                } else {
                    runCatching {
                        ctx.contentResolver.takePersistableUriPermission(
                            uri, android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION,
                        )
                    }.onFailure { Diag.w("persistable uri permission not granted: ${it.message}") }
                    rotation = SourceRotation.NONE
                    val mime = runCatching { ctx.contentResolver.getType(uri) }.getOrNull()
                    if (isNonRawImage(name, mime)) {
                        // A JPEG/HEIC chosen via the RAW document picker: process it on the normal
                        // photo path rather than forcing it through LibRaw (which would fail, then
                        // fall back to a lossy display-referred decode). RAW/DNG and ambiguous
                        // content URIs (e.g. MIUI document IDs with no extension) fall through to
                        // the RAW path below, so a genuine DNG is never misrouted.
                        sourceUri = uri; sourceKind = SourceKind.PHOTO
                        sourceName = "picked photo"
                        status = "photo selected"; previewTick++
                    } else {
                        sourceUri = uri; sourceKind = SourceKind.RAW
                        sourceName = "RAW: ${name.substringAfterLast('/')}"
                        status = "RAW selected"; previewTick++
                    }
                }
            }
        }
        val presetImporter = rememberLauncherForActivityResult(
            ActivityResultContracts.OpenDocument()
        ) { uri ->
            if (uri != null) {
                // Read the SAF stream off-main; decode (Compose-state write) on main.
                scope.launch {
                    val text = withContext(Dispatchers.IO) {
                        runCatching { Presets.readUri(ctx, uri) }.getOrNull()
                    }
                    if (text == null) { status = "import failed"; return@launch }
                    runCatching { Presets.decode(org.json.JSONObject(text), state) }
                        .onSuccess { status = "preset imported"; previewTick++ }
                        .onFailure { status = "import failed: ${it.message}" }
                }
            }
        }
        val presetExporter = rememberLauncherForActivityResult(
            ActivityResultContracts.CreateDocument("application/json")
        ) { uri ->
            if (uri != null) {
                scope.launch {
                    // Serialize on main (live Compose-state read); write off-main.
                    val json = runCatching { Presets.toJsonString(state) }.getOrNull()
                    if (json == null) { status = "export failed"; return@launch }
                    val r = withContext(Dispatchers.IO) { runCatching { Presets.exportJson(ctx, uri, json) } }
                    r.onSuccess { status = "preset exported" }
                        .onFailure { status = "export failed: ${it.message}" }
                }
            }
        }
        val lutExporter = rememberLauncherForActivityResult(
            ActivityResultContracts.CreateDocument("*/*")
        ) { uri ->
            val text = pendingLutText
            if (uri != null && text != null) {
                scope.launch {
                    // SAF write off-main; status (Compose state) lands back on main.
                    val r = withContext(Dispatchers.IO) { runCatching { saveTextToUri(ctx, uri, text) } }
                    r.onSuccess { status = "LUT saved" }
                        .onFailure { status = "LUT save failed: ${it.message}" }
                }
            }
            pendingLutText = null
        }

        // Decode the current source to a LinearImage capped to [maxEdge], applying the
        // EXIF orientation baseline THEN the user's manual rotate steps so imports appear
        // upright in both the preview and the export. The demo image has no EXIF.
        //
        // RAW/DNG: a compressed (lossy-JPEG / JPEG-XL) Samsung/Pixel Expert-RAW DNG makes
        // LibRaw throw RawDecodeException; we fall back to the platform ImageDecoder
        // (display-referred) and flag a one-shot snackbar. EXIF is then applied to the
        // fallback bitmap too.
        suspend fun loadSource(maxEdge: Int): LinearImage = withContext(Dispatchers.IO) {
            val uri = sourceUri
            // EXIF baseline read once from the original source stream.
            val exif = if (uri != null && sourceKind != SourceKind.DEMO) {
                readExifOrientation(ctx, uri)
            } else {
                ExifOrientation.NONE
            }
            // applyExifBaseline guards the RAW double-rotation case: LibRaw already
            // uprights its linear output (it honours the DNG Orientation tag during
            // dcraw_process), so applying the file's EXIF on top would double-rotate.
            // The platform ImageDecoder fallback does NOT upright by default for DNG, so
            // the fallback path opts back in.
            var applyExifBaseline = sourceKind != SourceKind.RAW
            val img = when (sourceKind) {
                SourceKind.RAW -> try {
                    decodeRawToLinear(
                        ctx, uri!!, state.rawWhiteBalance,
                        state.rawTemperature.toDouble(), state.rawTint.toDouble(), maxEdge,
                    )
                } catch (e: RawDecodeException) {
                    when (e.status) {
                        DecodeStatus.LOSSY_JPEG_DNG,
                        DecodeStatus.FILE_UNSUPPORTED -> {
                            // Compressed Expert-RAW DNG: platform decoder fallback, which
                            // does NOT auto-upright DNGs -> apply the EXIF baseline.
                            dngFallbackNotice = true
                            applyExifBaseline = true
                            decodeViaPlatform(ctx, uri!!, maxEdge)
                        }
                        else -> {
                            // Any other LibRaw decode verdict: still try the downsampling
                            // platform decoder rather than failing the load outright.
                            dngFallbackNotice = true
                            applyExifBaseline = true
                            decodeViaPlatform(ctx, uri!!, maxEdge)
                        }
                    }
                } catch (e: RuntimeException) {
                    // Defensive: any other native decode failure on a DNG/RAW still tries
                    // the platform decoder before giving up (e.g. an untyped error on a
                    // lossy DNG with no typed status).
                    dngFallbackNotice = true
                    applyExifBaseline = true
                    decodeViaPlatform(ctx, uri!!, maxEdge)
                }
                SourceKind.PHOTO -> decodeToLinearProPhoto(ctx, uri!!, maxEdge)
                SourceKind.DEMO -> syntheticLinearImage(256)
            }
            // EXIF baseline (when applicable) first, then the user's manual rotate steps.
            val based = if (applyExifBaseline) img.applyExif(exif) else img
            based.rotated(rotation).also {
                // Creative white balance: bake a pre-engine Bradford CAT into the linear input here
                // (parity-free — engine/spektra-core is untouched). No-op when neutral; it's part of
                // the decode-cache key so a change re-decodes, like raw temp/tint.
                if (!CreativeWhiteBalance.isNeutral(state.creativeWbTemp, state.creativeWbTint)) {
                    CreativeWhiteBalance.applyInPlace(
                        it.data, it.width * it.height,
                        CreativeWhiteBalance.matrix(state.creativeWbTemp, state.creativeWbTint),
                    )
                }
                // "Balance to film stock" (virtual 85-filter): adapt the D50 input to the film's reference
                // illuminant so a tungsten stock renders neutral. Same parity-free bake as Creative WB;
                // keyed on filmProfile + the toggle in the decode cache below. Gated on isMeaningful so
                // daylight stocks (already neutral) are a true no-op — no shift, no extra decode.
                if (state.balanceToFilmStock && FilmStockBalance.isMeaningful(ctx, state.filmProfile)) {
                    CreativeWhiteBalance.applyInPlace(
                        it.data, it.width * it.height,
                        FilmStockBalance.matrix(ctx, state.filmProfile),
                    )
                }
                // Breadcrumb: source KIND + result dims only (no URI/path — see Diag policy).
                Diag.i("decode kind=${sourceKind.name} ${it.width}x${it.height} maxEdge=$maxEdge")
            }
        }

        // PERF: proxy-source loader used ONLY by the interactive preview path. Consults the
        // single-entry DecodedSourceCache keyed by the decode-affecting inputs (URI + kind +
        // RAW WB/temp/tint + manual rotation + target edge). On a hit we re-feed the SAME
        // cached LinearImage to the engine — proven safe because spk_simulate/_preview take
        // `const spk_image* in` and only read it (see DecodedSourceCache for the full proof),
        // so no defensive copy is required. On a miss we run the full loadSource() decode and
        // store the result, dropping any previous cached source (one entry only). When ONLY
        // look/film params change (the common case while editing sliders) the key is unchanged
        // and we skip the expensive LibRaw/bitmap decode + linearization + rotation entirely.
        // EXPORT calls loadSource(EXPORT_MAX_EDGE_PX) directly — full-resolution, never cached;
        // the 100% magnifier calls loadSource(MAX_EDGE_PX) (a capped whole-image load it then
        // crops). Neither uses this preview cache.
        suspend fun loadSourceCachedForPreview(maxEdge: Int): LinearImage {
            // Profile id when "balance to film stock" is on (its CAT is baked into the decode), "" off.
            val filmBalance =
                if (state.balanceToFilmStock && FilmStockBalance.isMeaningful(ctx, state.filmProfile)) state.filmProfile else ""
            fun cacheGet() = sourceCache.get(
                uri = sourceUri?.toString(), kind = sourceKind.name,
                whiteBalance = state.rawWhiteBalance, temperature = state.rawTemperature,
                tint = state.rawTint, creativeTemp = state.creativeWbTemp, creativeTint = state.creativeWbTint,
                filmBalance = filmBalance, rotationDegrees = rotation.degrees, maxEdge = maxEdge,
            )
            cacheGet()?.let { return it }
            // Single-flight the decode in the stable lifecycleScope so two renders that both miss
            // (e.g. the settle pass + the GPU LUT bake) decode once, not twice.
            val key = listOf(
                sourceUri?.toString(), sourceKind.name, state.rawWhiteBalance,
                state.rawTemperature, state.rawTint, state.creativeWbTemp, state.creativeWbTint,
                filmBalance, rotation.degrees, maxEdge,
            ).joinToString("|")
            return previewDecodeFlight.run(key, scope) {
                cacheGet() ?: loadSource(maxEdge).also { decoded ->
                    sourceCache.put(
                        uri = sourceUri?.toString(), kind = sourceKind.name,
                        whiteBalance = state.rawWhiteBalance, temperature = state.rawTemperature,
                        tint = state.rawTint, creativeTemp = state.creativeWbTemp, creativeTint = state.creativeWbTint,
                        filmBalance = filmBalance, rotationDegrees = rotation.degrees, maxEdge = maxEdge,
                        img = decoded,
                    )
                }
            }
        }

        // PERF/OOM: cached counterpart of loadSourceCachedForPreview for the MAX_EDGE_PX zoom/
        // magnifier proxy, backed by the dedicated zoomSourceCache. The Lightroom zoom (renderRoi)
        // and the 100% magnifier both crop this whole-image proxy; without the cache each gesture
        // settle re-decoded it (LibRaw + 36MB managed alloc) and the pileup OOM'd the ART heap.
        // Same read-only-reuse proof as the preview cache (the engine treats `in` as const), so the
        // single cached LinearImage is safely re-fed to every crop.
        suspend fun loadSourceCachedForZoom(maxEdge: Int): LinearImage {
            // Profile id when "balance to film stock" is on (its CAT is baked into the decode), "" off.
            val filmBalance =
                if (state.balanceToFilmStock && FilmStockBalance.isMeaningful(ctx, state.filmProfile)) state.filmProfile else ""
            fun cacheGet() = zoomSourceCache.get(
                uri = sourceUri?.toString(), kind = sourceKind.name,
                whiteBalance = state.rawWhiteBalance, temperature = state.rawTemperature,
                tint = state.rawTint, creativeTemp = state.creativeWbTemp, creativeTint = state.creativeWbTint,
                filmBalance = filmBalance, rotationDegrees = rotation.degrees, maxEdge = maxEdge,
            )
            cacheGet()?.let { return it }
            // Single-flight the 2048px zoom/magnifier decode in the stable lifecycleScope: a
            // cancelled ROI render's native LibRaw decode keeps running, so the next gesture awaits
            // THAT decode instead of starting an overlapping one (the "5 decodes per pinch" drain).
            val key = listOf(
                sourceUri?.toString(), sourceKind.name, state.rawWhiteBalance,
                state.rawTemperature, state.rawTint, state.creativeWbTemp, state.creativeWbTint,
                filmBalance, rotation.degrees, maxEdge,
            ).joinToString("|")
            return zoomDecodeFlight.run(key, scope) {
                cacheGet() ?: loadSource(maxEdge).also { decoded ->
                    zoomSourceCache.put(
                        uri = sourceUri?.toString(), kind = sourceKind.name,
                        whiteBalance = state.rawWhiteBalance, temperature = state.rawTemperature,
                        tint = state.rawTint, creativeTemp = state.creativeWbTemp, creativeTint = state.creativeWbTint,
                        filmBalance = filmBalance, rotationDegrees = rotation.degrees, maxEdge = maxEdge,
                        img = decoded,
                    )
                }
            }
        }

        // 100% grain magnifier: render a real FULL-RESOLUTION crop around a tapped point.
        //
        // PERF/RACE: each tap previously launched an independent coroutine with no
        // cancellation, so rapid taps ran several full-res simulate()s in parallel and
        // raced their results into magnifierBitmap (last-write-wins, wasteful, and an
        // orphaned bitmap per superseded run). We now cancel the prior in-flight render
        // before starting a new one (last-tap-wins), and the result is only published if
        // the coroutine is still active — a cancelled run recycles its own bitmap instead
        // of writing it, so a superseded render can never leak or land on screen.
        fun openMagnifier(nx: Float, ny: Float) {
            val e = engine ?: return
            magnifierJobRef.value?.cancel()
            magnifierOpen = true
            magnifierBitmap = null
            magnifierRendering = true
            magnifierStatus = "rendering 100% crop…"
            magnifierJobRef.value = scope.launch {
                val result = runCatching {
                    withContext(Dispatchers.Default) {
                        val full = loadSourceCachedForZoom(MAX_EDGE_PX)
                        val crop = cropLinearImage(full, nx, ny, MAGNIFIER_CROP_PX)
                        // Effective film format so the crop's pixel_size_um matches the proxy it was
                        // cut from (the crop spans only cropFrac of the frame). Without it the engine
                        // treats the crop as a whole 35mm frame and grain/halation (µm-based) render
                        // too weak even at 1:1. See toParams(filmFormatMmOverride).
                        val cropFrac = maxOf(crop.width, crop.height).toFloat() /
                            maxOf(full.width, full.height).coerceAtLeast(1)
                        // SimResult holds a native off-heap buffer; close() frees it once the
                        // bitmap copy is made.
                        e.simulate(
                            crop,
                            state.toParams(filmFormatMmOverride = state.filmFormatMm * cropFrac),
                        ).use { res ->
                            simResultToBitmapGraded(res, state.savingCctfEncoding, state.saturation, state.vibrance, state.gamutCompress, state.localAdjustments)
                        }
                    }
                }
                if (!isActive) {
                    // Superseded by a newer tap (this job was cancelled): drop our render so
                    // it neither overwrites the latest request nor leaks an orphaned bitmap.
                    result.getOrNull()?.let { if (!it.isRecycled) it.recycle() }
                    return@launch
                }
                result.onSuccess {
                    magnifierBitmap = it
                    magnifierStatus = "${it.width}×${it.height}px · 1:1 full-res render"
                }.onFailure {
                    magnifierStatus = "crop render failed: ${it.message}"
                }
                magnifierRendering = false
            }
        }

        // Lightroom-style zoom: render the currently-visible region (a native-pixel crop of the
        // source) at ~screen resolution via the FAST preview path, then overlay it sharply on the
        // graphicsLayer-scaled proxy. Modeled on openMagnifier's last-wins cancellation. Memory is
        // bounded by ROI_RENDER_MAX_PX (a few MB), independent of source megapixels — this is the
        // Lightroom "smart preview" strategy, so it sidesteps the full-res OOM entirely. Suppressed
        // while cropping or comparing (those branches own the gestures / have no zoom).
        fun renderRoi(roi: RoiRect) {
            val e = engine ?: return
            if (cropOverlayOpen || maskOverlayOpen || sampleOverlayOpen || compareMode) return
            roiJobRef.value?.cancel()
            roiJobRef.value = scope.launch {
                val result = runCatching {
                    val full = loadSourceCachedForZoom(MAX_EDGE_PX)
                    val cw = (roi.wN * full.width).toInt().coerceAtLeast(8)
                    val ch = (roi.hN * full.height).toInt().coerceAtLeast(8)
                    val crop = withContext(Dispatchers.Default) {
                        cropLinearImageRect(full, roi.cxN, roi.cyN, cw, ch)
                    }
                    // Effective film format so the crop's pixel_size_um matches the proxy (the crop
                    // spans only cropFrac of the frame); keeps grain/halation at the right strength
                    // when zoomed instead of treating the crop as a whole frame.
                    val cropFrac = maxOf(crop.width, crop.height).toFloat() /
                        maxOf(full.width, full.height).coerceAtLeast(1)
                    suspend fun renderAt(edge: Int): Bitmap = withContext(Dispatchers.Default) {
                        e.simulatePreview(
                            crop,
                            state.toParams(
                                previewMaxSizeOverride = edge,
                                filmFormatMmOverride = state.filmFormatMm * cropFrac,
                            ),
                        ).use { res -> simResultToBitmapGraded(res, state.savingCctfEncoding, state.saturation, state.vibrance, state.gamutCompress, state.localAdjustments) }
                    }
                    // DRAFT pass: a fast low-res sharp crop so the zoomed region resolves ~5x sooner,
                    // then refine to ROI_RENDER_MAX_PX. Every bitmap is published (then owned by the
                    // DisposableEffect(roiOverlay), which recycles the prior one) OR recycled here —
                    // so a cancel mid-flight never leaks one.
                    val draft = renderAt(ROI_DRAFT_MAX_PX)
                    if (isActive) {
                        roiOverlay = RoiOverlay(draft, roi.cxN, roi.cyN, roi.wN, roi.hN)
                    } else {
                        if (!draft.isRecycled) draft.recycle()
                        return@runCatching null
                    }
                    renderAt(ROI_RENDER_MAX_PX)
                }
                if (!isActive) {
                    // Superseded (cancelled): drop the final render (the draft, if published, stays
                    // and is recycled by the DisposableEffect when the next overlay replaces it).
                    result.getOrNull()?.let { if (!it.isRecycled) it.recycle() }
                    return@launch
                }
                // On success publish the sharp final; the prior overlay (the draft) is recycled by
                // the DisposableEffect(roiOverlay) below. On failure keep the scaled proxy.
                result.onSuccess { bmp ->
                    if (bmp != null) roiOverlay = RoiOverlay(bmp, roi.cxN, roi.cyN, roi.wN, roi.hN)
                }
            }
        }

        fun clearRoi() {
            roiJobRef.value?.cancel()
            roiOverlay = null  // bitmap recycled by DisposableEffect(roiOverlay)
        }

        // Everything that feeds the NATIVE render for a fit preview at [fullEdge]:
        // the engine-param snapshot + the decode key. Reads Compose state — call on
        // the main thread. Grade inputs are deliberately absent (see GradeCache).
        fun gradeCacheKey(fullEdge: Int): GradeCache.Key {
            val filmBalance =
                if (state.balanceToFilmStock && FilmStockBalance.isMeaningful(ctx, state.filmProfile)) state.filmProfile else ""
            return GradeCache.Key(
                engineParams = state.toParams(skipGrainHalation = true),
                decode = GradeCache.DecodeKey(
                    uri = sourceUri?.toString(), kind = sourceKind.name,
                    whiteBalance = state.rawWhiteBalance, temperature = state.rawTemperature,
                    tint = state.rawTint, creativeTemp = state.creativeWbTemp,
                    creativeTint = state.creativeWbTint, filmBalance = filmBalance,
                    rotationDegrees = rotation.degrees, maxEdge = fullEdge,
                ),
            )
        }

        // Live DRAFT pass — the Lightroom-style render-system "port": on EVERY edit (a slider drag,
        // a preset/dropdown/toggle, a rotation), paint a small fast proxy so the image tracks the
        // change in ~hundreds of ms instead of sitting on the stale frame until the full settle
        // render lands ~1s later. Conflated (snapshotFlow.collect, NOT collectLatest): an in-flight
        // draft is never cancelled — it finishes, then immediately re-renders the latest params
        // (snapshotFlow conflates the bumps that arrived meanwhile), giving back-to-back updates.
        // Cheap by construction: it renders ONLY when the full-edge proxy is already decoded (a
        // cache peek — never decodes here, so it cannot race the settle pass's first decode) and
        // just asks the engine for a smaller DRAFT_RENDER_MAX_PX pass. Quiet: it touches only
        // `preview`, never status/previewBusy; the crisp full pass still lands on settle below.
        LaunchedEffect(Unit) {
            snapshotFlow { previewTick }.collect {
                val e = engine ?: return@collect
                if (cropOverlayOpen || maskOverlayOpen || sampleOverlayOpen || compareMode) return@collect
                // Lightroom's ICBSliderTrackingBegin/End gate. `interacting` has been
                // written by SliderInteraction since the widget shipped but was never
                // read, so the draft pass fired on EVERY edit — including discrete ones
                // (a switch, a dropdown, applying a preset), where it buys nothing: the
                // crisp settle pass is already only 500 ms away, so the draft just burns
                // a render and flashes a soft frame before the sharp one lands.
                //
                // Reading it here is what the widget's own doc comment always described:
                // draft while a slider is actually moving, straight to crisp otherwise.
                if (!interacting.value) return@collect
                val fullEdge = state.previewMaxSize.coerceAtLeast(256)
                // Grade-only edit? Re-grade the retained full-quality settle result in
                // pure Kotlin — zero native work, and it beats a low-res draft on quality.
                gradeCache.lookup(gradeCacheKey(fullEdge))?.let { pristine ->
                    runCatching {
                        withContext(Dispatchers.Default) {
                            gradeBufferToBitmap(pristine.scratchCopy(), pristine.width,
                                pristine.height, pristine.colorSpace, state.savingCctfEncoding,
                                state.saturation, state.vibrance, state.gamutCompress,
                                state.localAdjustments)
                        }
                    }.onSuccess { bmp -> withContext(Dispatchers.Main) { preview = bmp } }
                        .onFailure { Diag.w("render mode=draft failed: ${it.message}") }
                    return@collect
                }
                // Progressive ladder rung. The edge is a setting rather than a constant
                // so the coarse/fine trade can be swept on a real device: lower = the
                // image tracks the finger more closely, higher = the live frame is closer
                // to what the settle pass will show. Defaults to DRAFT_RENDER_MAX_PX, so
                // an untouched install renders exactly as before.
                val draftEdge = minOf(settings.draftRenderMaxPx, fullEdge)
                if (draftEdge >= fullEdge) return@collect       // no meaningful step-down to draft
                val proxy = sourceCache.get(
                    uri = sourceUri?.toString(), kind = sourceKind.name,
                    whiteBalance = state.rawWhiteBalance, temperature = state.rawTemperature,
                    tint = state.rawTint, creativeTemp = state.creativeWbTemp, creativeTint = state.creativeWbTint,
                    filmBalance = if (state.balanceToFilmStock && FilmStockBalance.isMeaningful(ctx, state.filmProfile)) state.filmProfile else "",
                    rotationDegrees = rotation.degrees, maxEdge = fullEdge,
                ) ?: return@collect                             // proxy not cached yet — settle owns the decode
                runCatching {
                    withContext(Dispatchers.Default) {
                        e.simulatePreview(
                            proxy,
                            state.toParams(previewMaxSizeOverride = draftEdge, skipGrainHalation = true),
                        ).use { res -> simResultToBitmapGraded(res, state.savingCctfEncoding, state.saturation, state.vibrance, state.gamutCompress, state.localAdjustments) }
                    }
                }.onSuccess { bmp -> withContext(Dispatchers.Main) { preview = bmp } }
                    .onFailure { Diag.w("render mode=draft failed: ${it.message}") }
            }
        }

        // Crisp FINAL preview render on settle: re-runs whenever params, source or rotation
        // change, after a debounce so it lands once the user pauses. The live DRAFT effect above
        // keeps the image moving during the edit (Lightroom's loupe), so this pass goes straight
        // to the full-resolution result instead of a separate coarse pass.
        LaunchedEffect(previewTick) {
            val e = engine ?: return@LaunchedEffect
            // Settle debounce, raised from 350ms. Each preview render maxes all CPU cores for
            // ~1s, so waiting a bit longer for the user to pause means fewer renders that start
            // then get cancelled mid-flight on the next edit ("coroutine scope left the
            // composition" in the logcat) — wasted all-core CPU that drains battery.
            delay(500)
            previewBusy = true
            renderErr = null
            status = "rendering preview…"
            val renderStart = System.currentTimeMillis()
            val fullEdge = state.previewMaxSize.coerceAtLeast(256)
            // Key built on MAIN (reads Compose state); the same snapshot renders below,
            // so the key and the render can never disagree.
            val cacheKey = gradeCacheKey(fullEdge)
            val cached = gradeCache.lookup(cacheKey)
            val prevBefore = beforePreview
            val result = runCatching {
                withContext(Dispatchers.Default) {
                    if (cached != null && prevBefore != null) {
                        // Grade-only edit (identical engine inputs): re-grade the retained
                        // pristine engine result — ZERO native work. The ungraded `before`
                        // is unchanged by construction.
                        prevBefore to gradeBufferToBitmap(cached.scratchCopy(), cached.width,
                            cached.height, cached.colorSpace, state.savingCctfEncoding,
                            state.saturation, state.vibrance, state.gamutCompress,
                            state.localAdjustments)
                    } else {
                        decoding = true
                        // The live DRAFT effect above already paints a fast low-res proxy during the
                        // edit, so this settle pass goes straight to the crisp full render (no separate
                        // coarse pass / extra fresh decode). Cached proxy source — re-decodes only when
                        // a decode-affecting key (URI/kind/WB/temp/tint/rotation/edge) changed;
                        // look-param edits reuse it.
                        val image = loadSourceCachedForPreview(fullEdge)
                        decoding = false
                        val before = linearToDisplayBitmap(image)
                        // Fit preview skips grain/halation (the user's "grain at 100%" choice): they
                        // are rendered by the zoom ROI, the magnifier and export, never the fit settle.
                        // Render with cacheKey.engineParams — the exact snapshot the key hashed.
                        val after = e.simulatePreview(image, cacheKey.engineParams).use { res ->
                            // Retain the PRISTINE engine output BEFORE the grade mutates res.data.
                            gradeCache.store(cacheKey, res.data, res.width, res.height, res.colorSpace)
                            simResultToBitmapGraded(res, state.savingCctfEncoding, state.saturation, state.vibrance, state.gamutCompress, state.localAdjustments)
                        }
                        before to after
                    }
                }
            }
            decoding = false
            result.onSuccess { (before, after) ->
                beforePreview = before; preview = after
                lastRenderMs = System.currentTimeMillis() - renderStart
                Diag.i("render mode=preview ${after.width}x${after.height} ${after.width * after.height}px ${lastRenderMs}ms")
                renderErr = null
                status = "preview ready"
            }.onFailure {
                Diag.w("render mode=preview failed after ${System.currentTimeMillis() - renderStart}ms: ${it.message}")
                renderErr = it.message?.take(60)
                status = "preview error: ${it.message}"
            }
            previewBusy = false
        }

        // GPU LUT preview feed (experimental, default OFF): after the CPU preview settles,
        // bake a 33³ .cube of the current look and keep it + the linear proxy for the GPU
        // path. The proxy decode is a cache hit (the main effect already loaded it), so this
        // adds only the LUT bake. Cancelled/replaced cleanly when previewTick advances.
        LaunchedEffect(previewTick, gpuEnabled) {
            if (!gpuEnabled) { gpuLut = null; gpuProxy = null; gpuGain = 1f; return@LaunchedEffect }
            val e = engine ?: return@LaunchedEffect
            delay(380)
            runCatching {
                withContext(Dispatchers.Default) {
                    val img = loadSourceCachedForPreview(state.previewMaxSize.coerceAtLeast(256))
                    val params = state.toParams()
                    // SHAPER_SRGB is REQUIRED here, not optional: LutGpuPreview's shader
                    // always indexes through the sRGB transfer, so a LUT baked on the linear
                    // lattice would be sampled at the wrong cell entirely — mid-grey 0.18
                    // looking up at 0.46, i.e. overexposed and flat. The only unshaped bake
                    // is the user-facing .cube export, which no shader consumes.
                    val lut = CubeLut.parse(
                        e.bakeCubeLut(params, 33, SpektraEngine.SHAPER_SRGB)
                    )
                    // The bake emits the pointwise transform at UNITY gain (a 3D LUT
                    // cannot carry auto-exposure), so meter the SAME proxy through the
                    // engine's own metering and hand the gain to the shader. Without it
                    // the GPU preview renders dark with lifted shadows — the scene lands
                    // in the film curve's toe. Metering is cheap next to the bake.
                    val gain = e.exposureGain(img, params)
                    Triple(img, lut, gain)
                }
            }.onSuccess { (img, lut, gain) ->
                if (lut != null) {
                    gpuProxy = img; gpuLut = lut; gpuGain = gain
                    Diag.i("gpu lut baked ${lut.size}^3 gain=%.3f — fit preview runs on GPU".format(gain))
                } else {
                    Diag.w("gpu lut parse failed -> CPU preview")
                }
            }.onFailure { Diag.w("gpu lut bake failed: ${it.message} -> CPU preview") }
        }

        // MEMORY (#2): `preview` and `beforePreview` are intentionally LEFT TO GC, not recycled
        // on swap. Recycling them is NOT provably safe: `preview` is consumed by
        // PreviewHistogramOverlay (Viewer.kt), which reads its pixels with getPixels() from a
        // background coroutine
        // (LaunchedEffect(bitmap){ withContext(Dispatchers.Default){ computeHistogram(bitmap) } }).
        // computeHistogram is a tight, non-suspending loop, so when `preview` swaps mid-compute
        // the previous coroutine is NOT actually cancelled (cancellation is cooperative) and runs
        // to completion on the OLD bitmap. Recycling that bitmap on the recomposition swap would
        // race the still-running getPixels() -> "Can't call getPixels() on a recycled bitmap"
        // crash, and a check-then-recycle can't close that window without coordinating with the
        // histogram coroutine. Per the audit's correctness-over-aggressiveness rule these orphaned
        // ARGB_8888 bitmaps (incl. the full-res export bitmap, which becomes `preview`) are left
        // for the GC, which reclaims them promptly under the memory pressure that actually
        // matters. Only the magnifier crop — which has NO async reader — is recycled
        // deterministically (see the magnifier overlay's DisposableEffect below).

        // re-trigger preview on any change to the params snapshot / source / rotation.
        //
        // PERF: wrap toParams() in derivedStateOf so the full SpektraParams tree (+ nested
        // Grain/Halation/DirCouplers/Glare/Diffusion) is allocated ONLY when a param state
        // actually changes — not on every recomposition of EditorScreen. derivedStateOf reads
        // exactly the same param states toParams() does, so it invalidates (and re-allocates)
        // iff one of those states changes, and otherwise returns the cached instance. The
        // resulting SpektraParams is structurally compared by LaunchedEffect's key machinery
        // (it is a data class), so a relaunch fires for every field that can alter the render —
        // identical trigger behaviour to the old per-frame snapshot, with no per-frame alloc.
        val snapshot by remember { derivedStateOf { state.toParams() } }
        LaunchedEffect(snapshot, sourceUri, sourceKind, rotation,
            state.rawWhiteBalance, state.rawTemperature, state.rawTint,
            state.creativeWbTemp, state.creativeWbTint, state.balanceToFilmStock,
            state.localAdjustments) { previewTick++ }

        // --- Non-destructive recipe: debounced auto-save ---
        LaunchedEffect(snapshot, recipeKey, recipeReady, defaultsJson, rotation,
            state.rawWhiteBalance, state.rawTemperature, state.rawTint,
            state.creativeWbTemp, state.creativeWbTint, state.balanceToFilmStock,
            state.localAdjustments) {
            if (!recipeReady || recipeKey == null) return@LaunchedEffect
            delay(700)
            val current = runCatching { Presets.toJsonString(state) }.getOrNull()
            // A non-NONE manual rotation is itself an edit worth persisting, even when the
            // params are otherwise default — so only treat as "pristine" if rotation is NONE.
            if (current != null && current == defaultsJson && rotation == SourceRotation.NONE) {
                // exists() is a file stat — keep it off main with the delete.
                withContext(Dispatchers.IO) {
                    if (Recipes.exists(ctx, recipeKey)) Recipes.delete(ctx, recipeKey)
                }
                hasRecipe = false
                return@LaunchedEffect
            }
            runCatching {
                // Reuse the main-thread serialization from above (torn-snapshot safety);
                // only the envelope build + file write cross to IO.
                val paramsJson = current ?: Presets.toJsonString(state)
                withContext(Dispatchers.IO) {
                    Recipes.saveJson(ctx, recipeKey, paramsJson, sourceName, rotation.degrees)
                }
            }.onSuccess { hasRecipe = true }
                .onFailure { Diag.w("recipe auto-save failed: ${it.message}") }
        }

        // --- Undo/redo capture (debounced coalescing) ---
        // Keyed on the same settle inputs as the auto-save effect, so it re-arms on every
        // edit and only the LAST change in a burst survives the delay (a slider drag fires
        // many state changes but they all collapse into ONE surviving settle). After the
        // state settles we compare the new snapshot to the last COMMITTED one:
        //   • restoring == true  -> this settle is the programmatic undo/redo restore;
        //     don't record it, just clear the flag (applySnapshot already moved the
        //     committed pointer). This is what breaks the feedback loop.
        //   • committedSnapshot == null -> first settle for this source/baseline; adopt it
        //     as the baseline WITHOUT pushing (canUndo stays false until a real edit).
        //   • differs from committed -> the user made an edit: push the PREVIOUS committed
        //     snapshot onto the undo stack and advance the committed pointer. One settled
        //     drag => exactly one undo step that returns to the pre-drag state.
        // Uses a slightly shorter delay than auto-save so a quick undo right after an edit
        // still finds the entry recorded; both are debounced independently and key on the
        // same inputs, so this adds no extra previewTick churn or re-decodes.
        LaunchedEffect(snapshot, recipeKey, recipeReady, rotation,
            state.rawWhiteBalance, state.rawTemperature, state.rawTint) {
            if (!recipeReady) return@LaunchedEffect
            delay(500)
            // settleDecision (unit-tested in EditHistoryTest) handles the subtle case where a
            // real edit lands within the restore settle window: it pushes the restored baseline
            // so the edit stays undoable instead of being silently adopted (one-undo-step loss).
            val now = snapshotNow()
            val action = settleDecision(restoring, committedSnapshot, now)
            action.push?.let { editHistory.push(it) }
            committedSnapshot = action.committed
            restoring = false
        }

        // catalog-grouped profile options for the Simulation pickers + Settings.
        val available = profiles.ifEmpty {
            listOf(state.filmProfile, state.printProfile).distinct()
        }
        val filmGroups = remember(available, catalogReady) {
            StockCatalog.optionsFor(ctx, available, forFilm = true).toGroups()
        }
        val printGroups = remember(available, catalogReady) {
            StockCatalog.optionsFor(ctx, available, forFilm = false).toGroups()
        }
        LaunchedEffect(filmGroups, printGroups) { onProfileGroups(filmGroups, printGroups) }

        // --- back handling on the root editor ---
        // 0) crop overlay open -> close it; 1) panel open -> close panel;
        // 2) else double-back-to-exit with one-time hint.
        BackHandler(enabled = cropOverlayOpen) { cropOverlayOpen = false }
        BackHandler(enabled = maskOverlayOpen) { maskOverlayOpen = false }
        BackHandler(enabled = sampleOverlayOpen) { sampleOverlayOpen = false; sampleWbMode = false }
        BackHandler(enabled = !cropOverlayOpen && !maskOverlayOpen && !sampleOverlayOpen && activeCategory != null) { activeCategory = null }
        BackHandler(enabled = !cropOverlayOpen && !maskOverlayOpen && !sampleOverlayOpen && activeCategory == null) {
            if (backArmed) {
                finish()
            } else {
                backArmed = true
                scope.launch {
                    val firstTime = !BackHintStore.hasShown(ctx)
                    if (firstTime) {
                        BackHintStore.markShown(ctx)
                        snackbarHost.currentSnackbarData?.dismiss()
                        snackbarHost.showSnackbar("Press back again to exit")
                    }
                    delay(2000)
                    backArmed = false
                }
            }
        }

        // ============================ LAYOUT ============================
        Box(
            Modifier
                .fillMaxSize()
                .background(SpectraIcons.nearBlackCanvas),
        ) {
            Column(Modifier.fillMaxSize()) {
                // --- TOP BAR ---
                EditorTopBar(
                    canExport = engine != null && !previewBusy && !exporting,
                    exporting = exporting,
                    canUndo = editHistory.canUndo,
                    canRedo = editHistory.canRedo,
                    onUndo = { doUndo() },
                    onRedo = { doRedo() },
                    onOpenSource = {
                        // Open the SAME photo picker the Source panel uses (no app exit).
                        photoPicker.launch(
                            PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)
                        )
                    },
                    onExport = { if (engine != null) showExportSheet = true },
                    onOpenSettings = onOpenSettings,
                    onOpenAbout = onOpenAbout,
                )

                // --- PREVIEW (pinned, weight) ---
                Box(
                    Modifier
                        .fillMaxWidth()
                        .weight(1f)
                        .heightIn(min = 220.dp),
                ) {
                    PreviewRegion(
                        preview = preview,
                        before = beforePreview,
                        busy = previewBusy,
                        decoding = decoding,
                        exporting = exporting,
                        lastRenderMs = lastRenderMs,
                        renderErr = renderErr,
                        compareMode = compareMode,
                        showHistogram = showHistogram,
                        gpuEnabled = gpuEnabled,
                        gpuProxy = gpuProxy,
                        gpuLut = gpuLut,
                        gpuGain = gpuGain,
                        onToggleCompare = { compareMode = !compareMode },
                        onToggleHistogram = { showHistogram = !showHistogram },
                        onRotate = { rotation = rotation.next() },
                        onEditCrop = { cropOverlayOpen = true },
                        onPointPicked = { nx, ny -> openMagnifier(nx, ny) },
                        renderKey = previewTick,
                        roiOverlay = roiOverlay,
                        onRoiSettled = { renderRoi(it) },
                        onRoiCleared = { clearRoi() },
                    )

                // --- ADJUSTMENT PANEL (floating bottom overlay) ---
                // Floated INSIDE the preview Box (aligned to its bottom) so opening/closing a
                // panel no longer resizes the weight(1f) preview every frame. That per-frame
                // resize churned the GLSurfaceView (it forced GPU preview off) and jolted the
                // CPU preview too; a constant-height preview fixes both and unblocks GPU.
                PanelOverlay(visible = activeCategory != null) {
                    AdjustmentPanel(
                        category = activeCategory,
                        onDismiss = { activeCategory = null },
                    ) {
                        CompositionLocalProvider(LocalSliderInteraction provides sliderInteraction) {
                        when (activeCategory) {
                            Category.INPUT -> InputSection(state, onEditCrop = { cropOverlayOpen = true },
                                onPickNeutral = { sampleWbMode = true; sampleOverlayOpen = true })
                            Category.RAW_WB -> ImportRawSection(state, isRaw = sourceKind == SourceKind.RAW,
                                onPickNeutral = { sampleWbMode = true; sampleOverlayOpen = true })
                            Category.SIMULATION -> SimulationSection(
                                s = state,
                                filmGroups = filmGroups,
                                printGroups = printGroups,
                                onOpenFilmCurves = {
                                    onOpenFilmCurves(
                                        state.filmProfile,
                                        StockCatalog.displayName(ctx, state.filmProfile),
                                    )
                                },
                                onOpenPrintCurves = {
                                    onOpenPrintCurves(
                                        state.printProfile,
                                        StockCatalog.displayName(ctx, state.printProfile),
                                    )
                                },
                                onFilmProfileChange = { id ->
                                    val changed = id != state.filmProfile
                                    state.filmProfile = id
                                    if (changed) {
                                        val name = StockCatalog.displayName(ctx, id)
                                        // A reversal (slide) film is most naturally viewed as a
                                        // positive — nudge Slide mode if the print is still showing;
                                        // otherwise offer to drop the previous stock's tweaks.
                                        if (StockCatalog.isReversalFilm(ctx, id) && !state.scanFilm) {
                                            offerSnackbarSuggestion(scope, snackbarHost,
                                                "$name is a slide film — view it as a positive?",
                                                "Slide mode") { state.scanFilm = true }
                                        } else {
                                            offerSnackbarSuggestion(scope, snackbarHost,
                                                "Switched to $name",
                                                "Use its defaults") { state.resetStockCharacter() }
                                        }
                                    }
                                },
                                onPrintProfileChange = { id ->
                                    val changed = id != state.printProfile
                                    state.printProfile = id
                                    if (changed) {
                                        offerSnackbarSuggestion(scope, snackbarHost,
                                            "Switched to ${StockCatalog.displayName(ctx, id)}",
                                            "Use its defaults") { state.resetStockCharacter() }
                                    }
                                },
                            )
                            Category.GRAIN -> GrainSection(state)
                            Category.PREFLASH -> PreflashSection(state)
                            Category.HALATION -> HalationSection(state)
                            Category.COUPLERS -> CouplersSection(state)
                            Category.GLARE -> GlareSection(state)
                            Category.EXPERIMENTAL -> ExperimentalSection(state)
                            Category.TONE_CURVE -> ToneCurveSection(state, preview)
                            Category.MASKS -> MasksSection(
                                state,
                                onEditOnPhoto = { idx -> maskEditIndex = idx; maskOverlayOpen = true },
                                onSampleColor = { idx ->
                                    sampleMaskIndex = idx; sampleLuminanceMode = false; sampleOverlayOpen = true
                                },
                                onSampleLuminance = { idx ->
                                    sampleMaskIndex = idx; sampleLuminanceMode = true; sampleOverlayOpen = true
                                },
                            )
                            Category.DISPLAY -> DisplaySection(state)
                            Category.PRESETS -> PresetPanel(
                                builtInGroups = builtInGroups,
                                onApplyBuiltIn = { p ->
                                    applyWithAmount { BuiltInPresets.apply(p, state) }
                                    status = "applied built-in '${p.name}'"; previewTick++
                                },
                                amount = presetAmount,
                                amountEnabled = presetFullJson != null,
                                onAmountChange = { a ->
                                    presetAmount = a
                                    val base = presetBaseJson; val full = presetFullJson
                                    if (base != null && full != null) {
                                        runCatching {
                                            val blended = PresetAmount.blend(
                                                org.json.JSONObject(base), org.json.JSONObject(full), a,
                                            )
                                            Presets.decode(blended, state)
                                        }.onFailure { Diag.w("preset amount blend failed: ${it.message}") }
                                        previewTick++
                                    }
                                },
                                presets = presetList,
                                selected = selectedPreset,
                                name = presetName,
                                onNameChange = { presetName = it },
                                onSelect = { selectedPreset = it },
                                onSave = {
                                    if (presetName.isNotBlank()) {
                                        val name = presetName
                                        // Serialize on main (toJsonString reads live Compose
                                        // state field-by-field — a torn snapshot off-main);
                                        // only the file write + listing cross to IO.
                                        scope.launch {
                                            val json = Presets.toJsonString(state)
                                            val names = withContext(Dispatchers.IO) {
                                                Presets.saveJson(ctx, name, json); Presets.list(ctx)
                                            }
                                            presetList = names
                                            status = "saved preset '$name'"
                                        }
                                    }
                                },
                                onApply = {
                                    if (selectedPreset.isNotBlank()) {
                                        val name = selectedPreset
                                        // Read the JSON off-main; decode (Compose-state write)
                                        // + applyWithAmount run back on the main thread.
                                        scope.launch {
                                            val text = withContext(Dispatchers.IO) {
                                                runCatching { Presets.read(ctx, name) }.getOrNull()
                                            }
                                            if (text == null) { status = "apply failed"; return@launch }
                                            runCatching {
                                                applyWithAmount { Presets.decode(org.json.JSONObject(text), state) }
                                            }
                                                .onSuccess { status = "applied '$name'"; previewTick++ }
                                                .onFailure { status = "apply failed: ${it.message}" }
                                        }
                                    }
                                },
                                onDelete = {
                                    if (selectedPreset.isNotBlank()) {
                                        val name = selectedPreset
                                        scope.launch {
                                            val names = withContext(Dispatchers.IO) {
                                                Presets.delete(ctx, name); Presets.list(ctx)
                                            }
                                            presetList = names
                                            status = "deleted '$name'"; selectedPreset = ""
                                        }
                                    }
                                },
                                onImport = { presetImporter.launch(arrayOf("application/json", "text/*", "*/*")) },
                                onExport = { presetExporter.launch("spectrafilm_preset.json") },
                                onCopySettings = {
                                    settingsClipboard = runCatching { Presets.toJsonString(state) }.getOrNull()
                                    status = if (settingsClipboard != null) "settings copied" else "copy failed"
                                },
                                canPasteSettings = settingsClipboard != null,
                                onPasteSettings = {
                                    settingsClipboard?.let { clip ->
                                        runCatching { Presets.decode(org.json.JSONObject(clip), state) }
                                            .onSuccess {
                                                // Pasting replaces the whole look, so the preset
                                                // amount blend anchor is now stale — clear it so a
                                                // later slider move can't overwrite the pasted look.
                                                presetBaseJson = null; presetFullJson = null; presetAmount = 1f
                                                status = "settings pasted"; previewTick++
                                            }
                                            .onFailure { status = "paste failed: ${it.message}" }
                                    }
                                },
                                onExportLut = { size, clf ->
                                    val e = engine ?: return@PresetPanel
                                    bakingLut = true; status = "baking ${if (clf) "CLF" else ".cube"} LUT…"
                                    scope.launch {
                                        val r = runCatching {
                                            withContext(Dispatchers.Default) {
                                                // UNSHAPED, deliberately: this file goes to
                                                // other software, and a .cube carries no
                                                // shaper metadata — it must stay in the
                                                // linear domain it advertises.
                                                val cube = e.bakeCubeLut(
                                                    state.toParams(), size,
                                                    SpektraEngine.SHAPER_NONE,
                                                )
                                                if (clf) {
                                                    val lut = CubeLut.parse(cube) ?: error("baked LUT could not be parsed")
                                                    val film = StockCatalog.displayName(ctx, state.filmProfile)
                                                    val print = StockCatalog.displayName(ctx, state.printProfile)
                                                    ClfWriter.write(lut, title = "$film / $print")
                                                } else {
                                                    cube
                                                }
                                            }
                                        }
                                        bakingLut = false
                                        r.onSuccess { text ->
                                            pendingLutText = text
                                            val fileName = lutFileName(
                                                StockCatalog.displayName(ctx, state.filmProfile),
                                                StockCatalog.displayName(ctx, state.printProfile),
                                                size, clf,
                                            )
                                            runCatching { lutExporter.launch(fileName) }
                                                .onFailure { status = "could not open save dialog: ${it.message}" }
                                        }.onFailure {
                                            status = "LUT bake failed: ${it.message}"
                                            Toast.makeText(ctx, "LUT bake failed: ${it.message}", Toast.LENGTH_LONG).show()
                                        }
                                    }
                                },
                                bakingLut = bakingLut,
                            )
                            Category.SOURCE -> SourcePanel(
                                sourceName = sourceName,
                                status = status,
                                showHistogram = showHistogram,
                                onToggleHistogram = { showHistogram = !showHistogram },
                                hasRecipe = hasRecipe,
                                onPickPhoto = {
                                    photoPicker.launch(
                                        PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)
                                    )
                                },
                                onOpenRaw = { rawPicker.launch(arrayOf("*/*")) },
                                onUseDemo = {
                                    sourceUri = null; sourceKind = SourceKind.DEMO
                                    sourceName = "synthetic demo image"; rotation = SourceRotation.NONE
                                    previewTick++
                                },
                                onResetEdits = {
                                    scope.launch {
                                        // Recipe file delete off-main; the state resets that
                                        // follow run back on the main thread.
                                        withContext(Dispatchers.IO) { Recipes.delete(ctx, recipeKey) }
                                        Recipes.resetToDefaults(state, settings, profiles)
                                        presetBaseJson = null; presetFullJson = null; presetAmount = 1f
                                        hasRecipe = false; rotation = SourceRotation.NONE
                                        // Reset clears history: the defaults become the new
                                        // empty-history baseline (you can't undo back across a
                                        // reset). `restoring` makes the next settle adopt it.
                                        editHistory.clear(); committedSnapshot = null; restoring = true
                                        status = "edits reset · recipe cleared"; previewTick++
                                        snackbarHost.currentSnackbarData?.dismiss()
                                        snackbarHost.showSnackbar("Edits reset; saved recipe cleared")
                                    }
                                },
                            )
                            null -> {}
                        }
                        }
                    }
                }
                }

                // --- BOTTOM CATEGORY BAR ---
                CategoryBar(
                    active = activeCategory,
                    onSelect = { cat ->
                        activeCategory = if (activeCategory == cat) null else cat
                    },
                )
            }

            // --- 100% grain magnifier overlay ---
            if (magnifierOpen) {
                // MEMORY: the magnifier crop is a real full-resolution ARGB_8888 render
                // (~512px native here, but it is the only path that decodes at MAX_EDGE_PX
                // before cropping). Recycle it deterministically once the overlay leaves
                // composition (close) or the rendered crop is replaced by a re-render. This
                // DisposableEffect is scoped INSIDE `if (magnifierOpen)`, so onDispose runs
                // only after the overlay is no longer composed/drawn — never a use-after-
                // recycle of the bitmap still on screen. `crop` is the value captured for
                // THIS effect instance; when `magnifierBitmap` changes (e.g. -> null on a
                // re-open) or the overlay closes, the captured previous bitmap is freed.
                val cropToFree = magnifierBitmap
                DisposableEffect(magnifierBitmap) {
                    onDispose {
                        if (cropToFree != null && !cropToFree.isRecycled) cropToFree.recycle()
                    }
                }
                MagnifierOverlay(
                    crop = magnifierBitmap,
                    rendering = magnifierRendering,
                    status = magnifierStatus,
                    onClose = {
                        // Cancel any in-flight crop render so it doesn't resurrect the overlay
                        // state or leak its bitmap (a cancelled job recycles its own result).
                        magnifierJobRef.value?.cancel()
                        magnifierRendering = false
                        magnifierOpen = false
                        magnifierBitmap = null
                    },
                )
            }

            // --- interactive crop overlay (Lightroom-style) ---
            // Only shown once a preview bitmap exists (the crop tool needs the image
            // to draw and to read its aspect ratio).
            val cropBmp = preview
            if (cropOverlayOpen && cropBmp != null) {
                CropOverlay(
                    bitmap = cropBmp,
                    initialCrop = state.crop,
                    initialCenter = state.cropCenter,
                    initialSize = state.cropSize,
                    onRotate = { rotation = rotation.next() },
                    onConfirm = { crop, center, size ->
                        state.crop = crop
                        state.cropCenter = center
                        state.cropSize = size
                        cropOverlayOpen = false
                        previewTick++
                    },
                    onCancel = { cropOverlayOpen = false },
                )
            }

            // --- draw-on-the-preview mask geometry editor ---
            if (maskOverlayOpen && cropBmp != null && maskEditIndex in state.localAdjustments.indices) {
                MaskGeometryOverlay(
                    bitmap = cropBmp,
                    mask = state.localAdjustments[maskEditIndex].mask,
                    onConfirm = { updated ->
                        val list = state.localAdjustments.toMutableList()
                        list[maskEditIndex] = list[maskEditIndex].copy(mask = updated)
                        state.localAdjustments = list
                        maskOverlayOpen = false
                        previewTick++
                    },
                    onCancel = { maskOverlayOpen = false },
                )
            }

            // --- eyedropper: WB gray-point, or a color-/luminance-range mask target ---
            val sampleMask = state.localAdjustments.getOrNull(sampleMaskIndex)?.mask
            val sampleReady = sampleOverlayOpen && cropBmp != null && (
                sampleWbMode ||
                    (sampleMask != null &&
                        (if (sampleLuminanceMode) sampleMask.luminanceRange != null else sampleMask.colorRange != null))
                )
            if (sampleReady && cropBmp != null) {
                PixelSampleOverlay(
                    bitmap = cropBmp,
                    title = when {
                        sampleWbMode -> "Tap a neutral (grey / white)"
                        sampleLuminanceMode -> "Tap to pick a tone"
                        else -> "Tap to pick a color"
                    },
                    hint = when {
                        sampleWbMode -> "Tap something that should be neutral grey or white — white balance is set to make it neutral."
                        sampleLuminanceMode -> "Tap a tone (a highlight or a shadow) to target it, then apply."
                        else -> "Tap the color you want the mask to target (e.g. a red), then apply."
                    },
                    onPick = { r, g, b, nx, ny ->
                        if (sampleWbMode) {
                            sampleOverlayOpen = false; sampleWbMode = false
                            // Sample the engine INPUT (linear ProPhoto — creative WB's space) at the tap
                            // and solve the WB that neutralizes it. Off the main thread (decode + crop).
                            scope.launch {
                                runCatching {
                                    val full = loadSourceCachedForZoom(MAX_EDGE_PX)
                                    val avg = withContext(Dispatchers.Default) {
                                        val crop = cropLinearImageRect(full, nx, ny, 5, 5)
                                        try { avgRgb(crop) } finally { crop.close() }
                                    }
                                    CreativeWhiteBalance.solveNeutral(
                                        avg.first, avg.second, avg.third,
                                        state.creativeWbTemp, state.creativeWbTint,
                                    )
                                }.onSuccess { (t, ti) ->
                                    state.creativeWbTemp = t; state.creativeWbTint = ti
                                    status = "white balance set from neutral"
                                }.onFailure {
                                    Toast.makeText(ctx, "Couldn't sample for white balance: ${it.message}", Toast.LENGTH_LONG).show()
                                }
                            }
                        } else {
                            val list = state.localAdjustments.toMutableList()
                            val m = list[sampleMaskIndex].mask
                            val nm = if (sampleLuminanceMode) {
                                m.copy(luminanceRange = com.spectrafilm.app.masks.LuminanceRange.fromSample(r, g, b))
                            } else {
                                val cr = m.colorRange ?: com.spectrafilm.app.masks.ColorRange()
                                m.copy(colorRange = cr.copy(targetR = r, targetG = g, targetB = b))
                            }
                            list[sampleMaskIndex] = list[sampleMaskIndex].copy(mask = nm)
                            state.localAdjustments = list
                            sampleOverlayOpen = false
                            previewTick++
                        }
                    },
                    onCancel = { sampleOverlayOpen = false; sampleWbMode = false },
                )
            }

            // --- full-screen export mask ---
            if (exporting) {
                ExportMask(
                    done = exportDone,
                    onDismiss = { exporting = false; exportDone = false },
                )
            }

            // --- Lightroom-style export options sheet ---
            if (showExportSheet) {
                ExportSheet(
                    options = exportOptions,
                    onOptionsChange = { exportOptions = it },
                    colorSpace = state.outputColorSpace,
                    onColorSpaceChange = { state.outputColorSpace = it },
                    cctf = state.savingCctfEncoding,
                    onCctfChange = { state.savingCctfEncoding = it },
                    keepGps = exportKeepGps,
                    onKeepGpsChange = { exportKeepGps = it },
                    onDismiss = { showExportSheet = false },
                    onExport = {
                        val e = engine
                        showExportSheet = false
                        if (e != null) {
                            // Remember the choices as the new Settings defaults.
                            settings.exportFormat = exportOptions.format
                            settings.exportQuality = exportOptions.jpegQuality
                            settings.exportKeepGps = exportKeepGps
                            var fmt = exportOptions.format
                            // Ultra HDR needs the API 34 Gainmap class; fall back to JPEG below that.
                            if (fmt == ExportFormat.ULTRA_HDR && android.os.Build.VERSION.SDK_INT < 34) {
                                fmt = ExportFormat.JPEG
                                Toast.makeText(ctx, "Ultra HDR needs Android 14+ — saving JPEG instead", Toast.LENGTH_LONG).show()
                            }
                            val exportFmt = fmt
                            val baseName = exportBaseName(exportOptions.customName, System.currentTimeMillis())
                            val longEdge = exportOptions.targetLongEdge()
                            val keepGps = exportKeepGps
                            // Wall-clock breadcrumbs for the on-device baseline capture
                            // (issue #119): logcat-visible start marker + duration on ok/fail.
                            val exportStartMs = System.currentTimeMillis()
                            Diag.i("export start format=${exportFmt.name}")
                            exporting = true; exportDone = false; status = "rendering full resolution…"
                            // Keep the process alive across the whole render: a long
                            // backgrounded export is a kill candidate for Samsung's
                            // device-health manager, and the service takes oom_score_adj
                            // from 700 to 50. It does NOT recover the ~4x background
                            // slowdown — no service-tier cpuset on the measured device
                            // contains the prime cores. See ExportForegroundService.
                            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
                                ContextCompat.checkSelfPermission(ctx, android.Manifest.permission.POST_NOTIFICATIONS)
                                != PackageManager.PERMISSION_GRANTED
                            ) {
                                runCatching { notificationPermission.launch(android.Manifest.permission.POST_NOTIFICATIONS) }
                            }
                            ExportForegroundService.start(ctx)
                            scope.launch {
                                val result = runCatching {
                                    withContext(Dispatchers.Default) {
                                        // Full-res off-heap buffers (the OOM fix) — close input/result promptly.
                                        val image = loadSource(EXPORT_MAX_EDGE_PX)
                                        if (exportFmt == ExportFormat.SCENE_LINEAR_TIFF) {
                                            // Export the decoded scene-linear INPUT (before the film
                                            // engine) as a 32-bit float TIFF; the engine is skipped.
                                            try {
                                                withContext(Dispatchers.IO) { saveLinearInputAsTiff32f(ctx, image, baseName) }
                                            } finally {
                                                image.close()
                                            }
                                            null  // no rendered bitmap to preview
                                        } else {
                                            // Copy source EXIF; GPS only when opted in.
                                            val srcExif = withContext(Dispatchers.IO) { readSourceExif(ctx, sourceUri, keepGps = keepGps) }
                                            val res = try {
                                                e.simulate(image, state.toParams())
                                            } finally {
                                                image.close()
                                            }
                                            try {
                                                val bmp0 = simResultToBitmapGraded(res, state.savingCctfEncoding, state.saturation, state.vibrance, state.gamutCompress, state.localAdjustments)
                                                // Post-render downscale for the bitmap formats (high-bit-depth
                                                // is always full-res → longEdge null). Free the full-res bitmap.
                                                val bmp = longEdge?.let { edge ->
                                                    scaleBitmapToLongEdge(bmp0, edge).also { if (it !== bmp0) bmp0.recycle() }
                                                } ?: bmp0
                                                withContext(Dispatchers.IO) {
                                                    when (exportFmt) {
                                                        ExportFormat.TIFF -> saveSimResultAsTiff(ctx, res, displayName = baseName)
                                                        ExportFormat.TIFF32F -> saveSimResultAsTiff(ctx, res, displayName = baseName, float32 = true)
                                                        ExportFormat.PNG16 -> saveSimResultAsPng16(ctx, res, displayName = baseName)
                                                        else -> saveToGallery(ctx, bmp, exportFmt, exportOptions.jpegQuality, srcExif, displayName = baseName)
                                                    }
                                                }
                                                bmp
                                            } finally {
                                                res.close()
                                            }
                                        }
                                    }
                                }
                                result.onSuccess { bmp ->
                                    bmp?.let { preview = it }
                                    Diag.i("export format=${exportFmt.name} ok in ${System.currentTimeMillis() - exportStartMs}ms")
                                    exportDone = true
                                    status = "saved to Pictures/Spektrafilm"
                                }.onFailure {
                                    Diag.w("export format=${exportFmt.name} failed after ${System.currentTimeMillis() - exportStartMs}ms: ${it.message}")
                                    exporting = false
                                    status = "export failed: ${it.message}"
                                    Toast.makeText(ctx, "Export failed: ${it.message}", Toast.LENGTH_LONG).show()
                                }
                            }.invokeOnCompletion {
                                // Covers success, failure AND cancellation: if the activity goes
                                // away mid-export the ongoing notification must go with it.
                                ExportForegroundService.stop(ctx)
                            }
                        }
                    },
                )
            }

            // Recipe restore / status snackbar (above the bottom bar / nav).
            SnackbarHost(
                hostState = snackbarHost,
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .navigationBarsPadding()
                    .padding(bottom = 88.dp, start = 16.dp, end = 16.dp),
            )
        }
    }

    // ---------------------------------------------------------------------------
    // New Lightroom-style chrome
    // ---------------------------------------------------------------------------

    @Composable
    private fun EditorTopBar(
        canExport: Boolean,
        exporting: Boolean,
        canUndo: Boolean,
        canRedo: Boolean,
        onUndo: () -> Unit,
        onRedo: () -> Unit,
        onOpenSource: () -> Unit,
        onExport: () -> Unit,
        onOpenSettings: () -> Unit,
        onOpenAbout: () -> Unit,
    ) {
        Box(
            Modifier
                .fillMaxWidth()
                .background(
                    Brush.verticalGradient(
                        listOf(Color.Black.copy(alpha = 0.55f), Color.Transparent),
                    )
                )
                .windowInsetsPadding(WindowInsets.statusBars),
        ) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .height(48.dp)
                    .padding(horizontal = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                // Open / pick a source photo (was an app-exit bug previously).
                TextTooltip("Open photo") {
                    PressIconButton(onClick = onOpenSource) {
                        Icon(
                            SpectraIcons.OpenPhoto, contentDescription = "Open photo",
                            tint = Color.White,
                        )
                    }
                }
                Spacer(Modifier.weight(1f))
                Text(
                    "Spektrafilm",
                    color = Color.White.copy(alpha = 0.92f),
                    fontWeight = FontWeight.SemiBold,
                )
                Spacer(Modifier.weight(1f))
                // Undo / redo — in-session edit history. Disabled (dimmed) when the
                // respective stack is empty; one slider drag = one undo step.
                TextTooltip("Undo") {
                    PressIconButton(onClick = onUndo, enabled = canUndo) {
                        Icon(
                            SpectraIcons.Undo, contentDescription = "Undo",
                            tint = if (canUndo) Color.White else Color.White.copy(alpha = 0.4f),
                        )
                    }
                }
                TextTooltip("Redo") {
                    PressIconButton(onClick = onRedo, enabled = canRedo) {
                        Icon(
                            SpectraIcons.Redo, contentDescription = "Redo",
                            tint = if (canRedo) Color.White else Color.White.copy(alpha = 0.4f),
                        )
                    }
                }
                // Export / save
                TextTooltip("Export to gallery") {
                    PressIconButton(onClick = onExport, enabled = canExport) {
                        if (exporting) {
                            CircularProgressIndicator(
                                modifier = Modifier.size(20.dp), strokeWidth = 2.dp, color = Color.White,
                            )
                        } else {
                            Icon(
                                SpectraIcons.Presets, contentDescription = "Export to gallery",
                                tint = if (canExport) Color.White else Color.White.copy(alpha = 0.4f),
                            )
                        }
                    }
                }
                TextTooltip("Settings") {
                    PressIconButton(onClick = onOpenSettings) {
                        Icon(SpectraIcons.Settings, contentDescription = "Settings", tint = Color.White)
                    }
                }
                TextTooltip("About") {
                    PressIconButton(onClick = onOpenAbout) {
                        Icon(SpectraIcons.Help, contentDescription = "About", tint = Color.White)
                    }
                }
            }
        }
    }

    /** The pinned preview region on the near-black canvas with a rotate control. */
    @Composable
    private fun PreviewRegion(
        preview: Bitmap?,
        before: Bitmap?,
        busy: Boolean,
        decoding: Boolean,
        exporting: Boolean,
        lastRenderMs: Long?,
        renderErr: String?,
        compareMode: Boolean,
        showHistogram: Boolean,
        gpuEnabled: Boolean,
        gpuProxy: LinearImage?,
        gpuLut: CubeLut?,
        gpuGain: Float,
        onToggleCompare: () -> Unit,
        onToggleHistogram: () -> Unit,
        onRotate: () -> Unit,
        onEditCrop: () -> Unit,
        onPointPicked: (Float, Float) -> Unit,
        renderKey: Int,
        roiOverlay: RoiOverlay?,
        onRoiSettled: (RoiRect) -> Unit,
        onRoiCleared: () -> Unit,
    ) {
        Box(
            Modifier
                .fillMaxSize()
                .background(SpectraIcons.nearBlackCanvas)
                .padding(horizontal = 16.dp),
            contentAlignment = Alignment.Center,
        ) {
            val bmp = preview
            // GPU LUT surface is the FIT view's instant path (the baked look sampled on-GPU,
            // no per-edit CPU re-render); the CPU ZoomableImage owns ZOOM, where it renders the
            // region with grain/halation via the ROI path. gpuBroken latches if GL can't build
            // on this device (→ CPU fallback, never a black screen); gpuZoomHandoff flips when
            // the user pinches/double-taps the GPU surface and resets when zoom returns to fit.
            var gpuBroken by remember { mutableStateOf(false) }
            var gpuZoomHandoff by remember { mutableStateOf(false) }
            // Scale the CPU ZoomableImage starts at after a GPU-surface handoff: 1f for a pinch/
            // double-tap (continue from fit), 2f for the explicit "+" so it lands already zoomed.
            var gpuZoomInitial by remember { mutableFloatStateOf(1f) }
            val gpuActive = gpuEnabled && gpuProxy != null && gpuLut != null &&
                !compareMode && !gpuBroken && !gpuZoomHandoff
            when {
                gpuActive -> GpuPreviewSurface(
                    proxy = gpuProxy!!,
                    lut = gpuLut!!,
                    exposureGain = gpuGain,
                    modifier = Modifier.fillMaxSize(),
                    onPointPicked = onPointPicked,
                    onZoomStart = { gpuZoomHandoff = true; gpuZoomInitial = 1f },
                    onZoomIn = { gpuZoomHandoff = true; gpuZoomInitial = 2f },
                    onUnavailable = {
                        gpuBroken = true
                        Diag.w("gpu surface unavailable (GL program build failed) -> CPU preview")
                    },
                )
                bmp != null && compareMode && before != null ->
                    CompareSlider(before = before, after = bmp, modifier = Modifier.fillMaxWidth())
                bmp != null -> ZoomableImage(
                    bitmap = bmp,
                    modifier = Modifier.fillMaxSize(),
                    initialScale = gpuZoomInitial,
                    onPointPicked = onPointPicked,
                    // Lightroom-style zoom: render the visible region at native resolution
                    // (renderKey = previewTick so an edit while zoomed re-renders the crop).
                    renderKey = renderKey,
                    onRoiSettled = onRoiSettled,
                    // Returning to fit also re-arms the GPU fit surface.
                    onRoiCleared = { onRoiCleared(); gpuZoomHandoff = false },
                    roiOverlay = roiOverlay,
                )
                else -> Text("No preview yet", color = Color.White.copy(alpha = 0.7f))
            }

            // Compact translucent histogram overlaid at the TOP EDGE of the preview
            // (Lightroom-style). Driven by the same `showHistogram` state as the
            // Source-panel toggle. computeHistogram runs on a bg coroutine inside
            // PreviewHistogramOverlay; it reads pixels from `bmp` which is the live
            // preview reference — the preview swap path replaces (not recycles) this
            // bitmap, so there is no recycle race here.
            if (showHistogram && bmp != null) {
                PreviewHistogramOverlay(
                    bitmap = bmp,
                    modifier = Modifier
                        .align(Alignment.TopCenter)
                        .padding(top = 4.dp),
                )
            }

            // Status pill — top-start of the preview, over the canvas.
            StatusPill(
                exporting = exporting,
                rendering = busy,
                decoding = decoding,
                lastRenderMs = lastRenderMs,
                renderErr = renderErr,
                modifier = Modifier
                    .align(Alignment.TopStart)
                    .padding(start = 6.dp, top = 6.dp),
            )

            // bottom-center controls on a scrim: histogram + compare + crop + rotate
            Row(
                Modifier
                    .align(Alignment.BottomCenter)
                    .padding(bottom = 10.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                TextTooltip("Histogram") {
                    CircleScrimButton(onClick = onToggleHistogram, active = showHistogram) {
                        Icon(
                            SpectraIcons.Histogram, contentDescription = "Toggle histogram",
                            tint = Color.White,
                        )
                    }
                }
                TextTooltip("Before / after compare") {
                    CircleScrimButton(onClick = onToggleCompare, active = compareMode) {
                        Icon(
                            SpectraIcons.Display, contentDescription = "Before / after compare",
                            tint = Color.White,
                        )
                    }
                }
                TextTooltip("Crop & transform") {
                    CircleScrimButton(onClick = onEditCrop) {
                        Icon(SpectraIcons.Crop, contentDescription = "Crop and transform", tint = Color.White)
                    }
                }
                TextTooltip("Rotate 90°") {
                    CircleScrimButton(onClick = onRotate) {
                        Icon(SpectraIcons.Rotate, contentDescription = "Rotate 90°", tint = Color.White)
                    }
                }
            }
        }
    }

    /**
     * A compact status pill overlaid in the top-start corner of the preview canvas.
     *
     * Priority: exporting > decoding > rendering > error > idle (last render time or "Ready").
     * Fades in when visible; the idle state auto-fades after 2 s using [animateFloatAsState].
     * Shows a small [CircularProgressIndicator] while busy.
     */
    @Composable
    private fun StatusPill(
        exporting: Boolean,
        rendering: Boolean,
        decoding: Boolean,
        lastRenderMs: Long?,
        renderErr: String?,
        modifier: Modifier = Modifier,
    ) {
        // Derive a single status from the priority chain.
        val pillState: PillState = when {
            exporting -> PillState.Busy("Exporting…")
            decoding  -> PillState.Busy("Decoding…")
            rendering -> PillState.Busy("Rendering…")
            renderErr != null -> PillState.Error(renderErr)
            lastRenderMs != null -> PillState.Done(lastRenderMs)
            else -> PillState.Hidden
        }

        // For the idle "Rendered in X ms" state we auto-fade after 2 s.
        var showIdle by remember { mutableStateOf(false) }
        LaunchedEffect(lastRenderMs, rendering, exporting, decoding, renderErr) {
            if (pillState is PillState.Done) {
                showIdle = true
                delay(2000)
                showIdle = false
            } else {
                showIdle = false
            }
        }

        val visible = pillState is PillState.Busy ||
            pillState is PillState.Error ||
            (pillState is PillState.Done && showIdle)

        val pillDesc = when (pillState) {
            is PillState.Busy  -> pillState.label
            is PillState.Error -> "Render error: ${pillState.message}"
            is PillState.Done  -> "Rendered in ${pillState.ms} ms"
            PillState.Hidden   -> ""
        }

        AnimatedVisibility(
            visible = visible,
            modifier = modifier,
            enter = fadeIn(animationSpec = androidx.compose.animation.core.tween(180)),
            exit  = fadeOut(animationSpec = androidx.compose.animation.core.tween(400)),
        ) {
            Surface(
                shape = RoundedCornerShape(20.dp),
                color = Color.Black.copy(alpha = 0.45f),
                contentColor = Color.White,
                modifier = Modifier
                    .wrapContentSize()
                    .semantics { contentDescription = pillDesc },
            ) {
                Row(
                    modifier = Modifier.padding(horizontal = 10.dp, vertical = 5.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    when (pillState) {
                        is PillState.Busy -> {
                            CircularProgressIndicator(
                                modifier = Modifier.size(13.dp),
                                strokeWidth = 1.5.dp,
                                color = Color.White,
                            )
                            Text(
                                pillState.label,
                                fontSize = 12.sp,
                                color = Color.White,
                            )
                        }
                        is PillState.Error -> {
                            Text(
                                "Error: ${pillState.message}",
                                fontSize = 12.sp,
                                color = MaterialTheme.colorScheme.errorContainer,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                        is PillState.Done -> {
                            Text(
                                "Rendered in ${pillState.ms} ms",
                                fontSize = 12.sp,
                                color = Color.White.copy(alpha = 0.85f),
                            )
                        }
                        PillState.Hidden -> { /* not shown */ }
                    }
                }
            }
        }
    }

    /** Internal sealed hierarchy for the status-pill priority logic. */
    private sealed interface PillState {
        data class Busy(val label: String) : PillState
        data class Error(val message: String) : PillState
        data class Done(val ms: Long) : PillState
        data object Hidden : PillState
    }

    /** 40dp circular control over a ~35% scrim. */
    @Composable
    private fun CircleScrimButton(
        onClick: () -> Unit,
        active: Boolean = false,
        content: @Composable () -> Unit,
    ) {
        val interaction = remember { MutableInteractionSource() }
        val pressed by interaction.collectIsPressedAsState()
        Box(
            Modifier
                .size(40.dp)
                .scale(if (pressed) 0.9f else 1f)
                .clip(CircleShape)
                .background(
                    if (active) MaterialTheme.colorScheme.primary.copy(alpha = 0.55f)
                    else Color.Black.copy(alpha = 0.35f)
                ),
            contentAlignment = Alignment.Center,
        ) {
            IconButton(onClick = onClick, interactionSource = interaction) { content() }
        }
    }

    /** A press-scaling icon button used in the top bar. */
    @Composable
    private fun PressIconButton(
        onClick: () -> Unit,
        enabled: Boolean = true,
        content: @Composable () -> Unit,
    ) {
        val interaction = remember { MutableInteractionSource() }
        val pressed by interaction.collectIsPressedAsState()
        IconButton(
            onClick = onClick,
            enabled = enabled,
            interactionSource = interaction,
            modifier = Modifier.scale(if (pressed) 0.88f else 1f),
        ) { content() }
    }

    /**
     * The adjustment panel as a floating bottom overlay inside the preview Box. Extracting it into a
     * BoxScope extension (a) lets it use Modifier.align to pin to the preview's bottom and (b) breaks
     * the enclosing Column's receiver chain, so AnimatedVisibility resolves to the top-level overload
     * instead of ColumnScope's (which would re-measure the column). The slide/fade reads as a bottom
     * sheet rising over the now constant-height preview, so opening a panel no longer resizes it.
     */
    @Composable
    private fun BoxScope.PanelOverlay(visible: Boolean, content: @Composable () -> Unit) {
        AnimatedVisibility(
            visible = visible,
            modifier = Modifier.align(Alignment.BottomCenter),
            enter = expandVertically(animationSpec = spring(
                dampingRatio = Spring.DampingRatioMediumBouncy,
                stiffness = Spring.StiffnessLow,
            )) + fadeIn(),
            exit = shrinkVertically(animationSpec = spring(
                stiffness = Spring.StiffnessMedium,
            )) + fadeOut(),
        ) { content() }
    }

    /** The adjustment panel body: drag-handle header (swipe down to dismiss) + scrolling content. */
    @Composable
    private fun AdjustmentPanel(
        category: Category?,
        onDismiss: () -> Unit,
        content: @Composable ColumnScope.() -> Unit,
    ) {
        val maxH = (LocalConfigurationHeightDp() * 0.38f).dp
        Surface(
            tonalElevation = 3.dp,
            shadowElevation = 8.dp,
            color = MaterialTheme.colorScheme.surface,
            shape = RoundedCornerShape(topStart = 18.dp, topEnd = 18.dp),
            modifier = Modifier.fillMaxWidth(),
        ) {
            Column(
                Modifier
                    .fillMaxWidth()
                    .heightIn(max = maxH),
            ) {
                // drag-handle header: swipe DOWN to dismiss; tap to dismiss too.
                Box(
                    Modifier
                        .fillMaxWidth()
                        .pointerInput(category) {
                            detectVerticalDragGestures { _, dragAmount ->
                                if (dragAmount > 18f) onDismiss()
                            }
                        },
                    contentAlignment = Alignment.Center,
                ) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Box(
                            Modifier
                                .padding(top = 8.dp)
                                .size(width = 36.dp, height = 4.dp)
                                .clip(RoundedCornerShape(2.dp))
                                .background(MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f)),
                        )
                        Text(
                            category?.label ?: "",
                            style = MaterialTheme.typography.labelLarge,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(vertical = 6.dp),
                        )
                    }
                }
                Column(
                    Modifier
                        .fillMaxWidth()
                        .verticalScroll(rememberScrollState())
                        .padding(horizontal = 16.dp)
                        .padding(bottom = 12.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                    content = content,
                )
            }
        }
    }

    /** Horizontally scrollable category bar with a sliding pill indicator. */
    @Composable
    private fun CategoryBar(
        active: Category?,
        onSelect: (Category) -> Unit,
    ) {
        val items = remember { Category.entries.toList() }
        val listState = rememberLazyListState()
        val scope = rememberCoroutineScope()

        // ease the tapped/active category toward center
        LaunchedEffect(active) {
            val idx = active?.let { items.indexOf(it) } ?: return@LaunchedEffect
            scope.launch { listState.animateScrollToItem(idx.coerceAtLeast(0)) }
        }

        Surface(
            color = SpectraIcons.nearBlackCanvas,
            tonalElevation = 0.dp,
            modifier = Modifier.fillMaxWidth(),
        ) {
            LazyRow(
                state = listState,
                modifier = Modifier
                    .fillMaxWidth()
                    .navigationBarsPadding()
                    .padding(vertical = 6.dp),
                horizontalArrangement = Arrangement.spacedBy(2.dp),
                contentPadding = PaddingValues(horizontal = 8.dp),
            ) {
                itemsIndexed(items) { _, cat ->
                    CategoryItem(
                        category = cat,
                        selected = cat == active,
                        onClick = { onSelect(cat) },
                    )
                }
            }
        }
    }

    @Composable
    private fun CategoryItem(
        category: Category,
        selected: Boolean,
        onClick: () -> Unit,
    ) {
        val accent = MaterialTheme.colorScheme.primary
        val interaction = remember { MutableInteractionSource() }
        val pressed by interaction.collectIsPressedAsState()
        TextTooltip(categoryHint(category)) {
        Column(
            Modifier
                .width(72.dp)
                .scale(if (pressed) 0.92f else 1f)
                .clip(RoundedCornerShape(12.dp))
                .background(if (selected) accent.copy(alpha = 0.18f) else Color.Transparent)
                .clickableNoRipple(interaction, onClick)
                .padding(vertical = 8.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Icon(
                imageVector = categoryIcon(category),
                contentDescription = category.label,
                tint = if (selected) accent else Color.White.copy(alpha = 0.78f),
                modifier = Modifier.size(24.dp),
            )
            Text(
                category.label,
                fontSize = 11.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                color = if (selected) accent else Color.White.copy(alpha = 0.78f),
            )
            // sliding pill indicator
            Box(
                Modifier
                    .padding(top = 2.dp)
                    .size(width = if (selected) 20.dp else 0.dp, height = 3.dp)
                    .clip(RoundedCornerShape(2.dp))
                    .background(if (selected) accent else Color.Transparent),
            )
        }
        }
    }

    /** One-line tooltip description for each editor category. */
    private fun categoryHint(c: Category) = when (c) {
        Category.SOURCE -> "Pick a photo / RAW, demo image, histogram & reset edits"
        Category.PRESETS -> "Built-in looks and your saved presets; import/export & LUT"
        Category.SIMULATION -> "Film stock, print paper, exposure & auto-exposure metering"
        Category.INPUT -> "Input colour space, spectral upsampling, filters, crop & upscale"
        Category.RAW_WB -> "White balance — eyedropper + warmth/tint (all sources), and RAW Kelvin"
        Category.GRAIN -> "Film grain structure, size and blur"
        Category.HALATION -> "Halation glow and in-emulsion light scatter"
        Category.GLARE -> "Print glare (stochastic; off by default)"
        Category.COUPLERS -> "Film color character — DIR couplers (chemical color crosstalk). Plain saturation lives in Output."
        Category.PREFLASH -> "Enlarger pre-flash exposure and filtration"
        Category.EXPERIMENTAL -> "Film and print density-curve gamma factors"
        Category.TONE_CURVE -> "Point tone curve on the final RGB — master + per-channel"
        Category.MASKS -> "Local adjustments — limit Exposure/Saturation/Contrast to a radial area"
        Category.DISPLAY -> "Output colour space, CCTF encoding and preview size"
    }

    private fun categoryIcon(c: Category) = when (c) {
        Category.SIMULATION -> SpectraIcons.Simulation
        Category.INPUT -> SpectraIcons.Input
        Category.RAW_WB -> SpectraIcons.ImportRaw
        Category.GRAIN -> SpectraIcons.Grain
        Category.PREFLASH -> SpectraIcons.Preflash
        Category.HALATION -> SpectraIcons.Halation
        Category.COUPLERS -> SpectraIcons.Couplers
        Category.GLARE -> SpectraIcons.Glare
        Category.EXPERIMENTAL -> SpectraIcons.Experimental
        Category.TONE_CURVE -> SpectraIcons.ToneCurve
        Category.MASKS -> SpectraIcons.Masks
        Category.DISPLAY -> SpectraIcons.Display
        Category.PRESETS -> SpectraIcons.Presets
        Category.SOURCE -> SpectraIcons.SourceImage
    }

    // ---------------------------------------------------------------------------
    // Panels that wrap existing functionality (preset/source) for the new layout
    // ---------------------------------------------------------------------------

    @Composable
    private fun PresetPanel(
        builtInGroups: Map<String, List<BuiltInPreset>>,
        onApplyBuiltIn: (BuiltInPreset) -> Unit,
        amount: Float,
        amountEnabled: Boolean,
        onAmountChange: (Float) -> Unit,
        presets: List<String>,
        selected: String,
        name: String,
        onNameChange: (String) -> Unit,
        onSelect: (String) -> Unit,
        onSave: () -> Unit,
        onApply: () -> Unit,
        onDelete: () -> Unit,
        onImport: () -> Unit,
        onExport: () -> Unit,
        onCopySettings: () -> Unit,
        canPasteSettings: Boolean,
        onPasteSettings: () -> Unit,
        onExportLut: (size: Int, clf: Boolean) -> Unit,
        bakingLut: Boolean,
    ) {
        if (builtInGroups.isNotEmpty()) {
            Text("Built-in looks", style = MaterialTheme.typography.titleSmall)
            var selectedBuiltIn by remember {
                mutableStateOf(builtInGroups.values.firstOrNull()?.firstOrNull()?.id ?: "")
            }
            val all = remember(builtInGroups) { builtInGroups.values.flatten() }
            GroupedDropdown(
                label = "Built-in preset",
                selectedId = selectedBuiltIn,
                groups = builtInGroups.map { (g, ps) ->
                    DropdownGroup(g, ps.map { DropdownOption(it.id, it.name) })
                },
                onSelect = { selectedBuiltIn = it },
            )
            val current = all.firstOrNull { it.id == selectedBuiltIn }
            if (current?.description?.isNotBlank() == true) {
                Text(
                    current.description,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Button(
                onClick = { current?.let(onApplyBuiltIn) },
                enabled = current != null,
                modifier = Modifier.fillMaxWidth(),
            ) { Text("Apply built-in preset") }
            Divider()
            Text("Your presets", style = MaterialTheme.typography.titleSmall)
        }

        // Lightroom-style preset amount: cross-fade the last-applied preset against the
        // look that preceded it. Shown only once a preset has been applied this session.
        if (amountEnabled) {
            EnhancedSlider(
                label = "Preset amount",
                value = amount * 100f,
                range = 0f..100f,
                step = 1f,
                decimals = 0,
                default = 100f,
                tooltip = "Mix the applied preset over the previous look (0% = before, 100% = full).",
                onValueChange = { onAmountChange((it / 100f).coerceIn(0f, 1f)) },
            )
            Divider()
        }

        OutlinedTextField(
            value = name, onValueChange = onNameChange,
            label = { Text("Preset name") }, singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        Button(onClick = onSave, modifier = Modifier.fillMaxWidth()) { Text("Save current as preset") }
        if (presets.isNotEmpty()) {
            Dropdown(
                label = "Saved presets",
                selected = selected.ifEmpty { presets.first() },
                options = presets,
                display = { it },
                onSelect = onSelect,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = onApply, modifier = Modifier.weight(1f)) { Text("Apply") }
                OutlinedButton(onClick = onDelete, modifier = Modifier.weight(1f)) { Text("Delete") }
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = onImport, modifier = Modifier.weight(1f)) { Text("Import .json") }
            OutlinedButton(onClick = onExport, modifier = Modifier.weight(1f)) { Text("Export / share") }
        }
        // Copy the current edit and paste it onto another image (session clipboard).
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = onCopySettings, modifier = Modifier.weight(1f)) { Text("Copy settings") }
            OutlinedButton(
                onClick = onPasteSettings,
                enabled = canPasteSettings,
                modifier = Modifier.weight(1f),
            ) { Text("Paste settings") }
        }
        Divider()
        var lutSize by remember { mutableIntStateOf(33) }
        var lutClf by remember { mutableStateOf(false) }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Size", style = MaterialTheme.typography.bodySmall)
            listOf(17, 33, 65).forEach { sz ->
                FilterChip(selected = lutSize == sz, onClick = { lutSize = sz }, label = { Text("$sz³") })
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Format", style = MaterialTheme.typography.bodySmall)
            FilterChip(selected = !lutClf, onClick = { lutClf = false }, label = { Text(".cube") })
            FilterChip(selected = lutClf, onClick = { lutClf = true }, label = { Text(".clf") })
        }
        Button(
            onClick = { onExportLut(lutSize, lutClf) },
            enabled = !bakingLut,
            modifier = Modifier.fillMaxWidth(),
        ) {
            if (bakingLut) {
                CircularProgressIndicator(
                    modifier = Modifier.size(18.dp), strokeWidth = 2.dp,
                    color = MaterialTheme.colorScheme.onPrimary,
                )
                Spacer(Modifier.width(10.dp))
                Text("Baking LUT…")
            } else {
                Text("Export LUT (${if (lutClf) ".clf" else ".cube"}, $lutSize³)")
            }
        }
        Text(
            "Bakes the current film + print look into a 3D LUT. .cube imports into most apps; " +
                ".clf (Common LUT Format) imports into DaVinci Resolve / OpenColorIO 2.3+. Spatial/" +
                "stochastic effects (grain, halation, diffusion, glare) can't be captured in a 3D LUT " +
                "and are omitted from the bake.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }

    @Composable
    private fun SourcePanel(
        sourceName: String,
        status: String,
        showHistogram: Boolean,
        onToggleHistogram: () -> Unit,
        hasRecipe: Boolean,
        onPickPhoto: () -> Unit,
        onOpenRaw: () -> Unit,
        onUseDemo: () -> Unit,
        onResetEdits: () -> Unit,
    ) {
        Text("Source: $sourceName", style = MaterialTheme.typography.bodySmall)
        Text(status, style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = onPickPhoto, modifier = Modifier.weight(1f)) { Text("Pick photo") }
            Button(onClick = onOpenRaw, modifier = Modifier.weight(1f)) { Text("Open RAW/DNG") }
        }
        OutlinedButton(onClick = onUseDemo, modifier = Modifier.fillMaxWidth()) { Text("Use demo image") }

        FilterChip(
            selected = showHistogram,
            onClick = onToggleHistogram,
            label = { Text("Histogram") },
        )
        Text(
            "Shows a live RGB histogram over the top of the preview.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        if (hasRecipe) {
            Text(
                "Edits auto-saved for this image (original never modified).",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            OutlinedButton(onClick = onResetEdits, modifier = Modifier.fillMaxWidth()) {
                Text("Reset edits / clear recipe")
            }
        }
        Text(
            "Tip: pinch to zoom · double-tap for 2x · tap a point for a 100% grain crop · " +
                "use the rotate button under the preview to rotate the image (preview + export).",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            "Film modeling powered by spektrafilm (GPLv3). Preview is downscaled for " +
                "interactivity; export renders at full resolution.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }

    @Composable
    private fun ExportMask(done: Boolean, onDismiss: () -> Unit) {
        Box(
            Modifier
                .fillMaxSize()
                .background(Color.Black.copy(alpha = 0.78f))
                .pointerInput(Unit) {
                    awaitPointerEventScope { while (true) awaitPointerEvent() }
                },
            contentAlignment = Alignment.Center,
        ) {
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                if (!done) {
                    CircularProgressIndicator(color = Color.White)
                    Text(
                        "Rendering at full resolution…",
                        color = Color.White,
                        style = MaterialTheme.typography.titleMedium,
                        textAlign = TextAlign.Center,
                    )
                } else {
                    Text(
                        "Saved to gallery",
                        color = Color.White,
                        style = MaterialTheme.typography.titleLarge,
                        textAlign = TextAlign.Center,
                    )
                    Text(
                        "Pictures/Spektrafilm",
                        color = Color.White.copy(alpha = 0.8f),
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    Button(onClick = onDismiss) { Text("View result") }
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Parameter sections (spektrafilm GUI order/grouping) — preserved verbatim
    // ---------------------------------------------------------------------------

    @Composable
    private fun InputSection(s: ParamsState, onEditCrop: () -> Unit, onPickNeutral: () -> Unit = {}) {
        var expanded by remember { mutableStateOf(true) }
        SectionCard("Input", expanded, { expanded = it }) {
            // Honesty: the engine's input space is fixed to linear ProPhoto RGB (the
            // runtime InputColorSpace enum has a single value). RAW imports are decoded
            // ACES2065-1 -> ProPhoto and photos are converted sRGB -> ProPhoto before the
            // engine, so this selector is retained for presets/forward-compat but does not
            // change the render. Other input spaces are the deferred input-gamut work.
            GatedBlock("The engine renders every input as linear ProPhoto RGB — other input color spaces aren't wired yet.") {
                Dropdown("Input color space", s.inputColorSpace, INPUT_COLOR_SPACES, { it },
                    { s.inputColorSpace = it })
            }
            SwitchRow("Apply CCTF decoding", s.inputCctfDecoding, { s.inputCctfDecoding = it },
                "Apply the inverse cctf transfer function of the color space")
            // Honesty: the engine only implements HANATOS2025 (filming.expose always calls
            // rgb_to_raw_hanatos2025); MALLETT2019 marshals but falls back, so the choice is inert.
            Divider()
            Text("Creative white balance", style = MaterialTheme.typography.labelLarge)
            Text(
                "A warm/cool + green/magenta grade on the image going into the film — works on every " +
                    "source (RAW, JPEG, HEIC). Separate from the RAW white balance; 0 = off.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            EnhancedSlider("Creative warmth", s.creativeWbTemp, -100f..100f, { s.creativeWbTemp = it },
                step = 1f, decimals = 0,
                tooltip = "Warm/cool the scene before the film sees it (Bradford adaptation in linear " +
                    "ProPhoto). Positive = warmer. 0 = no change.", default = 0f)
            EnhancedSlider("Creative tint", s.creativeWbTint, -100f..100f, { s.creativeWbTint = it },
                step = 1f, decimals = 0,
                tooltip = "Green ↔ magenta shift. Positive = magenta, negative = green. 0 = no change.",
                default = 0f)
            OutlinedButton(onClick = onPickNeutral, modifier = Modifier.fillMaxWidth()) {
                Text("Eyedropper — tap a neutral to set white balance")
            }
            Divider()
            GatedBlock("MALLETT2019 isn't implemented yet — both options currently render as HANATOS2025.") {
                Dropdown("Spectral upsampling", s.spectralUpsampling, Rgb2Raw.entries.toList(),
                    { it.name.lowercase() }, { s.spectralUpsampling = it })
            }
            SwitchRow("hanatos2025 adaptation window", s.adaptationWindow, { s.adaptationWindow = it },
                "Apply the hanatos2025 bandpass adaptation window when reconstructing spectra.")
            SwitchRow("hanatos2025 adaptation surface", s.adaptationSurface, { s.adaptationSurface = it },
                "Apply the hanatos2025 surface adaptation polynomial when reconstructing spectra.")
            EnhancedSlider("Spectral gaussian blur", s.spectralGaussianBlur, 0f..20f,
                { s.spectralGaussianBlur = it }, step = 0.1f, decimals = 1,
                tooltip = "Gaussian blur sigma applied to the reconstructed spectra (spectral-axis " +
                    "samples; each sample is 5 nm). 0 = off.", default = PARAM_DEFAULTS.spectralGaussianBlur)
            TripleSlider("UV filter", s.filterUv, 0f..800f, { s.filterUv = it }, step = 1f, decimals = 0,
                tooltip = "Filter UV light (amplitude, wavelength cutoff nm, sigma nm).",
                componentLabels = Triple("amp", "λ", "σ"), default = PARAM_DEFAULTS.filterUv)
            TripleSlider("IR filter", s.filterIr, 0f..800f, { s.filterIr = it }, step = 1f, decimals = 0,
                tooltip = "Filter IR light (amplitude, wavelength cutoff nm, sigma nm).",
                componentLabels = Triple("amp", "λ", "σ"), default = PARAM_DEFAULTS.filterIr)
            EnhancedSlider("Upscale factor", s.upscaleFactor, 0f..4f, { s.upscaleFactor = it },
                step = 0.5f, decimals = 1, tooltip = "Scale image size up to increase resolution", default = PARAM_DEFAULTS.upscaleFactor)
            // Crop is now an interactive overlay (see the crop button under the
            // preview). Upscale stays a slider: it is a resolution multiplier, not
            // part of the crop rectangle, so it does not fit the crop metaphor.
            Divider()
            Text(
                if (s.crop) "Crop: enabled (use the crop tool to adjust)"
                else "Crop: off (full frame)",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = onEditCrop, modifier = Modifier.weight(1f)) { Text("Edit crop") }
                if (s.crop) {
                    OutlinedButton(
                        onClick = {
                            s.crop = false
                            s.cropCenter = 0.5f to 0.5f
                            s.cropSize = 0.1f to 0.1f
                        },
                    ) { Text("Clear") }
                }
            }
            // Granular reset scope (Lightroom-style): restore just crop + upscale to
            // engine defaults, leaving the rest of the edit untouched.
            val geometryDirty = s.crop || s.upscaleFactor != PARAM_DEFAULTS.upscaleFactor
            if (geometryDirty) {
                OutlinedButton(
                    onClick = {
                        s.crop = false
                        s.cropCenter = 0.5f to 0.5f
                        s.cropSize = 0.1f to 0.1f
                        s.upscaleFactor = PARAM_DEFAULTS.upscaleFactor
                    },
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("Reset crop & geometry") }
            }
        }
    }

    @Composable
    private fun ImportRawSection(s: ParamsState, isRaw: Boolean, onPickNeutral: () -> Unit = {}) {
        var expanded by remember { mutableStateOf(true) }
        SectionCard("White balance", expanded, { expanded = it }) {
            // The eyedropper + creative warmth/tint work on EVERY source, so they're shown FIRST and
            // always — the eyedropper is the most prominent control so it's findable. The native RAW
            // camera WB (Kelvin/tint, re-decodes the file) is appended only for RAW/DNG sources.
            OutlinedButton(onClick = onPickNeutral, modifier = Modifier.fillMaxWidth()) {
                Text("Eyedropper — tap a neutral to set white balance")
            }
            Text(
                "Tap a grey or white area in your photo and the white balance is set to neutralize it. " +
                    "Works on every source — or use the sliders below.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            EnhancedSlider("Warmth", s.creativeWbTemp, -100f..100f, { s.creativeWbTemp = it },
                step = 1f, decimals = 0, default = 0f,
                tooltip = "Warm / cool the image. Positive = warmer; 0 = off. Works on every source.")
            EnhancedSlider("Tint", s.creativeWbTint, -100f..100f, { s.creativeWbTint = it },
                step = 1f, decimals = 0, default = 0f,
                tooltip = "Green ↔ magenta. Positive = magenta, negative = green; 0 = off.")

            Divider()
            // Balance to film stock (virtual 85-filter): the escape hatch for tungsten stocks, which
            // render a daylight scene authentically blue. Adapts the input to the film's reference white
            // so neutrals render neutral. The hint adapts to whether the selected film is tungsten.
            val ctx = LocalContext.current
            val tungsten = StockCatalog.entry(ctx, s.filmProfile)?.balance == "tungsten"
            SwitchRow(
                "Balance to film stock",
                s.balanceToFilmStock,
                { s.balanceToFilmStock = it },
                if (tungsten) {
                    "This is a tungsten-balanced stock, so it renders a daylight scene blue — that's " +
                        "authentic film behaviour. Turn this on to warm the input to the film's reference " +
                        "light and render neutral, like an 85 filter on the lens."
                } else {
                    "The virtual 85 filter — warms the input to a tungsten stock's reference light so it " +
                        "renders neutral. This stock is daylight-balanced (already neutral), so it has no " +
                        "effect here."
                },
            )

            if (isRaw) {
                Divider()
                Text("RAW camera white balance (re-decodes the file):", style = MaterialTheme.typography.labelLarge)
                val customActive = s.rawWhiteBalance == WhiteBalance.CUSTOM
                Dropdown("Camera white balance", s.rawWhiteBalance, WhiteBalance.entries.toList(),
                    { it.name.lowercase() }, { s.rawWhiteBalance = it })
                OutlinedButton(
                    onClick = {
                        s.rawWhiteBalance = WhiteBalance.AS_SHOT
                        s.rawTemperature = 5500f
                        s.rawTint = 1f
                    },
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("Reset to camera / as-shot WB") }
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .alpha(if (customActive) 1f else 0.45f),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    if (!customActive) {
                        Text(
                            "Temperature and Tint are used only when \"custom\" is selected above.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    EnhancedSlider(
                        "Temperature (K)", s.rawTemperature, 1000f..12000f,
                        { s.rawTemperature = it },
                        step = 100f, decimals = 0,
                        tooltip = "Colour temperature in Kelvin for Custom white balance (1000 K – 12000 K).",
                        default = PARAM_DEFAULTS.rawTemperature,
                    )
                    EnhancedSlider(
                        "Tint multiplier", s.rawTint, 0f..2f,
                        { s.rawTint = it },
                        step = 0.01f, decimals = 2,
                        tooltip = "Green/magenta tint multiplier for Custom white balance (1.0 = neutral).",
                        default = PARAM_DEFAULTS.rawTint,
                    )
                }
            }
        }
    }

    /**
     * Show a one-action suggestion snackbar (e.g. "Use its defaults" on a profile switch, §6h; or
     * "Slide mode" when a reversal film is picked, §6e). Non-destructive: [onAction] runs only if the
     * user taps [actionLabel]. UI/state only — no parity impact.
     */
    private fun offerSnackbarSuggestion(
        scope: CoroutineScope,
        host: SnackbarHostState,
        message: String,
        actionLabel: String,
        onAction: () -> Unit,
    ) {
        scope.launch {
            host.currentSnackbarData?.dismiss()
            val result = host.showSnackbar(
                message = message,
                actionLabel = actionLabel,
                withDismissAction = true,
                duration = SnackbarDuration.Long,
            )
            if (result == SnackbarResult.ActionPerformed) onAction()
        }
    }

    @Composable
    private fun SimulationSection(
        s: ParamsState,
        filmGroups: List<DropdownGroup>,
        printGroups: List<DropdownGroup>,
        onOpenFilmCurves: () -> Unit,
        onOpenPrintCurves: () -> Unit,
        onFilmProfileChange: (String) -> Unit,
        onPrintProfileChange: (String) -> Unit,
    ) {
        var expanded by remember { mutableStateOf(true) }
        SectionCard("Simulation", expanded, { expanded = it }) {
            // Lightroom-style sub-tabs split this tool's four groups (Film / Print / Scanner /
            // Output) so only one shows at a time, instead of one long scroll behind dividers.
            var simTab by remember { mutableStateOf(0) }
            SubTabRow(listOf("Film", "Print", "Scanner", "Output"), simTab, { simTab = it })
            when (simTab) {
                0 -> {
                    GroupedDropdown(
                        label = "Film profile",
                        selectedId = s.filmProfile,
                        groups = filmGroups,
                        onSelect = onFilmProfileChange,
                    )
                    OutlinedButton(
                        onClick = onOpenFilmCurves,
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text("View film profile curves") }
                    EnhancedSlider("Camera compensation EV", s.exposureCompensationEv, -10f..10f,
                        { s.exposureCompensationEv = it }, step = 0.25f, decimals = 2, default = 0f,
                        tooltip = "Add a bias to the auto-exposure of the camera")

                    AutoExposureControl(
                        autoExposure = s.autoExposure,
                        autoExposureMethod = s.autoExposureMethod,
                        methods = AUTO_EXPOSURE_METHODS,
                        onAutoExposureChange = { s.autoExposure = it },
                        onMethodChange = { s.autoExposureMethod = it },
                    )
                    EnhancedSlider("Film format mm", s.filmFormatMm, 8f..120f, { s.filmFormatMm = it },
                        step = 1f, decimals = 0,
                        tooltip = "Long edge of the film format in mm (8, 16, 35, 60, 120)", default = PARAM_DEFAULTS.filmFormatMm)
                    EnhancedSlider("Camera lens blur um", s.cameraLensBlurUm, 0f..20f, { s.cameraLensBlurUm = it },
                        step = 0.05f, decimals = 2,
                        tooltip = "Sigma of gaussian filter in um for the camera lens blur. " +
                            "Spatial effect — applied only when Halation is enabled (the spatial branch).", default = PARAM_DEFAULTS.cameraLensBlurUm)
                    DiffusionGroup("Camera diffusion filter", s.cameraDiffusionState)
                }
                1 -> {
                    GroupedDropdown(
                        label = "Print profile",
                        selectedId = s.printProfile,
                        groups = printGroups,
                        onSelect = onPrintProfileChange,
                    )
                    OutlinedButton(
                        onClick = onOpenPrintCurves,
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text("View print profile curves") }
                    EnhancedSlider("Print exposure", s.printExposure, 0f..4f, { s.printExposure = it },
                        step = 0.02f, decimals = 2, tooltip = "Changes the exposure time set in the virtual enlarger", default = PARAM_DEFAULTS.printExposure)
                    SwitchRow("Print auto compensation", s.printExposureCompensation,
                        { s.printExposureCompensation = it },
                        "Auto adjust the print exposure for the camera exposure compensation ev")
                    EnhancedSlider("Print Y filter shift", s.printYFilterShift, -50f..50f, { s.printYFilterShift = it },
                        step = 1f, decimals = 0, default = 0f, tooltip = "Y filter shift from neutral, in Kodak CC units")
                    EnhancedSlider("Print M filter shift", s.printMFilterShift, -50f..50f, { s.printMFilterShift = it },
                        step = 1f, decimals = 0, default = 0f, tooltip = "M filter shift from neutral, in Kodak CC units")
                    GatedBlock("Enlarger lens blur is not applicable to the enlarger stage (no engine call site).") {
                        EnhancedSlider("Enlarger lens blur", s.enlargerLensBlur, 0f..20f, { s.enlargerLensBlur = it },
                            step = 0.05f, decimals = 2, tooltip = "Sigma of gaussian filter for the enlarger lens blur", default = PARAM_DEFAULTS.enlargerLensBlur)
                    }
                    DiffusionGroup("Print diffusion filter", s.printDiffusionState)
                }
                2 -> {
                    val ctx = LocalContext.current
                    EnhancedSlider("Scan lens blur", s.scanLensBlur, 0f..20f, { s.scanLensBlur = it },
                        step = 0.05f, decimals = 2,
                        tooltip = "Sigma of gaussian filter in pixel for the scanner lens blur. " +
                            "Spatial effect — applied only when Halation is enabled (the spatial branch).", default = PARAM_DEFAULTS.scanLensBlur)
                    // Scan white/black correction pins the scan's white/black points to the target levels
                    // below. The engine makes it a STRICT no-op in Slide mode on a negative stock (it's
                    // active only for a slide/positive film on the scan-film route, or in print mode — all
                    // print papers are negative), so we flag and gray it out there; elsewhere it's active
                    // but often subtle at the default 0.98/0.01 levels.
                    val correctionNoOp = s.scanFilm && !StockCatalog.isReversalFilm(ctx, s.filmProfile)
                    if (correctionNoOp) {
                        Text(
                            "Scan white/black correction has no effect in Slide mode on a negative stock — " +
                                "it applies only to a slide/positive film, or in print mode. Matches the " +
                                "spektrafilm engine.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Column(
                        modifier = Modifier.fillMaxWidth().alpha(if (correctionNoOp) 0.5f else 1f),
                        verticalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        SwitchRow("Scan white correction", s.scanWhiteCorrection, { s.scanWhiteCorrection = it },
                            "Pin the scan's white point to the target white level below. Subtle at the " +
                                "default 0.98 — lower the white level to see it pull the highlights down.")
                        EnhancedSlider("Scan white level", s.scanWhiteLevel, 0f..1f, { s.scanWhiteLevel = it },
                            step = 0.005f, decimals = 3, tooltip = "Target white level when white correction is enabled", default = PARAM_DEFAULTS.scanWhiteLevel)
                        SwitchRow("Scan black correction", s.scanBlackCorrection, { s.scanBlackCorrection = it },
                            "Pin the scan's black point to the target black level below. Subtle at the " +
                                "default 0.01 — raise the black level to see it lift the shadows.")
                        EnhancedSlider("Scan black level", s.scanBlackLevel, 0f..1f, { s.scanBlackLevel = it },
                            step = 0.005f, decimals = 3, tooltip = "Target black level when black correction is enabled", default = PARAM_DEFAULTS.scanBlackLevel)
                    }
                    PairSlider("Scan unsharp mask", s.scanUnsharpMask, 0f..5f, { s.scanUnsharpMask = it },
                        step = 0.05f, decimals = 2, tooltip = "[sigma in pixel, amount]",
                        componentLabels = "σ" to "amt", default = PARAM_DEFAULTS.scanUnsharpMask)
                }
                else -> {
                    Dropdown("Output color space", s.outputColorSpace, ColorSpace.entries.toList(),
                        { it.name }, { s.outputColorSpace = it })
                    // Opt-in gamut compression (default Off => byte-identical to the
                    // parity oracle). Output: ACES Reference Gamut Compression softens
                    // out-of-gamut chromaticities toward the achromatic axis in the
                    // output space. Input: a radial CIE-xy compression toward the visible
                    // spectral locus, baked into the filming reconstruction LUT, tames
                    // over-saturated input before the film responds to it.
                    Dropdown(
                        "Output gamut compression",
                        s.outputGamutCompress,
                        listOf(
                            OutputGamutCompress.LEGACY_CLIP,
                            OutputGamutCompress.ACES_RGC,
                            OutputGamutCompress.OKLCH,
                            OutputGamutCompress.OKLRAB,
                        ),
                        {
                            when (it) {
                                OutputGamutCompress.LEGACY_CLIP -> "Off"
                                OutputGamutCompress.OFF -> "Off (no clip)"
                                OutputGamutCompress.ACES_RGC -> "ACES (tame out-of-gamut)"
                                OutputGamutCompress.OKLCH -> "Oklch (perceptual, keep hue)"
                                OutputGamutCompress.OKLRAB -> "Oklrab (perceptual, even lightness)"
                            }
                        },
                        { s.outputGamutCompress = it },
                    )
                    Dropdown(
                        "Input gamut compression",
                        s.inputGamutCompress,
                        InputGamutCompress.entries.toList(),
                        {
                            when (it) {
                                InputGamutCompress.OFF -> "Off"
                                InputGamutCompress.XY -> "Spectral locus (tame saturated input)"
                            }
                        },
                        { s.inputGamutCompress = it },
                    )
                    // Creative output grade (post-engine Oklab chroma; parity-safe). Negative
                    // Saturation mutes a too-punchy look; Vibrance boosts muted colors while
                    // sparing already-saturated ones.
                    EnhancedSlider("Saturation", s.saturation, -100f..100f, { s.saturation = it },
                        step = 1f, decimals = 0, default = 0f,
                        tooltip = "Overall colorfulness of the output. Negative = softer/more muted " +
                            "(tame a too-punchy look); positive = more vivid. Applied after the film render.")
                    EnhancedSlider("Vibrance", s.vibrance, -100f..100f, { s.vibrance = it },
                        step = 1f, decimals = 0, default = 0f,
                        tooltip = "Like Saturation but weighted to muted colors, so already-saturated " +
                            "tones (and skin) shift less. Applied after the film render.")
                    EnhancedSlider("Gamut compression", s.gamutCompress, 0f..100f, { s.gamutCompress = it },
                        step = 1f, decimals = 0, default = 0f,
                        tooltip = "ACES-style: softens the harsh cyan/edge fringe on very saturated colors " +
                            "by pulling them toward neutral. 0 = off. Helps when saturated highlights look unnatural.")
                    SwitchRow("Saving CCTF encoding", s.savingCctfEncoding, { s.savingCctfEncoding = it },
                        "Add or not the CCTF to the saved image file")
                    SwitchRow("Slide mode (skip print)", s.scanFilm, { s.scanFilm = it },
                        "Show the scanned film directly instead of a print — the natural view for " +
                            "slide/reversal stocks (a positive). For negative stocks this shows the " +
                            "raw orange negative.")
                }
            }
        }
    }

    @Composable
    private fun DiffusionGroup(title: String, d: DiffusionState) {
        var expanded by remember { mutableStateOf(false) }
        Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            SwitchRow(title, d.active, { d.active = it },
                "Toggle the diffusion filter on this stage.")
            TextButton(onClick = { expanded = !expanded }) {
                Text(if (expanded) "Hide diffusion details" else "Show diffusion details")
            }
            if (expanded) {
                Dropdown("Diffusion family", d.family, DIFFUSION_FAMILIES, { it }, { d.family = it })
                EnhancedSlider("Diffusion strength", d.strength, 0f..2f, { d.strength = it },
                    step = 0.125f, decimals = 3, tooltip = "Commercial filter stop: 0, 1/8, 1/4, 1/2, 1, 2.", default = PARAM_DEFAULTS.cameraDiffusionState.strength)
                EnhancedSlider("Spatial scale", d.spatialScale, 0f..4f, { d.spatialScale = it },
                    step = 0.1f, decimals = 2, tooltip = "Multiplier on the image-plane PSF widths.", default = PARAM_DEFAULTS.cameraDiffusionState.spatialScale)
                EnhancedSlider("Halo warmth", d.haloWarmth, -1.5f..1.5f, { d.haloWarmth = it },
                    step = 0.05f, decimals = 2, default = 0f, tooltip = "Additive offset on the halo warmth axis.")
                EnhancedSlider("Core intensity", d.coreIntensity, 0f..4f, { d.coreIntensity = it },
                    step = 0.05f, decimals = 2, default = PARAM_DEFAULTS.cameraDiffusionState.coreIntensity)
                EnhancedSlider("Core size", d.coreSize, 0.1f..4f, { d.coreSize = it }, step = 0.05f, decimals = 2, default = PARAM_DEFAULTS.cameraDiffusionState.coreSize)
                EnhancedSlider("Halo intensity", d.haloIntensity, 0f..4f, { d.haloIntensity = it },
                    step = 0.05f, decimals = 2, default = PARAM_DEFAULTS.cameraDiffusionState.haloIntensity)
                EnhancedSlider("Halo size", d.haloSize, 0.1f..4f, { d.haloSize = it }, step = 0.05f, decimals = 2, default = PARAM_DEFAULTS.cameraDiffusionState.haloSize)
                EnhancedSlider("Bloom intensity", d.bloomIntensity, 0f..4f, { d.bloomIntensity = it },
                    step = 0.05f, decimals = 2, default = PARAM_DEFAULTS.cameraDiffusionState.bloomIntensity)
                EnhancedSlider("Bloom size", d.bloomSize, 0.1f..4f, { d.bloomSize = it }, step = 0.05f, decimals = 2, default = PARAM_DEFAULTS.cameraDiffusionState.bloomSize)
            }
        }
    }

    @Composable
    private fun GrainSection(s: ParamsState) {
        var expanded by remember { mutableStateOf(true) }
        SectionCard("Grain", expanded, { expanded = it }, enabledSwitch = s.grainActive,
            onEnabledChange = { s.grainActive = it }, help = ParamHelpText.forKey(ParamHelpText.GRAIN)) {
            var advanced by remember { mutableStateOf(false) }
            // Basic: the two knobs most users reach for — grain size (ISO) and softness.
            EnhancedSlider("Particle area um2", s.grainParticleAreaUm2, 0f..2f, { s.grainParticleAreaUm2 = it },
                step = 0.2f, decimals = 2, tooltip = "Area of particles in um2, relates to ISO.", default = PARAM_DEFAULTS.grainParticleAreaUm2)
            EnhancedSlider("Blur", s.grainBlur, 0f..3f, { s.grainBlur = it }, step = 0.05f, decimals = 2,
                tooltip = "Sigma of gaussian blur in pixels for the grain.", default = PARAM_DEFAULTS.grainBlur)
            AdvancedToggle(advanced) { advanced = it }
            if (advanced) {
                SwitchRow("Sublayers active", s.grainSublayersActive, { s.grainSublayersActive = it })
                TripleSlider("Particle scale", s.grainParticleScale, 0f..5f, { s.grainParticleScale = it },
                    step = 0.1f, decimals = 2, tooltip = "Scale of particle area for the RGB layers.",
                    default = PARAM_DEFAULTS.grainParticleScale)
                TripleSlider("Particle scale layers", s.grainParticleScaleLayers, 0f..5f,
                    { s.grainParticleScaleLayers = it }, step = 0.25f, decimals = 2,
                    tooltip = "Scale of particle area for the sublayers in each color layer.",
                    default = PARAM_DEFAULTS.grainParticleScaleLayers)
                TripleSlider("Density min", s.grainDensityMin, 0f..0.5f, { s.grainDensityMin = it },
                    step = 0.01f, decimals = 3, tooltip = "Minimum density of the grain (0.03-0.06).",
                    default = PARAM_DEFAULTS.grainDensityMin)
                TripleSlider("Uniformity", s.grainUniformity, 0.5f..1f, { s.grainUniformity = it },
                    step = 0.005f, decimals = 3, tooltip = "Uniformity of the grain (0.94-0.98).",
                    default = PARAM_DEFAULTS.grainUniformity)
                EnhancedSlider("Blur dye clouds um", s.grainBlurDyeCloudsUm, 0f..5f, { s.grainBlurDyeCloudsUm = it },
                    step = 0.1f, decimals = 2, tooltip = "Scale the sigma of gaussian blur in um for the dye clouds.", default = PARAM_DEFAULTS.grainBlurDyeCloudsUm)
                PairSlider("Micro structure", s.grainMicroStructure, 0f..100f, { s.grainMicroStructure = it },
                    step = 0.1f, decimals = 2, tooltip = "[sigma blur um, molecular clump size nm]",
                    componentLabels = "σ" to "nm", default = PARAM_DEFAULTS.grainMicroStructure)
                IntSlider("Sublayers", s.grainNSubLayers, 1..5, { s.grainNSubLayers = it },
                    default = PARAM_DEFAULTS.grainNSubLayers)
            }
            Divider()
            // Granular reset scope (backlog #F): restore the grain parameters to engine
            // defaults without touching the section's on/off switch or other sections.
            OutlinedButton(
                onClick = {
                    val d = PARAM_DEFAULTS
                    s.grainSublayersActive = d.grainSublayersActive
                    s.grainParticleAreaUm2 = d.grainParticleAreaUm2
                    s.grainParticleScale = d.grainParticleScale
                    s.grainParticleScaleLayers = d.grainParticleScaleLayers
                    s.grainDensityMin = d.grainDensityMin
                    s.grainUniformity = d.grainUniformity
                    s.grainBlur = d.grainBlur
                    s.grainBlurDyeCloudsUm = d.grainBlurDyeCloudsUm
                    s.grainMicroStructure = d.grainMicroStructure
                    s.grainNSubLayers = d.grainNSubLayers
                },
                modifier = Modifier.fillMaxWidth(),
            ) { Text("Reset grain to defaults") }
        }
    }

    @Composable
    private fun PreflashSection(s: ParamsState) {
        var expanded by remember { mutableStateOf(true) }
        SectionCard("Preflash", expanded, { expanded = it }, help = ParamHelpText.forKey(ParamHelpText.PREFLASH)) {
            EnhancedSlider("Exposure", s.preflashExposure, 0f..2f, { s.preflashExposure = it },
                step = 0.005f, decimals = 3, tooltip = "Preflash exposure value in ev for the print", default = PARAM_DEFAULTS.preflashExposure)
            EnhancedSlider("Y filter shift", s.preflashYFilterShift, -20f..20f, { s.preflashYFilterShift = it },
                step = 1f, decimals = 0, default = 0f, tooltip = "Shift the Y filter from neutral for the preflash (Kodak CC)")
            EnhancedSlider("M filter shift", s.preflashMFilterShift, -20f..20f, { s.preflashMFilterShift = it },
                step = 1f, decimals = 0, default = 0f, tooltip = "Shift the M filter from neutral for the preflash (Kodak CC)")
        }
    }

    @Composable
    private fun HalationSection(s: ParamsState) {
        var expanded by remember { mutableStateOf(true) }
        SectionCard("Halation", expanded, { expanded = it }, enabledSwitch = s.halationActive,
            onEnabledChange = { s.halationActive = it }, help = ParamHelpText.forKey(ParamHelpText.HALATION)) {
            var advanced by remember { mutableStateOf(false) }
            // Basic: glow strength + size, the scatter strength, and the highlight boost.
            EnhancedSlider("Halation amount", s.halHalationAmount, 0f..4f, { s.halHalationAmount = it },
                step = 0.05f, decimals = 2, tooltip = "High-level halation strength multiplier.", default = PARAM_DEFAULTS.halHalationAmount)
            EnhancedSlider("Halation spatial scale", s.halHalationSpatialScale, 0f..4f,
                { s.halHalationSpatialScale = it }, step = 0.1f, decimals = 2,
                tooltip = "High-level halation size multiplier.", default = PARAM_DEFAULTS.halHalationSpatialScale)
            EnhancedSlider("Scatter amount", s.halScatterAmount, 0f..4f, { s.halScatterAmount = it },
                step = 0.05f, decimals = 2, tooltip = "High-level scatter strength. 1.0 = full physical scatter.", default = PARAM_DEFAULTS.halScatterAmount)
            EnhancedSlider("Boost EV", s.halBoostEv, 0f..6f, { s.halBoostEv = it }, step = 0.5f, decimals = 1,
                tooltip = "Maximum highlight boost in stops.", default = PARAM_DEFAULTS.halBoostEv)
            AdvancedToggle(advanced) { advanced = it }
            if (advanced) {
                EnhancedSlider("Scatter spatial scale", s.halScatterSpatialScale, 0f..4f,
                    { s.halScatterSpatialScale = it }, step = 0.1f, decimals = 2,
                    tooltip = "High-level scatter size multiplier (1.0 = physical defaults).", default = PARAM_DEFAULTS.halScatterSpatialScale)
                EnhancedSlider("Protect EV", s.halProtectEv, 0f..10f, { s.halProtectEv = it }, step = 0.5f, decimals = 1,
                    tooltip = "Protected range above midgray for the boost onset in stops.", default = PARAM_DEFAULTS.halProtectEv)
                EnhancedSlider("Boost range", s.halBoostRange, 0f..1f, { s.halBoostRange = it },
                    step = 0.05f, decimals = 2, tooltip = "How quickly the highlight boost ramps in (0-1).", default = PARAM_DEFAULTS.halBoostRange)
                TripleSlider("Scatter core um", s.halScatterCoreUm, 0f..20f, { s.halScatterCoreUm = it },
                    step = 0.5f, decimals = 2, tooltip = "Sigma of the scatter core Gaussian per channel, in um.",
                    default = PARAM_DEFAULTS.halScatterCoreUm)
                TripleSlider("Scatter tail um", s.halScatterTailUm, 0f..40f, { s.halScatterTailUm = it },
                    step = 1f, decimals = 1, tooltip = "Decay constant of the scatter exponential tail per channel, in um.",
                    default = PARAM_DEFAULTS.halScatterTailUm)
                TripleSlider("Scatter tail weight %", s.halScatterTailWeightPct, 0f..100f,
                    { s.halScatterTailWeightPct = it }, step = 1f, decimals = 1,
                    tooltip = "Weight of the scatter tail Gaussian per channel (0-100%).",
                    default = PARAM_DEFAULTS.halScatterTailWeightPct)
                TripleSlider("Halation strength %", s.halHalationStrengthPct, 0f..100f,
                    { s.halHalationStrengthPct = it }, step = 0.5f, decimals = 2,
                    tooltip = "Total back-reflection halation amplitude per channel (0-100%).",
                    default = PARAM_DEFAULTS.halHalationStrengthPct)
                TripleSlider("First sigma um", s.halFirstSigmaUm, 0f..200f, { s.halFirstSigmaUm = it },
                    step = 1f, decimals = 1, tooltip = "Sigma of the first halation bounce per channel, in um.",
                    default = PARAM_DEFAULTS.halFirstSigmaUm)
                IntSlider("N bounces", s.halNBounces, 1..5, { s.halNBounces = it },
                    tooltip = "Number of multi-bounce Gaussians summed (typical 2-3).",
                    default = PARAM_DEFAULTS.halNBounces)
                EnhancedSlider("Bounce decay", s.halBounceDecay, 0f..1f, { s.halBounceDecay = it },
                    step = 0.05f, decimals = 2, tooltip = "Per-bounce amplitude decay ratio (0.3-0.7).", default = PARAM_DEFAULTS.halBounceDecay)
                SwitchRow("Renormalize", s.halRenormalize, { s.halRenormalize = it },
                    "Divide by (1 + sum of bounce amplitudes) so mid-grey is preserved.")
            }
        }
    }

    @Composable
    private fun CouplersSection(s: ParamsState) {
        var expanded by remember { mutableStateOf(true) }
        SectionCard("Film color character (couplers)", expanded, { expanded = it }, enabledSwitch = s.couplersActive,
            onEnabledChange = { s.couplersActive = it }, help = ParamHelpText.forKey(ParamHelpText.COUPLERS)) {
            var advanced by remember { mutableStateOf(false) }
            Text(
                "Models film's chemical color crosstalk (DIR couplers) — the cause of film's " +
                    "characteristic color separation and edge effects. Looking for a plain saturation " +
                    "control? Use Saturation / Vibrance in Simulation → Output.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            // Basic: the three overall strengths. The per-channel mix matrix is advanced.
            EnhancedSlider("Effect strength", s.couplersAmount, 0f..4f, { s.couplersAmount = it },
                step = 0.05f, decimals = 2, tooltip = "Global multiplier on the DIR coupler inhibition matrix.", default = PARAM_DEFAULTS.couplersAmount)
            EnhancedSlider("Within-layer strength", s.couplersInhibitionSamelayer, 0f..4f,
                { s.couplersInhibitionSamelayer = it }, step = 0.05f, decimals = 2,
                tooltip = "Same-layer (diagonal) inhibition — within-channel contrast/acutance.", default = PARAM_DEFAULTS.couplersInhibitionSamelayer)
            EnhancedSlider("Cross-color strength", s.couplersInhibitionInterlayer, 0f..4f,
                { s.couplersInhibitionInterlayer = it }, step = 0.05f, decimals = 2,
                tooltip = "Cross-layer (off-diagonal) inhibition — how much each dye layer bleeds into the others.", default = PARAM_DEFAULTS.couplersInhibitionInterlayer)
            AdvancedToggle(advanced) { advanced = it }
            if (advanced) {
                GatedBlock("The DIR gamma matrix is set per film stock by the engine (each stock overwrites these) — adjusting them has no effect.") {
                    TripleSlider("Within-layer curve (R, G, B)", s.couplersGammaSamelayer, 0f..2f, { s.couplersGammaSamelayer = it },
                        step = 0.02f, decimals = 3, tooltip = "Per-channel same-layer DIR gamma (R, G, B).",
                        default = PARAM_DEFAULTS.couplersGammaSamelayer)
                    PairSlider("Color mix R→G/B", s.couplersGammaRtoGb, 0f..2f, { s.couplersGammaRtoGb = it },
                        step = 0.02f, decimals = 3, tooltip = "Cross-channel DIR inhibition (a color-mixing matrix term): from R onto G and B.",
                        componentLabels = "→G" to "→B", default = PARAM_DEFAULTS.couplersGammaRtoGb)
                    PairSlider("Color mix G→R/B", s.couplersGammaGtoRb, 0f..2f, { s.couplersGammaGtoRb = it },
                        step = 0.02f, decimals = 3, tooltip = "Cross-channel DIR inhibition (a color-mixing matrix term): from G onto R and B.",
                        componentLabels = "→R" to "→B", default = PARAM_DEFAULTS.couplersGammaGtoRb)
                    PairSlider("Color mix B→R/G", s.couplersGammaBtoRg, 0f..2f, { s.couplersGammaBtoRg = it },
                        step = 0.02f, decimals = 3, tooltip = "Cross-channel DIR inhibition (a color-mixing matrix term): from B onto R and G.",
                        componentLabels = "→R" to "→G", default = PARAM_DEFAULTS.couplersGammaBtoRg)
                }
                EnhancedSlider("Color bleed radius (µm)", s.couplersDiffusionSizeUm, 0f..100f, { s.couplersDiffusionSizeUm = it },
                    step = 5f, decimals = 1, tooltip = "Sigma in µm for the diffusion of the couplers (5-20 µm).", default = PARAM_DEFAULTS.couplersDiffusionSizeUm)
                EnhancedSlider("Color bleed tail (µm)", s.couplersDiffusionTailUm, 0f..500f, { s.couplersDiffusionTailUm = it },
                    step = 5f, decimals = 1, tooltip = "Long-range tail sigma in µm for the coupler diffusion.", default = PARAM_DEFAULTS.couplersDiffusionTailUm)
                EnhancedSlider("Color bleed tail weight", s.couplersDiffusionTailWeight, 0f..1f,
                    { s.couplersDiffusionTailWeight = it }, step = 0.01f, decimals = 3,
                    tooltip = "Weight of the long-range diffusion tail.", default = PARAM_DEFAULTS.couplersDiffusionTailWeight)
            }
        }
    }

    @Composable
    private fun GlareSection(s: ParamsState) {
        var expanded by remember { mutableStateOf(true) }
        SectionCard("Glare", expanded, { expanded = it }, enabledSwitch = s.glareActive,
            onEnabledChange = { s.glareActive = it }, help = ParamHelpText.forKey(ParamHelpText.GLARE)) {
            // Live control, but only on one route — say so instead of dimming.
            Text(
                "Glare applies on the print route only (no effect in Slide mode / scan-film).",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            EnhancedSlider("Percent", s.glarePercent, 0f..1f, { s.glarePercent = it },
                step = 0.01f, decimals = 2, tooltip = "Percentage of the glare light (typically 0.1-0.25)", default = PARAM_DEFAULTS.glarePercent)
            EnhancedSlider("Roughness", s.glareRoughness, 0f..1f, { s.glareRoughness = it },
                step = 0.05f, decimals = 2, tooltip = "Roughness of the glare light (0-1)", default = PARAM_DEFAULTS.glareRoughness)
            EnhancedSlider("Blur", s.glareBlur, 0f..10f, { s.glareBlur = it }, step = 0.1f, decimals = 2,
                tooltip = "Sigma of gaussian blur in pixels for the glare", default = PARAM_DEFAULTS.glareBlur)
        }
    }

    @Composable
    private fun ExperimentalSection(s: ParamsState) {
        var expanded by remember { mutableStateOf(true) }
        SectionCard("Experimental", expanded, { expanded = it },
            help = ParamHelpText.forKey(ParamHelpText.PRINT_GAMMA)) {
            EnhancedSlider("Film gamma factor", s.filmGammaFactor, 0f..3f, { s.filmGammaFactor = it },
                step = 0.05f, decimals = 2, tooltip = "Gamma factor of the negative density curves.", default = PARAM_DEFAULTS.filmGammaFactor)
            EnhancedSlider("Print gamma factor", s.printGammaFactor, 0f..3f, { s.printGammaFactor = it },
                step = 0.05f, decimals = 2, tooltip = "Gamma factor of the print paper.", default = PARAM_DEFAULTS.printGammaFactor)
            SwitchRow("Print curve morph (s023)", s.morphActive, { s.morphActive = it },
                tooltip = "Re-develop the print from the paper's parametric density-curve model with a coupled-gamma + developer-exhaustion morph. Off = the stock's measured curves.")
            if (s.morphActive) {
                EnhancedSlider("Morph · overall gamma", s.morphGammaFactor, 0.2f..3f, { s.morphGammaFactor = it },
                    step = 0.05f, decimals = 2, tooltip = "Global coupled gamma applied to all print curves.", default = PARAM_DEFAULTS.morphGammaFactor)
                EnhancedSlider("Morph · fast band", s.morphGammaFactorFast, 0.2f..3f, { s.morphGammaFactorFast = it },
                    step = 0.05f, decimals = 2, tooltip = "Gamma of the fast (low-threshold) sub-layer.", default = PARAM_DEFAULTS.morphGammaFactorFast)
                EnhancedSlider("Morph · slow band", s.morphGammaFactorSlow, 0.2f..3f, { s.morphGammaFactorSlow = it },
                    step = 0.05f, decimals = 2, tooltip = "Gamma of the slow (high-threshold) sub-layers.", default = PARAM_DEFAULTS.morphGammaFactorSlow)
                EnhancedSlider("Morph · red gamma", s.morphGammaFactorRed, 0.2f..3f, { s.morphGammaFactorRed = it },
                    step = 0.05f, decimals = 2, tooltip = "Per-channel gamma (red).", default = PARAM_DEFAULTS.morphGammaFactorRed)
                EnhancedSlider("Morph · green gamma", s.morphGammaFactorGreen, 0.2f..3f, { s.morphGammaFactorGreen = it },
                    step = 0.05f, decimals = 2, tooltip = "Per-channel gamma (green).", default = PARAM_DEFAULTS.morphGammaFactorGreen)
                EnhancedSlider("Morph · blue gamma", s.morphGammaFactorBlue, 0.2f..3f, { s.morphGammaFactorBlue = it },
                    step = 0.05f, decimals = 2, tooltip = "Per-channel gamma (blue).", default = PARAM_DEFAULTS.morphGammaFactorBlue)
                EnhancedSlider("Morph · developer exhaustion", s.morphDeveloperExhaustion, 0f..1f, { s.morphDeveloperExhaustion = it },
                    step = 0.02f, decimals = 2, tooltip = "Blend toward a Gumbel-matched curve shape (shoulder roll-off); preserves midgray.", default = PARAM_DEFAULTS.morphDeveloperExhaustion)
            }
        }
    }

    @Composable
    private fun DisplaySection(s: ParamsState) {
        var expanded by remember { mutableStateOf(true) }
        SectionCard("Display", expanded, { expanded = it }) {
            IntSlider("Preview max size", s.previewMaxSize, 128..1024, { s.previewMaxSize = it },
                tooltip = "Max size of the long edge of the preview image, in pixels.",
                default = PARAM_DEFAULTS.previewMaxSize)
        }
    }

    @Composable
    private fun Divider() {
        HorizontalDivider(Modifier.padding(vertical = 4.dp))
    }
}

/** Small helpers kept at file scope. */

/** Average RGB (0..1) of a small LinearImage crop — the WB eyedropper's sampled neutral. */
private fun avgRgb(img: com.spectrafilm.engine.LinearImage): Triple<Float, Float, Float> {
    val f = img.data.duplicate().order(java.nio.ByteOrder.nativeOrder()).asFloatBuffer()
    val n = (img.width * img.height).coerceAtLeast(1)
    var r = 0f; var g = 0f; var b = 0f
    for (i in 0 until n) { val k = i * 3; r += f.get(k); g += f.get(k + 1); b += f.get(k + 2) }
    val inv = 1f / n
    return Triple(r * inv, g * inv, b * inv)
}

@Composable
private fun LocalConfigurationHeightDp(): Int =
    androidx.compose.ui.platform.LocalConfiguration.current.screenHeightDp

private fun Modifier.clickableNoRipple(
    interactionSource: MutableInteractionSource,
    onClick: () -> Unit,
): Modifier = this.then(
    Modifier.clickable(
        interactionSource = interactionSource,
        indication = null,
        onClick = onClick,
    )
)
