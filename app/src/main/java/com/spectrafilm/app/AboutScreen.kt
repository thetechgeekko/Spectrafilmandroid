/*
 * Spektrafilm for Android — About section. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * The dedication, credits/thanks, author links, version, license and source. Rendered
 * either as its own scrollable screen (AboutScreen) or as a card embedded in Settings
 * (AboutCard). Links open via Links.open(). Self-contained Material3, no new dependency.
 */
package com.spectrafilm.app

import android.content.Context
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties

internal const val PROJECT_GPL_ASSET = "legal/spektrafilm/LICENSE.GPL-3.0"
internal const val PROJECT_NOTICE_ASSET = "legal/spektrafilm/NOTICE.md"
internal const val LIBRAW_DISTRIBUTION_ROUTE = "UNRESOLVED"
internal const val LIBRAW_ROUTE_STATUS =
    "LibRaw Android distribution route: UNRESOLVED."
private const val LIBRAW_COPYRIGHT_ASSET = "third_party/libraw/COPYRIGHT"
private const val LIBRAW_LGPL_ASSET = "third_party/libraw/LICENSE.LGPL"
private const val LIBRAW_CDDL_ASSET = "third_party/libraw/LICENSE.CDDL"

internal const val LIBRAW_LICENSE_STATUS =
    "LibRaw 0.22.2 is offered upstream under a choice of the GNU Lesser General Public " +
        "License version 2.1 or the Common Development and Distribution License version 1.0. " +
        "Spektrafilm Android's release route has not been selected. Bundling both upstream " +
        "license texts records provenance and does not elect either route."

private data class LegalDocument(
    val title: String,
    val assetPath: String,
)

private val bundledLegalDocuments = listOf(
    LegalDocument("App GPLv3", PROJECT_GPL_ASSET),
    LegalDocument("App notices", PROJECT_NOTICE_ASSET),
    LegalDocument("LibRaw copyright", LIBRAW_COPYRIGHT_ASSET),
    LegalDocument("LibRaw LGPL 2.1", LIBRAW_LGPL_ASSET),
    LegalDocument("LibRaw CDDL 1.0", LIBRAW_CDDL_ASSET),
)

/** Full-screen About, used when reached from the top-bar / Settings. */
@Composable
fun AboutScreen() {
    // Show the How-To guide over this screen when the user taps the button.
    // Local state: no MainActivity or NavController dependency.
    var showHowTo by remember { mutableStateOf(false) }

    if (showHowTo) {
        HowToUseScreen(onBack = { showHowTo = false })
        return
    }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        // Prominent "How to use this app" entry point at the top of About.
        Button(
            onClick = { showHowTo = true },
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("How to use this app")
        }
        AboutContent()
    }
}

/** About rendered as a collapsible card (embedded in Settings). */
@Composable
fun AboutCard() {
    var expanded by remember { mutableStateOf(false) }
    SectionCard("About", expanded, { expanded = it }) {
        AboutContent()
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun AboutContent() {
    val ctx = LocalContext.current
    var showOpenSourceNotices by remember { mutableStateOf(false) }

    if (showOpenSourceNotices) {
        OpenSourceNoticesDialog(onDismiss = { showOpenSourceNotices = false })
    }

    Text("Spektrafilm", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
    Text(
        "Version ${appVersionName(ctx)} (${appVersionCode(ctx)})",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )

    Text(
        "Spectral film simulation on your phone — film modeling powered by spektrafilm.",
        style = MaterialTheme.typography.bodyMedium,
    )

    Text("Dedication", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
    Text(
        "Dedicated to the pixls.us community.",
        style = MaterialTheme.typography.bodyMedium,
    )

    Text("Thanks", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
    Text(
        "Film modeling powered by spektrafilm (Andrea Volpato). UI inspired by Image " +
            "Toolbox (T8RIN). Colour math from colour-science. RAW decoding via LibRaw.",
        style = MaterialTheme.typography.bodyMedium,
    )
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        AssistChip(onClick = { Links.open(ctx, Links.SPEKTRAFILM) }, label = { Text("spektrafilm") })
        AssistChip(onClick = { Links.open(ctx, Links.IMAGE_TOOLBOX) }, label = { Text("Image Toolbox") })
        AssistChip(onClick = { Links.open(ctx, Links.COLOUR_SCIENCE) }, label = { Text("colour-science") })
        AssistChip(onClick = { Links.open(ctx, Links.LIBRAW) }, label = { Text("LibRaw") })
        AssistChip(onClick = { Links.open(ctx, Links.PIXLS) }, label = { Text("pixls.us") })
    }

    Text("Author", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
    Text("Akshay", style = MaterialTheme.typography.bodyMedium)
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        AssistChip(onClick = { Links.open(ctx, Links.AUTHOR_INSTAGRAM) }, label = { Text("Instagram") })
        AssistChip(onClick = { Links.open(ctx, Links.AUTHOR_YOUTUBE) }, label = { Text("YouTube") })
    }

    Text("License & source", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
    Text(
        "App code is free software released under GPL-3.0-only.",
        style = MaterialTheme.typography.bodyMedium,
    )
    Text(
        "$LIBRAW_ROUTE_STATUS Both original license texts are bundled for offline review.",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Button(
        onClick = { showOpenSourceNotices = true },
        modifier = Modifier.fillMaxWidth(),
    ) {
        Text("Open-source notices")
    }
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        AssistChip(onClick = { Links.open(ctx, Links.SOURCE) }, label = { Text("Source code") })
        AssistChip(onClick = { Links.open(ctx, Links.RELEASES) }, label = { Text("Releases") })
        AssistChip(onClick = { Links.open(ctx, Links.GPLV3) }, label = { Text("GPLv3") })
        AssistChip(onClick = { Links.open(ctx, Links.NEW_ISSUE) }, label = { Text("Report an issue") })
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun OpenSourceNoticesDialog(onDismiss: () -> Unit) {
    val ctx = LocalContext.current
    var selectedDocument by remember { mutableStateOf(bundledLegalDocuments.first()) }
    val documentText = remember(ctx, selectedDocument.assetPath) {
        readBundledLegalText(ctx, selectedDocument.assetPath)
    }

    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false),
    ) {
        Surface(modifier = Modifier.fillMaxSize()) {
            Column(modifier = Modifier.fillMaxSize()) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 8.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        "Open-source notices",
                        style = MaterialTheme.typography.titleLarge,
                        fontWeight = FontWeight.Bold,
                    )
                    TextButton(onClick = onDismiss) {
                        Text("Close")
                    }
                }
                HorizontalDivider()
                Column(
                    modifier = Modifier
                        .fillMaxSize()
                        .verticalScroll(rememberScrollState())
                        .padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Text(
                        "Spektrafilm Android",
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                    )
                    Text(
                        "The app source is distributed under GPL-3.0-only. Its complete license " +
                            "and project notices are available below without a network connection.",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    Text(
                        "LibRaw 0.22.2",
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                    )
                    Text(
                        "Copyright (C) 2008-2025 LibRaw LLC. RAW/DNG decoding in this app " +
                            "statically includes the pinned and locally patched LibRaw source.",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    Text(
                        LIBRAW_LICENSE_STATUS,
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    Text(
                        LIBRAW_ROUTE_STATUS,
                        style = MaterialTheme.typography.bodyMedium,
                        fontWeight = FontWeight.SemiBold,
                    )
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        AssistChip(
                            onClick = { Links.open(ctx, Links.LIBRAW_SOURCE) },
                            label = { Text("LibRaw source") },
                        )
                        AssistChip(
                            onClick = { Links.open(ctx, Links.LIBRAW_0222_ARCHIVE) },
                            label = { Text("0.22.2 archive") },
                        )
                        AssistChip(
                            onClick = { Links.open(ctx, Links.SOURCE) },
                            label = { Text("App source") },
                        )
                        AssistChip(
                            onClick = { Links.open(ctx, Links.RELEASES) },
                            label = { Text("Release materials") },
                        )
                    }
                    Text(
                        "Bundled legal documents",
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.SemiBold,
                    )
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        bundledLegalDocuments.forEach { document ->
                            FilterChip(
                                selected = selectedDocument == document,
                                onClick = { selectedDocument = document },
                                label = { Text(document.title) },
                            )
                        }
                    }
                    HorizontalDivider()
                    Text(
                        selectedDocument.title,
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.SemiBold,
                    )
                    SelectionContainer {
                        Text(
                            documentText,
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                        )
                    }
                }
            }
        }
    }
}

private fun readBundledLegalText(context: Context, assetPath: String): String =
    runCatching {
        context.assets.open(assetPath).bufferedReader(Charsets.UTF_8).use { it.readText() }
    }.getOrElse {
        "The bundled legal text could not be opened. See ${Links.SOURCE} for the canonical source."
    }
