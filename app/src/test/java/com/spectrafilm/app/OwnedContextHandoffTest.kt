/*
 * Spektrafilm for Android — cancellation-safe coroutine resource handoff regressions. GPLv3.
 */
package com.spectrafilm.app

import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlin.coroutines.CoroutineContext
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Test

class OwnedContextHandoffTest {
    private class Resource(private val closes: AtomicInteger) : AutoCloseable {
        override fun close() {
            closes.incrementAndGet()
        }
    }

    private class QueuedDispatcher : CoroutineDispatcher() {
        private val tasks = LinkedBlockingQueue<Runnable>()

        override fun dispatch(context: CoroutineContext, block: Runnable) {
            tasks.add(block)
        }

        fun awaitTask(): Runnable = requireNotNull(tasks.poll(10, TimeUnit.SECONDS)) {
            "timed out waiting for dispatched continuation"
        }
    }

    @Test
    fun promptCancellationAtDispatcherReturnDisposesUnclaimedResourceExactlyOnce() {
        val caller = QueuedDispatcher()
        val scopeJob = Job()
        val scope = CoroutineScope(scopeJob + caller)
        val parked = CountDownLatch(1)
        val produced = CountDownLatch(1)
        val closes = AtomicInteger()
        val received = AtomicReference<Resource?>()
        val job = scope.launch {
            received.set(
                withOwnedContext(Dispatchers.Default, Resource::close) {
                    // Hold the producer until the caller has actually parked at the dispatcher
                    // switch. `withContext` only routes its result back through a dispatch when
                    // it decided to suspend; that decision is a CAS the producing thread can win
                    // by finishing first, in which case the value is returned inline and NO
                    // continuation is ever enqueued on `caller`. The scenario under test — a
                    // resource unclaimed in the dispatcher-return window — does not exist on that
                    // path, so without this gate the test does not merely go slow, it waits for a
                    // task that will never arrive and fails the awaitTask() budget below.
                    check(parked.await(10, TimeUnit.SECONDS)) { "caller never reached the switch" }
                    Resource(closes).also { produced.countDown() }
                },
            )
        }

        // run() returning while the producer is still gated proves the caller suspended: the
        // producer cannot have completed, so the decision CAS can only have gone to SUSPENDED.
        caller.awaitTask().run()
        parked.countDown()
        check(produced.await(10, TimeUnit.SECONDS)) { "resource producer did not finish" }
        val returnContinuation = caller.awaitTask()

        job.cancel()
        returnContinuation.run()
        while (!job.isCompleted) caller.awaitTask().run()

        assertNull(received.get())
        assertEquals(1, closes.get())
        scope.cancel()
    }

    @Test
    fun successfulHandoffTransfersCleanupToCaller() = runBlocking {
        val closes = AtomicInteger()

        val resource = withOwnedContext(Dispatchers.Default, Resource::close) {
            Resource(closes)
        }

        assertEquals(0, closes.get())
        resource.close()
        assertEquals(1, closes.get())
    }

    @Test
    fun constructionFailureDisposesAlreadyCreatedOwnedValue() {
        val closes = AtomicInteger()
        val resource = Resource(closes)
        val oom = OutOfMemoryError("post-create scratch allocation")

        val observed = runCatching {
            disposeOnFailure(resource, Resource::close) { throw oom }
        }.exceptionOrNull()

        assertSame(oom, observed)
        assertEquals(1, closes.get())
    }
}
