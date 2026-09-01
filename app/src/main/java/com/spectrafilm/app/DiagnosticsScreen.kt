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
    // Read the persisted crash off the main thread (file IO).
    LaunchedEffect(Unit) { crash = withContext(Dispatchers.IO) { Diagnostics.lastCrash(ctx) } }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("App: ${Diagnostics.appVersion(ctx)}", style = MaterialTheme.typography.bodyMedium)
        Text(
            "Nothing is uploaded automatically. Reports include app/device version, " +
                "redact paths, URIs and image metadata, and keep the last crash for at most 7 days. " +
                "Copy or Share is always an explicit action.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        // --- last crash ---
        Text("Last crash", style = MaterialTheme.typography.titleMedium)
        if (crash == null) {
            Text("No crash recorded.", style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        } else {
            MonoBlock(crash!!)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = { clipboard.setText(AnnotatedString(crash!!)) }) { Text("Copy") }
                OutlinedButton(onClick = {
                    scope.launch {
                        withContext(Dispatchers.IO) { Diagnostics.clearLastCrash(ctx) }
                        crash = null
                    }
                }) { Text("Clear") }
            }
        }

        // --- logcat snapshot ---
        Text("Logcat snapshot", style = MaterialTheme.typography.titleMedium)
        Button(onClick = {
            scope.launch { val l = withContext(Dispatchers.IO) { Diagnostics.captureLogcat() }; log = l }
        }, modifier = Modifier.fillMaxWidth()) {
            Text(if (log == null) "Capture logcat" else "Re-capture logcat")
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
        ) { Text("Share diagnostics report") }
    }
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
