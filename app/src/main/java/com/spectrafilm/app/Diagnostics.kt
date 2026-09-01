/*
 * Spektrafilm for Android — bounded, user-exported diagnostics. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * A crash record stays in app-private storage for a short bounded lifetime. Logcat is
 * captured only after the user asks, and a report leaves the app only through an explicit
 * copy/share action. Every persistence and export boundary applies the same redaction and
 * UTF-8 byte limits; there is no automatic telemetry or crash upload.
 */
package com.spectrafilm.app

import android.content.Context
import android.content.Intent
import android.os.Process
import android.util.Log
import java.io.BufferedReader
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.InputStream
import java.io.InputStreamReader
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Callable
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException

/** Privacy-safe breadcrumb logger for engine and IO lifecycle events. */
object Diag {
    const val TAG = "Spektra"

    fun i(msg: String) = Log.i(TAG, msg)
    fun w(msg: String) = Log.w(TAG, msg)
    fun e(msg: String, t: Throwable? = null) {
        if (t != null) Log.e(TAG, msg, t) else Log.e(TAG, msg)
    }
}

object Diagnostics {
    internal const val CRASH_FORMAT = "spektrafilm-diagnostics/1"
    internal const val MAX_CRASH_BYTES = 64 * 1024
    internal const val MAX_LOG_BYTES = 128 * 1024
    internal const val MAX_REPORT_BYTES = 192 * 1024
    internal const val MAX_LOG_LINES = 500
    internal const val MAX_CRASH_AGE_MS = 7L * 24L * 60L * 60L * 1_000L

    private const val DIR = "diag"
    private const val CRASH_FILE = "last_crash.txt"
    private const val MAX_RAW_LINE_CHARS = 8 * 1024
    private const val LOGCAT_EXIT_TIMEOUT_MS = 1_000L
    private const val FUTURE_CLOCK_SKEW_MS = 5L * 60L * 1_000L
    private const val TRUNCATION_MARKER = "\n[truncated]\n"
    private val CRASH_HEADER_BYTES = "$CRASH_FORMAT\n".toByteArray(Charsets.UTF_8)

    private val PID_LINE_RE = Regex(".*\\s\\d+\\s\\d+\\s[VDIWEF]\\s.*")
    private val SENSITIVE_METADATA_RE = Regex(
        """(?i)"?\b(?:uri|path|file(?:name)?|display[_-]?name|source[_-]?name|gps(?:latitude|longitude)?|latitude|longitude|exif|camera(?:make|model)?|lens(?:make|model)?)\b"?\s*[:=]\s*[^\r\n]*""",
    )
    private val URI_RE = Regex(
        """(?i)\b[a-z][a-z0-9+.-]{1,31}://[^\r\n]*""",
    )
    private val WINDOWS_PATH_RE = Regex(
        """(?i)\b[A-Z]:\\[^\r\n]*""",
    )
    private val UNIX_PATH_RE = Regex(
        """(?<![\w])/(?:[^/\r\n]+/)*[^\r\n]*""",
    )
    private val IMAGE_NAME_RE = Regex(
        """(?im)^[^\r\n]*\.(?:dng|raw|nef|cr2|cr3|arw|raf|rw2|orf|pef|srw|jpg|jpeg|heic|heif|png|tif|tiff)(?![\w])[^\r\n]*""",
    )

    private val crashFileLock = Any()
    @Volatile private var installed = false

    /** Install once, persist a redacted crash record, then chain to Android's handler. */
    fun installCrashHandler(context: Context) {
        if (installed) return
        installed = true
        val app = context.applicationContext
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            runCatching { writeCrash(app, thread, throwable) }
            previous?.uncaughtException(thread, throwable)
        }
        // Retention is enforced on every process start, not only when the user opens
        // Diagnostics. Keep filesystem work away from the UI thread.
        runCatching { scheduleRetentionSweep(crashFile(app)) }
    }

    private fun crashFile(context: Context): File =
        File(File(context.filesDir, DIR).apply { mkdirs() }, CRASH_FILE)

    private fun writeCrash(context: Context, thread: Thread, throwable: Throwable) {
        val timestamp = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())
        val raw = buildString {
            append(CRASH_FORMAT).append('\n')
            append("=== Spektrafilm crash @ ").append(timestamp).append(" ===\n")
            append("thread: ").append(thread.name).append('\n')
            append(Log.getStackTraceString(throwable))
        }
        synchronized(crashFileLock) {
            crashFile(context).writeText(
                sanitizeForExport(raw, MAX_CRASH_BYTES),
                Charsets.UTF_8,
            )
        }
    }

    /** The current-format, recent persisted crash trace, or null. */
    fun lastCrash(context: Context): String? =
        readPersistedCrash(crashFile(context), System.currentTimeMillis())

    internal fun readPersistedCrash(file: File, nowMillis: Long): String? {
        return synchronized(crashFileLock) {
            if (!file.isFile) return@synchronized null
            if (purgeInvalidPersistedCrashLocked(file, nowMillis)) return@synchronized null
            val raw = runCatching { readFilePrefix(file, MAX_CRASH_BYTES) }.getOrNull()
                ?: run {
                    runCatching { file.delete() }
                    return@synchronized null
                }
            val safe = sanitizeForExport(raw, MAX_CRASH_BYTES)
            if (file.length() > MAX_CRASH_BYTES) {
                // A corrupt/manual restore must not leave an unbounded file resident merely
                // because reads are bounded. Compact it to the same safe representation.
                runCatching { file.writeText(safe, Charsets.UTF_8) }
            }
            safe
        }
    }

    /** Remove an invalid-format or expired record without requiring the diagnostics UI. */
    internal fun purgeInvalidPersistedCrash(file: File, nowMillis: Long): Boolean =
        synchronized(crashFileLock) {
            purgeInvalidPersistedCrashLocked(file, nowMillis)
        }

    private fun purgeInvalidPersistedCrashLocked(file: File, nowMillis: Long): Boolean {
        if (!file.isFile) return false
        val modified = file.lastModified()
        val invalidAge = modified <= 0L ||
            modified > nowMillis + FUTURE_CLOCK_SKEW_MS ||
            nowMillis - modified > MAX_CRASH_AGE_MS
        val validHeader = !invalidAge && hasExactCrashHeader(file)
        if (!invalidAge && validHeader) return false
        runCatching { file.delete() }
        return true
    }

    private fun hasExactCrashHeader(file: File): Boolean = runCatching {
        if (file.length() < CRASH_HEADER_BYTES.size) return@runCatching false
        val actual = ByteArray(CRASH_HEADER_BYTES.size)
        FileInputStream(file).use { input ->
            var offset = 0
            while (offset < actual.size) {
                val read = input.read(actual, offset, actual.size - offset)
                if (read <= 0) return@runCatching false
                offset += read
            }
        }
        actual.contentEquals(CRASH_HEADER_BYTES)
    }.getOrDefault(false)

    /** Injectable scheduler keeps the process-start retention behavior unit-testable. */
    internal fun scheduleRetentionSweep(
        file: File,
        nowMillis: () -> Long = System::currentTimeMillis,
        schedule: ((() -> Unit) -> Unit) = { task ->
            Thread({ task() }, "Spektrafilm-diagnostics-retention").apply {
                isDaemon = true
                start()
            }
        },
    ) {
        schedule {
            runCatching { purgeInvalidPersistedCrash(file, nowMillis()) }
        }
    }

    private fun readFilePrefix(file: File, maxBytes: Int): String {
        val output = ByteArrayOutputStream(minOf(maxBytes, 8 * 1024))
        var truncated = false
        FileInputStream(file).use { input ->
            val buffer = ByteArray(4 * 1024)
            var remaining = maxBytes
            while (remaining > 0) {
                val read = input.read(buffer, 0, minOf(buffer.size, remaining))
                if (read < 0) break
                if (read == 0) throw java.io.IOException("zero-length crash-record read")
                output.write(buffer, 0, read)
                remaining -= read
            }
            if (remaining == 0 && input.read() >= 0) truncated = true
        }
        val decoded = Charsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(output.toByteArray()))
            .toString()
        return buildString {
            append(decoded)
            if (truncated) append(TRUNCATION_MARKER)
        }
    }

    fun clearLastCrash(context: Context) {
        synchronized(crashFileLock) {
            runCatching { crashFile(context).delete() }
        }
    }

    /**
     * One-shot snapshot of this process's recent logcat. The requested line count is
     * clamped and the result is redacted and byte-bounded before it reaches the UI.
     */
    fun captureLogcat(maxLines: Int = MAX_LOG_LINES): String {
        val pid = Process.myPid().toString()
        val boundedLines = maxLines.coerceIn(1, MAX_LOG_LINES)
        return try {
            val process = ProcessBuilder(
                listOf("logcat", "-d", "-v", "threadtime", "-t", boundedLines.toString()),
            ).redirectErrorStream(true).start()
            try {
                val captured = collectLogcatWithDeadline(
                    input = process.inputStream,
                    pid = pid,
                    requestedMaxLines = boundedLines,
                    timeoutMs = LOGCAT_EXIT_TIMEOUT_MS,
                    onTimeout = { process.destroyForcibly() },
                )
                if (!process.waitFor(100L, TimeUnit.MILLISECONDS)) {
                    process.destroyForcibly()
                }
                captured?.ifBlank { "(no logcat lines for this process)" }
                    ?: "logcat capture failed"
            } finally {
                if (process.isAlive) process.destroyForcibly()
            }
        } catch (_: Exception) {
            "logcat capture failed"
        }
    }

    /** Drain on a bounded worker so the deadline can stop a process that never closes stdout. */
    internal fun collectLogcatWithDeadline(
        input: InputStream,
        pid: String,
        requestedMaxLines: Int,
        timeoutMs: Long,
        onTimeout: () -> Unit,
    ): String? {
        require(timeoutMs > 0L)
        val executor = Executors.newSingleThreadExecutor { runnable ->
            Thread(runnable, "Spektrafilm-logcat-drain").apply { isDaemon = true }
        }
        val future = executor.submit(Callable {
            BufferedReader(InputStreamReader(input, Charsets.UTF_8)).useLines { lines ->
                collectLogLines(lines, pid, requestedMaxLines)
            }
        })
        return try {
            future.get(timeoutMs, TimeUnit.MILLISECONDS)
        } catch (_: TimeoutException) {
            runCatching { onTimeout() }
            runCatching { input.close() }
            future.cancel(true)
            null
        } catch (_: Exception) {
            null
        } finally {
            executor.shutdownNow()
            runCatching { executor.awaitTermination(100L, TimeUnit.MILLISECONDS) }
        }
    }

    internal fun collectLogLines(
        lines: Sequence<String>,
        pid: String,
        requestedMaxLines: Int,
    ): String {
        val maxLines = requestedMaxLines.coerceIn(1, MAX_LOG_LINES)
        val output = BoundedUtf8Builder(MAX_LOG_BYTES)
        var examined = 0
        for (unboundedLine in lines) {
            if (examined >= maxLines) {
                output.markTruncated()
                break
            }
            examined++
            val rawLine = unboundedLine.take(MAX_RAW_LINE_CHARS)
            if (!rawLine.contains(" $pid ") && rawLine.matches(PID_LINE_RE)) continue
            val line = if (unboundedLine.length > MAX_RAW_LINE_CHARS) {
                rawLine + TRUNCATION_MARKER
            } else {
                rawLine
            }
            if (!output.append(redact(line) + '\n')) break
        }
        return output.toString()
    }

    /** Build a report locally. Calling this function does not export it. */
    fun buildReport(context: Context): String {
        val header = "Spektrafilm diagnostics\n" +
            "app: ${appVersion(context)}\n" +
            "device: ${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}; " +
            "Android ${android.os.Build.VERSION.RELEASE} " +
            "(API ${android.os.Build.VERSION.SDK_INT})\n\n"
        val crash = lastCrash(context)?.let { "--- last crash ---\n$it\n\n" } ?: ""
        return sanitizeForExport(
            header + crash + "--- logcat (recent) ---\n" + captureLogcat(),
            MAX_REPORT_BYTES,
        )
    }

    fun appVersion(context: Context): String = runCatching {
        val packageInfo = context.packageManager.getPackageInfo(context.packageName, 0)
        val versionCode: Long = if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.P) {
            packageInfo.longVersionCode
        } else {
            @Suppress("DEPRECATION")
            packageInfo.versionCode.toLong()
        }
        "${packageInfo.versionName} ($versionCode)"
    }.getOrDefault("unknown")

    /** Explicit user export through Android's share sheet; sanitize again at the boundary. */
    fun share(context: Context, report: String) {
        val safeReport = sanitizeForExport(report, MAX_REPORT_BYTES)
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_SUBJECT, "Spektrafilm diagnostics")
            putExtra(Intent.EXTRA_TEXT, safeReport)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        context.startActivity(Intent.createChooser(send, "Share diagnostics").apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        })
    }

    internal fun sanitizeForExport(raw: String, maxBytes: Int): String {
        require(maxBytes >= TRUNCATION_MARKER.toByteArray(Charsets.UTF_8).size)
        // Bound before regex work as well as after it, so a hostile log line cannot make
        // redaction itself an unbounded allocation surface.
        val boundedInput = boundUtf8(raw, maxBytes)
        return boundUtf8(redact(boundedInput), maxBytes)
    }

    private fun redact(raw: String): String = raw
        .replace(SENSITIVE_METADATA_RE, "[redacted-metadata]")
        .replace(URI_RE, "[redacted-uri]")
        .replace(WINDOWS_PATH_RE, "[redacted-path]")
        .replace(UNIX_PATH_RE, "[redacted-path]")
        .replace(IMAGE_NAME_RE, "[redacted-image]")

    private fun boundUtf8(raw: String, maxBytes: Int): String {
        var index = 0
        var bytes = 0
        while (index < raw.length) {
            val codePoint = Character.codePointAt(raw, index)
            val nextBytes = utf8Bytes(codePoint)
            if (bytes + nextBytes > maxBytes) break
            bytes += nextBytes
            index += Character.charCount(codePoint)
        }
        if (index == raw.length) return raw

        val markerBytes = TRUNCATION_MARKER.toByteArray(Charsets.UTF_8).size
        val contentLimit = maxBytes - markerBytes
        index = 0
        bytes = 0
        while (index < raw.length) {
            val codePoint = Character.codePointAt(raw, index)
            val nextBytes = utf8Bytes(codePoint)
            if (bytes + nextBytes > contentLimit) break
            bytes += nextBytes
            index += Character.charCount(codePoint)
        }
        return raw.substring(0, index) + TRUNCATION_MARKER
    }

    private fun utf8Bytes(codePoint: Int): Int = when {
        codePoint <= 0x7f -> 1
        codePoint <= 0x7ff -> 2
        codePoint <= 0xffff -> 3
        else -> 4
    }

    private class BoundedUtf8Builder(private val maxBytes: Int) {
        private val text = StringBuilder()
        private val markerBytes = TRUNCATION_MARKER.toByteArray(Charsets.UTF_8).size
        private var usedBytes = 0
        private var sealed = false

        fun append(value: String): Boolean {
            if (sealed) return false
            val contentLimit = maxBytes - markerBytes
            var index = 0
            while (index < value.length) {
                val codePoint = Character.codePointAt(value, index)
                val count = utf8Bytes(codePoint)
                if (usedBytes + count > contentLimit) {
                    markTruncated()
                    return false
                }
                text.appendCodePoint(codePoint)
                usedBytes += count
                index += Character.charCount(codePoint)
            }
            return true
        }

        fun markTruncated() {
            if (sealed) return
            text.append(TRUNCATION_MARKER)
            usedBytes += markerBytes
            sealed = true
        }

        override fun toString(): String = text.toString()
    }
}
