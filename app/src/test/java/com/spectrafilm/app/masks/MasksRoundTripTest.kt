/*
 * Spektrafilm for Android — unit tests for mask recipe serialization. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Guards that a local-adjustment stack survives the recipe `"masks"` JSON round-trip byte-faithfully
 * (so saved recipes restore masks), and that a missing/empty block degrades to today's global-only
 * behavior. Uses real org.json on the test classpath (same as PresetsRoundTripTest).
 */
package com.spectrafilm.app.masks

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.assertThrows
import org.junit.Test

class MasksRoundTripTest {

    private val sample = listOf(
        LocalAdjustment(
            Mask(
                components = listOf(
                    Mask.Component(BlendMode.ADD, MaskComponent.Radial(0.5f, 0.5f, 0.3f, 0.2f, 0.4f, angleDeg = 30f)),
                    Mask.Component(BlendMode.INTERSECT, MaskComponent.Linear(0.1f, 0.2f, 0.8f, 0.9f), invert = true, value = 0.5f),
                ),
                invert = false, opacity = 0.75f,
            ),
            TierADelta(exposureEv = 0.5f, temp = -10f, tint = 5f, saturation = 20f, contrast = -15f),
        ),
        LocalAdjustment(
            Mask(listOf(Mask.Component(BlendMode.ADD, MaskComponent.Radial(0.25f, 0.75f, 0.15f, 0.15f)))),
            TierADelta(exposureEv = -1f),
        ),
    )

    @Test
    fun roundTrip_isFaithful() {
        val restored = MaskJson.fromJson(MaskJson.toJson(sample))
        assertEquals(sample, restored)
    }

    @Test
    fun missingBlock_isEmpty() {
        assertTrue(MaskJson.fromJson(null).isEmpty())
        assertEquals(0, MaskJson.toJson(emptyList()).getJSONArray("adjustments").length())
    }

    @Test
    fun preservesPerComponentFlagsAndAngle() {
        val restored = MaskJson.fromJson(MaskJson.toJson(sample))
        val comp = restored[0].mask.components[1]
        assertEquals(BlendMode.INTERSECT, comp.mode)
        assertTrue("per-component invert kept", comp.invert)
        assertEquals("per-component value kept", 0.5f, comp.value, 0f)
        val radial = restored[0].mask.components[0].shape as MaskComponent.Radial
        assertEquals("radial angle kept", 30f, radial.angleDeg, 0f)
    }

    @Test
    fun currentDocument_hasStableSchemaIdAndVersion() {
        val json = MaskJson.toJson(sample)

        assertEquals("org.spektrafilm.mask-set", json.getString("schema"))
        assertEquals(1, json.getInt("version"))
        assertEquals(sample.size, json.getJSONArray("adjustments").length())
    }

    @Test
    fun legacyArray_migratesWithoutChangingMaskMeaning() {
        val current = MaskJson.toJson(sample)
        val legacy = current.getJSONArray("adjustments")

        assertEquals(sample, MaskJson.fromJson(legacy))
    }

    @Test
    fun unsupportedFutureVersion_isRejectedInsteadOfPartiallyApplied() {
        val future = JSONObject()
            .put("schema", "org.spektrafilm.mask-set")
            .put("version", 999)
            .put("adjustments", JSONArray())

        assertThrows(IllegalArgumentException::class.java) { MaskJson.fromJson(future) }
    }

    @Test
    fun schemaVersion_rejectsCoercedFractionStringAndOverflow() {
        for (invalid in listOf<Any>(1.9, "1", 4_294_967_297L)) {
            val document = JSONObject()
                .put("schema", MaskJson.SCHEMA_ID)
                .put("version", invalid)
                .put("adjustments", JSONArray())

            assertThrows(IllegalArgumentException::class.java) { MaskJson.fromJson(document) }
        }

        val mathematicallyIntegral = JSONObject()
            .put("schema", MaskJson.SCHEMA_ID)
            .put("version", 1.0)
            .put("adjustments", JSONArray())
        assertEquals(emptyList<LocalAdjustment>(), MaskJson.fromJson(mathematicallyIntegral))
    }

    @Test
    fun adjustmentAndComponentLimits_areEnforced() {
        val tooManyAdjustments = JSONObject()
            .put("schema", "org.spektrafilm.mask-set")
            .put("version", 1)
            .put("adjustments", JSONArray().apply {
                repeat(MaskJson.MAX_ADJUSTMENTS + 1) { put(JSONObject()) }
            })
        assertThrows(IllegalArgumentException::class.java) {
            MaskJson.fromJson(tooManyAdjustments)
        }

        val tooManyComponents = MaskJson.toJson(sample)
        tooManyComponents.getJSONArray("adjustments").getJSONObject(0)
            .getJSONObject("mask")
            .put("components", JSONArray().apply {
                repeat(MaskJson.MAX_COMPONENTS_PER_MASK + 1) { put(JSONObject()) }
            })
        assertThrows(IllegalArgumentException::class.java) {
            MaskJson.fromJson(tooManyComponents)
        }
    }
}
