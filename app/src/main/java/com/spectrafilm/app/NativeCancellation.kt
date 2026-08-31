/*
 * Spektrafilm for Android — coroutine bridge for synchronous native work. GPLv3.
 */
package com.spectrafilm.app

import com.spectrafilm.engine.RenderCancellation
import com.spectrafilm.libraw.RawDecodeCancellation
import com.spectrafilm.libraw.RawDecoder
import com.spectrafilm.pngwriter.PngCancellationToken
import com.spectrafilm.tiffwriter.TiffCancellationToken
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.suspendCancellableCoroutine

/** Requests cooperative native stop and disposes a result that loses the cancellation race. */
@OptIn(ExperimentalCoroutinesApi::class)
internal suspend fun <T> runCancellableNative(
    onLateResult: (T) -> Unit = {},
    block: (RenderCancellation) -> T,
): T = suspendCancellableCoroutine { continuation ->
    val cancellation = RenderCancellation()
    continuation.invokeOnCancellation { cancellation.cancel() }
    if (!continuation.isActive) return@suspendCancellableCoroutine
    try {
        val result = block(cancellation)
        if (continuation.isActive) {
            continuation.resume(result) { onLateResult(result) }
        } else {
            onLateResult(result)
        }
    } catch (failure: Throwable) {
        if (continuation.isActive) continuation.resumeWith(Result.failure(failure))
    }
}

/** Raw decode variant that also releases the native cancellation generation exactly once. */
@OptIn(ExperimentalCoroutinesApi::class)
internal suspend fun <T> runCancellableRawDecode(
    onLateResult: (T) -> Unit = {},
    block: (RawDecodeCancellation) -> T,
): T = suspendCancellableCoroutine { continuation ->
    val cancellation = try {
        RawDecoder.newCancellation()
    } catch (failure: Throwable) {
        if (continuation.isActive) continuation.resumeWith(Result.failure(failure))
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
            continuation.resume(result) { onLateResult(result) }
        } else {
            onLateResult(result)
        }
    } catch (failure: Throwable) {
        cancellation.close()
        if (continuation.isActive) continuation.resumeWith(Result.failure(failure))
    }
}

/** Bridges coroutine cancellation into the TIFF writer's native atomic signal. */
@OptIn(ExperimentalCoroutinesApi::class)
internal suspend fun <T> runCancellableTiffWrite(
    block: (TiffCancellationToken) -> T,
): T = suspendCancellableCoroutine { continuation ->
    val cancellation = TiffCancellationToken()
    continuation.invokeOnCancellation { cancellation.cancel() }
    if (!continuation.isActive) return@suspendCancellableCoroutine
    try {
        val result = block(cancellation)
        if (continuation.isActive) continuation.resume(result) { }
    } catch (failure: Throwable) {
        if (continuation.isActive) continuation.resumeWith(Result.failure(failure))
    }
}

/** Bridges coroutine cancellation into the PNG writer's native atomic signal. */
@OptIn(ExperimentalCoroutinesApi::class)
internal suspend fun <T> runCancellablePngWrite(
    block: (PngCancellationToken) -> T,
): T = suspendCancellableCoroutine { continuation ->
    val cancellation = PngCancellationToken()
    continuation.invokeOnCancellation { cancellation.cancel() }
    if (!continuation.isActive) return@suspendCancellableCoroutine
    try {
        val result = block(cancellation)
        if (continuation.isActive) continuation.resume(result) { }
    } catch (failure: Throwable) {
        if (continuation.isActive) continuation.resumeWith(Result.failure(failure))
    }
}
