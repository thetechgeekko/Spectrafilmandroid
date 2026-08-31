/*
 * Spektrafilm for Android. GPLv3. Film modeling powered by spektrafilm (GPLv3).
 */
package com.spectrafilm.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat

/**
 * Prevents a delayed completion command for export N from stopping the service after export N+1
 * has started. Service start IDs protect command ordering; this gate protects export ordering.
 */
internal class ExportForegroundServiceGenerationGate {
    private var newestStartedRunId = Long.MIN_VALUE

    fun recordStart(runId: Long) {
        if (runId > newestStartedRunId) newestStartedRunId = runId
    }

    fun mayStop(runId: Long): Boolean = runId >= newestStartedRunId
}

/**
 * Keeps a running export alive when the user leaves the app.
 *
 * ## Why this exists: kill-resistance, NOT speed
 *
 * A long backgrounded export is a kill candidate for Samsung's device-health
 * manager (`com.sec.android.sdhms`). Measured on SM-S948W, this service is what
 * stops that:
 *
 * ```
 *   with the service running:  oom_score_adj =  50
 *   after it stops:            oom_score_adj = 700
 * ```
 *
 * That is the whole of its value, and it is real — it is the fix for #153.
 *
 * ## It does NOT fix the 4x background slowdown. Measured, not assumed.
 *
 * This service was written believing it would. It does not, and the device run
 * that proved it also explained why. Backgrounding still costs ~3.9x (backgrounded
 * median 55063 ms vs foreground 14102 ms over 7 runs) even though the service
 * starts every single time and the notification posts every single time.
 *
 * The cpuset is the mechanism, and it predicts the result exactly — but a
 * foreground service does not determine which cpuset the process lands in:
 *
 * ```
 *   cpu0-5  max 3.63 GHz        cpu6-7  max 4.74 GHz   (prime pair)
 *
 *   /top-app          cpus=0-7      <- 14214 ms   foreground
 *   /foreground-boost cpus=0-7      <- 14927 ms   transient interaction boost
 *   /foreground       cpus=0-5      <- no prime cores; best an FGS normally rates
 *   /moderate         cpus=0-1,4-5  <- 55357 ms   where backgrounded exports land
 *   /background       cpus=0-1,4-5  <- identical mask to /moderate
 * ```
 *
 * `/moderate` is byte-identical to `/background` in CPU terms: half the cores and
 * zero prime cores. **No service-tier cpuset on this device contains cpu6/7**, so
 * changing `foregroundServiceType` cannot rescue it either. The perf half of #153
 * is not solvable with a foreground service on this hardware.
 *
 * The 4x is also only ever paid while the user leaves the app — the foreground
 * path was never slow. The remaining honest lever is the ~14 s foreground export.
 *
 * ## Ownership split
 *
 * It runs no rendering code of its own. [ExportWorkRuntime]'s process-owned coroutine
 * owns the export across Activity recreation; this service only holds the process's
 * lifecycle/scheduling class while that work runs. The service therefore remains off
 * the render path and carries no image-parity risk.
 *
 * Every failure path is swallowed: an export that would have succeeded slowly must
 * never fail because a notification channel or an OEM background-start rule said no.
 */
class ExportForegroundService : Service() {
    private val generationGate = ExportForegroundServiceGenerationGate()

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Must reach startForeground within ~5 s of startForegroundService or the
        // system throws; doing it first thing in onStartCommand is well inside that.
        try {
            ServiceCompat.startForeground(
                this,
                NOTIFICATION_ID,
                buildNotification(),
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
                } else {
                    0
                },
            )
        } catch (t: Throwable) {
            // API 31+ can refuse a background start, and OEM policies vary. The export
            // still runs — just at background priority, i.e. how it behaved before.
            Diag.w("export foreground service could not start: ${t.message}")
            stopSelf(startId)
            return START_NOT_STICKY
        }

        val runId = intent?.getLongExtra(EXTRA_RUN_ID, INVALID_RUN_ID) ?: INVALID_RUN_ID
        when (intent?.action) {
            ACTION_START -> {
                if (runId > INVALID_RUN_ID) generationGate.recordStart(runId)
            }
            ACTION_STOP -> {
                // Never call stopService() from the worker completion path. A tiny or
                // pre-cancelled export can finish before Android dispatches the original
                // start command; stopping it externally in that gap triggers
                // ForegroundServiceDidNotStartInTimeException. This queued command reaches
                // startForeground above first, then lets the service stop itself. The
                // startId and run generation both prevent an old completion from stopping
                // a newer export.
                if (runId > INVALID_RUN_ID && generationGate.mayStop(runId)) {
                    stopSelfResult(startId)
                }
            }
            else -> {
                Diag.w("export foreground service received an invalid command")
                stopSelfResult(startId)
            }
        }
        // Not sticky: if the process dies the export died with it, and restarting a
        // service with no work to do would only show a notification for nothing.
        return START_NOT_STICKY
    }

    private fun buildNotification(): Notification {
        ensureChannel(this)
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.app_name))
            .setContentText("Exporting photo…")
            .setSmallIcon(R.drawable.ic_launcher_monochrome)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setOngoing(true)
            // Indeterminate: the engine reports stage timings, not a monotonic
            // fraction, so a percentage here would be invented.
            .setProgress(0, 0, true)
            .build()
    }

    companion object {
        private const val CHANNEL_ID = "export"
        private const val NOTIFICATION_ID = 1001
        private const val ACTION_START = "com.spectrafilm.app.action.START_EXPORT_FOREGROUND"
        private const val ACTION_STOP = "com.spectrafilm.app.action.STOP_EXPORT_FOREGROUND"
        private const val EXTRA_RUN_ID = "com.spectrafilm.app.extra.EXPORT_RUN_ID"
        private const val INVALID_RUN_ID = 0L

        private fun ensureChannel(ctx: Context) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
            val mgr = ctx.getSystemService(NotificationManager::class.java) ?: return
            if (mgr.getNotificationChannel(CHANNEL_ID) != null) return
            mgr.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, "Export", NotificationManager.IMPORTANCE_LOW).apply {
                    description = "Keeps a photo export running at full speed in the background."
                    setShowBadge(false)
                },
            )
        }

        /**
         * Enter the foreground scheduling group. Safe to call when already running.
         *
         * Never throws: if the service cannot start, the export proceeds at whatever
         * priority the scheduler gives it, which is the pre-existing behaviour.
         */
        fun start(ctx: Context, runId: Long) {
            try {
                ContextCompat.startForegroundService(
                    ctx.applicationContext,
                    commandIntent(ctx, ACTION_START, runId),
                )
            } catch (t: Throwable) {
                Diag.w("export foreground service start refused: ${t.message}")
            }
        }

        /**
         * Queue service-owned shutdown after foreground promotion. Safe when the original start
         * command is still pending and safe against a later export generation.
         */
        fun stop(ctx: Context, runId: Long) {
            try {
                ContextCompat.startForegroundService(
                    ctx.applicationContext,
                    commandIntent(ctx, ACTION_STOP, runId),
                )
            } catch (t: Throwable) {
                Diag.w("export foreground service stop command refused: ${t.message}")
            }
        }

        private fun commandIntent(ctx: Context, action: String, runId: Long): Intent =
            Intent(ctx.applicationContext, ExportForegroundService::class.java)
                .setAction(action)
                .putExtra(EXTRA_RUN_ID, runId)
    }
}
