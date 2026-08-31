/*
 * Spektrafilm for Android — external links + app metadata. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Centralizes the project URLs (source, issue tracker, author socials) and a helper
 * to open them with an ACTION_VIEW intent, plus a way to read the installed app
 * version without enabling BuildConfig (uses PackageManager).
 */
package com.spectrafilm.app

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri

/** Project + author links used by Settings / About / onboarding. */
object Links {
    const val SOURCE = "https://github.com/thetechgeekko/Spektrafilm-android/"
    const val RELEASES = "https://github.com/thetechgeekko/Spektrafilm-android/releases"
    const val ISSUES = "https://github.com/thetechgeekko/Spektrafilm-android/issues"
    const val NEW_ISSUE = "https://github.com/thetechgeekko/Spektrafilm-android/issues/new"
    const val AUTHOR_INSTAGRAM = "https://www.instagram.com/akshay.pool/"
    const val AUTHOR_YOUTUBE = "https://www.youtube.com/@Akshayishere/videos"
    const val SPEKTRAFILM = "https://github.com/Spektrafilm"
    const val IMAGE_TOOLBOX = "https://github.com/T8RIN/ImageToolbox"
    const val COLOUR_SCIENCE = "https://www.colour-science.org/"
    const val LIBRAW = "https://www.libraw.org/"
    const val LIBRAW_SOURCE = "https://github.com/LibRaw/LibRaw"
    const val LIBRAW_0222_ARCHIVE = "https://www.libraw.org/data/LibRaw-0.22.2.tar.gz"
    const val PIXLS = "https://pixls.us/"
    const val GPLV3 = "https://www.gnu.org/licenses/gpl-3.0.html"

    /** Open [url] in an external browser/handler. Safe to call from the UI thread. */
    fun open(ctx: Context, url: String) {
        runCatching {
            val intent = Intent(Intent.ACTION_VIEW, Uri.parse(url))
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            ctx.startActivity(intent)
        }
    }
}

/** The installed app version name (e.g. "0.9.0"), or "?" if unavailable. */
fun appVersionName(ctx: Context): String =
    runCatching {
        @Suppress("DEPRECATION")
        ctx.packageManager.getPackageInfo(ctx.packageName, 0).versionName ?: "?"
    }.getOrDefault("?")

/** The installed app version code, or 0 if unavailable. */
fun appVersionCode(ctx: Context): Long =
    runCatching {
        val pi = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.P) {
            pi.longVersionCode
        } else {
            @Suppress("DEPRECATION")
            pi.versionCode.toLong()
        }
    }.getOrDefault(0L)
