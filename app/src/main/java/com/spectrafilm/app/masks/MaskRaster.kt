/*
 * Spektrafilm for Android — mask rasterization. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Turn a normalized [Mask] into a per-pixel alpha buffer at a concrete resolution, so the compositor
 * ([MaskCompositor], live on the `simResultToBitmapGraded` output seam) can blend a Tier-A adjustment
 * by alpha. Pure Kotlin; no engine touched. Because the mask geometry is normalized, the SAME mask
 * rasterizes correctly for the draft, the zoom ROI and the full-res export — only the [w]×[h] differs.
 */
package com.spectrafilm.app.masks

object MaskRaster {

    /** Rasterize one row into reusable caller-owned scratch using the full-frame coordinate law. */
    internal fun rasterizeRow(mask: Mask, w: Int, h: Int, y: Int, out: FloatArray) {
        require(w > 0 && h > 0) { "mask raster dimensions must be positive" }
        require(y in 0 until h) { "mask raster row is outside the image" }
        require(out.size >= w) { "mask raster row scratch is undersized" }
        if (mask.components.isEmpty() && !mask.invert) {
            java.util.Arrays.fill(out, 0, w, 0f)
            return
        }
        val invW = 1f / w
        val invH = 1f / h
        val ny = (y + 0.5f) * invH
        var x = 0
        while (x < w) {
            out[x] = finiteAlpha(mask.alphaAt((x + 0.5f) * invW, ny))
            x++
        }
    }

    /**
     * Rasterize [mask] to a row-major [w]×[h] alpha buffer (values in [0,1]). Pixel centers map to
     * normalized coordinates `((x+0.5)/w, (y+0.5)/h)`. An empty (no-component) non-inverted mask is all
     * zero. The buffer is allocated fresh on every call (the compositor rasterizes per render); a
     * mask-hash + size cache to reuse it remains future work.
     */
    fun rasterize(mask: Mask, w: Int, h: Int): FloatArray {
        require(w > 0 && h > 0) { "mask raster dimensions must be positive" }
        val out = FloatArray(Math.multiplyExact(w, h))
        if (mask.components.isEmpty() && !mask.invert) return out  // selects nothing → all 0
        val invW = 1f / w
        val invH = 1f / h
        var i = 0
        var y = 0
        while (y < h) {
            val ny = (y + 0.5f) * invH
            var x = 0
            while (x < w) {
                val nx = (x + 0.5f) * invW
                out[i] = finiteAlpha(mask.alphaAt(nx, ny))
                x++; i++
            }
            y++
        }
        return out
    }

    /** Mean alpha of [mask] over a coarse [n]×[n] sample grid — a cheap "is anything selected?" probe. */
    fun coverage(mask: Mask, n: Int = 32): Float {
        require(n > 0) { "mask coverage grid must be positive" }
        var sum = 0f
        for (yy in 0 until n) {
            val ny = (yy + 0.5f) / n
            for (xx in 0 until n) {
                sum += mask.alphaAt((xx + 0.5f) / n, ny)
            }
        }
        return sum / (n * n)
    }

    /** Invalid imported geometry must not propagate NaN/Inf into the caller's RGB buffer. */
    private fun finiteAlpha(alpha: Float): Float = if (alpha.isFinite()) alpha.coerceIn(0f, 1f) else 0f
}
