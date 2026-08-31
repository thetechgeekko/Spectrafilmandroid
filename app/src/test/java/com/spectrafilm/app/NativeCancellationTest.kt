/*
 * Spektrafilm for Android — coroutine/native cancellation bridge regressions. GPLv3.
 */
package com.spectrafilm.app

import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class NativeCancellationTest {
    @Test
    fun cancellationRequestsNativeStopAndDisposesLateResult() = runBlocking {
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val cancellation = AtomicReference<com.spectrafilm.engine.RenderCancellation>()
        val lateDisposals = AtomicInteger()

        val job = launch(Dispatchers.Default) {
            runCancellableNative(
                onLateResult = { lateDisposals.incrementAndGet() },
            ) { token ->
                cancellation.set(token)
                entered.countDown()
                check(release.await(5, TimeUnit.SECONDS))
                "late-result"
            }
        }

        assertTrue(entered.await(5, TimeUnit.SECONDS))
        job.cancel()
        assertTrue(cancellation.get().isCancellationRequested)
        release.countDown()
        job.cancelAndJoin()

        assertEquals(1, lateDisposals.get())
    }

    @Test
    fun cancellationRequestsTiffWriterStop() = runBlocking {
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val cancellation = AtomicReference<com.spectrafilm.tiffwriter.TiffCancellationToken>()

        val job = launch(Dispatchers.Default) {
            runCancellableTiffWrite { token ->
                cancellation.set(token)
                entered.countDown()
                check(release.await(5, TimeUnit.SECONDS))
                42L
            }
        }

        assertTrue(entered.await(5, TimeUnit.SECONDS))
        job.cancel()
        assertTrue(cancellation.get().isCancelled)
        release.countDown()
        job.cancelAndJoin()
    }
}
