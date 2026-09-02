/*
 * Spektrafilm for Android — on-device presets / recipes / masks test (PR1). GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * The non-destructive editing layer, on-device:
 *   - Presets: toJsonString -> decode preserves the editing state field-for-field.
 *   - Recipes: save (enveloped sidecar in filesDir) -> load restores the params payload.
 *   - Masks:   MaskCompositor.applyInPlace composites a radial local adjustment onto a REAL engine
 *              SimResult buffer — the seam that makes masks do something — changing the masked
 *              center while leaving the unmasked corner byte-identical.
 */
package com.spectrafilm.app

import com.spectrafilm.app.masks.BlendMode
import com.spectrafilm.app.masks.LocalAdjustment
import com.spectrafilm.app.masks.Mask
import com.spectrafilm.app.masks.MaskComponent
import com.spectrafilm.app.masks.MaskCompositor
import com.spectrafilm.app.masks.TierADelta
import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.LinearImage
import java.nio.ByteOrder
import kotlin.math.abs
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import androidx.test.ext.junit.runners.AndroidJUnit4

@RunWith(AndroidJUnit4::class)
class PresetsRecipesMasksTest {

    @Test
    fun presets_jsonRoundTrip_preservesFields() {
        val src = ParamsState().apply {
            filmProfile = "kodak_ektar_100"
            printProfile = "kodak_supra_endura"
            exposureCompensationEv = 1.25f
            outputColorSpace = ColorSpace.ADOBE_RGB
            scanUnsharpMask = 0.4f to 0.9f
            crop = true
        }
        val dst = ParamsState()
        Presets.decode(JSONObject(Presets.toJsonString(src)), dst)

        assertEquals("kodak_ektar_100", dst.filmProfile)
        assertEquals("kodak_supra_endura", dst.printProfile)
        assertEquals(1.25f, dst.exposureCompensationEv, 1e-4f)
        assertEquals(ColorSpace.ADOBE_RGB, dst.outputColorSpace)
        assertEquals(0.4f, dst.scanUnsharpMask.first, 1e-4f)
        assertEquals(0.9f, dst.scanUnsharpMask.second, 1e-4f)
        assertTrue(dst.crop)
    }

    @Test
    fun recipes_saveLoadEnvelope_restoresParams() {
        val ctx = DeviceTestSupport.targetCtx()
        val key = "device_test_recipe_key"
        val src = ParamsState().apply {
            filmProfile = "fujifilm_provia_100f"
            exposureCompensationEv = -0.5f
            outputColorSpace = ColorSpace.REC2020
        }
        try {
            Recipes.save(ctx, key, src, sourceName = "device_test.dng", rotationDegrees = 90)
            val dst = ParamsState()
            val applied = Recipes.load(ctx, key, dst)
            assertTrue("recipe existed and applied", applied)
            assertEquals("fujifilm_provia_100f", dst.filmProfile)
            assertEquals(-0.5f, dst.exposureCompensationEv, 1e-4f)
            assertEquals(ColorSpace.REC2020, dst.outputColorSpace)
            assertEquals("manual rotation persisted", SourceRotation.fromDegrees(90), Recipes.loadRotation(ctx, key))
        } finally {
            Recipes.delete(ctx, key)
        }
    }

    @Test
    fun masks_compositeOnSimResult_changesCenter_keepsCorner() {
        val w = 32
        val h = 32
        val engine = DeviceTestSupport.newEngine()
        try {
            LinearImage(DeviceTestSupport.uniformImage(w, h, 0.18f), w, h, "ProPhoto RGB").use { img ->
                engine.simulate(img, DeviceTestSupport.scanParams()).use { res ->
                    // Uniform input => uniform SimResult: center and corner start byte-identical.
                    val centerBefore = pixelR(res.data, res.width, 16, 16)
                    val cornerBefore = pixelR(res.data, res.width, 0, 0)
                    assertEquals("uniform output", cornerBefore, centerBefore, 1e-6f)

                    val radial = LocalAdjustment(
                        Mask(listOf(Mask.Component(BlendMode.ADD, MaskComponent.Radial(0.5f, 0.5f, 0.3f, 0.3f, 0.5f)))),
                        TierADelta(exposureEv = 2f),
                    )
                    MaskCompositor.applyInPlace(res.data, res.width, res.height, res.colorSpace, true, listOf(radial))

                    val centerAfter = pixelR(res.data, res.width, 16, 16)
                    val cornerAfter = pixelR(res.data, res.width, 0, 0)
                    assertTrue("masked center changed (${centerBefore} -> ${centerAfter})",
                        abs(centerAfter - centerBefore) > 1e-3f)
                    assertEquals("unmasked corner byte-identical", cornerBefore, cornerAfter, 0f)
                }
            }
        } finally {
            engine.close()
        }
    }

    private fun pixelR(data: java.nio.ByteBuffer, width: Int, x: Int, y: Int): Float {
        val f = data.duplicate().order(ByteOrder.nativeOrder())
        f.rewind()
        return f.asFloatBuffer().get((y * width + x) * 3)
    }
}
