/*
 * Spektrafilm for Android -- managed Coil preview conversion. GPLv3.
 */
package com.spectrafilm.libraw

import kotlin.math.pow

/** Copies a leased linear result into unmanaged ARGB_8888 pixels with bounded cancellation latency. */
internal fun rawCoilPreviewPixels(
    result: LinearResult,
    cancellation: RawDecodeCancellation,
): IntArray {
    val pixelCount = Math.multiplyExact(result.width, result.height)
    val requiredFloats = Math.multiplyExact(pixelCount, RGB_CHANNELS)
    val pixels = IntArray(pixelCount)
    val gamma = 1.0f / 2.2f

    cancellation.throwIfCancellationRequested()
    result.withDataLease { data ->
        val floats = data.asFloatBuffer()
        require(floats.remaining() >= requiredFloats) {
            "RAW preview buffer is truncated: ${floats.remaining()} < $requiredFloats floats"
        }
        var pixel = 0
        while (pixel < pixelCount) {
            if (pixel % CANCELLATION_POLL_PIXELS == 0) {
                cancellation.throwIfCancellationRequested()
            }
            val base = pixel * RGB_CHANNELS
            val red = encodePreviewChannel(floats.get(base), gamma)
            val green = encodePreviewChannel(floats.get(base + 1), gamma)
            val blue = encodePreviewChannel(floats.get(base + 2), gamma)
            pixels[pixel] = (0xFF shl 24) or (red shl 16) or (green shl 8) or blue
            pixel++
        }
    }
    cancellation.throwIfCancellationRequested()
    return pixels
}

private fun encodePreviewChannel(value: Float, gamma: Float): Int {
    val clamped = value.coerceIn(0f, 1f)
    return (clamped.pow(gamma) * 255f + 0.5f).toInt().coerceIn(0, 255)
}

private const val RGB_CHANNELS = 3
private const val CANCELLATION_POLL_PIXELS = 4 * 1024
