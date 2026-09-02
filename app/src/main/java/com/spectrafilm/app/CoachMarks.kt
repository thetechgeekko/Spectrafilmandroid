/*
 * Spektrafilm for Android — editor coach marks. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * A one-time, dismissible overlay shown the first time the editor is opened (after
 * onboarding). It surfaces the non-obvious gestures — tap-a-category, before/after
 * compare, tap-to-inspect-at-100%, pinch-zoom — the way Lightroom's coach marks do,
 * so the gesture-driven editor is discoverable without a manual. Gated by
 * AppSettings.seenEditorCoach; shown exactly once.
 */
package com.spectrafilm.app

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

/**
 * Full-screen one-time coach-mark overlay. [onDismiss] persists the "seen" flag and
 * removes the overlay. Tapping the scrim or the button both dismiss.
 */
@Composable
fun EditorCoachOverlay(onDismiss: () -> Unit) {
    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.72f))
            // Tap anywhere on the scrim to dismiss (no ripple — it's a backdrop).
            .clickable(
                interactionSource = remember { MutableInteractionSource() },
                indication = null,
                onClickLabel = stringResource(R.string.tool_coach_dismiss),
                onClick = onDismiss,
            ),
        contentAlignment = Alignment.Center,
    ) {
        Surface(
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surface,
            tonalElevation = 6.dp,
            modifier = Modifier
                .widthIn(max = 360.dp)
                .padding(28.dp),
        ) {
            Column(Modifier.padding(22.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                Text(
                    stringResource(R.string.tool_coach_title),
                    style = MaterialTheme.typography.headlineSmall,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.semantics { heading() },
                )
                CoachTip(
                    stringResource(R.string.tool_coach_category_title),
                    stringResource(R.string.tool_coach_category_body),
                )
                CoachTip(
                    stringResource(R.string.tool_coach_compare_title),
                    stringResource(R.string.tool_coach_compare_body),
                )
                CoachTip(
                    stringResource(R.string.tool_coach_inspect_title),
                    stringResource(R.string.tool_coach_inspect_body),
                )
                CoachTip(
                    stringResource(R.string.tool_coach_zoom_title),
                    stringResource(R.string.tool_coach_zoom_body),
                )
                Button(
                    onClick = onDismiss,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = 4.dp),
                ) { Text(stringResource(R.string.tool_coach_got_it)) }
            }
        }
    }
}

@Composable
private fun CoachTip(title: String, body: String) {
    // One TalkBack node per tip (title + body); the bullet is decorative.
    Row(verticalAlignment = Alignment.Top, modifier = Modifier.semantics(mergeDescendants = true) {}) {
        Box(
            Modifier
                .padding(top = 6.dp, end = 12.dp)
                .size(8.dp)
                .clip(CircleShape)
                .background(MaterialTheme.colorScheme.primary),
        )
        Column {
            Text(title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
            Text(
                body,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}
