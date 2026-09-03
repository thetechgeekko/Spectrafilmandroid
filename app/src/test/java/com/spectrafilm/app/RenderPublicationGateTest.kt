/*
 * Spektrafilm for Android — last-valid-render publication regressions. GPLv3.
 */
package com.spectrafilm.app

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RenderPublicationGateTest {
    @Test
    fun newerRevisionAndHigherQualityWinAndClaimExactlyOnce() {
        val gate = RenderPublicationGate()

        val editRevision = gate.nextRevision()
        val draft = gate.begin(editRevision, RenderPublicationPriority.DRAFT)
        val settle = gate.begin(editRevision, RenderPublicationPriority.SETTLE)

        assertFalse(gate.tryClaim(draft))
        assertTrue(gate.tryClaim(settle))
        assertFalse(gate.tryClaim(settle))

        val export = gate.begin(gate.nextRevision(), RenderPublicationPriority.EXPORT)
        assertFalse(gate.tryClaim(settle))
        assertTrue(gate.tryClaim(export))

        val nextEdit = gate.begin(gate.nextRevision(), RenderPublicationPriority.DRAFT)
        assertFalse(gate.tryClaim(export))
        assertTrue(gate.tryClaim(nextEdit))
    }

    @Test
    fun sourceInvalidationRejectsEveryOutstandingTicket() {
        val gate = RenderPublicationGate()
        val pending = gate.begin(gate.nextRevision(), RenderPublicationPriority.SETTLE)

        gate.invalidate()

        assertFalse(gate.tryClaim(pending))
        val replacement = gate.begin(gate.nextRevision(), RenderPublicationPriority.DRAFT)
        assertTrue(gate.tryClaim(replacement))
    }

    @Test
    fun delayedBeginAfterSourceInvalidationCannotReauthorizeOldRevision() {
        val gate = RenderPublicationGate()
        val staleRevision = gate.nextRevision()

        gate.invalidate()
        val delayedOldSource = gate.begin(staleRevision, RenderPublicationPriority.SETTLE)

        assertFalse(gate.tryClaim(delayedOldSource))
        val replacement = gate.begin(gate.nextRevision(), RenderPublicationPriority.DRAFT)
        assertTrue(gate.tryClaim(replacement))
    }
}
