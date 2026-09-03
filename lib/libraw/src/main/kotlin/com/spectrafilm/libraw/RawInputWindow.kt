/*
 * Spektrafilm for Android -- exact logical RAW ByteBuffer windows.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
package com.spectrafilm.libraw

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Prepares the caller's `position..limit` range for the JNI direct-buffer seam. */
internal object RawInputWindow {
    fun directRemaining(input: ByteBuffer): ByteBuffer {
        val remaining = input.remaining()
        RawInputLimits.requireWithinLimit(remaining)
        val logical = input.slice()
        return if (logical.isDirect) {
            logical.order(ByteOrder.nativeOrder())
        } else {
            ByteBuffer.allocateDirect(remaining)
                .order(ByteOrder.nativeOrder())
                .also { direct ->
                    direct.put(logical)
                    direct.flip()
                }
        }
    }
}
