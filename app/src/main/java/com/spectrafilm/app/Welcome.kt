/*
 * Spektrafilm for Android — animated welcome / onboarding. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * A multi-page first-run flow built on a HorizontalPager with a film + spectrum theme:
 *   (i)   hero / intro
 *   (ii)  how it works (pick photo/RAW → choose film + print → tune → export)
 *   (iii) what's under the hood (the bit-exact spektrafilm port + LibRaw + ITbox UI)
 *   (iv)  credits + "Get started" — plus a quick link to Settings and Report-an-issue.
 *
 * Pages animate (an alpha/slide-in per page on selection, plus a slow-shifting spectral
 * gradient backdrop). Shown only on first launch (gated by AppSettings.seenOnboarding);
 * re-openable from Settings/About. Self-contained: no new gradle dependency.
 */
package com.spectrafilm.app

import androidx.annotation.StringRes
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch

/** The spectral palette used across the onboarding backdrop and accents. */
private val SPECTRUM = listOf(
    Color(0xFF7B2FF7), // violet
    Color(0xFF2F6BFF), // blue
    Color(0xFF14C7C7), // cyan
    Color(0xFF34C759), // green
    Color(0xFFFFCC00), // yellow
    Color(0xFFFF8A00), // orange
    Color(0xFFFF3B30), // red
)

private data class OnboardPage(
    @StringRes val titleRes: Int,
    @StringRes val bodyRes: Int,
    val accent: Color,
)

private val PAGES = listOf(
    OnboardPage(R.string.screen_welcome_page1_title, R.string.screen_welcome_page1_body, SPECTRUM[0]),
    OnboardPage(R.string.screen_welcome_page2_title, R.string.screen_welcome_page2_body, SPECTRUM[2]),
    OnboardPage(R.string.screen_welcome_page3_title, R.string.screen_welcome_page3_body, SPECTRUM[4]),
    // The credits body carries the verbatim GPL line "Film modeling powered by spektrafilm".
    OnboardPage(R.string.screen_welcome_page4_title, R.string.screen_welcome_page4_body, SPECTRUM[6]),
)

/**
 * Full-screen onboarding pager. [onFinish] is invoked when the user taps "Get started"
 * (or "Skip"); the host should then mark onboarding seen and dismiss this. The last page
 * exposes quick links to Settings and to filing an issue via [onOpenSettings] /
 * [onReportIssue], plus a "How to use this app" button managed via local state so no
 * new parameter (and therefore no MainActivity change) is needed.
 */
@Composable
fun WelcomeFlow(
    onFinish: () -> Unit,
    onOpenSettings: () -> Unit,
    onReportIssue: () -> Unit,
) {
    // Show the How-To guide over the welcome flow when the user taps the button.
    // Local state: no new parameter, no MainActivity dependency.
    var showHowTo by remember { mutableStateOf(false) }

    if (showHowTo) {
        HowToUseScreen(onBack = { showHowTo = false })
        return
    }

    val pagerState = rememberPagerState(pageCount = { PAGES.size })
    val scope = rememberCoroutineScope()

    // Slowly drifting spectral gradient backdrop — held static when the user has turned
    // system animations off (reduced motion), since an infinite transition never settles.
    val ctx = LocalContext.current
    val reduceMotion = remember(ctx) {
        android.provider.Settings.Global.getFloat(
            ctx.contentResolver, android.provider.Settings.Global.ANIMATOR_DURATION_SCALE, 1f,
        ) == 0f
    }
    val shift = if (reduceMotion) 0f else {
        rememberInfiniteTransition(label = "spectrum").animateFloat(
            initialValue = 0f,
            targetValue = 1f,
            animationSpec = infiniteRepeatable(
                animation = tween(durationMillis = 9000, easing = LinearEasing),
            ),
            label = "shift",
        ).value
    }
    val pageIndicator = stringResource(
        R.string.screen_welcome_page_indicator, pagerState.currentPage + 1, PAGES.size,
    )

    Box(
        Modifier
            .fillMaxSize()
            .background(
                Brush.linearGradient(
                    colors = listOf(
                        Color(0xFF0A0A12),
                        SPECTRUM[(shift * SPECTRUM.size).toInt() % SPECTRUM.size].copy(alpha = 0.30f),
                        Color(0xFF0A0A12),
                    ),
                    start = Offset(0f, shift * 1200f),
                    end = Offset(1200f, (1f - shift) * 1200f),
                ),
            ),
    ) {
        Column(Modifier.fillMaxSize().padding(24.dp)) {
            // Skip (hidden on the final page where "Get started" takes over).
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                if (pagerState.currentPage < PAGES.lastIndex) {
                    TextButton(onClick = onFinish) {
                        Text(
                            stringResource(R.string.screen_welcome_skip),
                            color = Color.White.copy(alpha = 0.85f),
                        )
                    }
                } else {
                    Spacer(Modifier.height(48.dp))
                }
            }

            HorizontalPager(
                state = pagerState,
                modifier = Modifier.weight(1f).fillMaxWidth(),
            ) { page ->
                val selected = pagerState.currentPage == page
                val pageAlpha by animateFloatAsState(
                    targetValue = if (selected) 1f else 0.25f,
                    animationSpec = tween(450),
                    label = "pageAlpha",
                )
                OnboardingPageContent(
                    page = PAGES[page],
                    isLast = page == PAGES.lastIndex,
                    modifier = Modifier.alpha(pageAlpha),
                    onOpenSettings = onOpenSettings,
                    onReportIssue = onReportIssue,
                    onOpenHowTo = { showHowTo = true },
                )
            }

            // Page indicator dots (colour-only, so the row announces "Page n of m").
            Row(
                Modifier
                    .fillMaxWidth()
                    .padding(vertical = 16.dp)
                    .semantics { contentDescription = pageIndicator },
                horizontalArrangement = Arrangement.Center,
            ) {
                repeat(PAGES.size) { i ->
                    val active = pagerState.currentPage == i
                    val dotAlpha by animateFloatAsState(
                        if (active) 1f else 0.35f, label = "dot",
                    )
                    Box(
                        Modifier
                            .padding(horizontal = 4.dp)
                            .size(if (active) 10.dp else 8.dp)
                            .clip(CircleShape)
                            .alpha(dotAlpha)
                            .background(if (active) PAGES[i].accent else Color.White),
                    )
                }
            }

            // Navigation: Back / Next, or Get started on the last page.
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (pagerState.currentPage > 0) {
                    OutlinedButton(
                        onClick = { scope.launch { pagerState.animateScrollToPage(pagerState.currentPage - 1) } },
                        modifier = Modifier.weight(1f),
                    ) { Text(stringResource(R.string.screen_back)) }
                }
                if (pagerState.currentPage < PAGES.lastIndex) {
                    Button(
                        onClick = { scope.launch { pagerState.animateScrollToPage(pagerState.currentPage + 1) } },
                        modifier = Modifier.weight(1f),
                    ) { Text(stringResource(R.string.screen_welcome_next)) }
                } else {
                    Button(onClick = onFinish, modifier = Modifier.weight(1f)) {
                        Text(stringResource(R.string.screen_welcome_get_started))
                    }
                }
            }
        }
    }
}

@Composable
private fun OnboardingPageContent(
    page: OnboardPage,
    isLast: Boolean,
    modifier: Modifier = Modifier,
    onOpenSettings: () -> Unit,
    onReportIssue: () -> Unit,
    onOpenHowTo: () -> Unit,
) {
    Column(
        modifier = modifier.fillMaxSize().padding(8.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        // A spectral "film strip" emblem.
        Box(
            Modifier
                .fillMaxWidth(0.7f)
                .height(14.dp)
                .clip(RoundedCornerShape(7.dp))
                .background(Brush.horizontalGradient(SPECTRUM)),
        )
        Spacer(Modifier.height(36.dp))
        Text(
            stringResource(page.titleRes),
            style = MaterialTheme.typography.headlineMedium,
            fontWeight = FontWeight.Bold,
            color = Color.White,
            textAlign = TextAlign.Center,
            modifier = Modifier.semantics { heading() },
        )
        Spacer(Modifier.height(20.dp))
        Text(
            stringResource(page.bodyRes),
            style = MaterialTheme.typography.bodyLarge,
            color = Color.White.copy(alpha = 0.85f),
            textAlign = TextAlign.Center,
        )
        if (isLast) {
            Spacer(Modifier.height(28.dp))
            Text(
                stringResource(R.string.screen_welcome_author_links),
                style = MaterialTheme.typography.bodySmall,
                color = Color.White.copy(alpha = 0.7f),
                textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(16.dp))
            OutlinedButton(
                onClick = onOpenHowTo,
                modifier = Modifier.fillMaxWidth(0.85f),
            ) { Text(stringResource(R.string.screen_how_to_use_app)) }
            Spacer(Modifier.height(8.dp))
            val opensInBrowser = stringResource(R.string.screen_opens_in_browser)
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                OutlinedButton(onClick = onOpenSettings) {
                    Text(stringResource(R.string.screen_welcome_open_settings))
                }
                OutlinedButton(
                    onClick = onReportIssue,
                    // Label only: the button's own click action is kept (null action merges).
                    modifier = Modifier.semantics { onClick(label = opensInBrowser, action = null) },
                ) { Text(stringResource(R.string.screen_report_issue)) }
            }
        }
    }
}
