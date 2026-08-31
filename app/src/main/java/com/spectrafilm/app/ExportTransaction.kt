/*
 * Spektrafilm for Android — crash-recoverable, digest-verified export commit. GPLv3.
 */
package com.spectrafilm.app

import android.content.ContentValues
import android.content.ContentUris
import android.content.Context
import android.net.Uri
import android.os.Build
import android.provider.MediaStore
import androidx.annotation.RequiresApi
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.CancellationException
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

internal data class EncodedArtifact(
    val file: File,
    val length: Long,
    val sha256: String,
) {
    companion object {
        fun fromCompletedFile(
            file: File,
            encoderByteCount: Long = file.length(),
            isCancelled: () -> Boolean = { false },
        ): EncodedArtifact {
            if (!file.isFile) throw IOException("encoded artifact is missing: $file")
            val actualLength = file.length()
            if (encoderByteCount <= 0L) throw IOException("encoder returned no bytes")
            if (encoderByteCount != actualLength) {
                throw IOException("encoder returned $encoderByteCount bytes but staged file has $actualLength")
            }
            val measured = digest(file, isCancelled)
            return EncodedArtifact(file, measured.length, measured.sha256)
        }
    }
}

internal data class ExportDestinationSpec(
    val displayName: String,
    val mimeType: String,
    val relativePath: String,
) {
    init {
        require(displayName.isNotBlank() && displayName.length <= 255)
        require('/' !in displayName && '\\' !in displayName && '\u0000' !in displayName)
        require(mimeType.startsWith("image/") && mimeType.length <= 127)
        require(relativePath.isNotBlank() && relativePath.length <= 512)
    }
}

internal enum class StoredExportState { PENDING, PUBLISHED, MISSING, UNKNOWN }

internal class ExportReconciliationPendingException(
    message: String,
    cause: Throwable? = null,
) : IOException(message, cause)

internal interface PendingExportBackend {
    fun reserve(spec: ExportDestinationSpec): String
    fun openWrite(token: String): OutputStream
    fun openRead(token: String): InputStream
    fun publish(token: String, spec: ExportDestinationSpec): Int
    fun delete(token: String): Int
    fun state(token: String): StoredExportState
    /** Provider-owned discovery closes the insert-before-journal process-kill window. */
    fun pendingTokens(): Set<String> = emptySet()
}

internal interface PendingExportJournal {
    fun add(token: String)
    fun remove(token: String)
    fun tokens(): Set<String>
}

internal data class ExportRecoveryReport(
    val examined: Int,
    val removedPending: Int,
    val retiredCommittedOrMissing: Int,
    val retainedForRetry: Int,
    val removedAbandonedStages: Int = 0,
    val retainedAbandonedStages: Int = 0,
)

internal data class AbandonedExportStageRecoveryReport(
    val examined: Int,
    val removed: Int,
    val retained: Int,
)

/**
 * Synchronous commit state machine. Call from Dispatchers.IO. A success is returned
 * only after destination close, readback length/SHA-256 verification, one publish
 * update, and confirmation that the row is no longer pending.
 */
internal class ExportTransaction(
    private val backend: PendingExportBackend,
    private val journal: PendingExportJournal,
) {
    fun commit(
        artifact: EncodedArtifact,
        spec: ExportDestinationSpec,
        isCancelled: () -> Boolean = { false },
        onPublished: (String) -> Unit = {},
    ): String {
        throwIfExportCancelled(isCancelled)
        verifyArtifact(artifact, isCancelled)
        throwIfExportCancelled(isCancelled)
        val token = backend.reserve(spec)
        var publicationReported = false
        fun reportPublication() {
            if (publicationReported) return
            publicationReported = true
            onPublished(token)
        }
        try {
            journal.add(token)
        } catch (failure: Throwable) {
            val deleted = runCatching { backend.delete(token) }
            deleted.exceptionOrNull()?.let(failure::addSuppressed)
            val after = runCatching { backend.state(token) }.getOrDefault(StoredExportState.UNKNOWN)
            if (deleted.getOrDefault(0) != 1 && after != StoredExportState.MISSING) {
                failure.addSuppressed(
                    IOException("unjournaled tagged pending row will be reconciled at next recovery"),
                )
            }
            throw failure
        }

        try {
            throwIfExportCancelled(isCancelled)
            val copied = copyAndDigest(artifact.file, backend.openWrite(token), isCancelled)
            if (copied.length != artifact.length || copied.sha256 != artifact.sha256) {
                throw IOException("destination write digest differs from staged artifact")
            }

            val readback = backend.openRead(token).use { digest(it, isCancelled) }
            if (readback.length != artifact.length || readback.sha256 != artifact.sha256) {
                throw IOException("destination readback digest differs from staged artifact")
            }

            // Cancellation must win before the provider's publication linearization point.
            // If it arrives while publish() is in flight, the catch path observes provider state:
            // a confirmed PUBLISHED row still wins, while PENDING is deleted below.
            throwIfExportCancelled(isCancelled)
            val updated = backend.publish(token, spec)
            if (updated != 1) throw IOException("MediaStore publish updated $updated rows; expected 1")
            if (backend.state(token) != StoredExportState.PUBLISHED) {
                throw IOException("destination did not confirm published state")
            }
            // This callback is inside the synchronous transaction, immediately after provider
            // confirmation and before any caller can hit a cancellable dispatcher return.
            reportPublication()
            journal.remove(token)
            return token
        } catch (failure: Throwable) {
            val observed = runCatching { backend.state(token) }.getOrDefault(StoredExportState.UNKNOWN)
            // Once the provider confirms publication, commit wins over a late cancellation or
            // journal-cleanup failure. Recovery will idempotently retire a leftover journal token.
            if (observed == StoredExportState.PUBLISHED) {
                reportPublication()
                runCatching { journal.remove(token) }
                return token
            }
            if (observed == StoredExportState.UNKNOWN) {
                throw ExportReconciliationPendingException(
                    "export outcome is indeterminate; restart or reconcile before retrying",
                    failure,
                )
            }
            cleanupUncommitted(token, observed, failure)
            throw failure
        }
    }

    fun recover(): ExportRecoveryReport {
        var removed = 0
        var retired = 0
        var retained = 0
        val journaledTokens = journal.tokens()
        // Discovery is the only way to see a process-kill between provider insert and
        // durable journal add. Treat an unavailable provider query as reconciliation
        // failure instead of silently permitting another export.
        val discoveredTokens = backend.pendingTokens()
        val tokens = linkedSetOf<String>().apply {
            addAll(journaledTokens)
            addAll(discoveredTokens)
        }
        for (token in tokens) {
            val wasJournaled = token in journaledTokens
            when (runCatching { backend.state(token) }.getOrDefault(StoredExportState.UNKNOWN)) {
                StoredExportState.PUBLISHED, StoredExportState.MISSING -> {
                    if (!wasJournaled) {
                        retired++
                    } else {
                        runCatching { journal.remove(token) }
                            .onSuccess { retired++ }
                            .onFailure { retained++ }
                    }
                }
                StoredExportState.PENDING -> {
                    val deleted = runCatching { backend.delete(token) }.getOrDefault(0)
                    val after = runCatching { backend.state(token) }.getOrDefault(StoredExportState.UNKNOWN)
                    if (deleted == 1 || after == StoredExportState.MISSING) {
                        if (!wasJournaled) {
                            removed++
                        } else {
                            runCatching { journal.remove(token) }
                                .onSuccess { removed++ }
                                .onFailure { retained++ }
                        }
                    } else {
                        retained++
                    }
                }
                StoredExportState.UNKNOWN -> retained++
            }
        }
        return ExportRecoveryReport(tokens.size, removed, retired, retained)
    }

    private fun cleanupUncommitted(
        token: String,
        observed: StoredExportState,
        primary: Throwable,
    ) {
        if (observed == StoredExportState.MISSING) {
            runCatching { journal.remove(token) }.exceptionOrNull()?.let(primary::addSuppressed)
            return
        }
        if (observed != StoredExportState.PENDING) {
            // Unknown may already be published; retain the journal for safe next-start recovery.
            return
        }
        val deleted = runCatching { backend.delete(token) }
        deleted.exceptionOrNull()?.let(primary::addSuppressed)
        val after = runCatching { backend.state(token) }.getOrDefault(StoredExportState.UNKNOWN)
        if (deleted.getOrDefault(0) == 1 || after == StoredExportState.MISSING) {
            runCatching { journal.remove(token) }.exceptionOrNull()?.let(primary::addSuppressed)
        } else {
            primary.addSuppressed(IOException("pending destination cleanup will be retried"))
        }
    }

    private fun verifyArtifact(artifact: EncodedArtifact, isCancelled: () -> Boolean) {
        if (!artifact.file.isFile) throw IOException("staged artifact disappeared")
        val measured = digest(artifact.file, isCancelled)
        if (measured.length != artifact.length || measured.sha256 != artifact.sha256) {
            throw IOException("staged artifact changed after encoding")
        }
    }

    private fun copyAndDigest(
        file: File,
        output: OutputStream,
        isCancelled: () -> Boolean,
    ): DigestAndLength {
        val md = MessageDigest.getInstance("SHA-256")
        var count = 0L
        output.use { sink ->
            FileInputStream(file).use { source ->
                val buffer = ByteArray(COPY_BUFFER_BYTES)
                while (true) {
                    throwIfExportCancelled(isCancelled)
                    val read = source.read(buffer)
                    if (read < 0) break
                    sink.write(buffer, 0, read)
                    md.update(buffer, 0, read)
                    count += read
                    throwIfExportCancelled(isCancelled)
                }
            }
            sink.flush()
            if (sink is FileOutputStream) sink.fd.sync()
        }
        return DigestAndLength(count, md.digest().toHex())
    }
}

@RequiresApi(Build.VERSION_CODES.Q)
internal class AndroidPendingExportBackend(context: Context) : PendingExportBackend {
    private val resolver = context.applicationContext.contentResolver
    private val ownerPackageName = context.applicationContext.packageName

    override fun reserve(spec: ExportDestinationSpec): String {
        check(Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
        val values = ContentValues().apply {
            val extension = spec.displayName.substringAfterLast('.', "tmp")
                .filter { it.isLetterOrDigit() }
                .take(12)
                .ifEmpty { "tmp" }
            put(
                MediaStore.Images.Media.DISPLAY_NAME,
                "$MEDIASTORE_PENDING_NAME_PREFIX${UUID.randomUUID()}.$extension",
            )
            put(MediaStore.Images.Media.MIME_TYPE, spec.mimeType)
            put(MediaStore.Images.Media.RELATIVE_PATH, spec.relativePath)
            put(MediaStore.Images.Media.IS_PENDING, 1)
        }
        return resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)?.toString()
            ?: throw IOException("MediaStore insert failed")
    }

    override fun openWrite(token: String): OutputStream =
        resolver.openOutputStream(Uri.parse(token), "w")
            ?: throw IOException("could not open pending MediaStore output")

    override fun openRead(token: String): InputStream =
        resolver.openInputStream(Uri.parse(token))
            ?: throw IOException("could not reopen pending MediaStore output")

    override fun publish(token: String, spec: ExportDestinationSpec): Int = resolver.update(
        Uri.parse(token),
        ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, spec.displayName)
            put(MediaStore.Images.Media.IS_PENDING, 0)
        },
        null,
        null,
    )

    override fun delete(token: String): Int = resolver.delete(Uri.parse(token), null, null)

    @Suppress("DEPRECATION")
    override fun pendingTokens(): Set<String> {
        val collection = MediaStore.setIncludePending(MediaStore.Images.Media.EXTERNAL_CONTENT_URI)
        val cursor = resolver.query(
            collection,
            arrayOf(MediaStore.Images.Media._ID),
            "${MediaStore.Images.Media.IS_PENDING}=? AND " +
                "${MediaStore.Images.Media.OWNER_PACKAGE_NAME}=? AND " +
                "(${MediaStore.Images.Media.RELATIVE_PATH}=? OR ${MediaStore.Images.Media.RELATIVE_PATH}=?) AND " +
                "${MediaStore.Images.Media.DISPLAY_NAME} LIKE ?",
            arrayOf(
                "1",
                ownerPackageName,
                MEDIASTORE_EXPORT_RELATIVE_PATH,
                "$MEDIASTORE_EXPORT_RELATIVE_PATH/",
                "$MEDIASTORE_PENDING_NAME_PREFIX%",
            ),
            null,
        ) ?: throw IOException("MediaStore pending-row discovery returned no cursor")
        return cursor.use {
            buildSet {
                while (it.moveToNext()) {
                    add(
                        ContentUris.withAppendedId(
                            MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
                            it.getLong(0),
                        ).toString(),
                    )
                }
            }
        }
    }

    @Suppress("DEPRECATION")
    override fun state(token: String): StoredExportState {
        return try {
            val parsed = Uri.parse(token)
            // A syntactically foreign journal value cannot name a row owned by this backend.
            // Provider/query exceptions for an otherwise valid MediaStore URI remain UNKNOWN:
            // an exception is not evidence that the row is absent.
            if (parsed.scheme != "content" || parsed.authority != MediaStore.AUTHORITY) {
                return StoredExportState.MISSING
            }
            val uri = MediaStore.setIncludePending(parsed)
            val cursor = resolver.query(
                uri,
                arrayOf(MediaStore.Images.Media.IS_PENDING),
                null,
                null,
                null,
            ) ?: return StoredExportState.UNKNOWN
            cursor.use {
                if (!it.moveToFirst()) StoredExportState.MISSING
                else if (it.getInt(0) == 0) StoredExportState.PUBLISHED else StoredExportState.PENDING
            }
        } catch (_: SecurityException) {
            StoredExportState.UNKNOWN
        } catch (_: IllegalArgumentException) {
            StoredExportState.UNKNOWN
        }
    }
}

internal class SharedPreferencesPendingExportJournal(context: Context) : PendingExportJournal {
    private val prefs = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    override fun add(token: String) = synchronized(PROCESS_LOCK) {
        val next = tokens().toMutableSet().apply { add(token) }
        if (next.size > MAX_TOKENS) throw IOException("pending export journal is full")
        commit(next)
    }

    override fun remove(token: String) = synchronized(PROCESS_LOCK) {
        val next = tokens().toMutableSet().apply { remove(token) }
        commit(next)
    }

    override fun tokens(): Set<String> = synchronized(PROCESS_LOCK) {
        prefs.getStringSet(KEY_TOKENS, emptySet())?.toSet() ?: emptySet()
    }

    private fun commit(tokens: Set<String>) {
        check(prefs.edit().putStringSet(KEY_TOKENS, tokens.toSet()).commit()) {
            "could not commit pending export journal"
        }
    }

    private companion object {
        val PROCESS_LOCK = Any()
        const val PREFS = "pending_media_exports_v1"
        const val KEY_TOKENS = "uri_tokens"
        const val MAX_TOKENS = 128
    }
}

@RequiresApi(Build.VERSION_CODES.Q)
internal fun mediaStoreExportTransaction(context: Context): ExportTransaction = ExportTransaction(
    AndroidPendingExportBackend(context),
    SharedPreferencesPendingExportJournal(context),
)

internal fun recoverPendingMediaStoreExports(context: Context): ExportRecoveryReport =
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
        synchronized(mediaStoreExportProcessLock) {
            mediaStoreExportTransaction(context).recover()
        }
    } else {
        ExportRecoveryReport(0, 0, 0, 0)
    }

internal class ProcessStageRecoveryGate(val cutoffMillis: Long) {
    private val started = AtomicBoolean(false)

    init {
        require(cutoffMillis > 0L)
    }

    /** Marks the stage sweep consumed even when [operation] throws; it is never safe to retry it. */
    fun <T> runOnce(operation: (Long) -> T): T? {
        if (!started.compareAndSet(false, true)) return null
        return operation(cutoffMillis)
    }
}

private val processStageRecoveryGate = AtomicReference<ProcessStageRecoveryGate?>(null)
private val exportProviderRecoveryInFlight = AtomicBoolean(false)
private val exportProviderRecoveryCompleted = AtomicBoolean(false)

/** Capture the first Activity's cutoff synchronously; later recreations cannot move it forward. */
internal fun registerExportProcessStageCutoff(candidateMillis: Long): Long {
    val candidate = ProcessStageRecoveryGate(candidateMillis)
    processStageRecoveryGate.compareAndSet(null, candidate)
    return checkNotNull(processStageRecoveryGate.get()).cutoffMillis
}

/** Run only once per process so an Activity recreation cannot race a live export. */
internal fun recoverPendingMediaStoreExportsOnce(
    context: Context,
    priorProcessCutoffMillis: Long,
): ExportRecoveryReport? {
    val cutoff = registerExportProcessStageCutoff(priorProcessCutoffMillis)
    val stageRecovery = checkNotNull(processStageRecoveryGate.get()).runOnce {
        val abandoned = recoverAbandonedExportStages(context.cacheDir, cutoff)
        val legacyAbandoned = recoverAbandonedLegacyExportStages(context, cutoff)
        AbandonedExportStageRecoveryReport(
            examined = abandoned.examined + legacyAbandoned.examined,
            removed = abandoned.removed + legacyAbandoned.removed,
            retained = abandoned.retained + legacyAbandoned.retained,
        )
    }
    if (exportProviderRecoveryCompleted.get()) {
        return stageRecovery?.let {
            ExportRecoveryReport(0, 0, 0, 0, it.removed, it.retained)
        }
    }
    if (!exportProviderRecoveryInFlight.compareAndSet(false, true)) {
        return stageRecovery?.let {
            ExportRecoveryReport(0, 0, 0, 0, it.removed, it.retained)
        }
    }
    return try {
        recoverPendingMediaStoreExports(context).also {
            exportProviderRecoveryCompleted.set(true)
        }.copy(
            removedAbandonedStages = stageRecovery?.removed ?: 0,
            retainedAbandonedStages = stageRecovery?.retained ?: 0,
        )
    } finally {
        exportProviderRecoveryInFlight.set(false)
    }
}

/**
 * Delete only app-private export stages created before this process's first Activity.
 * The cutoff prevents asynchronous startup recovery from touching a live current-process stage.
 */
internal fun recoverAbandonedExportStages(
    cacheDir: File,
    priorProcessCutoffMillis: Long,
): AbandonedExportStageRecoveryReport {
    require(priorProcessCutoffMillis > 0L)
    var examined = 0
    var removed = 0
    var retained = 0
    cacheDir.listFiles()?.forEach { candidate ->
        if (
            candidate.isFile &&
            candidate.name.startsWith(EXPORT_STAGE_PREFIX) &&
            candidate.name.endsWith(EXPORT_STAGE_SUFFIX) &&
            candidate.lastModified() < priorProcessCutoffMillis
        ) {
            examined++
            if (candidate.delete()) removed++ else retained++
        }
    }
    return AbandonedExportStageRecoveryReport(examined, removed, retained)
}

@RequiresApi(Build.VERSION_CODES.Q)
internal fun publishStagedImage(
    context: Context,
    artifact: EncodedArtifact,
    displayName: String,
    mimeType: String,
    isCancelled: () -> Boolean = { false },
    onPublished: (Uri) -> Unit = {},
): Uri {
    check(Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
    val token = synchronized(mediaStoreExportProcessLock) {
        val transaction = mediaStoreExportTransaction(context)
        val recovery = transaction.recover()
        if (recovery.retainedForRetry > 0) {
            throw ExportReconciliationPendingException(
                "a previous export still requires provider reconciliation",
            )
        }
        transaction.commit(
            artifact,
            ExportDestinationSpec(
                displayName = displayName,
                mimeType = mimeType,
                relativePath = MEDIASTORE_EXPORT_RELATIVE_PATH,
            ),
            isCancelled = isCancelled,
            onPublished = { token -> onPublished(Uri.parse(token)) },
        )
    }
    return Uri.parse(token)
}

private data class DigestAndLength(val length: Long, val sha256: String)

private fun digest(
    file: File,
    isCancelled: () -> Boolean = { false },
): DigestAndLength = FileInputStream(file).use { digest(it, isCancelled) }

private fun digest(
    input: InputStream,
    isCancelled: () -> Boolean = { false },
): DigestAndLength {
    val md = MessageDigest.getInstance("SHA-256")
    var count = 0L
    val buffer = ByteArray(COPY_BUFFER_BYTES)
    while (true) {
        throwIfExportCancelled(isCancelled)
        val read = input.read(buffer)
        if (read < 0) break
        md.update(buffer, 0, read)
        count += read
        throwIfExportCancelled(isCancelled)
    }
    return DigestAndLength(count, md.digest().toHex())
}

private fun throwIfExportCancelled(isCancelled: () -> Boolean) {
    if (isCancelled()) throw CancellationException("export publication cancelled")
}

private fun ByteArray.toHex(): String = joinToString("") { "%02x".format(it) }

private const val COPY_BUFFER_BYTES = 256 * 1024
private const val EXPORT_STAGE_PREFIX = "spectrafilm-export-"
private const val EXPORT_STAGE_SUFFIX = ".part"
private const val MEDIASTORE_EXPORT_RELATIVE_PATH = "Pictures/Spektrafilm"
private const val MEDIASTORE_PENDING_NAME_PREFIX = "Spektrafilm-pending-tx-"
private val mediaStoreExportProcessLock = Any()
