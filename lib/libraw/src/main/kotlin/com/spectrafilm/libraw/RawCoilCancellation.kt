/*
 * Spektrafilm for Android -- cancellation boundary for the Coil RAW adapter.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
package com.spectrafilm.libraw

import java.util.concurrent.CancellationException
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.suspendCancellableCoroutine

/**
 * Runs synchronous Coil RAW work while forwarding coroutine cancellation to the
 * native decode generation. Ordinary decoder failures remain a Coil cache miss
 * (`null`); cancellation is never reclassified as a cache miss.
 */
@OptIn(ExperimentalCoroutinesApi::class)
internal suspend fun <T> runRawCoilDecode(
    cancellationFactory: () -> RawDecodeCancellation = RawDecoder::newCancellation,
    onLateResult: (T) -> Unit = {},
    block: (RawDecodeCancellation) -> T,
): T? = suspendCancellableCoroutine { continuation ->
    val cancellation = try {
        cancellationFactory()
    } catch (failure: CancellationException) {
        continuation.resumeWith(Result.failure(failure))
        return@suspendCancellableCoroutine
    } catch (failure: Exception) {
        continuation.resumeWith(Result.success(null))
        return@suspendCancellableCoroutine
    }

    continuation.invokeOnCancellation { cancellation.cancel() }
    if (!continuation.isActive) {
        cancellation.close()
        return@suspendCancellableCoroutine
    }

    try {
        val result = block(cancellation)
        cancellation.close()
        if (continuation.isActive) {
            continuation.resume(result) { _, lateResult, _ ->
                onLateResult(lateResult)
            }
        } else {
            onLateResult(result)
        }
    } catch (failure: Throwable) {
        cancellation.close()
        if (!continuation.isActive) return@suspendCancellableCoroutine
        when {
            failure is CancellationException ->
                continuation.resumeWith(Result.failure(failure))
            failure is RawDecodeException && failure.status == DecodeStatus.CANCELLED ->
                continuation.resumeWith(Result.failure(failure.asCancellationException()))
            failure is Exception ->
                continuation.resumeWith(Result.success(null))
            else ->
                continuation.resumeWith(Result.failure(failure))
        }
    }
}

private fun RawDecodeException.asCancellationException(): CancellationException =
    CancellationException(message ?: "RAW decode cancelled").also { it.initCause(this) }
