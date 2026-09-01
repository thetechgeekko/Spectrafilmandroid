/*
 * Spektrafilm for Android — decoded-source cache lease regressions. GPLv3.
 */
package com.spectrafilm.app

import com.spectrafilm.engine.LinearImage
import com.spectrafilm.libraw.WhiteBalance
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicInteger
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class DecodedSourceCacheTest {
    private class ThrowingConstructionHooks : DecodedSourceCacheConstructionHooks {
        var failTicket = false
        var failEntry = false
        var failLease = false

        override fun beforeTicketConstruction() {
            if (failTicket) throw OutOfMemoryError("ticket construction")
        }

        override fun beforeEntryConstruction() {
            if (failEntry) throw OutOfMemoryError("entry construction")
        }

        override fun beforeLeaseConstruction() {
            if (failLease) throw OutOfMemoryError("lease construction")
        }
    }

    private fun image(closes: AtomicInteger) = LinearImage.fromDataLease(
        data = ByteBuffer.allocateDirect(3 * 4).order(ByteOrder.nativeOrder()),
        width = 1,
        height = 1,
        lease = AutoCloseable { closes.incrementAndGet() },
    )

    private fun request(uri: String = "content://source/one") = DecodedSourceCache.Request(
        uri = uri,
        kind = "RAW",
        authorizationRequired = false,
        whiteBalance = WhiteBalance.AS_SHOT,
        temperature = 5_500f,
        tint = 0f,
        creativeTemp = 0f,
        creativeTint = 0f,
        filmBalance = "",
        rotationDegrees = 0,
        maxEdge = 640,
    )

    private fun put(cache: DecodedSourceCache, image: LinearImage) =
        cache.publish(cache.beginRequest(request()), image)

    private fun acquire(cache: DecodedSourceCache) =
        cache.acquire(cache.beginRequest(request()))

    @Test
    fun invalidateDefersNativeCloseUntilActiveLeaseEnds() {
        val closes = AtomicInteger()
        val cache = DecodedSourceCache()
        put(cache, image(closes))

        val lease = requireNotNull(acquire(cache))
        cache.invalidate()

        assertEquals(0, closes.get())
        assertNull(acquire(cache))

        lease.close()
        lease.close()
        assertEquals(1, closes.get())
    }

    @Test
    fun staleDecodeCannotEvictNewerRequestAndIsClosedExactlyOnce() {
        val staleCloses = AtomicInteger()
        val currentCloses = AtomicInteger()
        val cache = DecodedSourceCache()
        val staleTicket = cache.beginRequest(request("content://source/old"))
        val currentTicket = cache.beginRequest(request("content://source/new"))
        val currentImage = image(currentCloses)

        assertTrue(cache.publish(currentTicket, currentImage))
        assertFalse(cache.publish(staleTicket, image(staleCloses)))
        assertEquals(1, staleCloses.get())
        assertNull(cache.acquire(staleTicket))
        cache.acquire(currentTicket).use { lease ->
            assertSame(currentImage, requireNotNull(lease).image)
        }

        cache.invalidate()
        assertEquals(1, currentCloses.get())
    }

    @Test
    fun invalidateMakesDetachedDecodeTicketStale() {
        val closes = AtomicInteger()
        val cache = DecodedSourceCache()
        val ticket = cache.beginRequest(request())

        cache.invalidate()

        assertFalse(cache.publish(ticket, image(closes)))
        assertEquals(1, closes.get())
        assertNull(cache.acquire(ticket))
    }

    @Test
    fun requestChangeRetiresOldEntryImmediatelyButDefersCloseForActiveLease() {
        val closes = AtomicInteger()
        val cache = DecodedSourceCache()
        val oldTicket = cache.beginRequest(request("content://source/old"))
        assertTrue(cache.publish(oldTicket, image(closes)))
        val lease = requireNotNull(cache.acquire(oldTicket))

        val newTicket = cache.beginRequest(request("content://source/new"))

        assertEquals(0, closes.get())
        assertNull(cache.acquire(oldTicket))
        assertNull(cache.acquire(newTicket))
        lease.close()
        assertEquals(1, closes.get())
    }

    @Test
    fun requestChangeClosesUnleasedOldEntryEvenWhenReplacementNeverPublishes() {
        val closes = AtomicInteger()
        val cache = DecodedSourceCache()
        val oldTicket = cache.beginRequest(request("content://source/old"))
        assertTrue(cache.publish(oldTicket, image(closes)))

        cache.beginRequest(request("content://source/new"))

        assertEquals(1, closes.get())
        assertNull(cache.acquire(oldTicket))
    }

    @Test
    fun leaseConstructionOomRollsBackPinSoInvalidationCanCloseImage() {
        val hooks = ThrowingConstructionHooks()
        val closes = AtomicInteger()
        val cache = DecodedSourceCache(hooks)
        val ticket = cache.beginRequest(request())
        assertTrue(cache.publish(ticket, image(closes)))
        hooks.failLease = true

        assertTrue(cache.runCatching { acquire(ticket) }.exceptionOrNull() is OutOfMemoryError)
        cache.invalidate()

        assertEquals("phantom lease must not retain the image", 1, closes.get())
    }

    @Test
    fun ticketConstructionOomLeavesPreviousRequestAndEntryIntact() {
        val hooks = ThrowingConstructionHooks()
        val closes = AtomicInteger()
        val cache = DecodedSourceCache(hooks)
        val original = cache.beginRequest(request("content://source/old"))
        assertTrue(cache.publish(original, image(closes)))
        hooks.failTicket = true

        assertTrue(
            cache.runCatching { beginRequest(request("content://source/new")) }
                .exceptionOrNull() is OutOfMemoryError,
        )

        hooks.failTicket = false
        cache.acquire(original).use { assertTrue(it != null) }
        assertEquals(0, closes.get())
        cache.invalidate()
        assertEquals(1, closes.get())
    }

    @Test
    fun entryConstructionOomClosesIncomingImageAndPublishesNothing() {
        val hooks = ThrowingConstructionHooks()
        val closes = AtomicInteger()
        val cache = DecodedSourceCache(hooks)
        val ticket = cache.beginRequest(request())
        val incoming = image(closes)
        hooks.failEntry = true

        assertTrue(cache.runCatching { publish(ticket, incoming) }.exceptionOrNull() is OutOfMemoryError)

        assertEquals(1, closes.get())
        assertNull(cache.acquire(ticket))
    }
}
