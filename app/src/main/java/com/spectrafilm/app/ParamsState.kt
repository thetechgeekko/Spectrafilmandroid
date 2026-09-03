/*
 * Spektrafilm for Android — editable parameter state. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * A flat, Compose-observable mirror of the full SpektraParams tree. Edits update
 * the individual mutableState fields; toParams() rebuilds an immutable SpektraParams
 * and fromParams() loads one back. The grouping/field set tracks the spektrafilm GUI
 * (widget_sections.py / state.py).
 *
 * Unit note: the GUI (and these sliders) express halation.scatter_tail_weight and
 * halation.halation_strength in 0-100 percent, while the engine stores fractions
 * (params_mapper.py divides by 100). Those fields are kept in PERCENT here and
 * converted in toParams()/fromParams().
 */
package com.spectrafilm.app

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.spectrafilm.app.masks.LocalAdjustment
import com.spectrafilm.engine.CameraParams
import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.DiffusionFilterParams
import com.spectrafilm.engine.DirCouplersParams
import com.spectrafilm.engine.EnlargerParams
import com.spectrafilm.engine.FilmRenderingParams
import com.spectrafilm.engine.GlareParams
import com.spectrafilm.engine.GrainParams
import com.spectrafilm.engine.HalationParams
import com.spectrafilm.engine.InputGamutCompress
import com.spectrafilm.engine.IoParams
import com.spectrafilm.engine.OutputGamutCompress
import com.spectrafilm.engine.PrintCurvesMorphParams
import com.spectrafilm.engine.PrintRenderingParams
import com.spectrafilm.engine.Rgb2Raw
import com.spectrafilm.engine.ScannerParams
import com.spectrafilm.engine.SettingsParams
import com.spectrafilm.engine.SpektraParams
import com.spectrafilm.engine.ToneCurveChannel
import com.spectrafilm.engine.ToneCurveParams
import com.spectrafilm.libraw.WhiteBalance

/** Input color-space option labels matching spektrafilm's RGBColorSpaces enum values. */
val INPUT_COLOR_SPACES = listOf(
    "sRGB", "DCI-P3", "Display P3", "Adobe RGB (1998)",
    "ITU-R BT.2020", "ProPhoto RGB", "ACES2065-1",
)

/** Diffusion-filter PSF families (DiffusionFilterFamilies). */
val DIFFUSION_FAMILIES = listOf("glimmerglass", "black_pro_mist", "pro_mist", "cinebloom")

/** Auto-exposure metering methods (AutoExposureMethods). */
val AUTO_EXPOSURE_METHODS = listOf(
    "center_weighted", "matrix", "multi_zone", "partial",
    "highlight_weighted", "median", "average",
)

/**
 * Flat Compose-state mirror of SpektraParams. Construct from a SpektraParams via
 * [loadFrom]; read back with [toParams].
 */
class ParamsState {
    // --- profiles (Simulation) ---
    var filmProfile by mutableStateOf("kodak_portra_400")
    var printProfile by mutableStateOf("kodak_portra_endura")

    // --- Input ---
    var inputColorSpace by mutableStateOf("ProPhoto RGB")
    var inputCctfDecoding by mutableStateOf(false)
    var spectralUpsampling by mutableStateOf(Rgb2Raw.HANATOS2025)
    var adaptationWindow by mutableStateOf(true)
    var adaptationSurface by mutableStateOf(false)
    var spectralGaussianBlur by mutableFloatStateOf(0f)
    var filterUv by mutableStateOf(Triple(0f, 410f, 8f))
    var filterIr by mutableStateOf(Triple(0f, 675f, 15f))
    var upscaleFactor by mutableFloatStateOf(1f)
    var crop by mutableStateOf(false)
    var cropCenter by mutableStateOf(0.5f to 0.5f)
    var cropSize by mutableStateOf(0.1f to 0.1f)

    // --- Import Raw ---
    var rawWhiteBalance by mutableStateOf(WhiteBalance.AS_SHOT)
    var rawTemperature by mutableFloatStateOf(5500f)
    var rawTint by mutableFloatStateOf(1f)

    // --- Creative white balance (all sources; pre-engine Bradford CAT on the linear input). ---
    // Relative push in [-100,100]; 0 = identity. Distinct from the RAW-decode WB above (which is a
    // camera-illuminant correction, RAW-only). NOT a SpektraParams field — applied in loadSource.
    var creativeWbTemp by mutableFloatStateOf(0f)
    var creativeWbTint by mutableFloatStateOf(0f)

    // "Balance to film stock" — the virtual 85-filter. When ON, loadSource Bradford-adapts the linear
    // input from the D50 working white to the selected film's reference illuminant (FilmStockBalance),
    // so a tungsten stock (Vision3 200T/500T) renders a daylight scene neutral instead of authentically
    // blue. Default OFF preserves the bit-exact spektrafilm look. NOT a SpektraParams field — a
    // pre-engine input op, so it's part of the decode-cache key (with filmProfile), not toParams().
    var balanceToFilmStock by mutableStateOf(false)

    // Creative Contrast [-100,100]; 0 = identity. NOT a SpektraParams field — composed into the
    // master tone curve in toParams (ContrastCurve), so it drives the wired, parity-gated tone-curve
    // stage. Hue-neutral (master = all channels). Lives in the Tone Curve panel; gated by its switch.
    var contrast by mutableFloatStateOf(0f)

    // Creative Saturation / Vibrance [-100,100]; 0 = identity. NOT SpektraParams fields — applied as a
    // post-engine Oklab chroma grade on the output buffer (ColorGrade), so the engine + parity suite
    // are untouched. Saturation is uniform; Vibrance weights low-chroma (muted) colors more.
    var saturation by mutableFloatStateOf(0f)
    var vibrance by mutableFloatStateOf(0f)

    // ACES-style gamut compression amount [0,100]; 0 = off. NOT a SpektraParams field — a post-engine
    // pass (GamutCompress, via ColorGrade) that softens the harsh cyan/edge fringe on saturated colors.
    var gamutCompress by mutableFloatStateOf(0f)

    // Local-adjustment masks (the masking keystone). NOT a SpektraParams field — composited on the
    // engine OUTPUT (MaskCompositor, via simResultToBitmapGraded), so the engine + parity suite are
    // untouched. Empty = global-only (today's behavior). Round-trips in the recipe "masks" block.
    var localAdjustments by mutableStateOf<List<LocalAdjustment>>(emptyList())

    // --- Simulation / camera ---
    var exposureCompensationEv by mutableFloatStateOf(0f)
    // Default ON to match the upstream spektrafilm pipeline (CameraParams.auto_exposure = True): the
    // metered exposure places the scene correctly on the film's per-channel density curves, so a
    // properly-exposed render comes out balanced instead of skewed (e.g. a tungsten stock going blue).
    var autoExposure by mutableStateOf(true)
    var autoExposureMethod by mutableStateOf("center_weighted")
    var filmFormatMm by mutableFloatStateOf(35f)
    var cameraLensBlurUm by mutableFloatStateOf(0f)
    val cameraDiffusionState = DiffusionState()

    // --- Simulation / enlarger (print) ---
    var printIlluminant by mutableStateOf("TH-KG3")
    var printExposure by mutableFloatStateOf(1f)
    var printExposureCompensation by mutableStateOf(true)
    var printYFilterShift by mutableFloatStateOf(0f)
    var printMFilterShift by mutableFloatStateOf(0f)
    var enlargerLensBlur by mutableFloatStateOf(0f)
    val printDiffusionState = DiffusionState()

    // --- Preflash ---
    var preflashExposure by mutableFloatStateOf(0f)
    var preflashYFilterShift by mutableFloatStateOf(0f)
    var preflashMFilterShift by mutableFloatStateOf(0f)

    // --- Scanner ---
    var scanLensBlur by mutableFloatStateOf(0f)
    var scanWhiteCorrection by mutableStateOf(false)
    var scanWhiteLevel by mutableFloatStateOf(0.98f)
    var scanBlackCorrection by mutableStateOf(false)
    var scanBlackLevel by mutableFloatStateOf(0.01f)
    var scanUnsharpMask by mutableStateOf(0.7f to 0.7f)

    // --- Output / saving ---
    var outputColorSpace by mutableStateOf(ColorSpace.SRGB)
    var savingCctfEncoding by mutableStateOf(true)
    // Opt-in gamut compression (default-off => byte-identical to the parity oracle).
    var outputGamutCompress by mutableStateOf(OutputGamutCompress.LEGACY_CLIP)
    var inputGamutCompress by mutableStateOf(InputGamutCompress.OFF)
    var scanFilm by mutableStateOf(false)

    // --- Grain ---
    var grainActive by mutableStateOf(true)
    var grainSublayersActive by mutableStateOf(true)
    var grainParticleAreaUm2 by mutableFloatStateOf(0.2f)
    var grainParticleScale by mutableStateOf(Triple(0.8f, 1f, 2f))
    var grainParticleScaleLayers by mutableStateOf(Triple(2.5f, 1f, 0.5f))
    var grainDensityMin by mutableStateOf(Triple(0.07f, 0.08f, 0.12f))
    var grainUniformity by mutableStateOf(Triple(0.97f, 0.97f, 0.99f))
    var grainBlur by mutableFloatStateOf(0.65f)
    var grainBlurDyeCloudsUm by mutableFloatStateOf(1f)
    var grainMicroStructure by mutableStateOf(0.2f to 30f)
    var grainNSubLayers by mutableIntStateOf(1)

    // --- Halation (strengths/tail-weight in PERCENT 0-100) ---
    var halationActive by mutableStateOf(true)
    var halScatterAmount by mutableFloatStateOf(1f)
    var halScatterSpatialScale by mutableFloatStateOf(1f)
    var halHalationAmount by mutableFloatStateOf(1f)
    var halHalationSpatialScale by mutableFloatStateOf(1f)
    var halBoostEv by mutableFloatStateOf(0f)
    var halProtectEv by mutableFloatStateOf(4f)
    var halBoostRange by mutableFloatStateOf(0.3f)
    var halScatterCoreUm by mutableStateOf(Triple(2.2f, 2f, 1.6f))
    var halScatterTailUm by mutableStateOf(Triple(9.3f, 9.7f, 9.1f))
    var halScatterTailWeightPct by mutableStateOf(Triple(78f, 65f, 67f))
    var halHalationStrengthPct by mutableStateOf(Triple(5f, 1.5f, 0f))
    var halFirstSigmaUm by mutableStateOf(Triple(65f, 65f, 65f))
    var halNBounces by mutableIntStateOf(3)
    var halBounceDecay by mutableFloatStateOf(0.5f)
    var halRenormalize by mutableStateOf(true)

    // --- Couplers (DIR) ---
    var couplersActive by mutableStateOf(true)
    var couplersAmount by mutableFloatStateOf(1f)
    var couplersInhibitionSamelayer by mutableFloatStateOf(1f)
    var couplersInhibitionInterlayer by mutableFloatStateOf(1f)
    var couplersGammaSamelayer by mutableStateOf(Triple(0.341f, 0.324f, 0.273f))
    var couplersGammaRtoGb by mutableStateOf(0.355f to 0.305f)
    var couplersGammaGtoRb by mutableStateOf(0.154f to 0.358f)
    var couplersGammaBtoRg by mutableStateOf(0.171f to 0.225f)
    var couplersDiffusionSizeUm by mutableFloatStateOf(20f)
    var couplersDiffusionTailUm by mutableFloatStateOf(200f)
    var couplersDiffusionTailWeight by mutableFloatStateOf(0.06f)

    // --- Glare (print) ---
    var glareActive by mutableStateOf(true)
    var glarePercent by mutableFloatStateOf(0.03f)
    var glareRoughness by mutableFloatStateOf(0.7f)
    var glareBlur by mutableFloatStateOf(0.5f)

    // --- Experimental ---
    var filmGammaFactor by mutableFloatStateOf(1f)
    var printGammaFactor by mutableFloatStateOf(1f)

    // --- Print curve morph (s023, opt-in) — default off + identity is a strict
    // no-op (the engine uses the stored print density-curve table). ---
    var morphActive by mutableStateOf(false)
    var morphGammaFactor by mutableFloatStateOf(1f)
    var morphGammaFactorFast by mutableFloatStateOf(1f)
    var morphGammaFactorSlow by mutableFloatStateOf(1f)
    var morphGammaFactorRed by mutableFloatStateOf(1f)
    var morphGammaFactorGreen by mutableFloatStateOf(1f)
    var morphGammaFactorBlue by mutableFloatStateOf(1f)
    var morphDeveloperExhaustion by mutableFloatStateOf(0f)

    // --- Tone curve (Lightroom-style, applied to final display RGB) ---
    // Points are (x, y) in [0,1], x increasing; < 2 points = identity. Inactive by
    // default => the engine skips the stage (bit-exact with no curve).
    var toneCurveActive by mutableStateOf(false)
    var toneCurveMaster by mutableStateOf<List<Pair<Float, Float>>>(emptyList())
    var toneCurveRed by mutableStateOf<List<Pair<Float, Float>>>(emptyList())
    var toneCurveGreen by mutableStateOf<List<Pair<Float, Float>>>(emptyList())
    var toneCurveBlue by mutableStateOf<List<Pair<Float, Float>>>(emptyList())

    // --- Display / settings ---

    /**
     * Long edge of the interactive preview. Device/app-level setting seeded from
     * AppSettings.previewMaxSize (NOT preset/recipe state — loadFrom leaves it
     * alone, exactly like [gpuEngine] and [gpuExport] below).
     *
     * There were FOUR paths that replayed a stored value over the user's setting, and
     * removing only the obvious one did not fix it. Recipes are keyed by source uri
     * and auto-applied on open, so a real photo always got its stale value back while
     * the demo image (which has no recipe) honoured the setting — which is why the bug
     * presented as "inert for RAW only". The live path was NOT this class:
     *
     *   Recipes.load -> Presets.decode -> the "display" block in Presets   (the read
     *                                                                       that bit)
     *   Presets.encode "display"                                           (the write)
     *   BuiltInPresets                                                     (a 3rd copy)
     *   ParamsState.loadFrom                                               (this one)
     *
     * All four are gone. Dropping the READ is what disarms the recipes already on
     * disk, which keep carrying the field. It is still WRITTEN into the engine params
     * by toParams, exactly as gpuPreview/gpuExport are.
     */
    var previewMaxSize by mutableIntStateOf(640)

    /**
     * GPU engine preview (Vulkan, #146). Device/app-level setting seeded from
     * AppSettings.gpuEngine (NOT preset/recipe state — loadFrom leaves it
     * alone). Preview renders only; export ignores it inside the engine.
     */
    var gpuEngine by mutableStateOf(false)

    /**
     * EXPERIMENTAL GPU export (Vulkan, #154). Device/app-level setting seeded
     * from AppSettings.gpuExportEngine (NOT preset/recipe state). Applies to
     * full-resolution exports; independent of [gpuEngine].
     */
    var gpuExport by mutableStateOf(false)

    /** Reset to the engine defaults for the given profile pair. */
    fun loadFrom(p: SpektraParams) {
        filmProfile = p.filmProfile
        printProfile = p.printProfile

        inputColorSpace = p.io.inputColorSpace
        inputCctfDecoding = p.io.inputCctfDecoding
        spectralUpsampling = p.settings.rgbToRawMethod
        adaptationWindow = p.settings.applyHanatos2025AdaptationWindow
        adaptationSurface = p.settings.applyHanatos2025AdaptationSurface
        spectralGaussianBlur = p.settings.spectralGaussianBlur
        filterUv = p.camera.filterUv
        filterIr = p.camera.filterIr
        upscaleFactor = p.io.upscaleFactor
        crop = p.io.crop
        cropCenter = p.io.cropCenter
        cropSize = p.io.cropSize

        exposureCompensationEv = p.camera.exposureCompensationEv
        autoExposure = p.camera.autoExposure
        autoExposureMethod = p.camera.autoExposureMethod
        filmFormatMm = p.camera.filmFormatMm
        cameraLensBlurUm = p.camera.lensBlurUm
        cameraDiffusionState.loadFrom(p.camera.diffusionFilter)

        printIlluminant = p.enlarger.illuminant
        printExposure = p.enlarger.printExposure
        printExposureCompensation = p.enlarger.printExposureCompensation
        printYFilterShift = p.enlarger.yFilterShift
        printMFilterShift = p.enlarger.mFilterShift
        enlargerLensBlur = p.enlarger.lensBlur
        printDiffusionState.loadFrom(p.enlarger.diffusionFilter)

        preflashExposure = p.enlarger.preflashExposure
        preflashYFilterShift = p.enlarger.preflashYFilterShift
        preflashMFilterShift = p.enlarger.preflashMFilterShift

        scanLensBlur = p.scanner.lensBlur
        scanWhiteCorrection = p.scanner.whiteCorrection
        scanWhiteLevel = p.scanner.whiteLevel
        scanBlackCorrection = p.scanner.blackCorrection
        scanBlackLevel = p.scanner.blackLevel
        scanUnsharpMask = p.scanner.unsharpMask

        outputColorSpace = p.io.outputColorSpace
        savingCctfEncoding = p.io.outputCctfEncoding
        outputGamutCompress = p.io.outputGamutCompress
        inputGamutCompress = p.io.inputGamutCompress
        scanFilm = p.io.scanFilm

        val g = p.filmRender.grain
        grainActive = g.active
        grainSublayersActive = g.sublayersActive
        grainParticleAreaUm2 = g.agxParticleAreaUm2
        grainParticleScale = g.agxParticleScale
        grainParticleScaleLayers = g.agxParticleScaleLayers
        grainDensityMin = g.densityMin
        grainUniformity = g.uniformity
        grainBlur = g.blur
        grainBlurDyeCloudsUm = g.blurDyeCloudsUm
        grainMicroStructure = g.microStructure
        grainNSubLayers = g.nSubLayers

        val h = p.filmRender.halation
        halationActive = h.active
        halScatterAmount = h.scatterAmount
        halScatterSpatialScale = h.scatterSpatialScale
        halHalationAmount = h.halationAmount
        halHalationSpatialScale = h.halationSpatialScale
        halBoostEv = h.boostEv
        halProtectEv = h.protectEv
        halBoostRange = h.boostRange
        halScatterCoreUm = h.scatterCoreUm
        halScatterTailUm = h.scatterTailUm
        halScatterTailWeightPct = h.scatterTailWeight.times100()
        halHalationStrengthPct = h.halationStrength.times100()
        halFirstSigmaUm = h.halationFirstSigmaUm
        halNBounces = h.halationNBounces
        halBounceDecay = h.halationBounceDecay
        halRenormalize = h.halationRenormalize

        val c = p.filmRender.dirCouplers
        couplersActive = c.active
        couplersAmount = c.amount
        couplersInhibitionSamelayer = c.inhibitionSamelayer
        couplersInhibitionInterlayer = c.inhibitionInterlayer
        couplersGammaSamelayer = c.gammaSamelayerRgb
        couplersGammaRtoGb = c.gammaInterlayerRToGb
        couplersGammaGtoRb = c.gammaInterlayerGToRb
        couplersGammaBtoRg = c.gammaInterlayerBToRg
        couplersDiffusionSizeUm = c.diffusionSizeUm
        couplersDiffusionTailUm = c.diffusionTailUm
        couplersDiffusionTailWeight = c.diffusionTailWeight

        val gl = p.printRender.glare
        glareActive = gl.active
        glarePercent = gl.percent
        glareRoughness = gl.roughness
        glareBlur = gl.blur

        filmGammaFactor = p.filmRender.densityCurveGamma
        printGammaFactor = p.printRender.densityCurveGamma

        val pm = p.printRender.densityCurvesMorph
        morphActive = pm.active
        morphGammaFactor = pm.gammaFactor
        morphGammaFactorFast = pm.gammaFactorFast
        morphGammaFactorSlow = pm.gammaFactorSlow
        morphGammaFactorRed = pm.gammaFactorRed
        morphGammaFactorGreen = pm.gammaFactorGreen
        morphGammaFactorBlue = pm.gammaFactorBlue
        morphDeveloperExhaustion = pm.developerExhaustion

        val tc = p.toneCurve
        toneCurveActive = tc.active
        toneCurveMaster = tc.master.points
        toneCurveRed = tc.red.points
        toneCurveGreen = tc.green.points
        toneCurveBlue = tc.blue.points
    }

    /** Build an immutable SpektraParams from current state. */
    // [filmFormatMmOverride] lets the zoom/magnifier crop paths feed the engine an EFFECTIVE film
    // format so its `pixel_size_um = film_format_mm*1000/max(w,h)` derivation reflects the crop's
    // true physical extent (a crop covers only a fraction of the frame). Without it the engine
    // treats e.g. an 800px crop as if 800px spanned the whole 35mm frame, so grain/halation (both
    // µm-based) come out too weak even when zoomed in. The full-frame preview/export pass null and
    // are unaffected. See renderRoi/openMagnifier in MainActivity.
    fun toParams(
        previewMaxSizeOverride: Int? = null,
        filmFormatMmOverride: Float? = null,
        // Interactive-preview optimisation (Lightroom-style "grain at 100%"): drop the per-pixel
        // grain + the spatial halation/diffusion branch — the dominant preview cost, ~invisible at
        // fit/draft resolution. The live draft AND the full fit settle set this; the 100% zoom ROI,
        // the magnifier and EXPORT do NOT, so grain/halation still render where they're visible.
        // Preview-only — never set on the export path, so parity is unaffected. Couplers stay on.
        skipGrainHalation: Boolean = false,
    ): SpektraParams = SpektraParams(
        filmProfile = filmProfile,
        printProfile = printProfile,
        camera = CameraParams(
            exposureCompensationEv = exposureCompensationEv,
            autoExposure = autoExposure,
            autoExposureMethod = autoExposureMethod,
            lensBlurUm = cameraLensBlurUm,
            filmFormatMm = filmFormatMmOverride ?: filmFormatMm,
            filterUv = filterUv,
            filterIr = filterIr,
            diffusionFilter = cameraDiffusionState.toParams(),
        ),
        enlarger = EnlargerParams(
            illuminant = printIlluminant,
            printExposure = printExposure,
            printExposureCompensation = printExposureCompensation,
            yFilterShift = printYFilterShift,
            mFilterShift = printMFilterShift,
            lensBlur = enlargerLensBlur,
            diffusionFilter = printDiffusionState.toParams(),
            preflashExposure = preflashExposure,
            preflashYFilterShift = preflashYFilterShift,
            preflashMFilterShift = preflashMFilterShift,
        ),
        scanner = ScannerParams(
            lensBlur = scanLensBlur,
            whiteCorrection = scanWhiteCorrection,
            blackCorrection = scanBlackCorrection,
            whiteLevel = scanWhiteLevel,
            blackLevel = scanBlackLevel,
            unsharpMask = scanUnsharpMask,
        ),
        io = IoParams(
            inputColorSpace = inputColorSpace,
            inputCctfDecoding = inputCctfDecoding,
            outputColorSpace = outputColorSpace,
            outputCctfEncoding = savingCctfEncoding,
            outputGamutCompress = outputGamutCompress,
            inputGamutCompress = inputGamutCompress,
            crop = crop,
            cropCenter = cropCenter,
            cropSize = cropSize,
            upscaleFactor = upscaleFactor,
            scanFilm = scanFilm,
        ),
        settings = SettingsParams(
            rgbToRawMethod = spectralUpsampling,
            applyHanatos2025AdaptationWindow = adaptationWindow,
            applyHanatos2025AdaptationSurface = adaptationSurface,
            spectralGaussianBlur = spectralGaussianBlur,
            previewMaxSize = previewMaxSizeOverride ?: previewMaxSize,
            gpuPreview = gpuEngine,
            gpuExport = gpuExport,
        ),
        filmRender = FilmRenderingParams(
            densityCurveGamma = filmGammaFactor,
            grain = GrainParams(
                active = grainActive && !skipGrainHalation,
                sublayersActive = grainSublayersActive,
                agxParticleAreaUm2 = grainParticleAreaUm2,
                agxParticleScale = grainParticleScale,
                agxParticleScaleLayers = grainParticleScaleLayers,
                densityMin = grainDensityMin,
                uniformity = grainUniformity,
                blur = grainBlur,
                blurDyeCloudsUm = grainBlurDyeCloudsUm,
                microStructure = grainMicroStructure,
                nSubLayers = grainNSubLayers,
            ),
            halation = HalationParams(
                active = halationActive && !skipGrainHalation,
                scatterAmount = halScatterAmount,
                scatterSpatialScale = halScatterSpatialScale,
                halationAmount = halHalationAmount,
                halationSpatialScale = halHalationSpatialScale,
                scatterCoreUm = halScatterCoreUm,
                scatterTailUm = halScatterTailUm,
                scatterTailWeight = halScatterTailWeightPct.div100(),
                boostEv = halBoostEv,
                boostRange = halBoostRange,
                protectEv = halProtectEv,
                halationStrength = halHalationStrengthPct.div100(),
                halationFirstSigmaUm = halFirstSigmaUm,
                halationNBounces = halNBounces,
                halationBounceDecay = halBounceDecay,
                halationRenormalize = halRenormalize,
            ),
            dirCouplers = DirCouplersParams(
                active = couplersActive,
                amount = couplersAmount,
                inhibitionSamelayer = couplersInhibitionSamelayer,
                inhibitionInterlayer = couplersInhibitionInterlayer,
                gammaSamelayerRgb = couplersGammaSamelayer,
                gammaInterlayerRToGb = couplersGammaRtoGb,
                gammaInterlayerGToRb = couplersGammaGtoRb,
                gammaInterlayerBToRg = couplersGammaBtoRg,
                diffusionSizeUm = couplersDiffusionSizeUm,
                diffusionTailUm = couplersDiffusionTailUm,
                diffusionTailWeight = couplersDiffusionTailWeight,
            ),
            glare = GlareParams(
                active = glareActive,
                percent = glarePercent,
                roughness = glareRoughness,
                blur = glareBlur,
            ),
        ),
        printRender = PrintRenderingParams(
            densityCurveGamma = printGammaFactor,
            densityCurvesMorph = PrintCurvesMorphParams(
                active = morphActive,
                gammaFactor = morphGammaFactor,
                gammaFactorFast = morphGammaFactorFast,
                gammaFactorSlow = morphGammaFactorSlow,
                gammaFactorRed = morphGammaFactorRed,
                gammaFactorGreen = morphGammaFactorGreen,
                gammaFactorBlue = morphGammaFactorBlue,
                developerExhaustion = morphDeveloperExhaustion,
            ),
            glare = GlareParams(
                active = glareActive,
                percent = glarePercent,
                roughness = glareRoughness,
                blur = glareBlur,
            ),
        ),
        toneCurve = ToneCurveParams(
            active = toneCurveActive,
            // Contrast composes UNDER the user's drawn master curve; identity when contrast=0.
            master = ToneCurveChannel(ContrastCurve.composeMaster(toneCurveMaster, contrast)),
            red = ToneCurveChannel(toneCurveRed),
            green = ToneCurveChannel(toneCurveGreen),
            blue = ToneCurveChannel(toneCurveBlue),
        ),
    )

    /**
     * Reset the per-stock character overrides — grain, halation, DIR couplers and the film/print
     * density-curve gamma — to their neutral defaults, so a freshly selected film or paper shows its
     * own baked character instead of the previous stock's manual tweaks. Creative and global edits
     * (exposure, white balance, crop, tone curve, output grade, masks…) are deliberately left
     * untouched.
     *
     * Powers the "Use its defaults" snackbar action on profile switch (onboarding §6h). UI/state only
     * — the engine still receives a complete param set, so this carries no parity impact.
     */
    fun resetStockCharacter() {
        val d = ParamsState()
        // Grain
        grainActive = d.grainActive
        grainSublayersActive = d.grainSublayersActive
        grainParticleAreaUm2 = d.grainParticleAreaUm2
        grainParticleScale = d.grainParticleScale
        grainParticleScaleLayers = d.grainParticleScaleLayers
        grainDensityMin = d.grainDensityMin
        grainUniformity = d.grainUniformity
        grainBlur = d.grainBlur
        grainBlurDyeCloudsUm = d.grainBlurDyeCloudsUm
        grainMicroStructure = d.grainMicroStructure
        grainNSubLayers = d.grainNSubLayers
        // Halation
        halationActive = d.halationActive
        halScatterAmount = d.halScatterAmount
        halScatterSpatialScale = d.halScatterSpatialScale
        halHalationAmount = d.halHalationAmount
        halHalationSpatialScale = d.halHalationSpatialScale
        halBoostEv = d.halBoostEv
        halProtectEv = d.halProtectEv
        halBoostRange = d.halBoostRange
        halScatterCoreUm = d.halScatterCoreUm
        halScatterTailUm = d.halScatterTailUm
        halScatterTailWeightPct = d.halScatterTailWeightPct
        halHalationStrengthPct = d.halHalationStrengthPct
        halFirstSigmaUm = d.halFirstSigmaUm
        halNBounces = d.halNBounces
        halBounceDecay = d.halBounceDecay
        halRenormalize = d.halRenormalize
        // DIR couplers
        couplersActive = d.couplersActive
        couplersAmount = d.couplersAmount
        couplersInhibitionSamelayer = d.couplersInhibitionSamelayer
        couplersInhibitionInterlayer = d.couplersInhibitionInterlayer
        couplersGammaSamelayer = d.couplersGammaSamelayer
        couplersGammaRtoGb = d.couplersGammaRtoGb
        couplersGammaGtoRb = d.couplersGammaGtoRb
        couplersGammaBtoRg = d.couplersGammaBtoRg
        couplersDiffusionSizeUm = d.couplersDiffusionSizeUm
        couplersDiffusionTailUm = d.couplersDiffusionTailUm
        couplersDiffusionTailWeight = d.couplersDiffusionTailWeight
        // Density-curve gamma (Experimental)
        filmGammaFactor = d.filmGammaFactor
        printGammaFactor = d.printGammaFactor
    }
}

/** A diffusion-filter sub-section (camera or print stage). */
class DiffusionState {
    var active by mutableStateOf(false)
    var family by mutableStateOf("black_pro_mist")
    var strength by mutableFloatStateOf(0.5f)
    var spatialScale by mutableFloatStateOf(1f)
    var haloWarmth by mutableFloatStateOf(0f)
    var coreIntensity by mutableFloatStateOf(1f)
    var coreSize by mutableFloatStateOf(1f)
    var haloIntensity by mutableFloatStateOf(1f)
    var haloSize by mutableFloatStateOf(1f)
    var bloomIntensity by mutableFloatStateOf(1f)
    var bloomSize by mutableFloatStateOf(1f)

    fun loadFrom(d: DiffusionFilterParams) {
        active = d.active; family = d.filterFamily; strength = d.strength
        spatialScale = d.spatialScale; haloWarmth = d.haloWarmth
        coreIntensity = d.coreIntensity; coreSize = d.coreSize
        haloIntensity = d.haloIntensity; haloSize = d.haloSize
        bloomIntensity = d.bloomIntensity; bloomSize = d.bloomSize
    }

    fun toParams() = DiffusionFilterParams(
        active = active, filterFamily = family, strength = strength,
        spatialScale = spatialScale, haloWarmth = haloWarmth,
        coreIntensity = coreIntensity, coreSize = coreSize,
        haloIntensity = haloIntensity, haloSize = haloSize,
        bloomIntensity = bloomIntensity, bloomSize = bloomSize,
    )
}

private fun Triple<Float, Float, Float>.times100() =
    Triple(first * 100f, second * 100f, third * 100f)

private fun Triple<Float, Float, Float>.div100() =
    Triple(first / 100f, second / 100f, third / 100f)
