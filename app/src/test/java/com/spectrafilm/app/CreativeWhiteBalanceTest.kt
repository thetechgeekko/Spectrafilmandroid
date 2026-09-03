/*
 * Spektrafilm for Android — unit tests for creative white balance. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Pure color math (Bradford CAT on linear ProPhoto), JVM-testable with no native/Android deps.
 */
package com.spectrafilm.app

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CancellationException
import java.util.concurrent.atomic.AtomicInteger

class CreativeWhiteBalanceTest {

    private fun applyToGray(temp: Float, tint: Float, v: Float = 0.5f): FloatArray {
        val buf = ByteBuffer.allocateDirect(3 * 4).order(ByteOrder.nativeOrder())
        val f = buf.asFloatBuffer(); f.put(0, v); f.put(1, v); f.put(2, v)
        CreativeWhiteBalance.applyInPlace(buf, 1, CreativeWhiteBalance.matrix(temp, tint))
        val g = buf.duplicate().order(ByteOrder.nativeOrder()).asFloatBuffer()
        return floatArrayOf(g.get(0), g.get(1), g.get(2))
    }

    @Test
    fun neutral_isIdentity() {
        assertTrue(CreativeWhiteBalance.isNeutral(0f, 0f))
        assertFalse(CreativeWhiteBalance.isNeutral(5f, 0f))
        assertFalse(CreativeWhiteBalance.isNeutral(0f, 5f))
        val m = CreativeWhiteBalance.matrix(0f, 0f)
        val id = floatArrayOf(1f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 1f)
        for (i in 0..8) assertEquals("m[$i]", id[i], m[i], 1e-6f)
        // a gray pixel is unchanged at neutral
        val out = applyToGray(0f, 0f)
        for (c in out) assertEquals(0.5f, c, 1e-6f)
    }

    @Test
    fun whitepoint_at_d50_matches() {
        val w = CreativeWhiteBalance.whitepointXyz(5003.0) // ~D50
        assertEquals(0.9642, w[0], 2e-3)  // X
        assertEquals(1.0, w[1], 1e-9)     // Y
        assertEquals(0.8251, w[2], 3e-3)  // Z
    }

    @Test
    fun warmth_direction() {
        // temp>0 warms: red rises above blue on a neutral gray; temp<0 cools (red below blue).
        val warm = applyToGray(60f, 0f)
        assertTrue("warm R>B (${warm[0]} vs ${warm[2]})", warm[0] > warm[2])
        val cool = applyToGray(-60f, 0f)
        assertTrue("cool R<B (${cool[0]} vs ${cool[2]})", cool[0] < cool[2])
    }

    @Test
    fun tint_direction() {
        // tint>0 = magenta (green pulled down below R/B); tint<0 = green (green pushed up).
        val magenta = applyToGray(0f, 60f)
        assertTrue("magenta G<R", magenta[1] < magenta[0])
        assertTrue("magenta G<B", magenta[1] < magenta[2])
        val green = applyToGray(0f, -60f)
        assertTrue("green G>R", green[1] > green[0])
    }

    @Test
    fun applyInPlace_multiPixel_usesMatrix() {
        // Two pixels, a pure green-gain matrix (temp=0, tint>0) scales only the green channel.
        val n = 2
        val buf = ByteBuffer.allocateDirect(n * 3 * 4).order(ByteOrder.nativeOrder())
        val f = buf.asFloatBuffer()
        for (p in 0 until n) { f.put(p * 3, 0.4f); f.put(p * 3 + 1, 0.4f); f.put(p * 3 + 2, 0.4f) }
        val m = CreativeWhiteBalance.matrix(0f, 50f)
        CreativeWhiteBalance.applyInPlace(buf, n, m)
        val g = buf.duplicate().order(ByteOrder.nativeOrder()).asFloatBuffer()
        for (p in 0 until n) {
            assertEquals("R unchanged", 0.4f, g.get(p * 3), 1e-6f)
            assertTrue("G reduced", g.get(p * 3 + 1) < 0.4f)
            assertEquals("B unchanged", 0.4f, g.get(p * 3 + 2), 1e-6f)
        }
    }

    @Test
    fun applyInPlace_pollsCancellationDuringLongLoop() {
        val pixels = 5_000
        val data = ByteBuffer.allocateDirect(pixels * 3 * Float.SIZE_BYTES)
            .order(ByteOrder.nativeOrder())
        val checks = AtomicInteger(0)

        try {
            CreativeWhiteBalance.applyInPlace(
                data,
                pixels,
                CreativeWhiteBalance.matrix(20f, 10f),
                isCancelled = { checks.incrementAndGet() >= 3 },
            )
            fail("creative white-balance loop must observe cancellation")
        } catch (_: CancellationException) {
            // Expected.
        }

        assertTrue("long loop must poll repeatedly", checks.get() >= 3)
    }
}
