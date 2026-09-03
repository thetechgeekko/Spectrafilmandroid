package com.spectrafilm.app

import com.spectrafilm.engine.LinearImage
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicInteger
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotSame
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

class GpuProxySnapshotTest {
    @Test
    fun snapshotOwnsLogicalPixelsAfterSourceCloseAndReusesIdentity() {
        val original = ByteBuffer.allocateDirect(20).order(ByteOrder.nativeOrder())
        original.putFloat(4, 0.125f)
        original.putFloat(8, 0.25f)
        original.putFloat(12, 0.5f)
        original.position(4)
        original.limit(16)
        val closes = AtomicInteger()
        val image = LinearImage.fromDataLease(
            data = original,
            width = 1,
            height = 1,
            lease = AutoCloseable { closes.incrementAndGet() },
        )
        val cache = ProxySnapshotCache()

        val first = cache.snapshotOf(image)
        assertSame(first, cache.snapshotOf(image))

        // Neither source-view mutation nor its native-owner release can affect
        // the immutable renderer copy. A late recomposition of the same identity
        // also reuses the copy instead of attempting a lease after close.
        original.putFloat(4, 9.0f)
        image.close()
        assertEquals(1, closes.get())
        assertSame(first, cache.snapshotOf(image))

        val values = first.pixelsView().asFloatBuffer()
        assertEquals(0.125f, values.get(0), 0.0f)
        assertEquals(0.25f, values.get(1), 0.0f)
        assertEquals(0.5f, values.get(2), 0.0f)

        val a = first.pixelsView()
        val b = first.pixelsView()
        a.position(4)
        a.limit(8)
        assertEquals(0, b.position())
        assertEquals(12, b.limit())
        assertTrue(a.isReadOnly && b.isReadOnly)

        val replacementBuffer = ByteBuffer.allocateDirect(12).order(ByteOrder.nativeOrder())
        val replacement = LinearImage(replacementBuffer, 1, 1)
        assertNotSame(first, cache.snapshotOf(replacement))
    }

    @Test
    fun snapshotRejectsShortLogicalWindowBeforeCopy() {
        val short = ByteBuffer.allocateDirect(12).order(ByteOrder.nativeOrder())
        short.limit(8)
        val image = LinearImage(short, 1, 1)
        try {
            ProxySnapshot.capture(image)
            fail("expected short logical window rejection")
        } catch (expected: IllegalArgumentException) {
            assertTrue(expected.message.orEmpty().contains("requires 12"))
        }
    }

    @Test
    fun snapshotRejectsDimensionsWhoseRgbByteCountOverflowsLong() {
        val image = LinearImage(
            ByteBuffer.allocateDirect(12).order(ByteOrder.nativeOrder()),
            width = 900_000_002,
            height = 1_708_031_855,
        )

        try {
            ProxySnapshot.capture(image)
            fail("expected overflowing dimensions to be rejected")
        } catch (expected: IllegalArgumentException) {
            assertTrue(expected.message.orEmpty().contains("exceeds direct-buffer limits"))
        }
    }

    @Test
    fun pendingSubmissionPreservesWholeGenerationsAcrossTakePublishAndContextRestore() {
        val firstProxy = snapshotOf(0.25f)
        val secondProxy = snapshotOf(0.75f)
        val firstLut = CubeLut(2, FloatArray(24) { 0.1f })
        val secondLut = CubeLut(2, FloatArray(24) { 0.9f })
        val first = GpuSubmission(firstProxy, firstLut, exposureGain = 2f)
        val second = GpuSubmission(secondProxy, secondLut, exposureGain = 4f)
        val pending = PendingGpuSubmission()

        pending.publish(first)
        val uploading = requireNotNull(pending.take())
        pending.publish(second)

        assertSame(firstProxy, uploading.proxy)
        assertSame(firstLut, uploading.lut)
        assertEquals(2f, uploading.exposureGain, 0f)
        assertSame(second, pending.take())

        pending.restoreIfEmpty(first)
        pending.publish(second)
        pending.restoreIfEmpty(first)
        assertSame(second, pending.take())
        pending.restoreIfEmpty(first)
        assertSame(first, pending.take())
    }

    private fun snapshotOf(value: Float): ProxySnapshot {
        val data = ByteBuffer.allocateDirect(3 * Float.SIZE_BYTES)
            .order(ByteOrder.nativeOrder())
        data.asFloatBuffer().apply {
            put(0, value)
            put(1, value)
            put(2, value)
        }
        return ProxySnapshot.capture(LinearImage(data, width = 1, height = 1))
    }
}
