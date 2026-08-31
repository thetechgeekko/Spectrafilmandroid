/*
 * Spektrafilm for Android — unit tests for the recipe/preset JSON round-trip. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Presets is the non-destructive editing layer: edits are serialized to JSON and
 * re-applied on open/export. This guards that a serialize → parse → decode round-trip
 * preserves the editing state. Runs on the plain JVM with the real org.json on the
 * test classpath (see app/build.gradle.kts) — no device/Robolectric.
 */
package com.spectrafilm.app

import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.InputGamutCompress
import com.spectrafilm.engine.OutputGamutCompress
import com.spectrafilm.engine.Rgb2Raw
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class PresetsRoundTripTest {

    @Test
    fun roundTrip_preservesRepresentativeFields() {
        val src = ParamsState().apply {
            filmProfile = "kodak_ektar_100"
            printProfile = "kodak_supra_endura"
            spectralUpsampling = Rgb2Raw.MALLETT2019      // non-default enum
            crop = true
            rawTemperature = 4200f
            exposureCompensationEv = 1.5f
            autoExposure = true
            cameraLensBlurUm = 3.5f
            scanUnsharpMask = 0.4f to 0.9f
            outputColorSpace = ColorSpace.ADOBE_RGB       // non-default enum
        }

        // serialize → text → parse → decode into a fresh state
        val json = Presets.toJsonString(src)
        val dst = ParamsState()
        Presets.decode(JSONObject(json), dst)

        assertEquals("kodak_ektar_100", dst.filmProfile)
        assertEquals("kodak_supra_endura", dst.printProfile)
        assertEquals(Rgb2Raw.MALLETT2019, dst.spectralUpsampling)
        assertTrue(dst.crop)
        assertEquals(4200f, dst.rawTemperature, 1e-4f)
        assertEquals(1.5f, dst.exposureCompensationEv, 1e-4f)
        assertTrue(dst.autoExposure)
        assertEquals(3.5f, dst.cameraLensBlurUm, 1e-4f)
        assertEquals(0.4f, dst.scanUnsharpMask.first, 1e-4f)
        assertEquals(0.9f, dst.scanUnsharpMask.second, 1e-4f)
        assertEquals(ColorSpace.ADOBE_RGB, dst.outputColorSpace)
    }

    @Test
    fun decode_missingKeys_keepDefaults() {
        val dst = ParamsState()
        val defFilm = dst.filmProfile
        val defCs = dst.outputColorSpace
        // a minimal/old preset with only the version present must not clobber defaults
        Presets.decode(JSONObject("""{"version":1}"""), dst)
        assertEquals(defFilm, dst.filmProfile)
        assertEquals(defCs, dst.outputColorSpace)
        assertFalse(dst.crop)
    }

    @Test
    fun roundTrip_isStableAcrossTwoPasses() {
        val a = ParamsState().apply {
            exposureCompensationEv = -0.75f
            outputColorSpace = ColorSpace.REC2020
            scanUnsharpMask = 0.25f to 0.6f
        }
        val firstJson = Presets.toJsonString(a)
        val b = ParamsState().also { Presets.decode(JSONObject(firstJson), it) }
        val secondJson = Presets.toJsonString(b)
        // re-serializing the decoded state reproduces the same JSON (idempotent)
        assertEquals(firstJson, secondJson)
    }

    @Test
    fun decode_futureVersion_decodesKnownFieldsBestEffort() {
        // A preset written by a NEWER app (higher schema version) must still decode the fields this
        // build understands rather than being rejected — migrate() passes it through and the opt*
        // reads apply. Guards the version-aware decode that replaced the ignored PRESET_VERSION.
        val dst = ParamsState()
        Presets.decode(JSONObject("""{"version":999,"filmProfile":"kodak_ektar_100"}"""), dst)
        assertEquals("kodak_ektar_100", dst.filmProfile)
    }

    @Test
    fun decode_noVersionField_stillDecodes() {
        // A version-less JSON is treated as the current schema and decodes normally.
        val dst = ParamsState()
        Presets.decode(JSONObject("""{"filmProfile":"kodak_ektar_100"}"""), dst)
        assertEquals("kodak_ektar_100", dst.filmProfile)
    }

    @Test
    fun roundTrip_preservesGamutCompression() {
        val src = ParamsState().apply {
            outputGamutCompress = OutputGamutCompress.ACES_RGC   // non-default
            inputGamutCompress = InputGamutCompress.XY           // non-default
        }
        val dst = ParamsState()
        Presets.decode(JSONObject(Presets.toJsonString(src)), dst)
        assertEquals(OutputGamutCompress.ACES_RGC, dst.outputGamutCompress)
        assertEquals(InputGamutCompress.XY, dst.inputGamutCompress)
    }

    @Test
    fun decode_missingGamut_keepsByteIdenticalDefaultsOff() {
        // An old recipe (no gamut keys) MUST leave the default-off sentinels so the
        // rendered look is byte-identical to before the feature existed.
        val dst = ParamsState()
        Presets.decode(JSONObject("""{"version":1,"output":{"outputColorSpace":"SRGB"}}"""), dst)
        assertEquals(OutputGamutCompress.LEGACY_CLIP, dst.outputGamutCompress)
        assertEquals(InputGamutCompress.OFF, dst.inputGamutCompress)
    }

    @Test
    fun roundTrip_preservesLocalAdjustmentMasks() {
        val src = ParamsState().apply {
            localAdjustments = listOf(
                com.spectrafilm.app.masks.LocalAdjustment(
                    com.spectrafilm.app.masks.Mask(
                        listOf(com.spectrafilm.app.masks.Mask.Component(
                            com.spectrafilm.app.masks.BlendMode.ADD,
                            com.spectrafilm.app.masks.MaskComponent.Radial(0.4f, 0.6f, 0.3f, 0.2f, 0.5f, angleDeg = 15f),
                            value = 0.8f,
                        )),
                        opacity = 0.9f,
                    ),
                    com.spectrafilm.app.masks.TierADelta(exposureEv = 0.75f, saturation = 25f, contrast = -10f),
                ),
            )
        }
        val dst = ParamsState()
        Presets.decode(JSONObject(Presets.toJsonString(src)), dst)
        assertEquals(src.localAdjustments, dst.localAdjustments)
        assertEquals(1, dst.localAdjustments.size)
    }

    @Test
    fun decode_numericStringIsRejectedBeforeAnyLiveFieldMutation() {
        val malformed = JSONObject(Presets.toJsonString(ParamsState())).apply {
            put("filmProfile", "must-not-commit")
            getJSONObject("grade").put("contrast", "NaN")
        }
        val dst = ParamsState().apply {
            filmProfile = "keep-me"
            contrast = 0.75f
        }

        assertThrows(IllegalArgumentException::class.java) {
            Presets.decode(malformed, dst)
        }

        assertEquals("keep-me", dst.filmProfile)
        assertEquals(0.75f, dst.contrast, 0f)
    }

    @Test
    fun import_numericOverflowAndNonJsonNaNAreRejected() {
        val overflow = """{
            "schema":"$PRESET_SCHEMA_ID",
            "version":$PRESET_VERSION,
            "grade":{"contrast":1e400}
        }""".trimIndent()

        assertThrows(IllegalArgumentException::class.java) {
            Presets.parseImportJson(overflow)
        }
        assertThrows(IllegalArgumentException::class.java) {
            Presets.parseImportJson(
                """{"schema":"$PRESET_SCHEMA_ID","version":$PRESET_VERSION,"grade":{"contrast":NaN}}""",
            )
        }
    }

    @Test
    fun import_rejectsResourceAmplificationOutsideSupportedDomains() {
        val baseline = Presets.toJsonString(ParamsState())
        val inactiveEffects = Presets.toJsonString(ParamsState().apply {
            grainActive = false
            grainSublayersActive = false
            halationActive = false
        })
        val invalidDocuments = listOf(
            JSONObject(baseline).apply { getJSONObject("input").put("spectralGaussianBlur", -0.001) },
            JSONObject(baseline).apply { getJSONObject("input").put("spectralGaussianBlur", 20.001) },
            JSONObject(baseline).apply { getJSONObject("input").put("upscaleFactor", -0.001) },
            JSONObject(baseline).apply { getJSONObject("input").put("upscaleFactor", 0.25) },
            JSONObject(baseline).apply { getJSONObject("input").put("upscaleFactor", 4.001) },
            JSONObject(baseline).apply { getJSONObject("camera").put("filmFormatMm", 7.999) },
            JSONObject(baseline).apply { getJSONObject("camera").put("filmFormatMm", 120.001) },
            JSONObject(baseline).apply { getJSONObject("grain").put("particleAreaUm2", 0.199) },
            JSONObject(baseline).apply { getJSONObject("grain").put("particleAreaUm2", 2.001) },
            JSONObject(baseline).apply {
                getJSONObject("grain").put("particleScale", org.json.JSONArray(listOf(0.099, 1.0, 1.0)))
            },
            JSONObject(baseline).apply {
                getJSONObject("grain").put("particleScale", org.json.JSONArray(listOf(1.0, 5.001, 1.0)))
            },
            JSONObject(baseline).apply {
                getJSONObject("grain").put("particleScaleLayers", org.json.JSONArray(listOf(1.0, 0.249, 1.0)))
            },
            JSONObject(baseline).apply {
                getJSONObject("grain").put("particleScaleLayers", org.json.JSONArray(listOf(1.0, 1.0, 5.001)))
            },
            JSONObject(baseline).apply {
                getJSONObject("grain").put("densityMin", org.json.JSONArray(listOf(-0.001, 0.1, 0.1)))
            },
            JSONObject(baseline).apply {
                getJSONObject("grain").put("densityMin", org.json.JSONArray(listOf(0.1, 0.501, 0.1)))
            },
            JSONObject(baseline).apply { getJSONObject("grain").put("nSubLayers", 0) },
            JSONObject(baseline).apply { getJSONObject("grain").put("nSubLayers", 6) },
            JSONObject(baseline).apply { getJSONObject("halation").put("nBounces", 0) },
            JSONObject(baseline).apply { getJSONObject("halation").put("nBounces", 6) },
            JSONObject(inactiveEffects).apply {
                getJSONObject("grain").put("particleAreaUm2", 0.000001)
            },
            JSONObject(inactiveEffects).apply { getJSONObject("halation").put("nBounces", 6) },
        )

        for (document in invalidDocuments) {
            assertThrows(IllegalArgumentException::class.java) {
                Presets.parseImportJson(document.toString())
            }
        }

        // Validation is detached: a bad late field cannot partially commit an earlier field.
        val malformed = JSONObject(Presets.toJsonString(ParamsState())).apply {
            put("filmProfile", "must-not-commit")
            getJSONObject("grain").put(
                "particleScale",
                org.json.JSONArray(listOf(0.000001, 1.0, 1.0)),
            )
        }
        val dst = ParamsState().apply { filmProfile = "keep-me" }
        assertThrows(IllegalArgumentException::class.java) { Presets.decode(malformed, dst) }
        assertEquals("keep-me", dst.filmProfile)
    }

    @Test
    fun import_acceptsResourceAmplificationDomainBoundaries() {
        for (state in listOf(
            ParamsState().apply {
                spectralGaussianBlur = 0f; upscaleFactor = 0f
                filmFormatMm = 8f
                grainParticleAreaUm2 = 0.2f
                grainParticleScale = Triple(0.1f, 0.1f, 0.1f)
                grainParticleScaleLayers = Triple(0.25f, 0.25f, 0.25f)
                grainDensityMin = Triple(0f, 0f, 0f)
                grainNSubLayers = 1; halNBounces = 1
            },
            ParamsState().apply {
                spectralGaussianBlur = 20f; upscaleFactor = 0.5f
                filmFormatMm = 120f
                grainParticleAreaUm2 = 2f
                grainParticleScale = Triple(5f, 5f, 5f)
                grainParticleScaleLayers = Triple(5f, 5f, 5f)
                grainDensityMin = Triple(0.5f, 0.5f, 0.5f)
                grainNSubLayers = 5; halNBounces = 5
            },
            ParamsState().apply { upscaleFactor = 4f },
        )) {
            Presets.parseImportJson(Presets.toJsonString(state))
        }
    }

    @Test
    fun currentStateEncoding_rejectsInactiveResourceAmplificationValues() {
        val unsafe = ParamsState().apply {
            grainActive = false
            grainSublayersActive = false
            grainParticleAreaUm2 = 0.000001f
        }

        assertThrows(IllegalArgumentException::class.java) {
            Presets.toJsonString(unsafe)
        }
        assertThrows(IllegalArgumentException::class.java) {
            Presets.encode(unsafe)
        }
    }

    @Test
    fun import_futureAndForeignSchemasAreExplicitlyUnsupported() {
        val future = assertThrows(UnsupportedPresetDocumentException::class.java) {
            Presets.parseImportJson(
                """{"schema":"$PRESET_SCHEMA_ID","version":${PRESET_VERSION + 1}}""",
            )
        }
        assertTrue(future.message.orEmpty().contains("version"))

        val foreign = assertThrows(UnsupportedPresetDocumentException::class.java) {
            Presets.parseImportJson(
                """{"schema":"example.foreign-preset","version":1}""",
            )
        }
        assertTrue(foreign.message.orEmpty().contains("schema"))

        val hugeFuture = assertThrows(UnsupportedPresetDocumentException::class.java) {
            Presets.parseImportJson(
                """{"schema":"$PRESET_SCHEMA_ID","version":922337203685477580812345}""",
            )
        }
        assertTrue(hugeFuture.message.orEmpty().contains("922337203685477580812345"))
    }

    @Test
    fun import_rejectsVersionDecimalsThatAndroidWouldRoundToCurrentIntegers() {
        assertThrows(IllegalArgumentException::class.java) {
            Presets.parseImportJson(
                """{"schema":"$PRESET_SCHEMA_ID","version":2.0000000000000001}""",
            )
        }
        assertThrows(IllegalArgumentException::class.java) {
            Presets.parseImportJson(
                """{"schema":"$PRESET_SCHEMA_ID","version":2,"masks":{"schema":"org.spektrafilm.mask-set","version":1.00000000000000001,"adjustments":[]}}""",
            )
        }
        assertThrows(IllegalArgumentException::class.java) {
            Presets.parseImportJson(
                """{"version":1,"grain":{"nSubLayers":2.0000000000000001}}""",
            )
        }
    }

    @Test
    fun decode_malformedNumericArraysAreRejectedBeforeMutation() {
        val malformed = JSONObject(Presets.toJsonString(ParamsState())).apply {
            put("filmProfile", "must-not-commit")
            getJSONObject("input").put("filterUv", org.json.JSONArray().put(1).put("2").put(3))
        }
        val dst = ParamsState().apply { filmProfile = "keep-me" }

        assertThrows(IllegalArgumentException::class.java) {
            Presets.decode(malformed, dst)
        }

        assertEquals("keep-me", dst.filmProfile)
    }

    @Test
    fun decode_malformedToneCurvePointIsRejectedInsteadOfSkipped() {
        val malformed = JSONObject(Presets.toJsonString(ParamsState())).apply {
            getJSONObject("toneCurve").put(
                "master",
                org.json.JSONArray().put(org.json.JSONArray().put(0.25)),
            )
        }

        assertThrows(IllegalArgumentException::class.java) {
            Presets.decode(malformed, ParamsState())
        }
    }

    @Test
    fun import_validVersionOneDocumentMigratesAndKeepsFiniteArrays() {
        val migrated = Presets.parseImportJson(
            """{
                "version":1,
                "filmProfile":"legacy-film",
                "input":{"filterUv":[0.1,0.2,0.3]}
            }""".trimIndent(),
        )
        val dst = ParamsState()
        Presets.decode(migrated, dst)

        assertEquals(PRESET_SCHEMA_ID, migrated.getString("schema"))
        assertEquals(PRESET_VERSION, migrated.getInt("version"))
        assertEquals("legacy-film", dst.filmProfile)
        assertEquals(0.1f, dst.filterUv.first, 1e-6f)
        assertEquals(0.2f, dst.filterUv.second, 1e-6f)
        assertEquals(0.3f, dst.filterUv.third, 1e-6f)
    }
}
