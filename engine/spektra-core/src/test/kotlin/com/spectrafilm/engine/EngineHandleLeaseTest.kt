package com.spectrafilm.engine

import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class EngineHandleLeaseTest {
    @Test
    fun initializePublishesHandleExactlyOnce() {
        val state = EngineHandleLease(0L)

        state.initialize(42L)

        assertEquals(42L, state.withLease("test") { it })
        try {
            state.initialize(43L)
            throw AssertionError("second initialization must fail")
        } catch (_: IllegalStateException) {
            // Expected.
        }
        assertEquals(42L, state.withLease("test") { it })
    }

    @Test(expected = IllegalArgumentException::class)
    fun initializeRejectsNullNativeHandle() {
        EngineHandleLease(0L).initialize(0L)
    }

    @Test
    fun closeWaitsForActiveCallBeforeDestroyingHandle() {
        val state = EngineHandleLease(42L)
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val closeFinished = CountDownLatch(1)
        val destroyed = AtomicInteger()

        val call = thread {
            state.withLease("test") { handle ->
                assertEquals(42L, handle)
                entered.countDown()
                assertTrue(release.await(5, TimeUnit.SECONDS))
            }
        }
        assertTrue(entered.await(5, TimeUnit.SECONDS))

        val closer = thread {
            state.close {
                assertEquals(42L, it)
                destroyed.incrementAndGet()
            }
            closeFinished.countDown()
        }
        val queuedDeadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
        while (!state.isQueuedForTest(closer) && System.nanoTime() < queuedDeadline) {
            Thread.yield()
        }
        assertTrue("close never queued behind active call", state.isQueuedForTest(closer))
        assertEquals(1L, closeFinished.count)
        assertEquals(0, destroyed.get())

        release.countDown()
        assertTrue(closeFinished.await(5, TimeUnit.SECONDS))
        call.join(5_000)
        closer.join(5_000)
        assertFalse(call.isAlive)
        assertFalse(closer.isAlive)
        assertEquals(1, destroyed.get())
    }

    @Test
    fun concurrentCloseDestroysHandleExactlyOnce() {
        val state = EngineHandleLease(99L)
        val start = CountDownLatch(1)
        val destroyed = AtomicInteger()
        val workers = List(16) {
            thread {
                start.await()
                repeat(100) { state.close { destroyed.incrementAndGet() } }
            }
        }

        start.countDown()
        workers.forEach { it.join(5_000) }
        assertTrue(workers.none { it.isAlive })
        assertEquals(1, destroyed.get())
    }

    @Test(expected = IllegalStateException::class)
    fun closedStateRejectsNewCalls() {
        val state = EngineHandleLease(7L)
        state.close {}
        state.withLease("test") { error("closed lease must not run") }
    }
}
