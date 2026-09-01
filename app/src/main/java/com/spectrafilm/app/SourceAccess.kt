/*
 * Spektrafilm for Android — durable, least-privilege source URI access. GPLv3.
 */
package com.spectrafilm.app

import android.content.Context
import android.content.Intent
import android.net.Uri
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch
import java.net.URI
import java.util.concurrent.atomic.AtomicLong

internal enum class SourceAccessMode { PERSISTED, TRANSIENT }

internal data class PersistedSourceRef(
    val uri: String,
    val kind: String,
    val displayName: String,
    val accessMode: SourceAccessMode,
)

internal sealed interface SourceRestoreResult {
    data object None : SourceRestoreResult
    /** Explicit durable selection; unlike an absent record it cannot resurrect an older URI. */
    data object Demo : SourceRestoreResult
    data class Ready(val ref: PersistedSourceRef) : SourceRestoreResult
    data class NeedsAuthorization(val ref: PersistedSourceRef) : SourceRestoreResult
    data class Invalid(val reason: String) : SourceRestoreResult
}

internal sealed interface ReconciledSourceMutation<out T> {
    data class Applied<T>(val value: T) : ReconciledSourceMutation<T>
    data class Rejected(
        val failure: Throwable,
        val durableState: SourceRestoreResult?,
    ) : ReconciledSourceMutation<Nothing>
}

internal interface UriGrantBackend {
    fun takePersistableRead(uri: String): Boolean
    fun releasePersistableRead(uri: String)
    fun hasPersistedRead(uri: String): Boolean
    fun persistedReads(): Set<String>
    fun canRead(uri: String): Boolean
}

internal interface SourceRefStore {
    fun load(): PersistedSourceRef?
    fun save(ref: PersistedSourceRef)
    fun clear()
}

/**
 * Linearizes grant/store mutations while letting a newer UI intent invalidate work that has
 * not started. A mutation already in its critical section finishes first; the newest queued
 * mutation then deterministically establishes the final durable state.
 */
internal class LatestSourceMutationGate {
    private val generation = AtomicLong(0L)
    private val lock = Any()

    fun begin(): Long = generation.incrementAndGet()

    fun snapshot(): Long = generation.get()

    fun isCurrent(token: Long): Boolean = token == generation.get()

    fun <T> runIfCurrent(token: Long, mutation: () -> T): T? = synchronized(lock) {
        if (!isCurrent(token)) null else mutation()
    }
}

/** Pure policy/controller; Android grant and persistence APIs sit behind ports for JVM tests. */
internal class SourceAccessCoordinator(
    private val backend: UriGrantBackend,
    private val store: SourceRefStore,
) {
    private val mutationLock = Any()

    fun acquire(uri: String, kind: String, displayName: String): PersistedSourceRef =
        synchronized(mutationLock) {
        validateUri(uri)
        require(kind == "PHOTO" || kind == "RAW") { "unsupported source kind" }
        require(displayName.isNotBlank() && displayName.length <= MAX_DISPLAY_NAME_CHARS) {
            "invalid source display name"
        }
        check(backend.canRead(uri)) { "selected source is not readable" }
        val previous = store.load()

        val persistedBeforeAttempt = backend.hasPersistedRead(uri)
        val persisted = persistedBeforeAttempt ||
            (backend.takePersistableRead(uri) && backend.hasPersistedRead(uri))
        val newlyPersisted = persisted && !persistedBeforeAttempt
        val ref = PersistedSourceRef(
            uri = uri,
            kind = kind,
            displayName = displayName,
            accessMode = if (persisted) SourceAccessMode.PERSISTED else SourceAccessMode.TRANSIENT,
        )
        try {
            store.save(ref)
        } catch (failure: Throwable) {
            if (newlyPersisted) {
                runCatching { backend.releasePersistableRead(uri) }
                    .exceptionOrNull()
                    ?.let(failure::addSuppressed)
            }
            throw failure
        }
        if (
            previous?.accessMode == SourceAccessMode.PERSISTED &&
            previous.uri != ref.uri
        ) {
            // The new reference is already durable. A provider can still reject release
            // if it independently revoked the old grant, which is safe to treat as gone.
            runCatching { backend.releasePersistableRead(previous.uri) }
        }
        ref
    }

    fun restore(): SourceRestoreResult = synchronized(mutationLock) {
        val ref = store.load()
        if (ref == null) {
            runCatching { reconcilePersistedGrantsLocked(expectedUri = null) }
            return@synchronized SourceRestoreResult.None
        }
        if (ref == DEMO_TOMBSTONE) {
            runCatching { reconcilePersistedGrantsLocked(expectedUri = null) }
            return@synchronized SourceRestoreResult.Demo
        }
        try {
            validateUri(ref.uri)
            require(ref.kind == "PHOTO" || ref.kind == "RAW") { "unsupported stored source kind" }
            require(ref.displayName.isNotBlank() && ref.displayName.length <= MAX_DISPLAY_NAME_CHARS) {
                "invalid stored source display name"
            }
        } catch (failure: IllegalArgumentException) {
            runCatching { reconcilePersistedGrantsLocked(expectedUri = null) }
            return@synchronized SourceRestoreResult.Invalid(failure.message ?: "invalid stored source")
        }
        try {
            reconcilePersistedGrantsLocked(
                expectedUri = ref.uri.takeIf { ref.accessMode == SourceAccessMode.PERSISTED },
            )
            val readable = when (ref.accessMode) {
                SourceAccessMode.PERSISTED ->
                    backend.hasPersistedRead(ref.uri) && backend.canRead(ref.uri)
                // A non-persistable picker grant can remain valid across an Activity
                // recreation in the same process. Use it while it is actually readable;
                // a cold start after revocation still falls into reauthorization.
                SourceAccessMode.TRANSIENT -> backend.canRead(ref.uri)
            }
            if (readable) {
                SourceRestoreResult.Ready(ref)
            } else {
                SourceRestoreResult.NeedsAuthorization(ref)
            }
        } catch (_: Exception) {
            // Provider/runtime failures are not proof that the durable record is invalid.
            SourceRestoreResult.NeedsAuthorization(ref)
        }
    }

    fun clear() = synchronized(mutationLock) {
        val previous = store.load()
        store.clear()
        if (previous?.accessMode == SourceAccessMode.PERSISTED) {
            runCatching { backend.releasePersistableRead(previous.uri) }
        }
        runCatching { reconcilePersistedGrantsLocked(expectedUri = null) }
        Unit
    }

    /**
     * Commits the demo selection before releasing the old grant. The tombstone and the old editor
     * checkpoint may temporarily disagree after a process kill; restoration treats the tombstone
     * as authoritative and therefore never resurrects the URI.
     */
    fun selectDemo() = synchronized(mutationLock) {
        val previous = store.load()
        store.save(DEMO_TOMBSTONE)
        if (previous?.accessMode == SourceAccessMode.PERSISTED && previous != DEMO_TOMBSTONE) {
            runCatching { backend.releasePersistableRead(previous.uri) }
        }
        runCatching { reconcilePersistedGrantsLocked(expectedUri = null) }
        Unit
    }

    /** Releases grants left by either crash window around source-reference replacement. */
    fun reconcilePersistedGrants(): Int = synchronized(mutationLock) {
        val expected = store.load()
            ?.takeIf { it.accessMode == SourceAccessMode.PERSISTED }
            ?.uri
        reconcilePersistedGrantsLocked(expected)
    }

    private fun reconcilePersistedGrantsLocked(expectedUri: String?): Int {
        var released = 0
        for (uri in backend.persistedReads()) {
            if (uri == expectedUri) continue
            runCatching { backend.releasePersistableRead(uri) }
                .onSuccess { released++ }
        }
        return released
    }

    companion object {
        private const val MAX_DISPLAY_NAME_CHARS = 512
        internal val DEMO_TOMBSTONE = PersistedSourceRef(
            uri = "demo://selected",
            kind = "DEMO",
            displayName = "synthetic demo image",
            accessMode = SourceAccessMode.TRANSIENT,
        )

        private fun validateUri(raw: String) {
            require(raw.isNotBlank() && raw == raw.trim()) { "source URI is blank or padded" }
            val parsed = runCatching { URI(raw) }
                .getOrElse { throw IllegalArgumentException("malformed source URI", it) }
            require(parsed.scheme.equals("content", ignoreCase = true)) {
                "only content:// source URIs are accepted"
            }
            require(!parsed.rawAuthority.isNullOrBlank()) { "content URI has no authority" }
        }
    }
}

internal class AndroidUriGrantBackend(context: Context) : UriGrantBackend {
    private val resolver = context.applicationContext.contentResolver

    override fun takePersistableRead(uri: String): Boolean = runCatching {
        resolver.takePersistableUriPermission(
            Uri.parse(uri),
            Intent.FLAG_GRANT_READ_URI_PERMISSION,
        )
        hasPersistedRead(uri)
    }.getOrDefault(false)

    override fun releasePersistableRead(uri: String) {
        resolver.releasePersistableUriPermission(
            Uri.parse(uri),
            Intent.FLAG_GRANT_READ_URI_PERMISSION,
        )
    }

    override fun hasPersistedRead(uri: String): Boolean = resolver.persistedUriPermissions.any {
        it.uri.toString() == uri && it.isReadPermission
    }

    override fun persistedReads(): Set<String> = resolver.persistedUriPermissions
        .asSequence()
        .filter { it.isReadPermission }
        .map { it.uri.toString() }
        .toSet()

    override fun canRead(uri: String): Boolean = runCatching {
        resolver.openFileDescriptor(Uri.parse(uri), "r")?.use { true } ?: false
    }.getOrDefault(false)
}

internal class SharedPreferencesSourceRefStore(context: Context) : SourceRefStore {
    private val prefs = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    override fun load(): PersistedSourceRef? {
        if (prefs.getInt(KEY_VERSION, 0) != VERSION) return null
        val uri = prefs.getString(KEY_URI, null) ?: return null
        val kind = prefs.getString(KEY_KIND, null) ?: return null
        val displayName = prefs.getString(KEY_NAME, null) ?: return null
        val mode = runCatching {
            SourceAccessMode.valueOf(prefs.getString(KEY_MODE, null) ?: return null)
        }.getOrNull() ?: return null
        return PersistedSourceRef(uri, kind, displayName, mode)
    }

    override fun save(ref: PersistedSourceRef) {
        check(
            prefs.edit()
                .putInt(KEY_VERSION, VERSION)
                .putString(KEY_URI, ref.uri)
                .putString(KEY_KIND, ref.kind)
                .putString(KEY_NAME, ref.displayName)
                .putString(KEY_MODE, ref.accessMode.name)
                .commit()
        ) { "could not persist source access state" }
    }

    override fun clear() {
        check(prefs.edit().clear().commit()) { "could not clear source access state" }
    }

    private companion object {
        const val PREFS = "source_access_v1"
        const val VERSION = 1
        const val KEY_VERSION = "schema_version"
        const val KEY_URI = "uri"
        const val KEY_KIND = "kind"
        const val KEY_NAME = "display_name"
        const val KEY_MODE = "access_mode"
    }
}

internal class SourceAccessRuntime internal constructor(
    val coordinator: SourceAccessCoordinator,
    val mutations: LatestSourceMutationGate,
    private val workScope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.IO),
) {
    private val queue = Channel<() -> Unit>(Channel.UNLIMITED)

    init {
        workScope.launch {
            for (mutation in queue) mutation()
        }
    }

    /**
     * Enqueues a durable mutation in strict FIFO order. Awaiting this [Deferred] from an Activity
     * never makes the mutation its child, so configuration-change cancellation cannot drop it.
     * FIFO also ensures a recreated Activity's restore cannot overtake a previously queued clear.
     */
    fun <T> submit(token: Long, mutation: () -> T): Deferred<T?> {
        val result = CompletableDeferred<T?>()
        val sent = queue.trySend {
            try {
                result.complete(mutations.runIfCurrent(token, mutation))
            } catch (failure: Throwable) {
                result.completeExceptionally(failure)
            }
        }
        if (sent.isFailure) {
            result.completeExceptionally(
                IllegalStateException("source mutation queue is unavailable", sent.exceptionOrNull()),
            )
        }
        return result
    }

    /** A failed latest intent reports the state that is actually durable, for UI reconciliation. */
    fun <T> submitReconciled(
        token: Long,
        mutation: () -> T,
    ): Deferred<ReconciledSourceMutation<T>?> = submit(token) {
        try {
            ReconciledSourceMutation.Applied(mutation())
        } catch (failure: Throwable) {
            ReconciledSourceMutation.Rejected(
                failure = failure,
                durableState = runCatching { coordinator.restore() }.getOrNull(),
            )
        }
    }
}

/** One process-wide ordering boundary survives Activity recreation and canceled UI scopes. */
internal object SourceAccessRuntimeHolder {
    @Volatile
    private var runtime: SourceAccessRuntime? = null

    fun get(context: Context): SourceAccessRuntime = runtime ?: synchronized(this) {
        runtime ?: SourceAccessRuntime(
            coordinator = SourceAccessCoordinator(
                AndroidUriGrantBackend(context.applicationContext),
                SharedPreferencesSourceRefStore(context.applicationContext),
            ),
            mutations = LatestSourceMutationGate(),
        ).also { runtime = it }
    }
}

internal fun sourceAccessRuntime(context: Context): SourceAccessRuntime =
    SourceAccessRuntimeHolder.get(context)
