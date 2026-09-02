/*
 * Spektrafilm for Android — draw-on-the-preview mask geometry editor. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * A full-screen modal (mirroring CropOverlay) to POSITION a mask on the photo instead of dialing
 * Position/Size sliders: drag the radial to move it, drag its right/bottom handle to resize; drag a
 * gradient's endpoints. Because the image fills a Box with the image's aspect ratio, normalized 0..1
 * geometry maps straight to box pixels — alignment is correct by construction (no zoom/pan to track).
 * All the coordinate/hit-test math is the JVM-tested MaskGesture; this file is only the rendering +
 * gesture wiring. Edits the mask's FIRST component (the panel convention), preserving the rest.
 */
package com.spectrafilm.app

import android.graphics.Bitmap
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
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
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import com.spectrafilm.app.masks.BlendMode
import com.spectrafilm.app.masks.Mask
import com.spectrafilm.app.masks.MaskComponent
import com.spectrafilm.app.masks.MaskGesture
import kotlin.math.roundToInt

/**
 * Position [mask] on [bitmap] (the live preview, used to draw + read aspect). [onConfirm] returns the
 * mask with its first component's geometry updated; [onCancel] discards.
 */
@Composable
fun MaskGeometryOverlay(
    bitmap: Bitmap,
    mask: Mask,
    onConfirm: (Mask) -> Unit,
    onCancel: () -> Unit,
) {
    val imageAspect = bitmap.width.toFloat().coerceAtLeast(1f) / bitmap.height.toFloat().coerceAtLeast(1f)
    val image: ImageBitmap = remember(bitmap) { bitmap.asImageBitmap() }

    // The geometry being edited: the first component's shape (defaulting to a centered radial).
    var shape by remember {
        mutableStateOf(mask.components.firstOrNull()?.shape ?: MaskComponent.Radial(0.5f, 0.5f, 0.3f, 0.3f, 0.5f))
    }
    val committedShape = mask.components.firstOrNull()?.shape
        ?: MaskComponent.Radial(0.5f, 0.5f, 0.3f, 0.3f, 0.5f)
    // Emitted only for an armed #139 oracle, after this real Composable owns its local draft.
    LaunchedEffect(Unit) {
        Ticket139EditorProbe.publishMaskDraftInitialized(
            bitmapWidth = bitmap.width,
            bitmapHeight = bitmap.height,
            committed = committedShape,
            actual = shape,
        )
    }
    var canvasSize by remember { mutableStateOf(IntSize.Zero) }
    val handleRef = remember { mutableStateOf(MaskGesture.Handle.NONE) }

    fun confirm() {
        val comps = mask.components
        val updated = if (comps.isEmpty()) listOf(Mask.Component(BlendMode.ADD, shape))
        else comps.toMutableList().also { it[0] = it[0].copy(shape = shape) }
        onConfirm(mask.copy(components = updated))
    }

    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.92f))
            .statusBarsPadding()
            .navigationBarsPadding(),
    ) {
        Column(Modifier.fillMaxSize()) {
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                val cancelLabel = stringResource(R.string.tool_cancel)
                TextTooltip(cancelLabel) {
                    IconButton(onClick = onCancel) {
                        Icon(SpectraIcons.Cancel, contentDescription = cancelLabel, tint = Color.White)
                    }
                }
                Text(
                    stringResource(R.string.tool_mask_geometry_title), color = Color.White,
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.padding(start = 4.dp).semantics { heading() },
                )
                Spacer(Modifier.weight(1f))
                val applyLabel = stringResource(R.string.tool_apply)
                TextTooltip(applyLabel) {
                    IconButton(onClick = { confirm() }) {
                        Icon(SpectraIcons.Confirm, contentDescription = applyLabel, tint = Color.White)
                    }
                }
            }

            val canvasDesc = stringResource(R.string.tool_mask_geometry_canvas_desc)
            val revertLabel = stringResource(R.string.tool_mask_geometry_revert)
            Box(
                Modifier.weight(1f).fillMaxWidth().padding(12.dp),
                contentAlignment = Alignment.Center,
            ) {
                Box(
                    Modifier
                        .aspectRatio(imageAspect)
                        .fillMaxWidth()
                        .onSizeChanged { canvasSize = it }
                        // Drag-only canvas: the Masks panel's position sliders are the accessible
                        // editor; here a screen reader can revert the draft to the saved shape.
                        .semantics {
                            contentDescription = canvasDesc
                            customActions = listOf(
                                CustomAccessibilityAction(revertLabel) { shape = committedShape; true },
                            )
                        }
                        .pointerInput(canvasSize) {
                            detectDragGestures(
                                onDragStart = { pos ->
                                    handleRef.value = MaskGesture.pick(
                                        shape, pos.x, pos.y, canvasSize.width, canvasSize.height,
                                    )
                                },
                                onDragEnd = { handleRef.value = MaskGesture.Handle.NONE },
                                onDragCancel = { handleRef.value = MaskGesture.Handle.NONE },
                            ) { change, drag ->
                                change.consume()
                                val w = canvasSize.width.toFloat().coerceAtLeast(1f)
                                val h = canvasSize.height.toFloat().coerceAtLeast(1f)
                                shape = MaskGesture.applyDrag(shape, handleRef.value, drag.x / w, drag.y / h)
                            }
                        },
                ) {
                    Canvas(Modifier.fillMaxSize()) {
                        drawImage(
                            image = image,
                            dstSize = IntSize(size.width.roundToInt(), size.height.roundToInt()),
                            dstOffset = IntOffset(0, 0),
                        )
                        drawMaskChrome(shape)
                    }
                }
            }

            Text(
                stringResource(R.string.tool_mask_geometry_hint),
                color = Color.White.copy(alpha = 0.7f),
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp),
            )
        }
    }
}

/** Draw the editable shape: a radial ellipse + its center/edge handles, or a gradient axis + endpoints. */
private fun DrawScope.drawMaskChrome(shape: MaskComponent) {
    val w = size.width; val h = size.height
    val line = Color.White
    val handle = Color(0xFF7FD1FF)  // a light accent for the draggable handles
    when (shape) {
        is MaskComponent.Radial -> {
            val cx = shape.cx * w; val cy = shape.cy * h
            val rx = shape.rx * w; val ry = shape.ry * h
            drawOval(
                line, topLeft = Offset(cx - rx, cy - ry), size = Size(2f * rx, 2f * ry),
                style = Stroke(width = 2.5f),
            )
            drawCircle(line, 7f, Offset(cx, cy))                 // center
            drawCircle(handle, 11f, Offset(cx + rx, cy))         // RX handle (right edge)
            drawCircle(handle, 11f, Offset(cx, cy + ry))         // RY handle (bottom edge)
        }
        is MaskComponent.Linear -> {
            val p0 = Offset(shape.x0 * w, shape.y0 * h)
            val p1 = Offset(shape.x1 * w, shape.y1 * h)
            drawLine(line, p0, p1, strokeWidth = 2.5f)
            drawCircle(handle, 11f, p0)
            drawCircle(handle, 11f, p1)
        }
    }
}
