/*
 * Spektrafilm for Android — eyedropper overlay (sample a color from the photo). GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * A full-screen modal (mirroring MaskGeometryOverlay) to set a color-range mask's target by TAPPING the
 * photo instead of guessing R/G/B sliders — so "tame the reds" becomes "tap the red thing". Tap to move
 * a crosshair + see the live swatch; confirm to apply. The sampling math is the JVM-tested PixelSample;
 * this file is only rendering + the tap gesture + the one Bitmap.getPixel read.
 */
package com.spectrafilm.app

import android.graphics.Bitmap
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
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
import kotlin.math.roundToInt

/**
 * Sample an (r,g,b) color from [bitmap] (the live preview). [onPick] returns the chosen color in 0..1
 * plus the normalized tap location (nx,ny) — the latter lets a caller re-sample the engine INPUT at the
 * same spot (the WB eyedropper). [onCancel] discards. Tap the image to set the sample point, then confirm.
 */
@Composable
fun PixelSampleOverlay(
    bitmap: Bitmap,
    onPick: (Float, Float, Float, Float, Float) -> Unit,
    onCancel: () -> Unit,
    title: String = stringResource(R.string.tool_sample_title_color),
    hint: String = stringResource(R.string.tool_sample_hint_color),
) {
    val imageAspect = bitmap.width.toFloat().coerceAtLeast(1f) / bitmap.height.toFloat().coerceAtLeast(1f)
    val image: ImageBitmap = remember(bitmap) { bitmap.asImageBitmap() }

    var point by remember { mutableStateOf<Offset?>(null) }     // normalized 0..1 sample location
    var sampled by remember { mutableStateOf<Triple<Float, Float, Float>?>(null) }
    var canvasSize by remember { mutableStateOf(IntSize.Zero) }
    fun sampleAt(nx: Float, ny: Float) {
        point = Offset(nx, ny)
        val (px, py) = PixelSample.pixel(nx, ny, bitmap.width, bitmap.height)
        sampled = PixelSample.rgb01(bitmap.getPixel(px, py))
    }
    val canvasDesc = stringResource(R.string.tool_sample_canvas_desc)
    val sampleCenterLabel = stringResource(R.string.tool_sample_center)

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
                    title, color = Color.White,
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.padding(start = 4.dp).semantics { heading() },
                )
                Spacer(Modifier.weight(1f))
                // live swatch of the current sample (the colour is announced as 0..255 RGB)
                val s = sampled
                if (s != null) {
                    val swatchDesc = stringResource(
                        R.string.tool_sample_swatch_desc,
                        (s.first * 255f).roundToInt(),
                        (s.second * 255f).roundToInt(),
                        (s.third * 255f).roundToInt(),
                    )
                    Box(
                        Modifier.size(28.dp).clip(CircleShape)
                            .background(Color(s.first, s.second, s.third))
                            .padding(end = 4.dp)
                            .semantics { contentDescription = swatchDesc },
                    )
                    Spacer(Modifier.size(8.dp))
                }
                val applyLabel = stringResource(R.string.tool_apply)
                TextTooltip(applyLabel) {
                    IconButton(
                        onClick = { sampled?.let { val p = point; onPick(it.first, it.second, it.third, p?.x ?: 0.5f, p?.y ?: 0.5f) } },
                        enabled = sampled != null,
                    ) {
                        Icon(SpectraIcons.Confirm, contentDescription = applyLabel, tint = Color.White)
                    }
                }
            }

            Box(
                Modifier.weight(1f).fillMaxWidth().padding(12.dp),
                contentAlignment = Alignment.Center,
            ) {
                Box(
                    Modifier
                        .aspectRatio(imageAspect)
                        .fillMaxWidth()
                        .onSizeChanged { canvasSize = it }
                        .pointerInput(canvasSize) {
                            detectTapGestures { pos ->
                                val w = canvasSize.width.toFloat().coerceAtLeast(1f)
                                val h = canvasSize.height.toFloat().coerceAtLeast(1f)
                                sampleAt((pos.x / w).coerceIn(0f, 1f), (pos.y / h).coerceIn(0f, 1f))
                            }
                        }
                        // Tap-only canvas: expose a screen-reader path (sample the centre) alongside.
                        .semantics {
                            contentDescription = canvasDesc
                            customActions = listOf(
                                CustomAccessibilityAction(sampleCenterLabel) { sampleAt(0.5f, 0.5f); true },
                            )
                        },
                ) {
                    Canvas(Modifier.fillMaxSize()) {
                        drawImage(
                            image = image,
                            dstSize = IntSize(size.width.roundToInt(), size.height.roundToInt()),
                            dstOffset = IntOffset(0, 0),
                        )
                        point?.let { p ->
                            val c = Offset(p.x * size.width, p.y * size.height)
                            drawCircle(Color.White, radius = 16f, center = c, style = Stroke(width = 2.5f))
                            drawCircle(Color.Black, radius = 18f, center = c, style = Stroke(width = 1f))
                            drawLine(Color.White, Offset(c.x - 26f, c.y), Offset(c.x + 26f, c.y), strokeWidth = 1.5f)
                            drawLine(Color.White, Offset(c.x, c.y - 26f), Offset(c.x, c.y + 26f), strokeWidth = 1.5f)
                        }
                    }
                }
            }

            Text(
                hint,
                color = Color.White.copy(alpha = 0.7f),
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp),
            )
        }
    }
}
