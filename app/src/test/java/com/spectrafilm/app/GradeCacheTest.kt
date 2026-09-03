/*
 * Spektrafilm for Android — unit tests for the retained-result grade cache. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Proves the cache contract: store/lookup round-trip with an EAGER pristine copy
 * (mutating the source buffer after store, or a scratch copy after lookup, never
 * corrupts the retained master), key discrimination on engine params / decode key /
 * edge, single-slot replacement, and clear().
 */
package com.spectrafilm.app

import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.SpektraParams
import com.spectrafilm.libraw.WhiteBalance
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlin.concurrent.thread
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class GradeCacheTest {

    private val W = 4
    private val H = 3

    private class TrackingAllocator : GradeBufferAllocator {
        val releases = AtomicInteger()

        override fun allocate(sizeBytes: Int): GradeBufferOwner = object : GradeBufferOwner {
            private val data = ByteBuffer.allocateDirect(sizeBytes).order(ByteOrder.nativeOrder())
            private val closed = AtomicBoolean(false)
            private var leases = 0

            @Synchronized
            override fun acquire(): GradeBufferLease {
                check(!closed.get()) { "test grade buffer is closed" }
                leases++
                return GradeBufferLease(data.duplicate().order(ByteOrder.nativeOrder()), ::releaseLease)
            }

            @Synchronized
            private fun releaseLease() {
                check(leases > 0) { "test grade lease underflow" }
                leases--
                if (closed.get() && leases == 0) releases.incrementAndGet()
            }

            @Synchronized
            override fun close() {
                if (closed.compareAndSet(false, true) && leases == 0) releases.incrementAndGet()
            }
        }
    }

    private class BlockingAllocator : GradeBufferAllocator {
        private val delegate = TrackingAllocator()
        private val blockNext = AtomicBoolean(true)
        val entered = CountDownLatch(1)
        val proceed = CountDownLatch(1)
        val releases: Int get() = delegate.releases.get()

        override fun allocate(sizeBytes: Int): GradeBufferOwner {
            if (blockNext.compareAndSet(true, false)) {
                entered.countDown()
                check(proceed.await(5, TimeUnit.SECONDS)) { "timed out awaiting allocation release" }
            }
            return delegate.allocate(sizeBytes)
        }
    }

    private fun cache(allocator: TrackingAllocator = TrackingAllocator()) = GradeCache(allocator)

    private fun store(
        cache: GradeCache,
        cacheKey: GradeCache.Key = key(),
        data: ByteBuffer = buf(1f),
    ): Boolean {
        val ticket = cache.beginStore(cacheKey)
        return cache.store(ticket, cacheKey, data, W, H, ColorSpace.SRGB)
    }

    private fun buf(seed: Float): ByteBuffer {
        val b = ByteBuffer.allocateDirect(W * H * 3 * 4).order(ByteOrder.nativeOrder())
        val f = b.asFloatBuffer()
        for (i in 0 until W * H * 3) f.put(i, seed + i * 0.25f)
        return b
    }

    private fun params(film: String = "kodak_portra_400") =
        SpektraParams(filmProfile = film, printProfile = "kodak_portra_endura")

    private fun decodeKey(edge: Int = 640, rotation: Int = 0) = GradeCache.DecodeKey(
        uri = "content://photo/1", kind = "RAW", whiteBalance = WhiteBalance.AS_SHOT,
        temperature = 5500f, tint = 0f, creativeTemp = 0f, creativeTint = 0f,
        filmBalance = "", rotationDegrees = rotation, maxEdge = edge,
    )

    private fun key(film: String = "kodak_portra_400", edge: Int = 640, rotation: Int = 0) =
        GradeCache.Key(engineParams = params(film), decode = decodeKey(edge, rotation))

    @Test
    fun storeLookup_roundTripsDimsAndContent() {
        val cache = cache()
        store(cache)
        val hit = cache.lookup(key())
        assertNotNull(hit)
        hit!!.use { pinned ->
            assertEquals(W, pinned.width)
            assertEquals(H, pinned.height)
            assertEquals(ColorSpace.SRGB, pinned.colorSpace)
            pinned.withScratch { scratch ->
                val f = scratch.asFloatBuffer()
                for (i in 0 until W * H * 3) assertEquals(1f + i * 0.25f, f.get(i), 0f)
            }
        }
    }

    @Test
    fun store_copiesEagerly_sourceMutationAfterStoreIsInvisible() {
        val cache = cache()
        val src = buf(1f)
        store(cache, data = src)
        src.asFloatBuffer().put(0, 999f)  // simulate the in-place grade mutating res.data
        cache.lookup(key())!!.use { hit ->
            hit.withScratch { scratch ->
                assertEquals(1f, scratch.asFloatBuffer().get(0), 0f)
            }
        }
    }

    @Test
    fun scratchCopy_isIndependent_masterStaysPristine() {
        val cache = cache()
        store(cache)
        cache.lookup(key())!!.use { pristine ->
            pristine.withScratch { it.asFloatBuffer().put(0, 999f) }
            pristine.withScratch { assertEquals(1f, it.asFloatBuffer().get(0), 0f) }
        }
    }

    @Test
    fun lookup_missesOnAnyEngineOrDecodeChange() {
        val cache = cache()
        store(cache)
        assertNull("engine param change must miss", cache.lookup(key(film = "kodak_ektar_100")))
        assertNull("edge change must miss", cache.lookup(key(edge = 1024)))
        assertNull("rotation change must miss", cache.lookup(key(rotation = 90)))
        val hit = cache.lookup(key())
        assertNotNull("identical key must still hit", hit)
        hit!!.close()
    }

    @Test
    fun store_replacesTheSingleSlot() {
        val allocator = TrackingAllocator()
        val cache = cache(allocator)
        store(cache)
        store(cache, key(film = "kodak_ektar_100"), buf(2f))
        assertNull(cache.lookup(key()))
        val hit = cache.lookup(key(film = "kodak_ektar_100"))
        assertNotNull(hit)
        hit!!.close()
        assertEquals("replaced master must close promptly", 1, allocator.releases.get())
        cache.clear()
        assertEquals(2, allocator.releases.get())
    }

    @Test
    fun clear_dropsTheEntry() {
        val allocator = TrackingAllocator()
        val cache = cache(allocator)
        store(cache)
        cache.clear()
        assertNull(cache.lookup(key()))
        assertEquals(1, allocator.releases.get())
    }

    @Test
    fun scratchOwnerClosesWhenGradeThrows() {
        val allocator = TrackingAllocator()
        val cache = cache(allocator)
        store(cache)

        runCatching {
            cache.lookup(key())!!.use { hit ->
                hit.withScratch<Unit> { error("grade failed") }
            }
        }

        assertEquals("scratch allocation must release", 1, allocator.releases.get())
        cache.clear()
        assertEquals("master must still release", 2, allocator.releases.get())
    }

    @Test
    fun clearAfterLookup_defersMasterFreeUntilPinnedHitCloses() {
        val allocator = TrackingAllocator()
        val cache = cache(allocator)
        store(cache)
        val hit = cache.lookup(key())!!

        cache.clear()

        assertNull(cache.lookup(key()))
        assertEquals("clear must not free a pinned master", 0, allocator.releases.get())
        hit.withScratch { scratch ->
            assertEquals(1f, scratch.asFloatBuffer().get(0), 0f)
        }
        assertEquals("only the temporary scratch is free", 1, allocator.releases.get())
        hit.close()
        assertEquals("last pin deterministically frees the retired master", 2, allocator.releases.get())
    }

    @Test
    fun replacementAfterLookup_defersOldMasterFreeUntilPinnedHitCloses() {
        val allocator = TrackingAllocator()
        val cache = cache(allocator)
        store(cache)
        val oldHit = cache.lookup(key())!!

        store(cache, key(film = "kodak_ektar_100"), buf(2f))

        assertEquals("replacement must not free the pinned old master", 0, allocator.releases.get())
        oldHit.close()
        assertEquals(1, allocator.releases.get())
        cache.clear()
        assertEquals(2, allocator.releases.get())
    }

    @Test
    fun clearAndNewerPublication_rejectOldWorkerThatCallsStoreLate() {
        val cache = cache()
        val oldKey = key()
        val oldTicket = cache.beginStore(oldKey)

        cache.clear()
        val newKey = key(film = "kodak_ektar_100")
        val newTicket = cache.beginStore(newKey)
        assertTrue(cache.store(newTicket, newKey, buf(2f), W, H, ColorSpace.SRGB))

        assertFalse(cache.store(oldTicket, oldKey, buf(1f), W, H, ColorSpace.SRGB))
        assertNull(cache.lookup(oldKey))
        cache.lookup(newKey)!!.use { hit ->
            hit.withScratch { scratch ->
                assertEquals(2f, scratch.asFloatBuffer().get(0), 0f)
            }
        }
    }

    @Test
    fun clearDuringAllocation_rejectsInFlightPublicationAndClosesCandidate() {
        val allocator = BlockingAllocator()
        val cache = GradeCache(allocator)
        val cacheKey = key()
        val ticket = cache.beginStore(cacheKey)
        val published = AtomicReference<Boolean?>()
        val failure = AtomicReference<Throwable?>()
        val worker = thread(name = "grade-cache-store") {
            try {
                published.set(cache.store(ticket, cacheKey, buf(1f), W, H, ColorSpace.SRGB))
            } catch (thrown: Throwable) {
                failure.set(thrown)
            }
        }
        assertTrue("store did not reach allocation", allocator.entered.await(5, TimeUnit.SECONDS))

        cache.clear()
        allocator.proceed.countDown()
        worker.join(5_000)

        assertFalse("store worker did not finish", worker.isAlive)
        failure.get()?.let { throw AssertionError("store worker failed", it) }
        assertEquals(false, published.get())
        assertNull(cache.lookup(cacheKey))
        assertEquals("stale candidate owner must close", 1, allocator.releases)
    }

    @Test
    fun closePermanentlyRejectsLateTicketAndNewAuthority() {
        val cache = cache()
        val cacheKey = key()
        val ticket = cache.beginStore(cacheKey)

        cache.close()

        assertFalse(cache.store(ticket, cacheKey, buf(1f), W, H, ColorSpace.SRGB))
        assertTrue(cache.lookup(cacheKey) == null)
        assertTrue(cache.runCatching { beginStore(cacheKey) }.exceptionOrNull() is IllegalStateException)
    }

    @Test
    fun wrapperConstructionOomClosesAcquiredOwner() {
        val closes = AtomicInteger()
        val owner = AutoCloseable { closes.incrementAndGet() }
        val oom = OutOfMemoryError("wrapper construction")

        val observed = runCatching {
            handoffOrClose(owner) { throw oom }
        }.exceptionOrNull()

        assertTrue(observed === oom)
        assertEquals(1, closes.get())
    }

    @Test
    fun cacheAdmissionOomIsBestEffortButInvariantFailureRemainsLoud() {
        assertEquals(
            GradeCacheStoreOutcome.CAPACITY_DENIED,
            storeGradeCacheBestEffort { throw OutOfMemoryError("budget denied") },
        )
        val invariant = IllegalArgumentException("bad dimensions")
        val observed = runCatching {
            storeGradeCacheBestEffort { throw invariant }
        }.exceptionOrNull()
        assertTrue(observed === invariant)
    }
}
