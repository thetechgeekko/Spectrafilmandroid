/*
 * Spektrafilm for Android — source-image rotation.
 *
 * Rotation is applied to the decoded [LinearImage] BEFORE it is handed to the engine,
 * so both the live preview render and the full-resolution export reflect the same
 * orientation. The engine's [LinearImage.data] is a direct, native-order ByteBuffer of
 * interleaved RGB float32 in row-major order: floatIndex = (y * width + x) * 3 + c.
 */
package com.spectrafilm.app

import androidx.exifinterface.media.ExifInterface
import com.spectrafilm.engine.LinearImage
import com.spectrafilm.engine.SimResult
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

// Allocate the backing buffer for a rotated/flipped LinearImage of [floats] float32 elements.
// Above ~64 MB the buffer is allocated OFF the managed heap (native malloc) so a full-res
// export rotation of a large image (e.g. a 12 MP photo = 144 MB) doesn't OOM the ~256 MB ART
// heap — exactly the managed-buffer trap that hit the decode path. Small (preview-scale)
// buffers stay managed (GC handles them). Returns the buffer + the matching LinearImage
// onClose (null for managed). Falls back to managed if the native alloc fails.
private const val ROT_OFFHEAP_THRESHOLD_FLOATS = 16_000_000  // ~64 MB of float32

private fun allocRotBuf(floats: Int): Pair<ByteBuffer, ((ByteBuffer) -> Unit)?> {
    if (floats > ROT_OFFHEAP_THRESHOLD_FLOATS) {
        val nb = SimResult.allocDirectBuffer(floats.toLong() * 4)
        if (nb != null) {
            return nb.order(ByteOrder.nativeOrder()) to { b -> SimResult.freeDirectBuffer(b) }
        }
    }
    // Long-widen the byte count (floats * 4 overflows Int above ~536M floats) and fail
    // loudly rather than allocate a wrong-sized buffer, mirroring the off-heap branch above.
    val bytes = floats.toLong() * 4
    if (bytes > Int.MAX_VALUE) throw OutOfMemoryError("rotation buffer too large: $bytes bytes")
    return ByteBuffer.allocateDirect(bytes.toInt()).order(ByteOrder.nativeOrder()) to null
}

/** Clockwise rotation applied to the source before simulation. */
enum class SourceRotation(val degrees: Int) {
    NONE(0), CW90(90), CW180(180), CW270(270);

    /** Next 90-degree clockwise step. */
    fun next(): SourceRotation = when (this) {
        NONE -> CW90
        CW90 -> CW180
        CW180 -> CW270
        CW270 -> NONE
    }

    /** Compose two clockwise rotations (this THEN [other]); returns the net step. */
    fun then(other: SourceRotation): SourceRotation =
        fromDegrees(degrees + other.degrees)

    companion object {
        fun fromDegrees(deg: Int): SourceRotation = when (((deg % 360) + 360) % 360) {
            90 -> CW90
            180 -> CW180
            270 -> CW270
            else -> NONE
        }
    }
}

/**
 * The decoded-image geometry op derived from an EXIF Orientation tag: a clockwise
 * [rotation] plus an optional horizontal [flipH] mirror. Covers all 8 TIFF/EXIF
 * orientation values. EXIF is applied to the decoded [LinearImage] as a baseline,
 * BEFORE the user's manual [SourceRotation] steps, so imports appear upright.
 */
data class ExifOrientation(val rotation: SourceRotation, val flipH: Boolean) {
    val isIdentity: Boolean get() = rotation == SourceRotation.NONE && !flipH

    companion object {
        val NONE = ExifOrientation(SourceRotation.NONE, false)

        /** Map an [ExifInterface.TAG_ORIENTATION] value to a rotation (+ optional H flip). */
        fun fromExif(orientation: Int): ExifOrientation = when (orientation) {
            ExifInterface.ORIENTATION_NORMAL,
            ExifInterface.ORIENTATION_UNDEFINED -> NONE
            ExifInterface.ORIENTATION_FLIP_HORIZONTAL ->
                ExifOrientation(SourceRotation.NONE, true)
            ExifInterface.ORIENTATION_ROTATE_180 ->
                ExifOrientation(SourceRotation.CW180, false)
            ExifInterface.ORIENTATION_FLIP_VERTICAL ->
                ExifOrientation(SourceRotation.CW180, true)
            // Transpose: mirror across the main diagonal = rotate 90 CW then flip H.
            ExifInterface.ORIENTATION_TRANSPOSE ->
                ExifOrientation(SourceRotation.CW90, true)
            ExifInterface.ORIENTATION_ROTATE_90 ->
                ExifOrientation(SourceRotation.CW90, false)
            // Transverse: mirror across the anti-diagonal = rotate 270 CW then flip H.
            ExifInterface.ORIENTATION_TRANSVERSE ->
                ExifOrientation(SourceRotation.CW270, true)
            ExifInterface.ORIENTATION_ROTATE_270 ->
                ExifOrientation(SourceRotation.CW270, false)
            else -> NONE
        }
    }
}

/**
 * Apply an [ExifOrientation] baseline to this image. The horizontal flip is applied
 * FIRST (in the source pixel grid), then the clockwise rotation, matching the EXIF
 * decode convention where the stored orientation describes how to upright the pixels.
 */
fun LinearImage.applyExif(orientation: ExifOrientation): LinearImage {
    if (orientation.isIdentity) return this
    val flipped = if (orientation.flipH) this.flippedHorizontal() else this
    return flipped.rotated(orientation.rotation)
}

/** Return a new [LinearImage] mirrored left-to-right (horizontal flip). */
fun LinearImage.flippedHorizontal(): LinearImage {
    val ch = 3
    val w = width
    val h = height
    val src = data.duplicate().order(ByteOrder.nativeOrder()).asFloatBuffer()
    val (outBuf, onClose) = allocRotBuf(w * h * ch)
    val dst = outBuf.asFloatBuffer()
    for (y in 0 until h) {
        for (x in 0 until w) {
            val nx = w - 1 - x
            val s = (y * w + x) * ch
            val d = (y * w + nx) * ch
            dst.put(d, src.get(s))
            dst.put(d + 1, src.get(s + 1))
            dst.put(d + 2, src.get(s + 2))
        }
    }
    // This op allocated a fresh buffer, so the input is no longer needed. If the input owns an
    // off-heap native buffer (a full-res export decode) close() frees it; it is a no-op for the
    // common managed (allocateDirect) inputs.
    val flippedResult = LinearImage(outBuf, w, h, colorSpace, onClose = onClose)
    close()
    return flippedResult
}

// Below this many pixels a rotation is not worth spawning threads for; preview-scale
// images stay single-threaded.
private const val ROT_PARALLEL_MIN_PIXELS = 64_000L

// Transpose tile edge, in pixels. 64x64x3 float32 is ~49 KB, so a tile plus its
// destination run stays inside L2 while the runs are long enough (192 floats) for the
// bulk write to pay for itself.
private const val ROT_TILE = 64

internal fun defaultRotWorkers(pixels: Long): Int =
    if (pixels < ROT_PARALLEL_MIN_PIXELS) 1
    else Runtime.getRuntime().availableProcessors().coerceIn(1, 8)

/**
 * Return a new [LinearImage] rotated clockwise by [rotation]. [NONE] returns the input
 * unchanged (no copy). 90/270 swap width and height. Operates on the float view of the
 * native ByteBuffer.
 *
 * ## Why this is not the obvious per-pixel loop
 *
 * It used to be, and it cost **1155 ms** of a 12.5 MP export — a flat surcharge on ANY
 * non-zero rotation. The device measurement that found it noted that 180 degrees costs
 * exactly what 90 does even though 180 transposes nothing, which said the cost was the
 * loop SHAPE rather than the geometry: every branch did three `FloatBuffer.get()` and
 * three scattered indexed `put()` per pixel, single-threaded — ~75 M bounds-checked
 * buffer operations at 12.5 MP. See `docs/research/perf-lab.md` §16.6.
 *
 * So source rows are read in BULK into a plain FloatArray (sequential, unchecked), and
 * 180 — a pure reversal — writes its destination row in bulk too, with no scatter at
 * all. Only 90/270 still scatter, because their destination is a column.
 *
 * ## Worker-count invariance
 *
 * Work is split by SOURCE row. For every rotation a source row maps to a distinct
 * destination row (180) or column (90/270), so no two workers ever write the same output
 * element and the result is byte-identical for any worker count — the same contract
 * `kernels/parallel` holds natively. `RotationTest` asserts it 1-vs-8, exactly as the
 * engine parity suite does with `SPK_NUM_THREADS`.
 */
fun LinearImage.rotated(rotation: SourceRotation): LinearImage =
    rotatedWithWorkers(rotation, defaultRotWorkers(width.toLong() * height))

internal fun LinearImage.rotatedWithWorkers(
    rotation: SourceRotation,
    workers: Int,
): LinearImage {
    if (rotation == SourceRotation.NONE) return this
    val ch = 3
    val w = width
    val h = height
    val transposed = rotation == SourceRotation.CW90 || rotation == SourceRotation.CW270
    val nw = if (transposed) h else w
    val nh = if (transposed) w else h
    val (outBuf, onClose) = allocRotBuf(nw * nh * ch)
    val rowFloats = w * ch

    // 180: a source row becomes a destination row, reversed. Bulk in, reverse in a plain
    // array, bulk out — no scatter anywhere.
    fun reverseRows(src: FloatBuffer, dst: FloatBuffer, y0: Int, y1: Int) {
        val row = FloatArray(rowFloats)
        val out = FloatArray(rowFloats)
        for (y in y0 until y1) {
            src.position(y * rowFloats)
            src.get(row, 0, rowFloats)
            var x = 0
            while (x < w) {
                val s = x * ch
                val d = (w - 1 - x) * ch
                out[d] = row[s]; out[d + 1] = row[s + 1]; out[d + 2] = row[s + 2]
                x++
            }
            dst.position((h - 1 - y) * rowFloats)
            dst.put(out, 0, rowFloats)
        }
    }

    // 90/270: the destination is a COLUMN, so a row-at-a-time pass can only scatter. Work
    // a tile at a time instead — then within a tile each source column's slice lands on a
    // CONTIGUOUS destination column run, which bulk-writes. The transpose itself happens
    // in plain FloatArrays, so the inner loop has no bounds-checked buffer ops at all.
    fun transposeCols(src: FloatBuffer, dst: FloatBuffer, xStart: Int, xEnd: Int) {
        val tile = FloatArray(ROT_TILE * ROT_TILE * ch)
        val run = FloatArray(ROT_TILE * ch)
        var xb = xStart
        while (xb < xEnd) {
            val xw = minOf(ROT_TILE, xEnd - xb)
            var yb = 0
            while (yb < h) {
                val yh = minOf(ROT_TILE, h - yb)
                for (j in 0 until yh) {
                    src.position(((yb + j) * w + xb) * ch)
                    src.get(tile, j * xw * ch, xw * ch)
                }
                for (i in 0 until xw) {
                    val x = xb + i
                    val drow: Int
                    val dcol0: Int
                    if (rotation == SourceRotation.CW90) {
                        // (x,y) -> (h-1-y, x): destination row x, columns descending in y.
                        drow = x
                        dcol0 = h - (yb + yh)
                        for (j in 0 until yh) {
                            val k = (yh - 1 - j) * ch
                            val s = (j * xw + i) * ch
                            run[k] = tile[s]; run[k + 1] = tile[s + 1]; run[k + 2] = tile[s + 2]
                        }
                    } else {
                        // (x,y) -> (y, w-1-x): destination row w-1-x, columns ascending in y.
                        drow = w - 1 - x
                        dcol0 = yb
                        for (j in 0 until yh) {
                            val k = j * ch
                            val s = (j * xw + i) * ch
                            run[k] = tile[s]; run[k + 1] = tile[s + 1]; run[k + 2] = tile[s + 2]
                        }
                    }
                    dst.position((drow * nw + dcol0) * ch)
                    dst.put(run, 0, yh * ch)
                }
                yb += yh
            }
            xb += xw
        }
    }

    // Each worker takes its OWN buffer duplicates: a FloatBuffer's position is per-view,
    // so sharing one across threads would corrupt the bulk transfers above.
    fun work(a: Int, b: Int) {
        val src = data.duplicate().order(ByteOrder.nativeOrder()).asFloatBuffer()
        val dst = outBuf.duplicate().order(ByteOrder.nativeOrder()).asFloatBuffer()
        if (transposed) transposeCols(src, dst, a, b) else reverseRows(src, dst, a, b)
    }

    // Split 180 by source ROW and 90/270 by source COLUMN. In both cases the destination
    // ROW is then a function of the split variable alone, so every worker owns whole
    // destination rows — no shared element, and no false sharing either.
    val axis = if (transposed) w else h
    val n = workers.coerceIn(1, axis)
    if (n == 1) {
        work(0, axis)
    } else {
        val per = (axis + n - 1) / n
        val threads = ArrayList<Thread>(n - 1)
        var start = per
        while (start < axis) {
            val a = start
            val b = minOf(start + per, axis)
            threads += Thread { work(a, b) }.also { it.start() }
            start = b
        }
        work(0, minOf(per, axis))            // the caller takes the first chunk
        threads.forEach { it.join() }
    }

    val rotated = LinearImage(outBuf, nw, nh, colorSpace, onClose = onClose)
    close()
    return rotated
}
