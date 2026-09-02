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
import androidx.annotation.StringRes
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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
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
    @StringRes val titleRes: Int,
    val assetPath: String,
)

private val bundledLegalDocuments = listOf(
    LegalDocument(R.string.screen_about_doc_app_gpl, PROJECT_GPL_ASSET),
    LegalDocument(R.string.screen_about_doc_app_notices, PROJECT_NOTICE_ASSET),
    LegalDocument(R.string.screen_about_doc_libraw_copyright, LIBRAW_COPYRIGHT_ASSET),
    LegalDocument(R.string.screen_about_doc_libraw_lgpl, LIBRAW_LGPL_ASSET),
    LegalDocument(R.string.screen_about_doc_libraw_cddl, LIBRAW_CDDL_ASSET),
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
            Text(stringResource(R.string.screen_how_to_use_app))
        }
        AboutContent()
    }
}

/** About rendered as a collapsible card (embedded in Settings). */
@Composable
fun AboutCard() {
    var expanded by remember { mutableStateOf(false) }
    SectionCard(stringResource(R.string.screen_about_title), expanded, { expanded = it }) {
        AboutContent()
    }
}

/** An [AssistChip] that opens [url] in the browser; TalkBack announces the action as such. */
@Composable
private fun LinkChip(label: String, url: String) {
    val ctx = LocalContext.current
    val opensInBrowser = stringResource(R.string.screen_opens_in_browser)
    AssistChip(
        onClick = { Links.open(ctx, url) },
        label = { Text(label) },
        // Label only: the chip's own click action is kept (a null action merges).
        modifier = Modifier.semantics { onClick(label = opensInBrowser, action = null) },
    )
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun AboutContent() {
    val ctx = LocalContext.current
    var showOpenSourceNotices by remember { mutableStateOf(false) }

    if (showOpenSourceNotices) {
        OpenSourceNoticesDialog(onDismiss = { showOpenSourceNotices = false })
    }

    Text(
        stringResource(R.string.app_name),
        style = MaterialTheme.typography.headlineSmall,
        fontWeight = FontWeight.Bold,
        modifier = Modifier.semantics { heading() },
    )
    Text(
        stringResource(R.string.screen_about_version, appVersionName(ctx), appVersionCode(ctx)),
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )

    Text(
        stringResource(R.string.screen_about_tagline),
        style = MaterialTheme.typography.bodyMedium,
    )

    Text(
        stringResource(R.string.screen_about_dedication_heading),
        style = MaterialTheme.typography.titleSmall,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.semantics { heading() },
    )
    Text(
        stringResource(R.string.screen_dedication_pixls),
        style = MaterialTheme.typography.bodyMedium,
    )

    Text(
        stringResource(R.string.screen_about_thanks_heading),
        style = MaterialTheme.typography.titleSmall,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.semantics { heading() },
    )
    Text(
        stringResource(R.string.screen_about_thanks_body),
        style = MaterialTheme.typography.bodyMedium,
    )
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        LinkChip(stringResource(R.string.screen_link_spektrafilm), Links.SPEKTRAFILM)
        LinkChip(stringResource(R.string.screen_link_image_toolbox), Links.IMAGE_TOOLBOX)
        LinkChip(stringResource(R.string.screen_link_colour_science), Links.COLOUR_SCIENCE)
        LinkChip(stringResource(R.string.screen_link_libraw), Links.LIBRAW)
        LinkChip(stringResource(R.string.screen_link_pixls), Links.PIXLS)
    }

    Text(
        stringResource(R.string.screen_about_author_heading),
        style = MaterialTheme.typography.titleSmall,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.semantics { heading() },
    )
    Text(stringResource(R.string.screen_about_author_name), style = MaterialTheme.typography.bodyMedium)
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        LinkChip(stringResource(R.string.screen_link_instagram), Links.AUTHOR_INSTAGRAM)
        LinkChip(stringResource(R.string.screen_link_youtube), Links.AUTHOR_YOUTUBE)
    }

    Text(
        stringResource(R.string.screen_about_license_heading),
        style = MaterialTheme.typography.titleSmall,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.semantics { heading() },
    )
    Text(
        stringResource(R.string.screen_about_license_body),
        style = MaterialTheme.typography.bodyMedium,
    )
    Text(
        stringResource(R.string.screen_about_libraw_route_note, LIBRAW_ROUTE_STATUS),
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Button(
        onClick = { showOpenSourceNotices = true },
        modifier = Modifier.fillMaxWidth(),
    ) {
        Text(stringResource(R.string.screen_about_open_source_notices))
    }
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        LinkChip(stringResource(R.string.screen_link_source_code), Links.SOURCE)
        LinkChip(stringResource(R.string.screen_link_releases), Links.RELEASES)
        LinkChip(stringResource(R.string.screen_link_gplv3), Links.GPLV3)
        LinkChip(stringResource(R.string.screen_report_issue), Links.NEW_ISSUE)
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
                        stringResource(R.string.screen_about_open_source_notices),
                        style = MaterialTheme.typography.titleLarge,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier.semantics { heading() },
                    )
                    TextButton(onClick = onDismiss) {
                        Text(stringResource(R.string.screen_close))
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
                        stringResource(R.string.screen_about_notices_app_heading),
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier.semantics { heading() },
                    )
                    Text(
                        stringResource(R.string.screen_about_notices_app_body),
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    Text(
                        stringResource(R.string.screen_about_notices_libraw_heading),
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier.semantics { heading() },
                    )
                    Text(
                        stringResource(R.string.screen_about_notices_libraw_body),
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
                        LinkChip(stringResource(R.string.screen_link_libraw_source), Links.LIBRAW_SOURCE)
                        LinkChip(
                            stringResource(R.string.screen_link_libraw_archive), Links.LIBRAW_0222_ARCHIVE,
                        )
                        LinkChip(stringResource(R.string.screen_link_app_source), Links.SOURCE)
                        LinkChip(stringResource(R.string.screen_link_release_materials), Links.RELEASES)
                    }
                    Text(
                        stringResource(R.string.screen_about_bundled_docs_heading),
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier.semantics { heading() },
                    )
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        bundledLegalDocuments.forEach { document ->
                            FilterChip(
                                selected = selectedDocument == document,
                                onClick = { selectedDocument = document },
                                label = { Text(stringResource(document.titleRes)) },
                                // One-of-many document picker: announce as a radio choice.
                                modifier = Modifier.semantics { role = Role.RadioButton },
                            )
                        }
                    }
                    HorizontalDivider()
                    Text(
                        stringResource(selectedDocument.titleRes),
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier.semantics { heading() },
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
        context.getString(R.string.screen_about_legal_text_unavailable, Links.SOURCE)
    }
