/*
 * Spektrafilm for Android — source URI grant/restoration contract tests. GPLv3.
 * Film modeling powered by spektrafilm.
 */
package com.spectrafilm.app

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class SourceAccessTest {

    @Test
    fun acquire_contentUri_persistsGrantAndRestoresDurably() {
        val uri = "content://photos/42"
        val backend = FakeUriGrantBackend(
            takeSucceeds = true,
            readableUris = mutableSetOf(uri),
        )
        val store = FakeSourceRefStore()
        val coordinator = SourceAccessCoordinator(backend, store)

        val acquired = coordinator.acquire(uri, "PHOTO", "portrait.jpg")

        val expected = PersistedSourceRef(
            uri = uri,
            kind = "PHOTO",
            displayName = "portrait.jpg",
            accessMode = SourceAccessMode.PERSISTED,
        )
        assertEquals(expected, acquired)
        assertEquals(expected, store.stored)
        val reopenedStore = FakeSourceRefStore(store.stored)
        assertEquals(
            SourceRestoreResult.Ready(expected),
            SourceAccessCoordinator(backend, reopenedStore).restore(),
        )
    }

    @Test
    fun acquire_whenPersistableGrantFails_marksTransientAndRequiresAuthorizationAfterRestart() {
        val uri = "content://cloud/raw/7"
        val backend = FakeUriGrantBackend(
            takeSucceeds = false,
            readableUris = mutableSetOf(uri),
        )
        val store = FakeSourceRefStore()

        val acquired = SourceAccessCoordinator(backend, store)
            .acquire(uri, "RAW", "frame.dng")

        val expected = PersistedSourceRef(
            uri = uri,
            kind = "RAW",
            displayName = "frame.dng",
            accessMode = SourceAccessMode.TRANSIENT,
        )
        assertEquals(expected, acquired)
        assertEquals(expected, store.stored)
        val restartedBackend = FakeUriGrantBackend(readableUris = mutableSetOf())
        assertEquals(
            SourceRestoreResult.NeedsAuthorization(expected),
            SourceAccessCoordinator(restartedBackend, store).restore(),
        )
    }

    @Test
    fun restore_transientGrantStillReadableAfterActivityRecreation_isReady() {
        val uri = "content://cloud/live/8"
        val ref = PersistedSourceRef(uri, "PHOTO", "live.jpg", SourceAccessMode.TRANSIENT)
        val backend = FakeUriGrantBackend(readableUris = mutableSetOf(uri))

        assertEquals(
            SourceRestoreResult.Ready(ref),
            SourceAccessCoordinator(backend, FakeSourceRefStore(ref)).restore(),
        )
    }

    @Test
    fun restore_whenPersistedGrantWasRevoked_preservesMetadataForReauthorization() {
        val ref = PersistedSourceRef(
            uri = "content://photos/revoked",
            kind = "PHOTO",
            displayName = "family.heic",
            accessMode = SourceAccessMode.PERSISTED,
        )
        val store = FakeSourceRefStore(ref)
        val backend = FakeUriGrantBackend(
            persistedUris = mutableSetOf(),
            readableUris = mutableSetOf(),
        )

        val restored = SourceAccessCoordinator(backend, store).restore()

        assertEquals(SourceRestoreResult.NeedsAuthorization(ref), restored)
        assertEquals(ref, store.stored)
    }

    @Test
    fun restore_whenPersistedSourceIsUnreadable_requiresAuthorization() {
        val uri = "content://documents/moved"
        val ref = PersistedSourceRef(
            uri = uri,
            kind = "RAW",
            displayName = "moved.nef",
            accessMode = SourceAccessMode.PERSISTED,
        )
        val store = FakeSourceRefStore(ref)
        val backend = FakeUriGrantBackend(
            persistedUris = mutableSetOf(uri),
            readableUris = mutableSetOf(),
        )

        assertEquals(
            SourceRestoreResult.NeedsAuthorization(ref),
            SourceAccessCoordinator(backend, store).restore(),
        )
    }

    @Test
    fun acquire_fileUri_isRejectedWithoutStoreMutation() {
        assertRejectedWithoutStoreMutation("file:///sdcard/DCIM/private.jpg")
    }

    @Test
    fun acquire_httpUri_isRejectedWithoutStoreMutation() {
        assertRejectedWithoutStoreMutation("https://example.test/photo.jpg")
    }

    @Test
    fun acquire_blankUri_isRejectedWithoutStoreMutation() {
        assertRejectedWithoutStoreMutation("   ")
    }

    @Test
    fun acquire_unreadableContentUri_isRejectedBeforeGrantOrStoreMutation() {
        val uri = "content://photos/unreadable"
        val backend = FakeUriGrantBackend(readableUris = mutableSetOf())
        val store = FakeSourceRefStore()

        assertThrows(IllegalStateException::class.java) {
            SourceAccessCoordinator(backend, store).acquire(uri, "PHOTO", "missing.jpg")
        }

        assertEquals(1, backend.calls) // readability probe only
        assertEquals(0, backend.takeCalls)
        assertNull(store.stored)
    }

    @Test
    fun acquire_storeFailure_releasesNewPersistedGrant() {
        val uri = "content://photos/store-failure"
        val backend = FakeUriGrantBackend(readableUris = mutableSetOf(uri))
        val store = FakeSourceRefStore(failSave = true)

        assertThrows(IllegalStateException::class.java) {
            SourceAccessCoordinator(backend, store).acquire(uri, "PHOTO", "safe.jpg")
        }

        assertEquals(1, backend.releaseCalls)
        assertNull(store.stored)
    }

    @Test
    fun acquire_storeFailure_doesNotReleaseGrantThatPreExistedAttempt() {
        val uri = "content://photos/already-held"
        val backend = FakeUriGrantBackend(
            persistedUris = mutableSetOf(uri),
            readableUris = mutableSetOf(uri),
        )
        val store = FakeSourceRefStore(failSave = true)

        assertThrows(IllegalStateException::class.java) {
            SourceAccessCoordinator(backend, store).acquire(uri, "PHOTO", "held.jpg")
        }

        assertEquals(0, backend.takeCalls)
        assertEquals(0, backend.releaseCalls)
    }

    @Test
    fun clear_removesStoredSourceAndSubsequentRestoreIsNone() {
        val uri = "content://photos/clear-me"
        val backend = FakeUriGrantBackend(
            takeSucceeds = true,
            readableUris = mutableSetOf(uri),
        )
        val store = FakeSourceRefStore()
        val coordinator = SourceAccessCoordinator(backend, store)
        coordinator.acquire(uri, "PHOTO", "clear-me.jpg")

        coordinator.clear()

        assertNull(store.stored)
        assertEquals(1, store.clearCalls)
        assertEquals(1, backend.releaseCalls)
        assertEquals(SourceRestoreResult.None, coordinator.restore())
    }

    @Test
    fun selectDemo_killAfterTombstoneBeforeSessionCommit_neverRestoresOldUri() {
        val uri = "content://photos/old-before-demo"
        val old = PersistedSourceRef(uri, "PHOTO", "old.jpg", SourceAccessMode.PERSISTED)
        val backend = FakeUriGrantBackend(
            persistedUris = mutableSetOf(uri),
            readableUris = mutableSetOf(uri),
        )
        val store = FakeSourceRefStore(old)

        // Crash point: the source transaction committed, while the editor-session file can still
        // contain `old`. A brand-new coordinator models process death before session checkpoint.
        SourceAccessCoordinator(backend, store).selectDemo()
        val afterKill = SourceAccessCoordinator(backend, FakeSourceRefStore(store.stored)).restore()

        assertEquals(SourceRestoreResult.Demo, afterKill)
        assertEquals(SourceAccessCoordinator.DEMO_TOMBSTONE, store.stored)
        assertTrue(backend.persistedReads().isEmpty())
        val reconciled = reconcileEditorRestoration(
            session = richSessionForSource(old),
            sourceRestore = afterKill,
            savedFallback = EditorSavedFallback.Empty,
        )
        assertEquals(SourceKind.DEMO, reconciled.source.kind)
        assertNull(reconciled.source.uri)
        assertNull(reconciled.document)
    }

    @Test
    fun acquire_replacingPersistedSource_releasesPreviousGrantAfterDurableSave() {
        val oldUri = "content://photos/old"
        val newUri = "content://photos/new"
        val oldRef = PersistedSourceRef(oldUri, "PHOTO", "old.jpg", SourceAccessMode.PERSISTED)
        val backend = FakeUriGrantBackend(
            persistedUris = mutableSetOf(oldUri),
            readableUris = mutableSetOf(oldUri, newUri),
        )
        val store = FakeSourceRefStore(oldRef)

        val acquired = SourceAccessCoordinator(backend, store)
            .acquire(newUri, "PHOTO", "new.jpg")

        assertEquals(newUri, acquired.uri)
        assertEquals(newUri, store.stored?.uri)
        assertEquals(listOf(oldUri), backend.releasedUris)
    }

    @Test
    fun restore_reconcilesBothPersistableGrantCrashWindows() {
        val oldUri = "content://photos/old-crash"
        val currentUri = "content://photos/current"
        val uncommittedUri = "content://photos/taken-before-save"
        val current = PersistedSourceRef(
            currentUri,
            "PHOTO",
            "current.jpg",
            SourceAccessMode.PERSISTED,
        )
        val backend = FakeUriGrantBackend(
            persistedUris = mutableSetOf(oldUri, currentUri, uncommittedUri),
            readableUris = mutableSetOf(currentUri),
        )
        val store = FakeSourceRefStore(current)

        assertEquals(SourceRestoreResult.Ready(current), SourceAccessCoordinator(backend, store).restore())
        assertEquals(setOf(oldUri, uncommittedUri), backend.releasedUris.toSet())
        assertEquals(setOf(currentUri), backend.persistedReads())
    }

    @Test
    fun latestMutationGate_skipsQueuedStaleSelection() {
        val gate = LatestSourceMutationGate()
        val stale = gate.begin()
        val current = gate.begin()

        assertNull(gate.runIfCurrent(stale) { "stale" })
        assertEquals("current", gate.runIfCurrent(current) { "current" })
    }

    @Test
    fun latestMutationGate_serializesOverlappingAcquireThenClear() {
        val gate = LatestSourceMutationGate()
        val acquire = gate.begin()
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val events = Collections.synchronizedList(mutableListOf<String>())
        val acquireThread = Thread {
            gate.runIfCurrent(acquire) {
                entered.countDown()
                assertTrue(release.await(5, TimeUnit.SECONDS))
                events += "acquire"
            }
        }
        acquireThread.start()
        assertTrue(entered.await(5, TimeUnit.SECONDS))

        val clear = gate.begin()
        val clearThread = Thread {
            gate.runIfCurrent(clear) { events += "clear" }
        }
        clearThread.start()
        release.countDown()
        acquireThread.join(5_000)
        clearThread.join(5_000)

        assertEquals(listOf("acquire", "clear"), events)
    }

    @Test
    fun processOwnedMutation_survivesCanceledActivityWaiter_andRemainsDurable() = runBlocking {
        val staleSavedUri = "content://photos/stale-saved-activity-source"
        val uri = "content://photos/recreated-activity"
        val staleSavedRef = PersistedSourceRef(
            staleSavedUri,
            "PHOTO",
            "stale-a.jpg",
            SourceAccessMode.PERSISTED,
        )
        val backend = FakeUriGrantBackend(
            persistedUris = mutableSetOf(staleSavedUri),
            readableUris = mutableSetOf(staleSavedUri, uri),
        )
        val store = FakeSourceRefStore(staleSavedRef)
        val coordinator = SourceAccessCoordinator(backend, store)
        val gate = LatestSourceMutationGate()
        val runtime = SourceAccessRuntime(
            coordinator = coordinator,
            mutations = gate,
            workScope = CoroutineScope(SupervisorJob() + Dispatchers.Default),
        )
        val mutationEntered = CountDownLatch(1)
        val releaseMutation = CountDownLatch(1)
        val token = gate.begin()
        val durableMutation = runtime.submit(token) {
            mutationEntered.countDown()
            assertTrue(releaseMutation.await(5, TimeUnit.SECONDS))
            coordinator.acquire(uri, "PHOTO", "rotation-safe.jpg")
        }
        assertTrue(mutationEntered.await(5, TimeUnit.SECONDS))

        // Models an Activity waiter being canceled by recreation. The independently owned
        // mutation must still finish and be visible to the replacement Activity's restore.
        val canceledActivityWaiter = launch { durableMutation.await() }
        canceledActivityWaiter.cancelAndJoin()
        releaseMutation.countDown()

        val acquired = withTimeout(5_000) { durableMutation.await() }
        assertEquals(uri, acquired?.uri)
        assertEquals(uri, store.stored?.uri)
        assertEquals(listOf(staleSavedUri), backend.releasedUris)
        assertEquals(
            SourceRestoreResult.Ready(acquired!!),
            SourceAccessCoordinator(backend, store).restore(),
        )
    }

    @Test
    fun processQueue_acquireThenDemoThenRecreatedRestore_preservesDemoAsFinalState() = runBlocking {
        val uri = "content://photos/queue-race"
        val backend = FakeUriGrantBackend(readableUris = mutableSetOf(uri))
        val store = FakeSourceRefStore()
        val coordinator = SourceAccessCoordinator(backend, store)
        val gate = LatestSourceMutationGate()
        val runtime = SourceAccessRuntime(
            coordinator = coordinator,
            mutations = gate,
            workScope = CoroutineScope(SupervisorJob() + Dispatchers.Default),
        )
        val acquireEntered = CountDownLatch(1)
        val releaseAcquire = CountDownLatch(1)

        val acquireToken = gate.begin()
        val acquire = runtime.submit(acquireToken) {
            acquireEntered.countDown()
            assertTrue(releaseAcquire.await(5, TimeUnit.SECONDS))
            coordinator.acquire(uri, "PHOTO", "race.jpg")
        }
        assertTrue(acquireEntered.await(5, TimeUnit.SECONDS))

        // The user chooses demo while acquire is in flight, then Activity recreation queues
        // restore. FIFO requires the demo tombstone to commit before restore, even though both
        // share the latest
        // generation token and coordinator locking alone provides no fairness guarantee.
        val demoToken = gate.begin()
        val demo = runtime.submit(demoToken) { coordinator.selectDemo() }
        val recreatedRestore = runtime.submit(demoToken) { coordinator.restore() }
        releaseAcquire.countDown()

        withTimeout(5_000) { acquire.await() }
        withTimeout(5_000) { demo.await() }
        assertEquals(SourceRestoreResult.Demo, withTimeout(5_000) { recreatedRestore.await() })
        assertEquals(SourceAccessCoordinator.DEMO_TOMBSTONE, store.stored)
        assertTrue(backend.persistedReads().isEmpty())
    }

    @Test
    fun latestAcquireFailure_reportsPreviouslyCommittedDurableSourceForUiReconciliation() = runBlocking {
        val firstUri = "content://photos/first-fast-selection"
        val rejectedUri = "content://photos/latest-unreadable-selection"
        val backend = FakeUriGrantBackend(readableUris = mutableSetOf(firstUri))
        val store = FakeSourceRefStore()
        val coordinator = SourceAccessCoordinator(backend, store)
        val gate = LatestSourceMutationGate()
        val runtime = SourceAccessRuntime(coordinator, gate)

        val firstToken = gate.begin()
        val first = runtime.submitReconciled(firstToken) {
            coordinator.acquire(firstUri, "PHOTO", "first.jpg")
        }
        assertTrue(withTimeout(5_000) { first.await() } is ReconciledSourceMutation.Applied)

        val latestToken = gate.begin()
        val latest = runtime.submitReconciled(latestToken) {
            coordinator.acquire(rejectedUri, "PHOTO", "unreadable.jpg")
        }
        val rejected = withTimeout(5_000) { latest.await() }
            as ReconciledSourceMutation.Rejected

        assertEquals(SourceRestoreResult.Ready(store.stored!!), rejected.durableState)
        assertEquals(firstUri, store.stored?.uri)
    }

    @Test
    fun failedLegacyClear_reportsStillDurableSourceForUiReconciliation() = runBlocking {
        val uri = "content://photos/clear-commit-failed"
        val ref = PersistedSourceRef(uri, "PHOTO", "kept.jpg", SourceAccessMode.PERSISTED)
        val backend = FakeUriGrantBackend(
            persistedUris = mutableSetOf(uri),
            readableUris = mutableSetOf(uri),
        )
        val store = FakeSourceRefStore(initial = ref, failClear = true)
        val coordinator = SourceAccessCoordinator(backend, store)
        val gate = LatestSourceMutationGate()
        val runtime = SourceAccessRuntime(coordinator, gate)

        val token = gate.begin()
        val outcome = withTimeout(5_000) {
            runtime.submitReconciled(token) { coordinator.clear() }.await()
        } as ReconciledSourceMutation.Rejected

        assertEquals(SourceRestoreResult.Ready(ref), outcome.durableState)
        assertEquals(ref, store.stored)
    }

    private fun assertRejectedWithoutStoreMutation(uri: String) {
        val backend = FakeUriGrantBackend()
        val store = FakeSourceRefStore()
        val coordinator = SourceAccessCoordinator(backend, store)

        assertThrows(IllegalArgumentException::class.java) {
            coordinator.acquire(uri, "PHOTO", "untrusted")
        }
        assertNull(store.stored)
        assertEquals(0, store.saveCalls)
        assertEquals(0, store.clearCalls)
        assertEquals(0, backend.calls)
    }

    private fun richSessionForSource(ref: PersistedSourceRef): EditorSessionDocument {
        val snapshot = EditSnapshot("{}", 0)
        return EditorSessionDocument(
            source = EditorSourceState(
                uri = ref.uri,
                kind = SourceKind.valueOf(ref.kind),
                displayName = ref.displayName,
                authorizationRequired = false,
            ),
            current = snapshot,
            committed = snapshot,
            history = EditHistoryState(emptyList(), emptyList()),
            tool = EditorToolState(null, EditorOverlayTool.NONE, 0),
            preset = EditorPresetState(null, null, 1f, null, "", ""),
            export = EditorExportState(
                false,
                ExportOptions(ExportFormat.JPEG, 90, ExportSize.FULL, 2048, ""),
                false,
                EditorExportPhase.IDLE,
                null,
            ),
        )
    }

    private class FakeUriGrantBackend(
        private val takeSucceeds: Boolean = true,
        private val persistedUris: MutableSet<String> = mutableSetOf(),
        private val readableUris: MutableSet<String> = mutableSetOf(),
    ) : UriGrantBackend {
        var calls: Int = 0
            private set
        var takeCalls: Int = 0
            private set
        var releaseCalls: Int = 0
            private set
        val releasedUris = mutableListOf<String>()

        override fun takePersistableRead(uri: String): Boolean {
            calls++
            takeCalls++
            if (takeSucceeds) persistedUris += uri
            return takeSucceeds
        }

        override fun releasePersistableRead(uri: String) {
            calls++
            releaseCalls++
            releasedUris += uri
            persistedUris -= uri
        }

        override fun hasPersistedRead(uri: String): Boolean {
            calls++
            return uri in persistedUris
        }

        override fun persistedReads(): Set<String> {
            calls++
            return persistedUris.toSet()
        }

        override fun canRead(uri: String): Boolean {
            calls++
            return uri in readableUris
        }
    }

    private class FakeSourceRefStore(
        initial: PersistedSourceRef? = null,
        private val failSave: Boolean = false,
        private val failClear: Boolean = false,
    ) : SourceRefStore {
        var stored: PersistedSourceRef? = initial
            private set
        var saveCalls: Int = 0
            private set
        var clearCalls: Int = 0
            private set

        override fun load(): PersistedSourceRef? = stored

        override fun save(ref: PersistedSourceRef) {
            if (failSave) throw IllegalStateException("simulated durable-store failure")
            stored = ref
            saveCalls++
        }

        override fun clear() {
            if (failClear) throw IllegalStateException("simulated durable-clear failure")
            stored = null
            clearCalls++
        }
    }
}
