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
}
