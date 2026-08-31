/*
 * Spektrafilm for Android — engine parameters.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 *
 * 1:1 mirror of spektrafilm's RuntimePhotoParams tree
 * (src/spektrafilm/runtime/params_schema.py). Defaults match upstream so that, for a
 * given profile pair, SpektraParams() reproduces spektrafilm's out-of-the-box look.
 * The JNI bridge marshals these into the flat `spk_params` C struct.
 */
package com.spectrafilm.engine

/** Output color space, mirrors io.output_color_space + spk_color_space. */
enum class ColorSpace { SRGB, ADOBE_RGB, PROPHOTO, REC2020, ACES2065_1, LINEAR_SRGB }

/**
 * Output gamut compression algorithm (opt-in, default-off). Ordinals MUST match the C++
 * enum class OutputGamutCompress (model/gamut_compression.h) and the int32 the JNI
 * marshaller reads:
 *   LEGACY_CLIP=0 — DEFAULT. The engine's existing final clip in scanning; byte-identical
 *                   to every parity golden.
 *   OFF=1         — oracle "off": output RGB passes through with no compression and no clip.
 *   ACES_RGC=2    — ACES Reference Gamut Compression v1.3 (per-channel knee on the
 *                   achromatic distance) in the linear output space.
 *   OKLCH=3       — perceptual-hue-preserving chroma compression at constant Oklch (L, h):
 *                   a Reinhard knee on C / C_max(L, h), where the per-output-space gamut
 *                   boundary C_max is regenerated in-engine by bisection.
 *   OKLRAB=4      — the OKLCH reduction with the C_max lookup indexed by Ottosson's rebased
 *                   lightness Lr = f(L) (a monotonic remap of OkLab L toward CIELAB L*), for
 *                   a more perceptually uniform knee across light/dark. Same per-pixel cost.
 * The remaining perceptual algos (jzazbz/cam16ucs, C++ ordinals 5..6) are reserved upstream
 * and not yet ported, so they are intentionally absent here.
 */
enum class OutputGamutCompress { LEGACY_CLIP, OFF, ACES_RGC, OKLCH, OKLRAB }

/**
 * Input gamut compression algorithm (opt-in, default-off). Ordinals MUST match the C++
 * enum class InputGamutCompress:
 *   OFF=0 — DEFAULT. The Hanatos filming tc_lut is built exactly as before; byte-identical.
 *   XY=1  — ACES-RGC-style radial compression in CIE 1931 chromaticity from the film
 *           reference illuminant toward the visible spectral locus, baked into the tc_lut.
 * The oklch variant (C++ ordinal 2) is reserved/unported and intentionally absent.
 */
enum class InputGamutCompress { OFF, XY }

/** RGB→spectral upsampling method, mirrors settings.rgb_to_raw_method. */
enum class Rgb2Raw { HANATOS2025, MALLETT2019 }

/** White-balance mode for RAW import (GUI 'import raw'). */
enum class WhiteBalance { AS_SHOT, DAYLIGHT, TUNGSTEN, CUSTOM }

data class CameraParams(
    val exposureCompensationEv: Float = 0.0f,
    val autoExposure: Boolean = true,
    val autoExposureMethod: String = "center_weighted",
    val lensBlurUm: Float = 0.0f,
    val filmFormatMm: Float = 35.0f,
    val filterUv: Triple<Float, Float, Float> = Triple(0.0f, 410.0f, 8.0f),
    val filterIr: Triple<Float, Float, Float> = Triple(0.0f, 675.0f, 15.0f),
    val diffusionFilter: DiffusionFilterParams = DiffusionFilterParams(),
)

data class EnlargerParams(
    /** Native currently has the TH-KG3 SPD; unsupported IDs fail closed. */
    val illuminant: String = "TH-KG3",
    val printExposure: Float = 1.0f,
    val printExposureCompensation: Boolean = true,
    val normalizePrintExposure: Boolean = true,
    val yFilterShift: Float = 0.0f,
    val mFilterShift: Float = 0.0f,
    val yFilterNeutral: Float = 55f, // kodak cc values
    val mFilterNeutral: Float = 65f,
    val cFilterNeutral: Float = 0f,
    val lensBlur: Float = 0.0f,
    val diffusionFilter: DiffusionFilterParams = DiffusionFilterParams(),
    val preflashExposure: Float = 0.0f,
    val preflashYFilterShift: Float = 0.0f,
    val preflashMFilterShift: Float = 0.0f,
)

data class ScannerParams(
    val lensBlur: Float = 0.0f,
    val whiteCorrection: Boolean = false,
    val blackCorrection: Boolean = false,
    val whiteLevel: Float = 0.98f,
    val blackLevel: Float = 0.01f,
    val unsharpMask: Pair<Float, Float> = 0.7f to 0.7f,
)

data class GrainParams(
    val active: Boolean = true,
    val sublayersActive: Boolean = true,
    val agxParticleAreaUm2: Float = 0.2f,
    val agxParticleScale: Triple<Float, Float, Float> = Triple(0.8f, 1.0f, 2.0f),
    val agxParticleScaleLayers: Triple<Float, Float, Float> = Triple(2.5f, 1.0f, 0.5f),
    val densityMin: Triple<Float, Float, Float> = Triple(0.07f, 0.08f, 0.12f),
    val uniformity: Triple<Float, Float, Float> = Triple(0.97f, 0.97f, 0.99f),
    val blur: Float = 0.65f,
    val blurDyeCloudsUm: Float = 1.0f,
    val microStructure: Pair<Float, Float> = 0.2f to 30f,
    val nSubLayers: Int = 1,
)

data class HalationParams(
    val active: Boolean = true,
    val scatterAmount: Float = 1.0f,
    val scatterSpatialScale: Float = 1.0f,
    val halationAmount: Float = 1.0f,
    val halationSpatialScale: Float = 1.0f,
    val scatterCoreUm: Triple<Float, Float, Float> = Triple(2.2f, 2.0f, 1.6f),
    val scatterTailUm: Triple<Float, Float, Float> = Triple(9.3f, 9.7f, 9.1f),
    val scatterTailWeight: Triple<Float, Float, Float> = Triple(0.78f, 0.65f, 0.67f),
    val boostEv: Float = 0.0f,
    val boostRange: Float = 0.3f,
    val protectEv: Float = 4.0f,
    val halationStrength: Triple<Float, Float, Float> = Triple(0.05f, 0.015f, 0.0f),
    val halationFirstSigmaUm: Triple<Float, Float, Float> = Triple(65.0f, 65.0f, 65.0f),
    val halationNBounces: Int = 3,
    val halationBounceDecay: Float = 0.5f,
    val halationRenormalize: Boolean = true,
)

data class DirCouplersParams(
    val active: Boolean = true,
    val amount: Float = 1.0f,
    val inhibitionSamelayer: Float = 1.0f,
    val inhibitionInterlayer: Float = 1.0f,
    val gammaSamelayerRgb: Triple<Float, Float, Float> = Triple(0.341f, 0.324f, 0.273f),
    val gammaInterlayerRToGb: Pair<Float, Float> = 0.355f to 0.305f,
    val gammaInterlayerGToRb: Pair<Float, Float> = 0.154f to 0.358f,
    val gammaInterlayerBToRg: Pair<Float, Float> = 0.171f to 0.225f,
    val diffusionSizeUm: Float = 20.0f,
    val diffusionTailUm: Float = 200.0f,
    val diffusionTailWeight: Float = 0.06f,
)

data class GlareParams(
    val active: Boolean = true,
    val percent: Float = 0.03f,
    val roughness: Float = 0.7f,
    val blur: Float = 0.5f,
)

data class DiffusionFilterParams(
    val active: Boolean = false,
    /** glimmerglass, black_pro_mist, pro_mist, or cinebloom. */
    val filterFamily: String = "black_pro_mist",
    val strength: Float = 0.5f,
    val spatialScale: Float = 1.0f,
    val haloWarmth: Float = 0.0f,
    val coreIntensity: Float = 1.0f,
    val coreSize: Float = 1.0f,
    val haloIntensity: Float = 1.0f,
    val haloSize: Float = 1.0f,
    val bloomIntensity: Float = 1.0f,
    val bloomSize: Float = 1.0f,
)

data class FilmRenderingParams(
    val densityCurveGamma: Float = 1.0f,
    val grain: GrainParams = GrainParams(),
    val halation: HalationParams = HalationParams(),
    val dirCouplers: DirCouplersParams = DirCouplersParams(),
    val glare: GlareParams = GlareParams(),
)

// OPT-IN s023 print density-curve morph (print_render.density_curves_morph).
// `active` defaults false -> the engine uses the stored print density_curves table
// (byte-identical to the parity goldens). When active, the print density table is
// rebuilt from the paper's density_curves_model and morphed by a coupled gamma
// (global x band(fast/slow) x RGB) plus a developer-exhaustion Gumbel blend.
data class PrintCurvesMorphParams(
    val active: Boolean = false,
    val gammaFactor: Float = 1.0f,
    val gammaFactorFast: Float = 1.0f,
    val gammaFactorSlow: Float = 1.0f,
    val gammaFactorRed: Float = 1.0f,
    val gammaFactorGreen: Float = 1.0f,
    val gammaFactorBlue: Float = 1.0f,
    val developerExhaustion: Float = 0.0f,
)

data class PrintRenderingParams(
    val densityCurveGamma: Float = 1.0f,
    val glare: GlareParams = GlareParams(),
    val densityCurvesMorph: PrintCurvesMorphParams = PrintCurvesMorphParams(),
)

data class IoParams(
    /** Native currently accepts ProPhoto RGB (linear) and rejects other spaces. */
    val inputColorSpace: String = "ProPhoto RGB",
    val inputCctfDecoding: Boolean = false,
    val outputColorSpace: ColorSpace = ColorSpace.SRGB,
    val outputCctfEncoding: Boolean = true,
    /** Opt-in output gamut compression (default LEGACY_CLIP => byte-identical render). */
    val outputGamutCompress: OutputGamutCompress = OutputGamutCompress.LEGACY_CLIP,
    /** Opt-in input (filming-side) gamut compression (default OFF => byte-identical). */
    val inputGamutCompress: InputGamutCompress = InputGamutCompress.OFF,
    val crop: Boolean = false,
    val cropCenter: Pair<Float, Float> = 0.5f to 0.5f,
    val cropSize: Pair<Float, Float> = 0.1f to 0.1f,
    val upscaleFactor: Float = 1.0f,
    /** scan negative directly, skipping the print stage (RuntimePhotoParams.io.scan_film). */
    val scanFilm: Boolean = false,
)

data class SettingsParams(
    // The former useFastStats and previewMode knobs are intentionally absent:
    // native has no alternate stats implementation, and preview is selected by
    // SpektraEngine's explicit preview entry point. A repository-wide source
    // search found no callers, so removing them avoids exposing inert controls.
    val rgbToRawMethod: Rgb2Raw = Rgb2Raw.HANATOS2025,
    val applyHanatos2025AdaptationWindow: Boolean = true,
    val applyHanatos2025AdaptationSurface: Boolean = false,
    val spectralGaussianBlur: Float = 0.0f,
    val useEnlargerLut: Boolean = false,
    val useScannerLut: Boolean = false,
    val lutResolution: Int = 17,
    val previewMaxSize: Int = 640,
    val neutralPrintFiltersFromDatabase: Boolean = true,
    /**
     * GPU preview fast-path (GPU M1, #146). When true, interactive preview
     * renders (`simulate_preview` only) run the scan stage's spectral integral
     * on the GPU via Vulkan compute, after a one-time on-device self-check
     * against the CPU engine; any failure falls back to the CPU per frame.
     * Default false.
     */
    val gpuPreview: Boolean = false,
    /**
     * EXPERIMENTAL GPU export (#154, #149 option B). When true, `simulate`
     * (export) also routes the scan stage through the GPU under the same
     * on-device self-check + CPU fallback ("oracle-verified on your device").
     * Full-res exports are dispatched in slices. Independent of [gpuPreview];
     * default false, so a plain export is byte-identical to the CPU engine.
     */
    val gpuExport: Boolean = false,
)

/** Max control points per tone-curve channel; mirrors SPK_TONE_MAX_PTS in spektra.h. */
const val TONE_CURVE_MAX_POINTS = 16

/**
 * One tone-curve channel: control points (x, y in [0,1], x strictly increasing).
 * Fewer than 2 points is an identity (no-op) channel.
 */
data class ToneCurveChannel(val points: List<Pair<Float, Float>> = emptyList())

/**
 * Lightroom-style point tone curve on the final display-referred RGB. [active] gates
 * the whole stage (default off => the engine skips it and stays bit-exact). [master]
 * applies to all channels; [red]/[green]/[blue] apply per channel first.
 */
data class ToneCurveParams(
    val active: Boolean = false,
    val master: ToneCurveChannel = ToneCurveChannel(),
    val red: ToneCurveChannel = ToneCurveChannel(),
    val green: ToneCurveChannel = ToneCurveChannel(),
    val blue: ToneCurveChannel = ToneCurveChannel(),
)

/**
 * Top-level engine parameters, mirroring RuntimePhotoParams. `filmProfile` / `printProfile`
 * are profile ids resolved from bundled assets (see docs/ASSETS.md).
 */
data class SpektraParams(
    val filmProfile: String,
    val printProfile: String,
    val filmRender: FilmRenderingParams = FilmRenderingParams(),
    val printRender: PrintRenderingParams = PrintRenderingParams(),
    val camera: CameraParams = CameraParams(),
    val enlarger: EnlargerParams = EnlargerParams(),
    val scanner: ScannerParams = ScannerParams(),
    val io: IoParams = IoParams(),
    val settings: SettingsParams = SettingsParams(),
    val toneCurve: ToneCurveParams = ToneCurveParams(),
) {
    /**
     * Pack the tone curve into a flat float[] for the JNI bridge:
     *   [active, masterN, (x,y)*masterN, redN, (x,y)*redN, greenN, ..., blueN, ...]
     * Each channel is capped at [TONE_CURVE_MAX_POINTS]. Read by spektra_jni's
     * read_tone_curve(). Kept as one array so marshalling is a single JNI call.
     */
    fun toneCurvePacked(): FloatArray {
        val out = ArrayList<Float>(8)
        out.add(if (toneCurve.active) 1f else 0f)
        for (ch in listOf(toneCurve.master, toneCurve.red, toneCurve.green, toneCurve.blue)) {
            val pts = ch.points.take(TONE_CURVE_MAX_POINTS)
            out.add(pts.size.toFloat())
            for ((x, y) in pts) { out.add(x); out.add(y) }
        }
        return out.toFloatArray()
    }
}
