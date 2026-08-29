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
 * Holds the process in the foreground scheduling group for the duration of an export.
 *
 * ## Why this exists
 *
 * The export coroutine runs in the Activity's scope. When the user leaves the app
 * mid-export, Android moves the whole process to the `background` cpuset — on the
 * measured device (SM-S948W) that is the efficiency cluster — and the render slows
 * down by roughly **4x**, uniformly across every stage:
 *
 * ```
 *   12.5 MP export, same image, same settings:
 *     foreground   13894 ms          background   55631 ms     (4.00x)
 *   per stage, foreground -> background:
 *     preprocess 5.5x   grain 5.1x   filming_expose 5.0x   develop 4.0x
 *     print_expose 3.8x  scan 3.2x   scan_spatial 3.0x
 *     halation 2.1x      dir_couplers 2.1x
 * ```
 *
 * That 4x is larger than every kernel optimisation on this branch combined (which
 * measured 1.12x foreground), which is what makes this the highest-value change
 * available. A long backgrounded export is also a kill candidate for Samsung's
 * device-health manager (`com.sec.android.sdhms`); a foreground service with a
 * visible notification is exempt from that reaper.
 *
 * ## What it does NOT do
 *
 * It runs no work of its own. The export stays exactly where it is, in the caller's
 * coroutine — this service only pins the process's scheduling class while it runs.
 * That keeps the change off the render path entirely, so it carries no parity risk.
 *
 * Every failure path is swallowed: an export that would have succeeded slowly must
 * never fail because a notification channel or an OEM background-start rule said no.
 */
class ExportForegroundService : Service() {

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
        fun start(ctx: Context) {
            try {
                ContextCompat.startForegroundService(
                    ctx.applicationContext,
                    Intent(ctx.applicationContext, ExportForegroundService::class.java),
                )
            } catch (t: Throwable) {
                Diag.w("export foreground service start refused: ${t.message}")
            }
        }

        /** Leave the foreground group. Safe to call when not running. */
        fun stop(ctx: Context) {
            try {
                ctx.applicationContext.stopService(
                    Intent(ctx.applicationContext, ExportForegroundService::class.java),
                )
            } catch (t: Throwable) {
                Diag.w("export foreground service stop failed: ${t.message}")
            }
        }
    }
}
