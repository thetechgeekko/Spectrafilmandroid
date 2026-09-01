/*
 * Spektrafilm for Android — point tone-curve editor (UI + math). GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * A Lightroom-style point tone curve drawn over the preview histogram. The engine stage
 * (kernels/tonecurve.cpp) is already wired through ParamsState.toneCurve* and the facade's
 * ToneCurveParams; this is the missing UI. The on-screen curve is sampled with the SAME
 * Fritsch–Carlson monotone-cubic interpolation the engine bakes into its LUT, so what you
 * draw is what renders. Editing a channel just replaces the ParamsState list — the editor's
 * derivedStateOf snapshot then drives the existing draft/settle render exactly like a slider.
 */
package com.spectrafilm.app

import android.graphics.Bitmap
import android.view.HapticFeedbackConstants
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.unit.dp
import com.spectrafilm.engine.TONE_CURVE_MAX_POINTS
import kotlinx.coroutines.CancellationException
import kotlin.math.abs
import kotlin.math.ln
import kotlin.math.round
import kotlin.math.sqrt

/**
 * Pure tone-curve math — the editable point model plus the on-screen sampler. No Android deps,
 * so it is unit-tested on the plain JVM (ToneCurveTest). The sampler is a faithful port of the
 * engine's `build_tone_curve_1d` (Fritsch–Carlson monotone cubic Hermite, flat extrapolation
 * past the end points, clamp to [0,1]); the point ops keep x strictly increasing and the end
 * points pinned to x = 0 / 1 (only their output level moves, Lightroom-style).
 */
object ToneCurveMath {

    /** Min x gap kept between adjacent control points (touch-comfortable; above the 1e-3 quant). */
    const val MIN_GAP = 0.012f

    /**
     * A channel with fewer than 2 points is identity; surface the implied `[(0,0),(1,1)]` end
     * points so the editor always has two draggable handles to start from.
     */
    fun effective(points: List<Pair<Float, Float>>): List<Pair<Float, Float>> =
        if (points.size < 2) listOf(0f to 0f, 1f to 1f) else points

    /** True when every control point lies on y = x — the engine treats this as a strict no-op. */
    fun isIdentity(points: List<Pair<Float, Float>>): Boolean =
        effective(points).all { (x, y) -> x == y }

    /** Quantise to 1e-3 and clamp to [0,1] so a drag can't emit sub-pixel param churn. */
    private fun q(v: Float): Float = (round(v * 1000f) / 1000f).coerceIn(0f, 1f)

    /**
     * Move the point at [index] to ([x],[y]). End points keep x pinned to 0 / 1; interior points
     * are clamped strictly between their neighbours. Returns a new list with x strictly increasing.
     */
    fun move(points: List<Pair<Float, Float>>, index: Int, x: Float, y: Float): List<Pair<Float, Float>> {
        val pts = effective(points).toMutableList()
        if (index !in pts.indices) return pts
        val newX = when (index) {
            0 -> 0f
            pts.lastIndex -> 1f
            else -> {
                val lo = pts[index - 1].first + MIN_GAP
                val hi = pts[index + 1].first - MIN_GAP
                if (lo > hi) pts[index].first else q(x).coerceIn(lo, hi)
            }
        }
        pts[index] = newX to q(y)
        return pts
    }

    /**
     * Insert a control point at ([x],[y]), keeping x strictly increasing. Taps on an end-point
     * column (x ≤ 0 or ≥ 1) or within [MIN_GAP] of an existing point are ignored; the channel is
     * capped at [TONE_CURVE_MAX_POINTS].
     */
    fun add(points: List<Pair<Float, Float>>, x: Float, y: Float): List<Pair<Float, Float>> {
        val pts = effective(points)
        if (pts.size >= TONE_CURVE_MAX_POINTS) return pts
        val nx = q(x)
        if (nx <= 0f || nx >= 1f) return pts
        if (pts.any { abs(it.first - nx) < MIN_GAP }) return pts
        return (pts + (nx to q(y))).sortedBy { it.first }
    }

    /** Remove an interior control point; an end-point [index] (0 or last) is a no-op. */
    fun remove(points: List<Pair<Float, Float>>, index: Int): List<Pair<Float, Float>> {
        val pts = effective(points)
        if (index <= 0 || index >= pts.lastIndex) return pts
        return pts.filterIndexed { i, _ -> i != index }
    }

    /**
     * Sample the curve at [n] evenly spaced x in [0,1] → y. Faithful port of the engine's
     * Fritsch–Carlson monotone-cubic LUT bake: secants, averaged tangents, monotonicity
     * projection back into the radius-3 circle, Hermite eval, flat extrapolation past the end
     * points, clamp to [0,1]. Returns the identity ramp for an identity curve.
     */
    fun sample(points: List<Pair<Float, Float>>, n: Int): FloatArray {
        val out = FloatArray(n)
        val denom = (n - 1).coerceAtLeast(1)
        if (isIdentity(points)) {
            for (k in 0 until n) out[k] = k.toFloat() / denom
            return out
        }
        val pts = effective(points)
        val cnt = pts.size
        val xs = FloatArray(cnt) { pts[it].first }
        val ys = FloatArray(cnt) { pts[it].second }
        val segs = cnt - 1
        val d = FloatArray(segs) { i ->
            val dx = xs[i + 1] - xs[i]
            if (dx > 0f) (ys[i + 1] - ys[i]) / dx else 0f
        }
        val m = FloatArray(cnt)
        m[0] = d[0]
        m[cnt - 1] = d[segs - 1]
        for (i in 1 until cnt - 1) m[i] = 0.5f * (d[i - 1] + d[i])
        for (i in 0 until segs) {
            if (d[i] == 0f) {
                m[i] = 0f
                m[i + 1] = 0f
            } else {
                val a = m[i] / d[i]
                val b = m[i + 1] / d[i]
                val s = a * a + b * b
                if (s > 9f) {
                    val t = 3f / sqrt(s)
                    m[i] = t * a * d[i]
                    m[i + 1] = t * b * d[i]
                }
            }
        }
        var seg = 0
        for (k in 0 until n) {
            val x = k.toFloat() / denom
            when {
                x <= xs[0] -> out[k] = ys[0].coerceIn(0f, 1f)
                x >= xs[cnt - 1] -> out[k] = ys[cnt - 1].coerceIn(0f, 1f)
                else -> {
                    while (seg < segs - 1 && x > xs[seg + 1]) seg++
                    val h = xs[seg + 1] - xs[seg]
                    val t = (x - xs[seg]) / h
                    val t2 = t * t
                    val t3 = t2 * t
                    val h00 = 2f * t3 - 3f * t2 + 1f
                    val h10 = t3 - 2f * t2 + t
                    val h01 = -2f * t3 + 3f * t2
                    val h11 = t3 - t2
                    val yv = h00 * ys[seg] + h10 * h * m[seg] + h01 * ys[seg + 1] + h11 * h * m[seg + 1]
                    out[k] = yv.coerceIn(0f, 1f)
                }
            }
        }
        return out
    }
}

/** Index of the control point nearest [pos] within [radiusPx], or -1 — screen-space hit-test. */
private fun nearestPointIndex(
    pts: List<Pair<Float, Float>>,
    pos: Offset,
    w: Float,
    h: Float,
    radiusPx: Float,
): Int {
    var best = -1
    var bestSq = radiusPx * radiusPx
    pts.forEachIndexed { i, (x, y) ->
        val dx = pos.x - x * w
        val dy = pos.y - (1f - y) * h
        val sq = dx * dx + dy * dy
        if (sq <= bestSq) {
            bestSq = sq
            best = i
        }
    }
    return best
}

/**
 * The interactive curve canvas: a square graph over a faint [histo] backdrop. Tap empty space to
 * add a point, drag a point to shape, double-tap an interior point to remove it. End points slide
 * only vertically. [points] is the raw channel list (an empty/short list draws as identity);
 * [onChange] receives the new list. The gesture readers see the latest [points]/[onChange] via
 * rememberUpdatedState, so the detectors are never restarted mid-drag.
 */
@Composable
fun ToneCurveEditor(
    points: List<Pair<Float, Float>>,
    histo: IntArray?,
    curveColor: Color,
    onChange: (List<Pair<Float, Float>>) -> Unit,
    modifier: Modifier = Modifier,
) {
    val view = LocalView.current
    val gridColor = Color.White.copy(alpha = 0.13f)
    val identityColor = Color.White.copy(alpha = 0.22f)
    val canvasBg = Color(0xFF101014)
    val latestPoints by rememberUpdatedState(points)
    val latestOnChange by rememberUpdatedState(onChange)
    var dragIndex by remember { mutableIntStateOf(-1) }

    Canvas(
        modifier
            .fillMaxWidth()
            .aspectRatio(1f)
            .clip(RoundedCornerShape(12.dp))
            .background(canvasBg)
            .border(1.dp, Color.White.copy(alpha = 0.14f), RoundedCornerShape(12.dp))
            .pointerInput(Unit) {
                val touchPx = 22.dp.toPx()
                detectTapGestures(
                    onTap = { p ->
                        val pts = ToneCurveMath.effective(latestPoints)
                        if (nearestPointIndex(pts, p, size.width.toFloat(), size.height.toFloat(), touchPx) < 0) {
                            latestOnChange(
                                ToneCurveMath.add(latestPoints, p.x / size.width, 1f - p.y / size.height)
                            )
                            view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                        }
                    },
                    onDoubleTap = { p ->
                        val pts = ToneCurveMath.effective(latestPoints)
                        val i = nearestPointIndex(pts, p, size.width.toFloat(), size.height.toFloat(), touchPx)
                        if (i > 0 && i < pts.lastIndex) {
                            latestOnChange(ToneCurveMath.remove(latestPoints, i))
                            view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                        }
                    },
                )
            }
            .pointerInput(Unit) {
                val touchPx = 22.dp.toPx()
                detectDragGestures(
                    onDragStart = { p ->
                        dragIndex = nearestPointIndex(
                            ToneCurveMath.effective(latestPoints),
                            p, size.width.toFloat(), size.height.toFloat(), touchPx,
                        )
                    },
                    onDragEnd = { dragIndex = -1 },
                    onDragCancel = { dragIndex = -1 },
                    onDrag = { change, _ ->
                        change.consume()
                        val i = dragIndex
                        if (i >= 0) {
                            latestOnChange(
                                ToneCurveMath.move(
                                    latestPoints, i,
                                    change.position.x / size.width,
                                    1f - change.position.y / size.height,
                                )
                            )
                        }
                    },
                )
            },
    ) {
        val w = size.width
        val h = size.height
        fun sx(x: Float) = x * w
        fun sy(y: Float) = (1f - y) * h

        // Quarter grid.
        for (i in 1..3) {
            val gx = w * i / 4f
            val gy = h * i / 4f
            drawLine(gridColor, Offset(gx, 0f), Offset(gx, h), strokeWidth = 1f)
            drawLine(gridColor, Offset(0f, gy), Offset(w, gy), strokeWidth = 1f)
        }

        // Faint log-scaled histogram backdrop in the channel colour.
        if (histo != null) {
            val peak = histo.maxOrNull() ?: 0
            if (peak > 0) {
                val logPeak = ln(1f + peak.toFloat())
                val binW = w / 256f
                val backdrop = curveColor.copy(alpha = 0.16f)
                for (i in 0 until 256) {
                    val v = histo[i]
                    if (v <= 0) continue
                    val barH = (ln(1f + v.toFloat()) / logPeak) * h
                    val x = i * binW
                    drawLine(backdrop, Offset(x, h), Offset(x, h - barH), strokeWidth = binW.coerceAtLeast(1f))
                }
            }
        }

        // Identity reference (dashed).
        drawLine(
            identityColor, Offset(0f, h), Offset(w, 0f), strokeWidth = 1f,
            pathEffect = PathEffect.dashPathEffect(floatArrayOf(8f, 8f)),
        )

        // The curve — sampled with the engine's interpolation.
        val n = 96
        val ys = ToneCurveMath.sample(points, n)
        val path = Path().apply {
            moveTo(0f, sy(ys[0]))
            for (k in 1 until n) lineTo(w * k / (n - 1), sy(ys[k]))
        }
        drawPath(
            path, curveColor,
            style = Stroke(width = 2.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round),
        )

        // Control-point handles (ringed, with a punch-out so the curve reads through).
        val r = 6.dp.toPx()
        ToneCurveMath.effective(points).forEachIndexed { i, (x, y) ->
            val c = Offset(sx(x), sy(y))
            drawCircle(canvasBg, radius = r, center = c)
            drawCircle(curveColor, radius = r, center = c, style = Stroke(width = 2.dp.toPx()))
            drawCircle(curveColor, radius = if (i == dragIndex) r * 0.7f else r * 0.45f, center = c)
        }
    }
}

/** The four editable channels, ordered Master / Red / Green / Blue. */
private val TONE_CHANNELS = listOf("Master", "Red", "Green", "Blue")

/**
 * The Tone Curve adjustment panel: a channel selector over the [ToneCurveEditor], a one-line
 * how-to, and per-channel / all resets. The [preview] bitmap feeds the histogram backdrop
 * (recomputed off the main thread when it changes). Auto-arms the stage on the first edit so the
 * effect shows without hunting for the switch; "Reset all" disarms it back to a strict no-op.
 */
@Composable
fun ToneCurveSection(s: ParamsState, preview: Bitmap?) {
    var expanded by remember { mutableStateOf(true) }
    var channel by remember { mutableIntStateOf(0) }
    // A new preview must not temporarily show the prior source/frame's bins. Sampling itself owns
    // the off-main dispatch and bitmap read lease in computeHistogram.
    var hist by remember(preview) { mutableStateOf<Histogram?>(null) }
    LaunchedEffect(preview) {
        hist = try {
            preview?.let { computeHistogram(it) }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Throwable) {
            Diag.w("tone-curve histogram unavailable: ${failure.message}")
            null
        }
    }

    val points = when (channel) {
        1 -> s.toneCurveRed
        2 -> s.toneCurveGreen
        3 -> s.toneCurveBlue
        else -> s.toneCurveMaster
    }
    val setPoints: (List<Pair<Float, Float>>) -> Unit = { p ->
        if (!s.toneCurveActive) s.toneCurveActive = true
        when (channel) {
            1 -> s.toneCurveRed = p
            2 -> s.toneCurveGreen = p
            3 -> s.toneCurveBlue = p
            else -> s.toneCurveMaster = p
        }
    }
    val histo: IntArray?
    val curveColor: Color
    when (channel) {
        1 -> { histo = hist?.r; curveColor = Color(0xFFFF5A5A) }
        2 -> { histo = hist?.g; curveColor = Color(0xFF5AD46A) }
        3 -> { histo = hist?.b; curveColor = Color(0xFF5A93FF) }
        else -> { histo = hist?.luma; curveColor = Color(0xFFE6E6E6) }
    }

    SectionCard(
        "Tone Curve", expanded, { expanded = it },
        enabledSwitch = s.toneCurveActive,
        onEnabledChange = { s.toneCurveActive = it },
    ) {
        // Contrast: a hue-neutral S-curve composed into the master curve below. Auto-arms the stage
        // so the effect shows; "Reset all" clears it. Negative = mute the punchy look (forum #3).
        EnhancedSlider(
            label = "Contrast",
            value = s.contrast,
            range = -100f..100f,
            onValueChange = {
                s.contrast = it
                if (it != 0f && !s.toneCurveActive) s.toneCurveActive = true
            },
            decimals = 0,
            default = 0f,
            tooltip = "Hue-neutral contrast via the master tone curve. " +
                "Negative softens a too-punchy look; positive adds punch. Composes with a drawn curve.",
        )
        SubTabRow(TONE_CHANNELS, channel, { channel = it })
        ToneCurveEditor(points = points, histo = histo, curveColor = curveColor, onChange = setPoints)
        Text(
            "Tap to add a point · drag to shape · double-tap a point to remove.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            TextButton(onClick = {
                when (channel) {
                    1 -> s.toneCurveRed = emptyList()
                    2 -> s.toneCurveGreen = emptyList()
                    3 -> s.toneCurveBlue = emptyList()
                    else -> s.toneCurveMaster = emptyList()
                }
            }) { Text("Reset channel") }
            TextButton(onClick = {
                s.toneCurveMaster = emptyList()
                s.toneCurveRed = emptyList()
                s.toneCurveGreen = emptyList()
                s.toneCurveBlue = emptyList()
                s.contrast = 0f
                s.toneCurveActive = false
            }) { Text("Reset all") }
        }
    }
}
