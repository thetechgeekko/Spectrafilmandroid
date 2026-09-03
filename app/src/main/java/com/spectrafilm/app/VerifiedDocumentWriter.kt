/*
 * Spektrafilm for Android — checked SAF document publication. GPLv3.
 */
package com.spectrafilm.app

import android.content.Context
import android.net.Uri
import java.io.FileOutputStream
import java.io.IOException
import java.security.MessageDigest

/**
 * ACTION_CREATE_DOCUMENT returns a newly-created destination (duplicate names are
 * uniquified by the picker). Write, flush/sync, close, then reopen and SHA-256 verify.
 * On failure the app-created document is deleted when the provider permits it.
 */
internal fun writeVerifiedNewDocument(
    context: Context,
    uri: Uri,
    bytes: ByteArray,
    maxBytes: Int,
) {
    val resolver = context.applicationContext.contentResolver
    try {
        require(uri.scheme == "content") { "SAF destination must be content://" }
        if (bytes.isEmpty()) throw IOException("refusing to publish an empty document")
        if (bytes.size > maxBytes) {
            throw DocumentLimitException("document is ${bytes.size} bytes; limit is $maxBytes")
        }
        val expected = MessageDigest.getInstance("SHA-256").digest(bytes)
        val output = resolver.openOutputStream(uri, "rwt")
            ?: throw IOException("could not open SAF destination")
        output.use { stream ->
            stream.write(bytes)
            stream.flush()
            if (stream is FileOutputStream) stream.fd.sync()
        }

        val input = resolver.openInputStream(uri)
            ?: throw IOException("could not reopen SAF destination")
        val digest = MessageDigest.getInstance("SHA-256")
        var count = 0
        input.use { stream ->
            val buffer = ByteArray(64 * 1024)
            while (true) {
                val read = stream.read(buffer)
                if (read < 0) break
                count += read
                if (count > maxBytes) throw DocumentLimitException("SAF readback exceeds $maxBytes bytes")
                digest.update(buffer, 0, read)
            }
        }
        if (count != bytes.size || !MessageDigest.isEqual(expected, digest.digest())) {
            throw IOException("SAF destination failed digest verification")
        }
    } catch (failure: Throwable) {
        if (uri.scheme == "content") {
            val cleanup = runCatching { resolver.delete(uri, null, null) }
            cleanup.exceptionOrNull()?.let(failure::addSuppressed)
            if (cleanup.getOrDefault(0) != 1) {
                failure.addSuppressed(IOException("provider did not delete failed app-created document"))
            }
        }
        throw failure
    }
}
