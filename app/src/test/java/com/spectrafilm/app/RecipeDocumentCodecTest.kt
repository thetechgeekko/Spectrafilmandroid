/*
 * Spektrafilm for Android — versioned recipe envelope tests. GPLv3.
 */
package com.spectrafilm.app

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class RecipeDocumentCodecTest {

    private val key = "a".repeat(64)
    private val currentParams = Presets.toJsonString(ParamsState())

    @Test
    fun encodeDecode_emitsCurrentSchemaAndPreservesMetadata() {
        val text = RecipeDocumentCodec.encode(
            sourceKey = key,
            paramsJson = currentParams,
            sourceName = "frame.dng",
            manualRotationDeg = 90,
            updatedAtMillis = 1234L,
        )
        val root = JSONObject(text)
        val decoded = RecipeDocumentCodec.decode(text, expectedSourceKey = key)

        assertEquals("org.spektrafilm.recipe", root.getString("schema"))
        assertEquals(2, root.getInt("recipeVersion"))
        assertEquals(key, decoded.sourceKey)
        assertEquals("frame.dng", decoded.sourceName)
        assertEquals(90, decoded.manualRotationDeg)
        assertEquals(1234L, decoded.updatedAtMillis)
        assertEquals(PRESET_VERSION, decoded.params.getInt("version"))
    }

    @Test
    fun legacyV1Envelope_migratesPresetAndRotation() {
        val legacy = JSONObject()
            .put("recipeVersion", 1)
            .put("sourceKey", key)
            .put("sourceName", "old.nef")
            .put("updatedAt", 5L)
            .put("manualRotationDeg", 270)
            .put("params", JSONObject().put("version", 1).put("filmProfile", "legacy-film"))

        val decoded = RecipeDocumentCodec.decode(legacy.toString(), key)

        assertEquals(270, decoded.manualRotationDeg)
        assertEquals(PRESET_VERSION, decoded.params.getInt("version"))
        assertEquals(PRESET_SCHEMA_ID, decoded.params.getString("schema"))
        assertEquals("legacy-film", decoded.params.getString("filmProfile"))
    }

    @Test
    fun legacyV1Envelope_rejectsExplicitForeignSchema() {
        val legacy = JSONObject()
            .put("schema", "example.foreign-recipe")
            .put("recipeVersion", 1)
            .put("sourceKey", key)
            .put("sourceName", "old.nef")
            .put("params", JSONObject().put("version", 1))

        assertThrows(IllegalArgumentException::class.java) {
            RecipeDocumentCodec.decode(legacy.toString(), key)
        }
    }

    @Test
    fun mismatchedSourceKey_isRejected() {
        val text = RecipeDocumentCodec.encode(key, currentParams, "x.dng", 0, 1L)
        assertThrows(IllegalArgumentException::class.java) {
            RecipeDocumentCodec.decode(text, "b".repeat(64))
        }
    }

    @Test
    fun futureRecipeVersion_isRejected() {
        val root = JSONObject(RecipeDocumentCodec.encode(key, currentParams, "x.dng", 0, 1L))
            .put("recipeVersion", 999)
        assertThrows(IllegalArgumentException::class.java) {
            RecipeDocumentCodec.decode(root.toString(), key)
        }
    }

    @Test
    fun recipeVersion_rejectsFractionAndStringWithoutTruncatingLargeFutureVersion() {
        for (invalid in listOf<Any>(2.9, "2")) {
            val root = JSONObject(RecipeDocumentCodec.encode(key, currentParams, "x.dng", 0, 1L))
                .put("recipeVersion", invalid)
            assertThrows(IllegalArgumentException::class.java) {
                RecipeDocumentCodec.decode(root.toString(), key)
            }
        }

        val overflow = JSONObject(RecipeDocumentCodec.encode(key, currentParams, "x.dng", 0, 1L))
            .put("recipeVersion", 4_294_967_298L)
        val unsupported = assertThrows(UnsupportedRecipeVersionException::class.java) {
            RecipeDocumentCodec.decode(overflow.toString(), key)
        }
        assertEquals("4294967298", unsupported.version)

        val integral = JSONObject(RecipeDocumentCodec.encode(key, currentParams, "x.dng", 0, 1L))
            .put("recipeVersion", 2.0)
        assertEquals(key, RecipeDocumentCodec.decode(integral.toString(), key).sourceKey)
    }

    @Test
    fun futureRecipeEnvelopeWithoutParams_isRejectedInsteadOfMigratedAsBarePreset() {
        val root = JSONObject()
            .put("schema", RecipeDocumentCodec.SCHEMA_ID)
            .put("recipeVersion", 999)
            .put("sourceKey", key)
        assertThrows(IllegalArgumentException::class.java) {
            RecipeDocumentCodec.decode(root.toString(), key)
        }
    }

    @Test
    fun invalidKeyRotationTimestampAndOversizedName_areRejected() {
        assertThrows(IllegalArgumentException::class.java) {
            RecipeDocumentCodec.encode("../escape", currentParams, "x", 0, 1L)
        }
        assertThrows(IllegalArgumentException::class.java) {
            RecipeDocumentCodec.encode(key, currentParams, "x", 45, 1L)
        }
        assertThrows(IllegalArgumentException::class.java) {
            RecipeDocumentCodec.encode(key, currentParams, "x", 0, -1L)
        }
        assertThrows(IllegalArgumentException::class.java) {
            RecipeDocumentCodec.encode(key, currentParams, "x".repeat(513), 0, 1L)
        }
    }

    @Test
    fun recipeEncodeAndDecode_rejectResourceAmplificationParams() {
        val inactiveGrainParams = Presets.toJsonString(ParamsState().apply {
            grainActive = false
            grainSublayersActive = false
        })
        val unsafeForEncode = JSONObject(inactiveGrainParams).apply {
            getJSONObject("grain").put("particleAreaUm2", 0.000001)
        }
        assertThrows(IllegalArgumentException::class.java) {
            RecipeDocumentCodec.encode(key, unsafeForEncode.toString(), "x.dng", 0, 1L)
        }

        val unsafeForDecode = JSONObject(
            RecipeDocumentCodec.encode(key, inactiveGrainParams, "x.dng", 0, 1L),
        ).apply {
            getJSONObject("params").getJSONObject("grain").put(
                "particleScale",
                org.json.JSONArray(listOf(0.000001, 1.0, 1.0)),
            )
        }
        assertThrows(IllegalArgumentException::class.java) {
            RecipeDocumentCodec.decode(unsafeForDecode.toString(), key)
        }
    }

    @Test
    fun decode_rejectsCoercedMetadataTypesAndFractions() {
        fun document(): JSONObject = JSONObject(
            RecipeDocumentCodec.encode(key, currentParams, "frame.dng", 90, 1L),
        )

        for ((field, invalid) in listOf(
            "sourceName" to 7,
            "updatedAt" to -0.5,
            "updatedAt" to "1",
            "manualRotationDeg" to 90.9,
            "manualRotationDeg" to "90",
            "manualRotationDeg" to 4_294_967_386L,
        )) {
            val malformed = document().put(field, invalid)
            assertThrows(IllegalArgumentException::class.java) {
                RecipeDocumentCodec.decode(malformed.toString(), key)
            }
        }
    }

    @Test
    fun decode_rejectsDecimalsThatAndroidJsonTokenerWouldRoundToIntegers() {
        fun document(version: String, updatedAt: String, rotation: String): String =
            """{"schema":"${RecipeDocumentCodec.SCHEMA_ID}","recipeVersion":$version,"sourceKey":"$key","sourceName":"frame.dng","updatedAt":$updatedAt,"manualRotationDeg":$rotation,"params":$currentParams}"""

        for (malformed in listOf(
            document("2.0000000000000001", "1", "90"),
            document("2", "1.00000000000000001", "90"),
            document("2", "1", "90.000000000000001"),
        )) {
            assertThrows(IllegalArgumentException::class.java) {
                RecipeDocumentCodec.decode(malformed, key)
            }
        }
    }
}
