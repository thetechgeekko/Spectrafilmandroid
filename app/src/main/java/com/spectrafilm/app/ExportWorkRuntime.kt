/*
 * Spektrafilm for Android — process-owned export execution and durable UI handoff. GPLv3.
 */
package com.spectrafilm.app

import android.content.Context
import android.graphics.Bitmap
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

internal data class ExportPhaseSnapshot(
    val setupMs: Long,
    val decodeMs: Long,
    val exifMs: Long,
    val simulateMs: Long,
    val gradeMs: Long,
    val encodeMs: Long,
)

internal sealed interface ExportTerminalOutcome {
    val format: ExportFormat
    val renderId: Long

    data class Success(
        override val format: ExportFormat,
        override val renderId: Long,
        val bitmap: Bitmap?,
        val totalMs: Long,
        val phases: ExportPhaseSnapshot,
    ) : ExportTerminalOutcome

    data class Failure(
        override val format: ExportFormat,
        override val renderId: Long,
        val elapsedMs: Long,
        val cause: Throwable,
    ) : ExportTerminalOutcome

    data class Cancelled(
        override val format: ExportFormat,
        override val renderId: Long,
        val elapsedMs: Long,
    ) : ExportTerminalOutcome
}

internal sealed interface ExportRuntimeState {
    data object Idle : ExportRuntimeState
    data class Running(val runId: Long, val format: ExportFormat) : ExportRuntimeState
    data class Finished(val runId: Long, val outcome: ExportTerminalOutcome) : ExportRuntimeState
}

/**
 * Activity recreation must not own or cancel a durable MediaStore transaction. This process
 * singleton owns the coroutine and retains its terminal result until a recreated UI acknowledges
 * it. The foreground service owns scheduling/kill resistance; this runtime owns the actual work.
 */
internal object ExportWorkRuntime {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val ids = AtomicLong(0L)
    private val lock = Any()
    private val mutableState = MutableStateFlow<ExportRuntimeState>(ExportRuntimeState.Idle)
    private var activeJob: Job? = null
    private var terminalExpiryJob: Job? = null

    val state: StateFlow<ExportRuntimeState> = mutableState.asStateFlow()

    fun launch(
        context: Context,
        format: ExportFormat,
        startedAtMillis: Long,
        work: suspend () -> ExportTerminalOutcome,
    ): Long? = synchronized(lock) {
        if (activeJob?.isActive == true) return@synchronized null
        terminalExpiryJob?.cancel()
        terminalExpiryJob = null
        (mutableState.value as? ExportRuntimeState.Finished)
            ?.outcome
            ?.let { previous ->
                if (previous is ExportTerminalOutcome.Success) previous.bitmap?.recycle()
            }
        val runId = ids.incrementAndGet()
        val appContext = context.applicationContext
        mutableState.value = ExportRuntimeState.Running(runId, format)
        ExportForegroundService.start(appContext, runId)
        val job = scope.launch {
            val outcome = try {
                work()
            } catch (_: CancellationException) {
                ExportTerminalOutcome.Cancelled(
                    format = format,
                    renderId = 0L,
                    elapsedMs = System.currentTimeMillis() - startedAtMillis,
                )
            } catch (failure: Throwable) {
                ExportTerminalOutcome.Failure(
                    format = format,
                    renderId = 0L,
                    elapsedMs = System.currentTimeMillis() - startedAtMillis,
                    cause = failure,
                )
            }
            publishFinished(runId, outcome)
        }
        activeJob = job
        job.invokeOnCompletion { cause ->
            if (cause is CancellationException) {
                publishFinished(
                    runId,
                    ExportTerminalOutcome.Cancelled(
                        format = format,
                        renderId = 0L,
                        elapsedMs = System.currentTimeMillis() - startedAtMillis,
                    ),
                )
            }
            ExportForegroundService.stop(appContext, runId)
            synchronized(lock) {
                if (activeJob === job) activeJob = null
            }
        }
        runId
    }

    /** Requests cancellation only for the currently running generation. */
    fun cancel(runId: Long): Boolean {
        val job = synchronized(lock) {
            val current = mutableState.value
            if (current !is ExportRuntimeState.Running || current.runId != runId) {
                return false
            }
            activeJob
        } ?: return false
        job.cancel(CancellationException("export cancelled by user"))
        return true
    }

    /** Atomically transfers a retained terminal outcome (and its bitmap) to exactly one UI. */
    fun claimFinished(runId: Long): ExportTerminalOutcome? = synchronized(lock) {
        val current = mutableState.value
        if (current !is ExportRuntimeState.Finished || current.runId != runId) {
            return@synchronized null
        }
        terminalExpiryJob?.cancel()
        terminalExpiryJob = null
        mutableState.value = ExportRuntimeState.Idle
        current.outcome
    }

    private fun publishFinished(runId: Long, outcome: ExportTerminalOutcome) {
        synchronized(lock) {
            val current = mutableState.value
            if (current !is ExportRuntimeState.Running || current.runId != runId) return
            mutableState.value = ExportRuntimeState.Finished(runId, outcome)
            terminalExpiryJob?.cancel()
            terminalExpiryJob = scope.launch {
                delay(TERMINAL_RETENTION_MS)
                synchronized(lock) {
                    val retained = mutableState.value
                    if (retained is ExportRuntimeState.Finished && retained.runId == runId) {
                        val success = retained.outcome as? ExportTerminalOutcome.Success
                        success?.bitmap?.recycle()
                        mutableState.value = ExportRuntimeState.Idle
                        terminalExpiryJob = null
                    }
                }
            }
        }
    }

    private const val TERMINAL_RETENTION_MS = 5 * 60 * 1_000L
}
