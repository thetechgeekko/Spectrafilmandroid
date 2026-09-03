/*
 * Spektrafilm for Android — export publication linearization regressions. GPLv3.
 */
package com.spectrafilm.app

import java.util.concurrent.CancellationException
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.cancel
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.joinAll
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import org.junit.Assert.assertTrue
import org.junit.Test

class ExportCommitLinearizationTest {
    @Test
    fun publishedInsideWorkerRemainsVisibleWhenWithContextPromptlyCancelsReturn() = runBlocking {
        val linearization = ExportCommitLinearization()
        val cancellationSurfaced = AtomicBoolean(false)

        val observer = launch {
            try {
                withContext(Dispatchers.Default) {
                    linearization.markPublished()
                    currentCoroutineContext().cancel(
                        CancellationException("cancelled after provider publication"),
                    )
                }
            } catch (_: CancellationException) {
                cancellationSurfaced.set(true)
            }
        }
        joinAll(observer)

        assertTrue("test did not exercise prompt cancellation", cancellationSurfaced.get())
        assertTrue(
            "provider publication must synchronously win over cancellation at dispatcher return",
            linearization.isPublished,
        )
    }
}
