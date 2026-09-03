/*
 * Spektrafilm for Android — legal-notice metadata tests. GPLv3.
 */
package com.spectrafilm.app

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class AboutLegalNoticeTest {
    @Test
    fun libRawDisclosureKeepsDistributionRouteUnresolved() {
        assertEquals("UNRESOLVED", LIBRAW_DISTRIBUTION_ROUTE)
        assertEquals(
            "LibRaw Android distribution route: UNRESOLVED.",
            LIBRAW_ROUTE_STATUS,
        )
        assertTrue(LIBRAW_LICENSE_STATUS.contains("offered upstream under a choice"))
        assertTrue(LIBRAW_LICENSE_STATUS.contains("release route has not been selected"))
        assertTrue(LIBRAW_LICENSE_STATUS.contains("does not elect either route"))
        assertFalse(LIBRAW_LICENSE_STATUS.contains("GPLv3-compatible"))
    }

    @Test
    fun projectLegalLinksUseCanonicalRepository() {
        val canonicalRoot = "https://github.com/thetechgeekko/Spektrafilm-android"

        assertTrue(Links.SOURCE.startsWith(canonicalRoot))
        assertTrue(Links.RELEASES.startsWith(canonicalRoot))
        assertTrue(Links.ISSUES.startsWith(canonicalRoot))
        assertTrue(Links.NEW_ISSUE.startsWith(canonicalRoot))
        assertTrue(Links.LIBRAW_SOURCE == "https://github.com/LibRaw/LibRaw")
        assertTrue(
            Links.LIBRAW_0222_ARCHIVE ==
                "https://www.libraw.org/data/LibRaw-0.22.2.tar.gz",
        )
    }

    @Test
    fun projectLegalDocumentsAreBundledForOfflineReview() {
        assertTrue(PROJECT_GPL_ASSET == "legal/spektrafilm/LICENSE.GPL-3.0")
        assertTrue(PROJECT_NOTICE_ASSET == "legal/spektrafilm/NOTICE.md")
    }
}
