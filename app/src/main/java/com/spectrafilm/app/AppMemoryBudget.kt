/*
 * Spektrafilm for Android. GPLv3.
 * Film modeling powered by spektrafilm (GPLv3).
 */
package com.spectrafilm.app

import android.app.ActivityManager
import android.content.Context
import com.spectrafilm.engine.MemoryBudgetStage
import com.spectrafilm.engine.SpektraEngine
import java.util.concurrent.atomic.AtomicLong

internal enum class DeviceMemoryTier(val budgetBytes: Long) {
    COMPACT(512L * MIB),
    STANDARD(768L * MIB),
    LARGE(1_024L * MIB),
    XLARGE(1_536L * MIB),
}

internal data class DeviceMemoryPolicy(
    val tier: DeviceMemoryTier,
    val physicalBytes: Long,
)

/**
 * Select a conservative process-wide working-set ceiling from physical RAM.
 *
 * This deliberately does not use Android's `largeMemoryClass`: that is an ART heap allowance, not
 * evidence that native, GPU, writer, Bitmap, and JVM allocations can all grow independently. The
 * shared native coordinator is the single admission authority for those domains.
 */
internal fun deviceMemoryPolicy(physicalBytes: Long): DeviceMemoryPolicy {
    val tier = when {
        physicalBytes <= 0L -> DeviceMemoryTier.COMPACT
        physicalBytes <= 4L * GIB -> DeviceMemoryTier.COMPACT
        physicalBytes <= 6L * GIB -> DeviceMemoryTier.STANDARD
        physicalBytes <= 8L * GIB -> DeviceMemoryTier.LARGE
        else -> DeviceMemoryTier.XLARGE
    }
    return DeviceMemoryPolicy(tier, physicalBytes.coerceAtLeast(0L))
}

internal interface MemoryBudgetBridge {
    fun configure(limitBytes: Long)
    fun reserve(bytes: Long, stage: MemoryBudgetStage): Long
    fun release(token: Long): Boolean
    fun snapshotJson(): String
}

private object NativeMemoryBudgetBridge : MemoryBudgetBridge {
    override fun configure(limitBytes: Long) = SpektraEngine.configureMemoryBudget(limitBytes)
    override fun reserve(bytes: Long, stage: MemoryBudgetStage): Long =
        SpektraEngine.reserveJvmMemory(bytes, stage)

    override fun release(token: Long): Boolean = SpektraEngine.releaseJvmMemory(token)
    override fun snapshotJson(): String = SpektraEngine.memoryBudgetSnapshotJson()
}

internal class MemoryBudgetDeniedException(
    val requestedBytes: Long,
    val stage: MemoryBudgetStage,
) : OutOfMemoryError("memory budget denied $requestedBytes bytes at ${stage.name}")

/** Exact-once owner for bytes allocated outside the native coordinator (Bitmap/ART/GPU wrappers). */
internal class JvmMemoryReservation private constructor(
    token: Long,
    val bytes: Long,
    val stage: MemoryBudgetStage,
    private val bridge: MemoryBudgetBridge,
) : AutoCloseable {
    private val liveToken = AtomicLong(token)

    override fun close() {
        val token = liveToken.getAndSet(0L)
        if (token != 0L) check(bridge.release(token)) { "memory reservation token was not live" }
    }

    internal companion object {
        fun acquire(
            bytes: Long,
            stage: MemoryBudgetStage,
            bridge: MemoryBudgetBridge = NativeMemoryBudgetBridge,
            construct: (
                token: Long,
                bytes: Long,
                stage: MemoryBudgetStage,
                bridge: MemoryBudgetBridge,
            ) -> JvmMemoryReservation = ::JvmMemoryReservation,
        ): JvmMemoryReservation {
            require(bytes > 0L) { "memory reservation must be positive" }
            val token = bridge.reserve(bytes, stage)
            if (token == 0L) throw MemoryBudgetDeniedException(bytes, stage)
            return try {
                construct(token, bytes, stage, bridge)
            } catch (failure: Throwable) {
                try {
                    if (!bridge.release(token)) {
                        failure.addSuppressed(
                            IllegalStateException("admitted memory reservation token was not live"),
                        )
                    }
                } catch (releaseFailure: Throwable) {
                    failure.addSuppressed(releaseFailure)
                }
                throw failure
            }
        }
    }
}

/** Configures the process authority exactly once, before the shared engine is constructed. */
internal object AppMemoryBudget {
    @Volatile private var configuredPolicy: DeviceMemoryPolicy? = null

    fun configure(context: Context): DeviceMemoryPolicy {
        configuredPolicy?.let { return it }
        return synchronized(this) {
            configuredPolicy ?: run {
                val manager = context.applicationContext
                    .getSystemService(ActivityManager::class.java)
                val info = ActivityManager.MemoryInfo()
                manager?.getMemoryInfo(info)
                val policy = deviceMemoryPolicy(info.totalMem)
                NativeMemoryBudgetBridge.configure(policy.tier.budgetBytes)
                Diag.i(
                    "memory budget tier=${policy.tier.name.lowercase()} " +
                        "limit=${policy.tier.budgetBytes} physical=${policy.physicalBytes} " +
                        "snapshot=${NativeMemoryBudgetBridge.snapshotJson()}",
                )
                configuredPolicy = policy
                policy
            }
        }
    }
}

private const val MIB = 1_048_576L
private const val GIB = 1_073_741_824L
