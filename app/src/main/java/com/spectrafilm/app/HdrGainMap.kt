/*
 * Spektrafilm for Android — spatial HDR gain map derived from the render (issue #140). GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Replaces the 1x1 uniform placeholder that made "Ultra HDR" an over-promise: it produced a valid
 * Ultra HDR container wrapping an ordinary SDR image with a flat global boost, carrying no
 * per-pixel information from the render at all.
 *
 * The HDR signal is already in the render and was simply being discarded. The engine emits
 * display-referred float RGB, and `packToArgb` clamps it with `min(1f, max(0f, v))` on the way to
 * 8-bit — so every value above SDR white is real, rendered detail that the SDR container cannot
 * hold. The gain map encodes exactly that, per pixel, and nothing else. Where the render never
 * exceeds white, the map is flat and the file honestly says there is no extra headroom.
 *
 * Encoding follows the Android Gainmap contract. For a stored gain value g in [0,1]:
 *
 *     HDR = (SDR + epsilonSdr) * 2^(lerp(log2(ratioMin), log2(ratioMax), g^(1/gamma))) - epsilonHdr
 *
 * so with ratioMin = 1 and gamma = 1 the inverse this file computes is
 *
 *     g = log2((hdr + epsilonHdr) / (sdr + epsilonSdr)) / log2(ratioMax)
 *
 * Two details matter for correctness. The ratio is computed in LINEAR light, decoding the
 * CCTF first, because a ratio of gamma-encoded values is not a ratio of luminances. And the
 * per-pixel gain is the maximum across channels rather than a luminance, so recovering a blown
 * highlight cannot leave one channel still clipped.
 */
package com.spectrafilm.app

import com.spectrafilm.engine.ColorSpace
import java.nio.FloatBuffer
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

internal object HdrGainMap {

    private const val LOG2 = 0.6931471805599453

    /**
     * The computed map plus what it says about the render.
     *
     * [ratioMax] is the headroom actually present, not a constant: the encoder needs it to write
     * the metadata, and it is the honest answer to "how much HDR is in this image". [gainedPixels]
     * and [maxRatio] exist so the pipeline can report whether a given render had any headroom at
     * all rather than leaving it to be assumed.
     */
    class Result(
        val alpha: ByteArray,
        val width: Int,
        val height: Int,
        val ratioMax: Float,
        val maxRatio: Float,
        val gainedPixels: Int,
    ) {
        /** True when the render genuinely exceeded SDR white somewhere. */
        val hasHeadroom: Boolean get() = ratioMax > 1.0001f
    }

    /**
     * Build the gain map for [rgb], interleaved display-referred float RGB of [width]x[height].
     *
     * Read through a FloatBuffer with absolute indexing rather than a float array: the export
     * buffer is ~150 MB at 12 MP and peak RSS already reaches 1.8 GB, so copying it to build a
     * gain map would be the single most expensive thing in the export. Absolute gets also leave
     * the buffer's position untouched, so the caller's lease is unaffected.
     *
     * [downsample] shrinks the map relative to the image; gain maps are low-frequency by nature
     * and full-resolution ones waste space for no visible benefit. [ratioMaxCeiling] caps how much
     * headroom may be claimed, so a few stray overshooting pixels cannot stretch the whole map's
     * scale and flatten the real detail.
     */
    fun compute(
        rgb: FloatBuffer,
        width: Int,
        height: Int,
        colorSpace: ColorSpace,
        cctfEncoded: Boolean,
        downsample: Int,
        ratioMaxCeiling: Float,
        epsilonSdr: Float,
        epsilonHdr: Float,
    ): Result {
        require(width > 0 && height > 0) { "gain map needs a non-empty image" }
        require(downsample >= 1) { "downsample must be >= 1" }
        val mapW = max(1, (width + downsample - 1) / downsample)
        val mapH = max(1, (height + downsample - 1) / downsample)

        // Pass 1: the headroom actually present. Encoding against a fixed ceiling instead would
        // waste most of the 8-bit range on an image that only just exceeds white.
        var maxRatio = 1f
        var gained = 0
        var i = 0
        val pixels = width * height
        while (i < pixels) {
            val ratio = pixelRatio(rgb, i * 3, colorSpace, cctfEncoded, epsilonSdr, epsilonHdr)
            if (ratio > 1.0001f) gained++
            if (ratio > maxRatio) maxRatio = ratio
            i++
        }
        val ratioMax = min(max(maxRatio, 1f), ratioMaxCeiling)

        val alpha = ByteArray(mapW * mapH)
        if (ratioMax <= 1.0001f) {
            // No headroom: a zero map is a correct, no-op gain map. The container stays valid and
            // truthfully claims nothing.
            return Result(alpha, mapW, mapH, 1f, maxRatio, gained)
        }

        val logRatioMax = ln(ratioMax.toDouble()) / LOG2
        for (my in 0 until mapH) {
            val y0 = my * downsample
            val y1 = min(y0 + downsample, height)
            for (mx in 0 until mapW) {
                val x0 = mx * downsample
                val x1 = min(x0 + downsample, width)
                // Average the gain over the block rather than point-sampling, so a single bright
                // pixel does not become a hard edge in the map.
                var sum = 0.0
                var count = 0
                for (y in y0 until y1) {
                    val row = y * width
                    for (x in x0 until x1) {
                        val ratio = pixelRatio(
                            rgb, (row + x) * 3, colorSpace, cctfEncoded, epsilonSdr, epsilonHdr,
                        )
                        val g = (ln(ratio.toDouble()) / LOG2) / logRatioMax
                        sum += if (g < 0.0) 0.0 else if (g > 1.0) 1.0 else g
                        count++
                    }
                }
                val g = if (count == 0) 0.0 else sum / count
                alpha[my * mapW + mx] = (g * 255.0).roundToInt().coerceIn(0, 255).toByte()
            }
        }
        return Result(alpha, mapW, mapH, ratioMax, maxRatio, gained)
    }

    /**
     * Linear-light HDR-to-SDR ratio for one pixel, taken as the largest of the three channels so
     * that restoring a highlight cannot leave a channel clipped behind.
     */
    private fun pixelRatio(
        rgb: FloatBuffer,
        offset: Int,
        colorSpace: ColorSpace,
        cctfEncoded: Boolean,
        epsilonSdr: Float,
        epsilonHdr: Float,
    ): Float {
        var worst = 1f
        for (channel in 0 until 3) {
            val encoded = rgb.get(offset + channel)
            if (encoded.isNaN()) continue
            if (encoded <= 1f) continue // at or below white: this channel needs no gain
            val hdrLinear = OutputCctf.decode(colorSpace, encoded, cctfEncoded)
            val sdrLinear = OutputCctf.decode(colorSpace, 1f, cctfEncoded)
            val ratio = (hdrLinear + epsilonHdr) / (sdrLinear + epsilonSdr)
            if (ratio > worst) worst = ratio
        }
        return worst
    }
}
