/*
 * Spektrafilm for Android — editor-grade preview viewer. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * A set of self-contained composables that turn the flat preview Image into an
 * editor-grade viewer:
 *   - ZoomableImage: pinch-zoom + pan + double-tap fit/2x, clamped to bounds.
 *   - CompareSlider: a draggable split handle revealing the input (before) vs the
 *     rendered output (after) in the same frame.
 *   - PreviewHistogramOverlay: compact RGB + luma histogram overlaid on the live
 *     preview, computed off the main thread and drawn with a Compose Canvas.
 *   - MagnifierOverlay: a full-screen 1:1 view of a real full-resolution crop that
 *     the caller renders through the engine, so dye-cloud grain truly resolves.
 *
 * No new gradle dependency: everything is built on Compose pointerInput, graphicsLayer
 * and Canvas plus the Material3 widgets already used elsewhere.
 */
package com.spectrafilm.app

import android.graphics.Bitmap
import com.spectrafilm.engine.LinearImage
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.minimumInteractiveComponentSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.unit.IntOffset
import kotlinx.coroutines.FlowPreview
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.debounce
import kotlin.math.min
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.withContext
import java.util.IdentityHashMap
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.max
import kotlin.math.roundToInt

/** Zoom limits for [ZoomableImage]. */
private const val MIN_ZOOM = 1f
private const val MAX_ZOOM = 8f

/**
 * GPU LUT preview surface for the FIT view. Shows the current look INSTANTLY — the engine's
 * baked 3D LUT sampled on-GPU (see [GpuLutPreview]) — with no per-edit CPU re-render, the way
 * Lightroom's loupe stays live. It is fit-only by design: a pinch or double-tap calls
 * [onZoomStart] so the caller hands off to the CPU [ZoomableImage], which renders the zoomed
 * region with grain/halation (a pointwise LUT can't carry those — but at the downscaled fit
 * preview they're averaged to near-invisibility anyway, so nothing meaningful is lost at fit).
 * A single tap reports normalized image coords for the magnifier. If the GL program can't build
 * on this device, [onUnavailable] fires once so the caller falls back to the CPU bitmap.
 */
@Composable
fun GpuPreviewSurface(
    proxy: LinearImage,
    lut: CubeLut,
    modifier: Modifier = Modifier,
    exposureGain: Float = 1f,
    onPointPicked: ((Float, Float) -> Unit)? = null,
    onZoomStart: () -> Unit = {},
    onZoomIn: () -> Unit = onZoomStart,
    onUnavailable: () -> Unit = {},
    // Height of whatever overlays the bottom of this surface (the floating adjustment panel):
    // the zoom control centres in the part that is still visible (#181, 200% font).
    controlsBottomInset: Dp = 0.dp,
) {
    var viewSize by remember { mutableStateOf(IntSize.Zero) }
    val aspect = proxy.width.toFloat() / proxy.height.toFloat()
    val previewDesc = stringResource(R.string.tool_viewer_preview_desc)
    Box(
        modifier = modifier
            .onSizeChanged { viewSize = it }
            // The GL view has no semantics of its own; describe the preview + its gestures here.
            .semantics { contentDescription = previewDesc }
            .pointerInput(Unit) {
                // This surface is fit-only; any pinch hands off to the CPU zoom path.
                detectTransformGestures { _, _, zoom, _ ->
                    if (kotlin.math.abs(zoom - 1f) > 0.001f) onZoomStart()
                }
            }
            .pointerInput(onPointPicked) {
                detectTapGestures(
                    onDoubleTap = { onZoomStart() },
                    onTap = { tap ->
                        val cb = onPointPicked ?: return@detectTapGestures
                        val n = fitViewToImageNormalized(tap, viewSize, aspect)
                            ?: return@detectTapGestures
                        cb(n.first, n.second)
                    },
                )
            },
    ) {
        GpuLutPreview(
            proxy = proxy, lut = lut,
            exposureGain = exposureGain,
            modifier = Modifier.fillMaxSize(), onUnavailable = onUnavailable,
        )
        // Explicit zoom-in affordance on the fit-only GPU surface: hands off to the CPU
        // ZoomableImage already zoomed (the CPU path owns pinch/pan + the sharp ROI render).
        ZoomButton(
            "+",
            contentDescription = stringResource(R.string.tool_viewer_zoom_in),
            modifier = Modifier
                .align(Alignment.CenterEnd)
                .padding(end = 6.dp, bottom = controlsBottomInset),
        ) { onZoomIn() }
    }
}

/** Map a tap to normalized image coords for a ContentScale.Fit (letterboxed) image at fit zoom. */
private fun fitViewToImageNormalized(tap: Offset, view: IntSize, aspect: Float): Pair<Float, Float>? {
    if (view.width == 0 || view.height == 0) return null
    val viewA = view.width.toFloat() / view.height
    val dispW: Float
    val dispH: Float
    if (viewA > aspect) { dispH = view.height.toFloat(); dispW = dispH * aspect }
    else { dispW = view.width.toFloat(); dispH = dispW / aspect }
    val x0 = (view.width - dispW) / 2f
    val y0 = (view.height - dispH) / 2f
    val nx = (tap.x - x0) / dispW
    val ny = (tap.y - y0) / dispH
    return if (nx in 0f..1f && ny in 0f..1f) nx to ny else null
}

/**
 * A pinch-zoom + pan image that fits its bitmap to the available width (ContentScale.Fit)
 * at zoom 1. Pinch to zoom up to [MAX_ZOOM]x; drag to pan while zoomed; double-tap toggles
 * between fit (1x) and 2x centred on the tap. Pan is clamped so the image cannot be dragged
 * past its own edges. [onPointPicked] reports the tapped location in normalized image
 * coordinates (0..1, 0..1) for the magnifier; it is invoked on a single tap when not zoomed
 * past fit, otherwise a single tap is ignored (the gesture is treated as a pan inspect).
 */
@OptIn(FlowPreview::class)
@Composable
fun ZoomableImage(
    bitmap: Bitmap,
    modifier: Modifier = Modifier,
    onPointPicked: ((Float, Float) -> Unit)? = null,
    // Lightroom-style zoom: when zoomed past fit, [onRoiSettled] fires (debounced) with the
    // visible region so the caller can render that crop at native resolution and pass it back
    // as [roiOverlay]; [onRoiCleared] fires when zoom returns to fit. [renderKey] (e.g. the
    // preview tick) re-fires the settle so an edit while zoomed re-renders the sharp region.
    renderKey: Int = 0,
    onRoiSettled: ((RoiRect) -> Unit)? = null,
    onRoiCleared: (() -> Unit)? = null,
    roiOverlay: RoiOverlay? = null,
    // See [GpuPreviewSurface.controlsBottomInset].
    controlsBottomInset: Dp = 0.dp,
    // Scale to start at on first composition — used when the GPU fit surface hands off via its
    // "+" button so zoom-in lands already zoomed instead of at fit. Default 1f (fit) everywhere else.
    initialScale: Float = 1f,
) {
    var scale by remember { mutableFloatStateOf(initialScale) }
    var offset by remember { mutableStateOf(Offset.Zero) }
    var viewSize by remember { mutableStateOf(IntSize.Zero) }
    // While zoomed, an edit (renderKey bump) makes the current sharp ROI crop stale: hide it so the
    // live (soft) proxy — which the draft render keeps current — shows through, then reveal the
    // freshly rendered crop when it lands. The zoom-path half of the draft/final render port.
    var overlayStale by remember { mutableStateOf(false) }
    LaunchedEffect(renderKey) { overlayStale = true }
    LaunchedEffect(roiOverlay) { overlayStale = false }

    val image = remember(bitmap) { bitmap.asImageBitmap() }
    val aspect = bitmap.width.toFloat() / bitmap.height.toFloat()

    // Drive the sharp ROI render off the zoom/pan transform (and re-fire on a param edit via
    // renderKey). Debounced so we render on settle, not every gesture frame; the instantly
    // graphicsLayer-scaled proxy stays visible until the sharp crop lands (progressive, like
    // the two-pass preview). Reverts to the proxy when zoom returns to fit.
    if (onRoiSettled != null) {
        LaunchedEffect(renderKey, aspect) {
            snapshotFlow { Triple(scale, offset, viewSize) }
                // Raised from 280ms: each zoom settle triggers a full RAW proxy decode + an
                // all-core engine render (~1s). A longer settle coalesces a pinch/pan into a
                // single render instead of a herd of overlapping decodes (the main editing
                // battery drain seen in the device logcat).
                .debounce(500L)
                .collectLatest { (s, o, v) ->
                    val roi = viewportRoiNormalized(v, s, o, aspect)
                    if (roi == null) onRoiCleared?.invoke() else onRoiSettled(roi)
                }
        }
    }

    // Clamp the pan so the (scaled) CONTENT stays within the view bounds.
    //
    // The bound must come from the fitted CONTENT rect, not the viewport. The
    // bitmap is drawn ContentScale.Fit and is therefore LETTERBOXED: at fit it
    // fills one axis and leaves bars on the other. Using the viewport extent on
    // the letterboxed axis over-permits the pan by exactly the letterbox ratio,
    // and the image can be dragged clean out of view — a black viewport with the
    // zoom pill still reading 410%.
    //
    // Measured on device (998x1802 viewport, 4:3 landscape content, s=4.1):
    // content fits to 998x749, so the true bound is (749*4.1 - 1802)/2 = 634,
    // while the viewport formula gave (1802*(4.1-1))/2 = 2793 — 4.4x too far.
    //
    // The horizontal axis was correct only BY ACCIDENT: this content fills the
    // width at fit, so fitW == viewSize.width and the two formulas coincide.
    // That is why panning left/right behaved and up/down went black; a PORTRAIT
    // photo on this same screen would have broken the other way round.
    fun clampOffset(raw: Offset, s: Float): Offset =
        clampPanOffset(raw, viewSize, s, aspect)

    Box(
        modifier = modifier
            .clipToBounds()
            .onSizeChanged { viewSize = it }
            .pointerInput(Unit) {
                detectTransformGestures { centroid, pan, zoom, _ ->
                    val oldScale = scale
                    val newScale = (oldScale * zoom).coerceIn(MIN_ZOOM, MAX_ZOOM)
                    // Anchor the zoom about the gesture centroid (relative to view centre),
                    // then add the pan delta; finally clamp so edges can't be over-dragged.
                    val c = Offset(viewSize.width / 2f, viewSize.height / 2f)
                    val centroidRel = centroid - c
                    val newOffset = (offset - centroidRel) * (newScale / oldScale) + centroidRel + pan
                    scale = newScale
                    offset = clampOffset(newOffset, newScale)
                }
            }
            .pointerInput(onPointPicked) {
                detectTapGestures(
                    onDoubleTap = { tap ->
                        if (scale > 1.01f) {
                            scale = 1f
                            offset = Offset.Zero
                        } else {
                            val target = 2f
                            val c = Offset(viewSize.width / 2f, viewSize.height / 2f)
                            scale = target
                            offset = clampOffset((c - tap) * target, target)
                        }
                    },
                    onTap = { tap ->
                        val cb = onPointPicked ?: return@detectTapGestures
                        val n = viewToImageNormalized(tap, viewSize, scale, offset, aspect)
                        if (n != null) cb(n.x, n.y)
                    },
                )
            },
        contentAlignment = Alignment.Center,
    ) {
        val zoomState = stringResource(R.string.tool_viewer_zoom_state, (scale * 100f).roundToInt())
        Image(
            bitmap = image,
            contentDescription = stringResource(R.string.tool_viewer_preview_desc),
            contentScale = ContentScale.Fit,
            modifier = Modifier
                .fillMaxWidth()
                .aspectRatio(aspect)
                .semantics { stateDescription = zoomState }
                .graphicsLayer(
                    scaleX = scale,
                    scaleY = scale,
                    translationX = offset.x,
                    translationY = offset.y,
                ),
        )

        // Sharp ROI overlay: project the cached crop's normalized rect through the SAME
        // transform as the proxy, so it registers exactly and tracks pan/zoom. Drawn over the
        // (soft) scaled proxy; clipToBounds on the Box clips any overflow.
        val ov = roiOverlay
        if (ov != null && !overlayStale && scale > 1.01f && viewSize.width > 0) {
            val roiImage = remember(ov.bitmap) { ov.bitmap.asImageBitmap() }
            Canvas(Modifier.fillMaxSize()) {
                val p0 = imageNormToView(
                    ov.cxN - ov.wN / 2f, ov.cyN - ov.hN / 2f, viewSize, scale, offset, aspect,
                )
                val p1 = imageNormToView(
                    ov.cxN + ov.wN / 2f, ov.cyN + ov.hN / 2f, viewSize, scale, offset, aspect,
                )
                val dstW = (p1.x - p0.x).toInt()
                val dstH = (p1.y - p0.y).toInt()
                if (dstW > 0 && dstH > 0) {
                    drawImage(
                        image = roiImage,
                        srcOffset = IntOffset.Zero,
                        srcSize = IntSize(roiImage.width, roiImage.height),
                        dstOffset = IntOffset(p0.x.toInt(), p0.y.toInt()),
                        dstSize = IntSize(dstW, dstH),
                    )
                }
            }
        }

        // Explicit zoom controls (Lightroom-style), complementing pinch + double-tap: "+" zooms
        // in about centre, and once zoomed a "%" readout, "−" (zoom out) and "Fit" appear. They
        // drive the SAME scale/offset as the gestures, so pan-clamping and the sharp ROI render
        // apply identically. Anchored to the right edge so it clears the bottom control row.
        Column(
            modifier = Modifier
                .align(Alignment.CenterEnd)
                .padding(end = 6.dp, bottom = controlsBottomInset),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            ZoomButton("+", stringResource(R.string.tool_viewer_zoom_in)) {
                val ns = (scale * 1.6f).coerceIn(MIN_ZOOM, MAX_ZOOM)
                scale = ns
                offset = clampOffset(offset, ns)
            }
            if (scale > 1.01f) {
                Text(
                    stringResource(R.string.tool_viewer_zoom_percent, (scale * 100f).roundToInt()),
                    color = Color.White,
                    style = MaterialTheme.typography.labelSmall,
                    modifier = Modifier
                        .background(Color.Black.copy(alpha = 0.5f), RoundedCornerShape(6.dp))
                        .padding(horizontal = 6.dp, vertical = 2.dp),
                )
                ZoomButton("−", stringResource(R.string.tool_viewer_zoom_out)) {  // minus
                    val ns = (scale / 1.6f).coerceIn(MIN_ZOOM, MAX_ZOOM)
                    scale = ns
                    offset = if (ns <= 1.01f) Offset.Zero else clampOffset(offset, ns)
                }
                ZoomButton(
                    stringResource(R.string.tool_viewer_zoom_fit_short),
                    stringResource(R.string.tool_viewer_zoom_fit),
                ) {
                    scale = 1f
                    offset = Offset.Zero
                }
            }
        }
    }
}

/**
 * A small translucent circular button for the [ZoomableImage] zoom-control cluster. [label] is the
 * visible glyph ("+", "−", "Fit"); [contentDescription] is what TalkBack announces instead of it.
 */
@Composable
private fun ZoomButton(
    label: String,
    contentDescription: String,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    Surface(
        shape = CircleShape,
        color = Color.Black.copy(alpha = 0.5f),
        modifier = modifier
            .minimumInteractiveComponentSize()
            .size(40.dp)
            .clip(CircleShape)
            .clickable(role = Role.Button, onClick = onClick)
            .semantics { this.contentDescription = contentDescription },
    ) {
        Box(contentAlignment = Alignment.Center) {
            Text(
                label,
                color = Color.White,
                style = if (label.length > 1) MaterialTheme.typography.labelMedium
                else MaterialTheme.typography.titleLarge,
            )
        }
    }
}

/** A normalized image rectangle (centre + size, all in 0..1) — the visible region under zoom. */
data class RoiRect(val cxN: Float, val cyN: Float, val wN: Float, val hN: Float)

/** A sharp region-of-interest render plus the normalized image rect it represents, so the
 *  overlay tracks pan/zoom (and survives a stale frame) by re-projecting that rect. */
class RoiOverlay(
    val bitmap: Bitmap,
    val cxN: Float, val cyN: Float, val wN: Float, val hN: Float,
)

/** The fitted (zoom=1) image rectangle inside [view] under ContentScale.Fit: (left, top, w, h). */
private fun fitRect(view: IntSize, aspect: Float): FloatArray {
    val viewAspect = view.width.toFloat() / view.height.toFloat()
    val fitW: Float
    val fitH: Float
    if (aspect > viewAspect) { fitW = view.width.toFloat(); fitH = fitW / aspect }
    else { fitH = view.height.toFloat(); fitW = fitH * aspect }
    val left = view.width / 2f - fitW / 2f
    val top = view.height / 2f - fitH / 2f
    return floatArrayOf(left, top, fitW, fitH)
}

/**
 * Clamp a pan offset so the SCALED CONTENT cannot be dragged outside the viewport.
 *
 * Top-level and `internal` so the math is unit-testable — the defect below shipped
 * because the only clamp coverage used a square image in a square view, i.e. the
 * one geometry where the bug is invisible.
 */
internal fun clampPanOffset(raw: Offset, view: IntSize, scale: Float, aspect: Float): Offset {
    if (view.width <= 0 || view.height <= 0 || !(aspect > 0f)) return Offset.Zero
    val (_, _, fitW, fitH) = fitRect(view, aspect)
    val maxX = max(0f, (fitW * scale - view.width) / 2f)
    val maxY = max(0f, (fitH * scale - view.height) / 2f)
    return Offset(raw.x.coerceIn(-maxX, maxX), raw.y.coerceIn(-maxY, maxY))
}

/** Inverse of the viewport transform: a view-space point → normalized image coords (UNclamped). */
internal fun mapViewToImageNorm(
    p: Offset, view: IntSize, scale: Float, offset: Offset, aspect: Float,
): Offset {
    val cx = view.width / 2f
    val cy = view.height / 2f
    // Undo the graphicsLayer transform (scale about centre, then translate).
    val ux = (p.x - cx - offset.x) / scale + cx
    val uy = (p.y - cy - offset.y) / scale + cy
    val (left, top, fitW, fitH) = fitRect(view, aspect)
    return Offset((ux - left) / fitW, (uy - top) / fitH)
}

/** Forward transform: normalized image coords → view-space point under the current zoom/pan. */
internal fun imageNormToView(
    nx: Float, ny: Float, view: IntSize, scale: Float, offset: Offset, aspect: Float,
): Offset {
    val (left, top, fitW, fitH) = fitRect(view, aspect)
    val ux = left + nx * fitW
    val uy = top + ny * fitH
    val cx = view.width / 2f
    val cy = view.height / 2f
    return Offset((ux - cx) * scale + cx + offset.x, (uy - cy) * scale + cy + offset.y)
}

/**
 * Map a view-space tap to normalized image coordinates (0..1), accounting for the
 * ContentScale.Fit letterboxing and the current zoom/pan transform. Returns null if the
 * tap fell on the letterbox margin (outside the image).
 */
private fun viewToImageNormalized(
    tap: Offset, view: IntSize, scale: Float, offset: Offset, aspect: Float,
): Offset? {
    if (view.width == 0 || view.height == 0) return null
    val n = mapViewToImageNorm(tap, view, scale, offset, aspect)
    if (n.x < 0f || n.x > 1f || n.y < 0f || n.y > 1f) return null
    return n
}

/**
 * The visible image region under the current zoom/pan, as a normalized centre + size, or null
 * when not zoomed past fit (so the caller reverts to the proxy). Maps the viewport's corners
 * through the inverse transform and clamps to the image bounds. Pure — unit-tested.
 */
internal fun viewportRoiNormalized(
    view: IntSize, scale: Float, offset: Offset, aspect: Float,
): RoiRect? {
    if (view.width == 0 || view.height == 0 || scale <= 1.01f) return null
    val a = mapViewToImageNorm(Offset(0f, 0f), view, scale, offset, aspect)
    val b = mapViewToImageNorm(
        Offset(view.width.toFloat(), view.height.toFloat()), view, scale, offset, aspect,
    )
    val nx0 = min(a.x, b.x).coerceIn(0f, 1f)
    val ny0 = min(a.y, b.y).coerceIn(0f, 1f)
    val nx1 = max(a.x, b.x).coerceIn(0f, 1f)
    val ny1 = max(a.y, b.y).coerceIn(0f, 1f)
    val wN = nx1 - nx0
    val hN = ny1 - ny0
    if (wN <= 0f || hN <= 0f) return null
    return RoiRect((nx0 + nx1) / 2f, (ny0 + ny1) / 2f, wN, hN)
}

/**
 * A before/after split viewer. The [after] bitmap is shown across the whole frame; the
 * [before] bitmap is clipped to the left of a draggable vertical handle, revealing the
 * input where the handle hasn't passed. Drag the handle (or anywhere in the frame) left/
 * right to wipe between the two. Both bitmaps are drawn with ContentScale.Fit so they
 * register exactly.
 */
@Composable
fun CompareSlider(
    before: Bitmap,
    after: Bitmap,
    modifier: Modifier = Modifier,
) {
    var split by remember { mutableFloatStateOf(0.5f) }
    var width by remember { mutableIntStateOf(0) }
    val aspect = after.width.toFloat() / after.height.toFloat()
    val beforeImg = remember(before) { before.asImageBitmap() }
    val afterImg = remember(after) { after.asImageBitmap() }
    // Drag/tap-only wipe: one merged node describing the comparison, its split as state, and
    // three custom actions so a screen reader can move the wipe without a gesture.
    val compareDesc = stringResource(R.string.tool_viewer_compare_desc)
    val compareState = stringResource(R.string.tool_viewer_compare_state, (split * 100f).roundToInt())
    val showBefore = stringResource(R.string.tool_viewer_compare_show_before)
    val showAfter = stringResource(R.string.tool_viewer_compare_show_after)
    val splitHalf = stringResource(R.string.tool_viewer_compare_split_half)

    Box(
        modifier = modifier
            .fillMaxWidth()
            .aspectRatio(aspect)
            .clipToBounds()
            .onSizeChanged { width = it.width }
            .semantics(mergeDescendants = true) {
                contentDescription = compareDesc
                stateDescription = compareState
                customActions = listOf(
                    CustomAccessibilityAction(showBefore) { split = 1f; true },
                    CustomAccessibilityAction(showAfter) { split = 0f; true },
                    CustomAccessibilityAction(splitHalf) { split = 0.5f; true },
                )
            }
            .pointerInput(Unit) {
                detectTapGestures { pos ->
                    if (width > 0) split = (pos.x / width).coerceIn(0f, 1f)
                }
            }
            .pointerInput(Unit) {
                detectTransformGestures { centroid, _, _, _ ->
                    if (width > 0) split = (centroid.x / width).coerceIn(0f, 1f)
                }
            },
        contentAlignment = Alignment.Center,
    ) {
        // After (full frame). Both images are described by the parent node, so no per-image cd.
        Image(afterImg, null, Modifier.fillMaxSize(), contentScale = ContentScale.Fit)
        // Before, clipped to the left of the split via a draw-phase rect clip.
        Image(
            bitmap = beforeImg,
            contentDescription = null,
            contentScale = ContentScale.Fit,
            modifier = Modifier
                .fillMaxSize()
                .drawWithContent {
                    clipRect(right = size.width * split) { this@drawWithContent.drawContent() }
                },
        )
        // Handle + labels.
        Box(Modifier.fillMaxSize()) {
            Canvas(Modifier.fillMaxSize()) {
                val x = size.width * split
                drawLine(
                    color = Color.White,
                    start = Offset(x, 0f),
                    end = Offset(x, size.height),
                    strokeWidth = 3f,
                )
            }
            CompareTag(stringResource(R.string.tool_viewer_before_tag), Alignment.TopStart)
            CompareTag(stringResource(R.string.tool_viewer_after_tag), Alignment.TopEnd)
        }
    }
}

@Composable
private fun CompareTag(text: String, alignment: Alignment) {
    Box(Modifier.fillMaxSize()) {
        Text(
            text,
            color = Color.White,
            style = MaterialTheme.typography.labelSmall,
            modifier = Modifier
                .align(alignment)
                .padding(6.dp)
                .background(Color.Black.copy(alpha = 0.45f), RoundedCornerShape(6.dp))
                .padding(horizontal = 6.dp, vertical = 2.dp),
        )
    }
}

/**
 * A compact, translucent histogram overlaid on the TOP EDGE of the live preview
 * (Lightroom-mobile style). Sampling and binning both run off-main under an explicit read lease.
 * Replacing the bitmap resets the remembered bins synchronously, and retirement defers recycle
 * until any already-running reader releases its lease.
 */
@Composable
fun PreviewHistogramOverlay(bitmap: Bitmap, modifier: Modifier = Modifier) {
    // Keying state by identity prevents the previous frame's bins from being displayed while the
    // new frame is sampled. LaunchedEffect cancellation also rejects any late old-frame result.
    var hist by remember(bitmap) { mutableStateOf<Histogram?>(null) }
    LaunchedEffect(bitmap) {
        hist = try {
            computeHistogram(bitmap)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Throwable) {
            Diag.w("preview histogram unavailable: ${failure.message}")
            null
        }
    }
    val h = hist ?: return
    val histogramDesc = stringResource(R.string.tool_viewer_histogram_desc)
    Box(
        modifier = modifier
            .fillMaxWidth(0.6f)
            .height(56.dp)
            .clip(RoundedCornerShape(8.dp))
            .background(Color.Black.copy(alpha = 0.42f))
            .semantics { contentDescription = histogramDesc },
    ) {
        Canvas(Modifier.fillMaxSize().padding(4.dp)) { drawHistogram(h) }
    }
}

/** Per-channel 256-bin counts plus the per-channel maximum used for scaling. */
class Histogram(
    val r: IntArray,
    val g: IntArray,
    val b: IntArray,
    val luma: IntArray,
    val peak: Int,
)

/**
 * Small identity-keyed read-lease registry. The owner may retire a value immediately; physical
 * release is deferred until existing readers close, and new readers are rejected after retirement.
 */
internal class RetirableReadLeaseRegistry<T : Any>(
    private val isPhysicallyRetired: (T) -> Boolean,
    private val physicallyRetire: (T) -> Unit,
    private val beforeLeaseConstruction: () -> Unit = {},
) {
    private data class State(var readers: Int = 0, var retired: Boolean = false)

    internal class Lease<T : Any>(
        val value: T,
        private val release: () -> Unit,
    ) : AutoCloseable {
        private val closed = AtomicBoolean(false)

        override fun close() {
            if (closed.compareAndSet(false, true)) release()
        }
    }

    private val states = IdentityHashMap<T, State>()

    @Synchronized
    fun acquire(value: T): Lease<T>? {
        if (isPhysicallyRetired(value)) return null
        val state = states.getOrPut(value) { State() }
        if (state.retired) return null
        state.readers++
        return try {
            beforeLeaseConstruction()
            Lease(value) { release(value, state) }
        } catch (failure: Throwable) {
            state.readers--
            if (state.readers == 0 && !state.retired) states.remove(value)
            throw failure
        }
    }

    @Synchronized
    fun retire(value: T) {
        if (isPhysicallyRetired(value)) return
        val state = states[value]
        if (state == null) {
            // Retire under the same monitor as acquire. This closes the acquire/recycle window.
            physicallyRetire(value)
            return
        }
        if (state.retired) return
        state.retired = true
        if (state.readers == 0) retireNow(value, state)
    }

    @Synchronized
    private fun release(value: T, expected: State) {
        val state = states[value]
        check(state === expected && state.readers > 0) { "read-lease registry underflow" }
        state.readers--
        if (state.retired && state.readers == 0) retireNow(value, state)
    }

    /** Caller holds this registry's monitor. */
    private fun retireNow(value: T, expected: State) {
        check(states[value] === expected)
        physicallyRetire(value)
        states.remove(value)
    }
}

private val previewBitmapReadLeases = RetirableReadLeaseRegistry<Bitmap>(
    isPhysicallyRetired = { bitmap -> bitmap.isRecycled },
    physicallyRetire = { bitmap -> if (!bitmap.isRecycled) bitmap.recycle() },
)

/** Retire a preview deterministically once every histogram reader has released it. */
internal fun retirePreviewBitmap(bitmap: Bitmap) {
    previewBitmapReadLeases.retire(bitmap)
}

/** Bitmap-independent immutable sample set; safe to bin after its Bitmap is retired. */
internal class HistogramSamples internal constructor(pixels: IntArray) {
    internal val pixels: IntArray = pixels.copyOf()
}

/** Capture stride-decimated pixels while the caller holds a preview read lease. */
private fun sampleHistogram(bmp: Bitmap): HistogramSamples {
    val w = bmp.width
    val h = bmp.height
    val total = w.toLong() * h.toLong()
    val targetSamples = 200_000L
    val stride = max(1, (total / targetSamples).toInt())
    val sampledWidth = (w + stride - 1) / stride
    val sampledHeight = (h + stride - 1) / stride
    val samples = IntArray(sampledWidth * sampledHeight)
    val row = IntArray(w)
    var sampleIndex = 0
    var sy = 0
    while (sy < h) {
        bmp.getPixels(row, 0, w, 0, sy, w, 1)
        var sx = 0
        while (sx < w) {
            samples[sampleIndex++] = row[sx]
            sx += stride
        }
        sy += stride
    }
    check(sampleIndex == samples.size) { "histogram sample geometry mismatch" }
    return HistogramSamples(samples)
}

/**
 * Sample a bitmap (stride-decimated for speed) into 256-bin RGB + luma histograms. The entire
 * Bitmap access runs on Dispatchers.Default and is protected from deterministic recycle by a read
 * lease. A frame retired before the worker acquires it produces no histogram.
 */
suspend fun computeHistogram(bmp: Bitmap): Histogram? = withContext(Dispatchers.Default) {
    val lease = previewBitmapReadLeases.acquire(bmp) ?: return@withContext null
    lease.use { computeHistogram(sampleHistogram(it.value)) }
}

/** Bin a Bitmap-independent sample captured by [sampleHistogram]. */
internal fun computeHistogram(samples: HistogramSamples): Histogram {
    val r = IntArray(256); val g = IntArray(256); val b = IntArray(256); val l = IntArray(256)
    for (c in samples.pixels) {
        val rr = (c shr 16) and 0xFF
        val gg = (c shr 8) and 0xFF
        val bb = c and 0xFF
        r[rr]++; g[gg]++; b[bb]++
        val y = ((rr * 54 + gg * 183 + bb * 19) shr 8).coerceIn(0, 255)
        l[y]++
    }
    var peak = 1
    for (i in 0 until 256) {
        peak = max(peak, max(max(r[i], g[i]), max(b[i], l[i])))
    }
    return Histogram(r, g, b, l, peak)
}

private fun DrawScope.drawHistogram(hist: Histogram) {
    val w = size.width
    val h = size.height
    val binW = w / 256f
    // log scale compresses the spikes so the shape is readable.
    val logPeak = kotlin.math.ln(1f + hist.peak.toFloat())
    fun y(v: Int): Float = h - (kotlin.math.ln(1f + v.toFloat()) / logPeak) * h

    fun drawChannel(data: IntArray, color: Color) {
        for (i in 0 until 256) {
            val x = i * binW
            val top = y(data[i])
            drawLine(color, Offset(x, h), Offset(x, top), strokeWidth = binW.coerceAtLeast(1f))
        }
    }
    // luma backdrop, then additive-ish RGB on top
    drawChannel(hist.luma, Color(0x33FFFFFF))
    drawChannel(hist.r, Color(0x88FF4040))
    drawChannel(hist.g, Color(0x8840FF40))
    drawChannel(hist.b, Color(0x884070FF))
}

/**
 * A full-screen overlay that shows a real, full-resolution engine render of a crop, at
 * 1:1 (pixel-for-pixel) so that grain/dye-cloud structure actually resolves. While the
 * crop is being rendered, [crop] is null and a progress state is shown. The user dismisses
 * the overlay with the close button or by tapping the scrim.
 */
@Composable
fun MagnifierOverlay(
    crop: Bitmap?,
    rendering: Boolean,
    status: String,
    onClose: () -> Unit,
) {
    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.85f))
            .pointerInput(Unit) { detectTapGestures(onTap = { onClose() }) },
        contentAlignment = Alignment.Center,
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(12.dp),
            modifier = Modifier.padding(24.dp),
        ) {
            Text(
                stringResource(R.string.tool_viewer_magnifier_title),
                color = Color.White,
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.semantics { heading() },
            )
            Box(
                Modifier
                    .size(320.dp)
                    .clip(RoundedCornerShape(12.dp))
                    .background(Color(0xFF050505)),
                contentAlignment = Alignment.Center,
            ) {
                val c = crop
                if (c != null) {
                    // The crop is a real full-resolution engine render (~512px native), shown
                    // 1:1 — no upscale of the preview — so dye-cloud grain truly resolves.
                    Image(
                        bitmap = c.asImageBitmap(),
                        contentDescription = stringResource(R.string.tool_viewer_magnifier_image_desc),
                        contentScale = ContentScale.Fit,
                        modifier = Modifier.fillMaxSize(),
                    )
                } else {
                    CircularProgressIndicator(color = Color.White)
                }
            }
            // Progress copy is announced as it changes (polite: never interrupts).
            Text(
                status,
                color = Color.White.copy(alpha = 0.8f),
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
            )
            if (rendering) {
                Text(
                    stringResource(R.string.tool_viewer_magnifier_rendering),
                    color = Color.White.copy(alpha = 0.7f),
                    style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
                )
            }
            TextButton(onClick = onClose) {
                Text(stringResource(R.string.tool_close), color = Color.White)
            }
        }
    }
}
