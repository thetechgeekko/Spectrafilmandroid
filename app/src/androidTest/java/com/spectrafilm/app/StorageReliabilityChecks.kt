/*
 * Spektrafilm for Android — platform instrumentation for ticket #170 storage contracts. GPLv3.
 * No JUnit/AndroidX-test dependency enters the release-candidate gate APK.
 */
package com.spectrafilm.app

import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import android.provider.MediaStore
import android.util.Base64
import com.spectrafilm.engine.DirCouplersParams
import com.spectrafilm.engine.FilmRenderingParams
import com.spectrafilm.engine.GlareParams
import com.spectrafilm.engine.GrainParams
import com.spectrafilm.engine.HalationParams
import com.spectrafilm.engine.IoParams
import com.spectrafilm.engine.LinearImage
import com.spectrafilm.engine.SpektraParams
import com.spectrafilm.pngwriter.PngCancellationToken
import com.spectrafilm.pngwriter.PngWriter
import com.spectrafilm.tiffwriter.TiffCancellationToken
import com.spectrafilm.tiffwriter.TiffWriter
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Arrays
import java.util.LinkedHashSet
import java.util.concurrent.CancellationException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout

object StorageReliabilityChecks {
    private data class ActivityRecreationExportProbe(
        val runId: Long,
        val release: CompletableDeferred<Unit>,
        val completions: AtomicInteger,
        val engine: AtomicReference<Any?>,
    )

    private val activityRecreationProbeLock = Any()
    private var activityRecreationProbe: ActivityRecreationExportProbe? = null

    @JvmStatic
    fun run(context: Context) {
        nativeWriterBoundarySmoke(context)
        foregroundServicePromotesBeforeRapidCompletionStop(context)
        durableJournalRecoveryWithReopenedAdapters(context)
        outOfSpaceAndInterruptedCloseNeverPublish(context)
        recipeResetInvalidatesInFlightRestore(context)
        processOwnedExportSurvivesObserverRecreationAndCancelsExactlyOnce(context)
        revokedGrantRestoresAsNeedsAuthorization(context)
        realPersistableGrantReleaseRestoresAsNeedsAuthorization(context)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            duplicateNamesCommitDistinctVerifiedMediaStoreRows(context)
        }
    }

    /**
     * Hold the target main looper so tiny exports finish before Android can dispatch the service
     * start. A direct stopService() in that window causes
     * ForegroundServiceDidNotStartInTimeException; queued service-owned STOP commands must first
     * promote, then stop themselves. Remaining alive past the platform watchdog is the assertion.
     */
    private fun foregroundServicePromotesBeforeRapidCompletionStop(context: Context) = runBlocking {
        require(ExportWorkRuntime.state.value is ExportRuntimeState.Idle) {
            "export runtime was not Idle before foreground-service race probe"
        }
        val mainBlocked = CountDownLatch(1)
        val releaseMain = CountDownLatch(1)
        require(Handler(Looper.getMainLooper()).post {
            mainBlocked.countDown()
            releaseMain.await(MAIN_LOOPER_BLOCK_LIMIT_MS, TimeUnit.MILLISECONDS)
        }) { "could not enqueue foreground-service main-looper blocker" }
        require(mainBlocked.await(5, TimeUnit.SECONDS)) {
            "target main looper did not enter foreground-service race probe"
        }

        try {
            repeat(RAPID_FOREGROUND_EXPORT_COUNT) {
                val runId = requireNotNull(
                    withTimeout(5_000) {
                        var accepted: Long?
                        do {
                            accepted = ExportWorkRuntime.launch(
                                context = context,
                                format = ExportFormat.PNG16,
                                startedAtMillis = System.currentTimeMillis(),
                            ) { successfulRuntimeProbeOutcome() }
                            if (accepted == null) SystemClock.sleep(1)
                        } while (accepted == null)
                        accepted
                    },
                )
                val finished = withTimeout(5_000) {
                    ExportWorkRuntime.state.filter {
                        it is ExportRuntimeState.Finished && it.runId == runId
                    }.first() as ExportRuntimeState.Finished
                }
                require(finished.outcome is ExportTerminalOutcome.Success) {
                    "rapid foreground export did not complete successfully"
                }
                requireNotNull(ExportWorkRuntime.claimFinished(runId)) {
                    "rapid foreground export terminal result was not claimable"
                }
            }
        } finally {
            releaseMain.countDown()
        }

        // Android raises a missed-promotion failure asynchronously. Keep this
        // exact target process alive long enough that it cannot become a false pass.
        SystemClock.sleep(FOREGROUND_SERVICE_WATCHDOG_SETTLE_MS)
        require(ExportWorkRuntime.state.value is ExportRuntimeState.Idle) {
            "foreground-service race probe left export runtime non-Idle"
        }
    }

    private fun nativeWriterBoundarySmoke(context: Context) {
        val pixels = ByteBuffer.allocateDirect(8).order(ByteOrder.LITTLE_ENDIAN).apply {
            putShort(2, 0x1234.toShort())
            putShort(4, 0x5678.toShort())
            putShort(6, 0x7ABC.toShort())
            position(2)
            limit(8)
        }
        val png = File.createTempFile("ticket172-writer-", ".png", context.cacheDir)
        val tiff = File.createTempFile("ticket172-writer-", ".tif", context.cacheDir)
        try {
            val pngBytes = PngWriter.write(
                pixels.duplicate().order(ByteOrder.LITTLE_ENDIAN),
                width = 1,
                height = 1,
                outPath = png.absolutePath,
            )
            require(pngBytes == png.length() && pngBytes > 0L) {
                "PNG JNI writer byte count mismatch"
            }
            val tiffBytes = TiffWriter.write(
                pixels.duplicate().order(ByteOrder.LITTLE_ENDIAN),
                width = 1,
                height = 1,
                outPath = tiff.absolutePath,
            )
            require(tiffBytes == tiff.length() && tiffBytes > 0L) {
                "TIFF JNI writer byte count mismatch"
            }

            val pngCancellation = PngCancellationToken().also { it.cancel() }
            val pngFailure = runCatching {
                PngWriter.write(
                    pixels.duplicate().order(ByteOrder.LITTLE_ENDIAN),
                    1,
                    1,
                    png.absolutePath,
                    cancellation = pngCancellation,
                )
            }.exceptionOrNull()
            require(pngFailure is CancellationException) {
                "PNG pre-cancellation did not map to CancellationException: $pngFailure"
            }

            val tiffCancellation = TiffCancellationToken().also { it.cancel() }
            val tiffFailure = runCatching {
                TiffWriter.write(
                    pixels.duplicate().order(ByteOrder.LITTLE_ENDIAN),
                    1,
                    1,
                    tiff.absolutePath,
                    cancellation = tiffCancellation,
                )
            }.exceptionOrNull()
            require(tiffFailure is CancellationException) {
                "TIFF pre-cancellation did not map to CancellationException: $tiffFailure"
            }
        } finally {
            png.delete()
            tiff.delete()
        }
    }

    private fun processOwnedExportSurvivesObserverRecreationAndCancelsExactlyOnce(
        context: Context,
    ) = runBlocking {
        require(ExportWorkRuntime.state.value is ExportRuntimeState.Idle) {
            "export runtime did not start instrumentation in Idle"
        }

        val entered = CompletableDeferred<Unit>()
        val release = CompletableDeferred<Unit>()
        val runId = requireNotNull(
            ExportWorkRuntime.launch(
                context = context,
                format = ExportFormat.PNG16,
                startedAtMillis = System.currentTimeMillis(),
            ) {
                entered.complete(Unit)
                release.await()
                successfulRuntimeProbeOutcome()
            },
        )
        entered.await()
        val oldActivityObserver = launch {
            ExportWorkRuntime.state.filter {
                it is ExportRuntimeState.Finished && it.runId == runId
            }.first()
        }
        oldActivityObserver.cancelAndJoin()
        release.complete(Unit)
        val finished = withTimeout(5_000) {
            ExportWorkRuntime.state.filter {
                it is ExportRuntimeState.Finished && it.runId == runId
            }.first() as ExportRuntimeState.Finished
        }
        require(finished.outcome is ExportTerminalOutcome.Success) {
            "recreated observer did not receive retained export success"
        }
        val successClaims = coroutineScope {
            (0 until 16).map {
                async(Dispatchers.Default) { ExportWorkRuntime.claimFinished(runId) }
            }.awaitAll()
        }
        require(successClaims.count { it != null } == 1) {
            "terminal export success was not claimed exactly once"
        }

        val cancelEntered = CompletableDeferred<Unit>()
        val cancelRunId = requireNotNull(
            ExportWorkRuntime.launch(
                context = context,
                format = ExportFormat.PNG16,
                startedAtMillis = System.currentTimeMillis(),
            ) {
                cancelEntered.complete(Unit)
                awaitCancellation()
            },
        )
        cancelEntered.await()
        require(ExportWorkRuntime.cancel(cancelRunId)) { "active export rejected cancellation" }
        val cancelled = withTimeout(5_000) {
            ExportWorkRuntime.state.filter {
                it is ExportRuntimeState.Finished && it.runId == cancelRunId
            }.first() as ExportRuntimeState.Finished
        }
        require(cancelled.outcome is ExportTerminalOutcome.Cancelled) {
            "cancelled export was surfaced as ${cancelled.outcome::class.java.simpleName}"
        }
        val cancellationClaims = coroutineScope {
            (0 until 16).map {
                async(Dispatchers.Default) { ExportWorkRuntime.claimFinished(cancelRunId) }
            }.awaitAll()
        }
        require(cancellationClaims.count { it != null } == 1) {
            "terminal cancellation was not claimed exactly once"
        }
    }

    private fun successfulRuntimeProbeOutcome(
        renderId: Long = 0L,
        totalMs: Long = 1L,
        simulateMs: Long = 0L,
    ): ExportTerminalOutcome.Success =
        ExportTerminalOutcome.Success(
            format = ExportFormat.PNG16,
            renderId = renderId,
            bitmap = null,
            totalMs = totalMs,
            phases = ExportPhaseSnapshot(0L, 0L, 0L, simulateMs, 0L, 0L),
            // Explicit so this cross-APK call targets the primary constructor the
            // release-dex check pins, never a synthetic default-args bridge.
            publishedUri = null,
            publishedMimeType = null,
        )

    /**
     * Start a real native render before the runner recreates the Activity, then retain its native
     * result allocation until the replacement UI is resumed. This makes recreation exercise the
     * production process-owned engine/result lifetime instead of a fabricated terminal success.
     */
    @JvmStatic
    fun beginActivityRecreationExportProbe(context: Context): Long = runBlocking {
        synchronized(activityRecreationProbeLock) {
            check(activityRecreationProbe == null) { "Activity recreation probe already active" }
        }
        require(ExportWorkRuntime.state.value is ExportRuntimeState.Idle) {
            "export runtime was not Idle before Activity recreation probe"
        }
        val startup = CompletableDeferred<Throwable?>()
        val release = CompletableDeferred<Unit>()
        val completions = AtomicInteger(0)
        val retainedEngine = AtomicReference<Any?>(null)
        // This is a legitimate export owned by the Activity's already-authorized demo source.
        // The legacy four-argument launch ABI is intentionally UNBOUND and therefore must never
        // be publishable by a source-fenced UI; bind this probe to the exact current generation.
        val sourceIdentity = ExportSourceIdentityAuthority.bind(
            uri = null,
            kind = SourceKind.DEMO,
            authorizationRequired = false,
        )
        val runId = requireNotNull(
            ExportWorkRuntime.launch(
                context = context,
                format = ExportFormat.PNG16,
                startedAtMillis = System.currentTimeMillis(),
                sourceIdentity = sourceIdentity,
            ) {
                try {
                    val startedAt = System.nanoTime()
                    val engine = EngineHolder.get(context)
                    check(retainedEngine.compareAndSet(null, engine)) {
                        "Activity recreation probe replaced its native engine owner"
                    }
                    val pixels = ByteBuffer.allocateDirect(3 * Float.SIZE_BYTES)
                        .order(ByteOrder.nativeOrder())
                        .apply {
                            putFloat(0, 0.18f)
                            putFloat(Float.SIZE_BYTES, 0.18f)
                            putFloat(2 * Float.SIZE_BYTES, 0.18f)
                        }
                    val image = LinearImage(pixels, 1, 1)
                    val simulateStartedAt = System.nanoTime()
                    try {
                        engine.simulate(
                            image,
                            SpektraParams(
                                filmProfile = "kodak_portra_400",
                                printProfile = "kodak_portra_endura",
                                filmRender = FilmRenderingParams(
                                    grain = GrainParams(active = false),
                                    halation = HalationParams(active = false),
                                    dirCouplers = DirCouplersParams(active = false),
                                    glare = GlareParams(active = false),
                                ),
                                io = IoParams(scanFilm = true),
                            ),
                        ).use { nativeResult ->
                            val simulateMs = (System.nanoTime() - simulateStartedAt) / 1_000_000L
                            startup.complete(null)
                            release.await()
                            check(EngineHolder.get(context) === engine) {
                                "Activity recreation replaced the process-owned native engine"
                            }
                            nativeResult.acquireDataLease().use { lease ->
                                val data = lease.data
                                check(data.remaining() >= 3 * Float.SIZE_BYTES) {
                                    "native result window was truncated across Activity recreation"
                                }
                                check(data.getFloat(data.position()).isFinite()) {
                                    "native result became invalid across Activity recreation"
                                }
                            }
                            completions.incrementAndGet()
                            successfulRuntimeProbeOutcome(
                                renderId = nativeResult.renderId,
                                totalMs = (System.nanoTime() - startedAt) / 1_000_000L,
                                simulateMs = simulateMs,
                            )
                        }
                    } finally {
                        image.close()
                    }
                } catch (failure: Throwable) {
                    startup.complete(failure)
                    throw failure
                }
            },
        ) { "Activity recreation probe could not launch deterministic export" }
        val probe = ActivityRecreationExportProbe(
            runId,
            release,
            completions,
            retainedEngine,
        )
        synchronized(activityRecreationProbeLock) {
            check(activityRecreationProbe == null) { "Activity recreation probe raced another probe" }
            activityRecreationProbe = probe
        }
        try {
            withTimeout(30_000) { startup.await()?.let { throw it } }
        } catch (failure: Throwable) {
            abortActivityRecreationExportProbe(runId)
            throw failure
        }
        runId
    }

    /**
     * Release work only after the replacement Activity is resumed. Its real Compose observer must
     * consume the retained terminal result, leaving no stale or second claim behind.
     */
    @JvmStatic
    fun completeActivityRecreationExportProbe(context: Context, runId: Long) {
        val probe = synchronized(activityRecreationProbeLock) {
            requireNotNull(activityRecreationProbe).also {
                require(it.runId == runId) { "wrong Activity recreation probe generation" }
            }
        }
        probe.release.complete(Unit)
        runBlocking {
            withTimeout(15_000) {
                ExportWorkRuntime.state.filter { it is ExportRuntimeState.Idle }.first()
            }
        }
        require(probe.completions.get() == 1) {
            "recreated Activity probe work completed ${probe.completions.get()} times"
        }
        require(probe.engine.get() === EngineHolder.get(context)) {
            "recreated Activity did not retain the process-owned native engine"
        }
        require(ExportWorkRuntime.claimFinished(runId) == null) {
            "recreated Activity left a duplicate terminal publication claim"
        }
        synchronized(activityRecreationProbeLock) {
            if (activityRecreationProbe === probe) activityRecreationProbe = null
        }
    }

    /** Best-effort cleanup for a runner assertion failure; harmless after successful completion. */
    @JvmStatic
    fun abortActivityRecreationExportProbe(runId: Long) {
        val probe = synchronized(activityRecreationProbeLock) {
            activityRecreationProbe?.takeIf { it.runId == runId }
        } ?: return
        probe.release.complete(Unit)
        ExportWorkRuntime.cancel(runId)
        runCatching {
            runBlocking {
                withTimeout(5_000) {
                    ExportWorkRuntime.state.filter { state ->
                        state !is ExportRuntimeState.Running || state.runId != runId
                    }.first()
                }
            }
        }
        ExportWorkRuntime.claimFinished(runId)
        synchronized(activityRecreationProbeLock) {
            if (activityRecreationProbe === probe) activityRecreationProbe = null
        }
    }

    private fun recipeResetInvalidatesInFlightRestore(context: Context) {
        val key = "1701701701701701701701701701701701701701701701701701701701701701"
        val beforeReset = Recipes.generation(key)
        Recipes.delete(context, key)
        require(Recipes.generation(key) != beforeReset) {
            "recipe reset did not invalidate an in-flight restore generation"
        }
    }

    /**
     * Phase one of the host-orchestrated process-death probe. This method intentionally
     * leaves one real pending MediaStore row and its production journal entry durable.
     * The instrumentation process must exit before [recoverProcessDeathProbe] is run.
     */
    @JvmStatic
    fun seedProcessDeathProbe(context: Context): String {
        require(Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            "ticket #170 process-death probe requires API 29+ MediaStore pending rows"
        }
        val appContext = context.applicationContext
        val state = appContext.getSharedPreferences(PROCESS_DEATH_PREFS, Context.MODE_PRIVATE)
        val backend = AndroidPendingExportBackend(appContext)
        val journal = SharedPreferencesPendingExportJournal(appContext)

        // A previously interrupted probe is test-owned state, so it is safe to retire
        // before creating the single row that this seed invocation will identify.
        state.getString(KEY_PROCESS_DEATH_TOKEN, null)?.let { previous ->
            try { backend.delete(previous) } catch (_: Throwable) { }
            try { journal.remove(previous) } catch (_: Throwable) { }
        }
        require(state.edit().clear().commit()) { "could not reset process-death probe state" }

        val token = backend.reserve(
            ExportDestinationSpec(
                PROCESS_DEATH_REQUESTED_NAME,
                "image/png",
                "Pictures/Spektrafilm",
            ),
        )
        try {
            journal.add(token)
            val displayName = requirePendingRow(context, token)
            require(hasPrefix(displayName, PRODUCTION_PENDING_TAG)) {
                "seeded row is not tagged for transactional discovery: $displayName"
            }
            require(token in journal.tokens()) { "seeded token was not durably journaled" }
            require(
                state.edit()
                    .putString(KEY_PROCESS_DEATH_TOKEN, token)
                    .putString(KEY_PROCESS_DEATH_DISPLAY_NAME, displayName)
                    .putInt(KEY_PROCESS_DEATH_PID, Process.myPid())
                    .commit(),
            ) { "could not record process-death probe token" }
            return token
        } catch (failure: Throwable) {
            try { backend.delete(token) } catch (_: Throwable) { }
            try { journal.remove(token) } catch (_: Throwable) { }
            try { state.edit().clear().commit() } catch (_: Throwable) { }
            throw failure
        }
    }

    /** Phase two; the host must force-stop the target between seed and this call. */
    @JvmStatic
    fun recoverProcessDeathProbe(context: Context): String {
        require(Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            "ticket #170 process-death probe requires API 29+ MediaStore pending rows"
        }
        val appContext = context.applicationContext
        val state = appContext.getSharedPreferences(PROCESS_DEATH_PREFS, Context.MODE_PRIVATE)
        val token = state.getString(KEY_PROCESS_DEATH_TOKEN, null)
            ?: error("no durable seed token; run ticket170_phase=seed first")
        val expectedDisplayName = state.getString(KEY_PROCESS_DEATH_DISPLAY_NAME, null)
            ?: error("seed display-name evidence is missing")
        val seedPid = state.getInt(KEY_PROCESS_DEATH_PID, -1)
        require(seedPid > 0 && seedPid != Process.myPid()) {
            "recovery did not start in a new process (seed=$seedPid current=${Process.myPid()})"
        }
        val backend = AndroidPendingExportBackend(appContext)
        val journal = SharedPreferencesPendingExportJournal(appContext)

        require(backend.state(token) == StoredExportState.PENDING) {
            "seeded MediaStore row was not pending before recovery"
        }
        require(requirePendingRow(context, token) == expectedDisplayName) {
            "seeded MediaStore row identity changed across processes"
        }
        require(token in journal.tokens()) {
            "seeded journal token did not survive the process boundary"
        }

        val report = ExportTransaction(backend, journal).recover()
        require(backend.state(token) == StoredExportState.MISSING) {
            "recovery left the exact seeded MediaStore row"
        }
        require(token !in journal.tokens()) {
            "recovery left the exact seeded journal token"
        }
        require(report.removedPending >= 1) {
            "recovery did not report removal of the seeded pending row: $report"
        }
        require(state.edit().clear().commit()) {
            "could not retire successful process-death probe state"
        }
        return token
    }

    private fun durableJournalRecoveryWithReopenedAdapters(context: Context) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        val backend = AndroidPendingExportBackend(context)
        val token = backend.reserve(
            ExportDestinationSpec(
                "Spektrafilm_ticket170_process_death.png",
                "image/png",
                "Pictures/Spektrafilm",
            ),
        )
        SharedPreferencesPendingExportJournal(context).add(token)

        // New backend/journal instances model the next process reading durable state.
        ExportTransaction(
            AndroidPendingExportBackend(context),
            SharedPreferencesPendingExportJournal(context),
        ).recover()

        require(AndroidPendingExportBackend(context).state(token) == StoredExportState.MISSING) {
            "process-death recovery left a pending MediaStore row"
        }
        require(token !in SharedPreferencesPendingExportJournal(context).tokens()) {
            "process-death recovery left a journal entry"
        }
    }

    private fun outOfSpaceAndInterruptedCloseNeverPublish(context: Context) {
        val stage = File.createTempFile("ticket170-", ".bin", context.cacheDir)
        try {
            writeFileDurably(stage, byteArrayOf(1, 2, 3, 4, 5))
            val artifact = EncodedArtifact.fromCompletedFile(stage, stage.length())
            for (failure in intArrayOf(FAILURE_ENOSPC, FAILURE_CLOSE)) {
                val backend = FailingBackend(failure)
                val journal = MemoryJournal()
                var committed = false
                try {
                    ExportTransaction(backend, journal).commit(
                        artifact,
                        ExportDestinationSpec("failure.bin", "image/png", "Pictures/Spektrafilm"),
                    )
                    committed = true
                } catch (_: Throwable) { }
                require(!committed) { "$failure unexpectedly committed" }
                require(backend.publishCalls == 0) { "$failure published a partial destination" }
                require(backend.deleteCalls == 1) { "$failure did not delete its reservation" }
                require(journal.tokens().isEmpty()) { "$failure left a stale journal entry" }
            }
        } finally {
            stage.delete()
        }
    }

    private fun revokedGrantRestoresAsNeedsAuthorization(context: Context) {
        val store = ProbeSourceRefStore(context)
        val ref = PersistedSourceRef(
            "content://com.spectrafilm.test/revoked",
            "RAW",
            "revoked.dng",
            SourceAccessMode.PERSISTED,
        )
        try {
            store.save(ref)
            val reopened = ProbeSourceRefStore(context)
            val restored = SourceAccessCoordinator(RevokedGrantBackend, reopened).restore()
            require(restored == SourceRestoreResult.NeedsAuthorization(ref)) {
                "revoked persisted grant did not produce reauthorization state: $restored"
            }
        } finally {
            store.clear()
        }
    }

    private fun realPersistableGrantReleaseRestoresAsNeedsAuthorization(context: Context) {
        val resolver = context.contentResolver
        val uri = Ticket170GrantProvider.testUri
        val platformBackend = AndroidUriGrantBackend(context)
        val unrelatedBefore = persistedReadsExcept(platformBackend, uri.toString())
        val backend = ScopedProbeGrantBackend(platformBackend, uri.toString())
        val store = ProbeSourceRefStore(context)
        var ref: PersistedSourceRef? = null
        try {
            store.clear()
            val grant = resolver.call(
                uri,
                Ticket170GrantProvider.METHOD_GRANT,
                context.packageName,
                null,
            )
            require(grant?.getBoolean(Ticket170GrantProvider.KEY_SUCCESS) == true) {
                "test provider did not issue a persistable URI grant"
            }
            val coordinator = SourceAccessCoordinator(backend, store)
            ref = coordinator.acquire(uri.toString(), "RAW", "ticket170-real-grant.dng")
            require(ref.accessMode == SourceAccessMode.PERSISTED) {
                "real provider URI was recorded as transient"
            }
            require(backend.hasPersistedRead(uri.toString())) {
                "Android did not record the persistable read grant"
            }
            require(coordinator.restore() == SourceRestoreResult.Ready(ref)) {
                "live persisted grant did not restore as ready"
            }

            // Exercise both target-side release and provider-side revocation. The
            // test provider also closes its read gate, making readability observable.
            backend.releasePersistableRead(uri.toString())
            resolver.call(
                uri,
                Ticket170GrantProvider.METHOD_REVOKE,
                context.packageName,
                null,
            )
            require(!backend.hasPersistedRead(uri.toString())) {
                "released URI still appears in Android persisted grants"
            }
            require(!backend.canRead(uri.toString())) {
                "revoked provider URI remained readable"
            }
            val restored = SourceAccessCoordinator(
                backend,
                ProbeSourceRefStore(context),
            ).restore()
            require(restored == SourceRestoreResult.NeedsAuthorization(ref)) {
                "real revoked grant did not produce reauthorization state: $restored"
            }
            require(persistedReadsExcept(platformBackend, uri.toString()) == unrelatedBefore) {
                "source probe mutated an unrelated persisted URI grant"
            }
        } finally {
            try {
                if (backend.hasPersistedRead(uri.toString())) {
                    backend.releasePersistableRead(uri.toString())
                }
            } catch (_: Throwable) { }
            try {
                resolver.call(
                    uri,
                    Ticket170GrantProvider.METHOD_REVOKE,
                    context.packageName,
                    null,
                )
            } catch (_: Throwable) { }
            store.clear()
        }
    }

    private fun duplicateNamesCommitDistinctVerifiedMediaStoreRows(context: Context) {
        val png = Base64.decode(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl2n0sAAAAASUVORK5CYII=",
            Base64.DEFAULT,
        )
        val firstStage = File.createTempFile("ticket170-a-", ".png", context.cacheDir)
        val secondStage = File.createTempFile("ticket170-b-", ".png", context.cacheDir)
        var first: android.net.Uri? = null
        var second: android.net.Uri? = null
        try {
            writeFileDurably(firstStage, png)
            writeFileDurably(secondStage, png)
            val name = "Spektrafilm_ticket170_duplicate.png"
            first = publishStagedImage(
                context,
                EncodedArtifact.fromCompletedFile(firstStage, firstStage.length()),
                name,
                "image/png",
            )
            second = publishStagedImage(
                context,
                EncodedArtifact.fromCompletedFile(secondStage, secondStage.length()),
                name,
                "image/png",
            )
            require(first != second) { "duplicate display names reused one MediaStore row" }
            assertPublishedBytes(context, first, png)
            assertPublishedBytes(context, second, png)
        } finally {
            first?.let { context.contentResolver.delete(it, null, null) }
            second?.let { context.contentResolver.delete(it, null, null) }
            firstStage.delete()
            secondStage.delete()
        }
    }

    private fun assertPublishedBytes(context: Context, uri: android.net.Uri, expected: ByteArray) {
        val cursor = context.contentResolver.query(
            uri,
            arrayOf(MediaStore.Images.Media.IS_PENDING, MediaStore.Images.Media.DISPLAY_NAME),
            null,
            null,
            null,
        ) ?: error("published MediaStore row was not queryable")
        try {
            require(cursor.moveToFirst()) { "published MediaStore row is missing" }
            require(cursor.getInt(0) == 0) { "MediaStore row stayed pending" }
            require(!cursor.getString(1).isEmpty()) { "MediaStore display name is blank" }
        } finally {
            cursor.close()
        }
        val source = context.contentResolver.openInputStream(uri)
            ?: error("published MediaStore bytes were not readable")
        val actual = try {
            val sink = ByteArrayOutputStream()
            val buffer = ByteArray(8192)
            while (true) {
                val count = source.read(buffer)
                if (count < 0) break
                sink.write(buffer, 0, count)
            }
            sink.toByteArray()
        } finally {
            source.close()
        }
        require(Arrays.equals(expected, actual)) { "MediaStore readback differs from staged bytes" }
    }

    private fun requirePendingRow(context: Context, token: String): String {
        val uri = MediaStore.setIncludePending(Uri.parse(token))
        val cursor = context.contentResolver.query(
            uri,
            arrayOf(
                MediaStore.Images.Media.IS_PENDING,
                MediaStore.Images.Media.DISPLAY_NAME,
            ),
            null,
            null,
            null,
        ) ?: error("pending MediaStore row was not queryable")
        try {
            require(cursor.moveToFirst()) { "pending MediaStore row is missing" }
            require(cursor.getInt(0) == 1) { "MediaStore row was unexpectedly published" }
            return cursor.getString(1) ?: error("pending MediaStore row has no display name")
        } finally {
            cursor.close()
        }
    }

    /** Avoids Kotlin text helpers because the target APK is the parent classloader in release tests. */
    private fun hasPrefix(value: String, prefix: String): Boolean {
        if (value.length < prefix.length) return false
        var index = 0
        while (index < prefix.length) {
            if (value[index] != prefix[index]) return false
            index += 1
        }
        return true
    }

    private fun writeFileDurably(file: File, bytes: ByteArray) {
        val sink = FileOutputStream(file)
        try {
            sink.write(bytes)
            sink.flush()
            sink.fd.sync()
        } finally {
            sink.close()
        }
    }

    private class FailingBackend(private val failure: Int) : PendingExportBackend {
        var publishCalls = 0
        var deleteCalls = 0
        private var storedState = StoredExportState.MISSING
        private val sink = ByteArrayOutputStream()

        override fun reserve(spec: ExportDestinationSpec): String {
            storedState = StoredExportState.PENDING
            return "fake://pending"
        }

        override fun openWrite(token: String): OutputStream = object : OutputStream() {
            private var count = 0
            override fun write(value: Int) {
                if (failure == FAILURE_ENOSPC && count >= 2) throw IOException("ENOSPC")
                sink.write(value)
                count++
            }

            override fun close() {
                if (failure == FAILURE_CLOSE) throw IOException("interrupted close")
            }
        }

        override fun openRead(token: String): InputStream = ByteArrayInputStream(sink.toByteArray())
        override fun publish(token: String, spec: ExportDestinationSpec): Int {
            publishCalls++
            storedState = StoredExportState.PUBLISHED
            return 1
        }

        override fun delete(token: String): Int {
            deleteCalls++
            storedState = StoredExportState.MISSING
            return 1
        }

        override fun state(token: String): StoredExportState = storedState
    }

    private class MemoryJournal : PendingExportJournal {
        private val values = LinkedHashSet<String>()
        override fun add(token: String) { values += token }
        override fun remove(token: String) { values -= token }
        override fun tokens(): Set<String> = LinkedHashSet(values)
    }

    /** Dedicated durable store: instrumentation must never replace a user's selected source. */
    private class ProbeSourceRefStore(context: Context) : SourceRefStore {
        private val prefs = context.applicationContext.getSharedPreferences(
            SOURCE_PROBE_PREFS,
            Context.MODE_PRIVATE,
        )

        override fun load(): PersistedSourceRef? {
            if (prefs.getInt(SOURCE_KEY_VERSION, 0) != SOURCE_PROBE_VERSION) return null
            val uri = prefs.getString(SOURCE_KEY_URI, null) ?: return null
            val kind = prefs.getString(SOURCE_KEY_KIND, null) ?: return null
            val name = prefs.getString(SOURCE_KEY_NAME, null) ?: return null
            val modeName = prefs.getString(SOURCE_KEY_MODE, null) ?: return null
            val mode = try {
                SourceAccessMode.valueOf(modeName)
            } catch (_: IllegalArgumentException) {
                return null
            }
            return PersistedSourceRef(uri, kind, name, mode)
        }

        override fun save(ref: PersistedSourceRef) {
            check(
                prefs.edit()
                    .putInt(SOURCE_KEY_VERSION, SOURCE_PROBE_VERSION)
                    .putString(SOURCE_KEY_URI, ref.uri)
                    .putString(SOURCE_KEY_KIND, ref.kind)
                    .putString(SOURCE_KEY_NAME, ref.displayName)
                    .putString(SOURCE_KEY_MODE, ref.accessMode.name)
                    .commit(),
            ) { "could not save test-owned source reference" }
        }

        override fun clear() {
            check(prefs.edit().clear().commit()) { "could not clear test-owned source reference" }
        }
    }

    private object RevokedGrantBackend : UriGrantBackend {
        override fun takePersistableRead(uri: String): Boolean = false
        override fun releasePersistableRead(uri: String) = Unit
        override fun hasPersistedRead(uri: String): Boolean = false
        // The AndroidTest APK is separate from the minified target APK. Use the
        // platform collection directly so this probe does not create a runtime
        // dependency on a Kotlin helper that R8 can legitimately remove from the app.
        override fun persistedReads(): Set<String> = java.util.Collections.emptySet()
        override fun canRead(uri: String): Boolean = false
    }

    private fun persistedReadsExcept(backend: UriGrantBackend, excludedUri: String): Set<String> {
        val remaining = LinkedHashSet(backend.persistedReads())
        remaining.remove(excludedUri)
        return remaining
    }

    /** Keeps reconciliation in this instrumentation probe from touching real user grants. */
    private class ScopedProbeGrantBackend(
        private val delegate: UriGrantBackend,
        private val probeUri: String,
    ) : UriGrantBackend {
        override fun takePersistableRead(uri: String): Boolean {
            require(uri == probeUri)
            return delegate.takePersistableRead(uri)
        }

        override fun releasePersistableRead(uri: String) {
            require(uri == probeUri)
            delegate.releasePersistableRead(uri)
        }

        override fun hasPersistedRead(uri: String): Boolean =
            uri == probeUri && delegate.hasPersistedRead(uri)

        override fun persistedReads(): Set<String> {
            val scoped = LinkedHashSet<String>()
            for (uri in delegate.persistedReads()) {
                if (uri == probeUri) scoped.add(uri)
            }
            return scoped
        }

        override fun canRead(uri: String): Boolean =
            uri == probeUri && delegate.canRead(uri)
    }

    private const val FAILURE_ENOSPC = 1
    private const val FAILURE_CLOSE = 2
    private const val RAPID_FOREGROUND_EXPORT_COUNT = 3
    private const val MAIN_LOOPER_BLOCK_LIMIT_MS = 4_000L
    private const val FOREGROUND_SERVICE_WATCHDOG_SETTLE_MS = 6_000L
    private const val PROCESS_DEATH_PREFS = "ticket170_process_death_probe_v1"
    private const val KEY_PROCESS_DEATH_TOKEN = "media_store_uri"
    private const val KEY_PROCESS_DEATH_DISPLAY_NAME = "pending_display_name"
    private const val KEY_PROCESS_DEATH_PID = "seed_pid"
    private const val PROCESS_DEATH_REQUESTED_NAME = "Spektrafilm_ticket170_process_death.png"
    private const val PRODUCTION_PENDING_TAG = "Spektrafilm-pending-tx-"
    private const val SOURCE_PROBE_PREFS = "ticket170_source_ref_probe_v1"
    private const val SOURCE_PROBE_VERSION = 1
    private const val SOURCE_KEY_VERSION = "schema_version"
    private const val SOURCE_KEY_URI = "uri"
    private const val SOURCE_KEY_KIND = "kind"
    private const val SOURCE_KEY_NAME = "display_name"
    private const val SOURCE_KEY_MODE = "access_mode"
}
