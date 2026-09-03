/*
 * Spektrafilm for Android — in-session edit history (undo / redo). GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * A bounded two-stack undo/redo store over full-editing-state SNAPSHOTS. A snapshot
 * is the EXACT same JSON the recipe/preset layer already produces — Presets.toJsonString(state)
 * — paired with the editor-local manual rotation (which lives outside ParamsState). No
 * parallel params model: restoring a snapshot decodes that JSON back into the live
 * ParamsState via Presets.decode and re-applies the rotation, then the caller bumps
 * previewTick exactly like the recipe restore-on-open path.
 *
 * Coalescing is the CALLER's job (see MainActivity): this store only records discrete,
 * already-settled snapshots. The "commit" model used there pushes the PREVIOUS settled
 * snapshot when a new settled state differs, so one slider drag (which settles once) =
 * one undo entry.
 *
 * Stacks are bounded by both entry count and encoded byte size; oldest entries are dropped first.
 * canUndo/canRedo are Compose state so the top-bar buttons recompose enable/disable.
 */
package com.spectrafilm.app

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/** A single point-in-time editing state: the full params JSON + the manual rotation. */
data class EditSnapshot(val paramsJson: String, val rotationDegrees: Int)

/** Stable oldest-to-newest stack order used by the bounded editor-session document. */
data class EditHistoryState(
    val undo: List<EditSnapshot>,
    val redo: List<EditSnapshot>,
)

/** What the editor's settle effect should do: optionally [push] a baseline, then adopt [committed]. */
data class SettleAction(val push: EditSnapshot?, val committed: EditSnapshot?)

/**
 * The capture/settle decision, extracted from the editor so it is unit-testable. Given the
 * current `restoring` flag, the last `committed` snapshot, and the freshly-settled `now`
 * snapshot, returns what the settle effect should do (the caller always clears `restoring`).
 *
 * The subtle case is an edit that lands WITHIN the restore settle window: `restoring` is still
 * true but `now` already differs from the restored `committed` snapshot. The restored baseline
 * must be pushed so the edit stays undoable — otherwise the edit is silently adopted as the
 * committed state and that one undo step is lost (the bug this fixes).
 */
fun settleDecision(restoring: Boolean, committed: EditSnapshot?, now: EditSnapshot): SettleAction =
    when {
        // Programmatic restore (undo/redo). A pure restore (now == committed) records nothing;
        // an edit during the settle window pushes the restored baseline so it stays undoable.
        restoring -> SettleAction(push = if (now != committed) committed else null, committed = now)
        // First settle for this source/baseline: adopt without pushing (canUndo stays false).
        committed == null -> SettleAction(push = null, committed = now)
        // A real edit: push the PREVIOUS committed snapshot, then adopt the new one.
        now != committed -> SettleAction(push = committed, committed = now)
        // No change.
        else -> SettleAction(push = null, committed = committed)
    }

/**
 * Bounded undo/redo stacks of [EditSnapshot]s. Not thread-safe; all mutation happens on
 * the Compose/main thread (the editor never touches it off the main dispatcher).
 */
class EditHistory(
    private val cap: Int = DEFAULT_CAP,
    private val byteCap: Int = DEFAULT_BYTE_CAP,
) {

    init {
        require(cap > 0) { "history cap must be positive" }
        require(byteCap > 0) { "history byte cap must be positive" }
    }

    private val undoStack = ArrayDeque<EditSnapshot>()
    private val redoStack = ArrayDeque<EditSnapshot>()
    private var retainedBytes = 0L

    var canUndo by mutableStateOf(false)
        private set
    var canRedo by mutableStateOf(false)
        private set

    private fun refreshFlags() {
        canUndo = undoStack.isNotEmpty()
        canRedo = redoStack.isNotEmpty()
    }

    /**
     * Record [snapshot] as a new undo step. A fresh edit always invalidates the redo
     * branch (standard undo/redo semantics). When the cap is exceeded the OLDEST undo
     * entry is dropped so memory stays bounded.
     */
    fun push(snapshot: EditSnapshot) {
        undoStack.addLast(snapshot)
        retainedBytes += snapshot.retainedBytes()
        while (undoStack.size > cap) removeFirst(undoStack)
        clearStack(redoStack)
        trimToByteCap(prefer = undoStack)
        refreshFlags()
    }

    /**
     * Undo: returns the snapshot to restore, having moved [current] (the live state the
     * caller passes in) onto the redo stack. Returns null when there is nothing to undo.
     */
    fun undo(current: EditSnapshot): EditSnapshot? {
        val prev = removeLast(undoStack) ?: return null
        redoStack.addLast(current)
        retainedBytes += current.retainedBytes()
        while (redoStack.size > cap) removeFirst(redoStack)
        trimToByteCap(prefer = redoStack)
        refreshFlags()
        return prev
    }

    /**
     * Redo: returns the snapshot to restore, having moved [current] onto the undo stack.
     * Returns null when there is nothing to redo.
     */
    fun redo(current: EditSnapshot): EditSnapshot? {
        val next = removeLast(redoStack) ?: return null
        undoStack.addLast(current)
        retainedBytes += current.retainedBytes()
        while (undoStack.size > cap) removeFirst(undoStack)
        trimToByteCap(prefer = undoStack)
        refreshFlags()
        return next
    }

    /** Drop all history (e.g. on a source change). Leaves both stacks empty. */
    fun clear() {
        clearStack(undoStack)
        clearStack(redoStack)
        refreshFlags()
    }

    /** Capture both branches without exposing the mutable deques. */
    fun snapshotState(): EditHistoryState = EditHistoryState(
        undo = undoStack.toList(),
        redo = redoStack.toList(),
    )

    /** Replace both branches from a validated session document. */
    fun restoreState(state: EditHistoryState) {
        require(state.undo.size + state.redo.size <= cap) {
            "restored history exceeds $cap entries"
        }
        val restoredBytes = (state.undo + state.redo).sumOf { it.retainedBytes() }
        require(restoredBytes <= byteCap.toLong()) {
            "restored history exceeds $byteCap bytes"
        }
        clearStack(undoStack)
        clearStack(redoStack)
        state.undo.forEach {
            undoStack.addLast(it)
            retainedBytes += it.retainedBytes()
        }
        state.redo.forEach {
            redoStack.addLast(it)
            retainedBytes += it.retainedBytes()
        }
        refreshFlags()
    }

    private fun trimToByteCap(prefer: ArrayDeque<EditSnapshot>) {
        while (retainedBytes > byteCap) {
            val fallback = if (prefer === undoStack) redoStack else undoStack
            when {
                fallback.isNotEmpty() -> removeFirst(fallback)
                prefer.isNotEmpty() -> removeFirst(prefer)
                else -> break
            }
        }
    }

    private fun removeFirst(stack: ArrayDeque<EditSnapshot>) {
        retainedBytes -= stack.removeFirst().retainedBytes()
    }

    private fun removeLast(stack: ArrayDeque<EditSnapshot>): EditSnapshot? =
        stack.removeLastOrNull()?.also { retainedBytes -= it.retainedBytes() }

    private fun clearStack(stack: ArrayDeque<EditSnapshot>) {
        while (stack.isNotEmpty()) removeFirst(stack)
    }

    private fun EditSnapshot.retainedBytes(): Long =
        paramsJson.toByteArray(Charsets.UTF_8).size.toLong() + Int.SIZE_BYTES

    companion object {
        const val DEFAULT_CAP = 50
        const val DEFAULT_BYTE_CAP = 8 * 1024 * 1024
    }
}
