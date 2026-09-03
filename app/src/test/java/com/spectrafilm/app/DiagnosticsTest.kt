/*
 * Spektrafilm for Android — bounded, privacy-preserving diagnostics tests. GPLv3.
 */
package com.spectrafilm.app

import java.io.File
import java.io.InputStream
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DiagnosticsTest {

    @Test
    fun engineTcLutCacheSnapshotIsAVisibleBoundedReportSection() {
        val summary =
            """{"schema":"spk.tc_lut_cache.v1","hits":9,"misses":3,"evictions":2,"cache_held_bytes":885000}"""
        val snapshot = summary + "x".repeat(Diagnostics.MAX_ENGINE_CACHE_BYTES * 2)

        val section = Diagnostics.engineCacheSection(snapshot)

        assertTrue(section.contains("Filming tc_lut cache"))
        assertTrue(section.contains("spk.tc_lut_cache.v1"))
        assertTrue(section.contains("\"hits\":9"))
        assertTrue(section.contains("\"evictions\":2"))
        assertTrue(section.toByteArray(Charsets.UTF_8).size <= Diagnostics.MAX_ENGINE_CACHE_BYTES)
        assertTrue(section.endsWith("[truncated]\n"))
    }

    @Test
    fun exportedDiagnosticsRedactUrisPathsImageNamesAndMetadata() {
        val raw = """
            render width=4032 height=3024 elapsed=81ms
            device: Pixel; Android 16 (API 36)
            uri=content://media/external/images/media/42
            path=/storage/emulated/0/DCIM/Camera/private-frame.dng
            sourceName=family-trip.jpg GPSLatitude=43.7 EXIF=secret cameraModel=PrivateCamera lensModel=private
            path=/storage/emulated/0/My Vacation/private draft.dng
            sourceName=family trip.jpg
            {"GPSLatitude":43.7,"cameraModel":"Secret Phone","sourceName":"Family Trip.jpg"}
            failed reading C:\Users\Alice\My Vacation\private draft.dng
            failed reading /storage/emulated/0/My Vacation/private draft.dng
            failed reading /private-folder
            image metadata was Family Trip.jpg
            at com.spectrafilm.app.MainActivity.render(MainActivity.kt:123)
        """.trimIndent()

        val safe = Diagnostics.sanitizeForExport(raw, Diagnostics.MAX_REPORT_BYTES)

        for (secret in listOf(
            "content://", "/storage/", "private-frame", "family-trip", "43.7", "PrivateCamera",
            "Secret Phone", "private", "Vacation", "family trip", "Alice", "draft.dng",
            "Family Trip", "private-folder",
        )) {
            assertFalse("leaked $secret in $safe", safe.contains(secret, ignoreCase = true))
        }
        assertTrue(safe.contains("width=4032 height=3024 elapsed=81ms"))
        assertTrue(safe.contains("device: Pixel; Android 16 (API 36)"))
        assertTrue(safe.contains("MainActivity.kt:123"))
        assertTrue(safe.contains("[redacted"))
    }

    @Test
    fun imageLineRedactionPreservesCrLfAndNonImageDiagnosticLines() {
        val crlf = charArrayOf(13.toChar(), 10.toChar()).concatToString()
        val raw = listOf(
            "device: Pixel; Android 16 (API 36)",
            "opened Another Family Photo.heic",
            "at MainActivity.render(MainActivity.kt:123)",
        ).joinToString(crlf)

        val safe = Diagnostics.sanitizeForExport(raw, Diagnostics.MAX_REPORT_BYTES)

        assertFalse(safe.contains("Another Family Photo"))
        assertTrue(safe.contains("device: Pixel; Android 16 (API 36)"))
        assertTrue(safe.contains("MainActivity.kt:123"))
        assertEquals(2, safe.windowed(crlf.length).count { it == crlf })
    }

    @Test(timeout = 2_000L)
    fun maximumReportLineWithoutAnImageExtensionSanitizesInLinearTime() {
        val raw = "x".repeat(Diagnostics.MAX_REPORT_BYTES)

        val safe = Diagnostics.sanitizeForExport(raw, Diagnostics.MAX_REPORT_BYTES)

        assertEquals(raw, safe)
    }

    @Test
    fun oversizedLogSnapshotIsLineAndUtf8Bounded() {
        val lines = sequence {
            repeat(2_000) { index ->
                yield("08-31 10:00:00.000  123  456 I Spektra: uri=content://private/$index ${"x".repeat(400)}")
            }
        }

        val captured = Diagnostics.collectLogLines(lines, pid = "123", requestedMaxLines = 50_000)

        assertTrue(captured.toByteArray(Charsets.UTF_8).size <= Diagnostics.MAX_LOG_BYTES)
        assertFalse(captured.contains("content://"))
        assertTrue(captured.contains("[truncated]"))
        val payloadLines = captured.lineSequence()
            .count { it.isNotBlank() && it != "[truncated]" }
        assertTrue(payloadLines <= Diagnostics.MAX_LOG_LINES)
    }

    @Test
    fun restoredCrashWithWrongFormatOrExpiredAgeIsRejectedAndRemoved() {
        val now = 2_000_000_000_000L
        val wrongVersion = tempFile("spektrafilm-diagnostics/999\nprivate")
        wrongVersion.setLastModified(now)
        assertNull(Diagnostics.readPersistedCrash(wrongVersion, now))
        assertFalse(wrongVersion.exists())

        val stale = tempFile("${Diagnostics.CRASH_FORMAT}\nold crash")
        stale.setLastModified(now - Diagnostics.MAX_CRASH_AGE_MS - 1L)
        assertNull(Diagnostics.readPersistedCrash(stale, now))
        assertFalse(stale.exists())

        val malformedUtf8 = tempFile(Diagnostics.CRASH_FORMAT + "\n")
        malformedUtf8.appendBytes(byteArrayOf(0xc3.toByte(), 0x28))
        malformedUtf8.setLastModified(now)
        assertNull(Diagnostics.readPersistedCrash(malformedUtf8, now))
        assertFalse(malformedUtf8.exists())
    }

    @Test
    fun emptyAndHugeNoNewlineRestoredRecordsAreRejectedAndRemoved() {
        val now = 2_000_000_000_000L
        val empty = tempFile("")
        empty.setLastModified(now)
        assertNull(Diagnostics.readPersistedCrash(empty, now))
        assertFalse(empty.exists())

        val hugeNoNewline = tempFile("x".repeat(Diagnostics.MAX_CRASH_BYTES * 4))
        hugeNoNewline.setLastModified(now)
        assertNull(Diagnostics.readPersistedCrash(hugeNoNewline, now))
        assertFalse(hugeNoNewline.exists())
    }

    @Test
    fun logcatDeadlineClosesANonTerminatingStream() {
        val input = BlockingInputStream()
        var timedOut = false
        val started = System.nanoTime()

        val captured = Diagnostics.collectLogcatWithDeadline(
            input = input,
            pid = "123",
            requestedMaxLines = 10,
            timeoutMs = 50L,
            onTimeout = { timedOut = true },
        )

        val elapsedMs = (System.nanoTime() - started) / 1_000_000L
        assertNull(captured)
        assertTrue(timedOut)
        assertTrue(input.wasClosed)
        assertTrue("deadline did not return promptly: ${elapsedMs}ms", elapsedMs < 2_000L)
    }

    @Test
    fun startupRetentionSweepDeletesExpiredCrashWithoutOpeningDiagnostics() {
        val now = 2_000_000_000_000L
        val stale = tempFile("${Diagnostics.CRASH_FORMAT}\nstale crash")
        stale.setLastModified(now - Diagnostics.MAX_CRASH_AGE_MS - 1L)

        var scheduled = false
        Diagnostics.scheduleRetentionSweep(
            stale,
            nowMillis = { now },
            schedule = { task ->
                scheduled = true
                task()
            },
        )

        assertTrue(scheduled)
        assertFalse(stale.exists())
    }

    @Test
    fun currentCrashIsRedactedAndBoundedWhenPersistedFileIsOversized() {
        val now = 2_000_000_000_000L
        val file = tempFile(
            "${Diagnostics.CRASH_FORMAT}\n" +
                "path=/data/user/0/com.spectrafilm.app/private.dng\n" +
                "z".repeat(Diagnostics.MAX_CRASH_BYTES * 2),
        )
        file.setLastModified(now)

        val restored = Diagnostics.readPersistedCrash(file, now)

        assertTrue(restored != null)
        assertFalse(restored!!.contains("/data/user"))
        assertTrue(restored.toByteArray(Charsets.UTF_8).size <= Diagnostics.MAX_CRASH_BYTES)
        assertTrue(restored.contains("[truncated]"))
        assertTrue("oversized persisted file was not compacted", file.length() <= Diagnostics.MAX_CRASH_BYTES)
    }

    private fun tempFile(text: String): File =
        Files.createTempFile("spectrafilm-diagnostics-", ".txt").toFile().apply {
            writeText(text, Charsets.UTF_8)
            deleteOnExit()
        }

    private class BlockingInputStream : InputStream() {
        @Volatile var wasClosed = false
            private set

        override fun read(): Int = synchronized(this) {
            while (!wasClosed) (this as java.lang.Object).wait()
            -1
        }

        override fun read(buffer: ByteArray, offset: Int, length: Int): Int = read()

        override fun close() {
            synchronized(this) {
                wasClosed = true
                (this as java.lang.Object).notifyAll()
            }
        }
    }
}
