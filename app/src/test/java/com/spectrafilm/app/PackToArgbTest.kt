/*
 * Spektrafilm for Android — unit tests for the ARGB packing hot loop. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * packToArgb is the per-pixel loop of simResultToBitmap, split out so a JVM test can gate
 * it — the Bitmap.setPixels beside it cannot be tested off-device (android.jar's Bitmap is
 * a stub that throws). These assert the exact contract rather than comparing against a
 * restatement of the same formula: clamping at both ends, the round-half-up, opaque alpha,
 * channel order, and NaN landing on 0.
 */
package com.spectrafilm.app

import org.junit.Assert.assertEquals
import org.junit.Test

class PackToArgbTest {

    private fun packOne(r: Float, g: Float, b: Float): Int {
        val dst = IntArray(1)
        packToArgb(floatArrayOf(r, g, b), dst, 1)
        return dst[0]
    }

    @Test
    fun endpointsAndClamping() {
        assertEquals(0xFF000000.toInt(), packOne(0f, 0f, 0f))
        assertEquals(0xFFFFFFFF.toInt(), packOne(1f, 1f, 1f))
        // Out of range in both directions clamps, it does not wrap.
        assertEquals(0xFFFFFFFF.toInt(), packOne(2f, 9999f, 1.0001f))
        assertEquals(0xFF000000.toInt(), packOne(-1f, -0.0001f, -9999f))
    }

    @Test
    fun channelOrderIsArgb() {
        assertEquals(0xFFFF0000.toInt(), packOne(1f, 0f, 0f))
        assertEquals(0xFF00FF00.toInt(), packOne(0f, 1f, 0f))
        assertEquals(0xFF0000FF.toInt(), packOne(0f, 0f, 1f))
    }

    @Test
    fun roundsHalfUp() {
        // 0.5 * 255 = 127.5, +0.5 -> 128. The half-up is deliberate, not truncation.
        assertEquals(128, packOne(0.5f, 0f, 0f) shr 16 and 0xFF)
        // Just under the next step still rounds up once past .5.
        assertEquals(1, packOne(1f / 255f, 0f, 0f) shr 16 and 0xFF)
    }

    @Test
    fun nanClampsToZeroAndStaysOpaque() {
        assertEquals(0xFF000000.toInt(), packOne(Float.NaN, Float.NaN, Float.NaN))
        assertEquals(0xFF00FF00.toInt(), packOne(Float.NaN, 1f, Float.NaN))
    }

    @Test
    fun alphaIsAlwaysOpaque() {
        for (v in floatArrayOf(0f, 0.25f, 0.5f, 1f, -3f, 7f, Float.NaN)) {
            assertEquals("alpha for $v", 0xFF, packOne(v, v, v) ushr 24)
        }
    }

    @Test
    fun packsOnlyTheRequestedCount() {
        // The strip buffer is sized for a full band; a short final band must not write past
        // its own pixel count.
        val src = FloatArray(9) { 1f }
        val dst = IntArray(3) { 0x12345678 }
        packToArgb(src, dst, 2)
        assertEquals(0xFFFFFFFF.toInt(), dst[0])
        assertEquals(0xFFFFFFFF.toInt(), dst[1])
        assertEquals("third pixel must be untouched", 0x12345678, dst[2])
    }
}
