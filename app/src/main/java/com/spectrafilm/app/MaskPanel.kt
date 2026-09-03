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
import androidx.compose.ui.res.stringResource
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

    SectionCard(stringResource(R.string.tool_masks_title), expanded, { expanded = it }) {
        Text(
            stringResource(R.string.tool_masks_intro),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TextButton(onClick = {
                s.localAdjustments = masks + defaultRadialAdjustment()
                onSelectedIndexChange(masks.size)
            }) { Text(stringResource(R.string.tool_masks_add_radial)) }
            TextButton(onClick = {
                s.localAdjustments = masks + defaultLinearAdjustment()
                onSelectedIndexChange(masks.size)
            }) { Text(stringResource(R.string.tool_masks_add_gradient)) }
        }

        if (masks.isEmpty()) return@SectionCard

        val idx = clampSelectedMaskIndex(selectedIndex, masks.size)
        if (masks.size > 1) {
            SubTabRow(List(masks.size) { stringResource(R.string.tool_masks_tab, it + 1) }, idx, onSelectedIndexChange)
        }
        val adj = masks[idx]
        fun set(updated: LocalAdjustment) {
            s.localAdjustments = masks.toMutableList().also { it[idx] = updated }
        }

        TextButton(onClick = { onEditOnPhoto(idx) }) { Text(stringResource(R.string.tool_masks_position_on_photo)) }

        // --- Adjustment applied where the mask is opaque (Tier-A, pointwise on the output) ---
        EnhancedSlider(stringResource(R.string.tool_masks_temp), adj.delta.temp, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(temp = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = stringResource(R.string.tool_masks_temp_help))
        EnhancedSlider(stringResource(R.string.tool_masks_tint), adj.delta.tint, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(tint = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = stringResource(R.string.tool_masks_tint_help))
        EnhancedSlider(stringResource(R.string.tool_masks_exposure), adj.delta.exposureEv, -4f..4f,
            { set(adj.copy(delta = adj.delta.copy(exposureEv = it))) },
            step = 0.05f, decimals = 2, default = 0f,
            tooltip = stringResource(R.string.tool_masks_exposure_help))
        EnhancedSlider(stringResource(R.string.tool_masks_saturation), adj.delta.saturation, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(saturation = it))) },
            step = 1f, decimals = 0, default = 0f, tooltip = stringResource(R.string.tool_masks_saturation_help))
        EnhancedSlider(stringResource(R.string.tool_masks_hue), adj.delta.hue, -180f..180f,
            { set(adj.copy(delta = adj.delta.copy(hue = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = stringResource(R.string.tool_masks_hue_help))
        EnhancedSlider(stringResource(R.string.tool_masks_contrast), adj.delta.contrast, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(contrast = it))) },
            step = 1f, decimals = 0, default = 0f, tooltip = stringResource(R.string.tool_masks_contrast_help))
        EnhancedSlider(stringResource(R.string.tool_masks_whites), adj.delta.whites, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(whites = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = stringResource(R.string.tool_masks_whites_help))
        EnhancedSlider(stringResource(R.string.tool_masks_blacks), adj.delta.blacks, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(blacks = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = stringResource(R.string.tool_masks_blacks_help))
        // Class-S spatial ops (edge-aware; a neighborhood blur on the output luma).
        EnhancedSlider(stringResource(R.string.tool_masks_clarity), adj.delta.clarity, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(clarity = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = stringResource(R.string.tool_masks_clarity_help))
        EnhancedSlider(stringResource(R.string.tool_masks_texture), adj.delta.texture, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(texture = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = stringResource(R.string.tool_masks_texture_help))
        EnhancedSlider(stringResource(R.string.tool_masks_sharpness), adj.delta.sharpness, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(sharpness = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = stringResource(R.string.tool_masks_sharpness_help))
        EnhancedSlider(stringResource(R.string.tool_masks_highlights), adj.delta.highlights, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(highlights = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = stringResource(R.string.tool_masks_highlights_help))
        EnhancedSlider(stringResource(R.string.tool_masks_shadows), adj.delta.shadows, -100f..100f,
            { set(adj.copy(delta = adj.delta.copy(shadows = it))) },
            step = 1f, decimals = 0, default = 0f,
            tooltip = stringResource(R.string.tool_masks_shadows_help))

        // --- Shape (radial: position / size / feather) ---
        val comp = adj.mask.components.firstOrNull()
        val radial = comp?.shape as? MaskComponent.Radial
        if (comp != null && radial != null) {
            fun setShape(r: MaskComponent.Radial) =
                set(adj.copy(mask = adj.mask.copy(components = listOf(comp.copy(shape = r)))))
            EnhancedSlider(stringResource(R.string.tool_masks_position_x), radial.cx, 0f..1f,
                { setShape(radial.copy(cx = it)) }, step = 0.01f, decimals = 2, default = 0.5f)
            EnhancedSlider(stringResource(R.string.tool_masks_position_y), radial.cy, 0f..1f,
                { setShape(radial.copy(cy = it)) }, step = 0.01f, decimals = 2, default = 0.5f)
            EnhancedSlider(stringResource(R.string.tool_masks_size), radial.rx, 0.02f..1f,
                { setShape(radial.copy(rx = it, ry = it)) },
                step = 0.01f, decimals = 2, default = 0.3f, tooltip = stringResource(R.string.tool_masks_size_help))
            EnhancedSlider(stringResource(R.string.tool_masks_feather), radial.feather, 0f..1f,
                { setShape(radial.copy(feather = it)) },
                step = 0.01f, decimals = 2, default = 0.5f, tooltip = stringResource(R.string.tool_masks_feather_help))
        }

        // --- Shape (gradient: the two endpoints the ramp runs between) ---
        val linear = comp?.shape as? MaskComponent.Linear
        if (comp != null && linear != null) {
            fun setShape(l: MaskComponent.Linear) =
                set(adj.copy(mask = adj.mask.copy(components = listOf(comp.copy(shape = l)))))
            Text(
                stringResource(R.string.tool_masks_gradient_note),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            val startHelp = stringResource(R.string.tool_masks_gradient_start_help)
            val endHelp = stringResource(R.string.tool_masks_gradient_end_help)
            EnhancedSlider(stringResource(R.string.tool_masks_start_x), linear.x0, 0f..1f,
                { setShape(linear.copy(x0 = it)) }, step = 0.01f, decimals = 2, default = 0.5f, tooltip = startHelp)
            EnhancedSlider(stringResource(R.string.tool_masks_start_y), linear.y0, 0f..1f,
                { setShape(linear.copy(y0 = it)) }, step = 0.01f, decimals = 2, default = 0.2f, tooltip = startHelp)
            EnhancedSlider(stringResource(R.string.tool_masks_end_x), linear.x1, 0f..1f,
                { setShape(linear.copy(x1 = it)) }, step = 0.01f, decimals = 2, default = 0.5f, tooltip = endHelp)
            EnhancedSlider(stringResource(R.string.tool_masks_end_y), linear.y1, 0f..1f,
                { setShape(linear.copy(y1 = it)) }, step = 0.01f, decimals = 2, default = 0.8f, tooltip = endHelp)
        }

        SwitchRow(stringResource(R.string.tool_masks_invert), adj.mask.invert,
            { set(adj.copy(mask = adj.mask.copy(invert = it))) },
            stringResource(R.string.tool_masks_invert_help))
        EnhancedSlider(stringResource(R.string.tool_masks_opacity), adj.mask.opacity, 0f..1f,
            { set(adj.copy(mask = adj.mask.copy(opacity = it))) },
            step = 0.01f, decimals = 2, default = 1f)

        // --- Limit to a tonal range (luminance range mask) ---
        val lum = adj.mask.luminanceRange
        SwitchRow(stringResource(R.string.tool_masks_limit_tones), lum != null,
            { on -> set(adj.copy(mask = adj.mask.copy(luminanceRange = if (on) LuminanceRange() else null))) },
            stringResource(R.string.tool_masks_limit_tones_help))
        if (lum != null) {
            fun setLum(r: LuminanceRange) = set(adj.copy(mask = adj.mask.copy(luminanceRange = r)))
            OutlinedButton(onClick = { onSampleLuminance(idx) }, modifier = Modifier.fillMaxWidth()) {
                Text(stringResource(R.string.tool_masks_eyedropper_tone))
            }
            EnhancedSlider(stringResource(R.string.tool_masks_tone_min), lum.lumMin, 0f..1f,
                { setLum(lum.copy(lumMin = it)) },
                step = 0.01f, decimals = 2, default = 0f, tooltip = stringResource(R.string.tool_masks_tone_min_help))
            EnhancedSlider(stringResource(R.string.tool_masks_tone_max), lum.lumMax, 0f..1f,
                { setLum(lum.copy(lumMax = it)) },
                step = 0.01f, decimals = 2, default = 1f, tooltip = stringResource(R.string.tool_masks_tone_max_help))
            EnhancedSlider(stringResource(R.string.tool_masks_tone_feather), lum.feather, 0.01f..0.5f,
                { setLum(lum.copy(feather = it)) },
                step = 0.01f, decimals = 2, default = 0.1f, tooltip = stringResource(R.string.tool_masks_tone_feather_help))
            SwitchRow(stringResource(R.string.tool_masks_invert_tones), lum.invert, { setLum(lum.copy(invert = it)) },
                stringResource(R.string.tool_masks_invert_tones_help))
        }

        // --- Limit to a color (color range mask) — "tame the reds, not the skin" ---
        val col = adj.mask.colorRange
        SwitchRow(stringResource(R.string.tool_masks_limit_color), col != null,
            { on -> set(adj.copy(mask = adj.mask.copy(colorRange = if (on) ColorRange() else null))) },
            stringResource(R.string.tool_masks_limit_color_help))
        if (col != null) {
            fun setCol(r: ColorRange) = set(adj.copy(mask = adj.mask.copy(colorRange = r)))
            OutlinedButton(onClick = { onSampleColor(idx) }, modifier = Modifier.fillMaxWidth()) {
                Text(stringResource(R.string.tool_masks_eyedropper_color))
            }
            EnhancedSlider(stringResource(R.string.tool_masks_target_red), col.targetR, 0f..1f,
                { setCol(col.copy(targetR = it)) },
                step = 0.01f, decimals = 2, default = 0.5f, tooltip = stringResource(R.string.tool_masks_target_red_help))
            EnhancedSlider(stringResource(R.string.tool_masks_target_green), col.targetG, 0f..1f,
                { setCol(col.copy(targetG = it)) },
                step = 0.01f, decimals = 2, default = 0.5f, tooltip = stringResource(R.string.tool_masks_target_green_help))
            EnhancedSlider(stringResource(R.string.tool_masks_target_blue), col.targetB, 0f..1f,
                { setCol(col.copy(targetB = it)) },
                step = 0.01f, decimals = 2, default = 0.5f, tooltip = stringResource(R.string.tool_masks_target_blue_help))
            EnhancedSlider(stringResource(R.string.tool_masks_color_range), col.tolerance, 0.02f..1f,
                { setCol(col.copy(tolerance = it)) },
                step = 0.01f, decimals = 2, default = 0.6f, tooltip = stringResource(R.string.tool_masks_color_range_help))
            EnhancedSlider(stringResource(R.string.tool_masks_color_feather), col.feather, 0.01f..0.5f,
                { setCol(col.copy(feather = it)) },
                step = 0.01f, decimals = 2, default = 0.1f, tooltip = stringResource(R.string.tool_masks_color_feather_help))
            SwitchRow(stringResource(R.string.tool_masks_invert_color), col.invert, { setCol(col.copy(invert = it)) },
                stringResource(R.string.tool_masks_invert_color_help))
        }

        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TextButton(onClick = {
                val remaining = masks.toMutableList().also { it.removeAt(idx) }
                s.localAdjustments = remaining
                onSelectedIndexChange(clampSelectedMaskIndex(idx, remaining.size))
            }) { Text(stringResource(R.string.tool_masks_delete)) }
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
