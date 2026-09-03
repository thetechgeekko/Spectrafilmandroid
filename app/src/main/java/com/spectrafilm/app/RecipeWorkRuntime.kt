/*
 * Spektrafilm for Android — process-owned FIFO recipe persistence. GPLv3.
 */
package com.spectrafilm.app

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicLong

internal class RecipeEditEpoch {
    private val value = AtomicLong(0L)

    fun snapshot(): Long = value.get()
    fun invalidate(): Long = value.incrementAndGet()
    fun isCurrent(snapshot: Long): Boolean = snapshot == value.get()
}

internal class RecipeOperationRuntime internal constructor(
    workScope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.IO),
) {
    private val queue = Channel<() -> Unit>(Channel.UNLIMITED)

    init {
        workScope.launch {
            for (operation in queue) operation()
        }
    }

    /** Submission is synchronous, execution is FIFO and independent of any Activity waiter. */
    fun <T> submit(operation: () -> T): Deferred<T> {
        val result = CompletableDeferred<T>()
        val sent = queue.trySend {
            try {
                result.complete(operation())
            } catch (failure: Throwable) {
                result.completeExceptionally(failure)
            }
        }
        if (sent.isFailure) {
            result.completeExceptionally(
                IllegalStateException("recipe operation queue is unavailable", sent.exceptionOrNull()),
            )
        }
        return result
    }
}

internal object RecipeWorkRuntime {
    private val runtime = RecipeOperationRuntime()

    fun <T> submit(operation: () -> T): Deferred<T> = runtime.submit(operation)
}
