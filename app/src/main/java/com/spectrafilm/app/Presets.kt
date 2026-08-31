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
import java.math.BigDecimal
import java.math.BigInteger
import java.nio.charset.CharacterCodingException

internal const val PRESET_VERSION = 2
internal const val PRESET_SCHEMA_ID = "org.spektrafilm.preset"

/**
 * Resource-amplifying parameter domains shared by the editor and every preset/recipe boundary.
 * Keeping these ranges in one place prevents imported JSON from bypassing the UI limits and
 * driving unbounded native image allocation or iteration counts.
 */
internal object OperationalParamLimits {
    val SPECTRAL_GAUSSIAN_BLUR = 0f..20f
    val UPSCALE_FACTOR = 0f..4f
    val UPSCALE_NON_ZERO_FACTOR = 0.5f..4f
    val FILM_FORMAT_MM = 8f..120f
    val GRAIN_PARTICLE_AREA_UM2 = 0.2f..2f
    val GRAIN_PARTICLE_SCALE = 0.1f..5f
    val GRAIN_PARTICLE_SCALE_LAYERS = 0.25f..5f
    val GRAIN_DENSITY_MIN = 0f..0.5f
    val GRAIN_SUBLAYERS = 1..5
    val HALATION_BOUNCES = 1..5

    fun validate(state: ParamsState) {
        require(state.spectralGaussianBlur in SPECTRAL_GAUSSIAN_BLUR) {
            "input.spectralGaussianBlur must be in $SPECTRAL_GAUSSIAN_BLUR"
        }
        require(state.upscaleFactor == 0f || state.upscaleFactor in UPSCALE_NON_ZERO_FACTOR) {
            "input.upscaleFactor must be 0 or in $UPSCALE_NON_ZERO_FACTOR"
        }
        require(state.filmFormatMm in FILM_FORMAT_MM) {
            "camera.filmFormatMm must be in $FILM_FORMAT_MM"
        }
        require(state.grainParticleAreaUm2 in GRAIN_PARTICLE_AREA_UM2) {
            "grain.particleAreaUm2 must be in $GRAIN_PARTICLE_AREA_UM2"
        }
        require(state.grainParticleScale.allIn(GRAIN_PARTICLE_SCALE)) {
            "grain.particleScale components must be in $GRAIN_PARTICLE_SCALE"
        }
        require(state.grainParticleScaleLayers.allIn(GRAIN_PARTICLE_SCALE_LAYERS)) {
            "grain.particleScaleLayers components must be in $GRAIN_PARTICLE_SCALE_LAYERS"
        }
        require(state.grainDensityMin.allIn(GRAIN_DENSITY_MIN)) {
            "grain.densityMin components must be in $GRAIN_DENSITY_MIN"
        }
        require(state.grainNSubLayers in GRAIN_SUBLAYERS) {
            "grain.nSubLayers must be in $GRAIN_SUBLAYERS"
        }
        require(state.halNBounces in HALATION_BOUNCES) {
            "halation.nBounces must be in $HALATION_BOUNCES"
        }
    }

    private fun Triple<Float, Float, Float>.allIn(range: ClosedFloatingPointRange<Float>): Boolean =
        first in range && second in range && third in range
}

internal class UnsupportedPresetDocumentException(message: String) :
    IllegalArgumentException(message)

object Presets {

    private val INT_MIN_VALUE = BigInteger.valueOf(Int.MIN_VALUE.toLong())
    private val INT_MAX_VALUE = BigInteger.valueOf(Int.MAX_VALUE.toLong())

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
        val normalized = parsePersistentJson(json).toString(2)
        AtomicJsonStore.writeUtf8(file(ctx, name), normalized, AtomicJsonStore.MAX_PRESET_BYTES)
    }

    fun load(ctx: Context, name: String, into: ParamsState) {
        fromJson(readPersistentJson(ctx, name), into)
    }

    fun delete(ctx: Context, name: String) {
        AtomicJsonStore.delete(file(ctx, name))
    }

    /** Import a preset JSON from a SAF [uri] into [into]. */
    fun import(ctx: Context, uri: Uri, into: ParamsState) {
        fromJson(parseImportJson(readUriText(ctx, uri)), into)
    }

    /**
     * Read a saved preset's raw JSON text (the IO half of [load]). Lets a caller do the
     * file read off the main thread and then [decode] the result on the main thread, so
     * the Compose-state write stays on main. Throws if the file is missing/unreadable.
     */
    fun read(ctx: Context, name: String): String = readPersistentJson(ctx, name).toString(2)

    /** Read a SAF preset's raw JSON text (the IO half of [import]); decode on main. */
    fun readUri(ctx: Context, uri: Uri): String = parseImportJson(readUriText(ctx, uri)).toString(2)

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
        val normalized = parsePersistentJson(json).toString(2).toByteArray(Charsets.UTF_8)
        writeVerifiedNewDocument(ctx, uri, normalized, AtomicJsonStore.MAX_PRESET_BYTES)
    }

    fun toJsonString(state: ParamsState): String {
        OperationalParamLimits.validate(state)
        return toJson(state).toString(2)
    }

    /**
     * Reusable serialization hooks so the recipe (sidecar) layer shares this exact
     * JSON schema instead of forking a parallel one. [encode] mirrors [toJson];
     * [decode] mirrors [fromJson]. The recipe layer wraps the result with its own
     * envelope (source key + metadata) but the params payload is byte-for-byte the
     * same format as a saved preset.
     */
    internal fun encode(state: ParamsState): JSONObject {
        OperationalParamLimits.validate(state)
        return toJson(state)
    }

    internal fun decode(o: JSONObject, into: ParamsState) = fromJson(o, into)

    /** Strict import boundary: bounded caller reads this, rejects future/foreign schemas, then commits. */
    internal fun parseImportJson(text: String): JSONObject {
        val root = AtomicJsonStore.parseObject(text)
        val exactVersion = root.exactInteger("version", BigInteger.ONE)
        val currentVersion = BigInteger.valueOf(PRESET_VERSION.toLong())
        if (exactVersion < BigInteger.ONE || exactVersion > currentVersion) {
            throw UnsupportedPresetDocumentException("unsupported preset version: $exactVersion")
        }
        val version = exactVersion.toInt()
        val schema = root.opt("schema")
        if (schema !== null && schema !== JSONObject.NULL && schema != PRESET_SCHEMA_ID) {
            throw UnsupportedPresetDocumentException("unsupported preset schema: $schema")
        }
        if (version >= 2 && schema != PRESET_SCHEMA_ID) {
            throw UnsupportedPresetDocumentException("preset schema is required for version $version")
        }
        return validateCurrent(migrate(root))
    }

    private fun parsePersistentJson(text: String): JSONObject = parseImportJson(text)

    private fun readPersistentJson(ctx: Context, name: String): JSONObject {
        val target = file(ctx, name)
        return AtomicJsonStore.withPathLock(target) {
            val text = try {
                AtomicJsonStore.readUtf8(target, AtomicJsonStore.MAX_PRESET_BYTES)
            } catch (failure: Throwable) {
                // A bounded-size or malformed-UTF-8 failure proves corrupt content. Other I/O
                // failures may be transient and must not move the user's last good document.
                if (target.isFile &&
                    (failure is DocumentLimitException || failure is CharacterCodingException)
                ) {
                    runCatching { AtomicJsonStore.quarantine(target) }
                        .exceptionOrNull()
                        ?.let(failure::addSuppressed)
                }
                throw failure
            }
            try {
                parsePersistentJson(text)
            } catch (failure: UnsupportedPresetDocumentException) {
                // A newer/foreign document is not corrupt. Keep it byte-for-byte so a newer
                // app can still open it; callers surface the unsupported-version state.
                throw failure
            } catch (failure: Exception) {
                runCatching { AtomicJsonStore.quarantine(target) }
                    .exceptionOrNull()
                    ?.let(failure::addSuppressed)
                throw failure
            }
        }
    }

    private fun readUriText(ctx: Context, uri: Uri): String {
        require(uri.scheme == "content") { "preset source must be content://" }
        return ctx.contentResolver.openInputStream(uri)?.use {
            AtomicJsonStore.readUtf8(it, AtomicJsonStore.MAX_PRESET_BYTES)
        } ?: error("Could not open preset")
    }

    private fun file(ctx: Context, name: String): File = File(dir(ctx), "${safeName(name)}.json")

    private fun safeName(name: String): String = name.trim()
        .take(96)
        .ifEmpty { "preset" }
        .replace(Regex("[^A-Za-z0-9_\\- ]"), "_")

    // --- JSON (de)serialization ---

    private fun JSONObject.tri(key: String, t: Triple<Float, Float, Float>): JSONObject =
        put(key, JSONArray().put(t.first.toDouble()).put(t.second.toDouble()).put(t.third.toDouble()))

    private fun JSONObject.pair(key: String, p: Pair<Float, Float>): JSONObject =
        put(key, JSONArray().put(p.first.toDouble()).put(p.second.toDouble()))

    private fun JSONObject.triOf(key: String, def: Triple<Float, Float, Float>): Triple<Float, Float, Float> {
        if (!has(key)) return def
        val a = opt(key)
        require(a is JSONArray) { "$key must be a JSON array" }
        require(a.length() == 3) { "$key must contain exactly 3 numbers" }
        return Triple(
            a.finiteFloat(0, key),
            a.finiteFloat(1, key),
            a.finiteFloat(2, key),
        )
    }

    private fun JSONObject.pairOf(key: String, def: Pair<Float, Float>): Pair<Float, Float> {
        if (!has(key)) return def
        val a = opt(key)
        require(a is JSONArray) { "$key must be a JSON array" }
        require(a.length() == 2) { "$key must contain exactly 2 numbers" }
        return a.finiteFloat(0, key) to a.finiteFloat(1, key)
    }

    private fun JSONObject.f(key: String, def: Float): Float {
        if (!has(key)) return def
        return finiteDouble(key).toFloat().also {
            require(it.isFinite()) { "$key is outside the finite Float range" }
        }
    }

    private fun JSONObject.i(key: String, def: Int): Int {
        if (!has(key)) return def
        val value = exactInteger(key, BigInteger.valueOf(def.toLong()))
        require(value >= INT_MIN_VALUE && value <= INT_MAX_VALUE) { "$key is outside the Int range" }
        return value.toInt()
    }

    private fun JSONObject.exactInteger(key: String, def: BigInteger): BigInteger {
        if (!has(key)) return def
        val raw = opt(key)
        return try {
            when (raw) {
                is Byte, is Short, is Int, is Long -> BigInteger.valueOf((raw as Number).toLong())
                is BigInteger -> raw
                is BigDecimal -> raw.toBigIntegerExact()
                is Float -> {
                    require(raw.isFinite()) { "$key must be finite" }
                    BigDecimal.valueOf(raw.toDouble()).toBigIntegerExact()
                }
                is Double -> {
                    require(raw.isFinite()) { "$key must be finite" }
                    BigDecimal.valueOf(raw).toBigIntegerExact()
                }
                else -> throw IllegalArgumentException("$key must be a JSON integer")
            }
        } catch (failure: ArithmeticException) {
            throw IllegalArgumentException("$key must be an exact integer", failure)
        }
    }

    private fun JSONObject.finiteDouble(key: String): Double {
        val raw = opt(key)
        require(raw is Number) { "$key must be a JSON number" }
        return raw.toDouble().also { require(it.isFinite()) { "$key must be finite" } }
    }

    private fun JSONArray.finiteFloat(index: Int, field: String): Float {
        val raw = opt(index)
        require(raw is Number) { "$field[$index] must be a JSON number" }
        val value = raw.toDouble()
        require(value.isFinite()) { "$field[$index] must be finite" }
        return value.toFloat().also {
            require(it.isFinite()) { "$field[$index] is outside the finite Float range" }
        }
    }

    private fun pointsToJson(pts: List<Pair<Float, Float>>): JSONArray {
        val arr = JSONArray()
        for ((x, y) in pts) arr.put(JSONArray().put(x.toDouble()).put(y.toDouble()))
        return arr
    }

    private fun JSONObject.pointsOf(key: String): List<Pair<Float, Float>> {
        if (!has(key)) return emptyList()
        val arr = opt(key)
        require(arr is JSONArray) { "$key must be a JSON array" }
        val out = ArrayList<Pair<Float, Float>>(arr.length())
        for (i in 0 until arr.length()) {
            val p = arr.opt(i)
            require(p is JSONArray) { "$key[$i] must be a JSON array" }
            require(p.length() == 2) { "$key[$i] must contain exactly 2 numbers" }
            out.add(p.finiteFloat(0, "$key[$i]") to p.finiteFloat(1, "$key[$i]"))
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
        put("schema", PRESET_SCHEMA_ID)
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
     * shipped preset was v1. v2 wraps masks in their independently versioned interoperable document.
     * Newer-than-current objects passed directly to [decode] still decode known fields best-effort
     * (unknown keys are ignored and missing keys keep the current default; present numeric fields
     * must still be correctly typed and finite). When you add a field whose value for an *old*
     * preset should differ from a fresh [ParamsState] default, apply that here for `version < N`
     * rather than relying on the
     * constructor default.
     */
    private fun migrate(json: JSONObject): JSONObject {
        val version = json.i("version", 1)
        if (version >= PRESET_VERSION) return json
        // Preserve BigInteger/BigDecimal numeric lexemes across migration on Android;
        // JSONObject(String) would narrow decimals back through platform JSONTokener/Double.
        val migrated = AtomicJsonStore.parseObject(json.toString())
        if (version < 2) {
            migrated.optJSONArray("masks")?.let {
                migrated.put("masks", MaskJson.migrateLegacy(it))
            }
            migrated.put("schema", PRESET_SCHEMA_ID)
            migrated.put("version", 2)
        }
        return migrated
    }

    private fun validateCurrent(json: JSONObject): JSONObject {
        require(json.i("version", -1) == PRESET_VERSION) { "invalid preset version" }
        require(json.opt("schema") == PRESET_SCHEMA_ID) { "unsupported preset schema" }
        json.opt("masks")?.takeUnless { it === JSONObject.NULL }?.let { MaskJson.fromJson(it) }
        AtomicJsonStore.validate(json)
        validatePayload(json)
        return json
    }

    private fun fromJson(json: JSONObject, s: ParamsState) {
        AtomicJsonStore.validate(json)
        val o = migrate(json)
        AtomicJsonStore.validate(o)
        validatePayload(o)
        applyJson(o, s)
    }

    /** Decode once into detached state so a malformed late field cannot partly mutate live state. */
    private fun validatePayload(json: JSONObject) {
        val detached = ParamsState()
        applyJson(json, detached)
        OperationalParamLimits.validate(detached)
    }

    private fun applyJson(o: JSONObject, s: ParamsState) {
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

        o.opt("masks")?.takeUnless { it === JSONObject.NULL }?.let {
            s.localAdjustments = MaskJson.fromJson(it)
        }

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
            s.grainNSubLayers = g.i("nSubLayers", s.grainNSubLayers)
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
            s.halNBounces = h.i("nBounces", s.halNBounces)
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
            s.toneCurveMaster = t.pointsOf("master")
            s.toneCurveRed = t.pointsOf("red")
            s.toneCurveGreen = t.pointsOf("green")
            s.toneCurveBlue = t.pointsOf("blue")
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
