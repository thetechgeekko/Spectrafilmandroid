/*
 * Spektrafilm for Android — unit tests for the histogram shadow/highlight clip indicator. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Guards the pure channel-clip decision that lights the Lightroom-style corner triangles on the
 * preview histogram. The drawing itself is Compose; this is the testable core.
 */
package com.spectrafilm.app

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class HistogramClipTest {

    // threshold 0.001 of 100k sampled -> a channel must exceed 100 railed pixels to clip.
    private val thr = 0.001f
    private val sampled = 100_000

    @Test
    fun belowThreshold_noClip() {
        assertNull(clippedChannels(100, 0, 50, sampled, thr)) // 100 is not > 100
    }

    @Test
    fun singleChannel_clips() {
        assertEquals(Triple(true, false, false), clippedChannels(200, 5, 5, sampled, thr))
        assertEquals(Triple(false, false, true), clippedChannels(0, 0, 300, sampled, thr))
    }

    @Test
    fun allChannels_clip() {
        assertEquals(Triple(true, true, true), clippedChannels(500, 500, 500, sampled, thr))
    }

    @Test
    fun zeroSampled_isNull() {
        assertNull(clippedChannels(10, 10, 10, 0, thr))
    }
}
