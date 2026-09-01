/*
 * Spektrafilm for Android — interactive crop overlay. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * A Lightroom-mobile-style full-screen crop tool that replaces the old numeric
 * crop sliders. The user drags corner/edge handles of a crop rectangle over the
 * preview image, with a rule-of-thirds grid and a dimmed exterior; aspect-ratio
 * preset chips constrain the rectangle; a 90° rotate control is included.
 *
 * On confirm the selected rectangle is translated into the engine's existing crop
 * parameters (io.crop / io.crop_center / io.crop_size). The mapping is derived
 * directly from the engine port (runtime/stages/crop_resize.cpp::crop_image):
 *
 *   center is (x, y), fractions of (W, H).
 *   size   is (x, y), fractions of the LONG side max(W, H).
 *
 * So for a normalized rectangle [left, top, right, bottom] (each 0..1 of W or H):
 *   center.x = (left + right) / 2          center.y = (top + bottom) / 2
 *   size.x   = (right - left) * W / maxDim size.y   = (bottom - top) * H / maxDim
 *
 * where maxDim = max(W, H). This reproduces, on the engine side,
 *   cn  = round(shape * center_yx);  sz = round(maxDim * size_yx)
 * i.e. exactly the user's selected pixel box. We snap center so the box stays
 * inside the image (the engine also clamps x0, but we keep the UI honest).
 */
package com.spectrafilm.app

import android.graphics.Bitmap
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.Canvas
import kotlin.math.max
import kotlin.math.roundToInt

/** Aspect-ratio presets for the crop overlay. `ratio == null` means free / unconstrained. */
private enum class CropAspect(val label: String, val ratio: Float?) {
    FREE("Free", null),
    ORIGINAL("Original", null),  // resolved at runtime to the image aspect
    SQUARE("1:1", 1f),
    R4_3("4:3", 4f / 3f),
    R3_2("3:2", 3f / 2f),
    R16_9("16:9", 16f / 9f),
}

/**
 * The interactive crop tool. [bitmap] is the live preview (used purely to show the
 * image and read its aspect ratio). [initialCrop]/[initialCenter]/[initialSize]
 * seed the rectangle from the current recipe. [onRotate] rotates the source 90°
 * (the same control the preview uses). [onConfirm] reports `crop`, `center` (x,y)
 * and `size` (x,y) ready to write straight into ParamsState; [onCancel] discards.
 */
@Composable
fun CropOverlay(
    bitmap: Bitmap,
    initialCrop: Boolean,
    initialCenter: Pair<Float, Float>,
    initialSize: Pair<Float, Float>,
    onRotate: () -> Unit,
    onConfirm: (crop: Boolean, center: Pair<Float, Float>, size: Pair<Float, Float>) -> Unit,
    onCancel: () -> Unit,
) {
    val imgW = bitmap.width.toFloat().coerceAtLeast(1f)
    val imgH = bitmap.height.toFloat().coerceAtLeast(1f)
    val maxDim = max(imgW, imgH)
    val imageAspect = imgW / imgH

    val image: ImageBitmap = remember(bitmap) { bitmap.asImageBitmap() }

    // The crop rectangle in NORMALIZED IMAGE COORDS: left/top/right/bottom in 0..1
    // of (W, H). Seed from the incoming recipe crop, or default to the full frame.
    var rect by remember {
        mutableStateOf(
            if (initialCrop) {
                // Invert the engine mapping to recover the normalized rectangle.
                val halfW = (initialSize.first * maxDim / imgW) / 2f
                val halfH = (initialSize.second * maxDim / imgH) / 2f
                val cx = initialCenter.first
                val cy = initialCenter.second
                Rect(
                    left = (cx - halfW).coerceIn(0f, 1f),
                    top = (cy - halfH).coerceIn(0f, 1f),
                    right = (cx + halfW).coerceIn(0f, 1f),
                    bottom = (cy + halfH).coerceIn(0f, 1f),
                )
            } else {
                Rect(0f, 0f, 1f, 1f)
            }
        )
    }

    var aspect by remember { mutableStateOf(CropAspect.FREE) }

    // Fixed #139 evidence is gated inside the bridge and is a no-op for normal app sessions. The
    // effect runs only after this actual overlay instance has initialized its remembered draft.
    LaunchedEffect(Unit) {
        Ticket139EditorProbe.publishCropDraftInitialized(
            bitmapWidth = bitmap.width,
            bitmapHeight = bitmap.height,
            initialCrop = initialCrop,
            initialCenter = initialCenter,
            initialSize = initialSize,
            actualLeft = rect.left,
            actualTop = rect.top,
            actualRight = rect.right,
            actualBottom = rect.bottom,
        )
    }

    // Pixel size of the laid-out image inside the canvas (set by onSizeChanged on
    // the image Box). Used to convert drag pixels <-> normalized coords.
    var canvasSize by remember { mutableStateOf(IntSize.Zero) }

    // The handle grabbed at drag-start. Held in a remember box (mutated from the
    // gesture callbacks, never read during composition) so each overlay instance
    // owns its own handle state.
    val handleRef = remember { mutableStateOf(Handle.NONE) }

    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.92f))
            .statusBarsPadding()
            .navigationBarsPadding(),
    ) {
        Column(Modifier.fillMaxSize()) {
            // --- top action bar: cancel / title / rotate / confirm ---
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                TextTooltip("Cancel crop") {
                    androidx.compose.material3.IconButton(onClick = onCancel) {
                        Icon(SpectraIcons.Cancel, contentDescription = "Cancel crop", tint = Color.White)
                    }
                }
                Text(
                    "Crop", color = Color.White,
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.padding(start = 4.dp),
                )
                androidx.compose.foundation.layout.Spacer(Modifier.weight(1f))
                TextTooltip("Rotate 90°") {
                    androidx.compose.material3.IconButton(onClick = onRotate) {
                        Icon(SpectraIcons.Rotate, contentDescription = "Rotate 90°", tint = Color.White)
                    }
                }
                TextTooltip("Apply crop") {
                    androidx.compose.material3.IconButton(
                        onClick = {
                            val r = rect
                            val w = (r.right - r.left).coerceIn(0f, 1f)
                            val h = (r.bottom - r.top).coerceIn(0f, 1f)
                            // Full-frame selection => disable crop entirely.
                            val isFull = r.left <= 0.001f && r.top <= 0.001f &&
                                r.right >= 0.999f && r.bottom >= 0.999f
                            if (isFull) {
                                onConfirm(false, 0.5f to 0.5f, initialSize)
                            } else {
                                val cx = (r.left + r.right) / 2f
                                val cy = (r.top + r.bottom) / 2f
                                // size in fractions of the LONG side (see file header).
                                val sx = (w * imgW / maxDim)
                                val sy = (h * imgH / maxDim)
                                onConfirm(true, cx to cy, sx to sy)
                            }
                        },
                    ) {
                        Icon(SpectraIcons.Confirm, contentDescription = "Apply crop", tint = Color.White)
                    }
                }
            }

            // --- the image + crop rectangle ---
            Box(
                Modifier
                    .weight(1f)
                    .fillMaxWidth()
                    .padding(12.dp),
                contentAlignment = Alignment.Center,
            ) {
                Box(
                    Modifier
                        .aspectRatio(imageAspect)
                        .fillMaxWidth()
                        .onSizeChanged { canvasSize = it }
                        .pointerInput(aspect, canvasSize) {
                            detectDragGestures(
                                onDragStart = { pos ->
                                    handleRef.value = pickHandle(rect, pos, canvasSize)
                                },
                                onDragEnd = { handleRef.value = Handle.NONE },
                                onDragCancel = { handleRef.value = Handle.NONE },
                            ) { change, drag ->
                                change.consume()
                                val w = canvasSize.width.toFloat().coerceAtLeast(1f)
                                val h = canvasSize.height.toFloat().coerceAtLeast(1f)
                                val dx = drag.x / w
                                val dy = drag.y / h
                                rect = applyDrag(
                                    rect, handleRef.value, dx, dy,
                                    lockedAspect = resolveAspect(aspect, imageAspect),
                                    imgW = imgW, imgH = imgH,
                                )
                            }
                        },
                ) {
                    // image
                    Canvas(Modifier.fillMaxSize()) {
                        drawImageFit(image)
                        drawCropChrome(rect)
                    }
                }
            }

            // --- aspect-ratio preset chips ---
            Row(
                Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState())
                    .padding(horizontal = 12.dp, vertical = 10.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                CropAspect.entries.forEach { a ->
                    FilterChip(
                        selected = aspect == a,
                        onClick = {
                            aspect = a
                            val locked = resolveAspect(a, imageAspect)
                            if (locked != null) {
                                rect = constrainToAspect(rect, locked, imgW, imgH)
                            }
                        },
                        label = { Text(a.label) },
                    )
                }
            }
        }
    }
}

internal enum class Handle { NONE, MOVE, TL, TR, BL, BR, L, R, T, B }

/** Resolve an aspect preset to a concrete ratio (Original -> image aspect; Free -> null). */
private fun resolveAspect(a: CropAspect, imageAspect: Float): Float? = when (a) {
    CropAspect.FREE -> null
    CropAspect.ORIGINAL -> imageAspect
    else -> a.ratio
}

/** Hit-test which handle a press at [pos] (pixels in [canvas]) grabbed. */
private fun pickHandle(rect: Rect, pos: Offset, canvas: IntSize): Handle {
    if (canvas.width == 0 || canvas.height == 0) return Handle.MOVE
    val w = canvas.width.toFloat()
    val h = canvas.height.toFloat()
    val lx = rect.left * w; val rx = rect.right * w
    val ty = rect.top * h; val by = rect.bottom * h
    val tol = 48f
    fun near(a: Float, b: Float) = kotlin.math.abs(a - b) <= tol
    val nl = near(pos.x, lx); val nr = near(pos.x, rx)
    val nt = near(pos.y, ty); val nb = near(pos.y, by)
    return when {
        nl && nt -> Handle.TL
        nr && nt -> Handle.TR
        nl && nb -> Handle.BL
        nr && nb -> Handle.BR
        nl -> Handle.L
        nr -> Handle.R
        nt -> Handle.T
        nb -> Handle.B
        pos.x in lx..rx && pos.y in ty..by -> Handle.MOVE
        else -> Handle.MOVE
    }
}

/** Apply a normalized drag (dx, dy in 0..1) to [rect] for the active [handle]. */
private fun applyDrag(
    rect: Rect,
    handle: Handle,
    dx: Float,
    dy: Float,
    lockedAspect: Float?,
    imgW: Float,
    imgH: Float,
): Rect {
    val minSize = 0.05f
    var l = rect.left; var t = rect.top; var r = rect.right; var b = rect.bottom
    when (handle) {
        Handle.MOVE -> {
            val w = r - l; val h = b - t
            l = (l + dx).coerceIn(0f, 1f - w); r = l + w
            t = (t + dy).coerceIn(0f, 1f - h); b = t + h
        }
        Handle.L -> l = (l + dx).coerceIn(0f, r - minSize)
        Handle.R -> r = (r + dx).coerceIn(l + minSize, 1f)
        Handle.T -> t = (t + dy).coerceIn(0f, b - minSize)
        Handle.B -> b = (b + dy).coerceIn(t + minSize, 1f)
        Handle.TL -> { l = (l + dx).coerceIn(0f, r - minSize); t = (t + dy).coerceIn(0f, b - minSize) }
        Handle.TR -> { r = (r + dx).coerceIn(l + minSize, 1f); t = (t + dy).coerceIn(0f, b - minSize) }
        Handle.BL -> { l = (l + dx).coerceIn(0f, r - minSize); b = (b + dy).coerceIn(t + minSize, 1f) }
        Handle.BR -> { r = (r + dx).coerceIn(l + minSize, 1f); b = (b + dy).coerceIn(t + minSize, 1f) }
        Handle.NONE -> {}
    }
    var out = Rect(l, t, r, b)
    if (lockedAspect != null && handle != Handle.MOVE && handle != Handle.NONE) {
        out = constrainToAspect(out, lockedAspect, imgW, imgH, anchor = handle)
    }
    return out
}

/**
 * Re-fit [rect] to a target display aspect ratio (W:H in *pixels*). Because the
 * rectangle is stored in normalized image coords, the pixel aspect of the box is
 * (w * imgW) / (h * imgH); we adjust height to satisfy it, keeping it in bounds.
 */
internal fun constrainToAspect(
    rect: Rect,
    aspect: Float,
    imgW: Float,
    imgH: Float,
    anchor: Handle = Handle.BR,
): Rect {
    val w = rect.right - rect.left
    // desired normalized height so the pixel aspect (w*imgW)/(nh*imgH) == aspect.
    var nh = ((w * imgW / aspect) / imgH).coerceIn(0.05f, 1f)
    var nw = w
    // The corner OPPOSITE the dragged handle stays fixed and the box grows toward the
    // handle: a left/top edge drag keeps the right/bottom edge, and vice versa. (The old
    // code always kept the top-left corner, so TL/TR drags moved the wrong edges.)
    val leftFixed = anchor != Handle.TL && anchor != Handle.BL && anchor != Handle.L
    val topFixed = anchor != Handle.TL && anchor != Handle.TR && anchor != Handle.T
    // Clamp the height to the room past the fixed edge, shrinking width to keep the aspect.
    val availH = (if (topFixed) 1f - rect.top else rect.bottom).coerceAtLeast(0.05f)
    if (nh > availH) {
        nh = availH
        nw = ((nh * imgH * aspect) / imgW).coerceIn(0.05f, 1f)
    }
    var l = if (leftFixed) rect.left else rect.right - nw
    var t = if (topFixed) rect.top else rect.bottom - nh
    // Shift back into [0,1] without resizing.
    if (l < 0f) l = 0f
    if (t < 0f) t = 0f
    if (l + nw > 1f) l = (1f - nw).coerceAtLeast(0f)
    if (t + nh > 1f) t = (1f - nh).coerceAtLeast(0f)
    return Rect(l, t, (l + nw).coerceAtMost(1f), (t + nh).coerceAtMost(1f))
}

/** Draw the ImageBitmap to fill the whole DrawScope (the Box already has the aspect). */
private fun DrawScope.drawImageFit(image: ImageBitmap) {
    drawImage(
        image = image,
        dstSize = IntSize(size.width.roundToInt(), size.height.roundToInt()),
        dstOffset = IntOffset(0, 0),
    )
}

/** Dim outside the crop, draw the rule-of-thirds grid, the border and corner handles. */
private fun DrawScope.drawCropChrome(rect: Rect) {
    val w = size.width; val h = size.height
    val l = rect.left * w; val t = rect.top * h
    val r = rect.right * w; val b = rect.bottom * h
    val scrim = Color.Black.copy(alpha = 0.55f)
    // Dim the four exterior bands.
    drawRect(scrim, topLeft = Offset(0f, 0f), size = Size(w, t))
    drawRect(scrim, topLeft = Offset(0f, b), size = Size(w, h - b))
    drawRect(scrim, topLeft = Offset(0f, t), size = Size(l, b - t))
    drawRect(scrim, topLeft = Offset(r, t), size = Size(w - r, b - t))
    // Rule-of-thirds grid.
    val grid = Color.White.copy(alpha = 0.45f)
    val cw = (r - l); val ch = (b - t)
    for (i in 1..2) {
        val gx = l + cw * i / 3f
        drawLine(grid, Offset(gx, t), Offset(gx, b), strokeWidth = 1.5f)
        val gy = t + ch * i / 3f
        drawLine(grid, Offset(l, gy), Offset(r, gy), strokeWidth = 1.5f)
    }
    // Border.
    drawRect(
        Color.White, topLeft = Offset(l, t), size = Size(cw, ch),
        style = androidx.compose.ui.graphics.drawscope.Stroke(width = 2.5f),
    )
    // Corner handles (short L brackets).
    val hc = Color.White; val len = 26f; val sw = 5f
    fun corner(cx: Float, cy: Float, sx: Int, sy: Int) {
        drawLine(hc, Offset(cx, cy), Offset(cx + len * sx, cy), strokeWidth = sw)
        drawLine(hc, Offset(cx, cy), Offset(cx, cy + len * sy), strokeWidth = sw)
    }
    corner(l, t, 1, 1); corner(r, t, -1, 1); corner(l, b, 1, -1); corner(r, b, -1, -1)
}
