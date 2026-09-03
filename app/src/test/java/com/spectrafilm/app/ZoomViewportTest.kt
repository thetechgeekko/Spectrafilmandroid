/*
 * Spektrafilm for Android — unit tests for the Lightroom-zoom viewport math. GPLv3.
 *
 * The ROI overlay only registers against the proxy if the forward (image->view) and inverse
 * (view->image) transforms are exact inverses, and if the visible-region computation matches
 * the graphicsLayer transform. These are pure functions (no Android framework), so they run on
 * the plain JVM under :app:testDebugUnitTest.
 */
package com.spectrafilm.app

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.unit.IntSize
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ZoomViewportTest {
    private val view = IntSize(1000, 1000)
    private val aspect = 1f // square image in square view → no letterbox

    @Test fun notZoomed_returnsNull() {
        assertNull(viewportRoiNormalized(view, 1f, Offset.Zero, aspect))
        // Just past fit but within the dead-zone is still treated as "not zoomed".
        assertNull(viewportRoiNormalized(view, 1.005f, Offset.Zero, aspect))
    }

    @Test fun zeroViewSize_returnsNull() {
        assertNull(viewportRoiNormalized(IntSize.Zero, 4f, Offset.Zero, aspect))
    }

    @Test fun zoom2x_centered_isHalfSizeCentered() {
        val roi = viewportRoiNormalized(view, 2f, Offset.Zero, aspect)
        assertNotNull(roi)
        roi!!
        assertEquals(0.5f, roi.cxN, 1e-4f)
        assertEquals(0.5f, roi.cyN, 1e-4f)
        assertEquals(0.5f, roi.wN, 1e-3f)
        assertEquals(0.5f, roi.hN, 1e-3f)
    }

    @Test fun zoom4x_centered_isQuarterSize() {
        val roi = viewportRoiNormalized(view, 4f, Offset.Zero, aspect)
        assertNotNull(roi)
        assertEquals(0.25f, roi!!.wN, 1e-3f)
        assertEquals(0.25f, roi.hN, 1e-3f)
    }

    @Test fun forwardInverse_roundTrip() {
        val scale = 3f
        val offset = Offset(40f, -25f)
        for (nx in listOf(0.1f, 0.5f, 0.9f)) {
            for (ny in listOf(0.2f, 0.5f, 0.8f)) {
                val v = imageNormToView(nx, ny, view, scale, offset, aspect)
                val back = mapViewToImageNorm(v, view, scale, offset, aspect)
                assertEquals("nx round-trip", nx, back.x, 1e-3f)
                assertEquals("ny round-trip", ny, back.y, 1e-3f)
            }
        }
    }

    @Test fun panRight_revealsLeftOfImage() {
        // graphicsLayer translationX > 0 moves content right → the viewport sees the LEFT part
        // of the image, so the ROI centre moves left of 0.5.
        val roi = viewportRoiNormalized(view, 2f, Offset(200f, 0f), aspect)
        assertNotNull(roi)
        assertTrue("centre should shift left under positive pan", roi!!.cxN < 0.5f)
    }

    @Test fun letterboxedWideImage_roiStaysInBounds() {
        // A 2:1 image in a square view is letterboxed top/bottom; the ROI must still clamp to
        // [0,1] and have positive extent.
        val wide = 2f
        val roi = viewportRoiNormalized(view, 3f, Offset(0f, 0f), wide)
        assertNotNull(roi)
        roi!!
        assertTrue(roi.cxN in 0f..1f && roi.cyN in 0f..1f)
        assertTrue(roi.wN > 0f && roi.wN <= 1f)
        assertTrue(roi.hN > 0f && roi.hN <= 1f)
    }

    // ---- clampPanOffset: the pan bound must come from the fitted CONTENT rect ----
    //
    // Regression tests for a shipped defect: the bound was computed from the
    // VIEWPORT, but the bitmap is drawn ContentScale.Fit and is letterboxed. On the
    // letterboxed axis that over-permits the pan, and the image can be dragged
    // clean out of view — a black viewport with the zoom pill still reading 410%.
    //
    // Note the fixture: every other test in this file uses a square image in a
    // square view, which is exactly the one geometry where this bug is invisible
    // (fitW == view.width and fitH == view.height, so the wrong formula coincides
    // with the right one). That is why it was not caught here.

    /** Real device geometry from the on-device repro: 998x1802 viewport, 4:3 landscape. */
    private val phoneView = IntSize(998, 1802)
    private val landscape43 = 4f / 3f

    @Test fun letterboxedAxis_boundComesFromContentNotViewport() {
        // fitH = 998 / (4/3) = 748.5, so at s=4.1 the true bound is
        // (748.5*4.1 - 1802)/2 = 633.4. The old viewport formula gave
        // (1802*(4.1-1))/2 = 2793.1 — 4.4x too far.
        val far = clampPanOffset(Offset(0f, 5000f), phoneView, 4.1f, landscape43)
        assertEquals(633.4f, far.y, 1f)
        assertTrue("must not permit the old viewport bound", far.y < 1000f)
    }

    @Test fun filledAxis_isUnchanged_theOldFormulaWasRightHereByAccident() {
        // This content fills the width at fit, so fitW == view.width and the old
        // and new formulas agree: (998*4.1 - 998)/2 == (998*(4.1-1))/2 == 1546.9.
        val far = clampPanOffset(Offset(9000f, 0f), phoneView, 4.1f, landscape43)
        assertEquals(1546.9f, far.x, 1f)
    }

    @Test fun atFit_noPanIsAllowedOnEitherAxis() {
        val p = clampPanOffset(Offset(500f, 500f), phoneView, 1f, landscape43)
        assertEquals(0f, p.x, 1e-3f)
        assertEquals(0f, p.y, 1e-3f)
    }

    @Test fun tallContent_lettersboxesTheOtherAxisInstead() {
        // The mirror case the landscape repro could never surface. Note it needs a
        // genuinely TALL image: this viewport's own aspect is 998/1802 = 0.554, so
        // even a 2:3 portrait (0.667) still fills the WIDTH here and letterboxes on
        // Y like the landscape case. Only aspect < 0.554 flips which axis is
        // letterboxed. (I got this wrong first time and the arithmetic caught it.)
        val tall = 0.4f // 2:5
        val p = clampPanOffset(Offset(9000f, 9000f), phoneView, 3f, tall)
        // fitH = 1802 (fills height), fitW = 1802*0.4 = 720.8
        // maxX = (720.8*3 - 998)/2 = 582.2   <- now the letterboxed axis
        assertEquals(582.2f, p.x, 1f)
        // maxY = (1802*3 - 1802)/2 = 1802.0  <- now the filled axis
        assertEquals(1802f, p.y, 1f)
    }

    @Test fun degenerateInputs_clampToZeroRatherThanNaN() {
        assertEquals(Offset.Zero, clampPanOffset(Offset(10f, 10f), IntSize.Zero, 4f, landscape43))
        assertEquals(Offset.Zero, clampPanOffset(Offset(10f, 10f), phoneView, 4f, 0f))
        assertEquals(Offset.Zero, clampPanOffset(Offset(10f, 10f), phoneView, 4f, Float.NaN))
    }
}
