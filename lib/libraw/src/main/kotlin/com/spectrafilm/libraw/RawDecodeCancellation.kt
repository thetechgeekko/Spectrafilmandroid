/*
 * Spektrafilm for Android -- cooperative RAW decode cancellation owner.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
package com.spectrafilm.libraw

import java.util.concurrent.atomic.AtomicInteger

/**
 * Reusable cancellation generation for synchronous RAW decode calls.
 *
 * [cancel] is idempotent. [close] cancels any in-flight native lease, removes
 * the generation from the registry, and is safe under repeated/concurrent calls.
 */
class RawDecodeCancellation private constructor(
    private val token: Long,
    private val cancelNative: () -> Unit,
    private val releaseNative: () -> Unit,
) : AutoCloseable {

    private val state = AtomicInteger(OPEN)

    /** Requests cancellation, returning false only after this owner is closed. */
    fun cancel(): Boolean {
        while (true) {
            when (state.get()) {
                CANCELLED -> return true
                CLOSED -> return false
                OPEN -> if (state.compareAndSet(OPEN, CANCELLED)) {
                    cancelNative()
                    return true
                }
            }
        }
    }

    val isCancellationRequested: Boolean get() = state.get() != OPEN

    /** Fail the current managed stage with the same typed status as native cancellation. */
    internal fun throwIfCancellationRequested() {
        if (isCancellationRequested) {
            throw RawDecodeException(
                "RAW decode cancelled",
                DecodeStatus.CANCELLED.code,
                0,
            )
        }
    }

    override fun close() {
        if (state.getAndSet(CLOSED) != CLOSED) releaseNative()
    }

    internal fun tokenForDecode(): Long {
        if (state.get() == CLOSED) {
            throw RawDecodeException(
                "RAW decode cancellation token is closed",
                DecodeStatus.CANCELLED.code,
                0,
            )
        }
        return token
    }

    internal companion object {
        private const val OPEN = 0
        private const val CANCELLED = 1
        private const val CLOSED = 2

        fun fromNative(
            token: Long,
            cancelNative: () -> Unit,
            releaseNative: () -> Unit,
        ): RawDecodeCancellation = RawDecodeCancellation(
            token,
            cancelNative,
            releaseNative,
        )

        fun forTest(
            token: Long,
            cancelNative: () -> Unit,
            releaseNative: () -> Unit,
        ): RawDecodeCancellation = fromNative(token, cancelNative, releaseNative)
    }
}
