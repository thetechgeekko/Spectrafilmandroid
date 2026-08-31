/*
 * Spektrafilm for Android — unit tests for source-image rotation. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Proves the pure geometry of Rotation.kt on a tiny LinearImage: rotated() 90/180/270
 * pixel mappings + dimension swaps, flippedHorizontal(), a 4×90° round-trip, and the
 * SourceRotation enum algebra (next/then/fromDegrees). ExifInterface mapping and the
 * >2 GB allocation guard are out of scope (the former needs android.jar, the latter a
 * huge allocation).
 */
package com.spectrafilm.app

import com.spectrafilm.engine.LinearImage
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CancellationException
import java.util.concurrent.atomic.AtomicInteger
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

class RotationTest {

    private val W = 3
    private val H = 2

    /**
     * A [W]×[H] image where every channel encodes its source coordinate:
     * r = x, g = y, b = linear pixel index. Any misplaced pixel is detectable.
     */
    private fun coordImage(w: Int = W, h: Int = H): LinearImage {
        val b = ByteBuffer.allocateDirect(w * h * 3 * 4).order(ByteOrder.nativeOrder())
        val f = b.asFloatBuffer()
        for (y in 0 until h) {
            for (x in 0 until w) {
                val k = (y * w + x) * 3
                f.put(k, x.toFloat())
                f.put(k + 1, y.toFloat())
                f.put(k + 2, (y * w + x).toFloat())
            }
        }
        return LinearImage(b, w, h)
    }

    private fun chan(img: LinearImage, x: Int, y: Int, c: Int): Float =
        img.acquireDataLease().use { lease ->
            val data = lease.data
            data.asFloatBuffer().get((y * img.width + x) * 3 + c)
        }

    /** Assert the pixel at ([x],[y]) of [img] came from source coordinate ([sx],[sy]). */
    private fun assertFrom(img: LinearImage, x: Int, y: Int, sx: Int, sy: Int) {
        assertEquals("r (src x) at ($x,$y)", sx.toFloat(), chan(img, x, y, 0), 0f)
        assertEquals("g (src y) at ($x,$y)", sy.toFloat(), chan(img, x, y, 1), 0f)
        assertEquals("b (src idx) at ($x,$y)", (sy * W + sx).toFloat(), chan(img, x, y, 2), 0f)
    }

    // --- rotated() ---

    @Test
    fun rotateNone_returnsSameInstanceNoCopy() {
        val img = coordImage()
        assertSame(img, img.rotated(SourceRotation.NONE))
    }

    @Test
    fun rotate90_swapsDimensionsAndMapsPixels() {
        val out = coordImage().rotated(SourceRotation.CW90)
        assertEquals(H, out.width)
        assertEquals(W, out.height)
        // CW90: source (x,y) lands at (H-1-y, x).
        for (y in 0 until H) for (x in 0 until W) {
            assertFrom(out, H - 1 - y, x, x, y)
        }
    }

    @Test
    fun rotate180_keepsDimensionsAndMapsPixels() {
        val out = coordImage().rotated(SourceRotation.CW180)
        assertEquals(W, out.width)
        assertEquals(H, out.height)
        // CW180: source (x,y) lands at (W-1-x, H-1-y).
        for (y in 0 until H) for (x in 0 until W) {
            assertFrom(out, W - 1 - x, H - 1 - y, x, y)
        }
    }

    @Test
    fun rotate270_swapsDimensionsAndMapsPixels() {
        val out = coordImage().rotated(SourceRotation.CW270)
        assertEquals(H, out.width)
        assertEquals(W, out.height)
        // CW270: source (x,y) lands at (y, W-1-x).
        for (y in 0 until H) for (x in 0 until W) {
            assertFrom(out, y, W - 1 - x, x, y)
        }
    }

    @Test
    fun fourQuarterTurns_roundTripToOriginal() {
        var img = coordImage()
        repeat(4) { img = img.rotated(SourceRotation.CW90) }
        assertEquals(W, img.width)
        assertEquals(H, img.height)
        for (y in 0 until H) for (x in 0 until W) assertFrom(img, x, y, x, y)
    }

    // --- flippedHorizontal() ---

    @Test
    fun flipHorizontal_mirrorsLeftToRight() {
        val out = coordImage().flippedHorizontal()
        assertEquals(W, out.width)
        assertEquals(H, out.height)
        // Flip H: source (x,y) lands at (W-1-x, y).
        for (y in 0 until H) for (x in 0 until W) {
            assertFrom(out, W - 1 - x, y, x, y)
        }
    }

    @Test
    fun flipHorizontal_twice_isIdentity() {
        val out = coordImage().flippedHorizontal().flippedHorizontal()
        for (y in 0 until H) for (x in 0 until W) assertFrom(out, x, y, x, y)
    }

    // --- SourceRotation algebra ---

    @Test
    fun next_cyclesThroughQuarterTurns() {
        assertEquals(SourceRotation.CW90, SourceRotation.NONE.next())
        assertEquals(SourceRotation.CW180, SourceRotation.CW90.next())
        assertEquals(SourceRotation.CW270, SourceRotation.CW180.next())
        assertEquals(SourceRotation.NONE, SourceRotation.CW270.next())
    }

    @Test
    fun then_composesModulo360() {
        assertEquals(SourceRotation.NONE, SourceRotation.CW90.then(SourceRotation.CW270))
        assertEquals(SourceRotation.NONE, SourceRotation.CW180.then(SourceRotation.CW180))
        assertEquals(SourceRotation.CW270, SourceRotation.CW90.then(SourceRotation.CW180))
        assertEquals(SourceRotation.CW90, SourceRotation.CW90.then(SourceRotation.NONE))
    }

    @Test
    fun fromDegrees_normalizesAnyMultipleOf90() {
        assertEquals(SourceRotation.NONE, SourceRotation.fromDegrees(0))
        assertEquals(SourceRotation.CW90, SourceRotation.fromDegrees(90))
        assertEquals(SourceRotation.CW180, SourceRotation.fromDegrees(180))
        assertEquals(SourceRotation.CW270, SourceRotation.fromDegrees(270))
        assertEquals(SourceRotation.NONE, SourceRotation.fromDegrees(360))
        assertEquals(SourceRotation.CW90, SourceRotation.fromDegrees(450))
        assertEquals(SourceRotation.CW270, SourceRotation.fromDegrees(-90))
        assertEquals(SourceRotation.CW270, SourceRotation.fromDegrees(-450))
    }

    // --- parallel rotation (perf-lab §16.6): the fast path must not move a byte ---

    /** Deterministic pseudo-random image, so a mis-split row is not masked by flat data. */
    private fun noiseImage(w: Int, h: Int): LinearImage {
        val b = ByteBuffer.allocateDirect(w * h * 3 * 4).order(ByteOrder.nativeOrder())
        val f = b.asFloatBuffer()
        var s = 12345
        for (i in 0 until w * h * 3) {
            s = s * 1103515245 + 12345
            f.put(i, ((s ushr 8) and 0xFFFF) / 65535f)
        }
        return LinearImage(b, w, h)
    }

    private fun floatsOf(img: LinearImage): FloatArray {
        return img.acquireDataLease().use { lease ->
            val data = lease.data
            val f = data.asFloatBuffer()
            val a = FloatArray(img.width * img.height * 3)
            f.get(a)
            a
        }
    }

    private fun ownedImage(plain: LinearImage, closes: AtomicInteger): LinearImage =
        plain.acquireDataLease().use { lease ->
            LinearImage.fromDataLease(
                lease.data,
                plain.width,
                plain.height,
                lease = AutoCloseable { closes.incrementAndGet() },
            )
        }

    /** The obvious per-pixel mapping this file used to do inline — the reference. */
    private fun naiveRotate(src: FloatArray, w: Int, h: Int, r: SourceRotation): FloatArray {
        val transposed = r == SourceRotation.CW90 || r == SourceRotation.CW270
        val nw = if (transposed) h else w
        val out = FloatArray(w * h * 3)
        for (y in 0 until h) for (x in 0 until w) {
            val nx: Int
            val ny: Int
            when (r) {
                SourceRotation.CW90 -> { nx = h - 1 - y; ny = x }
                SourceRotation.CW180 -> { nx = w - 1 - x; ny = h - 1 - y }
                SourceRotation.CW270 -> { nx = y; ny = w - 1 - x }
                SourceRotation.NONE -> { nx = x; ny = y }
            }
            val s = (y * w + x) * 3
            val d = (ny * nw + nx) * 3
            out[d] = src[s]; out[d + 1] = src[s + 1]; out[d + 2] = src[s + 2]
        }
        return out
    }

    private val quarterTurns =
        listOf(SourceRotation.CW90, SourceRotation.CW180, SourceRotation.CW270)

    @Test
    fun rotation_isWorkerCountInvariant() {
        // 300x250 = 75 000 px, above ROT_PARALLEL_MIN_PIXELS, so the default path is
        // parallel too. Height 250 is not divisible by 8, which is where a row-split
        // off-by-one would show.
        for (r in quarterTurns) {
            val one = floatsOf(noiseImage(300, 250).rotatedWithWorkers(r, 1))
            val many = floatsOf(noiseImage(300, 250).rotatedWithWorkers(r, 8))
            assertArrayEquals("$r: 1 worker vs 8 must be byte-identical", one, many, 0f)
        }
    }

    @Test
    fun rotation_matchesNaiveReference() {
        // Deliberately non-square with prime-ish dimensions divisible by no worker count.
        val w = 61
        val h = 43
        val src = floatsOf(noiseImage(w, h))
        for (r in quarterTurns) {
            for (workers in intArrayOf(1, 3, 8, 64)) {
                val got = floatsOf(noiseImage(w, h).rotatedWithWorkers(r, workers))
                assertArrayEquals("$r with $workers workers", naiveRotate(src, w, h, r), got, 0f)
            }
        }
    }

    @Test
    fun rotation_matchesReferenceAcrossTheTileBoundary() {
        // 90/270 now work a 64x64 tile at a time, so the interesting sizes are the ones
        // that straddle that edge: a partial tile in either axis, and exactly one tile.
        for (w in intArrayOf(63, 64, 65, 127, 129)) {
            for (h in intArrayOf(63, 64, 65)) {
                val src = floatsOf(noiseImage(w, h))
                for (r in quarterTurns) {
                    for (workers in intArrayOf(1, 5)) {
                        val got = floatsOf(noiseImage(w, h).rotatedWithWorkers(r, workers))
                        assertArrayEquals(
                            "$r ${w}x$h with $workers workers",
                            naiveRotate(src, w, h, r), got, 0f,
                        )
                    }
                }
            }
        }
    }

    @Test
    fun defaultWorkers_isOneForPreviewScaleImages() {
        // A preview render must not pay thread-spawn cost; a full-res export should.
        assertEquals(1, defaultRotWorkers(6L))
        assertEquals(1, defaultRotWorkers(63_999L))
        val cpus = Runtime.getRuntime().availableProcessors().coerceIn(1, 8)
        assertEquals(cpus, defaultRotWorkers(12_500_000L))
    }

    @Test
    fun rotationCancellation_isObservedBeforeWorkAndClosesConsumedInput() {
        val closes = AtomicInteger(0)
        val plain = coordImage()
        val owned = ownedImage(plain, closes)

        try {
            owned.rotatedWithWorkers(
                SourceRotation.CW90,
                workers = 1,
                isCancelled = { true },
            )
            fail("cancelled rotation must throw")
        } catch (_: CancellationException) {
            // Expected: cancellation is a stable control-flow result, not partial output.
        }

        assertEquals("consuming transform must release its input exactly once", 1, closes.get())
        owned.close()
        assertEquals("LinearImage close remains idempotent", 1, closes.get())
    }

    @Test
    fun rotationCancellation_isPolledDuringLongWork() {
        val closes = AtomicInteger(0)
        val checks = AtomicInteger(0)
        val plain = noiseImage(300, 250)
        val owned = ownedImage(plain, closes)

        try {
            owned.rotatedWithWorkers(
                SourceRotation.CW90,
                workers = 1,
                isCancelled = { checks.incrementAndGet() >= 3 },
            )
            fail("rotation must observe cancellation after work starts")
        } catch (_: CancellationException) {
            // Expected.
        }

        assertTrue("long work must poll more than once", checks.get() >= 3)
        assertEquals("cancelled consuming transform releases input", 1, closes.get())
    }

    @Test
    fun parallelWorkerCancellation_isPropagatedInsteadOfPublishingPartialOutput() {
        val caller = Thread.currentThread()
        val closes = AtomicInteger(0)
        val plain = noiseImage(300, 250)
        val owned = ownedImage(plain, closes)

        try {
            owned.rotatedWithWorkers(
                SourceRotation.CW90,
                workers = 2,
                isCancelled = { Thread.currentThread() !== caller },
            )
            fail("background-worker cancellation must invalidate the whole output")
        } catch (_: CancellationException) {
            // Expected: worker failures cross the join boundary as stable cancellation.
        }

        assertEquals("failed parallel transform releases input", 1, closes.get())
    }

    @Test
    fun rotationRejectsTruncatedLogicalWindowBeforeReading() {
        val closes = AtomicInteger(0)
        val truncated = ByteBuffer.allocateDirect(16).order(ByteOrder.nativeOrder()).apply {
            position(4)
            limit(12) // Two floats remain; one RGB pixel requires three.
        }
        val owned = LinearImage.fromDataLease(
            truncated,
            width = 1,
            height = 1,
            lease = AutoCloseable { closes.incrementAndGet() },
        )

        try {
            owned.rotatedWithWorkers(SourceRotation.CW180, workers = 1)
            fail("truncated logical buffer must be rejected")
        } catch (failure: IllegalArgumentException) {
            assertTrue(failure.message.orEmpty().contains("buffer", ignoreCase = true))
        }

        assertEquals("rejected consuming transform releases input", 1, closes.get())
    }

    @Test
    fun horizontalFlipCancellation_releasesConsumedInputWithoutOutput() {
        val closes = AtomicInteger(0)
        val plain = coordImage()
        val owned = ownedImage(plain, closes)

        try {
            owned.flippedHorizontal(isCancelled = { true })
            fail("cancelled horizontal flip must throw")
        } catch (_: CancellationException) {
            // Expected.
        }

        assertEquals("cancelled consuming flip releases input", 1, closes.get())
    }
}
