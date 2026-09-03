/*
 * Spektrafilm for Android — bundled built-in presets. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Loads assets/spektra/presets.json (shipped in the engine assets, read here via the
 * AssetManager) and applies a curated film+print look onto a ParamsState. Unlike the
 * user presets in Presets.kt — which round-trip the *full* SpektraParams tree under a
 * flat schema — these built-ins are authored sparsely: each preset's "params" object
 * mirrors the engine's nested SpektraParams (camelCase) and only lists the fields it
 * deliberately sets. Anything omitted retains its current editor value (which is the
 * ParamsState default only on a fresh/reset editor).
 *
 * The nested authored schema differs from Presets' flat schema, so this file owns its
 * own mapping (applyParams). Two unit conventions are handled here: halation
 * scatterTailWeight / halationStrength ship as 0-1 fractions in the engine tree but
 * ParamsState keeps them in 0-100 percent (see ParamsState header), so they are scaled
 * by 100 on the way in.
 */
package com.spectrafilm.app

import android.content.Context
import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.Rgb2Raw
import org.json.JSONArray
import org.json.JSONObject

/** A built-in preset's metadata + its raw authored `params` object. */
data class BuiltInPreset(
    val id: String,
    val name: String,
    val group: String,
    val description: String,
    val params: JSONObject,
)

object BuiltInPresets {

    private const val ASSET_PATH = "spektra/presets.json"

    @Volatile private var cache: List<BuiltInPreset>? = null

    /**
     * Load and parse the bundled presets (cached). Reads off the AssetManager; callers
     * should invoke this from a background dispatcher. Returns an empty list if the asset
     * is missing or malformed rather than throwing.
     */
    fun load(ctx: Context): List<BuiltInPreset> {
        cache?.let { return it }
        val parsed = runCatching {
            val text = ctx.assets.open(ASSET_PATH).use { it.readBytes().decodeToString() }
            val arr = JSONObject(text).optJSONArray("presets") ?: JSONArray()
            buildList {
                for (i in 0 until arr.length()) {
                    val o = arr.optJSONObject(i) ?: continue
                    add(
                        BuiltInPreset(
                            id = o.optString("id"),
                            name = o.optString("name", o.optString("id")),
                            group = o.optString("group", "Presets"),
                            description = o.optString("description", ""),
                            params = o.optJSONObject("params") ?: JSONObject(),
                        ),
                    )
                }
            }
        }.getOrDefault(emptyList())
        cache = parsed
        return parsed
    }

    /** Presets grouped by their `group`, preserving first-seen group order and list order. */
    fun grouped(ctx: Context): Map<String, List<BuiltInPreset>> {
        val out = LinkedHashMap<String, MutableList<BuiltInPreset>>()
        for (p in load(ctx)) out.getOrPut(p.group) { mutableListOf() }.add(p)
        return out
    }

    /** Apply a built-in preset's authored params onto [into]. Omitted fields are untouched. */
    fun apply(preset: BuiltInPreset, into: ParamsState) = applyParams(preset.params, into)

    // --- nested-schema mapping (engine SpektraParams camelCase) -> ParamsState ---

    private fun applyParams(p: JSONObject, s: ParamsState) {
        if (p.has("filmProfile")) s.filmProfile = p.optString("filmProfile", s.filmProfile)
        if (p.has("printProfile")) s.printProfile = p.optString("printProfile", s.printProfile)

        p.optJSONObject("io")?.let { io ->
            if (io.has("inputColorSpace")) s.inputColorSpace = io.optString("inputColorSpace", s.inputColorSpace)
            if (io.has("inputCctfDecoding")) s.inputCctfDecoding = io.optBoolean("inputCctfDecoding", s.inputCctfDecoding)
            if (io.has("outputCctfEncoding")) s.savingCctfEncoding = io.optBoolean("outputCctfEncoding", s.savingCctfEncoding)
            if (io.has("scanFilm")) s.scanFilm = io.optBoolean("scanFilm", s.scanFilm)
            if (io.has("crop")) s.crop = io.optBoolean("crop", s.crop)
            if (io.has("upscaleFactor")) s.upscaleFactor = io.f("upscaleFactor", s.upscaleFactor)
            s.cropCenter = io.pairOf("cropCenter", s.cropCenter)
            s.cropSize = io.pairOf("cropSize", s.cropSize)
            if (io.has("outputColorSpace")) {
                s.outputColorSpace = enumOf(io.optString("outputColorSpace"), ColorSpace.entries, s.outputColorSpace)
            }
        }

        p.optJSONObject("settings")?.let { st ->
            if (st.has("rgbToRawMethod")) {
                s.spectralUpsampling = enumOf(st.optString("rgbToRawMethod"), Rgb2Raw.entries, s.spectralUpsampling)
            }
            if (st.has("applyHanatos2025AdaptationWindow")) s.adaptationWindow = st.optBoolean("applyHanatos2025AdaptationWindow", s.adaptationWindow)
            if (st.has("applyHanatos2025AdaptationSurface")) s.adaptationSurface = st.optBoolean("applyHanatos2025AdaptationSurface", s.adaptationSurface)
            if (st.has("spectralGaussianBlur")) s.spectralGaussianBlur = st.f("spectralGaussianBlur", s.spectralGaussianBlur)
        }

        p.optJSONObject("camera")?.let { c ->
            if (c.has("exposureCompensationEv")) s.exposureCompensationEv = c.f("exposureCompensationEv", s.exposureCompensationEv)
            if (c.has("autoExposure")) s.autoExposure = c.optBoolean("autoExposure", s.autoExposure)
            if (c.has("autoExposureMethod")) s.autoExposureMethod = c.optString("autoExposureMethod", s.autoExposureMethod)
            if (c.has("filmFormatMm")) s.filmFormatMm = c.f("filmFormatMm", s.filmFormatMm)
            if (c.has("lensBlurUm")) s.cameraLensBlurUm = c.f("lensBlurUm", s.cameraLensBlurUm)
            s.filterUv = c.triOf("filterUv", s.filterUv)
            s.filterIr = c.triOf("filterIr", s.filterIr)
            c.optJSONObject("diffusionFilter")?.let { applyDiffusion(it, s.cameraDiffusionState) }
        }

        p.optJSONObject("enlarger")?.let { e ->
            if (e.has("illuminant")) s.printIlluminant = e.optString("illuminant", s.printIlluminant)
            if (e.has("printExposure")) s.printExposure = e.f("printExposure", s.printExposure)
            if (e.has("printExposureCompensation")) s.printExposureCompensation = e.optBoolean("printExposureCompensation", s.printExposureCompensation)
            if (e.has("yFilterShift")) s.printYFilterShift = e.f("yFilterShift", s.printYFilterShift)
            if (e.has("mFilterShift")) s.printMFilterShift = e.f("mFilterShift", s.printMFilterShift)
            if (e.has("lensBlur")) s.enlargerLensBlur = e.f("lensBlur", s.enlargerLensBlur)
            if (e.has("preflashExposure")) s.preflashExposure = e.f("preflashExposure", s.preflashExposure)
            if (e.has("preflashYFilterShift")) s.preflashYFilterShift = e.f("preflashYFilterShift", s.preflashYFilterShift)
            if (e.has("preflashMFilterShift")) s.preflashMFilterShift = e.f("preflashMFilterShift", s.preflashMFilterShift)
            e.optJSONObject("diffusionFilter")?.let { applyDiffusion(it, s.printDiffusionState) }
        }

        p.optJSONObject("scanner")?.let { sc ->
            if (sc.has("lensBlur")) s.scanLensBlur = sc.f("lensBlur", s.scanLensBlur)
            if (sc.has("whiteCorrection")) s.scanWhiteCorrection = sc.optBoolean("whiteCorrection", s.scanWhiteCorrection)
            if (sc.has("whiteLevel")) s.scanWhiteLevel = sc.f("whiteLevel", s.scanWhiteLevel)
            if (sc.has("blackCorrection")) s.scanBlackCorrection = sc.optBoolean("blackCorrection", s.scanBlackCorrection)
            if (sc.has("blackLevel")) s.scanBlackLevel = sc.f("blackLevel", s.scanBlackLevel)
            s.scanUnsharpMask = sc.pairOf("unsharpMask", s.scanUnsharpMask)
        }

        p.optJSONObject("filmRender")?.let { fr ->
            if (fr.has("densityCurveGamma")) s.filmGammaFactor = fr.f("densityCurveGamma", s.filmGammaFactor)

            fr.optJSONObject("grain")?.let { g ->
                if (g.has("active")) s.grainActive = g.optBoolean("active", s.grainActive)
                if (g.has("sublayersActive")) s.grainSublayersActive = g.optBoolean("sublayersActive", s.grainSublayersActive)
                if (g.has("agxParticleAreaUm2")) s.grainParticleAreaUm2 = g.f("agxParticleAreaUm2", s.grainParticleAreaUm2)
                s.grainParticleScale = g.triOf("agxParticleScale", s.grainParticleScale)
                s.grainParticleScaleLayers = g.triOf("agxParticleScaleLayers", s.grainParticleScaleLayers)
                s.grainDensityMin = g.triOf("densityMin", s.grainDensityMin)
                s.grainUniformity = g.triOf("uniformity", s.grainUniformity)
                if (g.has("blur")) s.grainBlur = g.f("blur", s.grainBlur)
                if (g.has("blurDyeCloudsUm")) s.grainBlurDyeCloudsUm = g.f("blurDyeCloudsUm", s.grainBlurDyeCloudsUm)
                s.grainMicroStructure = g.pairOf("microStructure", s.grainMicroStructure)
                if (g.has("nSubLayers")) s.grainNSubLayers = g.optInt("nSubLayers", s.grainNSubLayers)
            }

            fr.optJSONObject("halation")?.let { h ->
                if (h.has("active")) s.halationActive = h.optBoolean("active", s.halationActive)
                if (h.has("scatterAmount")) s.halScatterAmount = h.f("scatterAmount", s.halScatterAmount)
                if (h.has("scatterSpatialScale")) s.halScatterSpatialScale = h.f("scatterSpatialScale", s.halScatterSpatialScale)
                if (h.has("halationAmount")) s.halHalationAmount = h.f("halationAmount", s.halHalationAmount)
                if (h.has("halationSpatialScale")) s.halHalationSpatialScale = h.f("halationSpatialScale", s.halHalationSpatialScale)
                if (h.has("boostEv")) s.halBoostEv = h.f("boostEv", s.halBoostEv)
                if (h.has("protectEv")) s.halProtectEv = h.f("protectEv", s.halProtectEv)
                if (h.has("boostRange")) s.halBoostRange = h.f("boostRange", s.halBoostRange)
                s.halScatterCoreUm = h.triOf("scatterCoreUm", s.halScatterCoreUm)
                s.halScatterTailUm = h.triOf("scatterTailUm", s.halScatterTailUm)
                // engine fraction (0-1) -> ParamsState percent (0-100)
                h.optJSONArray("scatterTailWeight")?.let { s.halScatterTailWeightPct = tripleFromArray(it).times100() }
                h.optJSONArray("halationStrength")?.let { s.halHalationStrengthPct = tripleFromArray(it).times100() }
                s.halFirstSigmaUm = h.triOf("halationFirstSigmaUm", s.halFirstSigmaUm)
                if (h.has("halationNBounces")) s.halNBounces = h.optInt("halationNBounces", s.halNBounces)
                if (h.has("halationBounceDecay")) s.halBounceDecay = h.f("halationBounceDecay", s.halBounceDecay)
                if (h.has("halationRenormalize")) s.halRenormalize = h.optBoolean("halationRenormalize", s.halRenormalize)
            }

            fr.optJSONObject("dirCouplers")?.let { c ->
                if (c.has("active")) s.couplersActive = c.optBoolean("active", s.couplersActive)
                if (c.has("amount")) s.couplersAmount = c.f("amount", s.couplersAmount)
                if (c.has("inhibitionSamelayer")) s.couplersInhibitionSamelayer = c.f("inhibitionSamelayer", s.couplersInhibitionSamelayer)
                if (c.has("inhibitionInterlayer")) s.couplersInhibitionInterlayer = c.f("inhibitionInterlayer", s.couplersInhibitionInterlayer)
                s.couplersGammaSamelayer = c.triOf("gammaSamelayerRgb", s.couplersGammaSamelayer)
                s.couplersGammaRtoGb = c.pairOf("gammaInterlayerRToGb", s.couplersGammaRtoGb)
                s.couplersGammaGtoRb = c.pairOf("gammaInterlayerGToRb", s.couplersGammaGtoRb)
                s.couplersGammaBtoRg = c.pairOf("gammaInterlayerBToRg", s.couplersGammaBtoRg)
                if (c.has("diffusionSizeUm")) s.couplersDiffusionSizeUm = c.f("diffusionSizeUm", s.couplersDiffusionSizeUm)
                if (c.has("diffusionTailUm")) s.couplersDiffusionTailUm = c.f("diffusionTailUm", s.couplersDiffusionTailUm)
                if (c.has("diffusionTailWeight")) s.couplersDiffusionTailWeight = c.f("diffusionTailWeight", s.couplersDiffusionTailWeight)
            }

            fr.optJSONObject("glare")?.let { gl -> applyGlare(gl, s) }
        }

        // glare also lives under printRender in the engine tree; accept either.
        p.optJSONObject("printRender")?.let { pr ->
            if (pr.has("densityCurveGamma")) s.printGammaFactor = pr.f("densityCurveGamma", s.printGammaFactor)
            pr.optJSONObject("glare")?.let { gl -> applyGlare(gl, s) }
        }
        p.optJSONObject("glare")?.let { gl -> applyGlare(gl, s) }

        // Tone curve (final display-RGB stage). Points are [[x,y],…] in [0,1], x increasing;
        // an empty/<2-point channel is identity. Mirrors Presets.kt's flat-schema toneCurve.
        p.optJSONObject("toneCurve")?.let { tc ->
            if (tc.has("active")) s.toneCurveActive = tc.optBoolean("active", s.toneCurveActive)
            tc.optJSONArray("master")?.let { s.toneCurveMaster = pointsOf(it) }
            tc.optJSONArray("red")?.let { s.toneCurveRed = pointsOf(it) }
            tc.optJSONArray("green")?.let { s.toneCurveGreen = pointsOf(it) }
            tc.optJSONArray("blue")?.let { s.toneCurveBlue = pointsOf(it) }
        }
    }

    /**
     * Map an authored `diffusionFilter` object (camera or enlarger) onto a [DiffusionState].
     * Field names mirror the engine SpektraParams tree (`filterFamily`); `family` is also
     * accepted for symmetry with the flat user-preset schema in Presets.kt.
     */
    private fun applyDiffusion(o: JSONObject, d: DiffusionState) {
        if (o.has("active")) d.active = o.optBoolean("active", d.active)
        when {
            o.has("filterFamily") -> d.family = o.optString("filterFamily", d.family)
            o.has("family") -> d.family = o.optString("family", d.family)
        }
        if (o.has("strength")) d.strength = o.f("strength", d.strength)
        if (o.has("spatialScale")) d.spatialScale = o.f("spatialScale", d.spatialScale)
        if (o.has("haloWarmth")) d.haloWarmth = o.f("haloWarmth", d.haloWarmth)
        if (o.has("coreIntensity")) d.coreIntensity = o.f("coreIntensity", d.coreIntensity)
        if (o.has("coreSize")) d.coreSize = o.f("coreSize", d.coreSize)
        if (o.has("haloIntensity")) d.haloIntensity = o.f("haloIntensity", d.haloIntensity)
        if (o.has("haloSize")) d.haloSize = o.f("haloSize", d.haloSize)
        if (o.has("bloomIntensity")) d.bloomIntensity = o.f("bloomIntensity", d.bloomIntensity)
        if (o.has("bloomSize")) d.bloomSize = o.f("bloomSize", d.bloomSize)
    }

    private fun pointsOf(a: JSONArray): List<Pair<Float, Float>> = buildList {
        for (i in 0 until a.length()) {
            val pt = a.optJSONArray(i) ?: continue
            add(pt.optDouble(0).toFloat() to pt.optDouble(1).toFloat())
        }
    }

    private fun applyGlare(gl: JSONObject, s: ParamsState) {
        if (gl.has("active")) s.glareActive = gl.optBoolean("active", s.glareActive)
        if (gl.has("percent")) s.glarePercent = gl.f("percent", s.glarePercent)
        if (gl.has("roughness")) s.glareRoughness = gl.f("roughness", s.glareRoughness)
        if (gl.has("blur")) s.glareBlur = gl.f("blur", s.glareBlur)
    }

    // --- small JSON helpers (mirror Presets.kt conventions) ---

    private fun JSONObject.f(key: String, def: Float) = optDouble(key, def.toDouble()).toFloat()

    private fun JSONObject.triOf(key: String, def: Triple<Float, Float, Float>): Triple<Float, Float, Float> {
        val a = optJSONArray(key) ?: return def
        return tripleFromArray(a)
    }

    private fun JSONObject.pairOf(key: String, def: Pair<Float, Float>): Pair<Float, Float> {
        val a = optJSONArray(key) ?: return def
        return a.optDouble(0).toFloat() to a.optDouble(1).toFloat()
    }

    private fun tripleFromArray(a: JSONArray): Triple<Float, Float, Float> =
        Triple(a.optDouble(0).toFloat(), a.optDouble(1).toFloat(), a.optDouble(2).toFloat())

    private fun Triple<Float, Float, Float>.times100() =
        Triple(first * 100f, second * 100f, third * 100f)

    private fun <E : Enum<E>> enumOf(name: String, values: List<E>, def: E): E =
        values.firstOrNull { it.name == name } ?: def
}
