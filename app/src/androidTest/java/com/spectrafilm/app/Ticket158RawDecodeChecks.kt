/*
 * Spektrafilm for Android — release/R8 RAW decode performance and byte-identity probe.
 * GPL-3.0-only.
 */
package com.spectrafilm.app

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import com.spectrafilm.libraw.RawDecoder
import java.io.File
import java.security.MessageDigest

/** Runs the production fd -> LibRaw -> JNI direct-buffer path in the minified target APK. */
object Ticket158RawDecodeChecks {
    @JvmStatic
    fun run(
        context: Context,
        rawUri: String,
        rawPath: String,
        repeats: Int,
        expectedSha256: String,
        exploratory: Boolean,
    ): String {
        require(repeats in 1..10)
        require(rawUri.isEmpty() != rawPath.isEmpty()) {
            "ticket #158 probe requires exactly one RAW source"
        }
        require(expectedSha256.isEmpty() == exploratory) {
            "ticket #158 exact qualification requires one pinned expected digest"
        }
        val openDescriptor = descriptorFactory(context, rawUri, rawPath)

        var exploratoryDigest: String? = null
        val evidence = StringBuilder(
            if (exploratory) {
                "TICKET158_RAW_RELEASE_R8_EXPLORATORY: RESULT (UNQUALIFIED)\n"
            } else {
                "TICKET158_RAW_RELEASE_R8: PASS\n"
            },
        )
        repeat(repeats) { iteration ->
            val descriptor = requireNotNull(openDescriptor()) {
                "cannot open ticket #158 RAW source"
            }
            try {
                val decodeAt = SystemClock.elapsedRealtimeNanos()
                val result = RawDecoder.decodeToLinear(descriptor.fd)
                try {
                    val decodeMs = elapsedMs(decodeAt)
                    val digestAt = SystemClock.elapsedRealtimeNanos()
                    val lease = result.acquireDataLease()
                    val digest = try {
                        val sha256 = MessageDigest.getInstance("SHA-256")
                        sha256.update(lease.data.duplicate())
                        toHex(sha256.digest())
                    } finally {
                        lease.close()
                    }
                    val digestMs = elapsedMs(digestAt)
                    if (exploratory && exploratoryDigest == null) {
                        exploratoryDigest = digest
                    }
                    val comparisonDigest =
                        if (exploratory) exploratoryDigest else expectedSha256
                    require(digest == comparisonDigest) {
                        "RAW output digest changed at repetition $iteration: " +
                            "$digest != $comparisonDigest"
                    }
                    evidence.append("iteration=").append(iteration)
                        .append(" width=").append(result.width)
                        .append(" height=").append(result.height)
                        .append(" decode_ms=").append(decodeMs)
                        .append(" digest_ms=").append(digestMs)
                        .append(" sha256=").append(digest)
                        .append('\n')
                } finally {
                    result.close()
                }
            } finally {
                descriptor.close()
            }
        }
        return evidence.toString()
    }

    private fun descriptorFactory(
        context: Context,
        rawUri: String,
        rawPath: String,
    ): () -> ParcelFileDescriptor? {
        if (rawUri.isNotEmpty()) {
            val uri = Uri.parse(rawUri)
            require(uri.scheme == "content") { "ticket #158 URI source must use content:" }
            return { context.contentResolver.openFileDescriptor(uri, "r") }
        }

        val externalRoot = requireNotNull(context.getExternalFilesDir(null)).canonicalFile
        val rawFile = File(rawPath).canonicalFile
        var parent = rawFile.parentFile
        var insideExternalRoot = false
        while (parent != null) {
            if (parent == externalRoot) {
                insideExternalRoot = true
                break
            }
            parent = parent.parentFile
        }
        require(insideExternalRoot) {
            "ticket #158 path must remain inside the target app external-files sandbox"
        }
        require(rawFile.isFile) { "ticket #158 RAW path is not a regular file" }
        return { ParcelFileDescriptor.open(rawFile, ParcelFileDescriptor.MODE_READ_ONLY) }
    }

    private fun elapsedMs(startedAt: Long): Double =
        (SystemClock.elapsedRealtimeNanos() - startedAt) / 1_000_000.0

    private fun toHex(bytes: ByteArray): String {
        val alphabet = "0123456789abcdef"
        val chars = CharArray(bytes.size * 2)
        var index = 0
        while (index < bytes.size) {
            val value = bytes[index].toInt() and 0xff
            chars[index * 2] = alphabet[value ushr 4]
            chars[index * 2 + 1] = alphabet[value and 0x0f]
            index += 1
        }
        return String(chars)
    }
}
