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
import androidx.compose.foundation.selection.toggleable
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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.semantics
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

    val opensInBrowser = stringResource(R.string.screen_opens_in_browser)
    // Label only: each link button keeps its own click action (null action merges).
    val linkSemantics = Modifier.semantics { onClick(label = opensInBrowser, action = null) }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(
            stringResource(R.string.screen_settings_title),
            style = MaterialTheme.typography.headlineMedium,
            modifier = Modifier.semantics { heading() },
        )

        // --- Appearance ---
        SettingsCard(stringResource(R.string.screen_settings_appearance)) {
            Dropdown(
                label = stringResource(R.string.screen_settings_theme),
                selected = theme,
                options = ThemeMode.entries.toList(),
                display = { it.display },
                onSelect = { theme = it; settings.theme = it; onThemeChanged(it) },
            )
        }

        // --- Defaults ---
        SettingsCard(stringResource(R.string.screen_settings_render_defaults)) {
            Text(
                stringResource(R.string.screen_settings_render_defaults_note),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Dropdown(
                label = stringResource(R.string.screen_settings_default_output_cs),
                selected = outputCs,
                options = ColorSpace.entries.toList(),
                display = { it.name },
                onSelect = { outputCs = it; settings.defaultOutputColorSpace = it },
            )
            IntSlider(
                label = stringResource(R.string.screen_settings_preview_max_size),
                value = previewSize,
                range = 128..1024,
                onValueChange = { previewSize = it; settings.previewMaxSize = it },
                tooltip = stringResource(R.string.screen_settings_preview_max_size_tip),
            )
            IntSlider(
                label = stringResource(R.string.screen_settings_draft_size),
                value = draftSize,
                range = 128..512,
                onValueChange = { draftSize = it; settings.draftRenderMaxPx = it },
                tooltip = stringResource(R.string.screen_settings_draft_size_tip),
            )
            if (filmGroups.isNotEmpty()) {
                GroupedDropdown(
                    label = stringResource(R.string.screen_settings_default_film),
                    selectedId = film.ifEmpty { filmGroups.firstOrNull()?.options?.firstOrNull()?.id ?: "" },
                    groups = filmGroups,
                    onSelect = { film = it; settings.defaultFilmProfile = it },
                )
            }
            if (printGroups.isNotEmpty()) {
                GroupedDropdown(
                    label = stringResource(R.string.screen_settings_default_print),
                    selectedId = print.ifEmpty { printGroups.firstOrNull()?.options?.firstOrNull()?.id ?: "" },
                    groups = printGroups,
                    onSelect = { print = it; settings.defaultPrintProfile = it },
                )
            }
        }

        // --- Export ---
        SettingsCard(stringResource(R.string.screen_settings_export)) {
            Dropdown(
                label = stringResource(R.string.screen_settings_export_format),
                selected = format,
                options = ExportFormat.entries.toList(),
                display = { it.display },
                onSelect = { format = it; settings.exportFormat = it },
            )
            if (format == ExportFormat.JPEG || format == ExportFormat.ULTRA_HDR) {
                IntSlider(
                    label = stringResource(R.string.screen_settings_jpeg_quality),
                    value = quality,
                    range = 1..100,
                    onValueChange = { quality = it; settings.exportQuality = it },
                )
            }
            SettingToggleRow(
                title = stringResource(R.string.screen_settings_keep_gps),
                note = stringResource(R.string.screen_settings_keep_gps_note),
                checked = keepGps,
                onCheckedChange = { keepGps = it; settings.exportKeepGps = it },
                modifier = Modifier.padding(top = 8.dp),
            )
        }

        // --- Experimental ---
        SettingsCard(stringResource(R.string.screen_settings_experimental)) {
            SettingToggleRow(
                title = stringResource(R.string.screen_settings_gpu_preview),
                note = stringResource(R.string.screen_settings_gpu_preview_note),
                checked = gpuPreview,
                onCheckedChange = { gpuPreview = it; settings.gpuPreview = it },
            )
            SettingToggleRow(
                title = stringResource(R.string.screen_settings_gpu_engine),
                note = stringResource(R.string.screen_settings_gpu_engine_note),
                checked = gpuEngine,
                onCheckedChange = { gpuEngine = it; settings.gpuEngine = it },
            )
            SettingToggleRow(
                title = stringResource(R.string.screen_settings_gpu_export),
                note = stringResource(R.string.screen_settings_gpu_export_note),
                checked = gpuExport,
                onCheckedChange = { gpuExport = it; settings.gpuExportEngine = it },
            )
        }

        // --- Updates & diagnostics ---
        SettingsCard(stringResource(R.string.screen_settings_updates_heading)) {
            Button(
                onClick = {
                    if (checking) return@Button
                    checking = true; updateStatus = ctx.getString(R.string.screen_settings_checking)
                    scope.launch {
                        val info = AppUpdater.checkForUpdate(ctx)
                        checking = false
                        when {
                            info == null ->
                                updateStatus = ctx.getString(R.string.screen_settings_update_check_failed)
                            info.isNewer -> {
                                pendingUpdate = info
                                updateStatus = ctx.getString(
                                    R.string.screen_settings_update_available_tag, info.latestTag,
                                )
                            }
                            else ->
                                updateStatus =
                                    ctx.getString(R.string.screen_settings_up_to_date, info.currentVersion)
                        }
                    }
                },
                modifier = Modifier.fillMaxWidth(),
            ) {
                val labelRes =
                    if (checking) R.string.screen_settings_checking
                    else R.string.screen_settings_check_updates
                Text(stringResource(labelRes))
            }
            updateStatus?.let {
                Text(
                    it, style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
                )
            }
            OutlinedButton(onClick = onOpenDiagnostics, modifier = Modifier.fillMaxWidth()) {
                Text(stringResource(R.string.screen_settings_diagnostics_logs))
            }
        }

        pendingUpdate?.let { info ->
            AlertDialog(
                onDismissRequest = { pendingUpdate = null },
                title = { Text(stringResource(R.string.screen_settings_update_available)) },
                text = {
                    Text(
                        stringResource(
                            R.string.screen_settings_update_dialog_body, info.latestTag, info.currentVersion,
                        ),
                    )
                },
                confirmButton = {
                    TextButton(
                        onClick = { AppUpdater.openRelease(ctx, info); pendingUpdate = null },
                        modifier = linkSemantics,
                    ) {
                        Text(stringResource(R.string.screen_settings_open_release))
                    }
                },
                dismissButton = {
                    TextButton(onClick = { pendingUpdate = null }) {
                        Text(stringResource(R.string.screen_settings_later))
                    }
                },
            )
        }

        // --- Help / feedback ---
        SettingsCard(stringResource(R.string.screen_settings_help_heading)) {
            Button(
                onClick = { Links.open(ctx, Links.NEW_ISSUE) },
                modifier = Modifier.fillMaxWidth().then(linkSemantics),
            ) { Text(stringResource(R.string.screen_report_issue)) }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(
                    onClick = { Links.open(ctx, Links.ISSUES) },
                    modifier = Modifier.weight(1f).then(linkSemantics),
                ) { Text(stringResource(R.string.screen_settings_view_issues)) }
                OutlinedButton(
                    onClick = { Links.open(ctx, Links.SOURCE) },
                    modifier = Modifier.weight(1f).then(linkSemantics),
                ) { Text(stringResource(R.string.screen_settings_source)) }
            }
            OutlinedButton(
                onClick = onShowOnboarding,
                modifier = Modifier.fillMaxWidth(),
            ) { Text(stringResource(R.string.screen_settings_show_onboarding)) }
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

/**
 * A labelled switch row. The whole row is the toggle (`Role.Switch`), so TalkBack sees one
 * node — "title, note, switch, on/off" — and the tap target is the full row, not the thumb.
 */
@Composable
private fun SettingToggleRow(
    title: String,
    note: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .toggleable(value = checked, role = Role.Switch, onValueChange = onCheckedChange),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            Text(
                note,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        // The row owns the toggle; a null callback keeps the Switch from adding a second node.
        Switch(checked = checked, onCheckedChange = null)
    }
}
