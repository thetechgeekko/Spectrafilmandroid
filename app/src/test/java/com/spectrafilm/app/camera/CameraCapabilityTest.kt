/*
 * Spektrafilm for Android — camera capability routing regressions (#196). GPLv3.
 */
package com.spectrafilm.app.camera

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The capture route is decided by RUNTIME capability evidence, never model/OS guesses
 * (#196). These pin the pure decision against every capability shape the probe can see.
 */
class CameraCapabilityTest {

    private val rawCapable = intArrayOf(
        CameraRouteDecision.CAPABILITY_BACKWARD_COMPATIBLE,
        CameraRouteDecision.CAPABILITY_RAW,
        CameraRouteDecision.CAPABILITY_MANUAL_SENSOR,
    )

    @Test
    fun rawRouteRequiresBothTheCapabilityAndAConcreteRawSize() {
        val decision = CameraRouteDecision.decide(
            capabilities = rawCapable,
            rawSizes = listOf(4080 to 3060, 2040 to 1530),
        )
        assertTrue(decision.rawSupported)
        assertEquals(4080, decision.maxRawWidth)
        assertEquals(3060, decision.maxRawHeight)
        assertTrue(decision.manualSensor)
    }

    @Test
    fun rawCapabilityWithoutSizesFallsBackHonestly() {
        val decision = CameraRouteDecision.decide(
            capabilities = rawCapable,
            rawSizes = emptyList(),
        )
        assertFalse(decision.rawSupported)
        assertEquals(0, decision.maxRawWidth)
    }

    @Test
    fun sizesWithoutTheAdvertisedCapabilityFallBackHonestly() {
        val decision = CameraRouteDecision.decide(
            capabilities = intArrayOf(CameraRouteDecision.CAPABILITY_BACKWARD_COMPATIBLE),
            rawSizes = listOf(4000 to 3000),
        )
        assertFalse(decision.rawSupported)
    }

    @Test
    fun missingCapabilityListFailsClosed() {
        val decision = CameraRouteDecision.decide(capabilities = null, rawSizes = listOf(1 to 1))
        assertFalse(decision.rawSupported)
        assertFalse(decision.manualSensor)
    }

    @Test
    fun largestRawSizeWinsByArea() {
        val decision = CameraRouteDecision.decide(
            capabilities = rawCapable,
            // Wider-but-smaller-area first: max is by area, not by first or by width.
            rawSizes = listOf(8000 to 2, 4000 to 3000),
        )
        assertEquals(4000, decision.maxRawWidth)
        assertEquals(3000, decision.maxRawHeight)
    }

    @Test
    fun captureRouteLabelIsHonest() {
        assertEquals(
            "RAW (DNG)",
            CameraRouteDecision.decide(rawCapable, listOf(4 to 3)).routeLabel,
        )
        assertEquals(
            "JPEG (this camera does not provide RAW)",
            CameraRouteDecision.decide(intArrayOf(), emptyList()).routeLabel,
        )
    }

    @Test
    fun probeFailureYieldsNoDecisionRatherThanAGuess() {
        assertNull(CameraRouteDecision.decideOrNull { error("camera service died") })
    }
}
