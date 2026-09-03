/*
 * Spektrafilm for Android — in-app "How to use this app" guide. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * A self-contained, scrollable full-screen composable that walks a photographer
 * through every part of the app: what Spektrafilm is, loading a photo, the editing
 * layout, choosing a look, key tools, RAW white balance, non-destructive editing,
 * exporting, tips, and credits. Edge-to-edge safe via windowInsetsPadding(systemBars).
 *
 * Shown from two places:
 *   1. AboutScreen — via local `showHowTo` state inside that composable.
 *   2. WelcomeFlow — via local `showHowTo` state on the last onboarding page.
 * Neither caller needs a parameter change — no MainActivity dependency.
 */
package com.spectrafilm.app

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

/**
 * Full-screen "How to use this app" guide.
 *
 * Self-contained: no NavController, no MainActivity internals. The back affordance is
 * provided by the [onBack] lambda; callers (AboutScreen, WelcomeFlow) manage
 * show/hide via a local [androidx.compose.runtime.mutableStateOf] boolean.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HowToUseScreen(onBack: () -> Unit) {
    val ctx = LocalContext.current

    // System back dismisses the guide (mirrors the top-bar Back button) instead of
    // falling through to the host — which could pop the whole app from onboarding.
    BackHandler { onBack() }

    Surface(
        color = MaterialTheme.colorScheme.background,
        modifier = Modifier.fillMaxSize(),
    ) {
        Column(
            Modifier
                .fillMaxSize()
                .windowInsetsPadding(WindowInsets.systemBars),
        ) {
            TopAppBar(
                title = {
                    Text(
                        stringResource(R.string.screen_howto_title),
                        modifier = Modifier.semantics { heading() },
                    )
                },
                navigationIcon = {
                    TextButton(onClick = onBack) { Text(stringResource(R.string.screen_back)) }
                },
            )
            Column(
                Modifier
                    .weight(1f)
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = 16.dp, vertical = 8.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                HowToContent(ctx)
                Spacer(Modifier.height(24.dp))
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Main guide content (extracted so it can be tested in isolation if needed)
// ---------------------------------------------------------------------------

@Composable
private fun HowToContent(ctx: android.content.Context) {

    // --- 1. What Spektrafilm is ---
    GuideSection(
        icon = SpectraIcons.Simulation,
        title = stringResource(R.string.screen_howto_s1_title),
    ) {
        GuideBody(stringResource(R.string.screen_howto_s1_p1))
        GuideBody(stringResource(R.string.screen_howto_s1_p2))
        // Verbatim GPL attribution line (see the resource).
        AttributionNote(stringResource(R.string.screen_howto_attribution))
    }

    // --- 2. Getting a photo in ---
    GuideSection(
        icon = SpectraIcons.Input,
        title = stringResource(R.string.screen_howto_s2_title),
    ) {
        GuideStep(1, stringResource(R.string.screen_howto_s2_step1))
        GuideStep(2, stringResource(R.string.screen_howto_s2_step2))
        GuideStep(3, stringResource(R.string.screen_howto_s2_step3))
        GuideStep(4, stringResource(R.string.screen_howto_s2_step4))
        GuideBody(stringResource(R.string.screen_howto_s2_p1))
    }

    // --- 3. Editing screen layout ---
    GuideSection(
        icon = SpectraIcons.SourceImage,
        title = stringResource(R.string.screen_howto_s3_title),
    ) {
        GuideBody(stringResource(R.string.screen_howto_s3_p1))
        GuideStep(1, stringResource(R.string.screen_howto_s3_step1))
        GuideStep(2, stringResource(R.string.screen_howto_s3_step2))
        GuideStep(3, stringResource(R.string.screen_howto_s3_step3))
        GuideBody(stringResource(R.string.screen_howto_s3_p2))
        CategoryList()
        GuideBody(stringResource(R.string.screen_howto_s3_p3))
    }

    // --- 4. Choosing a look ---
    GuideSection(
        icon = SpectraIcons.Presets,
        title = stringResource(R.string.screen_howto_s4_title),
    ) {
        GuideBody(stringResource(R.string.screen_howto_s4_p1))
        GuideStep(1, stringResource(R.string.screen_howto_s4_step1))
        GuideStep(2, stringResource(R.string.screen_howto_s4_step2))
        GuideStep(3, stringResource(R.string.screen_howto_s4_step3))
        GuideStep(4, stringResource(R.string.screen_howto_s4_step4))
    }

    // --- 5. Key tools ---
    GuideSection(
        icon = SpectraIcons.Rotate,
        title = stringResource(R.string.screen_howto_s5_title),
    ) {
        GuideBody(stringResource(R.string.screen_howto_s5_p1))
        GuideStep(1, stringResource(R.string.screen_howto_s5_step1))
        GuideStep(2, stringResource(R.string.screen_howto_s5_step2))
        GuideStep(3, stringResource(R.string.screen_howto_s5_step3))
        GuideStep(4, stringResource(R.string.screen_howto_s5_step4))
        GuideStep(5, stringResource(R.string.screen_howto_s5_step5))
    }

    // --- 6. RAW white balance ---
    GuideSection(
        icon = SpectraIcons.ImportRaw,
        title = stringResource(R.string.screen_howto_s6_title),
    ) {
        GuideBody(stringResource(R.string.screen_howto_s6_p1))
        GuideStep(1, stringResource(R.string.screen_howto_s6_step1))
        GuideStep(2, stringResource(R.string.screen_howto_s6_step2))
        GuideStep(3, stringResource(R.string.screen_howto_s6_step3))
        GuideBody(stringResource(R.string.screen_howto_s6_p2))
    }

    // --- 7. Non-destructive editing ---
    GuideSection(
        icon = SpectraIcons.Settings,
        title = stringResource(R.string.screen_howto_s7_title),
    ) {
        GuideBody(stringResource(R.string.screen_howto_s7_p1))
        GuideStep(1, stringResource(R.string.screen_howto_s7_step1))
        GuideStep(2, stringResource(R.string.screen_howto_s7_step2))
        GuideStep(3, stringResource(R.string.screen_howto_s7_step3))
        GuideStep(4, stringResource(R.string.screen_howto_s7_step4))
    }

    // --- 8. Exporting ---
    GuideSection(
        icon = SpectraIcons.Display,
        title = stringResource(R.string.screen_howto_s8_title),
    ) {
        GuideBody(stringResource(R.string.screen_howto_s8_p1))
        GuideBody(stringResource(R.string.screen_howto_s8_p2))
        GuideStep(1, stringResource(R.string.screen_howto_s8_step1))
        GuideStep(2, stringResource(R.string.screen_howto_s8_step2))
        GuideStep(3, stringResource(R.string.screen_howto_s8_step3))
        GuideBody(stringResource(R.string.screen_howto_s8_p3))
    }

    // --- 9. Tips ---
    GuideSection(
        icon = SpectraIcons.Halation,
        title = stringResource(R.string.screen_howto_s9_title),
    ) {
        GuideStep(1, stringResource(R.string.screen_howto_s9_step1))
        GuideStep(2, stringResource(R.string.screen_howto_s9_step2))
        GuideStep(3, stringResource(R.string.screen_howto_s9_step3))
        GuideStep(4, stringResource(R.string.screen_howto_s9_step4))
        GuideStep(5, stringResource(R.string.screen_howto_s9_step5))
    }

    // --- 10. Credits / links ---
    GuideSection(
        icon = SpectraIcons.Glare,
        title = stringResource(R.string.screen_howto_s10_title),
    ) {
        // Carries the verbatim GPL line "Film modeling powered by spektrafilm".
        GuideBody(stringResource(R.string.screen_howto_s10_p1))
        GuideBody(stringResource(R.string.screen_dedication_pixls))
        GuideBody(stringResource(R.string.screen_howto_s10_p2))
        val opensInBrowser = stringResource(R.string.screen_opens_in_browser)
        // Label only: each button keeps its own click action (null action merges).
        val linkSemantics = Modifier.semantics { onClick(label = opensInBrowser, action = null) }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TextButton(onClick = { Links.open(ctx, Links.SPEKTRAFILM) }, modifier = linkSemantics) {
                Text(stringResource(R.string.screen_link_spektrafilm))
            }
            TextButton(onClick = { Links.open(ctx, Links.PIXLS) }, modifier = linkSemantics) {
                Text(stringResource(R.string.screen_link_pixls))
            }
            TextButton(onClick = { Links.open(ctx, Links.SOURCE) }, modifier = linkSemantics) {
                Text(stringResource(R.string.screen_link_source_code))
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Category list helper (inline in section 3)
// ---------------------------------------------------------------------------

/** (category name, one-line description) string-resource pairs, in category-bar order. */
private val CATEGORY_ROWS: List<Pair<Int, Int>> = listOf(
    R.string.screen_howto_cat_source to R.string.screen_howto_cat_source_desc,
    R.string.screen_howto_cat_presets to R.string.screen_howto_cat_presets_desc,
    R.string.screen_howto_cat_simulation to R.string.screen_howto_cat_simulation_desc,
    R.string.screen_howto_cat_input to R.string.screen_howto_cat_input_desc,
    R.string.screen_howto_cat_raw_wb to R.string.screen_howto_cat_raw_wb_desc,
    R.string.screen_howto_cat_grain to R.string.screen_howto_cat_grain_desc,
    R.string.screen_howto_cat_halation to R.string.screen_howto_cat_halation_desc,
    R.string.screen_howto_cat_glare to R.string.screen_howto_cat_glare_desc,
    R.string.screen_howto_cat_couplers to R.string.screen_howto_cat_couplers_desc,
    R.string.screen_howto_cat_preflash to R.string.screen_howto_cat_preflash_desc,
    R.string.screen_howto_cat_experimental to R.string.screen_howto_cat_experimental_desc,
    R.string.screen_howto_cat_display to R.string.screen_howto_cat_display_desc,
    R.string.screen_howto_cat_settings to R.string.screen_howto_cat_settings_desc,
)

@Composable
private fun CategoryList() {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        CATEGORY_ROWS.forEach { (nameRes, descriptionRes) ->
            Row(
                // Badge + description read as one item.
                modifier = Modifier.fillMaxWidth().semantics(mergeDescendants = true) {},
                verticalAlignment = Alignment.Top,
            ) {
                Surface(
                    shape = RoundedCornerShape(6.dp),
                    color = MaterialTheme.colorScheme.secondaryContainer,
                    modifier = Modifier.padding(top = 2.dp),
                ) {
                    Text(
                        stringResource(nameRes),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSecondaryContainer,
                        modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                    )
                }
                Text(
                    stringResource(R.string.screen_howto_category_line, stringResource(descriptionRes)),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(start = 4.dp),
                )
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Layout primitives
// ---------------------------------------------------------------------------

/** A guide section: a card with an icon + heading and a [content] column. */
@Composable
private fun GuideSection(
    icon: ImageVector,
    title: String,
    content: @Composable () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.35f),
        ),
    ) {
        Column(
            Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Icon(
                    imageVector = icon,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.size(22.dp),
                )
                Text(
                    title,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.semantics { heading() },
                )
            }
            content()
        }
    }
}

/** A numbered step inside a section. */
@Composable
private fun GuideStep(number: Int, text: String) {
    // The badge shows a bare digit; TalkBack hears "Step n" then the text, as one item.
    val stepLabel = stringResource(R.string.screen_howto_step_number, number)
    Row(
        modifier = Modifier.fillMaxWidth().semantics(mergeDescendants = true) {},
        verticalAlignment = Alignment.Top,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Surface(
            shape = RoundedCornerShape(10.dp),
            color = MaterialTheme.colorScheme.primary.copy(alpha = 0.15f),
            modifier = Modifier.padding(top = 1.dp),
        ) {
            Text(
                number.toString(),
                style = MaterialTheme.typography.labelSmall,
                fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.primary,
                modifier = Modifier
                    .padding(horizontal = 6.dp, vertical = 2.dp)
                    .semantics { contentDescription = stepLabel },
            )
        }
        Text(
            text,
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.weight(1f),
        )
    }
}

/** A paragraph of body text. */
@Composable
private fun GuideBody(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurface,
    )
}

/** A small attribution / note chip. */
@Composable
private fun AttributionNote(text: String) {
    Surface(
        shape = RoundedCornerShape(8.dp),
        color = MaterialTheme.colorScheme.tertiaryContainer,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Text(
            text,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onTertiaryContainer,
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
        )
    }
}
