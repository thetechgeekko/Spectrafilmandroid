/*
 * Spektrafilm for Android — ticket #181 platform-only accessibility scan. GPLv3.
 *
 * Java on purpose: the release androidTest APK carries no Kotlin runtime and resolves kotlin.*
 * from the R8-shrunk target app, where every stdlib member it touches must be kept and asserted
 * one by one (proguard-rules.pro, tools/r8_check). Plain Java has no such cross-APK dependency.
 */
package com.spectrafilm.app;

import android.accessibilityservice.AccessibilityService;
import android.app.Activity;
import android.app.Instrumentation;
import android.app.UiAutomation;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.Spanned;
import android.text.TextUtils;
import android.text.style.ClickableSpan;
import android.view.accessibility.AccessibilityNodeInfo;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.TimeoutException;
import java.util.function.BooleanSupplier;
import java.util.regex.Pattern;

/**
 * Mirrors the core Accessibility Test Framework presets (speakable text, touch target, duplicate
 * speakable, range/edit labels) plus a critical-journey smoke with nothing but {@code UiAutomation}
 * and {@code AccessibilityNodeInfo}, so no AndroidX test dependency enters the signed gate APK.
 *
 * <p>Node lookups resolve the app's own string resources through the target context, so the exact
 * displayed text (en-XA / ar-XB pseudo-locales, font_scale 2.0) is matched by plain equality.
 *
 * <p>Evidence: {@code <externalFilesDir>/ticket181/<nn>-<screen>.txt|.png} + {@code summary.txt}.
 */
public final class Ticket181AccessibilityChecks {
    private static final String EVIDENCE_DIR = "ticket181";
    private static final String MAIN_ACTIVITY = "com.spectrafilm.app.MainActivity";
    private static final String ROLE_DESCRIPTION_KEY = "AccessibilityNodeInfo.roleDescription";
    private static final float MIN_TARGET_DP = 48f;
    private static final float WARN_TARGET_DP = 44f;
    private static final long SETTLE_TIMEOUT_MS = 6_000L;
    private static final int MAX_TAB_SCROLLS = 8;

    /** String resource names, resolved by getIdentifier: R8 drops R$string from the release app. */
    private static final String STRING_EXPORT = "editor_action_export";
    private static final List<String> CRITICAL_ACTIONS = Arrays.asList(
            STRING_EXPORT, "editor_action_undo", "editor_action_redo", "editor_action_settings");
    private static final List<String> BRIDGE_DESTINATIONS =
            Arrays.asList("SETTINGS", "ABOUT", "DIAGNOSTICS", "CURVES_FILM", "CURVES_PRINT");
    private static final Set<String> CONTROL_CLASSES = new HashSet<>(Arrays.asList(
            "android.widget.Button", "android.widget.ImageButton", "android.widget.CheckBox",
            "android.widget.RadioButton", "android.widget.Switch", "android.widget.ToggleButton",
            "android.widget.CompoundButton", "android.widget.Spinner", "android.widget.SeekBar",
            "android.widget.EditText", "android.widget.CheckedTextView"));
    private static final Pattern LINK_TEXT =
            Pattern.compile("https?://|www\\.", Pattern.CASE_INSENSITIVE);
    private static final Pattern WHITESPACE = Pattern.compile("\\s+");

    private Ticket181AccessibilityChecks() {}

    private enum Severity { ERROR, WARN }

    private static final class Finding {
        final String screen;
        final String check;
        final Severity severity;
        final String detail;

        Finding(String screen, String check, Severity severity, String detail) {
            this.screen = screen;
            this.check = check;
            this.severity = severity;
            this.detail = detail;
        }

        @Override
        public String toString() {
            return severity + " " + screen + " " + check + ": " + detail;
        }
    }

    private static final class Node {
        final AccessibilityNodeInfo info;
        final Node parent;
        final int depth;
        final List<Node> children = new ArrayList<>();
        final Rect bounds = new Rect();
        final String className;
        final String text;
        final String contentDescription;
        final String stateDescription;
        final String hint;
        final String roleDescription;
        final boolean visible;
        final boolean clickable;
        final List<String> ownPieces = new ArrayList<>();

        Node(AccessibilityNodeInfo info, Node parent, int depth) {
            this.info = info;
            this.parent = parent;
            this.depth = depth;
            info.getBoundsInScreen(bounds);
            className = str(info.getClassName());
            text = str(info.getText());
            contentDescription = str(info.getContentDescription());
            stateDescription = Build.VERSION.SDK_INT >= 30 ? str(info.getStateDescription()) : "";
            hint = Build.VERSION.SDK_INT >= 26 ? str(info.getHintText()) : "";
            final Bundle extras = info.getExtras();
            roleDescription = extras == null ? "" : str(extras.getCharSequence(ROLE_DESCRIPTION_KEY));
            visible = info.isVisibleToUser();
            clickable = info.isClickable() || info.isLongClickable();
            for (String piece : new String[] {text, contentDescription, stateDescription, hint}) {
                if (!isBlank(piece)) ownPieces.add(piece);
            }
        }

        /** Own pieces first, then each child's pieces in tree order (depth-first). */
        List<String> pieces() {
            final List<String> out = new ArrayList<>();
            collectPieces(out);
            return out;
        }

        private void collectPieces(List<String> out) {
            out.addAll(ownPieces);
            for (Node child : children) child.collectPieces(out);
        }

        String speakable() {
            return TextUtils.join(" ", pieces());
        }

        boolean isDescendantOf(Node other) {
            for (Node a = parent; a != null; a = a.parent) if (a == other) return true;
            return false;
        }

        String describe() {
            return className + " text=\"" + text + "\" cd=\"" + contentDescription + "\" role=\""
                    + roleDescription + "\" bounds=[" + bounds.left + "," + bounds.top + "-"
                    + bounds.right + "," + bounds.bottom + "]";
        }
    }

    /** Runs the whole a11y journey on a seeded, EDITOR-ready activity; returns the result stream. */
    public static String run(Instrumentation instrumentation, Activity activity)
            throws IOException {
        final Session session = new Session(instrumentation, activity);
        Activity recreated = null;
        Throwable failure = null;
        try {
            session.scan("editor-crop-overlay", false);
            session.pressBack("close seeded crop overlay");
            session.pressBack("close seeded Input panel");
            session.scan("editor-no-panel", true);
            for (Category category : Category.values()) {
                session.openCategory(category);
                session.scan("editor-panel-" + slug(category.name()), true);
            }
            session.pressBack("close last category panel");
            session.exportSheetJourney();
            for (String destination : BRIDGE_DESTINATIONS) session.visitDestination(destination);
            session.openCategory(Category.GRAIN);
            recreated = session.recreateWithPanelOpen(activity);
            session.scan("editor-recreated", true);
            return session.report();
        } catch (Throwable t) {
            failure = t;
            throw t;
        } finally {
            session.writeSummary(failure);
            final Activity done = recreated;
            if (done != null) {
                instrumentation.runOnMainSync(() -> { if (!done.isFinishing()) done.finish(); });
            }
        }
    }

    private static String slug(String name) {
        return name.toLowerCase(Locale.ROOT).replace('_', '-');
    }

    private static String speakableKey(String raw) {
        return trim(WHITESPACE.matcher(raw).replaceAll(" ")).toLowerCase(Locale.ROOT);
    }

    // ---- Kotlin stdlib semantics reproduced by hand -------------------------------------------

    private static String str(CharSequence value) {
        return value == null ? "" : value.toString();
    }

    /** Kotlin's Char.isWhitespace(): Character.isWhitespace || Character.isSpaceChar. */
    private static boolean isWhitespace(char c) {
        return Character.isWhitespace(c) || Character.isSpaceChar(c);
    }

    private static boolean isBlank(String value) {
        for (int i = 0; i < value.length(); i++) if (!isWhitespace(value.charAt(i))) return false;
        return true;
    }

    private static String trim(String value) {
        int start = 0;
        int end = value.length();
        while (start < end && isWhitespace(value.charAt(start))) start++;
        while (end > start && isWhitespace(value.charAt(end - 1))) end--;
        return value.substring(start, end);
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new IllegalStateException(message);
    }

    private static String joinDescriptions(List<Node> nodes, String separator) {
        final StringBuilder sb = new StringBuilder();
        for (int i = 0; i < nodes.size(); i++) {
            if (i > 0) sb.append(separator);
            sb.append(nodes.get(i).describe());
        }
        return sb.toString();
    }

    /** Last node with the largest bounds.bottom (ties -> deepest in tree order), or null when empty. */
    private static Node lowest(List<Node> nodes) {
        Node best = null;
        for (Node node : nodes) {
            if (best == null || node.bounds.bottom >= best.bounds.bottom) best = node;
        }
        return best;
    }

    private static void deleteRecursively(File file) {
        final File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteRecursively(child);
        file.delete();
    }

    private static void writeText(File file, String text) throws IOException {
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        }
    }

    private static final class Session {
        private final Instrumentation instrumentation;
        private final UiAutomation automation;
        private final Context context;
        private final float density;
        private final File evidence;
        private final List<Finding> findings = new ArrayList<>();
        private final List<String> screenLines = new ArrayList<>();
        private final List<String> journal = new ArrayList<>();
        private int screenIndex = 0;

        Session(Instrumentation instrumentation, Activity activity) {
            this.instrumentation = instrumentation;
            automation = instrumentation.getUiAutomation();
            context = instrumentation.getTargetContext();
            density = activity.getResources().getDisplayMetrics().density;
            evidence = new File(context.getExternalFilesDir(null), EVIDENCE_DIR);
            deleteRecursively(evidence);
            check(evidence.mkdirs() || evidence.isDirectory(),
                    "ticket #181: cannot create " + evidence);
        }

        /** The app's own displayed text for a string resource, looked up by name (no R class). */
        private String appString(String name) {
            final int id = context.getResources()
                    .getIdentifier(name, "string", context.getPackageName());
            if (id == 0) throw new AssertionError("missing string resource " + name);
            return context.getString(id);
        }

        // ---- journey steps -------------------------------------------------------------------

        void scan(String screen, boolean critical) throws IOException {
            settle();
            final List<Node> nodes = snapshot();
            final int before = findings.size();
            checkSpeakable(screen, nodes);
            checkTouchTargets(screen, nodes);
            checkDuplicateSpeakable(screen, nodes);
            checkRangeLabels(screen, nodes);
            checkEditLabels(screen, nodes);
            final List<String> criticalReport =
                    critical ? checkCriticalVisible(screen, nodes) : new ArrayList<>();
            final List<Finding> screenFindings =
                    new ArrayList<>(findings.subList(before, findings.size()));
            int errors = 0;
            int warnings = 0;
            for (Finding f : screenFindings) { if (f.severity == Severity.ERROR) errors++; else warnings++; }
            final String id = String.format(Locale.ROOT, "%02d-%s", screenIndex++, screen);
            writeTree(id, nodes, screenFindings, criticalReport);
            writeScreenshot(id);
            journal.add("SCAN " + id + ": nodes=" + nodes.size()
                    + " errors=" + errors + " warnings=" + warnings);
            screenLines.add("TICKET181_A11Y " + screen + ": errors=" + errors
                    + " warnings=" + warnings);
        }

        void pressBack(String reason) {
            check(automation.performGlobalAction(AccessibilityService.GLOBAL_ACTION_BACK),
                    "ticket #181: GLOBAL_ACTION_BACK rejected (" + reason + ")");
            journal.add("BACK: " + reason);
            settle();
        }

        void openCategory(Category category) {
            final String label = context.getString(category.getLabelRes());
            final Node tab = findCategoryTab(label);
            if (tab == null) throw new AssertionError(missingTabMessage(category, label));
            click(tab, "category tab " + category.name() + " '" + label + "'");
        }

        void exportSheetJourney() throws IOException {
            final int editorWindow = rootNode().getWindowId();
            final Node export = awaitEnabledExport(appString(STRING_EXPORT));
            click(export, "Export");
            if (!waitUntil(10_000L, () -> activeWindowId() != editorWindow)) {
                findings.add(new Finding("export-sheet", "JOURNEY", Severity.ERROR,
                        "export sheet window did not become active within 10 s after clicking Export"));
            }
            scan("export-sheet", false);
            pressBack("dismiss export sheet");
            if (!waitUntil(10_000L, () -> activeWindowId() == editorWindow)) {
                findings.add(new Finding("export-sheet", "JOURNEY", Severity.ERROR,
                        "editor window did not regain focus within 10 s after BACK on the export sheet"));
            }
        }

        void visitDestination(String destination) throws IOException {
            Ticket139EditorTestBridge.requestDestination(destination);
            waitForDestination(destination, 15_000L);
            journal.add("NAVIGATE: " + destination);
            scan(slug(destination), false);
            final long readyBefore = Ticket139EditorTestBridge.liveEditorReadyGeneration();
            Ticket139EditorTestBridge.requestDestination("EDITOR");
            waitForDestination("EDITOR", 15_000L);
            waitForLiveReadyAfter(readyBefore, 45_000L);
            journal.add("NAVIGATE: EDITOR (after " + destination + ")");
        }

        Activity recreateWithPanelOpen(Activity activity) {
            final Instrumentation.ActivityMonitor monitor =
                    instrumentation.addMonitor(MAIN_ACTIVITY, null, false);
            try {
                final long hostBefore = Ticket139EditorTestBridge.hostGeneration();
                final long readyBefore = Ticket139EditorTestBridge.liveEditorReadyGeneration();
                instrumentation.runOnMainSync(activity::recreate);
                final Activity recreated =
                        instrumentation.waitForMonitorWithTimeout(monitor, 20_000L);
                check(recreated != null, "ticket #181: Activity recreation timed out");
                check(recreated != activity, "ticket #181: recreation reused old Activity");
                check(waitUntil(20_000L,
                                () -> Ticket139EditorTestBridge.hostGeneration() > hostBefore),
                        "ticket #181: editor host did not rebuild after recreation");
                waitForDestination("EDITOR", 20_000L);
                waitForLiveReadyAfter(readyBefore, 45_000L);
                journal.add("RECREATE: MainActivity recreated with the Grain panel open");
                return recreated;
            } finally {
                instrumentation.removeMonitor(monitor);
            }
        }

        // ---- checks (ATF preset mirrors) -----------------------------------------------------

        private void checkSpeakable(String screen, List<Node> nodes) {
            for (Node node : nodes) {
                if (!node.visible || !needsSpeakableText(node)) continue;
                // A tab half scrolled out of a LazyRow has its label node culled from the tree.
                if (touchesScrollerEdge(node)) continue;
                if (isBlank(node.speakable())) {
                    findings.add(new Finding(screen, "SPEAKABLE", Severity.ERROR,
                            "no speakable text on own node or descendants: " + node.describe()));
                }
            }
        }

        private boolean needsSpeakableText(Node node) {
            return node.clickable || node.info.isCheckable() || node.info.isEditable()
                    || (node.info.isFocusable() && CONTROL_CLASSES.contains(node.className));
        }

        private void checkTouchTargets(String screen, List<Node> nodes) {
            for (Node node : nodes) {
                if (!node.visible || !node.clickable) continue;
                if ("android.widget.SeekBar".equals(node.className) || isLinkOnlyText(node)) {
                    continue;
                }
                final float w = node.bounds.width() / density;
                final float h = node.bounds.height() / density;
                if (w >= MIN_TARGET_DP && h >= MIN_TARGET_DP) continue;
                if (hasBigClickableAncestor(node) || hasBigClickableDescendant(node)) continue;
                final boolean clipped = touchesScrollerEdge(node);
                final boolean offScreen = touchesScreenEdge(nodes.get(0), node, w, h);
                final boolean warnOnly = (w >= WARN_TARGET_DP && h >= WARN_TARGET_DP) || clipped || offScreen;
                final String size = String.format(Locale.ROOT, "%.0fx%.0fdp", w, h);
                final String why = clipped ? " (clipped by a scroll container edge)"
                        : offScreen ? " (clipped by the screen edge)" : "";
                findings.add(new Finding(screen, "TOUCH_TARGET",
                        warnOnly ? Severity.WARN : Severity.ERROR,
                        size + " < " + (int) MIN_TARGET_DP + "dp" + why + ": " + node.describe()));
            }
        }

        private boolean hasBigClickableAncestor(Node node) {
            for (Node a = node.parent; a != null; a = a.parent) {
                if (a.clickable && a.bounds.width() / density >= MIN_TARGET_DP
                        && a.bounds.height() / density >= MIN_TARGET_DP) return true;
            }
            return false;
        }

        /**
         * M3 IconButton/TextTooltip shape: a 40dp anchor wrapping the 48dp
         * minimumInteractiveComponentSize node that actually receives the tap.
         */
        private boolean hasBigClickableDescendant(Node node) {
            for (Node child : node.children) {
                if (child.clickable && child.bounds.width() / density >= MIN_TARGET_DP
                        && child.bounds.height() / density >= MIN_TARGET_DP) return true;
                if (hasBigClickableDescendant(child)) return true;
            }
            return false;
        }

        /** Partially scrolled-out items report their clipped bounds; do not fail those. */
        private boolean touchesScrollerEdge(Node node) {
            // Every scrollable ancestor, not just the nearest: a chip row that overflows only
            // under a pseudo-locale becomes a horizontal scroller nested in the panel's
            // vertical one, and the chip is still clipped by the vertical edge.
            final Rect b = node.bounds;
            for (Node scroller = node.parent; scroller != null; scroller = scroller.parent) {
                if (!scroller.info.isScrollable()) continue;
                final Rect s = scroller.bounds;
                // Only along the scroll axis: every LazyRow item touches the row's top and bottom.
                final boolean horizontal = hasAction(scroller, AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_LEFT)
                        || hasAction(scroller, AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_RIGHT);
                final boolean vertical = hasAction(scroller, AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_UP)
                        || hasAction(scroller, AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_DOWN);
                if ((horizontal && (b.left <= s.left || b.right >= s.right))
                        || (vertical && (b.top <= s.top || b.bottom >= s.bottom))) return true;
            }
            return false;
        }

        /** A half-expanded sheet's last row runs off the bottom of the display, not of a scroller. */
        private static boolean touchesScreenEdge(Node root, Node node, float w, float h) {
            final Rect b = node.bounds;
            final Rect s = root.bounds;
            return (h < MIN_TARGET_DP && (b.top <= s.top || b.bottom >= s.bottom))
                    || (w < MIN_TARGET_DP && (b.left <= s.left || b.right >= s.right));
        }

        private static boolean hasAction(Node node, AccessibilityNodeInfo.AccessibilityAction action) {
            return node.info.getActionList().contains(action);
        }

        private boolean isLinkOnlyText(Node node) {
            if (!"android.widget.TextView".equals(node.className) || !node.children.isEmpty()) {
                return false;
            }
            final CharSequence raw = node.info.getText();
            if (raw == null) return false;
            final ClickableSpan[] spans = raw instanceof Spanned
                    ? ((Spanned) raw).getSpans(0, raw.length(), ClickableSpan.class)
                    : null;
            return (spans != null && spans.length > 0) || LINK_TEXT.matcher(raw).find();
        }

        private void checkDuplicateSpeakable(String screen, List<Node> nodes) {
            for (Node parent : nodes) {
                final Map<String, List<Node>> groups = new LinkedHashMap<>();
                for (Node child : parent.children) {
                    if (!child.visible || !child.clickable) continue;
                    final String label = speakableKey(child.speakable());
                    if (isBlank(label)) continue;
                    groups.computeIfAbsent(label, k -> new ArrayList<>()).add(child);
                }
                for (Map.Entry<String, List<Node>> entry : groups.entrySet()) {
                    final List<Node> dupes = entry.getValue();
                    if (dupes.size() < 2) continue;
                    findings.add(new Finding(screen, "DUPLICATE_SPEAKABLE", Severity.ERROR,
                            dupes.size() + " clickable siblings announce \"" + entry.getKey()
                                    + "\": " + joinDescriptions(dupes, " | ")));
                }
            }
        }

        private void checkRangeLabels(String screen, List<Node> nodes) {
            for (Node node : nodes) {
                if (node.info.getRangeInfo() == null || !isBlank(node.contentDescription)) continue;
                findings.add(new Finding(screen, "RANGE_LABEL", Severity.ERROR,
                        "range control without contentDescription: " + node.describe()));
            }
        }

        private void checkEditLabels(String screen, List<Node> nodes) {
            for (Node node : nodes) {
                if (!node.info.isEditable()) continue;
                // Compose M3 TextField: the label is a merged child Text node, announced after
                // the value ("ProPhoto RGB, Input color space, drop-down list").
                final boolean labelled = !isBlank(node.hint) || !isBlank(node.contentDescription)
                        || node.info.getLabeledBy() != null || hasLabelChild(node);
                if (!labelled) {
                    findings.add(new Finding(screen, "EDIT_LABEL", Severity.ERROR,
                            "editable field without hint/contentDescription/labeledBy/label child: "
                                    + node.describe()));
                }
            }
        }

        private static boolean hasLabelChild(Node field) {
            for (Node child : field.children) {
                for (String piece : child.pieces()) {
                    if (!isBlank(piece) && !trim(piece).equals(trim(field.text))) return true;
                }
            }
            return false;
        }

        /** Returns one "name=visible|hidden|missing" entry per critical action (also filed as errors). */
        private List<String> checkCriticalVisible(String screen, List<Node> nodes) {
            final List<String> report = new ArrayList<>();
            for (String resName : CRITICAL_ACTIONS) {
                final String name = appString(resName);
                final List<Node> matches = new ArrayList<>();
                boolean anyVisible = false;
                for (Node node : nodes) {
                    if (node.text.contains(name) || node.contentDescription.contains(name)) {
                        matches.add(node);
                        if (node.visible) anyVisible = true;
                    }
                }
                final String state = anyVisible ? "visible" : matches.isEmpty() ? "missing" : "hidden";
                if (!"visible".equals(state)) {
                    final String described = joinDescriptions(matches, " | ");
                    findings.add(new Finding(screen, "CRITICAL_VISIBLE", Severity.ERROR,
                            "\"" + name + "\" is " + state + "; matching nodes: "
                                    + (described.isEmpty() ? "none" : described)));
                }
                final StringBuilder line = new StringBuilder(name).append('=').append(state);
                for (Node match : matches) {
                    line.append(" [").append(match.text).append('|')
                            .append(match.contentDescription).append(']');
                }
                report.add(line.toString());
            }
            return report;
        }

        // ---- node discovery ------------------------------------------------------------------

        private Node findCategoryTab(String label) {
            Node found = locateTab(snapshot(), label);
            if (found != null) return found;
            final int[] actions = {AccessibilityNodeInfo.ACTION_SCROLL_FORWARD,
                    AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD};
            for (int action : actions) {
                for (int attempt = 0; attempt < MAX_TAB_SCROLLS; attempt++) {
                    final Node bar = categoryBar(snapshot());
                    if (bar == null) return null;
                    if (!bar.info.performAction(action)) break;
                    settle();
                    found = locateTab(snapshot(), label);
                    if (found != null) return found;
                }
            }
            return null;
        }

        private Node locateTab(List<Node> nodes, String label) {
            final Node bar = categoryBar(nodes);
            final String wanted = trim(label);
            final List<Node> candidates = new ArrayList<>();
            final List<Node> inBar = new ArrayList<>();
            for (Node node : nodes) {
                // isClickable() only: the M3 tooltip anchor wrapping each tab is long-clickable
                // with the same bounds and label pieces, and rejects ACTION_CLICK.
                if (!node.visible || !node.info.isClickable() || !hasPiece(node, wanted)) continue;
                candidates.add(node);
                if (bar != null && node.isDescendantOf(bar)) inBar.add(node);
            }
            final Node best = lowest(inBar);
            return best != null ? best : lowest(candidates);
        }

        private static boolean hasPiece(Node node, String wanted) {
            for (String piece : node.pieces()) if (trim(piece).equals(wanted)) return true;
            return false;
        }

        /** Lowest horizontal collection on screen (Compose LazyRow: rowCount == 1), else lowest scroller. */
        private Node categoryBar(List<Node> nodes) {
            final List<Node> rows = new ArrayList<>();
            final List<Node> scrollers = new ArrayList<>();
            for (Node node : nodes) {
                final AccessibilityNodeInfo.CollectionInfo collection = node.info.getCollectionInfo();
                if (collection != null && collection.getRowCount() == 1) rows.add(node);
                if (node.info.isScrollable()) scrollers.add(node);
            }
            return lowest(rows.isEmpty() ? scrollers : rows);
        }

        private String missingTabMessage(Category category, String label) {
            final List<Node> clickable = new ArrayList<>();
            for (Node node : snapshot()) if (node.clickable) clickable.add(node);
            final StringBuilder sb = new StringBuilder("ticket #181: category tab ")
                    .append(category.name()).append(" '").append(label).append("' not found. ")
                    .append("Clickable nodes (").append(clickable.size()).append("):\n");
            for (int i = 0; i < clickable.size(); i++) {
                if (i > 0) sb.append('\n');
                sb.append("  ").append(clickable.get(i).describe())
                        .append(" speakable=\"").append(clickable.get(i).speakable()).append('"');
            }
            return sb.toString();
        }

        private Node awaitEnabledExport(String label) {
            final long deadline = SystemClock.elapsedRealtime() + 30_000L;
            Node last = null;
            while (SystemClock.elapsedRealtime() < deadline) {
                last = null;
                for (Node node : snapshot()) {
                    if (!node.visible || !node.info.isClickable() || !anyPieceContains(node, label)) continue;
                    if (last == null || node.bounds.top < last.bounds.top) last = node;
                }
                if (last != null && last.info.isEnabled()) return last;
                SystemClock.sleep(250L);
            }
            throw new AssertionError("ticket #181: enabled '" + label
                    + "' button not found within 30 s (last="
                    + (last == null ? "null" : last.describe()) + ")");
        }

        private static boolean anyPieceContains(Node node, String label) {
            for (String piece : node.pieces()) if (piece.contains(label)) return true;
            return false;
        }

        private void click(Node node, String what) {
            if (!node.info.performAction(AccessibilityNodeInfo.ACTION_CLICK)) {
                throw new IllegalStateException("ticket #181: ACTION_CLICK rejected on " + what
                        + ": " + node.describe());
            }
            journal.add("CLICK: " + what + " -> " + node.describe());
            settle();
        }

        // ---- tree access ---------------------------------------------------------------------

        private int activeWindowId() {
            final AccessibilityNodeInfo root = automation.getRootInActiveWindow();
            return root == null ? -1 : root.getWindowId();
        }

        private AccessibilityNodeInfo rootNode() {
            for (int attempt = 0; attempt < 20; attempt++) {
                final AccessibilityNodeInfo root = automation.getRootInActiveWindow();
                if (root != null) {
                    root.refresh();
                    return root;
                }
                SystemClock.sleep(250L);
            }
            throw new AssertionError("ticket #181: no active window root after 5 s");
        }

        private List<Node> snapshot() {
            final List<Node> out = new ArrayList<>();
            collect(rootNode(), null, 0, out);
            return out;
        }

        private static void collect(AccessibilityNodeInfo info, Node parent, int depth, List<Node> out) {
            final Node node = new Node(info, parent, depth);
            out.add(node);
            if (parent != null) parent.children.add(node);
            for (int i = 0; i < info.getChildCount(); i++) {
                final AccessibilityNodeInfo child = info.getChild(i);
                if (child == null) continue;
                collect(child, node, depth + 1, out);
            }
        }

        /** Idle + two consecutive identical tree fingerprints (panel springs/scroll animations). */
        private void settle() {
            instrumentation.waitForIdleSync();
            try {
                automation.waitForIdle(500L, 5_000L);
            } catch (TimeoutException ignored) {
                // A busy preview loop keeps the window "active"; the fingerprint loop below covers it.
            }
            int previous = fingerprint(snapshot());
            final long deadline = SystemClock.elapsedRealtime() + SETTLE_TIMEOUT_MS;
            while (SystemClock.elapsedRealtime() < deadline) {
                SystemClock.sleep(300L);
                final int current = fingerprint(snapshot());
                if (current == previous) return;
                previous = current;
            }
        }

        private static int fingerprint(List<Node> nodes) {
            int acc = 17;
            for (Node n : nodes) {
                acc = acc * 31 + n.className.hashCode() + n.bounds.hashCode() + n.text.hashCode()
                        + n.contentDescription.hashCode() + (n.visible ? 1 : 0);
            }
            return acc;
        }

        private boolean waitUntil(long timeoutMillis, BooleanSupplier condition) {
            final long deadline = SystemClock.elapsedRealtime() + timeoutMillis;
            while (SystemClock.elapsedRealtime() < deadline) {
                instrumentation.waitForIdleSync();
                if (condition.getAsBoolean()) return true;
                SystemClock.sleep(25L);
            }
            return false;
        }

        private void waitForDestination(String expected, long timeoutMillis) {
            final boolean reached = waitUntil(timeoutMillis,
                    () -> expected.equals(Ticket139EditorTestBridge.currentDestination()));
            if (!reached) {
                throw new IllegalStateException("ticket #181 destination timed out: expected="
                        + expected + " actual=" + Ticket139EditorTestBridge.currentDestination());
            }
        }

        private void waitForLiveReadyAfter(long previous, long timeoutMillis) {
            final boolean ready = waitUntil(timeoutMillis,
                    () -> Ticket139EditorTestBridge.liveEditorReadyGeneration() > previous);
            if (!ready) {
                throw new IllegalStateException(
                        "ticket #181 live editor ready timed out after generation " + previous);
            }
        }

        // ---- evidence ------------------------------------------------------------------------

        private void writeTree(String id, List<Node> nodes, List<Finding> screenFindings,
                List<String> critical) throws IOException {
            final StringBuilder sb = new StringBuilder();
            sb.append("screen=").append(id).append(" density=").append(density)
                    .append(" nodes=").append(nodes.size()).append('\n');
            if (!critical.isEmpty()) {
                sb.append("critical: ").append(TextUtils.join("; ", critical)).append('\n');
            }
            sb.append("findings=").append(screenFindings.size()).append('\n');
            for (Finding finding : screenFindings) sb.append("  ").append(finding).append('\n');
            sb.append("tree:\n");
            for (Node node : nodes) {
                for (int i = 0; i < node.depth; i++) sb.append("  ");
                sb.append(dumpLine(node)).append('\n');
            }
            writeText(new File(evidence, id + ".txt"), sb.toString());
        }

        private static String dumpLine(Node node) {
            final StringBuilder flags = new StringBuilder();
            if (node.clickable) flags.append(" clickable");
            if (node.info.isCheckable()) flags.append(" checkable");
            if (node.info.isSelected()) flags.append(" selected");
            flags.append(node.info.isEnabled() ? " enabled" : " disabled");
            if (node.visible) flags.append(" visible");
            if (node.info.isScrollable()) flags.append(" scrollable");
            if (node.info.getRangeInfo() != null) flags.append(" range");
            if (node.info.isEditable()) flags.append(" editable");
            final Rect b = node.bounds;
            return node.className + " text=\"" + node.text + "\" cd=\"" + node.contentDescription
                    + "\" state=\"" + node.stateDescription + "\" role=\"" + node.roleDescription
                    + "\" bounds=[" + b.left + "," + b.top + "-" + b.right + "," + b.bottom + "]"
                    + flags;
        }

        private void writeScreenshot(String id) throws IOException {
            final Bitmap bitmap = automation.takeScreenshot();
            if (bitmap == null) {
                journal.add("SCREENSHOT " + id + ": takeScreenshot() returned null");
                return;
            }
            try (FileOutputStream out = new FileOutputStream(new File(evidence, id + ".png"))) {
                bitmap.compress(Bitmap.CompressFormat.PNG, 100, out);
            }
            bitmap.recycle();
        }

        private List<Finding> bySeverity(Severity severity) {
            final List<Finding> out = new ArrayList<>();
            for (Finding finding : findings) if (finding.severity == severity) out.add(finding);
            return out;
        }

        String report() {
            final List<Finding> errors = bySeverity(Severity.ERROR);
            final StringBuilder sb = new StringBuilder();
            for (String line : screenLines) sb.append(line).append('\n');
            if (errors.isEmpty()) {
                sb.append("TICKET181_ACCESSIBILITY: PASS\n");
            } else {
                sb.append("TICKET181_ACCESSIBILITY: FAIL\n");
                for (Finding error : errors) sb.append(error).append('\n');
            }
            return sb.toString();
        }

        void writeSummary(Throwable failure) {
            final StringBuilder sb = new StringBuilder(report());
            if (failure != null) sb.append("ABORTED: ").append(failure).append('\n');
            sb.append("warnings:\n");
            for (Finding warning : bySeverity(Severity.WARN)) {
                sb.append("  ").append(warning).append('\n');
            }
            sb.append("journal:\n");
            for (String entry : journal) sb.append("  ").append(entry).append('\n');
            try {
                writeText(new File(evidence, "summary.txt"), sb.toString());
            } catch (Throwable ignored) {
                // Best effort (Kotlin runCatching): a failing summary must not mask the real cause.
            }
        }
    }
}
