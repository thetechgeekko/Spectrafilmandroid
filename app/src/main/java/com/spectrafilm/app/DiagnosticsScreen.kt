/*
 * Spektrafilm for Android — Diagnostics screen. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Shows a bounded, redacted crash record and an on-demand redacted logcat snapshot.
 * Nothing leaves the device until the user explicitly copies or shares it.
 */
package com.spectrafilm.app

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
fun DiagnosticsScreen() {
    val ctx = LocalContext.current
    val clipboard = LocalClipboardManager.current
    val scope = rememberCoroutineScope()
    var crash by remember { mutableStateOf<String?>(null) }
    var log by remember { mutableStateOf<String?>(null) }
    var engineCache by remember { mutableStateOf<String?>(null) }
    // Read the persisted crash off the main thread (file IO).
    LaunchedEffect(Unit) {
        crash = withContext(Dispatchers.IO) { Diagnostics.lastCrash(ctx) }
        engineCache = withContext(Dispatchers.Default) { Diagnostics.engineCacheSnapshot() }
    }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(
            stringResource(R.string.screen_diag_app_version, Diagnostics.appVersion(ctx)),
            style = MaterialTheme.typography.bodyMedium,
        )
        Text(
            stringResource(R.string.screen_diag_privacy_note),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        // --- last crash ---
        SectionHeading(stringResource(R.string.screen_diag_last_crash))
        val crashText = crash
        if (crashText == null) {
            Text(
                stringResource(R.string.screen_diag_no_crash), style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        } else {
            MonoBlock(crashText)
            // Short visible labels ("Copy"/"Clear") get a fuller spoken description of what they act on.
            val copyDesc = stringResource(R.string.screen_diag_copy_crash_desc)
            val clearDesc = stringResource(R.string.screen_diag_clear_crash_desc)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(
                    onClick = { clipboard.setText(AnnotatedString(crashText)) },
                    modifier = Modifier.semantics { contentDescription = copyDesc },
                ) { Text(stringResource(R.string.screen_diag_copy)) }
                OutlinedButton(
                    onClick = {
                        scope.launch {
                            withContext(Dispatchers.IO) { Diagnostics.clearLastCrash(ctx) }
                            crash = null
                        }
                    },
                    modifier = Modifier.semantics { contentDescription = clearDesc },
                ) { Text(stringResource(R.string.screen_diag_clear)) }
            }
        }

        // --- bounded native cache state ---
        SectionHeading(stringResource(R.string.screen_diag_cache_heading))
        Text(
            stringResource(R.string.screen_diag_cache_note),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        engineCache?.let { MonoBlock(it) }
            ?: Text(
                stringResource(R.string.screen_diag_cache_loading),
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
            )
        OutlinedButton(
            onClick = {
                scope.launch(Dispatchers.Default) {
                    val snapshot = Diagnostics.engineCacheSnapshot()
                    withContext(Dispatchers.Main) { engineCache = snapshot }
                }
            },
            modifier = Modifier.fillMaxWidth(),
        ) { Text(stringResource(R.string.screen_diag_refresh_cache)) }

        // --- logcat snapshot ---
        SectionHeading(stringResource(R.string.screen_diag_logcat_heading))
        Button(onClick = {
            scope.launch { val l = withContext(Dispatchers.IO) { Diagnostics.captureLogcat() }; log = l }
        }, modifier = Modifier.fillMaxWidth()) {
            val labelRes =
                if (log == null) R.string.screen_diag_capture_logcat
                else R.string.screen_diag_recapture_logcat
            Text(stringResource(labelRes))
        }
        log?.let { MonoBlock(it) }

        // --- share full report ---
        Button(
            onClick = {
                scope.launch {
                    val report = withContext(Dispatchers.IO) { Diagnostics.buildReport(ctx) }
                    Diagnostics.share(ctx, report)
                }
            },
            modifier = Modifier.fillMaxWidth(),
        ) { Text(stringResource(R.string.screen_diag_share_report)) }
    }
}

@Composable
private fun SectionHeading(text: String) {
    Text(text, style = MaterialTheme.typography.titleMedium, modifier = Modifier.semantics { heading() })
}

@Composable
private fun MonoBlock(text: String) {
    Surface(
        shape = RoundedCornerShape(10.dp),
        color = MaterialTheme.colorScheme.surfaceVariant,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Text(
            text,
            style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
            modifier = Modifier
                .horizontalScroll(rememberScrollState())
                .padding(10.dp),
        )
    }
}
