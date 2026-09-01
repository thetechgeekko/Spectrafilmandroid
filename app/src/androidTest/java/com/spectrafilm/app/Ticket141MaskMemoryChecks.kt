/*
 * Spektrafilm for Android — ticket #141 release-device mask memory probe. GPLv3.
 */
package com.spectrafilm.app

import android.os.Debug
import android.os.Build
import android.os.SharedMemory
import android.os.SystemClock
import android.system.Os
import android.system.OsConstants
import android.content.Context
import com.spectrafilm.app.masks.MaskCompositor
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets
import java.security.MessageDigest
import java.io.FileInputStream
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/** Exact release/R8 memory probe called by the shared release-candidate runner. */
object Ticket141MaskMemoryChecks {
    private val pageSizeKiB = Os.sysconf(OsConstants._SC_PAGESIZE) / 1024L

    @JvmStatic
    fun run(context: Context, width: Int, height: Int, maskCount: Int, repeats: Int): String {
        require(width > 0 && height > 0)
        require(maskCount in 1..16)
        require(repeats in 1..5)
        val budgetEvidence = MaskCompositor.configureTicket141MemoryBudget(context)
        val pixels = Math.multiplyExact(width.toLong(), height.toLong())
        val byteCount = Math.multiplyExact(Math.multiplyExact(pixels, 3L), Float.SIZE_BYTES.toLong())
        require(byteCount <= Int.MAX_VALUE) { "ticket #141 probe buffer exceeds ByteBuffer extent" }

        check(Build.VERSION.SDK_INT >= 27) { "ticket #141 SharedMemory probe requires API 27+" }
        // allocateDirect() is backed by a non-movable ART array on Android and cannot represent the
        // 600 MB 50 MP float image under a 512 MiB heap. Production receives JNI/native-owned image
        // memory, so map anonymous SharedMemory to exercise the same non-heap ByteBuffer contract.
        val sharedMemory = SharedMemory.create("ticket141-rgb", byteCount.toInt())
        val buffer = sharedMemory.mapReadWrite().order(ByteOrder.nativeOrder())
        try {
        val baselineInfo = currentMemoryInfo()
        val peakPssKiB = AtomicLong(baselineInfo.totalPss.toLong())
        val baselineRssKiB = currentRssKiB()
        val peakRssKiB = AtomicLong(baselineRssKiB)
        val sampling = AtomicBoolean(true)
        val sampler = Thread({
            while (sampling.get()) {
                val sample = currentMemoryInfo()
                peakPssKiB.accumulateAndGet(sample.totalPss.toLong()) { current, candidate -> maxOf(current, candidate) }
                peakRssKiB.accumulateAndGet(currentRssKiB()) { current, candidate -> maxOf(current, candidate) }
                try {
                    Thread.sleep(2L)
                } catch (_: InterruptedException) {
                    Thread.currentThread().interrupt()
                    return@Thread
                }
            }
        }, "ticket141-pss-sampler")

        val baselinePssKiB = baselineInfo.totalPss.toLong()
        val digests = ArrayList<String>(repeats)
        val durationsMs = ArrayList<Double>(repeats)
        var planEvidence = ""
        sampler.start()
        try {
            repeat(repeats) { iteration ->
                seed(buffer, pixels.toInt())
                val started = SystemClock.elapsedRealtimeNanos()
                val currentPlan = MaskCompositor.runTicket141MemoryProbe(
                    buffer, width, height, maskCount,
                )
                if (iteration == 0) planEvidence = currentPlan else check(currentPlan == planEvidence) {
                    "ticket #141 scratch plan changed between repeats"
                }
                durationsMs += (SystemClock.elapsedRealtimeNanos() - started) / 1_000_000.0
                digests += sha256(buffer)
                if (iteration > 0) check(digests[iteration] == digests[0]) {
                    "ticket #141 repeat digest changed"
                }
            }
        } finally {
            sampling.set(false)
            sampler.join(5_000L)
        }
        val peakPss = peakPssKiB.get()
        val peakRss = peakRssKiB.get()
        return buildString {
            append("TICKET141_MASK_MEMORY: RESULT\n")
            append("width=").append(width).append(" height=").append(height)
                .append(" pixels=").append(pixels).append(" masks=").append(maskCount)
                .append(" repeats=").append(repeats).append('\n')
            append(planEvidence).append(' ').append(budgetEvidence).append('\n')
            append("baseline_pss_kib=").append(baselinePssKiB)
                .append(" peak_pss_kib=").append(peakPss)
                .append(" delta_pss_kib=").append((peakPss - baselinePssKiB).coerceAtLeast(0L)).append('\n')
            append("baseline_rss_kib=").append(baselineRssKiB)
                .append(" peak_rss_kib=").append(peakRss)
                .append(" delta_rss_kib=").append((peakRss - baselineRssKiB).coerceAtLeast(0L)).append('\n')
            append("durations_ms=")
            var index = 0
            while (index < durationsMs.size) {
                if (index > 0) append(',')
                append(durationsMs[index])
                index++
            }
            append(" sha256=").append(digests[0]).append('\n')
            append("note=single-process release-device observation; output-buffer bytes are included; ")
            append("not a 12.5/50 MP qualification until both approved cells and graceful denial run\n")
        }
        } finally {
            SharedMemory.unmap(buffer)
            sharedMemory.close()
        }
    }

    @JvmStatic
    fun runForcedDenial(width: Int, height: Int, maskCount: Int): String {
        require(width > 0 && height > 0)
        require(maskCount in 1..16)
        val pixels = Math.multiplyExact(width.toLong(), height.toLong())
        val byteCount = Math.multiplyExact(Math.multiplyExact(pixels, 3L), Float.SIZE_BYTES.toLong())
        require(byteCount <= Int.MAX_VALUE)
        val buffer = ByteBuffer.allocateDirect(byteCount.toInt()).order(ByteOrder.nativeOrder())
        seed(buffer, pixels.toInt())
        val evidence = MaskCompositor.runTicket141ForcedDenialProbe(
            buffer, width, height, maskCount,
        )
        return "TICKET141_MASK_DENIAL: PASS width=$width height=$height masks=$maskCount " +
            "$evidence\n"
    }

    private fun seed(buffer: ByteBuffer, pixels: Int) {
        val floats = buffer.duplicate().order(ByteOrder.nativeOrder()).asFloatBuffer()
        var p = 0
        while (p < pixels) {
            val base = p * 3
            floats.put(base, ((p * 13L) % 997L).toFloat() / 997f)
            floats.put(base + 1, ((p * 29L + 7L) % 991L).toFloat() / 991f)
            floats.put(base + 2, ((p * 43L + 11L) % 983L).toFloat() / 983f)
            p++
        }
    }

    private fun sha256(buffer: ByteBuffer): String {
        val digest = MessageDigest.getInstance("SHA-256")
        digest.update(buffer.duplicate().apply { clear() })
        val bytes = digest.digest()
        val alphabet = "0123456789abcdef"
        val chars = CharArray(bytes.size * 2)
        for (index in bytes.indices) {
            val value = bytes[index].toInt() and 0xff
            chars[index * 2] = alphabet[value ushr 4]
            chars[index * 2 + 1] = alphabet[value and 0x0f]
        }
        return String(chars)
    }

    private fun currentMemoryInfo(): Debug.MemoryInfo {
        val info = Debug.MemoryInfo()
        Debug.getMemoryInfo(info)
        return info
    }

    /** Linux resident pages from /proc; compileSdk 34 has no Debug.MemoryInfo RSS accessor. */
    private fun currentRssKiB(): Long {
        val bytes = ByteArray(128)
        val input = FileInputStream("/proc/self/statm")
        val count = try {
            input.read(bytes)
        } finally {
            input.close()
        }
        check(count > 0) { "empty /proc/self/statm" }
        var index = 0
        while (index < count && bytes[index].toInt() > 0x20) index++ // virtual pages
        while (index < count && bytes[index].toInt() <= 0x20) index++
        var digits = 0
        var residentPages = 0L
        while (index < count) {
            val digit = bytes[index].toInt() - '0'.code
            if (digit !in 0..9) break
            residentPages = Math.addExact(Math.multiplyExact(residentPages, 10L), digit.toLong())
            digits++
            index++
        }
        check(digits > 0) {
            "missing resident pages in " + String(bytes, 0, count, StandardCharsets.US_ASCII)
        }
        return Math.multiplyExact(residentPages, pageSizeKiB)
    }
}
