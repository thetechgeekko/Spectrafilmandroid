/*
 * Spektrafilm for Android — preset persistence. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Presets are SpektraParams snapshots serialized to JSON with android's built-in
 * org.json (no kotlinx.serialization, so the build graph is unchanged). Saved
 * presets live in filesDir/presets/<name>.json. Import reads any .json Uri; export
 * writes the current preset to a CreateDocument/share Uri. Every SpektraParams field
 * is round-tripped.
 */
package com.spectrafilm.app

import android.content.Context
import android.net.Uri
import com.spectrafilm.app.masks.MaskJson
import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.InputGamutCompress
import com.spectrafilm.engine.OutputGamutCompress
import com.spectrafilm.engine.Rgb2Raw
import com.spectrafilm.libraw.WhiteBalance
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

private const val PRESET_VERSION = 1

object Presets {

    private fun dir(ctx: Context): File =
        File(ctx.filesDir, "presets").apply { mkdirs() }

    /** Names (without .json) of saved presets, alphabetically. */
    fun list(ctx: Context): List<String> =
        dir(ctx).listFiles { f -> f.extension == "json" }
            ?.map { it.nameWithoutExtension }?.sorted() ?: emptyList()

    fun save(ctx: Context, name: String, state: ParamsState) =
        saveJson(ctx, name, toJsonString(state))

    /**
     * Persist a PRE-SERIALIZED preset [json] (from [toJsonString]) under [name].
     * [toJsonString] reads live Compose [ParamsState] field-by-field, so it must run on the
     * main thread; passing the string here lets the caller serialize on main and cross only
     * the file write to IO — avoiding a torn (partially-updated) off-thread snapshot.
     */
    fun saveJson(ctx: Context, name: String, json: String) {
        val safe = name.trim().ifEmpty { "preset" }.replace(Regex("[^A-Za-z0-9_\\- ]"), "_")
        File(dir(ctx), "$safe.json").writeText(json)
    }

    fun load(ctx: Context, name: String, into: ParamsState) {
        val text = File(dir(ctx), "$name.json").readText()
        fromJson(JSONObject(text), into)
    }

    fun delete(ctx: Context, name: String) {
        File(dir(ctx), "$name.json").delete()
    }

    /** Import a preset JSON from a SAF [uri] into [into]. */
    fun import(ctx: Context, uri: Uri, into: ParamsState) {
        fromJson(JSONObject(readUri(ctx, uri)), into)
    }

    /**
     * Read a saved preset's raw JSON text (the IO half of [load]). Lets a caller do the
     * file read off the main thread and then [decode] the result on the main thread, so
     * the Compose-state write stays on main. Throws if the file is missing/unreadable.
     */
    fun read(ctx: Context, name: String): String =
        File(dir(ctx), "$name.json").readText()

    /** Read a SAF preset's raw JSON text (the IO half of [import]); decode on main. */
    fun readUri(ctx: Context, uri: Uri): String =
        ctx.contentResolver.openInputStream(uri)?.use { it.readBytes().decodeToString() }
            ?: error("Could not open preset")

    /** Export the current [state] to a SAF [uri]. */
    fun export(ctx: Context, uri: Uri, state: ParamsState) {
        exportJson(ctx, uri, toJsonString(state))
    }

    /**
     * Export a PRE-SERIALIZED preset [json] (from [toJsonString]) to a SAF [uri] — the IO
     * half of [export]. Serialize on the main thread, cross only the write to IO (same
     * torn-snapshot rationale as [saveJson]).
     */
    fun exportJson(ctx: Context, uri: Uri, json: String) {
        ctx.contentResolver.openOutputStream(uri)?.use {
            it.write(json.toByteArray())
        } ?: error("Could not open output for preset")
    }

    fun toJsonString(state: ParamsState): String = toJson(state).toString(2)

    /**
     * Reusable serialization hooks so the recipe (sidecar) layer shares this exact
     * JSON schema instead of forking a parallel one. [encode] mirrors [toJson];
     * [decode] mirrors [fromJson]. The recipe layer wraps the result with its own
     * envelope (source key + metadata) but the params payload is byte-for-byte the
     * same format as a saved preset.
     */
    internal fun encode(state: ParamsState): JSONObject = toJson(state)

    internal fun decode(o: JSONObject, into: ParamsState) = fromJson(o, into)

    // --- JSON (de)serialization ---

    private fun JSONObject.tri(key: String, t: Triple<Float, Float, Float>): JSONObject =
        put(key, JSONArray().put(t.first.toDouble()).put(t.second.toDouble()).put(t.third.toDouble()))

    private fun JSONObject.pair(key: String, p: Pair<Float, Float>): JSONObject =
        put(key, JSONArray().put(p.first.toDouble()).put(p.second.toDouble()))

    private fun JSONObject.triOf(key: String, def: Triple<Float, Float, Float>): Triple<Float, Float, Float> {
        val a = optJSONArray(key) ?: return def
        // optDouble tolerates a short/malformed array instead of throwing mid-decode
        // (a partial mutation of ParamsState); a missing element falls back to the default.
        return Triple(
            a.optDouble(0, def.first.toDouble()).toFloat(),
            a.optDouble(1, def.second.toDouble()).toFloat(),
            a.optDouble(2, def.third.toDouble()).toFloat(),
        )
    }

    private fun JSONObject.pairOf(key: String, def: Pair<Float, Float>): Pair<Float, Float> {
        val a = optJSONArray(key) ?: return def
        return a.optDouble(0, def.first.toDouble()).toFloat() to a.optDouble(1, def.second.toDouble()).toFloat()
    }

    private fun JSONObject.f(key: String, def: Float) = optDouble(key, def.toDouble()).toFloat()

    private fun pointsToJson(pts: List<Pair<Float, Float>>): JSONArray {
        val arr = JSONArray()
        for ((x, y) in pts) arr.put(JSONArray().put(x.toDouble()).put(y.toDouble()))
        return arr
    }

    private fun jsonToPoints(arr: JSONArray?): List<Pair<Float, Float>> {
        if (arr == null) return emptyList()
        val out = ArrayList<Pair<Float, Float>>(arr.length())
        for (i in 0 until arr.length()) {
            val p = arr.optJSONArray(i) ?: continue
            if (p.length() < 2) continue  // skip a malformed point instead of throwing
            out.add(p.optDouble(0, 0.0).toFloat() to p.optDouble(1, 0.0).toFloat())
        }
        return out
    }

    private fun diffJson(d: DiffusionState) = JSONObject()
        .put("active", d.active).put("family", d.family)
        .put("strength", d.strength.toDouble()).put("spatialScale", d.spatialScale.toDouble())
        .put("haloWarmth", d.haloWarmth.toDouble()).put("coreIntensity", d.coreIntensity.toDouble())
        .put("coreSize", d.coreSize.toDouble()).put("haloIntensity", d.haloIntensity.toDouble())
        .put("haloSize", d.haloSize.toDouble()).put("bloomIntensity", d.bloomIntensity.toDouble())
        .put("bloomSize", d.bloomSize.toDouble())

    private fun readDiff(o: JSONObject, d: DiffusionState) {
        d.active = o.optBoolean("active", d.active)
        d.family = o.optString("family", d.family)
        d.strength = o.f("strength", d.strength); d.spatialScale = o.f("spatialScale", d.spatialScale)
        d.haloWarmth = o.f("haloWarmth", d.haloWarmth); d.coreIntensity = o.f("coreIntensity", d.coreIntensity)
        d.coreSize = o.f("coreSize", d.coreSize); d.haloIntensity = o.f("haloIntensity", d.haloIntensity)
        d.haloSize = o.f("haloSize", d.haloSize); d.bloomIntensity = o.f("bloomIntensity", d.bloomIntensity)
        d.bloomSize = o.f("bloomSize", d.bloomSize)
    }

    private fun toJson(s: ParamsState): JSONObject = JSONObject().apply {
        put("version", PRESET_VERSION)
        put("filmProfile", s.filmProfile)
        put("printProfile", s.printProfile)

        put("input", JSONObject().apply {
            put("inputColorSpace", s.inputColorSpace)
            put("inputCctfDecoding", s.inputCctfDecoding)
            put("spectralUpsampling", s.spectralUpsampling.name)
            put("adaptationWindow", s.adaptationWindow)
            put("adaptationSurface", s.adaptationSurface)
            put("spectralGaussianBlur", s.spectralGaussianBlur.toDouble())
            tri("filterUv", s.filterUv)
            tri("filterIr", s.filterIr)
            put("upscaleFactor", s.upscaleFactor.toDouble())
            put("crop", s.crop)
            pair("cropCenter", s.cropCenter)
            pair("cropSize", s.cropSize)
        })

        put("raw", JSONObject().apply {
            put("whiteBalance", s.rawWhiteBalance.name)
            put("temperature", s.rawTemperature.toDouble())
            put("tint", s.rawTint.toDouble())
        })

        put("creativeWb", JSONObject().apply {
            put("temp", s.creativeWbTemp.toDouble())
            put("tint", s.creativeWbTint.toDouble())
            put("balanceToFilmStock", s.balanceToFilmStock)
        })

        put("grade", JSONObject().apply {
            put("contrast", s.contrast.toDouble())
            put("saturation", s.saturation.toDouble())
            put("vibrance", s.vibrance.toDouble())
            put("gamutCompress", s.gamutCompress.toDouble())
        })

        put("masks", MaskJson.toJson(s.localAdjustments))

        put("camera", JSONObject().apply {
            put("exposureCompensationEv", s.exposureCompensationEv.toDouble())
            put("autoExposure", s.autoExposure)
            put("autoExposureMethod", s.autoExposureMethod)
            put("filmFormatMm", s.filmFormatMm.toDouble())
            put("lensBlurUm", s.cameraLensBlurUm.toDouble())
            put("diffusion", diffJson(s.cameraDiffusionState))
        })

        put("enlarger", JSONObject().apply {
            put("illuminant", s.printIlluminant)
            put("printExposure", s.printExposure.toDouble())
            put("printExposureCompensation", s.printExposureCompensation)
            put("yFilterShift", s.printYFilterShift.toDouble())
            put("mFilterShift", s.printMFilterShift.toDouble())
            put("lensBlur", s.enlargerLensBlur.toDouble())
            put("diffusion", diffJson(s.printDiffusionState))
            put("preflashExposure", s.preflashExposure.toDouble())
            put("preflashYFilterShift", s.preflashYFilterShift.toDouble())
            put("preflashMFilterShift", s.preflashMFilterShift.toDouble())
        })

        put("scanner", JSONObject().apply {
            put("lensBlur", s.scanLensBlur.toDouble())
            put("whiteCorrection", s.scanWhiteCorrection)
            put("whiteLevel", s.scanWhiteLevel.toDouble())
            put("blackCorrection", s.scanBlackCorrection)
            put("blackLevel", s.scanBlackLevel.toDouble())
            pair("unsharpMask", s.scanUnsharpMask)
        })

        put("output", JSONObject().apply {
            put("outputColorSpace", s.outputColorSpace.name)
            put("savingCctfEncoding", s.savingCctfEncoding)
            put("outputGamutCompress", s.outputGamutCompress.name)
            put("inputGamutCompress", s.inputGamutCompress.name)
            put("scanFilm", s.scanFilm)
        })

        put("grain", JSONObject().apply {
            put("active", s.grainActive)
            put("sublayersActive", s.grainSublayersActive)
            put("particleAreaUm2", s.grainParticleAreaUm2.toDouble())
            tri("particleScale", s.grainParticleScale)
            tri("particleScaleLayers", s.grainParticleScaleLayers)
            tri("densityMin", s.grainDensityMin)
            tri("uniformity", s.grainUniformity)
            put("blur", s.grainBlur.toDouble())
            put("blurDyeCloudsUm", s.grainBlurDyeCloudsUm.toDouble())
            pair("microStructure", s.grainMicroStructure)
            put("nSubLayers", s.grainNSubLayers)
        })

        put("halation", JSONObject().apply {
            put("active", s.halationActive)
            put("scatterAmount", s.halScatterAmount.toDouble())
            put("scatterSpatialScale", s.halScatterSpatialScale.toDouble())
            put("halationAmount", s.halHalationAmount.toDouble())
            put("halationSpatialScale", s.halHalationSpatialScale.toDouble())
            put("boostEv", s.halBoostEv.toDouble())
            put("protectEv", s.halProtectEv.toDouble())
            put("boostRange", s.halBoostRange.toDouble())
            tri("scatterCoreUm", s.halScatterCoreUm)
            tri("scatterTailUm", s.halScatterTailUm)
            tri("scatterTailWeightPct", s.halScatterTailWeightPct)
            tri("halationStrengthPct", s.halHalationStrengthPct)
            tri("firstSigmaUm", s.halFirstSigmaUm)
            put("nBounces", s.halNBounces)
            put("bounceDecay", s.halBounceDecay.toDouble())
            put("renormalize", s.halRenormalize)
        })

        put("couplers", JSONObject().apply {
            put("active", s.couplersActive)
            put("amount", s.couplersAmount.toDouble())
            put("inhibitionSamelayer", s.couplersInhibitionSamelayer.toDouble())
            put("inhibitionInterlayer", s.couplersInhibitionInterlayer.toDouble())
            tri("gammaSamelayer", s.couplersGammaSamelayer)
            pair("gammaRtoGb", s.couplersGammaRtoGb)
            pair("gammaGtoRb", s.couplersGammaGtoRb)
            pair("gammaBtoRg", s.couplersGammaBtoRg)
            put("diffusionSizeUm", s.couplersDiffusionSizeUm.toDouble())
            put("diffusionTailUm", s.couplersDiffusionTailUm.toDouble())
            put("diffusionTailWeight", s.couplersDiffusionTailWeight.toDouble())
        })

        put("glare", JSONObject().apply {
            put("active", s.glareActive)
            put("percent", s.glarePercent.toDouble())
            put("roughness", s.glareRoughness.toDouble())
            put("blur", s.glareBlur.toDouble())
        })

        put("experimental", JSONObject().apply {
            put("filmGammaFactor", s.filmGammaFactor.toDouble())
            put("printGammaFactor", s.printGammaFactor.toDouble())
            put("morphActive", s.morphActive)
            put("morphGammaFactor", s.morphGammaFactor.toDouble())
            put("morphGammaFactorFast", s.morphGammaFactorFast.toDouble())
            put("morphGammaFactorSlow", s.morphGammaFactorSlow.toDouble())
            put("morphGammaFactorRed", s.morphGammaFactorRed.toDouble())
            put("morphGammaFactorGreen", s.morphGammaFactorGreen.toDouble())
            put("morphGammaFactorBlue", s.morphGammaFactorBlue.toDouble())
            put("morphDeveloperExhaustion", s.morphDeveloperExhaustion.toDouble())
        })

        put("toneCurve", JSONObject().apply {
            put("active", s.toneCurveActive)
            put("master", pointsToJson(s.toneCurveMaster))
            put("red", pointsToJson(s.toneCurveRed))
            put("green", pointsToJson(s.toneCurveGreen))
            put("blue", pointsToJson(s.toneCurveBlue))
        })

        // NO "display" block. previewMaxSize is an app-level device setting owned by
        // AppSettings, not a property of a look — see ParamsState.previewMaxSize.
    }

    /**
     * Bring an older preset/recipe JSON up to the current [PRESET_VERSION] before decoding. The
     * version was previously written by [toJson] but never read; reading it here gives one tested
     * place to handle field renames or changed defaults when the schema next breaks. Today every
     * shipped preset is v1, so this is an identity pass. Newer-than-current files decode best-effort
     * (every field below uses opt*-with-default, so unknown keys are ignored and missing keys keep
     * the current default). When you add a field whose value for an *old* preset should differ from
     * a fresh [ParamsState] default, apply that here for `version < N` rather than relying on the
     * constructor default.
     */
    private fun migrate(json: JSONObject): JSONObject {
        val version = json.optInt("version", PRESET_VERSION)
        if (version >= PRESET_VERSION) return json
        // --- older-schema migrations land here as the schema evolves, e.g.:
        //   if (version < 2) json.optJSONObject("camera")?.let { c ->
        //       if (c.has("lensBlurUm") && !c.has("lensBlur")) c.put("lensBlur", c.getDouble("lensBlurUm"))
        //   }
        return json
    }

    private fun fromJson(json: JSONObject, s: ParamsState) {
        val o = migrate(json)
        s.filmProfile = o.optString("filmProfile", s.filmProfile)
        s.printProfile = o.optString("printProfile", s.printProfile)

        o.optJSONObject("input")?.let { i ->
            s.inputColorSpace = i.optString("inputColorSpace", s.inputColorSpace)
            s.inputCctfDecoding = i.optBoolean("inputCctfDecoding", s.inputCctfDecoding)
            s.spectralUpsampling = enumOf(i.optString("spectralUpsampling"), Rgb2Raw.entries, s.spectralUpsampling)
            s.adaptationWindow = i.optBoolean("adaptationWindow", s.adaptationWindow)
            s.adaptationSurface = i.optBoolean("adaptationSurface", s.adaptationSurface)
            s.spectralGaussianBlur = i.f("spectralGaussianBlur", s.spectralGaussianBlur)
            s.filterUv = i.triOf("filterUv", s.filterUv)
            s.filterIr = i.triOf("filterIr", s.filterIr)
            s.upscaleFactor = i.f("upscaleFactor", s.upscaleFactor)
            s.crop = i.optBoolean("crop", s.crop)
            s.cropCenter = i.pairOf("cropCenter", s.cropCenter)
            s.cropSize = i.pairOf("cropSize", s.cropSize)
        }

        o.optJSONObject("raw")?.let { r ->
            s.rawWhiteBalance = enumOf(r.optString("whiteBalance"), WhiteBalance.entries, s.rawWhiteBalance)
            s.rawTemperature = r.f("temperature", s.rawTemperature)
            s.rawTint = r.f("tint", s.rawTint)
        }

        o.optJSONObject("creativeWb")?.let { c ->
            s.creativeWbTemp = c.f("temp", s.creativeWbTemp)
            s.creativeWbTint = c.f("tint", s.creativeWbTint)
            s.balanceToFilmStock = c.optBoolean("balanceToFilmStock", s.balanceToFilmStock)
        }

        o.optJSONObject("grade")?.let { g ->
            s.contrast = g.f("contrast", s.contrast)
            s.saturation = g.f("saturation", s.saturation)
            s.vibrance = g.f("vibrance", s.vibrance)
            s.gamutCompress = g.f("gamutCompress", s.gamutCompress)
        }

        o.optJSONArray("masks")?.let { s.localAdjustments = MaskJson.fromJson(it) }

        o.optJSONObject("camera")?.let { c ->
            s.exposureCompensationEv = c.f("exposureCompensationEv", s.exposureCompensationEv)
            s.autoExposure = c.optBoolean("autoExposure", s.autoExposure)
            s.autoExposureMethod = c.optString("autoExposureMethod", s.autoExposureMethod)
            s.filmFormatMm = c.f("filmFormatMm", s.filmFormatMm)
            s.cameraLensBlurUm = c.f("lensBlurUm", s.cameraLensBlurUm)
            c.optJSONObject("diffusion")?.let { readDiff(it, s.cameraDiffusionState) }
        }

        o.optJSONObject("enlarger")?.let { e ->
            s.printIlluminant = e.optString("illuminant", s.printIlluminant)
            s.printExposure = e.f("printExposure", s.printExposure)
            s.printExposureCompensation = e.optBoolean("printExposureCompensation", s.printExposureCompensation)
            s.printYFilterShift = e.f("yFilterShift", s.printYFilterShift)
            s.printMFilterShift = e.f("mFilterShift", s.printMFilterShift)
            s.enlargerLensBlur = e.f("lensBlur", s.enlargerLensBlur)
            e.optJSONObject("diffusion")?.let { readDiff(it, s.printDiffusionState) }
            s.preflashExposure = e.f("preflashExposure", s.preflashExposure)
            s.preflashYFilterShift = e.f("preflashYFilterShift", s.preflashYFilterShift)
            s.preflashMFilterShift = e.f("preflashMFilterShift", s.preflashMFilterShift)
        }

        o.optJSONObject("scanner")?.let { sc ->
            s.scanLensBlur = sc.f("lensBlur", s.scanLensBlur)
            s.scanWhiteCorrection = sc.optBoolean("whiteCorrection", s.scanWhiteCorrection)
            s.scanWhiteLevel = sc.f("whiteLevel", s.scanWhiteLevel)
            s.scanBlackCorrection = sc.optBoolean("blackCorrection", s.scanBlackCorrection)
            s.scanBlackLevel = sc.f("blackLevel", s.scanBlackLevel)
            s.scanUnsharpMask = sc.pairOf("unsharpMask", s.scanUnsharpMask)
        }

        o.optJSONObject("output")?.let { ou ->
            s.outputColorSpace = enumOf(ou.optString("outputColorSpace"), ColorSpace.entries, s.outputColorSpace)
            s.savingCctfEncoding = ou.optBoolean("savingCctfEncoding", s.savingCctfEncoding)
            s.outputGamutCompress =
                enumOf(ou.optString("outputGamutCompress"), OutputGamutCompress.entries, s.outputGamutCompress)
            s.inputGamutCompress =
                enumOf(ou.optString("inputGamutCompress"), InputGamutCompress.entries, s.inputGamutCompress)
            s.scanFilm = ou.optBoolean("scanFilm", s.scanFilm)
        }

        o.optJSONObject("grain")?.let { g ->
            s.grainActive = g.optBoolean("active", s.grainActive)
            s.grainSublayersActive = g.optBoolean("sublayersActive", s.grainSublayersActive)
            s.grainParticleAreaUm2 = g.f("particleAreaUm2", s.grainParticleAreaUm2)
            s.grainParticleScale = g.triOf("particleScale", s.grainParticleScale)
            s.grainParticleScaleLayers = g.triOf("particleScaleLayers", s.grainParticleScaleLayers)
            s.grainDensityMin = g.triOf("densityMin", s.grainDensityMin)
            s.grainUniformity = g.triOf("uniformity", s.grainUniformity)
            s.grainBlur = g.f("blur", s.grainBlur)
            s.grainBlurDyeCloudsUm = g.f("blurDyeCloudsUm", s.grainBlurDyeCloudsUm)
            s.grainMicroStructure = g.pairOf("microStructure", s.grainMicroStructure)
            s.grainNSubLayers = g.optInt("nSubLayers", s.grainNSubLayers)
        }

        o.optJSONObject("halation")?.let { h ->
            s.halationActive = h.optBoolean("active", s.halationActive)
            s.halScatterAmount = h.f("scatterAmount", s.halScatterAmount)
            s.halScatterSpatialScale = h.f("scatterSpatialScale", s.halScatterSpatialScale)
            s.halHalationAmount = h.f("halationAmount", s.halHalationAmount)
            s.halHalationSpatialScale = h.f("halationSpatialScale", s.halHalationSpatialScale)
            s.halBoostEv = h.f("boostEv", s.halBoostEv)
            s.halProtectEv = h.f("protectEv", s.halProtectEv)
            s.halBoostRange = h.f("boostRange", s.halBoostRange)
            s.halScatterCoreUm = h.triOf("scatterCoreUm", s.halScatterCoreUm)
            s.halScatterTailUm = h.triOf("scatterTailUm", s.halScatterTailUm)
            s.halScatterTailWeightPct = h.triOf("scatterTailWeightPct", s.halScatterTailWeightPct)
            s.halHalationStrengthPct = h.triOf("halationStrengthPct", s.halHalationStrengthPct)
            s.halFirstSigmaUm = h.triOf("firstSigmaUm", s.halFirstSigmaUm)
            s.halNBounces = h.optInt("nBounces", s.halNBounces)
            s.halBounceDecay = h.f("bounceDecay", s.halBounceDecay)
            s.halRenormalize = h.optBoolean("renormalize", s.halRenormalize)
        }

        o.optJSONObject("couplers")?.let { c ->
            s.couplersActive = c.optBoolean("active", s.couplersActive)
            s.couplersAmount = c.f("amount", s.couplersAmount)
            s.couplersInhibitionSamelayer = c.f("inhibitionSamelayer", s.couplersInhibitionSamelayer)
            s.couplersInhibitionInterlayer = c.f("inhibitionInterlayer", s.couplersInhibitionInterlayer)
            s.couplersGammaSamelayer = c.triOf("gammaSamelayer", s.couplersGammaSamelayer)
            s.couplersGammaRtoGb = c.pairOf("gammaRtoGb", s.couplersGammaRtoGb)
            s.couplersGammaGtoRb = c.pairOf("gammaGtoRb", s.couplersGammaGtoRb)
            s.couplersGammaBtoRg = c.pairOf("gammaBtoRg", s.couplersGammaBtoRg)
            s.couplersDiffusionSizeUm = c.f("diffusionSizeUm", s.couplersDiffusionSizeUm)
            s.couplersDiffusionTailUm = c.f("diffusionTailUm", s.couplersDiffusionTailUm)
            s.couplersDiffusionTailWeight = c.f("diffusionTailWeight", s.couplersDiffusionTailWeight)
        }

        o.optJSONObject("glare")?.let { gl ->
            s.glareActive = gl.optBoolean("active", s.glareActive)
            s.glarePercent = gl.f("percent", s.glarePercent)
            s.glareRoughness = gl.f("roughness", s.glareRoughness)
            s.glareBlur = gl.f("blur", s.glareBlur)
        }

        o.optJSONObject("experimental")?.let { ex ->
            s.filmGammaFactor = ex.f("filmGammaFactor", s.filmGammaFactor)
            s.printGammaFactor = ex.f("printGammaFactor", s.printGammaFactor)
            s.morphActive = ex.optBoolean("morphActive", s.morphActive)
            s.morphGammaFactor = ex.f("morphGammaFactor", s.morphGammaFactor)
            s.morphGammaFactorFast = ex.f("morphGammaFactorFast", s.morphGammaFactorFast)
            s.morphGammaFactorSlow = ex.f("morphGammaFactorSlow", s.morphGammaFactorSlow)
            s.morphGammaFactorRed = ex.f("morphGammaFactorRed", s.morphGammaFactorRed)
            s.morphGammaFactorGreen = ex.f("morphGammaFactorGreen", s.morphGammaFactorGreen)
            s.morphGammaFactorBlue = ex.f("morphGammaFactorBlue", s.morphGammaFactorBlue)
            s.morphDeveloperExhaustion = ex.f("morphDeveloperExhaustion", s.morphDeveloperExhaustion)
        }

        o.optJSONObject("toneCurve")?.let { t ->
            s.toneCurveActive = t.optBoolean("active", s.toneCurveActive)
            s.toneCurveMaster = jsonToPoints(t.optJSONArray("master"))
            s.toneCurveRed = jsonToPoints(t.optJSONArray("red"))
            s.toneCurveGreen = jsonToPoints(t.optJSONArray("green"))
            s.toneCurveBlue = jsonToPoints(t.optJSONArray("blue"))
        }

        // "display".previewMaxSize is deliberately NOT read back. Recipes are keyed by
        // source uri and auto-applied on open, so this line silently replayed a stale
        // preview size over the user's Settings value on every real photo — the demo
        // image (which has no recipe) honoured the setting, which is exactly why the
        // bug looked like "inert for RAW only". Every already-saved recipe still
        // carries the old field; dropping the READ is what disarms them.
    }

    private fun <E : Enum<E>> enumOf(name: String, values: List<E>, def: E): E =
        values.firstOrNull { it.name == name } ?: def
}
