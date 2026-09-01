/*
 * Spektrafilm for Android — unit tests for the in-session undo/redo store. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * The first JVM unit tests in the project. EditHistory is pure logic (two bounded
 * stacks + Compose snapshot flags), so it runs on the plain JVM with no device or
 * Robolectric — `:app:testDebugUnitTest`.
 */
package com.spectrafilm.app

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class EditHistoryTest {

    private fun snap(tag: String, rot: Int = 0) = EditSnapshot(paramsJson = tag, rotationDegrees = rot)

    @Test
    fun emptyHistory_cannotUndoOrRedo() {
        val h = EditHistory()
        assertFalse(h.canUndo)
        assertFalse(h.canRedo)
        assertNull("undo on empty returns null", h.undo(snap("cur")))
        assertNull("redo on empty returns null", h.redo(snap("cur")))
    }

    @Test
    fun push_enablesUndo_andLeavesRedoDisabled() {
        val h = EditHistory()
        h.push(snap("a"))
        assertTrue(h.canUndo)
        assertFalse(h.canRedo)
    }

    @Test
    fun undo_returnsPushedSnapshot_andMovesCurrentToRedo() {
        val h = EditHistory()
        h.push(snap("a"))
        val restored = h.undo(snap("live"))
        assertEquals(snap("a"), restored)
        assertFalse("nothing left to undo", h.canUndo)
        assertTrue("the live state is now redoable", h.canRedo)
    }

    @Test
    fun redo_returnsSnapshot_andRestoresUndo() {
        val h = EditHistory()
        h.push(snap("a"))
        h.undo(snap("live"))                 // redo stack now holds "live"
        val redone = h.redo(snap("a"))       // caller passes the just-restored "a" as current
        assertEquals(snap("live"), redone)
        assertTrue(h.canUndo)
        assertFalse(h.canRedo)
    }

    @Test
    fun push_invalidatesRedoBranch() {
        val h = EditHistory()
        h.push(snap("a"))
        h.undo(snap("live"))                 // canRedo == true
        assertTrue(h.canRedo)
        h.push(snap("b"))                    // a fresh edit must drop the redo branch
        assertFalse("a new edit invalidates redo", h.canRedo)
        assertNull(h.redo(snap("cur")))
    }

    @Test
    fun undoRedo_roundTrip_preservesOrder() {
        val h = EditHistory()
        h.push(snap("s1"))
        h.push(snap("s2"))
        // current live state is "s3"; undo should walk s3<-s2<-s1
        assertEquals(snap("s2"), h.undo(snap("s3")))
        assertEquals(snap("s1"), h.undo(snap("s2")))
        assertFalse(h.canUndo)
        // redo should walk back forward to s2 then s3
        assertEquals(snap("s2"), h.redo(snap("s1")))
        assertEquals(snap("s3"), h.redo(snap("s2")))
        assertFalse(h.canRedo)
    }

    @Test
    fun cap_dropsOldestUndoEntry() {
        val h = EditHistory(cap = 2)
        h.push(snap("s1"))
        h.push(snap("s2"))
        h.push(snap("s3"))                   // s1 evicted; undo stack == [s2, s3]
        assertEquals(snap("s3"), h.undo(snap("live")))
        assertEquals(snap("s2"), h.undo(snap("s3")))
        assertFalse("only `cap` entries are retained", h.canUndo)
        assertNull(h.undo(snap("s2")))
    }

    @Test
    fun clear_emptiesBothStacks() {
        val h = EditHistory()
        h.push(snap("a"))
        h.undo(snap("live"))                 // populate redo too
        h.push(snap("b"))
        h.clear()
        assertFalse(h.canUndo)
        assertFalse(h.canRedo)
        assertNull(h.undo(snap("cur")))
        assertNull(h.redo(snap("cur")))
    }

    @Test
    fun snapshotAndRestore_preserveBothBranchesAndCursorOrder() {
        val original = EditHistory()
        original.push(snap("s1"))
        original.push(snap("s2"))
        assertEquals(snap("s2"), original.undo(snap("s3")))

        val restored = EditHistory()
        restored.restoreState(original.snapshotState())

        assertEquals(snap("s1"), restored.undo(snap("s2")))
        assertEquals(snap("s2"), restored.redo(snap("s1")))
        assertEquals(snap("s3"), restored.redo(snap("s2")))
    }

    @Test
    fun rejectedRestore_isTransactionalAndLeavesPriorHistoryUntouched() {
        val history = EditHistory(cap = 2, byteCap = 64)
        history.push(snap("kept"))

        org.junit.Assert.assertThrows(IllegalArgumentException::class.java) {
            history.restoreState(
                EditHistoryState(
                    undo = listOf(snap("a"), snap("b"), snap("c")),
                    redo = emptyList(),
                ),
            )
        }

        assertEquals(snap("kept"), history.undo(snap("live")))
    }

    @Test
    fun byteCapDropsOldestSnapshotInsteadOfGrowingWithoutBound() {
        val history = EditHistory(cap = 50, byteCap = 14)
        history.push(snap("123456")) // 6 UTF-8 bytes + 4 rotation bytes
        history.push(snap("abcdef"))

        assertEquals(snap("abcdef"), history.undo(snap("live")))
        assertFalse("the oldest entry was evicted by the byte cap", history.canUndo)
    }

    @Test
    fun snapshot_carriesRotation() {
        val h = EditHistory()
        h.push(snap("a", rot = 90))
        val restored = h.undo(snap("live", rot = 270))
        assertEquals(90, restored?.rotationDegrees)
    }

    // --- settleDecision: the editor's capture/settle logic (extracted for testing) ---

    @Test
    fun settle_pureRestore_recordsNothing() {
        // undo/redo restore with no follow-up edit: don't push, adopt the restored snapshot.
        val restored = snap("restored")
        val a = settleDecision(restoring = true, committed = restored, now = restored)
        assertNull(a.push)
        assertEquals(restored, a.committed)
    }

    @Test
    fun settle_editWithinRestoreWindow_pushesBaseline() {
        // The regression: a real edit lands within the restore settle window (restoring still
        // true, `now` already differs). The restored baseline MUST be pushed so the edit is undoable.
        val restored = snap("restored")
        val edited = snap("edited")
        val a = settleDecision(restoring = true, committed = restored, now = edited)
        assertEquals("the restored baseline is pushed so the edit stays undoable", restored, a.push)
        assertEquals(edited, a.committed)
    }

    @Test
    fun settle_firstBaseline_adoptsWithoutPush() {
        val a = settleDecision(restoring = false, committed = null, now = snap("base"))
        assertNull(a.push)
        assertEquals(snap("base"), a.committed)
    }

    @Test
    fun settle_normalEdit_pushesPreviousCommitted() {
        val prev = snap("prev")
        val now = snap("now")
        val a = settleDecision(restoring = false, committed = prev, now = now)
        assertEquals(prev, a.push)
        assertEquals(now, a.committed)
    }

    @Test
    fun settle_noChange_recordsNothing() {
        val s = snap("s")
        val a = settleDecision(restoring = false, committed = s, now = s)
        assertNull(a.push)
        assertEquals(s, a.committed)
    }
}
