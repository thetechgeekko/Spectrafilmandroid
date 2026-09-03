/*
 * Spektrafilm for Android — single-flight lifecycle regressions. GPLv3.
 */
package com.spectrafilm.app

import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.yield
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class SingleFlightTest {
    @Test
    fun sameKeyWaitersShareOneActiveBlock() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        try {
            val gate = CompletableDeferred<Unit>()
            val calls = AtomicInteger()
            val flight = SingleFlight<Int>()
            val first = async(start = CoroutineStart.UNDISPATCHED) {
                flight.run("same", scope) { calls.incrementAndGet(); gate.await(); 7 }
            }
            while (calls.get() == 0) yield()
            val second = async(start = CoroutineStart.UNDISPATCHED) {
                flight.run("same", scope) { calls.incrementAndGet(); 9 }
            }
            gate.complete(Unit)

            assertEquals(7, first.await())
            assertEquals(7, second.await())
            assertEquals(1, calls.get())
        } finally {
            scope.cancel()
        }
    }

    @Test
    fun differentKeyCancelsSupersededFlightWithoutClearingReplacement() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        try {
            val oldStarted = CompletableDeferred<Unit>()
            val oldCancelled = AtomicBoolean(false)
            val newGate = CompletableDeferred<Unit>()
            val flight = SingleFlight<Int>()
            val old = async {
                runCatching {
                    flight.run("old", scope) {
                        oldStarted.complete(Unit)
                        try {
                            CompletableDeferred<Unit>().await()
                            1
                        } finally {
                            oldCancelled.set(true)
                        }
                    }
                }
            }
            oldStarted.await()
            val replacement = async(start = CoroutineStart.UNDISPATCHED) {
                flight.run("new", scope) { newGate.await(); 2 }
            }
            while (!oldCancelled.get()) yield()
            val joined = async(start = CoroutineStart.UNDISPATCHED) {
                flight.run("new", scope) { 3 }
            }
            newGate.complete(Unit)

            assertTrue(old.await().isFailure)
            assertEquals(2, replacement.await())
            assertEquals(2, joined.await())
        } finally {
            scope.cancel()
        }
    }

    @Test
    fun invalidateCancelsAndForgetsCurrentGeneration() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        try {
            val started = CompletableDeferred<Unit>()
            val flight = SingleFlight<Int>()
            val old = async {
                runCatching {
                    flight.run("old", scope) {
                        started.complete(Unit)
                        CompletableDeferred<Unit>().await()
                        1
                    }
                }
            }
            started.await()
            flight.invalidate()
            assertTrue(old.await().isFailure)
            assertEquals(2, flight.run("new", scope) { 2 })
        } finally {
            scope.cancel()
        }
    }

    @Test
    fun cancelledSoleWaiterDoesNotRetainCompletedStableScopeResult() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        try {
            val started = CompletableDeferred<Unit>()
            val finish = CompletableDeferred<Unit>()
            val blockReturned = CompletableDeferred<Unit>()
            val flight = SingleFlight<ByteArray>()
            val waiter = async(start = CoroutineStart.UNDISPATCHED) {
                flight.run("source", scope) {
                    started.complete(Unit)
                    finish.await()
                    ByteArray(1024).also { blockReturned.complete(Unit) }
                }
            }
            started.await()

            waiter.cancel()
            waiter.join()
            finish.complete(Unit)
            blockReturned.await()
            withTimeout(2_000) {
                while (!flight.isIdle()) yield()
            }

            assertTrue(flight.isIdle())
        } finally {
            scope.cancel()
        }
    }
}
