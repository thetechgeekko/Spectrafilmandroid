/*
 * Spektrafilm for Android — local-adjustment (mask) editor panel. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * The user-facing surface of the masking keystone: add a radial or gradient mask and limit the full
 * Tier-A adjustment set (exposure, temp/tint, saturation, hue, contrast, whites/blacks, clarity,
 * texture, sharpness, highlights, shadows) to one area. Edits update [ParamsState.localAdjustments],
 * which `simResultToBitmapGraded` composites on the engine OUTPUT (MaskCompositor) — the film render
 * + parity suite are untouched.
 *
 * v1 is slider-driven (position/size/feather + the adjustment), which is fully verifiable here; the
 * draw-on-the-preview gesture overlay + linear masks come next (gesture feel needs an on-device pass).
 * The panel edits the first component of each mask (masks it creates have exactly one radial); a
 * multi-component mask imported from a recipe still applies fully, the panel just edits its first shape.
 */
package com.spectrafilm.app

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.spectrafilm.app.masks.BlendMode
import com.spectrafilm.app.masks.ColorRange
import com.spectrafilm.app.masks.LocalAdjustment
import com.spectrafilm.app.masks.LuminanceRange
import com.spectrafilm.app.masks.Mask
import com.spectrafilm.app.masks.MaskComponent
import com.spectrafilm.app.masks.TierADelta

@Composable
fun MasksSection(
    s: ParamsState,
    selectedIndex: Int,
    onSelectedIndexChange: (Int) -> Unit,
    onEditOnPhoto: (Int) -> Unit = {},
    onSampleColor: (Int) -> Unit = {},
    onSampleLuminance: (Int) -> Unit = {},
) {
    var expanded by remember { mutableStateOf(true) }
    val masks = s.localAdjustments

    SectionCard("Masks", expanded, { expanded = it }) {
        Text(
            "Local adjustments: a mask limits the full adjustment set — exposure, temp/tint, " +
                "saturation, hue, contrast, whites/blacks, and the detail controls — to one area. A radial " +
                "targets a spot (brighten a face); a gradient ramps across the frame (darken a sky from " +
                "the top). Composited on the final image — the film render is untouched.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TextButton(onClick = {
                s.localAdjustments = masks + defaultRadialAdjustment()
                onSelectedIndexChange(masks.size)
            }) { Text("+ Radial mask") }
            TextButton(onClick = {
                s.localAdjustments = masks + defaultLinearAdjustment()
                onSelectedIndexChange(masks.size)
            }) { Text("+ Gradient mask") }
        }

        if (masks.isEmpty()) return@SectionCard

        val idx = clampSelectedMaskIndex(selectedIndex, masks.size)
        if (masks.size > 1) {
            SubTabRow(List(masks.size) { "Mask ${it + 1}" }, idx, onSelectedIndexChange)
        }
        val adj = masks[idx]
        fun set(updated: LocalAdjustment) {
            s.localAdjustments = masks.toMutableList().also { it[idx] = updated }
        }

        TextButton(onClick = { onEditOnPhoto(idx) }) { Text("Position on photo") }

        // --- Adjustment applied where the mask is opaque (Tier-A, pointwise on the output) ---
        EnhancedSlider("Temp", adj.delta.temp, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(temp = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = "White balance inside the mask: + warms (yellow), − cools (blue).")
        EnhancedSlider("Tint", adj.delta.tint, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(tint = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = "White balance inside the mask: + magenta, − green.")
        EnhancedSlider("Exposure", adj.delta.exposureEv, -4f..4f,
            { set(adj.copy(delta = adj.delta.copy(exposureEv = it))) },
            step = 0.05f, decimals = 2, default = 0f,
            tooltip = "Brighten / darken inside the mask, in stops (local dodge & burn).")
        EnhancedSlider("Saturation", adj.delta.saturation, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(saturation = it))) },
            step = 1f, decimals = 0, default = 0f, tooltip = "Colorfulness inside the mask.")
        EnhancedSlider("Hue shift", adj.delta.hue, -180f..180f,
            { set(adj.copy(delta = adj.delta.copy(hue = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = "Rotate the hue of colors inside the mask, in degrees (e.g. shift a sky toward teal).")
        EnhancedSlider("Contrast", adj.delta.contrast, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(contrast = it))) },
            step = 1f, decimals = 0, default = 0f, tooltip = "Contrast inside the mask.")
        EnhancedSlider("Whites", adj.delta.whites, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(whites = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = "The brightest tones inside the mask: + brightens highlights, − recovers them.")
        EnhancedSlider("Blacks", adj.delta.blacks, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(blacks = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = "The darkest tones inside the mask: + lifts shadows, − deepens them.")
        // Class-S spatial ops (edge-aware; a neighborhood blur on the output luma).
        EnhancedSlider("Clarity", adj.delta.clarity, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(clarity = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = "Midtone local contrast inside the mask (+ punch, − soften).")
        EnhancedSlider("Texture", adj.delta.texture, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(texture = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = "Fine detail inside the mask: + emphasizes, − smooths.")
        EnhancedSlider("Sharpness", adj.delta.sharpness, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(sharpness = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = "Edge sharpness inside the mask (unsharp mask on fine detail).")
        EnhancedSlider("Highlights", adj.delta.highlights, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(highlights = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = "Bright regions inside the mask: − recovers, + brightens.")
        EnhancedSlider("Shadows", adj.delta.shadows, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(shadows = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = "Dark regions inside the mask: + lifts, − deepens.")

        // --- Shape (radial: position / size / feather) ---
        val comp = adj.mask.components.firstOrNull()
        val radial = comp?.shape as? MaskComponent.Radial
        if (comp != null && radial != null) {
            fun setShape(r: MaskComponent.Radial) =
                set(adj.copy(mask = adj.mask.copy(components = listOf(comp.copy(shape = r)))))
            EnhancedSlider("Position X", radial.cx, 0f..1f, { setShape(radial.copy(cx = it)) },
                step = 0.01f, decimals = 2, default = 0.5f)
            EnhancedSlider("Position Y", radial.cy, 0f..1f, { setShape(radial.copy(cy = it)) },
                step = 0.01f, decimals = 2, default = 0.5f)
            EnhancedSlider("Size", radial.rx, 0.02f..1f, { setShape(radial.copy(rx = it, ry = it)) },
                step = 0.01f, decimals = 2, default = 0.3f, tooltip = "Radius of the mask (fraction of the frame).")
            EnhancedSlider("Feather", radial.feather, 0f..1f, { setShape(radial.copy(feather = it)) },
                step = 0.01f, decimals = 2, default = 0.5f, tooltip = "Softness of the mask edge.")
        }

        // --- Shape (gradient: the two endpoints the ramp runs between) ---
        val linear = comp?.shape as? MaskComponent.Linear
        if (comp != null && linear != null) {
            fun setShape(l: MaskComponent.Linear) =
                set(adj.copy(mask = adj.mask.copy(components = listOf(comp.copy(shape = l)))))
            Text(
                "The effect ramps from 0 at the start point to full at the end point.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            EnhancedSlider("Start X", linear.x0, 0f..1f, { setShape(linear.copy(x0 = it)) },
                step = 0.01f, decimals = 2, default = 0.5f, tooltip = "Where the gradient begins (no effect).")
            EnhancedSlider("Start Y", linear.y0, 0f..1f, { setShape(linear.copy(y0 = it)) },
                step = 0.01f, decimals = 2, default = 0.2f, tooltip = "Where the gradient begins (no effect).")
            EnhancedSlider("End X", linear.x1, 0f..1f, { setShape(linear.copy(x1 = it)) },
                step = 0.01f, decimals = 2, default = 0.5f, tooltip = "Where the gradient reaches full effect.")
            EnhancedSlider("End Y", linear.y1, 0f..1f, { setShape(linear.copy(y1 = it)) },
                step = 0.01f, decimals = 2, default = 0.8f, tooltip = "Where the gradient reaches full effect.")
        }

        SwitchRow("Invert (affect outside)", adj.mask.invert,
            { set(adj.copy(mask = adj.mask.copy(invert = it))) },
            "Apply the adjustment OUTSIDE the mask instead of inside (e.g. darken all but the subject).")
        EnhancedSlider("Mask opacity", adj.mask.opacity, 0f..1f,
            { set(adj.copy(mask = adj.mask.copy(opacity = it))) },
            step = 0.01f, decimals = 2, default = 1f)

        // --- Limit to a tonal range (luminance range mask) ---
        val lum = adj.mask.luminanceRange
        SwitchRow("Limit to tones", lum != null,
            { on -> set(adj.copy(mask = adj.mask.copy(luminanceRange = if (on) LuminanceRange() else null))) },
            "Restrict the adjustment to a brightness range — e.g. only the highlights, or only the " +
                "shadows. Turn on, then use the eyedropper to pick the tone from your photo.")
        if (lum != null) {
            fun setLum(r: LuminanceRange) = set(adj.copy(mask = adj.mask.copy(luminanceRange = r)))
            OutlinedButton(onClick = { onSampleLuminance(idx) }, modifier = Modifier.fillMaxWidth()) {
                Text("Eyedropper — pick a tone from the photo")
            }
            EnhancedSlider("Tone min", lum.lumMin, 0f..1f, { setLum(lum.copy(lumMin = it)) },
                step = 0.01f, decimals = 2, default = 0f, tooltip = "Darkest tone the adjustment affects.")
            EnhancedSlider("Tone max", lum.lumMax, 0f..1f, { setLum(lum.copy(lumMax = it)) },
                step = 0.01f, decimals = 2, default = 1f, tooltip = "Brightest tone the adjustment affects.")
            EnhancedSlider("Tone feather", lum.feather, 0.01f..0.5f, { setLum(lum.copy(feather = it)) },
                step = 0.01f, decimals = 2, default = 0.1f, tooltip = "Softness of the tonal range edges.")
            SwitchRow("Invert tones", lum.invert, { setLum(lum.copy(invert = it)) },
                "Affect the tones OUTSIDE the range instead.")
        }

        // --- Limit to a color (color range mask) — "tame the reds, not the skin" ---
        val col = adj.mask.colorRange
        SwitchRow("Limit to a color", col != null,
            { on -> set(adj.copy(mask = adj.mask.copy(colorRange = if (on) ColorRange() else null))) },
            "Restrict the adjustment to one color family — e.g. only the reds, leaving skin untouched. " +
                "Turn on, then use the eyedropper to pick the color from your photo.")
        if (col != null) {
            fun setCol(r: ColorRange) = set(adj.copy(mask = adj.mask.copy(colorRange = r)))
            OutlinedButton(onClick = { onSampleColor(idx) }, modifier = Modifier.fillMaxWidth()) {
                Text("Eyedropper — pick a color from the photo")
            }
            EnhancedSlider("Target red", col.targetR, 0f..1f, { setCol(col.copy(targetR = it)) },
                step = 0.01f, decimals = 2, default = 0.5f, tooltip = "The color to affect — red component.")
            EnhancedSlider("Target green", col.targetG, 0f..1f, { setCol(col.copy(targetG = it)) },
                step = 0.01f, decimals = 2, default = 0.5f, tooltip = "The color to affect — green component.")
            EnhancedSlider("Target blue", col.targetB, 0f..1f, { setCol(col.copy(targetB = it)) },
                step = 0.01f, decimals = 2, default = 0.5f, tooltip = "The color to affect — blue component.")
            EnhancedSlider("Color range", col.tolerance, 0.02f..1f, { setCol(col.copy(tolerance = it)) },
                step = 0.01f, decimals = 2, default = 0.6f, tooltip = "How wide a range of colors counts as a match.")
            EnhancedSlider("Color feather", col.feather, 0.01f..0.5f, { setCol(col.copy(feather = it)) },
                step = 0.01f, decimals = 2, default = 0.1f, tooltip = "Softness of the color-selection edges.")
            SwitchRow("Invert color", col.invert, { setCol(col.copy(invert = it)) },
                "Affect the colors OUTSIDE the range instead.")
        }

        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TextButton(onClick = {
                val remaining = masks.toMutableList().also { it.removeAt(idx) }
                s.localAdjustments = remaining
                onSelectedIndexChange(clampSelectedMaskIndex(idx, remaining.size))
            }) { Text("Delete mask") }
        }
    }
}

/** A centered radial mask with a no-op adjustment — the starting point the user then dials in. */
private fun defaultRadialAdjustment() = LocalAdjustment(
    Mask(listOf(Mask.Component(BlendMode.ADD, MaskComponent.Radial(0.5f, 0.5f, 0.3f, 0.3f, 0.5f)))),
    TierADelta(),
)

/** A top-to-bottom gradient mask with a no-op adjustment (a graduated filter the user then dials in). */
private fun defaultLinearAdjustment() = LocalAdjustment(
    Mask(listOf(Mask.Component(BlendMode.ADD, MaskComponent.Linear(0.5f, 0.2f, 0.5f, 0.8f)))),
    TierADelta(),
)
