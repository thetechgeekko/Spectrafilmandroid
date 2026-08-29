/*
 * Spektrafilm for Android — Settings screen. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * App-level settings persisted via AppSettings (SharedPreferences): default output
 * color space, preview resolution, default film/print profiles, export format +
 * quality, theme, a "Show onboarding again" action, a "Report an issue" entry (and a
 * "View issues" link), and an embedded About card. Edits write straight back to the
 * passed AppSettings and mirror into local Compose state so the controls update live.
 * Only theme is re-applied immediately (via onThemeChanged); the render defaults
 * (preview size, default profiles, output space) apply on the next launch.
 */
package com.spectrafilm.app

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import kotlinx.coroutines.launch
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.spectrafilm.engine.ColorSpace

/**
 * The Settings UI. [settings] is the live store; [onThemeChanged] lets the host re-apply
 * the theme immediately. [filmGroups]/[printGroups] are the catalog-grouped profile
 * options (may be empty before the engine is ready); [onShowOnboarding] re-launches the
 * welcome flow.
 */
@Composable
fun SettingsScreen(
    settings: AppSettings,
    filmGroups: List<DropdownGroup>,
    printGroups: List<DropdownGroup>,
    onThemeChanged: (ThemeMode) -> Unit,
    onShowOnboarding: () -> Unit,
    onOpenDiagnostics: () -> Unit = {},
) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    var updateStatus by remember { mutableStateOf<String?>(null) }
    var checking by remember { mutableStateOf(false) }
    var pendingUpdate by remember { mutableStateOf<UpdateInfo?>(null) }

    // Local mirrors of the persisted values so controls update without a recomposition key.
    var theme by remember { mutableStateOf(settings.theme) }
    var outputCs by remember { mutableStateOf(settings.defaultOutputColorSpace) }
    var previewSize by remember { mutableIntStateOf(settings.previewMaxSize) }
    var draftSize by remember { mutableIntStateOf(settings.draftRenderMaxPx) }
    var film by remember { mutableStateOf(settings.defaultFilmProfile) }
    var print by remember { mutableStateOf(settings.defaultPrintProfile) }
    var format by remember { mutableStateOf(settings.exportFormat) }
    var quality by remember { mutableIntStateOf(settings.exportQuality) }
    var keepGps by remember { mutableStateOf(settings.exportKeepGps) }
    var gpuPreview by remember { mutableStateOf(settings.gpuPreview) }
    var gpuEngine by remember { mutableStateOf(settings.gpuEngine) }
    var gpuExport by remember { mutableStateOf(settings.gpuExportEngine) }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("Settings", style = MaterialTheme.typography.headlineMedium)

        // --- Appearance ---
        SettingsCard("Appearance") {
            Dropdown(
                label = "Theme",
                selected = theme,
                options = ThemeMode.entries.toList(),
                display = { it.display },
                onSelect = { theme = it; settings.theme = it; onThemeChanged(it) },
            )
        }

        // --- Defaults ---
        SettingsCard("Render defaults") {
            Text(
                "Applied when the app starts. Per-image edits and presets still override these.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Dropdown(
                label = "Default output color space",
                selected = outputCs,
                options = ColorSpace.entries.toList(),
                display = { it.name },
                onSelect = { outputCs = it; settings.defaultOutputColorSpace = it },
            )
            IntSlider(
                label = "Preview max size",
                value = previewSize,
                range = 128..1024,
                onValueChange = { previewSize = it; settings.previewMaxSize = it },
                tooltip = "Long edge of the interactive preview, in pixels.",
            )
            IntSlider(
                label = "Draft render size",
                value = draftSize,
                range = 128..512,
                onValueChange = { draftSize = it; settings.draftRenderMaxPx = it },
                tooltip = "Long edge of the fast frame drawn while a slider is being " +
                    "dragged, before the crisp render lands. Lower tracks your finger " +
                    "more closely; higher previews the final result more faithfully.",
            )
            if (filmGroups.isNotEmpty()) {
                GroupedDropdown(
                    label = "Default film profile",
                    selectedId = film.ifEmpty { filmGroups.firstOrNull()?.options?.firstOrNull()?.id ?: "" },
                    groups = filmGroups,
                    onSelect = { film = it; settings.defaultFilmProfile = it },
                )
            }
            if (printGroups.isNotEmpty()) {
                GroupedDropdown(
                    label = "Default print profile",
                    selectedId = print.ifEmpty { printGroups.firstOrNull()?.options?.firstOrNull()?.id ?: "" },
                    groups = printGroups,
                    onSelect = { print = it; settings.defaultPrintProfile = it },
                )
            }
        }

        // --- Export ---
        SettingsCard("Export") {
            Dropdown(
                label = "Export format",
                selected = format,
                options = ExportFormat.entries.toList(),
                display = { it.display },
                onSelect = { format = it; settings.exportFormat = it },
            )
            if (format == ExportFormat.JPEG || format == ExportFormat.ULTRA_HDR) {
                IntSlider(
                    label = "JPEG quality",
                    value = quality,
                    range = 1..100,
                    onValueChange = { quality = it; settings.exportQuality = it },
                )
            }
            Row(
                modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("Preserve location (GPS)", style = MaterialTheme.typography.bodyLarge)
                    Text(
                        "Off: strip GPS from exports (recommended). Other EXIF (camera, " +
                            "exposure, date) is always kept.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Switch(
                    checked = keepGps,
                    onCheckedChange = { keepGps = it; settings.exportKeepGps = it },
                )
            }
        }

        // --- Experimental ---
        SettingsCard("Experimental") {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("GPU preview (experimental)", style = MaterialTheme.typography.bodyLarge)
                    Text(
                        "Render the fit preview instantly on the GPU (a 3D LUT of the current " +
                            "look) instead of a ~1s CPU render per edit. Export is always the " +
                            "exact CPU engine. Off by default: the current surface can stutter " +
                            "or overlap the controls while panels animate — being reworked.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Switch(
                    checked = gpuPreview,
                    onCheckedChange = { gpuPreview = it; settings.gpuPreview = it },
                )
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("GPU engine (Vulkan, experimental)", style = MaterialTheme.typography.bodyLarge)
                    Text(
                        "Run the film simulation's scan stage on the GPU for interactive " +
                            "previews — more accurate than the preview LUT and verified " +
                            "against the CPU engine on this device at first use, with " +
                            "automatic CPU fallback. Export always uses the exact CPU " +
                            "engine. Applies on the next return to the editor.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Switch(
                    checked = gpuEngine,
                    onCheckedChange = { gpuEngine = it; settings.gpuEngine = it },
                )
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("GPU export (Vulkan, experimental)", style = MaterialTheme.typography.bodyLarge)
                    Text(
                        "Also run the scan stage on the GPU for full-resolution exports — " +
                            "\"oracle-verified on your device\": the result is checked against " +
                            "the CPU engine on this device and falls back to CPU automatically " +
                            "if it ever drifts. Off by default. Independent of the preview toggle.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Switch(
                    checked = gpuExport,
                    onCheckedChange = { gpuExport = it; settings.gpuExportEngine = it },
                )
            }
        }

        // --- Updates & diagnostics ---
        SettingsCard("Updates & diagnostics") {
            Button(
                onClick = {
                    if (checking) return@Button
                    checking = true; updateStatus = "Checking…"
                    scope.launch {
                        val info = AppUpdater.checkForUpdate(ctx)
                        checking = false
                        when {
                            info == null -> updateStatus = "Couldn't check (no connection?)."
                            info.isNewer -> { pendingUpdate = info; updateStatus = "Update available: ${info.latestTag}" }
                            else -> updateStatus = "Up to date (${info.currentVersion})."
                        }
                    }
                },
                modifier = Modifier.fillMaxWidth(),
            ) { Text(if (checking) "Checking…" else "Check for updates") }
            updateStatus?.let {
                Text(it, style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            OutlinedButton(onClick = onOpenDiagnostics, modifier = Modifier.fillMaxWidth()) {
                Text("Diagnostics & logs")
            }
        }

        pendingUpdate?.let { info ->
            AlertDialog(
                onDismissRequest = { pendingUpdate = null },
                title = { Text("Update available") },
                text = {
                    Text("A newer version (${info.latestTag}) is available. You're on " +
                        "${info.currentVersion}. Open the release page to download the signed APK?")
                },
                confirmButton = {
                    TextButton(onClick = { AppUpdater.openRelease(ctx, info); pendingUpdate = null }) {
                        Text("Open release")
                    }
                },
                dismissButton = { TextButton(onClick = { pendingUpdate = null }) { Text("Later") } },
            )
        }

        // --- Help / feedback ---
        SettingsCard("Help & feedback") {
            Button(
                onClick = { Links.open(ctx, Links.NEW_ISSUE) },
                modifier = Modifier.fillMaxWidth(),
            ) { Text("Report an issue") }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(
                    onClick = { Links.open(ctx, Links.ISSUES) },
                    modifier = Modifier.weight(1f),
                ) { Text("View issues") }
                OutlinedButton(
                    onClick = { Links.open(ctx, Links.SOURCE) },
                    modifier = Modifier.weight(1f),
                ) { Text("Source") }
            }
            OutlinedButton(
                onClick = onShowOnboarding,
                modifier = Modifier.fillMaxWidth(),
            ) { Text("Show onboarding again") }
        }

        // --- About ---
        AboutCard()
    }
}

/** A simple always-expanded settings card (reuses the editor SectionCard look). */
@Composable
private fun SettingsCard(title: String, content: @Composable () -> Unit) {
    var expanded by remember { mutableStateOf(true) }
    SectionCard(title, expanded, { expanded = it }) { content() }
}
