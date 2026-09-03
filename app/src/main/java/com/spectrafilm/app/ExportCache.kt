/*
 * Spektrafilm for Android — content-addressed export cache storage (issue #179). GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Stores the ENCODED container bytes of a completed export under a complete content key, so a
 * later export of the same image with the same settings publishes bytes instead of re-rendering.
 *
 * Why the encoded container and not the engine's float buffer: the #119 baseline measured PNG16
 * encode at 3251 ms p50 on the Tier A device, so a cache that re-encoded on hit would miss the
 * approved 3000 ms p95 on that step alone, before any I/O. Caching after the encoder makes a hit
 * a validate-and-copy of 1.6 MB (JPEG) to 71 MB (TIFF16). That is also why the cache key must
 * carry the OutputDescriptor: the stored artifact is format-specific.
 *
 * Safety model — a false hit is the worst possible failure here, because it publishes a WRONG
 * image silently, so every uncertainty resolves to a miss:
 *   * the payload is renamed into place only after it is fully written and fsynced, and the
 *     metadata is renamed AFTER the payload, so a crash can only ever leave an orphan payload
 *     with no metadata, which reads as a miss;
 *   * a hit re-reads the recorded key, byte count and SHA-256 and rejects any disagreement;
 *   * a contract-version change invalidates every entry without needing to understand them.
 * Entries live in the app-private cache directory, which Android excludes from backup and which
 * the platform may evict on its own — both of which are the behaviour we want for user photos.
 */
package com.spectrafilm.app

import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.security.MessageDigest
import org.json.JSONObject

/**
 * A validated cache entry. [payload] is the exact container the encoder produced; publishing it
 * is a byte-for-byte copy.
 */
internal class ExportCacheEntry(val payload: File, val bytes: Long, val sha256: String)

internal class ExportCache(
    private val root: File,
    private val budget: Budget = Budget(),
    /** Bumped whenever stored bytes stop being interchangeable with freshly rendered ones. */
    private val contractVersion: String,
    /**
     * Injected so the JVM unit tests can run the miss/corruption paths at all: `android.util.Log`
     * is not mocked on the plain JVM and throws, and enabling `returnDefaultValues` globally to
     * dodge that would silently stub out every other Android call in the suite too.
     */
    private val log: (String, Throwable?) -> Unit = { message, failure ->
        Log.w(TAG, message, failure)
    },
) {
    /**
     * Byte, entry and age ceilings. Defaults are deliberately small: the tap-to-gallery case
     * needs the CURRENT image at the CURRENT settings, not a history, and a 16-bit export is
     * ~70 MB, so a large cache would trade a lot of the user's storage for little benefit.
     */
    class Budget(
        val maxBytes: Long = 320L * 1024 * 1024,
        val maxEntries: Int = 4,
        val maxAgeMillis: Long = 24L * 60 * 60 * 1000,
    )

    private val lock = Any()

    private fun payloadFile(key: String) = File(root, "$key.bin")
    private fun metaFile(key: String) = File(root, "$key.json")

    /**
     * The entry for [key], or null for any doubt whatsoever. Never throws: a cache is an
     * optimization, and a failure to read one must degrade to a normal render.
     */
    fun get(key: String): ExportCacheEntry? = synchronized(lock) {
        runCatching { readValidated(key) }.getOrElse { failure ->
            log("cache read failed for $key; treating as a miss", failure)
            discard(key)
            null
        }
    }

    private fun readValidated(key: String): ExportCacheEntry? {
        val payload = payloadFile(key)
        val meta = metaFile(key)
        // Metadata is renamed last, so its absence means the write never completed.
        if (!payload.isFile || !meta.isFile) return null
        val parsed = JSONObject(meta.readText(Charsets.UTF_8))
        if (parsed.optString("key") != key) return discardAnd(key, "key mismatch")
        if (parsed.optString("contract") != contractVersion) {
            return discardAnd(key, "contract ${parsed.optString("contract")} != $contractVersion")
        }
        val recordedBytes = parsed.optLong("bytes", -1L)
        if (recordedBytes != payload.length()) {
            return discardAnd(key, "length ${payload.length()} != recorded $recordedBytes")
        }
        val age = System.currentTimeMillis() - parsed.optLong("created_at", 0L)
        if (age > budget.maxAgeMillis || age < -SKEW_TOLERANCE_MILLIS) {
            return discardAnd(key, "age ${age}ms outside the budget")
        }
        val recordedDigest = parsed.optString("sha256")
        val actualDigest = digestOf(payload)
        if (!recordedDigest.equals(actualDigest, ignoreCase = true)) {
            return discardAnd(key, "payload digest mismatch")
        }
        return ExportCacheEntry(payload, recordedBytes, actualDigest)
    }

    private fun discardAnd(key: String, reason: String): ExportCacheEntry? {
        log("cache miss for $key: $reason", null)
        discard(key)
        return null
    }

    /**
     * Run [write] into a temporary file and, only if it completes, commit it under [key].
     * Returns whether the entry is now readable. Any failure leaves the cache unchanged.
     */
    fun put(key: String, write: (OutputStream) -> Unit): Boolean = synchronized(lock) {
        runCatching { commit(key, write) }.getOrElse { failure ->
            log("cache write failed for $key", failure)
            discard(key)
            discardTemp(key)
            false
        }
    }

    private fun commit(key: String, write: (OutputStream) -> Unit): Boolean {
        if (!root.isDirectory && !root.mkdirs()) throw IOException("cannot create $root")
        val tempPayload = File(root, "$key.bin.tmp")
        val digest = MessageDigest.getInstance("SHA-256")
        FileOutputStream(tempPayload).use { sink ->
            DigestingOutputStream(sink, digest).use(write)
            sink.flush()
            // Durability before the rename, so a crash cannot expose a half-written payload
            // under the final name.
            sink.fd.sync()
        }
        val bytes = tempPayload.length()
        if (bytes <= 0L) throw IOException("encoder produced $bytes bytes")
        // Payload first, metadata second: the reverse order could advertise an entry whose
        // bytes are not there yet.
        if (!tempPayload.renameTo(payloadFile(key))) throw IOException("cannot commit payload")
        writeMetadata(key, bytes, hex(digest.digest()))
        evict(keep = key)
        return true
    }

    /**
     * Take ownership of an already-encoded, already-digested staged file by moving it under
     * [key]. The encoder has just hashed these exact bytes to publish them, so re-reading 70 MB
     * to hash them again would be pure waste; [sha256] is that digest and [bytes] its length.
     *
     * The move is a rename within the app's own cache tree, so it is atomic and costs nothing.
     * Callers must invoke this only AFTER the artifact has published successfully — a cache is
     * a record of exports that happened, not of exports that were attempted.
     */
    fun adopt(key: String, staged: File, bytes: Long, sha256: String): Boolean =
        synchronized(lock) {
            runCatching {
                if (!root.isDirectory && !root.mkdirs()) throw IOException("cannot create $root")
                if (!staged.isFile) throw IOException("staged artifact is missing: $staged")
                if (staged.length() != bytes) {
                    throw IOException("staged length ${staged.length()} != declared $bytes")
                }
                val payload = payloadFile(key)
                payload.delete()
                if (!staged.renameTo(payload)) {
                    // Different filesystem, or a stage the platform will not let us move: copying
                    // is slower but the caller has already published, so this is off the hot path.
                    staged.inputStream().use { source ->
                        FileOutputStream(payload).use { sink ->
                            source.copyTo(sink, DEFAULT_BUFFER)
                            sink.flush()
                            sink.fd.sync()
                        }
                    }
                }
                writeMetadata(key, bytes, sha256)
                evict(keep = key)
                true
            }.getOrElse { failure ->
                log("cache adopt failed for $key", failure)
                discard(key)
                false
            }
        }

    private fun writeMetadata(key: String, bytes: Long, sha256: String) {
        val meta = JSONObject()
            .put("key", key)
            .put("contract", contractVersion)
            .put("bytes", bytes)
            .put("sha256", sha256)
            .put("created_at", System.currentTimeMillis())
        val tempMeta = File(root, "$key.json.tmp")
        FileOutputStream(tempMeta).use { sink ->
            sink.write(meta.toString().toByteArray(Charsets.UTF_8))
            sink.flush()
            sink.fd.sync()
        }
        if (!tempMeta.renameTo(metaFile(key))) throw IOException("cannot commit metadata")
    }

    /** Copy a validated entry to [sink]. Returns false if it stopped being valid. */
    fun copyTo(entry: ExportCacheEntry, sink: OutputStream): Boolean = runCatching {
        entry.payload.inputStream().use { source -> source.copyTo(sink, DEFAULT_BUFFER) }
        true
    }.getOrElse { failure ->
        log("cache copy failed", failure)
        false
    }

    fun discard(key: String) {
        payloadFile(key).delete()
        metaFile(key).delete()
    }

    private fun discardTemp(key: String) {
        File(root, "$key.bin.tmp").delete()
        File(root, "$key.json.tmp").delete()
    }

    /**
     * Enforce the byte/entry/age ceilings, newest first. [keep] is never evicted: it is the entry
     * the caller just committed, and evicting it would make the write pointless.
     */
    fun evict(keep: String? = null) = synchronized(lock) {
        val metas = root.listFiles { file -> file.name.endsWith(".json") } ?: return@synchronized
        val now = System.currentTimeMillis()
        val entries = metas
            .map { it.name.removeSuffix(".json") to it }
            .sortedByDescending { (_, file) -> file.lastModified() }
        var keptBytes = 0L
        var keptCount = 0
        for ((key, file) in entries) {
            val payload = payloadFile(key)
            val age = now - file.lastModified()
            val bytes = payload.length()
            val overBudget = keptBytes + bytes > budget.maxBytes || keptCount >= budget.maxEntries
            if (key != keep && (age > budget.maxAgeMillis || overBudget || !payload.isFile)) {
                discard(key)
                continue
            }
            keptBytes += bytes
            keptCount += 1
        }
        // Orphans: a payload whose metadata never landed, or a temp file from a killed write.
        root.listFiles()?.forEach { file ->
            if (file.name.endsWith(".tmp")) {
                file.delete()
            } else if (file.name.endsWith(".bin") &&
                !metaFile(file.name.removeSuffix(".bin")).isFile
            ) {
                file.delete()
            }
        }
    }

    private fun digestOf(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { source ->
            val buffer = ByteArray(DEFAULT_BUFFER)
            while (true) {
                val read = source.read(buffer)
                if (read <= 0) break
                digest.update(buffer, 0, read)
            }
        }
        return hex(digest.digest())
    }

    /** Digests while writing, so committing never has to read the payload back. */
    private class DigestingOutputStream(
        private val sink: OutputStream,
        private val digest: MessageDigest,
    ) : OutputStream() {
        override fun write(b: Int) {
            sink.write(b)
            digest.update(b.toByte())
        }

        override fun write(b: ByteArray, off: Int, len: Int) {
            sink.write(b, off, len)
            digest.update(b, off, len)
        }

        override fun flush() = sink.flush()

        // The FileOutputStream is owned by the caller's `use`, which needs it open afterwards
        // to fsync: closing the wrapper must not close the file.
        override fun close() = sink.flush()
    }

    internal companion object {
        private const val TAG = "SpektraExportCache"
        private const val DEFAULT_BUFFER = 1 shl 16

        /** Clocks move. A metadata timestamp slightly in the future is not corruption. */
        private const val SKEW_TOLERANCE_MILLIS = 60L * 60 * 1000

        fun hex(bytes: ByteArray): String {
            val out = StringBuilder(bytes.size * 2)
            for (b in bytes) {
                val v = b.toInt() and 0xFF
                out.append("0123456789abcdef"[v ushr 4])
                out.append("0123456789abcdef"[v and 0x0F])
            }
            return out.toString()
        }

        fun readAll(input: InputStream): ByteArray = input.readBytes()
    }
}

/**
 * Process-wide handle to the export cache (#179).
 *
 * The directory deliberately sits in a SUBDIRECTORY of cacheDir: `recoverAbandonedExportStages`
 * reclaims anything in the cacheDir root matching the encoder's `spektrafilm-export-*.part`
 * staging pattern, and a cache living beside those would be swept away on the next launch.
 * Being under cacheDir also means the existing backup allowlist already excludes it from cloud
 * backup and device transfer, which is what rendered photographs require.
 */
internal object ExportCaches {
    private const val DIR = "export-cache"

    @Volatile
    private var instance: ExportCache? = null

    fun of(context: android.content.Context): ExportCache {
        val app = context.applicationContext
        return instance ?: synchronized(this) {
            instance ?: ExportCache(
                root = java.io.File(app.cacheDir, DIR),
                contractVersion = contractVersionOf(app),
            ).also { instance = it }
        }
    }

    /**
     * Read from PackageManager rather than BuildConfig, which this module does not generate.
     *
     * `lastUpdateTime` is included deliberately: it changes on every install, so ANY rebuild
     * invalidates the whole cache automatically. Without it, a developer who rebuilds the engine
     * without touching the version would be served pre-change bytes from a cache that had no way
     * to know the pixels moved — the same silently-stale hazard the numeric contract exists for,
     * except hit constantly during development rather than rarely in release.
     */
    fun contractVersionOf(app: android.content.Context): String {
        val info = runCatching {
            app.packageManager.getPackageInfo(app.packageName, 0)
        }.getOrNull()
        @Suppress("DEPRECATION")
        val code = when {
            info == null -> -1L
            android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.P ->
                info.longVersionCode
            else -> info.versionCode.toLong()
        }
        val installed = info?.lastUpdateTime ?: -1L
        return ExportCacheKey.contractVersion(
            code.toInt(),
            "${info?.versionName ?: "?"}+$installed",
            ExportCacheKey.NUMERIC_CONTRACT,
        )
    }
}
