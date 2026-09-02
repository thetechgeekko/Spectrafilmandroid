/*
 * Spektrafilm for Android. GPLv3.
 */
package com.spectrafilm.app

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * The wall clock export metadata is stamped with.
 *
 * A complete-container SHA-256 gate (contract level C4, see
 * `docs/BIT_IDENTICAL_EXPORT_ROADMAP.md`) cannot repeat while the writer embeds the
 * current time: two identical renders would differ only in their TIFF `DateTime`.
 * The #177 benchmark pins this clock for the duration of a measured cell so those
 * digests are a function of the pixels alone. Production never pins it.
 */
internal object ExportClock {
    private const val EXIF_PATTERN = "yyyy:MM:dd HH:mm:ss"

    @Volatile
    private var pinnedMillis: Long? = null

    /** Epoch millis for export metadata: the pinned instant, else the system clock. */
    fun nowMillis(): Long = pinnedMillis ?: System.currentTimeMillis()

    /** EXIF/TIFF `DateTime` string for [nowMillis]. */
    fun exifDateTime(): String =
        SimpleDateFormat(EXIF_PATTERN, Locale.US).format(Date(nowMillis()))

    /** Pins the clock to [millis], or restores the system clock when null. */
    fun pin(millis: Long?) {
        pinnedMillis = millis
    }

    /** Runs [block] with the clock pinned to [millis], restoring the previous state. */
    fun <T> pinned(millis: Long, block: () -> T): T {
        val previous = pinnedMillis
        pinnedMillis = millis
        return try {
            block()
        } finally {
            pinnedMillis = previous
        }
    }
}
