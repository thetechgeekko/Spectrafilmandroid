/*
 * Spektrafilm for Android — bounded editor-session persistence. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * The document stores identifiers and editable model state only. Bitmaps, decoded/native buffers,
 * URI grants, exception text and other process-owned resources are deliberately excluded. The
 * source grant remains authoritative in SourceAccess; this document only lets a recreated editor
 * resume the same cursor, bounded history and UI selection after that grant is reconciled.
 */
package com.spectrafilm.app

import android.content.Context
import java.io.File
import java.io.IOException
import java.net.URI
import java.math.BigDecimal
import java.math.BigInteger
import java.util.concurrent.CancellationException
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject

internal enum class EditorOverlayTool {
    NONE,
    CROP,
    MASK_GEOMETRY,
    MASK_SAMPLE_COLOR,
    MASK_SAMPLE_LUMINANCE,
    WHITE_BALANCE_SAMPLE,
}

/**
 * Tool selection is durable even when a tool's in-progress gesture is not. CropOverlay and
 * MaskGeometryOverlay create a new draft from committed ParamsState/mask geometry when recomposed;
 * reopening either tool is therefore a clean no-op until the user gestures and confirms.
 */
internal fun restorableEditorOverlay(saved: EditorOverlayTool): EditorOverlayTool = saved

internal enum class EditorExportPhase {
    IDLE,
    RUNNING,
    SUCCESS,
    FAILURE,
    CANCELLED,
    RECONCILING,
}

internal data class EditorSourceState(
    val uri: String?,
    val kind: SourceKind,
    val displayName: String,
    val authorizationRequired: Boolean,
)

private val VALID_EDITOR_ROTATIONS = setOf(0, 90, 180, 270)

/** Small SavedState fallback. A valid versioned session always takes precedence over this value. */
internal data class EditorSavedFallback(
    val source: EditorSourceState?,
    val rotationDegrees: Int,
) {
    companion object {
        val Empty = EditorSavedFallback(source = null, rotationDegrees = 0)
    }
}

/** One authoritative startup value, produced only after SourceAccess reconciliation completes. */
internal data class EditorRestoration(
    val document: EditorSessionDocument?,
    val source: EditorSourceState,
    val rotationDegrees: Int,
    val retainedSessionCursor: Boolean,
    val usedSavedFallback: Boolean,
)

private fun demoEditorSource() = EditorSourceState(
    uri = null,
    kind = SourceKind.DEMO,
    displayName = "synthetic demo image",
    authorizationRequired = false,
)

private fun PersistedSourceRef.toEditorSource(authorizationRequired: Boolean): EditorSourceState =
    EditorSourceState(
        uri = uri,
        kind = SourceKind.valueOf(kind),
        displayName = displayName,
        authorizationRequired = authorizationRequired,
    )

/**
 * Reconcile all restoration inputs before EditorScreen enters composition. Durable SourceAccess
 * identity is authoritative; a complete session supplies the cursor only for that exact subject.
 * SavedState is a last-resort, identity-checked fallback and can never override a valid session.
 */
internal fun reconcileEditorRestoration(
    session: EditorSessionDocument?,
    sourceRestore: SourceRestoreResult,
    savedFallback: EditorSavedFallback,
): EditorRestoration {
    val reconciledSessionSource = session?.source?.let { saved ->
        when (sourceRestore) {
            SourceRestoreResult.Demo -> if (saved.kind == SourceKind.DEMO) demoEditorSource() else null
            is SourceRestoreResult.Ready -> sourceRestore.ref
                .takeIf { saved.uri == it.uri && saved.kind.name == it.kind }
                ?.toEditorSource(authorizationRequired = false)
            is SourceRestoreResult.NeedsAuthorization -> sourceRestore.ref
                .takeIf { saved.uri == it.uri && saved.kind.name == it.kind }
                ?.toEditorSource(authorizationRequired = true)
            SourceRestoreResult.None,
            is SourceRestoreResult.Invalid,
            -> if (saved.kind == SourceKind.DEMO) {
                demoEditorSource()
            } else {
                saved.copy(authorizationRequired = true)
            }
        }
    }
    if (session != null && reconciledSessionSource != null) {
        val reconciled = session.copy(source = reconciledSessionSource)
        return EditorRestoration(
            document = reconciled,
            source = reconciledSessionSource,
            rotationDegrees = reconciled.current.rotationDegrees,
            retainedSessionCursor = true,
            usedSavedFallback = false,
        )
    }

    val fallbackSource = savedFallback.source?.let { saved ->
        when (sourceRestore) {
            SourceRestoreResult.Demo -> if (saved.kind == SourceKind.DEMO) demoEditorSource() else null
            is SourceRestoreResult.Ready -> sourceRestore.ref
                .takeIf { saved.uri == it.uri && saved.kind.name == it.kind }
                ?.toEditorSource(authorizationRequired = false)
            is SourceRestoreResult.NeedsAuthorization -> sourceRestore.ref
                .takeIf { saved.uri == it.uri && saved.kind.name == it.kind }
                ?.toEditorSource(authorizationRequired = true)
            SourceRestoreResult.None,
            is SourceRestoreResult.Invalid,
            -> if (saved.kind == SourceKind.DEMO) {
                demoEditorSource()
            } else {
                saved.copy(authorizationRequired = true)
            }
        }
    }
    if (fallbackSource != null && savedFallback.rotationDegrees in VALID_EDITOR_ROTATIONS) {
        return EditorRestoration(
            document = null,
            source = fallbackSource,
            rotationDegrees = savedFallback.rotationDegrees,
            retainedSessionCursor = false,
            usedSavedFallback = true,
        )
    }

    val authoritativeSource = when (sourceRestore) {
        SourceRestoreResult.Demo -> demoEditorSource()
        is SourceRestoreResult.Ready -> sourceRestore.ref.toEditorSource(authorizationRequired = false)
        is SourceRestoreResult.NeedsAuthorization ->
            sourceRestore.ref.toEditorSource(authorizationRequired = true)
        SourceRestoreResult.None,
        is SourceRestoreResult.Invalid,
        -> demoEditorSource()
    }
    return EditorRestoration(
        document = null,
        source = authoritativeSource,
        rotationDegrees = 0,
        retainedSessionCursor = false,
        usedSavedFallback = false,
    )
}

/** Stable selection even when the editor has no masks; otherwise always a valid list index. */
internal fun clampSelectedMaskIndex(index: Int, maskCount: Int): Int = when {
    maskCount <= 0 -> 0
    else -> index.coerceIn(0, maskCount - 1)
}

/** Whether a durable grant reconciliation still names the session's exact editing subject. */
internal fun shouldRetainEditorCursor(
    saved: EditorSourceState,
    restored: SourceRestoreResult,
): Boolean = when (restored) {
    SourceRestoreResult.Demo -> saved.kind == SourceKind.DEMO
    is SourceRestoreResult.Ready ->
        saved.uri == restored.ref.uri && saved.kind.name == restored.ref.kind
    is SourceRestoreResult.NeedsAuthorization ->
        saved.uri == restored.ref.uri && saved.kind.name == restored.ref.kind
    // No durable capability is expected for a transient picker after process death. Preserve the
    // edits and require reauthorization instead of silently switching to the demo subject.
    SourceRestoreResult.None,
    is SourceRestoreResult.Invalid,
    -> true
}

internal data class EditorToolState(
    val category: Category?,
    val overlay: EditorOverlayTool,
    val maskIndex: Int,
)

internal data class EditorExportState(
    val sheetOpen: Boolean,
    val options: ExportOptions,
    val keepGps: Boolean,
    val phase: EditorExportPhase,
    val runtimeRunId: Long?,
)

/**
 * Whether the checkpointed export sheet should be on screen after a restore.
 *
 * The sheet is journalled so an Activity recreation — a rotation, a theme change, a
 * configuration change mid-export — does not throw away what the user was configuring. A COLD
 * start is different: reopening a modal the user last saw hours ago hides the photo behind a
 * sheet they did not open, which reads as "the editor rendered nothing". So on a cold start the
 * sheet returns only when an export is actually live and the user needs to see its progress.
 */
internal fun exportSheetVisibleAtRestore(
    checkpointed: Boolean,
    phase: EditorExportPhase,
    coldStart: Boolean,
): Boolean = checkpointed && (!coldStart || phase != EditorExportPhase.IDLE)

/** A process-owned export cannot still be running when its exact run id is absent after restore. */
internal fun reconcileRestoredExport(
    saved: EditorExportState,
    activeRuntimeRunId: Long?,
): EditorExportState = when {
    // A checkpointed RUNNING export whose run the process runtime no longer carries (or whose
    // id changed) is orphaned: its durable MediaStore transaction is reconciled by recovery,
    // never blindly retried by a recreated UI.
    saved.phase == EditorExportPhase.RUNNING && saved.runtimeRunId != activeRuntimeRunId ->
        saved.copy(phase = EditorExportPhase.RECONCILING, runtimeRunId = null, sheetOpen = false)
    // Ticket #139 adoption: a run the runtime is still executing for THIS editor's
    // publication-authorized source identity is the editor's own export. The recreated
    // cursor resumes it instead of restarting at the checkpointed pre-launch phase.
    saved.phase != EditorExportPhase.RUNNING && activeRuntimeRunId != null ->
        saved.copy(phase = EditorExportPhase.RUNNING, runtimeRunId = activeRuntimeRunId)
    else -> saved
}

internal data class EditorPresetState(
    val baseJson: String?,
    val fullJson: String?,
    val amount: Float,
    val clipboardJson: String?,
    val selectedPreset: String,
    val presetName: String,
)

internal data class EditorSessionDocument(
    val source: EditorSourceState,
    val current: EditSnapshot,
    val committed: EditSnapshot?,
    val history: EditHistoryState,
    val tool: EditorToolState,
    val preset: EditorPresetState,
    val export: EditorExportState,
)

/** Compare a live/restored cursor to an oracle captured before that editor was allowed to launch. */
internal fun verifyCompleteEditorCursor(
    actual: EditorSessionDocument,
    oracle: EditorSessionDocument,
    expectedPhase: EditorExportPhase,
    expectedRuntimeRunId: Long? = null,
) {
    val runtimeRunId = if (expectedPhase == EditorExportPhase.RUNNING) {
        requireNotNull(expectedRuntimeRunId) { "RUNNING cursor oracle requires an exact run id" }
            .also { require(it > 0L) { "cursor oracle run id must be positive" } }
    } else {
        require(expectedRuntimeRunId == null) { "only RUNNING cursor oracle may carry a run id" }
        null
    }
    check(actual.source == oracle.source) { "restored source differs from pre-launch oracle" }
    check(actual.current == oracle.current) { "restored current snapshot differs from pre-launch oracle" }
    check(actual.committed == oracle.committed) {
        "restored committed snapshot differs from pre-launch oracle"
    }
    check(actual.history.undo == oracle.history.undo) {
        "restored undo branch differs from pre-launch oracle " +
            "(actualCount=${actual.history.undo.size}, oracleCount=${oracle.history.undo.size}, " +
            "actualRotations=${actual.history.undo.map(EditSnapshot::rotationDegrees)}, " +
            "oracleRotations=${oracle.history.undo.map(EditSnapshot::rotationDegrees)}, " +
            "actualKinds=${actual.history.undo.map { snapshot ->
                when {
                    snapshot == oracle.current -> "CURRENT"
                    snapshot in oracle.history.undo -> "UNDO"
                    snapshot in oracle.history.redo -> "REDO"
                    else -> "OTHER"
                }
            }})"
    }
    check(actual.history.redo == oracle.history.redo) {
        "restored redo branch differs from pre-launch oracle"
    }
    check(actual.tool == oracle.tool) { "restored tool cursor differs from pre-launch oracle" }
    check(actual.preset == oracle.preset) { "restored preset cursor differs from pre-launch oracle" }
    val expectedExport = oracle.export.copy(
        phase = expectedPhase,
        runtimeRunId = runtimeRunId,
    )
    check(actual.export == expectedExport) {
        "restored export cursor differs from pre-launch oracle " +
            "(actual=${actual.export}, expected=$expectedExport)"
    }
}

/** Catalog compatibility is external to the JSON schema and must cover every restorable branch. */
internal fun EditorSessionDocument.referencesUnavailableProfiles(
    availableProfiles: Set<String>,
): Boolean {
    if (availableProfiles.isEmpty()) return false
    val snapshots = buildList {
        add(current.paramsJson)
        committed?.paramsJson?.let(::add)
        history.undo.forEach { add(it.paramsJson) }
        history.redo.forEach { add(it.paramsJson) }
        preset.baseJson?.let(::add)
        preset.fullJson?.let(::add)
        preset.clipboardJson?.let(::add)
    }
    return snapshots.any { json ->
        runCatching {
            val params = ParamsState()
            Presets.decode(AtomicJsonStore.parseObject(json), params)
            params.filmProfile !in availableProfiles || params.printProfile !in availableProfiles
        }.getOrDefault(true)
    }
}

internal class UnsupportedEditorSessionException(message: String) : Exception(message)

internal data class EditorSessionEncodedCost(
    val nodeCount: Int,
    val maxDepth: Int,
    val utf8Bytes: Long,
)

internal object EditorSessionDocumentCodec {
    const val SCHEMA_ID = "org.spektrafilm.editor-session"
    const val SCHEMA_VERSION = 1
    const val MAX_DOCUMENT_BYTES = 32 * 1024 * 1024
    private const val MAX_SOURCE_URI_CHARS = 16 * 1024
    private const val MAX_SOURCE_NAME_CHARS = 512
    private const val MAX_EXPORT_NAME_CHARS = 4_096
    private const val FIXED_ROOT_NODES = 30L
    private const val FIXED_ROOT_MAX_DEPTH = 4
    private const val DIRECT_PARAMS_DEPTH_OFFSET = 2
    private const val HISTORY_PARAMS_DEPTH_OFFSET = 4
    private const val HISTORY_ROTATION_DEPTH = 5
    private const val JSON_NULL_BYTES = 4L
    private const val JSON_SCALAR_WRAPPER_BYTES = 6L // {"v":<value>}

    private val limits = JsonStructureLimits(
        maxDepth = 48,
        maxNodes = 600_000,
        maxArrayLength = 128,
        maxObjectKeys = 4_096,
        maxStringChars = MAX_SOURCE_URI_CHARS,
        maxTokenChars = 128,
        maxInputChars = MAX_DOCUMENT_BYTES,
    )
    private val snapshotLimits = limits.copy(
        // Two mandatory snapshots must always leave room for the fixed session envelope. Optional
        // history/preset copies are removed by retainEncodableEditorSession when the aggregate root
        // reaches its stricter bound.
        maxDepth = 40,
        maxNodes = 200_000,
        maxInputChars = AtomicJsonStore.MAX_PRESET_BYTES,
    )

    private data class SnapshotEncodedCost(
        val paramsNodes: Int,
        val paramsMaxDepth: Int,
        val encodedSnapshotBytes: Long,
        val encodedParamsBytes: Long,
    ) {
        val snapshotNodes: Long get() = paramsNodes.toLong() + 2L
    }

    private data class MeasuredSnapshot(
        val cost: SnapshotEncodedCost,
        val params: ParamsState?,
    )

    private data class RetentionSelection(
        val undoDropped: Int,
        val redoDropped: Int,
        val dropClipboard: Boolean,
        val dropAnchors: Boolean,
    )

    private class SnapshotSuffixCosts(private val values: List<SnapshotEncodedCost>) {
        private val suffixNodes = LongArray(values.size + 1)
        private val suffixBytes = LongArray(values.size + 1)
        private val suffixDepth = IntArray(values.size + 1)

        init {
            for (index in values.lastIndex downTo 0) {
                suffixNodes[index] = suffixNodes[index + 1] + values[index].snapshotNodes
                suffixBytes[index] = suffixBytes[index + 1] + values[index].encodedSnapshotBytes
                suffixDepth[index] = maxOf(
                    suffixDepth[index + 1],
                    values[index].paramsMaxDepth +
                        EditorSessionDocumentCodec.HISTORY_PARAMS_DEPTH_OFFSET,
                    EditorSessionDocumentCodec.HISTORY_ROTATION_DEPTH,
                )
            }
        }

        fun nodeCount(dropped: Int): Long = suffixNodes[dropped]

        fun maxDepth(dropped: Int): Int = suffixDepth[dropped]

        fun arrayBytes(dropped: Int): Long {
            val count = values.size - dropped
            return EditorSessionDocumentCodec.jsonArrayBytes(count, suffixBytes[dropped])
        }
    }

    private data class CostProfile(
        val document: EditorSessionDocument,
        val current: SnapshotEncodedCost,
        val committed: SnapshotEncodedCost?,
        val undo: SnapshotSuffixCosts,
        val redo: SnapshotSuffixCosts,
        val undoCount: Int,
        val redoCount: Int,
        val base: SnapshotEncodedCost?,
        val full: SnapshotEncodedCost?,
        val clipboard: SnapshotEncodedCost?,
        val sourceBytes: Long,
        val toolBytes: Long,
        val exportBytes: Long,
    ) {
        val maximumRetentionStep: Int = undoCount + redoCount +
            (if (clipboard != null) 1 else 0) + (if (base != null) 1 else 0)

        fun selection(step: Int): RetentionSelection {
            require(step in 0..maximumRetentionStep)
            val undoDropped = minOf(step, undoCount)
            val redoDropped = minOf((step - undoCount).coerceAtLeast(0), redoCount)
            var auxiliaryStep = (step - undoCount - redoCount).coerceAtLeast(0)
            val dropClipboard = clipboard != null && auxiliaryStep-- > 0
            val dropAnchors = base != null && auxiliaryStep > 0
            return RetentionSelection(undoDropped, redoDropped, dropClipboard, dropAnchors)
        }

        fun cost(selection: RetentionSelection): EditorSessionEncodedCost {
            val retainedBase = base.takeUnless { selection.dropAnchors }
            val retainedFull = full.takeUnless { selection.dropAnchors }
            val retainedClipboard = clipboard.takeUnless { selection.dropClipboard }
            val undoBytes = undo.arrayBytes(selection.undoDropped)
            val redoBytes = redo.arrayBytes(selection.redoDropped)
            val historyBytes = EditorSessionDocumentCodec.jsonObjectBytes(
                "undo" to undoBytes,
                "redo" to redoBytes,
            )
            val presetBytes = EditorSessionDocumentCodec.jsonObjectBytes(
                "base" to (retainedBase?.encodedParamsBytes ?: EditorSessionDocumentCodec.JSON_NULL_BYTES),
                "full" to (retainedFull?.encodedParamsBytes ?: EditorSessionDocumentCodec.JSON_NULL_BYTES),
                "amount" to EditorSessionDocumentCodec.jsonScalarBytes(
                    if (selection.dropAnchors) 1.0 else document.preset.amount.toDouble(),
                ),
                "clipboard" to (
                    retainedClipboard?.encodedParamsBytes ?: EditorSessionDocumentCodec.JSON_NULL_BYTES
                ),
                "selectedPreset" to EditorSessionDocumentCodec.jsonScalarBytes(
                    document.preset.selectedPreset,
                ),
                "presetName" to EditorSessionDocumentCodec.jsonScalarBytes(document.preset.presetName),
            )
            val rootFields = ArrayList<Pair<String, Long>>(9).apply {
                add(
                    "schema" to EditorSessionDocumentCodec.jsonScalarBytes(
                        EditorSessionDocumentCodec.SCHEMA_ID,
                    ),
                )
                add(
                    "version" to EditorSessionDocumentCodec.jsonScalarBytes(
                        EditorSessionDocumentCodec.SCHEMA_VERSION,
                    ),
                )
                add("source" to sourceBytes)
                add("current" to current.encodedSnapshotBytes)
                committed?.let { add("committed" to it.encodedSnapshotBytes) }
                add("history" to historyBytes)
                add("tool" to toolBytes)
                add("preset" to presetBytes)
                add("export" to exportBytes)
            }

            var nodes = EditorSessionDocumentCodec.FIXED_ROOT_NODES + current.snapshotNodes +
                undo.nodeCount(selection.undoDropped) + redo.nodeCount(selection.redoDropped)
            committed?.let { nodes += it.snapshotNodes }
            nodes += retainedBase?.paramsNodes?.toLong() ?: 1L
            nodes += retainedFull?.paramsNodes?.toLong() ?: 1L
            nodes += retainedClipboard?.paramsNodes?.toLong() ?: 1L

            var maxDepth = EditorSessionDocumentCodec.FIXED_ROOT_MAX_DEPTH
            maxDepth = maxOf(
                maxDepth,
                current.paramsMaxDepth + EditorSessionDocumentCodec.DIRECT_PARAMS_DEPTH_OFFSET,
            )
            committed?.let {
                maxDepth = maxOf(
                    maxDepth,
                    it.paramsMaxDepth + EditorSessionDocumentCodec.DIRECT_PARAMS_DEPTH_OFFSET,
                )
            }
            maxDepth = maxOf(
                maxDepth,
                undo.maxDepth(selection.undoDropped),
                redo.maxDepth(selection.redoDropped),
            )
            retainedBase?.let {
                maxDepth = maxOf(
                    maxDepth,
                    it.paramsMaxDepth + EditorSessionDocumentCodec.DIRECT_PARAMS_DEPTH_OFFSET,
                )
            }
            retainedFull?.let {
                maxDepth = maxOf(
                    maxDepth,
                    it.paramsMaxDepth + EditorSessionDocumentCodec.DIRECT_PARAMS_DEPTH_OFFSET,
                )
            }
            retainedClipboard?.let {
                maxDepth = maxOf(
                    maxDepth,
                    it.paramsMaxDepth + EditorSessionDocumentCodec.DIRECT_PARAMS_DEPTH_OFFSET,
                )
            }
            return EditorSessionEncodedCost(
                nodeCount = nodes.toInt(),
                maxDepth = maxDepth,
                utf8Bytes = EditorSessionDocumentCodec.jsonObjectBytes(rootFields),
            )
        }
    }

    fun encode(document: EditorSessionDocument): String {
        val profile = buildCostProfile(document)
        val cost = profile.cost(profile.selection(0))
        enforceAggregateCost(cost)
        return materializeAndEncode(document, cost)
    }

    internal fun measureEncodedCost(document: EditorSessionDocument): EditorSessionEncodedCost {
        val profile = buildCostProfile(document)
        return profile.cost(profile.selection(0)).also(::enforceAggregateCost)
    }

    internal fun retainAndEncode(
        document: EditorSessionDocument,
        onFinalRootMaterialization: () -> Unit,
    ): EncodableEditorSession {
        // Profile every branch before deciding what to drop. An invalid oldest history/clipboard
        // entry therefore fails closed instead of disappearing merely because aggregate retention
        // would otherwise remove it. Parsed trees and their canonical strings die one at a time.
        val profile = buildCostProfile(document)
        var retainedSelection: RetentionSelection? = null
        var retainedCost: EditorSessionEncodedCost? = null
        for (step in 0..profile.maximumRetentionStep) {
            val selection = profile.selection(step)
            val cost = profile.cost(selection)
            if (aggregateCostFailure(cost) == null) {
                retainedSelection = selection
                retainedCost = cost
                break
            }
        }
        val selection = retainedSelection ?: throw DocumentLimitException(
            "mandatory current/source/committed editor cursor exceeds the session contract",
        )
        val cost = requireNotNull(retainedCost)
        val retained = document.copy(
            history = EditHistoryState(
                undo = document.history.undo.drop(selection.undoDropped),
                redo = document.history.redo.drop(selection.redoDropped),
            ),
            preset = document.preset.copy(
                baseJson = if (selection.dropAnchors) null else document.preset.baseJson,
                fullJson = if (selection.dropAnchors) null else document.preset.fullJson,
                amount = if (selection.dropAnchors) 1f else document.preset.amount,
                clipboardJson = if (selection.dropClipboard) null else document.preset.clipboardJson,
            ),
        )
        // This callback is deliberately reachable only from the single final whole-root path;
        // subtree profiling and arithmetic candidate selection can never satisfy its regression.
        onFinalRootMaterialization()
        return EncodableEditorSession(retained, materializeAndEncode(retained, cost))
    }

    private fun materializeAndEncode(
        document: EditorSessionDocument,
        expectedCost: EditorSessionEncodedCost,
    ): String {
        val root = JSONObject().apply {
            put("schema", SCHEMA_ID)
            put("version", SCHEMA_VERSION)
            put("source", sourceToJson(document.source))
            put("current", snapshotToJson(document.current))
            document.committed?.let { put("committed", snapshotToJson(it)) }
            put("history", JSONObject().apply {
                put("undo", snapshotsToJson(document.history.undo))
                put("redo", snapshotsToJson(document.history.redo))
            })
            put("tool", JSONObject().apply {
                put("category", document.tool.category?.name ?: JSONObject.NULL)
                put("overlay", document.tool.overlay.name)
                put("maskIndex", document.tool.maskIndex)
            })
            put("preset", presetToJson(document.preset))
            put("export", JSONObject().apply {
                put("sheetOpen", document.export.sheetOpen)
                put("keepGps", document.export.keepGps)
                put("phase", document.export.phase.name)
                put("runtimeRunId", document.export.runtimeRunId ?: JSONObject.NULL)
                put("options", exportOptionsToJson(document.export.options))
            })
        }
        val encoded = root.toString()
        // Validate the one final serialized root with the exact reader preflight. This covers every
        // aggregate limit (including token/string/object/array bounds) and proves the arithmetic
        // profile, without creating a second object tree.
        val actual = AtomicJsonStore.measureText(encoded, limits)
        check(actual.nodeCount == expectedCost.nodeCount) {
            "editor-session node profile disagrees with final serialization"
        }
        check(actual.maxDepth == expectedCost.maxDepth) {
            "editor-session depth profile disagrees with final serialization"
        }
        check(actual.utf8Bytes.toLong() == expectedCost.utf8Bytes) {
            "editor-session cost profile disagrees with final serialization"
        }
        enforceAggregateCost(expectedCost)
        return encoded
    }

    fun decode(text: String): EditorSessionDocument {
        val encodedBytes = AtomicJsonStore.utf8Length(text)
        if (encodedBytes > MAX_DOCUMENT_BYTES) {
            throw DocumentLimitException("editor session exceeds $MAX_DOCUMENT_BYTES bytes")
        }
        val root = AtomicJsonStore.parseObject(text, limits)
        require(root.optString("schema") == SCHEMA_ID) { "unsupported editor-session schema" }
        val version = exactLong(root.opt("version"), "version")
        if (version > SCHEMA_VERSION) {
            throw UnsupportedEditorSessionException("editor session version $version is newer than $SCHEMA_VERSION")
        }
        require(version == SCHEMA_VERSION.toLong()) { "unsupported editor-session version: $version" }

        val source = sourceFromJson(root.requiredObject("source"))
        val current = snapshotFromJson(root.requiredObject("current"))
        val committed = root.opt("committed")
            ?.takeUnless { it === JSONObject.NULL }
            ?.let { value ->
                require(value is JSONObject) { "committed must be an object" }
                snapshotFromJson(value)
            }
        val historyJson = root.requiredObject("history")
        val history = EditHistoryState(
            undo = snapshotsFromJson(historyJson.requiredArray("undo")),
            redo = snapshotsFromJson(historyJson.requiredArray("redo")),
        )
        val toolJson = root.requiredObject("tool")
        val category = toolJson.opt("category")
            ?.takeUnless { it === JSONObject.NULL }
            ?.let {
                require(it is String) { "tool category must be a string" }
                strictEnum<Category>(it, "tool category")
            }
        val tool = EditorToolState(
            category = category,
            overlay = strictEnum(toolJson.requiredString("overlay"), "overlay tool"),
            maskIndex = exactInt(toolJson.opt("maskIndex"), "maskIndex"),
        )
        val preset = presetFromJson(root.requiredObject("preset"))
        val exportJson = root.requiredObject("export")
        val runId = exportJson.opt("runtimeRunId")
            ?.takeUnless { it === JSONObject.NULL }
            ?.let { exactLong(it, "runtimeRunId") }
        val export = EditorExportState(
            sheetOpen = exportJson.requiredBoolean("sheetOpen"),
            options = exportOptionsFromJson(exportJson.requiredObject("options")),
            keepGps = exportJson.requiredBoolean("keepGps"),
            phase = strictEnum(exportJson.requiredString("phase"), "export phase"),
            runtimeRunId = runId,
        )
        return EditorSessionDocument(source, current, committed, history, tool, preset, export).also {
            validateDocument(it)
        }
    }

    private fun validateDocument(document: EditorSessionDocument) {
        buildCostProfile(document)
    }

    private fun buildCostProfile(document: EditorSessionDocument): CostProfile {
        validateSource(document.source)
        require(document.history.undo.size + document.history.redo.size <= EditHistory.DEFAULT_CAP) {
            "editor history exceeds ${EditHistory.DEFAULT_CAP} entries"
        }
        val current = measureSnapshot(document.current, retainParams = true)
        val committed = document.committed?.let { measureSnapshot(it) }
        val undo = document.history.undo.map { measureSnapshot(it) }
        val redo = document.history.redo.map { measureSnapshot(it) }
        // EditSnapshot retains the canonical params string after a disk round trip. Account that
        // durable representation, not a potentially smaller noncanonical input, so admission is
        // symmetric before and after process recreation.
        val historyBytes = (undo + redo).sumOf {
            it.cost.encodedParamsBytes + Int.SIZE_BYTES
        }
        require(historyBytes <= EditHistory.DEFAULT_BYTE_CAP.toLong()) {
            "editor history exceeds ${EditHistory.DEFAULT_BYTE_CAP} bytes"
        }
        require(document.tool.maskIndex in 0 until com.spectrafilm.app.masks.MaskJson.MAX_ADJUSTMENTS) {
            "mask selection is out of range"
        }
        val selectedMask = checkNotNull(current.params).localAdjustments
            .getOrNull(document.tool.maskIndex)
            ?.mask
        if (document.tool.overlay in MASK_TOOLS) {
            requireNotNull(selectedMask) { "selected mask no longer exists" }
        }
        when (document.tool.overlay) {
            EditorOverlayTool.MASK_SAMPLE_COLOR ->
                require(selectedMask?.colorRange != null) { "selected mask has no color range" }
            EditorOverlayTool.MASK_SAMPLE_LUMINANCE ->
                require(selectedMask?.luminanceRange != null) { "selected mask has no luminance range" }
            else -> Unit
        }
        validatePresetScalars(document.preset)
        val base = document.preset.baseJson?.let { measureParamsJson(it) }
        val full = document.preset.fullJson?.let { measureParamsJson(it) }
        val clipboard = document.preset.clipboardJson?.let { measureParamsJson(it) }
        validateExport(document.export)

        val toolJson = JSONObject().apply {
            put("category", document.tool.category?.name ?: JSONObject.NULL)
            put("overlay", document.tool.overlay.name)
            put("maskIndex", document.tool.maskIndex)
        }
        val exportJson = JSONObject().apply {
            put("sheetOpen", document.export.sheetOpen)
            put("keepGps", document.export.keepGps)
            put("phase", document.export.phase.name)
            put("runtimeRunId", document.export.runtimeRunId ?: JSONObject.NULL)
            put("options", exportOptionsToJson(document.export.options))
        }
        return CostProfile(
            document = document,
            current = current.cost,
            committed = committed?.cost,
            undo = SnapshotSuffixCosts(undo.map { it.cost }),
            redo = SnapshotSuffixCosts(redo.map { it.cost }),
            undoCount = undo.size,
            redoCount = redo.size,
            base = base?.cost,
            full = full?.cost,
            clipboard = clipboard?.cost,
            sourceBytes = encodedSmallObjectBytes(sourceToJson(document.source)),
            toolBytes = encodedSmallObjectBytes(toolJson),
            exportBytes = encodedSmallObjectBytes(exportJson),
        )
    }

    private fun validateSource(source: EditorSourceState) {
        require(source.displayName.isNotBlank() && source.displayName.length <= MAX_SOURCE_NAME_CHARS) {
            "invalid source display name"
        }
        when (source.kind) {
            SourceKind.DEMO -> {
                require(source.uri == null) { "demo source must not carry a URI" }
                require(!source.authorizationRequired) { "demo source cannot require authorization" }
            }
            SourceKind.PHOTO, SourceKind.RAW -> {
                val raw = requireNotNull(source.uri) { "non-demo source requires a URI" }
                require(raw.isNotBlank() && raw == raw.trim() && raw.length <= MAX_SOURCE_URI_CHARS) {
                    "invalid source URI"
                }
                val parsed = runCatching { URI(raw) }
                    .getOrElse { throw IllegalArgumentException("malformed source URI", it) }
                require(parsed.scheme.equals("content", ignoreCase = true)) {
                    "only content:// source URIs are accepted"
                }
                require(!parsed.rawAuthority.isNullOrBlank()) { "content URI has no authority" }
            }
        }
    }

    private fun measureSnapshot(
        snapshot: EditSnapshot,
        retainParams: Boolean = false,
    ): MeasuredSnapshot {
        require(snapshot.rotationDegrees in VALID_EDITOR_ROTATIONS) { "invalid editor rotation" }
        val measured = measureParamsJson(snapshot.paramsJson, retainParams)
        return measured.copy(
            cost = measured.cost.copy(
                encodedSnapshotBytes = jsonObjectBytes(
                    "params" to measured.cost.encodedParamsBytes,
                    "rotationDegrees" to jsonScalarBytes(snapshot.rotationDegrees),
                ),
            ),
        )
    }

    private fun measureParamsJson(
        json: String,
        retainParams: Boolean = false,
    ): MeasuredSnapshot {
        val parsed = AtomicJsonStore.parseMeasuredObject(json, snapshotLimits)
        if (parsed.measurement.utf8Bytes > AtomicJsonStore.MAX_PRESET_BYTES) {
            throw DocumentLimitException(
                "editor snapshot input is ${parsed.measurement.utf8Bytes} bytes; " +
                    "limit is ${AtomicJsonStore.MAX_PRESET_BYTES}",
            )
        }
        val canonical = canonicalParams(parsed.value)
        val params = ParamsState().also { Presets.decode(parsed.value, it) }
        // Canonical text is held only for this one subtree. Its byte count is exact for the same
        // JSONObject that final root materialization embeds; only the scalar count survives.
        return MeasuredSnapshot(
            cost = SnapshotEncodedCost(
                paramsNodes = parsed.measurement.nodeCount,
                paramsMaxDepth = parsed.measurement.maxDepth,
                encodedSnapshotBytes = 0L,
                encodedParamsBytes = canonical.utf8Bytes.toLong(),
            ),
            params = params.takeIf { retainParams },
        )
    }

    private data class CanonicalParams(val text: String, val utf8Bytes: Int)

    private fun canonicalParams(value: JSONObject): CanonicalParams {
        val text = value.toString()
        val utf8Bytes = AtomicJsonStore.utf8Length(text)
        if (utf8Bytes > AtomicJsonStore.MAX_PRESET_BYTES) {
            throw DocumentLimitException(
                "editor snapshot canonical JSON is $utf8Bytes bytes; " +
                    "limit is ${AtomicJsonStore.MAX_PRESET_BYTES}",
            )
        }
        return CanonicalParams(text, utf8Bytes)
    }

    private fun validateExport(export: EditorExportState) {
        require(export.options.jpegQuality in 1..100) { "invalid JPEG quality" }
        require(export.options.customLongEdge in 0..ExportOptions.MAX_CUSTOM_EDGE) {
            "invalid custom export edge"
        }
        require(export.options.customName.length <= MAX_EXPORT_NAME_CHARS) {
            "export name is too long"
        }
        require(export.runtimeRunId == null || export.runtimeRunId > 0L) {
            "invalid export run id"
        }
        require((export.phase == EditorExportPhase.RUNNING) == (export.runtimeRunId != null)) {
            "only a running export may carry a run id"
        }
    }

    private fun validatePresetScalars(preset: EditorPresetState) {
        require((preset.baseJson == null) == (preset.fullJson == null)) {
            "preset amount anchors must be present as a pair"
        }
        require(preset.amount.isFinite() && preset.amount in 0f..1f) {
            "invalid preset amount"
        }
        require(preset.selectedPreset.length <= 96 && preset.presetName.length <= 96) {
            "preset selection/name is too long"
        }
    }

    private fun encodedSmallObjectBytes(value: JSONObject): Long {
        AtomicJsonStore.validate(value, limits)
        return AtomicJsonStore.utf8Length(value.toString()).toLong()
    }

    private fun jsonScalarBytes(value: Any?): Long {
        val wrapper = JSONObject().put("v", value ?: JSONObject.NULL).toString()
        return AtomicJsonStore.utf8Length(wrapper).toLong() - JSON_SCALAR_WRAPPER_BYTES
    }

    private fun jsonObjectBytes(vararg fields: Pair<String, Long>): Long =
        jsonObjectBytes(fields.asList())

    private fun jsonObjectBytes(fields: List<Pair<String, Long>>): Long {
        var bytes = 2L // braces
        if (fields.size > 1) bytes += fields.size - 1L
        fields.forEach { (key, valueBytes) ->
            bytes += jsonScalarBytes(key) + 1L + valueBytes // quoted key + colon + value
        }
        return bytes
    }

    private fun jsonArrayBytes(count: Int, valuesBytes: Long): Long =
        2L + valuesBytes + (count - 1).coerceAtLeast(0)

    /*
     * Every variable subtree has already passed the same lexical contract, semantic Presets
     * decode, and canonical-byte measurement. The fixed envelope cannot exceed its collection,
     * string, or token limits, so only the three additive root limits vary with retention. The
     * one final serialized-root preflight below rechecks the complete contract as an invariant.
     */
    private fun aggregateCostFailure(cost: EditorSessionEncodedCost): String? = when {
        cost.maxDepth > limits.maxDepth -> "JSON depth exceeds ${limits.maxDepth}"
        cost.nodeCount > limits.maxNodes -> "JSON node count exceeds ${limits.maxNodes}"
        cost.utf8Bytes > MAX_DOCUMENT_BYTES ->
            "editor session is ${cost.utf8Bytes} bytes; limit is $MAX_DOCUMENT_BYTES"
        else -> null
    }

    private fun enforceAggregateCost(cost: EditorSessionEncodedCost) {
        aggregateCostFailure(cost)?.let { throw DocumentLimitException(it) }
    }

    private fun sourceToJson(source: EditorSourceState) = JSONObject().apply {
        put("uri", source.uri ?: JSONObject.NULL)
        put("kind", source.kind.name)
        put("displayName", source.displayName)
        put("authorizationRequired", source.authorizationRequired)
    }

    private fun sourceFromJson(value: JSONObject) = EditorSourceState(
        uri = value.opt("uri")?.takeUnless { it === JSONObject.NULL }?.let {
            require(it is String) { "source URI must be a string" }
            it
        },
        kind = strictEnum(value.requiredString("kind"), "source kind"),
        displayName = value.requiredString("displayName"),
        authorizationRequired = value.requiredBoolean("authorizationRequired"),
    )

    private fun snapshotToJson(snapshot: EditSnapshot) = JSONObject().apply {
        put("params", AtomicJsonStore.parseObject(snapshot.paramsJson, snapshotLimits))
        put("rotationDegrees", snapshot.rotationDegrees)
    }

    private fun snapshotFromJson(value: JSONObject): EditSnapshot {
        val params = value.requiredObject("params")
        val canonical = canonicalParams(params)
        // Validate now so a poisoned undo entry cannot survive until the user taps Undo.
        Presets.decode(params, ParamsState())
        return EditSnapshot(
            paramsJson = canonical.text,
            rotationDegrees = exactInt(value.opt("rotationDegrees"), "rotationDegrees"),
        )
    }

    private fun snapshotsToJson(snapshots: List<EditSnapshot>) = JSONArray().apply {
        snapshots.forEach { put(snapshotToJson(it)) }
    }

    private fun snapshotsFromJson(array: JSONArray): List<EditSnapshot> =
        List(array.length()) { index ->
            array.optJSONObject(index)
                ?.let(::snapshotFromJson)
                ?: throw IllegalArgumentException("history[$index] must be an object")
        }

    private fun exportOptionsToJson(options: ExportOptions) = JSONObject().apply {
        put("format", options.format.name)
        put("jpegQuality", options.jpegQuality)
        put("size", options.size.name)
        put("customLongEdge", options.customLongEdge)
        put("customName", options.customName)
    }

    private fun presetToJson(preset: EditorPresetState) = JSONObject().apply {
        put("base", preset.baseJson?.let(::parseSnapshotObject) ?: JSONObject.NULL)
        put("full", preset.fullJson?.let(::parseSnapshotObject) ?: JSONObject.NULL)
        put("amount", preset.amount.toDouble())
        put("clipboard", preset.clipboardJson?.let(::parseSnapshotObject) ?: JSONObject.NULL)
        put("selectedPreset", preset.selectedPreset)
        put("presetName", preset.presetName)
    }

    private fun presetFromJson(value: JSONObject) = EditorPresetState(
        baseJson = value.optionalObjectString("base"),
        fullJson = value.optionalObjectString("full"),
        amount = value.requiredFiniteFloat("amount"),
        clipboardJson = value.optionalObjectString("clipboard"),
        selectedPreset = value.requiredString("selectedPreset"),
        presetName = value.requiredString("presetName"),
    )

    private fun parseSnapshotObject(text: String): JSONObject =
        AtomicJsonStore.parseObject(text, snapshotLimits)

    private fun exportOptionsFromJson(value: JSONObject) = ExportOptions(
        format = strictEnum(value.requiredString("format"), "export format"),
        jpegQuality = exactInt(value.opt("jpegQuality"), "jpegQuality"),
        size = strictEnum(value.requiredString("size"), "export size"),
        customLongEdge = exactInt(value.opt("customLongEdge"), "customLongEdge"),
        customName = value.requiredString("customName"),
    )

    private fun JSONObject.requiredObject(key: String): JSONObject =
        optJSONObject(key) ?: throw IllegalArgumentException("$key must be an object")

    private fun JSONObject.requiredArray(key: String): JSONArray =
        optJSONArray(key) ?: throw IllegalArgumentException("$key must be an array")

    private fun JSONObject.requiredString(key: String): String {
        val value = opt(key)
        require(value is String) { "$key must be a string" }
        return value
    }

    private fun JSONObject.requiredBoolean(key: String): Boolean {
        val value = opt(key)
        require(value is Boolean) { "$key must be a boolean" }
        return value
    }

    private fun JSONObject.optionalObjectString(key: String): String? {
        val value = opt(key) ?: return null
        if (value === JSONObject.NULL) return null
        require(value is JSONObject) { "$key must be an object" }
        return value.toString()
    }

    private fun JSONObject.requiredFiniteFloat(key: String): Float {
        val value = opt(key)
        require(value is Number) { "$key must be a number" }
        return value.toDouble().toFloat().also { require(it.isFinite()) { "$key must be finite" } }
    }

    private inline fun <reified T : Enum<T>> strictEnum(value: String, field: String): T =
        runCatching { enumValueOf<T>(value) }
            .getOrElse { throw IllegalArgumentException("unsupported $field: $value") }

    private fun exactInt(value: Any?, field: String): Int {
        val number = exactInteger(value, field)
        require(number >= INT_MIN && number <= INT_MAX) { "$field is outside Int range" }
        return number.toInt()
    }

    private fun exactLong(value: Any?, field: String): Long {
        val number = exactInteger(value, field)
        require(number >= LONG_MIN && number <= LONG_MAX) { "$field is outside Long range" }
        return number.toLong()
    }

    private fun exactInteger(value: Any?, field: String): BigInteger = try {
        when (value) {
            is BigInteger -> value
            is BigDecimal -> value.toBigIntegerExact()
            is Byte, is Short, is Int, is Long -> BigInteger.valueOf((value as Number).toLong())
            is Float -> {
                require(value.isFinite()) { "$field must be finite" }
                BigDecimal(value.toString()).toBigIntegerExact()
            }
            is Double -> {
                require(value.isFinite()) { "$field must be finite" }
                BigDecimal(value.toString()).toBigIntegerExact()
            }
            else -> throw IllegalArgumentException("$field must be an integer")
        }
    } catch (fractional: ArithmeticException) {
        throw IllegalArgumentException("$field must be an integer", fractional)
    }

    private val MASK_TOOLS = setOf(
        EditorOverlayTool.MASK_GEOMETRY,
        EditorOverlayTool.MASK_SAMPLE_COLOR,
        EditorOverlayTool.MASK_SAMPLE_LUMINANCE,
    )
    private val INT_MIN = BigInteger.valueOf(Int.MIN_VALUE.toLong())
    private val INT_MAX = BigInteger.valueOf(Int.MAX_VALUE.toLong())
    private val LONG_MIN = BigInteger.valueOf(Long.MIN_VALUE)
    private val LONG_MAX = BigInteger.valueOf(Long.MAX_VALUE)
}

/**
 * Deterministic checkpoint retention for a structurally dense but otherwise valid live cursor.
 * The current/source/committed cursor is never discarded. Oldest undo entries are removed first,
 * then oldest redo entries, then the clipboard, then the paired preset-amount anchors. Dropping
 * anchors resets amount to 1 so a restored editor cannot display an unbacked interpolation value.
 */
internal data class EncodableEditorSession(
    val document: EditorSessionDocument,
    val encoded: String,
)

internal fun retainEncodableEditorSession(
    document: EditorSessionDocument,
    onFinalRootMaterialization: () -> Unit = {},
): EncodableEditorSession = EditorSessionDocumentCodec.retainAndEncode(
    document,
    onFinalRootMaterialization,
)

internal sealed interface EditorSessionReadResult {
    data object Missing : EditorSessionReadResult
    data class Loaded(val document: EditorSessionDocument) : EditorSessionReadResult
    data class CorruptQuarantined(val reason: String) : EditorSessionReadResult
    data class Unsupported(val reason: String) : EditorSessionReadResult
    data class Unavailable(val reason: String) : EditorSessionReadResult
}

internal enum class EditorSessionWriteAccess { WRITABLE, RECOVERING, PROTECTED }

/** A future-version document permanently protects disk; transient IO must prove recovery first. */
internal fun editorSessionWriteAccess(result: EditorSessionReadResult): EditorSessionWriteAccess =
    when (result) {
        is EditorSessionReadResult.Unsupported -> EditorSessionWriteAccess.PROTECTED
        is EditorSessionReadResult.Unavailable -> EditorSessionWriteAccess.RECOVERING
        else -> EditorSessionWriteAccess.WRITABLE
    }

internal fun editorSessionWritesProtected(result: EditorSessionReadResult): Boolean =
    editorSessionWriteAccess(result) == EditorSessionWriteAccess.PROTECTED

internal enum class EditorRecoveryConflictPolicy { RECOVERED_DURABLE, LIVE_MUTATION }

internal data class EditorRecoveryResolution(
    val restoration: EditorRestoration,
    val policy: EditorRecoveryConflictPolicy,
)

/**
 * Resolves a transient-read recovery before writes are enabled. A recovered durable document wins
 * unless the current editor has recorded a real user mutation; even then the live document must
 * reconcile to the currently durable source identity before it can win.
 */
internal fun resolveLoadedEditorSessionRecovery(
    recovered: EditorSessionDocument,
    sourceRestore: SourceRestoreResult,
    savedFallback: EditorSavedFallback,
    liveDocument: EditorSessionDocument?,
    liveMutated: Boolean,
): EditorRecoveryResolution {
    val recoveredRestoration = reconcileEditorRestoration(
        session = recovered,
        sourceRestore = sourceRestore,
        savedFallback = savedFallback,
    )
    if (liveMutated && liveDocument != null) {
        val liveRestoration = reconcileEditorRestoration(
            session = liveDocument,
            sourceRestore = sourceRestore,
            savedFallback = savedFallback,
        )
        if (liveRestoration.document != null) {
            return EditorRecoveryResolution(
                restoration = liveRestoration,
                policy = EditorRecoveryConflictPolicy.LIVE_MUTATION,
            )
        }
    }
    return EditorRecoveryResolution(
        restoration = recoveredRestoration,
        policy = EditorRecoveryConflictPolicy.RECOVERED_DURABLE,
    )
}

/** Per-source recipe authority. Source identity alone is deliberately never writable. */
internal sealed interface EditorRecipeAccess {
    data object None : EditorRecipeAccess
    data class Pending(val key: String, val generation: Long) : EditorRecipeAccess
    data class Writable(val key: String, val generation: Long) : EditorRecipeAccess
    data class Protected(val key: String, val generation: Long) : EditorRecipeAccess
}

internal fun classifyEditorRecipeAccess(
    key: String,
    generationBeforeRead: Long,
    generationAfterRead: Long,
    result: RecipeReadResult,
): EditorRecipeAccess {
    if (generationBeforeRead != generationAfterRead) {
        return EditorRecipeAccess.Pending(key, generationAfterRead)
    }
    return when (result) {
        is RecipeReadResult.Loaded,
        RecipeReadResult.Missing,
        is RecipeReadResult.CorruptQuarantined,
        -> EditorRecipeAccess.Writable(key, generationAfterRead)
        is RecipeReadResult.Unsupported,
        is RecipeReadResult.CorruptQuarantineFailed,
        is RecipeReadResult.IoFailure,
        -> EditorRecipeAccess.Protected(key, generationAfterRead)
    }
}

internal fun EditorRecipeAccess.writableGenerationFor(key: String): Long? =
    (this as? EditorRecipeAccess.Writable)
        ?.takeIf { it.key == key }
        ?.generation

/** Retry a transient startup read until storage itself proves whether writes are safe. */
internal suspend fun awaitEditorSessionWriteRecovery(
    read: suspend () -> EditorSessionReadResult,
    onUnavailable: suspend () -> Unit,
): EditorSessionReadResult {
    while (true) {
        val result = read()
        if (result !is EditorSessionReadResult.Unavailable) return result
        onUnavailable()
    }
}

/**
 * Keep an exactly-once owner unclaimed until its newest session marker is physically durable.
 * A transient app-private IO failure is retried from the coordinator's retained newest value;
 * cancellation and deterministic/programming failures still propagate to the caller.
 */
internal suspend fun awaitDurableEditorSessionCheckpoint(
    checkpoint: () -> Boolean,
    flush: suspend () -> Unit,
    onRetryableFailure: suspend (IOException) -> Unit,
): Boolean {
    while (true) {
        if (!checkpoint()) return false
        try {
            flush()
            return true
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: DocumentLimitException) {
            throw failure
        } catch (failure: IOException) {
            onRetryableFailure(failure)
        }
    }
}

internal object EditorSessionStore {
    private const val DIRECTORY = "editor-session"
    private const val FILE_NAME = "current.json"

    fun read(context: Context): EditorSessionReadResult {
        val file = try {
            file(context)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            return EditorSessionReadResult.Unavailable("editor-session storage is unavailable")
        }
        return read(file)
    }

    /**
     * File-level classification seam; read, decode, classification and any quarantine are one
     * path-locked transaction. A valid app replacement therefore cannot land after corrupt bytes
     * were observed and then be mistaken for those bytes during quarantine.
     */
    internal fun read(
        file: File,
        readText: (File) -> String = { target ->
            AtomicJsonStore.readUtf8(target, EditorSessionDocumentCodec.MAX_DOCUMENT_BYTES)
        },
        quarantine: (File) -> Unit = { target ->
            AtomicJsonStore.quarantine(target)
            Unit
        },
    ): EditorSessionReadResult = AtomicJsonStore.withPathLock(file) {
        try {
            // Do not preflight only the base path. AtomicFile.openRead() must first get the chance
            // to promote a valid legacy .bak left by an interrupted commit.
            EditorSessionReadResult.Loaded(EditorSessionDocumentCodec.decode(readText(file)))
        } catch (missing: java.io.FileNotFoundException) {
            // FileNotFoundException can also represent a transient access problem. It is Missing
            // only when neither readable AtomicFile generation exists after openRead recovery.
            if (!file.exists() && !File(file.path + ".bak").exists()) {
                EditorSessionReadResult.Missing
            } else {
                EditorSessionReadResult.Unavailable(
                    "editor-session storage is unavailable (${missing.javaClass.simpleName})",
                )
            }
        } catch (future: UnsupportedEditorSessionException) {
            // A downgrade must not overwrite a valid newer document.
            EditorSessionReadResult.Unsupported(future.message ?: "newer editor session")
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            if (!failure.isDeterministicEditorSessionContentFailure()) {
                return@withPathLock EditorSessionReadResult.Unavailable(
                    "editor-session storage is unavailable (${failure.javaClass.simpleName})",
                )
            }
            try {
                quarantine(file)
                EditorSessionReadResult.CorruptQuarantined(
                    "invalid editor session (${failure.javaClass.simpleName})",
                )
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (quarantineFailure: Exception) {
                EditorSessionReadResult.Unavailable(
                    "editor-session quarantine unavailable " +
                        "(${failure.javaClass.simpleName}/${quarantineFailure.javaClass.simpleName})",
                )
            }
        }
    }

    private fun Exception.isDeterministicEditorSessionContentFailure(): Boolean =
        this is DocumentLimitException ||
            this is java.nio.charset.CharacterCodingException ||
            this is IllegalArgumentException ||
            this is org.json.JSONException

    fun write(context: Context, document: EditorSessionDocument) {
        writeEncoded(context, EditorSessionDocumentCodec.encode(document))
    }

    internal fun writeEncoded(context: Context, encoded: String) {
        AtomicJsonStore.writeUtf8(
            file(context),
            encoded,
            EditorSessionDocumentCodec.MAX_DOCUMENT_BYTES,
        )
    }

    private fun file(context: Context): File = File(
        File(context.applicationContext.noBackupFilesDir, DIRECTORY),
        FILE_NAME,
    )
}

/**
 * Linearizable latest-only checkpoint boundary. An offer completed before [flushThenRead] starts is
 * necessarily written before that read. Concurrent offers may linearize immediately after the read,
 * while offers arriving during a blocked write are drained before a waiting read acquires the lock.
 */
internal class LatestCheckpointCoordinator<T>(
    private val writer: (T) -> Unit,
    private val afterGenerationAssigned: (Long) -> Unit = {},
) {
    private data class Versioned<T>(val generation: Long, val value: T)

    private val offeredGeneration = AtomicLong(0L)
    private val acceptedGeneration = AtomicLong(0L)
    private val pending = AtomicReference<Versioned<T>?>(null)
    private val diskLock = Any()

    fun offer(value: T) {
        val generation = offeredGeneration.incrementAndGet()
        afterGenerationAssigned(generation)
        while (true) {
            val accepted = acceptedGeneration.get()
            // Keep a monotonic high-water mark even after the slot has been drained. Otherwise an
            // older producer paused before publication could resume into the now-null slot and
            // regress disk after a newer checkpoint had already been written.
            if (generation <= accepted) return
            if (acceptedGeneration.compareAndSet(accepted, generation)) break
        }
        val offered = Versioned(generation, value)
        while (true) {
            val current = pending.get()
            // Producers overlap: a delayed older producer must never overwrite a newer value that
            // already published. Generation comparison makes the latest-only slot monotonic.
            if (current != null && current.generation > generation) return
            if (pending.compareAndSet(current, offered)) return
        }
    }

    fun flush() = synchronized(diskLock) {
        flushLatestLocked()
    }

    fun <R> flushThenRead(reader: () -> R): R = synchronized(diskLock) {
        while (true) {
            flushLatestLocked()
            val generationBeforeRead = acceptedGeneration.get()
            val result = reader()
            val generationAfterRead = acceptedGeneration.get()
            // A checkpoint offered after the drain but before/during the physical read must be
            // included. Retry under the same disk lock; the signal worker cannot reorder a write.
            if (generationBeforeRead == generationAfterRead && pending.get() == null) {
                return@synchronized result
            }
        }
        error("unreachable")
    }

    private fun flushLatestLocked() {
        while (true) {
            // Do not clear before the physical write. On failure the same checkpoint (or a newer
            // concurrently offered replacement) remains pending, and flush/flushThenRead propagate
            // instead of returning a stale disk read as success.
            val checkpoint = pending.get() ?: return
            writer(checkpoint.value)
            pending.compareAndSet(checkpoint, null)
        }
    }
}

/**
 * Serializes checkpoint ownership changes with the offer they authorize. Activity replacement can
 * overlap the outgoing host's final Compose effects; without this fence, that logically stale
 * callback can arrive after the replacement has restored or seeded a newer document and become the
 * latest physical write merely because it ran later on the main thread.
 */
internal class EditorSessionCheckpointOwners {
    private val lock = Any()
    private var activeOwner = 0L

    fun acquire(): Long = synchronized(lock) {
        check(activeOwner != Long.MAX_VALUE) { "editor-session owner generation exhausted" }
        ++activeOwner
    }

    fun retire() = synchronized(lock) {
        check(activeOwner != Long.MAX_VALUE) { "editor-session owner generation exhausted" }
        ++activeOwner
        Unit
    }

    fun offerIfCurrent(owner: Long, offer: () -> Unit): Boolean = synchronized(lock) {
        if (owner <= 0L || owner != activeOwner) return@synchronized false
        offer()
        true
    }
}

/**
 * Latest-only, process-owned checkpoint writer. A slider can settle repeatedly while storage is
 * slow, but only the newest immutable document is retained; no unbounded queue of multi-megabyte
 * history closures is possible. [read] shares the disk lock and flushes the pending document first,
 * so an Activity created immediately after recreation cannot observe the previous checkpoint.
 */
internal object EditorSessionCheckpointRuntime {
    private data class Pending(
        val context: Context,
        val document: EditorSessionDocument,
    )

    private val signal = Channel<Unit>(Channel.CONFLATED)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val owners = EditorSessionCheckpointOwners()
    private val coordinator = LatestCheckpointCoordinator<Pending>(writer = { checkpoint ->
        val retained = retainEncodableEditorSession(checkpoint.document)
        EditorSessionStore.writeEncoded(checkpoint.context, retained.encoded)
    })

    init {
        scope.launch {
            for (ignored in signal) {
                try {
                    coordinator.flush()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    // The coordinator retains the failed/newest value. A future checkpoint signal,
                    // explicit flush or restoration read retries it; never log document contents.
                    Diag.w("editor session save failed (${failure.javaClass.simpleName})")
                }
            }
        }
    }

    /** Acquires the sole checkpoint lease for a newly composed Activity host. */
    fun acquireOwner(): Long = owners.acquire()

    /**
     * Invalidates every existing host before a direct fixture/recovery replacement is written.
     * Callers must then [flush] the already accepted slot before installing that replacement.
     */
    fun retireOwners() = owners.retire()

    fun checkpoint(context: Context, document: EditorSessionDocument, owner: Long): Boolean {
        val accepted = owners.offerIfCurrent(owner) {
            coordinator.offer(Pending(context.applicationContext, document))
        }
        if (!accepted) return false
        if (signal.trySend(Unit).isFailure) {
            Diag.w("editor session checkpoint signal was rejected")
        }
        return true
    }

    /** Call from an IO dispatcher. */
    fun read(context: Context): EditorSessionReadResult = try {
        coordinator.flushThenRead { EditorSessionStore.read(context.applicationContext) }
    } catch (cancelled: CancellationException) {
        throw cancelled
    } catch (failure: Exception) {
        // A pending checkpoint could not become durable. Returning Unavailable is explicit and
        // prevents startup from treating the older on-disk generation as a successful restore.
        EditorSessionReadResult.Unavailable(
            "pending editor-session checkpoint is unavailable (${failure.javaClass.simpleName})",
        )
    }

    /** Call from an IO dispatcher before consuming an exactly-once terminal result. */
    fun flush() = coordinator.flush()
}
