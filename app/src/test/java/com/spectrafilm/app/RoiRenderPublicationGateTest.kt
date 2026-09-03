/*
 * Spektrafilm for Android — sharp-ROI generation publication regressions. GPLv3.
 */
package com.spectrafilm.app

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RoiRenderPublicationGateTest {
    @Test
    fun editRenderKeyInvalidatesOldGenerationBeforeReplacementRenderStarts() {
        val gate = RoiRenderPublicationGate()
        val oldGeneration = gate.begin(renderKey = 41)

        assertTrue(gate.canPublish(oldGeneration, currentRenderKey = 41))
        assertFalse(gate.canPublish(oldGeneration, currentRenderKey = 42))

        val replacement = gate.begin(renderKey = 42)
        assertTrue(gate.canPublish(replacement, currentRenderKey = 42))
    }

    @Test
    fun newerRoiRequestInvalidatesOlderRequestWithinSameEditGeneration() {
        val gate = RoiRenderPublicationGate()
        val oldViewport = gate.begin(renderKey = 7)
        val latestViewport = gate.begin(renderKey = 7)

        assertFalse(gate.canPublish(oldViewport, currentRenderKey = 7))
        assertTrue(gate.canPublish(latestViewport, currentRenderKey = 7))
    }
}
