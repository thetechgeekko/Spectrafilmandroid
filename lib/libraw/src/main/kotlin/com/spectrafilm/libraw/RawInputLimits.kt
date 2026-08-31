/*
 * Spektrafilm for Android -- bounded managed-heap RAW input helpers.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
package com.spectrafilm.libraw

import java.io.ByteArrayOutputStream
import java.io.InputStream

/** Keeps every managed input adapter aligned with the native encoded-input ceiling. */
object RawInputLimits {
    const val MAX_ENCODED_BYTES: Int = 64 * 1024 * 1024
    private const val CHUNK_BYTES = 64 * 1024
    private const val MIB = 1024 * 1024

    /** Validate a declared provider size before narrowing it for a direct-buffer allocation. */
    fun checkedCapacity(
        byteCount: Long,
        maxBytes: Int = MAX_ENCODED_BYTES,
    ): Int {
        require(maxBytes >= 0) { "maxBytes must be non-negative" }
        if (byteCount < 0L || byteCount > maxBytes.toLong()) throwInputTooLarge(maxBytes)
        return Math.toIntExact(byteCount)
    }

    internal fun requireWithinLimit(
        byteCount: Int,
        maxBytes: Int = MAX_ENCODED_BYTES,
    ) {
        checkedCapacity(byteCount.toLong(), maxBytes)
    }

    /**
     * Read no more than [maxBytes] plus one observation byte. The extra byte is
     * never retained: it exists only to distinguish an exact-limit stream from
     * an over-limit stream without allowing an unbounded `readBytes()` growth.
     */
    internal fun readBounded(
        stream: InputStream,
        maxBytes: Int = MAX_ENCODED_BYTES,
        isCancelled: () -> Boolean = { false },
    ): ByteArray {
        require(maxBytes >= 0) { "maxBytes must be non-negative" }
        val available = runCatching { stream.available() }.getOrDefault(0)
        // available() is only a hint and may be hostile or simply inaccurate;
        // never let it force a giant eager Java-heap allocation.
        val initialCapacity = minOf(
            maxBytes,
            maxOf(8 * 1024, minOf(available.coerceAtLeast(0), CHUNK_BYTES)),
        )
        val output = ByteArrayOutputStream(initialCapacity)
        val chunk = ByteArray(CHUNK_BYTES)
        var total = 0

        while (true) {
            if (isCancelled()) throwCancelled()
            val remaining = maxBytes - total
            val requested = minOf(chunk.size, remaining + 1)
            val read = stream.read(chunk, 0, requested)
            if (isCancelled()) throwCancelled()
            if (read < 0) return output.toByteArray()
            if (read == 0) {
                val one = stream.read()
                if (one < 0) return output.toByteArray()
                if (total == maxBytes) throwInputTooLarge(maxBytes)
                output.write(one)
                ++total
                continue
            }
            if (read > remaining) throwInputTooLarge(maxBytes)
            output.write(chunk, 0, read)
            total += read
        }
    }

    private fun throwInputTooLarge(maxBytes: Int): Nothing {
        val limit = if (maxBytes % MIB == 0) {
            "${maxBytes / MIB} MiB"
        } else {
            "$maxBytes bytes"
        }
        throw RawDecodeException(
            "RAW input exceeds $limit safety limit",
            DecodeStatus.INPUT.code,
            0,
        )
    }

    private fun throwCancelled(): Nothing = throw RawDecodeException(
        "RAW decode cancelled",
        DecodeStatus.CANCELLED.code,
        0,
    )
}
