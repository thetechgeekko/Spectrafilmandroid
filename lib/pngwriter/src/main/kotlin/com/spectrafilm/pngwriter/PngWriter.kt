/*
 * Spektrafilm for Android — lib:pngwriter Kotlin facade.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 *
 * Writes a 16-bit-per-channel RGB PNG (bit_depth=16, color_type=2, filter=None,
 * zlib-deflated IDAT) with an optional embedded iCCP chunk and tEXt Software
 * tag. The native writer (libsfpng.so) depends only on the system zlib; see
 * png_writer.cpp for the full spec-compliance notes.
 *
 * Pixel input is 16-bit RGB, interleaved R,G,B, row-major, width*height*3
 * samples in the engine's native little-endian byte order. The writer byte-
 * swaps to big-endian before deflating, as required by the PNG spec (RFC 2083
 * §2.3). Sample values in the output range from 0 (black) to 65535 (white).
 *
 * NOTE: app/UI wiring (output Uri, threading, color-space/ICC selection) is a
 * later wave; this facade is the stable callable surface for it.
 */
package com.spectrafilm.pngwriter

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CancellationException
import java.util.concurrent.atomic.AtomicBoolean

private const val PACK_COPY_CHUNK_BYTES = 64 * 1024

class PngCancellationToken {
    private val state = AtomicBoolean(false)

    val isCancelled: Boolean
        get() = state.get()

    fun cancel() {
        state.set(true)
    }

    internal val nativeSignal: AtomicBoolean
        get() = state

    internal fun throwIfCancelled() {
        if (isCancelled) throw CancellationException("PNG write cancelled")
    }
}

internal fun checkedPngByteCount(
    width: Int,
    height: Int,
    bytesPerSample: Int,
): Int {
    require(width > 0 && height > 0) { "PNG dimensions must be positive" }
    require(bytesPerSample > 0) { "PNG bytes per sample must be positive" }
    val total = try {
        val rowSamples = Math.multiplyExact(width.toLong(), 3L)
        val rowBytes = Math.multiplyExact(rowSamples, bytesPerSample.toLong())
        Math.multiplyExact(rowBytes, height.toLong())
    } catch (_: ArithmeticException) {
        throw IllegalArgumentException("PNG pixel byte count overflow")
    }
    require(total <= Int.MAX_VALUE.toLong()) {
        "PNG pixel byte count exceeds ByteBuffer limits"
    }
    return total.toInt()
}

internal fun checkedPngOutputPath(outPath: String) {
    require(outPath.isNotEmpty()) { "PNG output path must not be empty" }
    require('\u0000' !in outPath) { "PNG output path must not contain NUL" }
}

internal fun packedPngBuffer(
    source: ByteBuffer,
    width: Int,
    height: Int,
    bytesPerSample: Int,
    cancellation: PngCancellationToken? = null,
): ByteBuffer {
    val requiredBytes = checkedPngByteCount(width, height, bytesPerSample)
    require(source.remaining() >= requiredBytes) {
        "pixel buffer too small: need $requiredBytes bytes, have ${source.remaining()}"
    }
    val selected = source.duplicate().apply {
        limit(position() + requiredBytes)
    }
    cancellation?.throwIfCancelled()
    if (selected.isDirect) {
        require(selected.position() % bytesPerSample == 0) {
            "direct pixel buffer position must be $bytesPerSample-byte aligned"
        }
        return selected.slice().order(ByteOrder.LITTLE_ENDIAN)
    }
    val packed = ByteBuffer.allocateDirect(requiredBytes).order(ByteOrder.LITTLE_ENDIAN)
    while (selected.hasRemaining()) {
        cancellation?.throwIfCancelled()
        val previousLimit = selected.limit()
        selected.limit(selected.position() + minOf(PACK_COPY_CHUNK_BYTES, selected.remaining()))
        packed.put(selected)
        selected.limit(previousLimit)
    }
    cancellation?.throwIfCancelled()
    return packed.apply { flip() }
}

object PngWriter {

    private object NativeLibrary {
        init {
            System.loadLibrary("sfpng")
        }

        fun ensureLoaded() = Unit
    }

    /**
     * Write a 16-bit RGB PNG from a direct [ByteBuffer] of little-endian uint16
     * samples (length = width*height*3*2 bytes). Fastest path: no per-pixel copy.
     *
     * The engine's display-referred float output should be quantised to uint16
     * ([0,1] → [0,65535], round-to-nearest) before calling. The writer byte-swaps
     * to big-endian internally; the caller never needs to think about byte order.
     *
     * @param rgb16     direct ByteBuffer, width*height*3 little-endian uint16 samples
     * @param width     image width in pixels
     * @param height    image height in pixels
     * @param outPath   absolute filesystem path to write
     * @param icc       optional raw ICC profile bytes (null/empty => no iCCP chunk)
     * @param software  producer string written as tEXt "Software" tag; empty => omit
     * @return number of bytes written
     * @throws IllegalStateException on any write failure
     */
    fun write(
        rgb16: ByteBuffer,
        width: Int,
        height: Int,
        outPath: String,
        icc: ByteArray? = null,
        software: String = "Spektrafilm",
        cancellation: PngCancellationToken? = null,
    ): Long {
        checkedPngOutputPath(outPath)
        val direct = packedPngBuffer(
            rgb16, width, height, bytesPerSample = 2, cancellation = cancellation,
        )
        NativeLibrary.ensureLoaded()
        return nativeWriteBuffer(
            direct, width, height, software, icc, outPath,
            cancellation?.nativeSignal,
        )
    }

    /**
     * Write a 16-bit RGB PNG from a [ShortArray] of width*height*3 samples
     * (interpreted as unsigned 16-bit, little-endian). Convenience overload for
     * callers that already have a short[].
     */
    fun write(
        rgb16: ShortArray,
        width: Int,
        height: Int,
        outPath: String,
        icc: ByteArray? = null,
        software: String = "Spektrafilm",
        cancellation: PngCancellationToken? = null,
    ): Long {
        checkedPngOutputPath(outPath)
        val requiredSamples = checkedPngByteCount(width, height, bytesPerSample = 2) / 2
        require(rgb16.size >= requiredSamples) {
            "short buffer too small: need $requiredSamples samples, have ${rgb16.size}"
        }
        cancellation?.throwIfCancelled()
        NativeLibrary.ensureLoaded()
        return nativeWriteShorts(
            rgb16, width, height, software, icc, outPath,
            cancellation?.nativeSignal,
        )
    }

    /**
     * Write a 16-bit RGB PNG from a float RGB buffer quantised to uint16.
     *
     * `rgbFloat` is width*height*3 float samples in [0,1] (values outside are
     * clamped). Quantisation is round-to-nearest over [0,65535]. Convenience
     * overload that matches the TIFF writer's `writeTiffFloatToFile` signature
     * so the two can be used interchangeably from the export layer.
     *
     * This overload builds a temporary uint16 buffer on the JVM and delegates to
     * [write]; for large images callers may prefer to quantise in the engine and
     * pass a pre-built uint16 buffer to avoid the extra allocation.
     */
    fun writeFloat(
        rgbFloat: FloatArray,
        width: Int,
        height: Int,
        outPath: String,
        icc: ByteArray? = null,
        software: String = "Spektrafilm",
        cancellation: PngCancellationToken? = null,
    ): Long {
        checkedPngOutputPath(outPath)
        val requiredBytes = checkedPngByteCount(width, height, bytesPerSample = 2)
        val requiredSamples = requiredBytes / 2
        require(rgbFloat.size >= requiredSamples) {
            "float buffer too small: need $requiredSamples samples, have ${rgbFloat.size}"
        }
        cancellation?.throwIfCancelled()
        val buf = ByteBuffer.allocateDirect(requiredBytes)
            .order(ByteOrder.LITTLE_ENDIAN)
        val sBuf = buf.asShortBuffer()
        val rowSamples = width * 3
        for (y in 0 until height) {
            cancellation?.throwIfCancelled()
            val rowStart = y * rowSamples
            for (x in 0 until rowSamples) {
                if (x % (PACK_COPY_CHUNK_BYTES / Float.SIZE_BYTES) == 0) {
                    cancellation?.throwIfCancelled()
                }
                val v = rgbFloat[rowStart + x].coerceIn(0f, 1f)
                sBuf.put((v * 65535f + 0.5f).toInt().toShort())
            }
        }
        buf.rewind()
        return write(buf, width, height, outPath, icc, software, cancellation)
    }

    // --- native bridge (png_writer_jni.cpp / libsfpng.so) ---
    private external fun nativeWriteBuffer(
        rgb16: ByteBuffer,
        width: Int,
        height: Int,
        software: String,
        icc: ByteArray?,
        outPath: String,
        cancellationSignal: AtomicBoolean?,
    ): Long

    private external fun nativeWriteShorts(
        rgb16: ShortArray,
        width: Int,
        height: Int,
        software: String,
        icc: ByteArray?,
        outPath: String,
        cancellationSignal: AtomicBoolean?,
    ): Long
}
