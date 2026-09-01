/*
 * Spektrafilm for Android — bitmap-independent histogram regressions. GPLv3.
 */
package com.spectrafilm.app

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class HistogramTest {
    @Test
    fun sampledPixelsAreBinnedWithoutRetainingABitmap() {
        val samples = HistogramSamples(
            intArrayOf(
                0x00FF0000,
                0x0000FF00,
                0x000000FF,
                0x00FFFFFF,
            ),
        )

        val histogram = computeHistogram(samples)

        assertEquals(2, histogram.r[255])
        assertEquals(2, histogram.g[255])
        assertEquals(2, histogram.b[255])
        assertEquals(4, histogram.luma.sum())
        assertEquals(2, histogram.peak)
    }

    @Test
    fun samplesDefensivelyCopyPixelsBeforeBackgroundBinning() {
        val pixels = intArrayOf(0x00FF0000)
        val samples = HistogramSamples(pixels)
        pixels[0] = 0x000000FF

        val histogram = computeHistogram(samples)

        assertEquals(1, histogram.r[255])
        assertEquals(0, histogram.b[255])
    }

    @Test
    fun retirementRejectsNewReadersAndWaitsForExistingLease() {
        data class Resource(var physicallyRetired: Boolean = false)
        val resource = Resource()
        var retireCalls = 0
        val registry = RetirableReadLeaseRegistry<Resource>(
            isPhysicallyRetired = { it.physicallyRetired },
            physicallyRetire = {
                it.physicallyRetired = true
                retireCalls++
            },
        )
        val lease = registry.acquire(resource)!!

        registry.retire(resource)

        assertFalse("active reader must keep storage alive", resource.physicallyRetired)
        assertNull("retired frame must reject new readers", registry.acquire(resource))
        lease.close()
        assertTrue(resource.physicallyRetired)
        assertEquals(1, retireCalls)

        lease.close()
        registry.retire(resource)
        assertEquals("close/retire must be idempotent", 1, retireCalls)
    }

    @Test
    fun leaseConstructionOomRollsBackReaderBeforeRetirement() {
        data class Resource(var physicallyRetired: Boolean = false)
        val resource = Resource()
        var failLease = true
        var retireCalls = 0
        val registry = RetirableReadLeaseRegistry<Resource>(
            isPhysicallyRetired = { it.physicallyRetired },
            physicallyRetire = {
                it.physicallyRetired = true
                retireCalls++
            },
            beforeLeaseConstruction = {
                if (failLease) throw OutOfMemoryError("lease construction")
            },
        )

        assertTrue(registry.runCatching { acquire(resource) }.exceptionOrNull() is OutOfMemoryError)
        failLease = false
        registry.retire(resource)

        assertTrue(resource.physicallyRetired)
        assertEquals("phantom reader must not retain the frame", 1, retireCalls)
    }
}
