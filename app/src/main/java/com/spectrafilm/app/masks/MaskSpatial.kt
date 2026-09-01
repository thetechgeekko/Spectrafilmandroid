/*
 * Spektrafilm for Android — spatial (Class-S) mask primitives. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * The neighborhood pass the edge-aware local adjustments need (Lightroom's Clarity / Sharpness /
 * Texture / Highlights / Shadows are spatial — a per-pixel curve can't produce them). Pure Kotlin,
 * JVM-testable, applied on the engine OUTPUT luma in the Tier-2 compositor — ZERO engine/parity impact.
 *
 * Primitive: a separable 3-pass box blur (≈ Gaussian, O(n) regardless of radius, edge-clamped) of a
 * single-channel buffer. Unsharp-mask ops then add back a weighted (luma − blur) detail at different
 * radii: small = Sharpness (high-freq), mid = Texture, large + midtone-weighted = Clarity (local
 * contrast). Radii scale with the image's long edge so the look is resolution-independent (one mask
 * drives the 640px draft and the full-res export identically).
 */
package com.spectrafilm.app.masks

object MaskSpatial {

    internal fun effectiveRadius(w: Int, h: Int, radiusPx: Float): Int {
        if (w < 2 || h < 2) return 0
        val radius = radiusPx.toInt()
        return if (radius < 1) 0 else radius
    }

    /**
     * Separable box blur (3 passes ≈ Gaussian) of single-channel [src] (length ≥ [w]*[h]), with an
     * integer radius derived from [radiusPx]. Edge-clamped; returns a NEW array (input untouched).
     * radius < 1 → a copy (no blur).
     */
    fun blur(src: FloatArray, w: Int, h: Int, radiusPx: Float): FloatArray {
        val r = radiusPx.toInt()
        if (r < 1 || w < 2 || h < 2 || src.size < w * h) return src.copyOf()
        var cur = src.copyOf()
        val tmp = FloatArray(w * h)
        repeat(3) {
            boxH(cur, tmp, w, h, r)   // cur -> tmp (horizontal)
            boxV(tmp, cur, w, h, r)   // tmp -> cur (vertical)
        }
        return cur
    }

    private fun boxH(src: FloatArray, dst: FloatArray, w: Int, h: Int, r: Int) {
        val norm = 1f / (2 * r + 1)
        for (y in 0 until h) {
            val row = y * w
            var sum = 0f
            for (k in -r..r) sum += src[row + k.coerceIn(0, w - 1)]
            dst[row] = sum * norm
            for (x in 1 until w) {
                sum += src[row + (x + r).coerceIn(0, w - 1)] - src[row + (x - r - 1).coerceIn(0, w - 1)]
                dst[row + x] = sum * norm
            }
        }
    }

    private fun boxV(src: FloatArray, dst: FloatArray, w: Int, h: Int, r: Int) {
        val norm = 1f / (2 * r + 1)
        for (x in 0 until w) {
            var sum = 0f
            for (k in -r..r) sum += src[k.coerceIn(0, h - 1) * w + x]
            dst[x] = sum * norm
            for (y in 1 until h) {
                sum += src[(y + r).coerceIn(0, h - 1) * w + x] - src[(y - r - 1).coerceIn(0, h - 1) * w + x]
                dst[y * w + x] = sum * norm
            }
        }
    }

    /**
     * Midtone weight in [0,1]: 1 at luma=0.5, falling to 0 at black/white. Lets Clarity boost local
     * contrast in the midtones while sparing the extremes (the standard way to avoid halo/clipping).
     */
    fun midtoneWeight(luma: Float): Float {
        val x = 2f * luma.coerceIn(0f, 1f) - 1f   // -1..1, 0 at midtone
        val a = 1f - x * x                          // 1 at mid, 0 at the extremes
        return a * a                                // sharpen the peak
    }

    // Blur radii as a fraction of the image long edge (resolution-independent look). [RECON] tunables.
    const val RADIUS_FRAC_SHARP = 0.0015f
    const val RADIUS_FRAC_TEXTURE = 0.006f
    const val RADIUS_FRAC_CLARITY = 0.03f
    const val RADIUS_FRAC_REGION = 0.05f
}

/**
 * One exact, row-streamed instance of the legacy `H→V` three-box blur. Horizontal rows retain the
 * legacy left-to-right rolling sum; each vertical column retains the legacy top-to-bottom recurrence.
 * Interleaving independent rows/columns changes scheduling, not any floating-point operation order.
 */
internal class MaskTripleBoxStream(
    private val width: Int,
    private val height: Int,
    val radius: Int,
    private val scratch: MaskPipelineScratch,
    private val cancellation: MaskCancellationSignal = MaskCancellationSignal.NEVER,
) {
    private val vertical = Array(3) { index ->
        VerticalBoxStream(width, height, radius, scratch.rings[index], scratch.sums[index])
    }
    private var originalRows = 0

    init {
        require(radius > 0) { "streamed blur radius must be positive" }
        require(scratch.radius == radius) { "streamed blur scratch radius mismatch" }
    }

    fun reset() {
        originalRows = 0
        vertical.forEach { it.reset() }
    }

    /** Feed one original luma row. Returns the final blur row index emitted by this feed, or -1. */
    fun feedOriginal(source: FloatArray): Int {
        check(originalRows < height) { "too many source rows supplied to streamed blur" }
        boxHorizontal(source, scratch.rows[0])
        cancellation.checkpoint(MaskCancellationPoint.HORIZONTAL_PASS_1)
        originalRows++
        return cascadeFromFirst(scratch.rows[0])
    }

    /**
     * Advance one bottom-edge-clamped flush tick. At most one final row is emitted. Call until
     * [complete]. Running every active radius once per tick preserves their bounded relative delay.
     */
    fun advanceBottom(): Int {
        check(originalRows == height) { "streamed blur cannot flush before all source rows" }
        if (complete) return -1
        return when {
            vertical[0].needsBottomInput -> cascadeFromFirst(scratch.rows[0])
            vertical[1].needsBottomInput -> cascadeFromSecond(scratch.rows[2])
            else -> cascadeFromThird(scratch.rows[4])
        }
    }

    val complete: Boolean get() = vertical[2].outputRows == height

    fun outputAt(row: Int, x: Int): Float {
        require(row in 0 until height && x in 0 until width)
        val slot = row % scratch.delayRows
        return scratch.outputDelay[slot * width + x]
    }

    private fun cascadeFromFirst(input: FloatArray): Int {
        val emitted = vertical[0].push(input, scratch.rows[1])
        cancellation.checkpoint(MaskCancellationPoint.VERTICAL_PASS_1)
        if (!emitted) return -1
        boxHorizontal(scratch.rows[1], scratch.rows[2])
        cancellation.checkpoint(MaskCancellationPoint.HORIZONTAL_PASS_2)
        return cascadeFromSecond(scratch.rows[2])
    }

    private fun cascadeFromSecond(input: FloatArray): Int {
        val emitted = vertical[1].push(input, scratch.rows[3])
        cancellation.checkpoint(MaskCancellationPoint.VERTICAL_PASS_2)
        if (!emitted) return -1
        boxHorizontal(scratch.rows[3], scratch.rows[4])
        cancellation.checkpoint(MaskCancellationPoint.HORIZONTAL_PASS_3)
        return cascadeFromThird(scratch.rows[4])
    }

    private fun cascadeFromThird(input: FloatArray): Int {
        val emitted = vertical[2].push(input, scratch.rows[5])
        cancellation.checkpoint(MaskCancellationPoint.VERTICAL_PASS_3)
        if (!emitted) return -1
        val row = vertical[2].outputRows - 1
        val slot = row % scratch.delayRows
        scratch.rows[5].copyInto(scratch.outputDelay, slot * width, 0, width)
        return row
    }

    private fun boxHorizontal(source: FloatArray, destination: FloatArray) {
        val norm = 1f / (2 * radius + 1)
        var sum = 0f
        for (k in -radius..radius) sum += source[k.coerceIn(0, width - 1)]
        destination[0] = sum * norm
        var x = 1
        while (x < width) {
            sum += source[(x + radius).coerceIn(0, width - 1)] -
                source[(x - radius - 1).coerceIn(0, width - 1)]
            destination[x] = sum * norm
            x++
        }
    }
}

/** A single vertical box pass with the legacy recurrence and an edge-clamped streaming input. */
private class VerticalBoxStream(
    private val width: Int,
    private val height: Int,
    private val radius: Int,
    private val ring: FloatArray,
    private val sums: FloatArray,
) {
    private val ringRows = ring.size / width
    private val norm = 1f / (2 * radius + 1)
    private var logicalInputs = 0
    var outputRows: Int = 0
        private set

    val needsBottomInput: Boolean get() = logicalInputs < height + radius

    init {
        require(ringRows == minOf(height, 2 * radius + 1)) { "vertical ring extent mismatch" }
        require(sums.size >= width) { "vertical sum scratch is undersized" }
    }

    fun reset() {
        logicalInputs = 0
        outputRows = 0
    }

    /** Push one actual row or one repeated bottom row. Returns true when [out] receives a row. */
    fun push(input: FloatArray, out: FloatArray): Boolean {
        check(logicalInputs < height + radius) { "too many rows supplied to vertical box pass" }
        val logical = logicalInputs++
        if (logical < radius) {
            storeActual(logical, input)
            return false
        }
        if (logical == radius) {
            storeActual(logical, input)
            var x = 0
            while (x < width) {
                var sum = 0f
                for (k in -radius..radius) sum += sourceAt(k.coerceIn(0, height - 1), x)
                sums[x] = sum
                out[x] = sum * norm
                x++
            }
            outputRows = 1
            return true
        }

        val y = logical - radius
        val subtractRow = (y - radius - 1).coerceAtLeast(0)
        var x = 0
        while (x < width) {
            // Parentheses pin the legacy `sum += add - subtract` accumulation order.
            val sum = sums[x] + (input[x] - sourceAt(subtractRow, x))
            sums[x] = sum
            out[x] = sum * norm
            x++
        }
        storeActual(logical, input)
        outputRows++
        return true
    }

    private fun storeActual(logical: Int, input: FloatArray) {
        if (logical >= height) return // virtual rows are the already-retained last image row
        input.copyInto(ring, (logical % ringRows) * width, 0, width)
    }

    private fun sourceAt(row: Int, x: Int): Float = ring[(row % ringRows) * width + x]
}
