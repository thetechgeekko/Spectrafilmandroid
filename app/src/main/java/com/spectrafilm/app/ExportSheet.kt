/*
 * Spektrafilm for Android — Lightroom-style export options sheet (§6a/§6b). GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * A format-aware export bottom sheet modeled on Lightroom mobile's export UI (RE'd from lrmobile:
 * format → format-specific options → dimensions → colour → naming → metadata), replacing the old
 * "export with whatever's in Settings" flow. UI only — the chosen [ExportOptions] drive the
 * post-engine encode in MainActivity, so there is no engine param and no parity impact.
 */
package com.spectrafilm.app

import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.spectrafilm.engine.ColorSpace

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ExportSheet(
    options: ExportOptions,
    onOptionsChange: (ExportOptions) -> Unit,
    colorSpace: ColorSpace,
    onColorSpaceChange: (ColorSpace) -> Unit,
    cctf: Boolean,
    onCctfChange: (Boolean) -> Unit,
    keepGps: Boolean,
    onKeepGpsChange: (Boolean) -> Unit,
    onDismiss: () -> Unit,
    onExport: () -> Unit,
) {
    // Dropdown's `display` is a plain (non-composable) lambda, so resolve via the context.
    val context = LocalContext.current
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(start = 20.dp, end = 20.dp, bottom = 28.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Text(
                stringResource(R.string.tool_export_title),
                style = MaterialTheme.typography.headlineSmall,
                modifier = Modifier.semantics { heading() },
            )
            val isSceneLinear = options.format == ExportFormat.SCENE_LINEAR_TIFF

            // --- Format (+ format-specific quality) ---
            Dropdown(
                label = stringResource(R.string.tool_export_format),
                selected = options.format,
                options = ExportFormat.entries.toList(),
                display = { context.getString(it.labelRes()) },
                onSelect = { onOptionsChange(options.copy(format = it)) },
            )
            if (options.format == ExportFormat.JPEG || options.format == ExportFormat.ULTRA_HDR) {
                IntSlider(
                    label = stringResource(R.string.tool_export_quality),
                    value = options.jpegQuality,
                    range = 10..100,
                    default = 90,
                    onValueChange = { onOptionsChange(options.copy(jpegQuality = it)) },
                )
            }
            if (isSceneLinear) {
                Text(
                    stringResource(R.string.tool_export_scene_linear_note),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            HorizontalDivider()

            // --- Dimensions (post-render downscale, like Lightroom) ---
            Text(
                stringResource(R.string.tool_export_size_heading),
                style = MaterialTheme.typography.titleSmall,
                modifier = Modifier.semantics { heading() },
            )
            if (options.format.isHighBitDepth()) {
                Text(
                    stringResource(R.string.tool_export_high_bit_depth_note),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            } else {
                Dropdown(
                    label = stringResource(R.string.tool_export_dimensions),
                    selected = options.size,
                    options = ExportSize.entries.toList(),
                    display = { context.getString(it.labelRes) },
                    onSelect = { onOptionsChange(options.copy(size = it)) },
                )
                if (options.size == ExportSize.CUSTOM) {
                    OutlinedTextField(
                        value = if (options.customLongEdge <= 0) "" else options.customLongEdge.toString(),
                        onValueChange = { v ->
                            onOptionsChange(options.copy(customLongEdge = v.filter(Char::isDigit).take(5).toIntOrNull() ?: 0))
                        },
                        label = { Text(stringResource(R.string.tool_export_long_edge)) },
                        singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Text(
                        stringResource(
                            R.string.tool_export_custom_clamp,
                            ExportOptions.MIN_CUSTOM_EDGE,
                            ExportOptions.MAX_CUSTOM_EDGE,
                        ),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }

            if (!isSceneLinear) {
                HorizontalDivider()

                // --- Colour ---
                Dropdown(
                    label = stringResource(R.string.tool_export_color_space),
                    selected = colorSpace,
                    options = ColorSpace.entries.toList(),
                    display = { context.getString(it.labelRes()) },
                    onSelect = onColorSpaceChange,
                )
                SwitchRow(
                    label = stringResource(R.string.tool_export_cctf),
                    checked = cctf,
                    onCheckedChange = onCctfChange,
                    tooltip = stringResource(R.string.tool_export_cctf_help),
                )
            }

            HorizontalDivider()

            // --- Naming + metadata ---
            OutlinedTextField(
                value = options.customName,
                onValueChange = { onOptionsChange(options.copy(customName = it)) },
                label = { Text(stringResource(R.string.tool_export_file_name)) },
                placeholder = { Text(stringResource(R.string.tool_export_file_name_placeholder)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            if (!isSceneLinear) {
                SwitchRow(
                    label = stringResource(R.string.tool_export_include_gps),
                    checked = keepGps,
                    onCheckedChange = onKeepGpsChange,
                    tooltip = stringResource(R.string.tool_export_include_gps_help),
                )
            }

            // --- Actions ---
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                OutlinedButton(onClick = onDismiss, modifier = Modifier.weight(1f)) {
                    Text(stringResource(R.string.tool_cancel))
                }
                Button(onClick = onExport, modifier = Modifier.weight(1f)) {
                    Text(stringResource(R.string.tool_export_action))
                }
            }

            Text(
                stringResource(R.string.tool_export_attribution),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f),
            )
        }
    }
}

/** Localised display name for an [ExportFormat] (the enum's `display` stays as the log/diagnostic id). */
@StringRes
private fun ExportFormat.labelRes(): Int = when (this) {
    ExportFormat.PNG -> R.string.tool_export_format_png
    ExportFormat.JPEG -> R.string.tool_export_format_jpeg
    ExportFormat.ULTRA_HDR -> R.string.tool_export_format_ultra_hdr
    ExportFormat.TIFF -> R.string.tool_export_format_tiff
    ExportFormat.PNG16 -> R.string.tool_export_format_png16
    ExportFormat.TIFF32F -> R.string.tool_export_format_tiff32f
    ExportFormat.SCENE_LINEAR_TIFF -> R.string.tool_export_format_scene_linear_tiff
}

/** Friendly label for an output [ColorSpace] in the export sheet. */
@StringRes
private fun ColorSpace.labelRes(): Int = when (this) {
    ColorSpace.SRGB -> R.string.tool_color_space_srgb
    ColorSpace.ADOBE_RGB -> R.string.tool_color_space_adobe_rgb
    ColorSpace.PROPHOTO -> R.string.tool_color_space_prophoto
    ColorSpace.REC2020 -> R.string.tool_color_space_rec2020
    ColorSpace.ACES2065_1 -> R.string.tool_color_space_aces2065_1
    ColorSpace.LINEAR_SRGB -> R.string.tool_color_space_linear_srgb
}
