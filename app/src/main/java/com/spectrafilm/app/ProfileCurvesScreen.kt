/*
 * Spektrafilm for Android — profile curve browser. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Plots, for a selected film or print profile, its:
 *   - Spectral sensitivities  (log_sensitivity  81×3, per R/G/B layer, vs wavelength 380–780 nm)
 *   - Density / H-D curves    (density_curves  256×3, density vs log-exposure)
 *   - Dye density spectra     (channel_density  81×3, per dye layer, vs wavelength)
 *
 * All data is read from the bundled assets: spektra/profiles/<id>.json.
 * Charts are drawn entirely on Compose Canvas — no external charting library.
 *
 * Entry point: ProfileCurvesScreen(profileId, profileName, onBack).
 */
package com.spectrafilm.app

import android.content.Context
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.*
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

/** Parsed curve data from one profile JSON. Null fields = not present / all-null. */
data class ProfileCurveData(
    /** Wavelengths in nm (typically 380–780 in 5 nm steps). */
    val wavelengths: List<Float>,
    /** log-sensitivity per wavelength, 3 channels (R, G, B).  Null entries skipped. */
    val logSensitivity: List<FloatArray>?,   // outer = wavelength index, inner = [R, G, B]
    /** log-exposure axis (256 points). */
    val logExposure: List<Float>?,
    /** Density values per log-exposure, 3 channels.  Null entries skipped. */
    val densityCurves: List<FloatArray>?,     // outer = exposure index, inner = [R, G, B]
    /** Dye/channel spectral density per wavelength, 3 channels. */
    val channelDensity: List<FloatArray>?,    // outer = wavelength index, inner = [R, G, B]
    /** Profile display name from JSON info.name (may be empty). */
    val profileName: String,
    /** Profile type ("negative", "reversal", …) */
    val profileType: String,
)

// ---------------------------------------------------------------------------
// JSON loading (runs on IO thread)
// ---------------------------------------------------------------------------

private fun loadProfileCurves(ctx: Context, profileId: String): ProfileCurveData? {
    val assetPath = "spektra/profiles/$profileId.json"
    val text = runCatching {
        ctx.assets.open(assetPath).use { it.readBytes().decodeToString() }
    }.getOrNull() ?: return null

    val root = runCatching { JSONObject(text) }.getOrNull() ?: return null
    val info = root.optJSONObject("info") ?: JSONObject()
    val data = root.optJSONObject("data") ?: return null

    fun jsonArrayToFloatList(arr: JSONArray?): List<Float>? {
        arr ?: return null
        val out = ArrayList<Float>(arr.length())
        for (i in 0 until arr.length()) {
            val v = arr.opt(i) ?: return null
            out.add(if (v is Number) v.toFloat() else return null)
        }
        return out
    }

    /** Read a 2D JSON array as List<FloatArray?>, where inner = one row; nulls within rows
     *  are replaced with Float.NaN so callers can skip them. */
    fun jsonArrayTo2D(arr: JSONArray?): List<FloatArray>? {
        arr ?: return null
        val out = ArrayList<FloatArray>(arr.length())
        for (i in 0 until arr.length()) {
            val row = arr.optJSONArray(i) ?: return null
            val fa = FloatArray(row.length())
            for (j in 0 until row.length()) {
                fa[j] = if (row.isNull(j)) Float.NaN else (row.opt(j) as? Number)?.toFloat() ?: Float.NaN
            }
            out.add(fa)
        }
        return out
    }

    val wavelengths = jsonArrayToFloatList(data.optJSONArray("wavelengths")) ?: return null
    val logSensitivity = jsonArrayTo2D(data.optJSONArray("log_sensitivity"))
    val logExposure = jsonArrayToFloatList(data.optJSONArray("log_exposure"))
    val densityCurves = jsonArrayTo2D(data.optJSONArray("density_curves"))
    val channelDensity = jsonArrayTo2D(data.optJSONArray("channel_density"))

    return ProfileCurveData(
        wavelengths = wavelengths,
        logSensitivity = logSensitivity,
        logExposure = logExposure,
        densityCurves = densityCurves,
        channelDensity = channelDensity,
        profileName = info.optString("name", profileId),
        profileType = info.optString("type", ""),
    )
}

// ---------------------------------------------------------------------------
// Top-level screen composable
// ---------------------------------------------------------------------------

/**
 * Profile-curve browser screen. Shows spectral sensitivities, density/H-D curves, and
 * dye density spectra for [profileId]. Call from a NavScaffold via [onBack].
 *
 * @param profileId   engine profile id ("kodak_portra_400", etc.)
 * @param displayName friendly name shown in the title (from StockCatalog)
 * @param onBack      close / navigate back
 */
@Composable
fun ProfileCurvesScreen(
    profileId: String,
    displayName: String,
    onBack: () -> Unit,
) {
    val ctx = LocalContext.current.applicationContext
    var curveData by remember(profileId) { mutableStateOf<ProfileCurveData?>(null) }
    var loading by remember(profileId) { mutableStateOf(true) }
    var error by remember(profileId) { mutableStateOf<String?>(null) }

    LaunchedEffect(profileId) {
        loading = true
        error = null
        curveData = null
        val result = withContext(Dispatchers.IO) {
            runCatching { loadProfileCurves(ctx, profileId) }
        }
        result.onSuccess { curveData = it }
            .onFailure { error = it.message }
        loading = false
    }

    Column(Modifier.fillMaxSize()) {
        // --- Top bar ---
        @OptIn(ExperimentalMaterial3Api::class)
        TopAppBar(
            title = {
                Text(
                    stringResource(R.string.screen_curves_title, displayName),
                    modifier = Modifier.semantics { heading() },
                )
            },
            navigationIcon = {
                TextButton(onClick = onBack) { Text(stringResource(R.string.screen_back)) }
            },
        )

        Box(Modifier.weight(1f)) {
            // Status shown by a spinner / colour is also announced as text.
            val statusModifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite }
            when {
                loading -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    val loadingDesc = stringResource(R.string.screen_curves_loading)
                    CircularProgressIndicator(
                        modifier = statusModifier.semantics { contentDescription = loadingDesc },
                    )
                }
                error != null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(stringResource(R.string.screen_curves_error, error ?: ""),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.error,
                        modifier = statusModifier)
                }
                curveData == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(stringResource(R.string.screen_curves_not_found, profileId),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = statusModifier)
                }
                else -> curveData?.let { CurveContent(data = it, displayName = displayName) }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Scrollable content with three chart cards
// ---------------------------------------------------------------------------

@Composable
private fun CurveContent(data: ProfileCurveData, displayName: String) {
    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        // Meta chip
        if (data.profileType.isNotBlank()) {
            Surface(
                shape = RoundedCornerShape(8.dp),
                color = MaterialTheme.colorScheme.secondaryContainer,
            ) {
                Text(
                    data.profileType,
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSecondaryContainer,
                    modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
                )
            }
        }

        // --- Chart 1: Spectral Sensitivity ---
        if (data.logSensitivity != null) {
            val title = stringResource(R.string.screen_curves_sensitivity_title)
            val subtitle = stringResource(R.string.screen_curves_sensitivity_subtitle)
            val labels = listOf(
                stringResource(R.string.screen_curves_legend_r_layer),
                stringResource(R.string.screen_curves_legend_g_layer),
                stringResource(R.string.screen_curves_legend_b_layer),
            )
            ChartCard(title = title, subtitle = subtitle) {
                val channelColors = channelColors()
                SpectralChart(
                    xValues = data.wavelengths,
                    ySeriesList = transposeSeries(data.logSensitivity, 3),
                    channelColors = channelColors,
                    channelLabels = labels,
                    xLabel = stringResource(R.string.screen_curves_axis_wavelength),
                    yLabel = stringResource(R.string.screen_curves_axis_log_s),
                    description = chartDescription(title, displayName, subtitle, labels),
                    xRange = 380f..780f,
                )
            }
        } else {
            MissingDataNote(stringResource(R.string.screen_curves_sensitivity_missing))
        }

        // --- Chart 2: H-D / Density curves ---
        if (data.densityCurves != null && data.logExposure != null) {
            val title = stringResource(R.string.screen_curves_hd_title)
            val subtitle = stringResource(R.string.screen_curves_hd_subtitle)
            val labels = listOf(
                stringResource(R.string.screen_curves_legend_r),
                stringResource(R.string.screen_curves_legend_g),
                stringResource(R.string.screen_curves_legend_b),
            )
            ChartCard(title = title, subtitle = subtitle) {
                val channelColors = channelColors()
                GenericChart(
                    xValues = data.logExposure,
                    ySeriesList = transposeSeries(data.densityCurves, 3),
                    channelColors = channelColors,
                    channelLabels = labels,
                    xLabel = stringResource(R.string.screen_curves_axis_log_e),
                    yLabel = stringResource(R.string.screen_curves_axis_density),
                    description = chartDescription(title, displayName, subtitle, labels),
                )
            }
        } else {
            MissingDataNote(stringResource(R.string.screen_curves_hd_missing))
        }

        // --- Chart 3: Dye density spectra ---
        if (data.channelDensity != null) {
            val title = stringResource(R.string.screen_curves_dye_title)
            val subtitle = stringResource(R.string.screen_curves_dye_subtitle)
            val labels = listOf(
                stringResource(R.string.screen_curves_legend_c_dye),
                stringResource(R.string.screen_curves_legend_m_dye),
                stringResource(R.string.screen_curves_legend_y_dye),
            )
            ChartCard(title = title, subtitle = subtitle) {
                val channelColors = channelColors()
                SpectralChart(
                    xValues = data.wavelengths,
                    ySeriesList = transposeSeries(data.channelDensity, 3),
                    channelColors = channelColors,
                    channelLabels = labels,
                    xLabel = stringResource(R.string.screen_curves_axis_wavelength),
                    yLabel = stringResource(R.string.screen_curves_axis_density),
                    description = chartDescription(title, displayName, subtitle, labels),
                    xRange = 380f..780f,
                )
            }
        } else {
            MissingDataNote(stringResource(R.string.screen_curves_dye_missing))
        }

        Spacer(Modifier.height(32.dp))
    }
}

// ---------------------------------------------------------------------------
// Chart cards
// ---------------------------------------------------------------------------

@Composable
private fun ChartCard(
    title: String,
    subtitle: String,
    modifier: Modifier = Modifier,
    content: @Composable ColumnScope.() -> Unit,
) {
    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.35f),
        ),
    ) {
        Column(
            Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(
                title,
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.semantics { heading() },
            )
            Text(subtitle,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            content()
        }
    }
}

/** Spoken summary of a chart, e.g. "Density curve chart for Kodak Portra 400: ... Series: R, G, B." */
@Composable
private fun chartDescription(
    title: String,
    displayName: String,
    subtitle: String,
    channelLabels: List<String>,
): String = stringResource(
    R.string.screen_curves_chart_desc, title, displayName, subtitle, channelLabels.joinToString(", "),
)

@Composable
private fun MissingDataNote(text: String) {
    Surface(
        shape = RoundedCornerShape(12.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.35f),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Text(
            text,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(16.dp),
        )
    }
}

// ---------------------------------------------------------------------------
// Spectral chart (x-axis = fixed 380–780 nm range)
// ---------------------------------------------------------------------------

@Composable
private fun SpectralChart(
    xValues: List<Float>,       // wavelengths
    ySeriesList: List<List<Float>>, // one list per channel (same length as xValues)
    channelColors: List<Color>,
    channelLabels: List<String>,
    xLabel: String,
    yLabel: String,
    description: String,
    xRange: ClosedFloatingPointRange<Float> = 380f..780f,
    chartHeight: Dp = 200.dp,
) {
    GenericChart(
        xValues = xValues,
        ySeriesList = ySeriesList,
        channelColors = channelColors,
        channelLabels = channelLabels,
        xLabel = xLabel,
        yLabel = yLabel,
        description = description,
        xMin = xRange.start,
        xMax = xRange.endInclusive,
        chartHeight = chartHeight,
    )
}

// ---------------------------------------------------------------------------
// Generic two-axis canvas chart
// ---------------------------------------------------------------------------

@OptIn(ExperimentalTextApi::class)
@Composable
private fun GenericChart(
    xValues: List<Float>,
    ySeriesList: List<List<Float>>,
    channelColors: List<Color>,
    channelLabels: List<String>,
    xLabel: String,
    yLabel: String,
    description: String,
    xMin: Float? = null,
    xMax: Float? = null,
    chartHeight: Dp = 200.dp,
) {
    val onSurface = MaterialTheme.colorScheme.onSurface
    val onSurfaceVariant = MaterialTheme.colorScheme.onSurfaceVariant
    val gridColor = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.5f)
    val textMeasurer = rememberTextMeasurer()

    // Compute data ranges (ignoring NaN)
    val allX = xValues.filter { it.isFinite() }
    val computedXMin = xMin ?: (allX.minOrNull() ?: 0f)
    val computedXMax = xMax ?: (allX.maxOrNull() ?: 1f)

    val allY = ySeriesList.flatten().filter { it.isFinite() }
    val rawYMin = allY.minOrNull() ?: 0f
    val rawYMax = allY.maxOrNull() ?: 1f
    // Add a 5% margin top and bottom
    val yPad = (rawYMax - rawYMin) * 0.05f
    val computedYMin = rawYMin - yPad
    val computedYMax = rawYMax + yPad

    // Legend
    Legend(channelColors, channelLabels)

    Spacer(Modifier.height(6.dp))

    Canvas(
        modifier = Modifier
            .fillMaxWidth()
            .height(chartHeight)
            // The plot is pixels only; this is what TalkBack reads for it.
            .semantics { contentDescription = description },
    ) {
        val padLeft = 52f
        val padRight = 12f
        val padTop = 12f
        val padBottom = 42f

        val plotW = size.width - padLeft - padRight
        val plotH = size.height - padTop - padBottom

        if (plotW <= 0f || plotH <= 0f) return@Canvas

        // Helper: data → canvas coordinates
        fun xToCanvas(x: Float) = padLeft + (x - computedXMin) / (computedXMax - computedXMin) * plotW
        fun yToCanvas(y: Float) = padTop + (1f - (y - computedYMin) / (computedYMax - computedYMin)) * plotH

        // --- Gridlines + axes ---
        val gridStroke = Stroke(width = 1f)

        // Horizontal gridlines (Y axis)
        val yTicks = niceTickmarks(computedYMin, computedYMax, 5)
        for (tick in yTicks) {
            val cy = yToCanvas(tick)
            if (cy < padTop || cy > padTop + plotH + 1f) continue
            drawLine(gridColor, Offset(padLeft, cy), Offset(padLeft + plotW, cy),
                strokeWidth = 1f)
            val label = formatTick(tick)
            val measured = textMeasurer.measure(
                AnnotatedString(label),
                style = TextStyle(fontSize = 9.sp, color = onSurfaceVariant),
            )
            drawText(measured, topLeft = Offset(padLeft - measured.size.width - 4f,
                cy - measured.size.height / 2f))
        }

        // Vertical gridlines (X axis)
        val xTicks = niceTickmarks(computedXMin, computedXMax, 6)
        for (tick in xTicks) {
            val cx = xToCanvas(tick)
            if (cx < padLeft || cx > padLeft + plotW + 1f) continue
            drawLine(gridColor, Offset(cx, padTop), Offset(cx, padTop + plotH),
                strokeWidth = 1f)
            val label = formatTick(tick)
            val measured = textMeasurer.measure(
                AnnotatedString(label),
                style = TextStyle(fontSize = 9.sp, color = onSurfaceVariant),
            )
            drawText(measured, topLeft = Offset(cx - measured.size.width / 2f,
                padTop + plotH + 4f))
        }

        // Border box
        drawRect(
            color = onSurfaceVariant.copy(alpha = 0.4f),
            topLeft = Offset(padLeft, padTop),
            size = androidx.compose.ui.geometry.Size(plotW, plotH),
            style = Stroke(width = 1.5f),
        )

        // --- Axis labels (text) ---
        val xLabelMeasured = textMeasurer.measure(
            AnnotatedString(xLabel),
            style = TextStyle(fontSize = 10.sp, color = onSurface, fontWeight = FontWeight.Medium),
        )
        drawText(xLabelMeasured,
            topLeft = Offset(padLeft + plotW / 2f - xLabelMeasured.size.width / 2f,
                padTop + plotH + 24f))

        // --- Data series ---
        ySeriesList.forEachIndexed { seriesIndex, yValues ->
            if (seriesIndex >= channelColors.size) return@forEachIndexed
            val color = channelColors[seriesIndex]
            drawSpectralLine(xValues, yValues, color, ::xToCanvas, ::yToCanvas,
                padLeft, padTop, padLeft + plotW, padTop + plotH)
        }
    }

    // Y-axis label (rotated text is not trivially done in Canvas without rotate;
    // use a small Box with a rotated modifier instead)
    // We append the label below the chart in plain text since it's the simplest
    // approach that doesn't require graphicsLayer:
    Text(
        yLabel,
        style = MaterialTheme.typography.bodySmall.copy(fontSize = 10.sp),
        color = onSurfaceVariant,
    )
}

// ---------------------------------------------------------------------------
// Actual line-drawing helper (runs inside Canvas DrawScope)
// ---------------------------------------------------------------------------

private fun DrawScope.drawSpectralLine(
    xValues: List<Float>,
    yValues: List<Float>,
    color: Color,
    xToCanvas: (Float) -> Float,
    yToCanvas: (Float) -> Float,
    clipLeft: Float,
    clipTop: Float,
    clipRight: Float,
    clipBottom: Float,
) {
    val path = Path()
    var first = true
    for (i in xValues.indices) {
        val x = xValues.getOrNull(i) ?: break
        val y = yValues.getOrNull(i) ?: break
        if (!x.isFinite() || !y.isFinite()) { first = true; continue }
        val cx = xToCanvas(x).coerceIn(clipLeft - 2f, clipRight + 2f)
        val cy = yToCanvas(y).coerceIn(clipTop - 2f, clipBottom + 2f)
        if (first) { path.moveTo(cx, cy); first = false }
        else path.lineTo(cx, cy)
    }
    drawPath(path, color, style = Stroke(width = 2.5f, cap = StrokeCap.Round))
}

// ---------------------------------------------------------------------------
// Legend row
// ---------------------------------------------------------------------------

@Composable
private fun Legend(colors: List<Color>, labels: List<String>) {
    Row(
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.padding(top = 2.dp),
    ) {
        colors.zip(labels).forEach { (color, label) ->
            Row(verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                Canvas(Modifier.size(width = 24.dp, height = 3.dp)) {
                    drawLine(color, Offset(0f, size.height / 2f),
                        Offset(size.width, size.height / 2f), strokeWidth = 3f)
                }
                Text(label,
                    style = MaterialTheme.typography.bodySmall.copy(fontSize = 10.sp))
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Transpose row-major 2D list (outer=sample, inner=channel) → per-channel lists. */
private fun transposeSeries(rows: List<FloatArray>, nChannels: Int): List<List<Float>> {
    val out = List(nChannels) { ArrayList<Float>(rows.size) }
    for (row in rows) {
        for (ch in 0 until nChannels) {
            out[ch].add(if (ch < row.size) row[ch] else Float.NaN)
        }
    }
    return out
}

/** Generate [count] evenly-spaced nice tick values covering [lo..hi]. */
private fun niceTickmarks(lo: Float, hi: Float, count: Int): List<Float> {
    if (lo >= hi || count <= 0) return emptyList()
    val rawStep = (hi - lo) / count.toFloat()
    val magnitude = Math.pow(10.0, Math.floor(Math.log10(rawStep.toDouble()))).toFloat()
    val niceStep = when {
        rawStep / magnitude < 1.5f -> magnitude
        rawStep / magnitude < 3.5f -> 2f * magnitude
        rawStep / magnitude < 7.5f -> 5f * magnitude
        else -> 10f * magnitude
    }
    val start = Math.ceil((lo / niceStep).toDouble()).toFloat() * niceStep
    val ticks = ArrayList<Float>()
    var t = start
    while (t <= hi + niceStep * 0.001f) {
        ticks.add(t)
        t += niceStep
    }
    return ticks
}

private fun formatTick(v: Float): String =
    if (v == v.toLong().toFloat()) v.toLong().toString()
    else "%.2f".format(v).trimEnd('0').trimEnd('.')

/** Returns the three R/G/B channel colors appropriate for the current theme. */
@Composable
private fun channelColors(): List<Color> = listOf(
    Color(0xFFE53935.toInt()), // red
    Color(0xFF43A047.toInt()), // green
    Color(0xFF1E88E5.toInt()), // blue
)
