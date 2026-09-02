/*
 * Spektrafilm for Android — UI widgets. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Self-contained Material3 composables styled after Image Toolbox: rounded
 * "preference" setting cards, a collapsible section header, an enhanced slider
 * with an inline value label, RGB-triple / pair controls, switch rows and enum
 * dropdowns. No external dependency on Image Toolbox.
 */
package com.spectrafilm.app

import android.view.HapticFeedbackConstants
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.wrapContentSize
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.PlainTooltip
import androidx.compose.material3.TooltipBox
import androidx.compose.material3.TooltipDefaults
import androidx.compose.material3.rememberTooltipState
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.minimumInteractiveComponentSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.dismiss
import androidx.compose.ui.semantics.error
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntRect
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Popup
import androidx.compose.ui.window.PopupPositionProvider
import androidx.compose.ui.window.PopupProperties
import kotlin.math.roundToInt

/**
 * Reports slider drag begin/end to the editor (Lightroom's ICBSliderTrackingBegin/End): [onChange]
 * fires on each drag frame, [onFinished] on release. The editor uses this to render a fast live
 * DRAFT only while a slider is actively dragged, then the crisp full pass on release — so a discrete
 * edit (switch/dropdown) skips the draft and goes straight to the crisp render. Provided via
 * [LocalSliderInteraction]; the default is a no-op so a slider still works with no provider.
 */
class SliderInteraction(
    val onChange: () -> Unit = {},
    val onFinished: () -> Unit = {},
)

/** CompositionLocal carrying the active [SliderInteraction]; defaults to a no-op. */
val LocalSliderInteraction = staticCompositionLocalOf { SliderInteraction() }

/**
 * Wraps arbitrary [content] in a Material3 [TooltipBox] that surfaces [text] on
 * long-press (and hover, on devices that report it). Reusable so every clickable
 * control in the editor can expose its purpose without re-plumbing the boilerplate.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TextTooltip(
    text: String,
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    val state = rememberTooltipState()
    TooltipBox(
        positionProvider = TooltipDefaults.rememberPlainTooltipPositionProvider(),
        tooltip = { PlainTooltip { Text(text) } },
        state = state,
        modifier = modifier,
    ) { content() }
}

/**
 * An Image-Toolbox-style collapsible section: a rounded card with a header row
 * (title + optional enabled-switch + chevron) that expands/collapses its content.
 */
@Composable
fun SectionCard(
    title: String,
    expanded: Boolean,
    onExpandedChange: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
    enabledSwitch: Boolean? = null,
    onEnabledChange: ((Boolean) -> Unit)? = null,
    help: ParamHelp? = null,
    content: @Composable () -> Unit,
) {
    var showHelp by remember { mutableStateOf(false) }
    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.35f),
        ),
    ) {
        val rotation by animateFloatAsState(if (expanded) 180f else 0f, label = "chevron")
        // The header is one TalkBack node: "<title>, Expanded/Collapsed, heading, button".
        // The help badge and the enable switch stay separate, individually focusable nodes.
        val expandedState = stringResource(
            if (expanded) R.string.widget_state_expanded else R.string.widget_state_collapsed,
        )
        val toggleLabel = stringResource(
            if (expanded) R.string.widget_section_collapse else R.string.widget_section_expand,
        )
        val enableDescription = stringResource(R.string.widget_section_enable_switch, title)
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clickableNoRipple(onClickLabel = toggleLabel, role = Role.Button) {
                    onExpandedChange(!expanded)
                }
                .semantics {
                    heading()
                    stateDescription = expandedState
                }
                .padding(horizontal = 16.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                title,
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.weight(1f),
            )
            if (help != null) {
                HelpBadge(title) { showHelp = true }
                Spacer(Modifier.width(8.dp))
            }
            if (enabledSwitch != null && onEnabledChange != null) {
                Switch(
                    checked = enabledSwitch,
                    onCheckedChange = onEnabledChange,
                    modifier = Modifier.semantics { contentDescription = enableDescription },
                )
                Spacer(Modifier.width(8.dp))
            }
            Chevron(modifier = Modifier.rotate(rotation))
        }
        AnimatedVisibility(visible = expanded) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(start = 16.dp, end = 16.dp, bottom = 14.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) { content() }
        }
    }
    if (help != null && showHelp) {
        HelpSheet(help) { showHelp = false }
    }
}

/**
 * A small circular "?" affordance for a [SectionCard] header. Drawn with a glyph rather than a
 * material-icons dependency (matching [Chevron]); a long-press tooltip + semantics label name the
 * section so it is discoverable and accessible. [onClick] opens the section's [HelpSheet].
 */
@Composable
private fun HelpBadge(title: String, onClick: () -> Unit) {
    val accent = MaterialTheme.colorScheme.primary
    val description = stringResource(R.string.widget_help_badge_description, title)
    val showHelpLabel = stringResource(R.string.widget_help_badge_action)
    TextTooltip(stringResource(R.string.widget_help_badge_tooltip, title)) {
        Box(
            modifier = Modifier
                .minimumInteractiveComponentSize()
                .size(26.dp)
                .clip(CircleShape)
                .border(BorderStroke(1.dp, accent.copy(alpha = 0.55f)), CircleShape)
                .clickable(onClickLabel = showHelpLabel, role = Role.Button, onClick = onClick)
                .semantics { contentDescription = description },
            contentAlignment = Alignment.Center,
        ) {
            Text(
                "?",
                style = MaterialTheme.typography.labelLarge,
                fontWeight = FontWeight.Bold,
                color = accent,
            )
        }
    }
}

/**
 * A bottom-sheet "help sheet" carrying a control's plain-language [ParamHelp]: a headline, a
 * one-line summary and a fuller explanation, with the GPLv3 spektrafilm attribution. Content is
 * scrollable so long bodies fit on short screens.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HelpSheet(help: ParamHelp, onDismiss: () -> Unit) {
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(start = 24.dp, end = 24.dp, bottom = 32.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(
                help.title,
                style = MaterialTheme.typography.headlineSmall,
                modifier = Modifier.semantics { heading() },
            )
            Text(
                help.summary,
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.primary,
            )
            Text(
                help.body,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                "Film modeling powered by spektrafilm",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f),
            )
        }
    }
}

/**
 * A disclosure control that splits a dense section into a "basic" set (always shown) and an
 * "advanced" set (revealed on demand). Pure presentation: the hidden controls keep their state and
 * the engine still receives every param, so toggling this changes nothing about the render.
 */
@Composable
fun AdvancedToggle(advanced: Boolean, onToggle: (Boolean) -> Unit) {
    TextButton(
        onClick = { onToggle(!advanced) },
        modifier = Modifier.fillMaxWidth(),
    ) {
        Text(
            stringResource(
                if (advanced) R.string.widget_advanced_hide else R.string.widget_advanced_show,
            ),
        )
    }
}

/** A simple downward chevron drawn on a Canvas (avoids a material-icons dependency). */
@Composable
private fun Chevron(modifier: Modifier = Modifier) {
    val color = LocalContentColor.current
    Canvas(modifier = modifier.size(24.dp)) {
        val w = size.width
        val h = size.height
        val stroke = w * 0.09f
        drawLine(color, Offset(w * 0.30f, h * 0.40f), Offset(w * 0.50f, h * 0.62f),
            strokeWidth = stroke, cap = StrokeCap.Round)
        drawLine(color, Offset(w * 0.50f, h * 0.62f), Offset(w * 0.70f, h * 0.40f),
            strokeWidth = stroke, cap = StrokeCap.Round)
    }
}

/** Convenience clickable without a ripple import dependency cost. */
@Composable
private fun Modifier.clickableNoRipple(
    onClickLabel: String? = null,
    role: Role? = null,
    onClick: () -> Unit,
): Modifier =
    this.then(
        Modifier.clickableImpl(onClickLabel, role, onClick),
    )

@Composable
private fun Modifier.clickableImpl(onClickLabel: String?, role: Role?, onClick: () -> Unit): Modifier {
    val interaction = remember { MutableInteractionSource() }
    return this.then(
        Modifier.clickable(
            interactionSource = interaction,
            indication = null,
            onClickLabel = onClickLabel,
            role = role,
            onClick = onClick,
        ),
    )
}

/**
 * An enhanced single-value slider: a labelled row with the current value shown in a
 * pill, snapping to [step] and clamped to [range]. [tooltip] is rendered as helper text.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun EnhancedSlider(
    label: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    onValueChange: (Float) -> Unit,
    modifier: Modifier = Modifier,
    step: Float = 0f,
    decimals: Int = 2,
    tooltip: String? = null,
    default: Float? = null,
) {
    val view = LocalView.current
    val interaction = LocalSliderInteraction.current
    var editing by remember { mutableStateOf(false) }
    val formatted = formatValue(value, decimals)
    Column(modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(label, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
            // The value pill is interactive (Lightroom-style): single-tap to type an exact
            // value, double-tap to reset to the parameter's neutral default. Typing works on
            // every slider; reset is offered only when a [default] is supplied and the value
            // isn't already there. Both give a tick of haptic feedback. The double-tap has no
            // TalkBack equivalent, so the reset is also exposed as a custom accessibility action.
            val resetDefault = default?.takeIf { it != value }
            val reset: (() -> Unit)? = resetDefault?.let { dv ->
                {
                    onValueChange(snap(dv, range, step))
                    view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                }
            }
            val pillDescription = stringResource(R.string.widget_value_pill_description, label, formatted)
            val enterLabel = stringResource(R.string.widget_value_pill_enter_exact, label)
            val resetLabel = stringResource(R.string.widget_value_pill_reset_default, label)
            ValuePill(
                formatted,
                modifier = Modifier
                    .minimumInteractiveComponentSize()
                    .combinedClickable(
                        interactionSource = remember { MutableInteractionSource() },
                        indication = null,
                        onClickLabel = enterLabel,
                        role = Role.Button,
                        onClick = { editing = true },
                        onDoubleClick = reset,
                    )
                    .semantics {
                        contentDescription = pillDescription
                        if (reset != null) {
                            customActions = listOf(CustomAccessibilityAction(resetLabel) { reset(); true })
                        }
                    },
            )
        }
        val steps = if (step > 0f) {
            (((range.endInclusive - range.start) / step).roundToInt() - 1).coerceAtLeast(0)
        } else 0
        // Lightroom-style tactile feedback: a light tick when a drag settles, so an
        // adjustment "lands" physically. Fired on release (not per-frame) to stay subtle.
        Slider(
            value = value.coerceIn(range.start, range.endInclusive),
            onValueChange = { onValueChange(snap(it, range, step)); interaction.onChange() },
            onValueChangeFinished = {
                view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                interaction.onFinished()
            },
            valueRange = range,
            steps = steps,
            // Announce "<label>, <value>" rather than a bare percentage.
            modifier = Modifier.semantics {
                contentDescription = label
                stateDescription = formatted
            },
        )
        if (tooltip != null) {
            Text(
                tooltip,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        if (editing) {
            NumericEntryDialog(
                label = label,
                initial = formatValue(value, decimals),
                range = range,
                step = step,
                decimals = decimals,
                onDismiss = { editing = false },
                onConfirm = {
                    onValueChange(it)
                    view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                },
            )
        }
    }
}

@Composable
private fun ValuePill(text: String, modifier: Modifier = Modifier) {
    Surface(
        modifier = modifier,
        shape = RoundedCornerShape(10.dp),
        color = MaterialTheme.colorScheme.secondaryContainer,
    ) {
        Text(
            text,
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSecondaryContainer,
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
        )
    }
}

/** A linked RGB-triple control: a label plus three sliders sharing one range/step. */
@Composable
fun TripleSlider(
    label: String,
    value: Triple<Float, Float, Float>,
    range: ClosedFloatingPointRange<Float>,
    onValueChange: (Triple<Float, Float, Float>) -> Unit,
    step: Float = 0f,
    decimals: Int = 2,
    tooltip: String? = null,
    componentLabels: Triple<String, String, String> = Triple(
        stringResource(R.string.widget_component_r),
        stringResource(R.string.widget_component_g),
        stringResource(R.string.widget_component_b),
    ),
    default: Triple<Float, Float, Float>? = null,
) {
    Column(Modifier.fillMaxWidth()) {
        Text(label, style = MaterialTheme.typography.bodyMedium)
        if (tooltip != null) {
            Text(
                tooltip,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        EnhancedSlider(componentLabels.first, value.first, range, step = step, decimals = decimals,
            default = default?.first, onValueChange = { onValueChange(value.copy(first = it)) })
        EnhancedSlider(componentLabels.second, value.second, range, step = step, decimals = decimals,
            default = default?.second, onValueChange = { onValueChange(value.copy(second = it)) })
        EnhancedSlider(componentLabels.third, value.third, range, step = step, decimals = decimals,
            default = default?.third, onValueChange = { onValueChange(value.copy(third = it)) })
    }
}

/** A linked pair control: a label plus two sliders. */
@Composable
fun PairSlider(
    label: String,
    value: Pair<Float, Float>,
    range: ClosedFloatingPointRange<Float>,
    onValueChange: (Pair<Float, Float>) -> Unit,
    step: Float = 0f,
    decimals: Int = 2,
    tooltip: String? = null,
    componentLabels: Pair<String, String> =
        stringResource(R.string.widget_component_1) to stringResource(R.string.widget_component_2),
    default: Pair<Float, Float>? = null,
) {
    Column(Modifier.fillMaxWidth()) {
        Text(label, style = MaterialTheme.typography.bodyMedium)
        if (tooltip != null) {
            Text(
                tooltip,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        EnhancedSlider(componentLabels.first, value.first, range, step = step, decimals = decimals,
            default = default?.first, onValueChange = { onValueChange(value.copy(first = it)) })
        EnhancedSlider(componentLabels.second, value.second, range, step = step, decimals = decimals,
            default = default?.second, onValueChange = { onValueChange(value.copy(second = it)) })
    }
}

/** A switch row (label + helper tooltip + trailing switch). */
@Composable
fun SwitchRow(
    label: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    tooltip: String? = null,
) {
    // The whole row toggles and is ONE TalkBack node (label + tooltip + state); the inner Switch
    // is display-only (onCheckedChange = null), so the row keeps its own >= 48 dp height.
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .toggleable(
                value = checked,
                interactionSource = remember { MutableInteractionSource() },
                indication = null,
                role = Role.Switch,
                onValueChange = onCheckedChange,
            ),
    ) {
        Column(Modifier.weight(1f)) {
            Text(label, style = MaterialTheme.typography.bodyMedium)
            if (tooltip != null) {
                Text(
                    tooltip,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        Switch(checked = checked, onCheckedChange = null)
    }
}

/** An integer slider. */
@Composable
fun IntSlider(
    label: String,
    value: Int,
    range: IntRange,
    onValueChange: (Int) -> Unit,
    tooltip: String? = null,
    default: Int? = null,
) {
    EnhancedSlider(
        label = label,
        value = value.toFloat(),
        range = range.first.toFloat()..range.last.toFloat(),
        step = 1f,
        decimals = 0,
        tooltip = tooltip,
        default = default?.toFloat(),
        onValueChange = { onValueChange(it.roundToInt()) },
    )
}

/**
 * A compact, horizontally-scrolling sub-tab strip for splitting one tool panel into groups
 * (Lightroom-style, e.g. Film / Print / Scanner / Output). The selected tab is a filled
 * primary pill; selection state is owned by the caller.
 */
@Composable
fun SubTabRow(
    tabs: List<String>,
    selected: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .selectableGroup(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        tabs.forEachIndexed { i, label ->
            val active = i == selected
            Surface(
                shape = RoundedCornerShape(10.dp),
                color = if (active) MaterialTheme.colorScheme.primary
                else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f),
                modifier = Modifier
                    .minimumInteractiveComponentSize()
                    .clip(RoundedCornerShape(10.dp))
                    .selectable(selected = active, role = Role.Tab) { onSelect(i) },
            ) {
                Text(
                    label,
                    style = MaterialTheme.typography.labelLarge,
                    color = if (active) MaterialTheme.colorScheme.onPrimary
                    else MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 14.dp, vertical = 8.dp),
                )
            }
        }
    }
}

/** A read-only exposed dropdown for a list of options. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun <T> Dropdown(
    label: String,
    selected: T,
    options: List<T>,
    display: (T) -> String,
    onSelect: (T) -> Unit,
    modifier: Modifier = Modifier,
) {
    var expanded by remember { mutableStateOf(false) }
    ExposedDropdownMenuBox(
        expanded = expanded,
        onExpandedChange = { expanded = it },
        modifier = modifier,
    ) {
        OutlinedTextField(
            value = display(selected),
            onValueChange = {},
            readOnly = true,
            label = { Text(label) },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) },
            modifier = Modifier.menuAnchor().fillMaxWidth(),
        )
        ExposedDropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            options.forEach { opt ->
                DropdownMenuItem(
                    text = { Text(display(opt)) },
                    onClick = { onSelect(opt); expanded = false },
                )
            }
        }
    }
}

/** One selectable option in a grouped dropdown: a stable id plus its display label. */
data class DropdownOption(val id: String, val label: String)

/** A titled group of [DropdownOption]s (e.g. a stock category) for a grouped dropdown. */
data class DropdownGroup(val title: String, val options: List<DropdownOption>)

/** Convert StockCatalog [ProfileOption]s into ordered [DropdownGroup]s, preserving order. */
fun List<ProfileOption>.toGroups(): List<DropdownGroup> {
    val byGroup = LinkedHashMap<String, MutableList<DropdownOption>>()
    for (o in this) byGroup.getOrPut(o.groupTitle) { mutableListOf() }.add(DropdownOption(o.id, o.label))
    return byGroup.map { (title, opts) -> DropdownGroup(title, opts) }
}

/**
 * A read-only exposed dropdown whose options are organized under non-selectable group
 * headers. The field shows the label of the currently selected id (falling back to the
 * raw id when it isn't among the options). Used for built-in presets and for the
 * catalog-grouped film/print profile pickers.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun GroupedDropdown(
    label: String,
    selectedId: String,
    groups: List<DropdownGroup>,
    onSelect: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    var expanded by remember { mutableStateOf(false) }
    val selectedLabel = remember(selectedId, groups) {
        groups.firstNotNullOfOrNull { g -> g.options.firstOrNull { it.id == selectedId }?.label } ?: selectedId
    }
    ExposedDropdownMenuBox(
        expanded = expanded,
        onExpandedChange = { expanded = it },
        modifier = modifier,
    ) {
        OutlinedTextField(
            value = selectedLabel,
            onValueChange = {},
            readOnly = true,
            label = { Text(label) },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) },
            modifier = Modifier.menuAnchor().fillMaxWidth(),
        )
        ExposedDropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            groups.forEachIndexed { gi, group ->
                if (group.title.isNotBlank()) {
                    Text(
                        group.title,
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.primary,
                        modifier = Modifier
                            .padding(start = 16.dp, top = if (gi == 0) 8.dp else 12.dp, bottom = 4.dp)
                            .semantics { heading() },
                    )
                }
                group.options.forEach { opt ->
                    DropdownMenuItem(
                        text = { Text(opt.label) },
                        onClick = { onSelect(opt.id); expanded = false },
                    )
                }
            }
        }
    }
}

/**
 * A small "not yet active" badge + helper line for parameters that are present in the
 * UI (forward-compatible) but have no engine effect yet. Wrap the inert controls in a
 * [GatedBlock] to dim them and append this note, so users aren't misled (repo issue #6).
 */
@Composable
fun NotYetActiveNote(
    detail: String = stringResource(R.string.widget_not_yet_active_detail),
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.semantics(mergeDescendants = true) {},
    ) {
        Surface(
            shape = RoundedCornerShape(8.dp),
            color = MaterialTheme.colorScheme.tertiaryContainer,
        ) {
            Text(
                stringResource(R.string.widget_not_yet_active_badge),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onTertiaryContainer,
                modifier = Modifier.padding(horizontal = 8.dp, vertical = 3.dp),
            )
        }
        Spacer(Modifier.width(8.dp))
        Text(
            detail,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

/**
 * Visually gates [content] for parameters that don't do anything yet: shows a
 * [NotYetActiveNote] header and dims the controls (still interactive, so values are
 * retained/forward-compatible, but clearly marked as inert).
 */
@Composable
fun GatedBlock(
    note: String = stringResource(R.string.widget_not_yet_active_detail),
    content: @Composable () -> Unit,
) {
    Column(Modifier.fillMaxWidth()) {
        NotYetActiveNote(note)
        Spacer(Modifier.width(0.dp))
        Column(
            modifier = Modifier.fillMaxWidth().alpha(0.55f),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) { content() }
    }
}

private fun snap(v: Float, range: ClosedFloatingPointRange<Float>, step: Float): Float {
    if (step <= 0f) return v.coerceIn(range.start, range.endInclusive)
    val snapped = range.start + (((v - range.start) / step).roundToInt()) * step
    return snapped.coerceIn(range.start, range.endInclusive)
}

private fun formatValue(v: Float, decimals: Int): String =
    if (decimals <= 0) v.roundToInt().toString() else "%.${decimals}f".format(v)

/**
 * Parse a user-typed slider value into a snapped, in-range [Float], or null when the text
 * isn't a finite number. Tolerates surrounding whitespace, a leading '+', and a comma
 * decimal separator; clamps to [range] and snaps to [step] via the same [snap] the drag
 * path uses, so keyboard entry and dragging land on identical values. Pure (no Android
 * deps) so it is unit-tested in SliderInputTest.
 */
internal fun parseSliderInput(
    text: String,
    range: ClosedFloatingPointRange<Float>,
    step: Float,
): Float? {
    val v = text.trim().removePrefix("+").replace(',', '.').toFloatOrNull() ?: return null
    if (v.isNaN() || v.isInfinite()) return null
    return snap(v, range, step)
}

/**
 * A small dialog for typing an exact slider value — Lightroom's "tap the number" affordance.
 * Confirms only a parseable, in-range number (the Set button disables otherwise); a "±"
 * toggle covers negative ranges, where the numeric IME often omits a minus key.
 */
@Composable
private fun NumericEntryDialog(
    label: String,
    initial: String,
    range: ClosedFloatingPointRange<Float>,
    step: Float,
    decimals: Int,
    onDismiss: () -> Unit,
    onConfirm: (Float) -> Unit,
) {
    var text by remember { mutableStateOf(initial) }
    val parsed = parseSliderInput(text, range, step)
    // [parsed] clamps out-of-range input (Set still works, landing on the bound), so the range
    // hint is flagged as an error both for unparseable text and for a typed value outside the
    // range — the latter would otherwise be clamped silently.
    val typed = text.trim().removePrefix("+").replace(',', '.').toFloatOrNull()
    val outOfRange = typed != null && (typed < range.start || typed > range.endInclusive)
    val isError = text.isNotBlank() && (parsed == null || outOfRange)
    val rangeHint = stringResource(
        R.string.widget_numeric_entry_range_hint,
        formatValue(range.start, decimals),
        formatValue(range.endInclusive, decimals),
    )
    val signDescription = stringResource(R.string.widget_numeric_entry_sign_toggle)
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(label, modifier = Modifier.semantics { heading() }) },
        text = {
            Column {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    OutlinedTextField(
                        value = text,
                        onValueChange = { text = it },
                        singleLine = true,
                        label = { Text(stringResource(R.string.widget_numeric_entry_field_label)) },
                        isError = isError,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                        modifier = Modifier.weight(1f),
                    )
                    if (range.start < 0f) {
                        Spacer(Modifier.width(8.dp))
                        OutlinedButton(
                            onClick = {
                                text = when {
                                    text.startsWith("-") -> text.removePrefix("-")
                                    text.isBlank() -> "-"
                                    else -> "-$text"
                                }
                            },
                            modifier = Modifier.semantics { contentDescription = signDescription },
                        ) { Text("±") }
                    }
                }
                Spacer(Modifier.height(6.dp))
                Text(
                    rangeHint,
                    style = MaterialTheme.typography.bodySmall,
                    color = if (isError) MaterialTheme.colorScheme.error
                    else MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.semantics { if (isError) error(rangeHint) },
                )
            }
        },
        confirmButton = {
            TextButton(enabled = parsed != null, onClick = { parsed?.let(onConfirm); onDismiss() }) {
                Text(stringResource(R.string.widget_numeric_entry_set))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.widget_numeric_entry_cancel)) }
        },
    )
}

// ---------------------------------------------------------------------------
// AutoExposureControl — Lightroom-mobile style expandable metering control
// ---------------------------------------------------------------------------

/**
 * Converts a snake_case method id to a human-readable Title Case label.
 * e.g. "center_weighted" -> "Center Weighted"
 */
private fun meteringMethodLabel(id: String): String =
    id.split('_').joinToString(" ") { word ->
        word.replaceFirstChar { it.uppercaseChar() }
    }

/**
 * A small drag-handle bar drawn via Canvas — the grabber affordance at the
 * top of the expanded metering list.
 */
@Composable
private fun DragHandle(modifier: Modifier = Modifier) {
    val color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f)
    Canvas(modifier = modifier.size(width = 32.dp, height = 4.dp)) {
        drawRoundRect(
            color = color,
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(2.dp.toPx()),
        )
    }
}

/**
 * Adaptive popup position provider for the metering-method panel.
 *
 * Placement logic:
 *   - If there is enough room ABOVE the anchor (anchorBounds.top >= popupContentSize.height),
 *     place the panel so its bottom edge meets the anchor's top edge (grows upward).
 *   - Otherwise, place the panel so its top edge meets the anchor's bottom edge (grows downward).
 *   - Horizontally, align the panel's left edge to the anchor's left edge.
 *   - Both x and y are clamped to stay fully within the window bounds.
 *
 * The [placeAbove] callback lets the composable know which direction was chosen so
 * the drag-handle can be positioned on the correct edge.
 */
private class MeteringPopupPositionProvider(
    private val onPlacedAbove: (Boolean) -> Unit,
) : PopupPositionProvider {
    override fun calculatePosition(
        anchorBounds: IntRect,
        windowSize: IntSize,
        layoutDirection: LayoutDirection,
        popupContentSize: IntSize,
    ): IntOffset {
        val placeAbove = anchorBounds.top >= popupContentSize.height
        onPlacedAbove(placeAbove)

        val y = if (placeAbove) {
            // Bottom of popup aligns with top of anchor.
            anchorBounds.top - popupContentSize.height
        } else {
            // Top of popup aligns with bottom of anchor.
            anchorBounds.bottom
        }

        val x = anchorBounds.left

        // Clamp so the panel never goes off-screen.
        val clampedX = x.coerceIn(0, maxOf(0, windowSize.width - popupContentSize.width))
        val clampedY = y.coerceIn(0, maxOf(0, windowSize.height - popupContentSize.height))

        return IntOffset(clampedX, clampedY)
    }
}

/**
 * Lightroom-mobile-style expandable auto-exposure / metering-method control.
 *
 * Collapsed state: a single "Auto" button (OutlinedButton when off, filled Button
 * showing the active method name when on).
 *
 * Expanded state: an elevated Surface rendered in a [Popup] anchored to the Auto
 * button. The popup positions itself ABOVE the button when there is room, or BELOW
 * when the button is near the top of the screen (adaptive vertical anchoring).
 * Tapping outside the popup or pressing Back dismisses it (focusable = true).
 * A drag handle and a swipe gesture also collapse it. Swiping up on the
 * (collapsed) button expands it.
 *
 * State ownership: [autoExposure] / [autoExposureMethod] are owned by the caller
 * (ParamsState fields); expand/collapse is local [remember] state.
 */
@Composable
fun AutoExposureControl(
    autoExposure: Boolean,
    autoExposureMethod: String,
    methods: List<String>,
    onAutoExposureChange: (Boolean) -> Unit,
    onMethodChange: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    var expanded by remember { mutableStateOf(false) }

    // Whether the popup was last placed above the button (drives drag direction logic).
    var popupIsAbove by remember { mutableStateOf(true) }

    // Accumulated vertical drag on the button (swipe-up to expand).
    var buttonDragAccum by remember { mutableFloatStateOf(0f) }
    // Accumulated vertical drag on the list panel (swipe-down to collapse).
    var listDragAccum by remember { mutableFloatStateOf(0f) }
    // Threshold (px) for swipe recognition — ~40dp worth at most densities.
    val swipeThresholdPx = 80f

    // Human-readable label for the current method.
    val currentLabel = meteringMethodLabel(autoExposureMethod)

    val collapse = { expanded = false }

    // The popup position provider is remembered so it is stable across recompositions.
    val positionProvider = remember {
        MeteringPopupPositionProvider(onPlacedAbove = { popupIsAbove = it })
    }

    // The drag handle's swipe-to-collapse has no TalkBack equivalent, so (as Material's bottom
    // sheet handle does) it carries a name plus a "dismiss" accessibility action.
    val handleDescription = stringResource(R.string.widget_ae_drag_handle)
    val dismissLabel = stringResource(R.string.widget_ae_dismiss_menu)
    val handleSemantics = Modifier.semantics(mergeDescendants = true) {
        contentDescription = handleDescription
        dismiss(dismissLabel) { collapse(); true }
    }

    // ----- Popup containing the metering-method panel -----
    // Placed outside the normal layout flow; the anchor Box below provides the bounds.
    // focusable = true enables tap-outside-to-dismiss and back-press dismissal.
    if (expanded) {
        Popup(
            popupPositionProvider = positionProvider,
            onDismissRequest = collapse,
            properties = PopupProperties(focusable = true),
        ) {
            // Animate on first appearance: slide in from the direction of the button.
            AnimatedVisibility(
                visible = true,
                enter = slideInVertically(
                    // If above → slide up (negative = from below the panel's final position).
                    // If below → slide down (positive).
                    initialOffsetY = { fullHeight ->
                        if (popupIsAbove) fullHeight else -fullHeight
                    },
                    animationSpec = tween(220),
                ) + fadeIn(animationSpec = tween(220)),
            ) {
                Surface(
                    modifier = Modifier
                        .wrapContentSize()
                        .padding(horizontal = 4.dp, vertical = 4.dp)
                        // Swipe gesture on the panel to collapse.
                        // When above: swipe DOWN (positive dragAmount) collapses.
                        // When below: swipe UP (negative dragAmount) collapses.
                        .pointerInput(popupIsAbove) {
                            detectVerticalDragGestures(
                                onDragStart = { listDragAccum = 0f },
                                onDragEnd = { listDragAccum = 0f },
                                onDragCancel = { listDragAccum = 0f },
                                onVerticalDrag = { change, dragAmount ->
                                    change.consume()
                                    listDragAccum += dragAmount
                                    val shouldCollapse = if (popupIsAbove) {
                                        listDragAccum > swipeThresholdPx   // swipe down
                                    } else {
                                        listDragAccum < -swipeThresholdPx  // swipe up
                                    }
                                    if (shouldCollapse) {
                                        expanded = false
                                        listDragAccum = 0f
                                    }
                                },
                            )
                        },
                    shape = RoundedCornerShape(16.dp),
                    tonalElevation = 4.dp,
                    shadowElevation = 6.dp,
                    color = MaterialTheme.colorScheme.surface,
                ) {
                    Column(
                        modifier = Modifier.wrapContentSize(),
                        verticalArrangement = Arrangement.spacedBy(0.dp),
                    ) {
                        // Drag handle — placed on the edge nearest the button:
                        //   • panel above button → handle at the BOTTOM of the panel.
                        //   • panel below button → handle at the TOP of the panel.
                        // When above we render the main content first, then the handle.
                        if (!popupIsAbove) {
                            // Below: handle at top.
                            Box(
                                modifier = Modifier
                                    .wrapContentSize()
                                    .then(handleSemantics)
                                    .padding(top = 8.dp, bottom = 4.dp)
                                    .align(Alignment.CenterHorizontally),
                                contentAlignment = Alignment.Center,
                            ) {
                                DragHandle()
                            }
                        }

                        Text(
                            stringResource(R.string.widget_ae_metering_method),
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier
                                .padding(horizontal = 16.dp, vertical = 4.dp)
                                .semantics { heading() },
                        )

                        HorizontalDivider(modifier = Modifier.padding(horizontal = 16.dp))

                        // The rows form one radio group: exactly one of Off / Manual or a
                        // metering method is selected at a time.
                        Column(Modifier.selectableGroup()) {
                            // "Off / Manual" entry — always at the top.
                            MeteringMethodRow(
                                label = stringResource(R.string.widget_ae_off_manual),
                                isSelected = !autoExposure,
                                onClick = {
                                    onAutoExposureChange(false)
                                    expanded = false
                                },
                            )

                            HorizontalDivider(
                                modifier = Modifier.padding(horizontal = 16.dp),
                                color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.5f),
                            )

                            // All metering methods.
                            methods.forEachIndexed { index, method ->
                                MeteringMethodRow(
                                    label = meteringMethodLabel(method),
                                    isSelected = autoExposure && autoExposureMethod == method,
                                    onClick = {
                                        onAutoExposureChange(true)
                                        onMethodChange(method)
                                        // Keep the list open so the user sees the selection.
                                        // Dismissed via swipe, tap-outside, or back-press.
                                    },
                                )
                                if (index < methods.lastIndex) {
                                    HorizontalDivider(
                                        modifier = Modifier.padding(horizontal = 16.dp),
                                        color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.3f),
                                    )
                                }
                            }
                        }

                        if (popupIsAbove) {
                            // Above: handle at bottom — nearest the button.
                            Box(
                                modifier = Modifier
                                    .wrapContentSize()
                                    .then(handleSemantics)
                                    .padding(top = 4.dp, bottom = 8.dp)
                                    .align(Alignment.CenterHorizontally),
                                contentAlignment = Alignment.Center,
                            ) {
                                DragHandle()
                            }
                        } else {
                            Spacer(Modifier.height(8.dp))
                        }
                    }
                }
            }
        }
    }

    // ----- Collapsed "Auto" button (stays in normal layout flow) -----
    // This Box is the popup anchor — the Popup() sibling above is positioned relative to it.
    // Tap: if off → turn on and expand. If on → toggle list visibility.
    // Swipe up: expand the list.
    Box(
        modifier = modifier
            .fillMaxWidth()
            .pointerInput(Unit) {
                detectVerticalDragGestures(
                    onDragStart = { buttonDragAccum = 0f },
                    onDragEnd = { buttonDragAccum = 0f },
                    onDragCancel = { buttonDragAccum = 0f },
                    onVerticalDrag = { change, dragAmount ->
                        change.consume()
                        buttonDragAccum += dragAmount
                        // Negative dragAmount = upward swipe → expand.
                        if (buttonDragAccum < -swipeThresholdPx) {
                            if (!autoExposure) onAutoExposureChange(true)
                            expanded = true
                            buttonDragAccum = 0f
                        }
                    },
                )
            },
    ) {
        // Both buttons announce as "Auto-exposure, <On, method | Off, manual exposure>, button";
        // the swipe-up gesture needs no accessible twin because a tap does the same thing.
        val aeDescription = stringResource(R.string.widget_ae_description)
        val aeStateOn = stringResource(R.string.widget_ae_state_on, currentLabel)
        val aeStateOff = stringResource(R.string.widget_ae_state_off)
        val chooseLabel = stringResource(R.string.widget_ae_choose_method)
        val turnOnLabel = stringResource(R.string.widget_ae_turn_on)
        TextTooltip(stringResource(R.string.widget_ae_tooltip)) {
        if (autoExposure) {
            // ON: filled/accent button showing active method name.
            Button(
                onClick = {
                    // Tap while on: toggle the list panel.
                    expanded = !expanded
                },
                modifier = Modifier
                    .fillMaxWidth()
                    .semantics {
                        contentDescription = aeDescription
                        stateDescription = aeStateOn
                        onClick(label = chooseLabel, action = null)
                    },
                colors = ButtonDefaults.buttonColors(
                    containerColor = MaterialTheme.colorScheme.primary,
                    contentColor = MaterialTheme.colorScheme.onPrimary,
                ),
            ) {
                Text(
                    stringResource(R.string.widget_ae_button_on, currentLabel),
                    style = MaterialTheme.typography.labelLarge,
                )
            }
        } else {
            // OFF: outlined button, subtitle "Manual exposure".
            Column(Modifier.fillMaxWidth()) {
                OutlinedButton(
                    onClick = {
                        // Tap while off: turn on and expand.
                        onAutoExposureChange(true)
                        expanded = true
                    },
                    modifier = Modifier
                        .fillMaxWidth()
                        .semantics {
                            contentDescription = aeDescription
                            stateDescription = aeStateOff
                            onClick(label = turnOnLabel, action = null)
                        },
                ) {
                    Text(stringResource(R.string.widget_ae_button_off))
                }
                Text(
                    stringResource(R.string.widget_ae_manual_exposure),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(start = 4.dp, top = 2.dp),
                )
            }
        }
        }
    }
}

/**
 * A single row in the metering-method list: label + trailing checkmark when selected.
 */
@Composable
private fun MeteringMethodRow(
    label: String,
    isSelected: Boolean,
    onClick: () -> Unit,
) {
    val primaryColor = MaterialTheme.colorScheme.primary
    val onSurface = MaterialTheme.colorScheme.onSurface
    // Radio-button semantics expose the selection that is otherwise shown only by colour/weight
    // and the drawn checkmark.
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .selectable(selected = isSelected, role = Role.RadioButton, onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(
            text = label,
            style = if (isSelected) {
                MaterialTheme.typography.bodyMedium.copy(fontWeight = FontWeight.SemiBold)
            } else {
                MaterialTheme.typography.bodyMedium
            },
            color = if (isSelected) primaryColor else onSurface,
            modifier = Modifier.weight(1f),
        )
        if (isSelected) {
            // Checkmark drawn via Canvas — no material-icons dependency.
            val checkColor = primaryColor
            Canvas(modifier = Modifier.size(20.dp)) {
                val w = size.width
                val h = size.height
                val stroke = w * 0.10f
                drawLine(
                    color = checkColor,
                    start = Offset(w * 0.18f, h * 0.52f),
                    end = Offset(w * 0.42f, h * 0.76f),
                    strokeWidth = stroke,
                    cap = StrokeCap.Round,
                )
                drawLine(
                    color = checkColor,
                    start = Offset(w * 0.42f, h * 0.76f),
                    end = Offset(w * 0.82f, h * 0.28f),
                    strokeWidth = stroke,
                    cap = StrokeCap.Round,
                )
            }
        }
    }
}
