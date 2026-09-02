/*
 * Spektrafilm for Android — spatial HDR gain map tests (issue #140). GPLv3.
 *
 * The defect these guard against is not a crash: it is a file that claims to be Ultra HDR while
 * carrying no per-pixel information from the render. So the tests assert the map actually VARIES
 * with the image, and that an image with no headroom honestly encodes none.
 */
package com.spectrafilm.app

import com.spectrafilm.engine.ColorSpace
import kotlin.math.abs
import kotlin.math.ln
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class HdrGainMapTest {

    private fun compute(
        rgb: FloatArray,
        width: Int,
        height: Int,
        downsample: Int = 1,
        ceiling: Float = 4f,
    ) = HdrGainMap.compute(
        rgb = java.nio.FloatBuffer.wrap(rgb),
        width = width,
        height = height,
        colorSpace = ColorSpace.SRGB,
        cctfEncoded = true,
        downsample = downsample,
        ratioMaxCeiling = ceiling,
        epsilonSdr = 0.015625f,
        epsilonHdr = 0.015625f,
    )

    private fun flat(value: Float, width: Int, height: Int) =
        FloatArray(width * height * 3) { value }

    @Test
    fun `an image entirely at or below white claims no headroom`() {
        val result = compute(flat(0.5f, 4, 4), 4, 4)
        assertFalse("a fully-SDR render must not claim HDR", result.hasHeadroom)
        assertEquals(1f, result.ratioMax, 1e-6f)
        assertEquals(0, result.gainedPixels)
        assertTrue("a no-headroom map must be a no-op", result.alpha.all { it.toInt() == 0 })
    }

    @Test
    fun `exactly white claims no headroom`() {
        val result = compute(flat(1f, 4, 4), 4, 4)
        assertFalse(result.hasHeadroom)
        assertEquals(0, result.gainedPixels)
    }

    @Test
    fun `values above white produce real headroom`() {
        val result = compute(flat(1.5f, 4, 4), 4, 4)
        assertTrue("1.5 encoded is above white and must gain", result.hasHeadroom)
        assertEquals(16, result.gainedPixels)
        assertTrue("ratioMax should exceed 1", result.ratioMax > 1f)
    }

    /**
     * The whole point of #140: the map must carry PER-PIXEL information. A uniform map is exactly
     * the placeholder this replaces, so a varying image must produce a varying map.
     */
    @Test
    fun `a varying render produces a varying map, not a uniform one`() {
        val w = 8
        val h = 1
        val rgb = FloatArray(w * h * 3)
        for (x in 0 until w) {
            // Left half at white, right half increasingly above it.
            val v = if (x < 4) 1f else 1f + (x - 3) * 0.5f
            rgb[x * 3] = v
            rgb[x * 3 + 1] = v
            rgb[x * 3 + 2] = v
        }
        val result = compute(rgb, w, h)
        assertTrue(result.hasHeadroom)
        assertEquals(4, result.gainedPixels)
        val distinct = result.alpha.map { it.toInt() and 0xFF }.distinct()
        assertTrue("expected a spatially varying gain map, got $distinct", distinct.size > 2)
        // The SDR half must ask for no boost at all.
        assertEquals(0, result.alpha[0].toInt() and 0xFF)
        // The brightest pixel defines ratioMax, so it saturates the map.
        assertEquals(255, result.alpha[w - 1].toInt() and 0xFF)
    }

    @Test
    fun `the encoded gain inverts back to the rendered ratio`() {
        // g = log2(ratio)/log2(ratioMax), so re-applying the Gainmap formula must return the
        // ratio the render actually produced. Checked on an INTERMEDIATE pixel: the brightest
        // one defines ratioMax and would saturate to g=1 whatever the encoding did.
        val rgb = FloatArray(2 * 3)
        rgb[0] = 1.4f; rgb[1] = 1.4f; rgb[2] = 1.4f
        rgb[3] = 2.0f; rgb[4] = 2.0f; rgb[5] = 2.0f
        val result = compute(rgb, 2, 1, ceiling = 16f)

        val sdrLinear = OutputCctf.decode(ColorSpace.SRGB, 1f, true)
        fun rendered(encoded: Float) =
            (OutputCctf.decode(ColorSpace.SRGB, encoded, true) + 0.015625f) /
                (sdrLinear + 0.015625f)

        // The ceiling is out of the way, so the brightest pixel sets ratioMax exactly.
        assertEquals(rendered(2.0f).toDouble(), result.ratioMax.toDouble(), 1e-4)

        val g = (result.alpha[0].toInt() and 0xFF) / 255.0
        val logRatioMax = ln(result.ratioMax.toDouble()) / 0.6931471805599453
        val reconstructed = Math.pow(2.0, g * logRatioMax)
        val expected = rendered(1.4f).toDouble()
        assertTrue(
            "reconstructed $reconstructed should match the rendered ratio $expected",
            abs(reconstructed - expected) < 0.02,
        )
        assertEquals("the brightest pixel saturates", 255, result.alpha[1].toInt() and 0xFF)
    }

    @Test
    fun `the ceiling caps how much headroom may be claimed`() {
        val result = compute(flat(8f, 2, 2), 2, 2, ceiling = 2f)
        assertEquals("ratioMax must not exceed the ceiling", 2f, result.ratioMax, 1e-6f)
        assertTrue("the true ratio is still reported honestly", result.maxRatio > 2f)
    }

    @Test
    fun `downsampling shrinks the map and averages the block`() {
        val w = 8
        val h = 8
        val rgb = flat(1f, w, h)
        // One quadrant above white.
        for (y in 0 until 4) {
            for (x in 0 until 4) {
                val o = (y * w + x) * 3
                rgb[o] = 2f; rgb[o + 1] = 2f; rgb[o + 2] = 2f
            }
        }
        val result = compute(rgb, w, h, downsample = 4)
        assertEquals(2, result.width)
        assertEquals(2, result.height)
        assertEquals(4, result.alpha.size)
        assertEquals("the bright quadrant saturates", 255, result.alpha[0].toInt() and 0xFF)
        assertEquals("the flat quadrant asks for nothing", 0, result.alpha[1].toInt() and 0xFF)
    }

    @Test
    fun `a non-multiple size still covers every pixel`() {
        val result = compute(flat(1.5f, 5, 3), 5, 3, downsample = 2)
        assertEquals(3, result.width)
        assertEquals(2, result.height)
        assertEquals(6, result.alpha.size)
    }

    @Test
    fun `NaN samples do not poison the map`() {
        // The scanning stage relies on NaN propagation for profile nulls, so NaN can reach here.
        val rgb = flat(1f, 2, 2)
        rgb[0] = Float.NaN
        val result = compute(rgb, 2, 2)
        assertFalse(result.hasHeadroom)
        assertTrue(result.alpha.all { it.toInt() == 0 })
    }

    @Test
    fun `negative samples are treated as below white`() {
        val result = compute(flat(-0.5f, 2, 2), 2, 2)
        assertFalse(result.hasHeadroom)
        assertEquals(0, result.gainedPixels)
    }
}
