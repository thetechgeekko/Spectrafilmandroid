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
        val produced = CountDownLatch(1)
        val closes = AtomicInteger()
        val received = AtomicReference<Resource?>()
        val job = scope.launch {
            received.set(
                withOwnedContext(Dispatchers.Default, Resource::close) {
                    Resource(closes).also { produced.countDown() }
                },
            )
        }

        caller.awaitTask().run()
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
}
