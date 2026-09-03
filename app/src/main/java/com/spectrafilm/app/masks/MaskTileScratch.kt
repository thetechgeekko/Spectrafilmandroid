/*
 * Spektrafilm for Android — bounded scratch ownership for mask composition. GPLv3.
 */
package com.spectrafilm.app.masks

import com.spectrafilm.app.JvmMemoryReservation
import com.spectrafilm.app.MemoryBudgetBridge
import com.spectrafilm.engine.MemoryBudgetStage
import java.util.concurrent.atomic.AtomicBoolean

internal fun interface MaskFloatArrayAllocator {
    fun allocate(size: Int): FloatArray
}

internal data class MaskRadiusDemand(
    val radius: Int,
    val delayRows: Int,
)

internal data class MaskScratchPlan(
    val width: Int,
    val height: Int,
    val demands: List<MaskRadiusDemand>,
    val floatPayloadBytes: Long,
    val reservedBytes: Long,
    val arrayCount: Int,
    val fullFramePlaneBytes: Long,
) {
    val maxRadius: Int = demands.maxOfOrNull { it.radius } ?: 0

    companion object {
        private const val ARRAY_ACCOUNTING_BYTES = 32L
        private const val OWNER_ACCOUNTING_BYTES = 4096L

        /**
         * Build a checked worst-case plan for every radius combination used by the adjustment stack.
         * A radius appears once and receives the largest output delay it needs in any adjustment.
         */
        fun create(width: Int, height: Int, adjustmentRadii: List<IntArray>): MaskScratchPlan {
            require(width > 0 && height > 0) { "mask scratch dimensions must be positive" }
            val delayByRadius = sortedMapOf<Int, Int>()
            for (radii in adjustmentRadii) {
                val positive = radii.filter { it > 0 }.distinct().sorted()
                val maxRadius = positive.lastOrNull() ?: 0
                for (radius in positive) {
                    val delay = checkedInt(checkedAdd(checkedMul(3L, (maxRadius - radius).toLong()), 1L))
                    delayByRadius[radius] = maxOf(delayByRadius[radius] ?: 0, delay)
                }
            }
            val demands = delayByRadius.map { MaskRadiusDemand(it.key, it.value) }

            // Shared source-luma and alpha rows plus the three-value Oklab work vector.
            var floats = checkedAdd(checkedMul(2L, width.toLong()), 3L)
            var arrays = 3
            for (demand in demands) {
                val ringRows = minOf(height.toLong(), checkedAdd(checkedMul(2L, demand.radius.toLong()), 1L))
                val ringFloats = checkedMul(checkedMul(3L, ringRows), width.toLong())
                val sumFloats = checkedMul(3L, width.toLong())
                val workFloats = checkedMul(6L, width.toLong())
                val delayFloats = checkedMul(demand.delayRows.toLong(), width.toLong())
                floats = checkedAdd(floats, checkedAdd(checkedAdd(ringFloats, sumFloats), checkedAdd(workFloats, delayFloats)))
                arrays = Math.addExact(arrays, 13) // 3 rings + 3 sums + 6 rows + one output-delay ring

                checkedArraySize(checkedMul(ringRows, width.toLong()))
                checkedArraySize(delayFloats)
            }
            checkedArraySize(width.toLong())
            val payload = checkedMul(floats, Float.SIZE_BYTES.toLong())
            val fullFramePlane = checkedMul(
                checkedMul(width.toLong(), height.toLong()),
                Float.SIZE_BYTES.toLong(),
            )
            val accounted = checkedAdd(
                payload,
                checkedAdd(checkedMul(arrays.toLong(), ARRAY_ACCOUNTING_BYTES), OWNER_ACCOUNTING_BYTES),
            )
            return MaskScratchPlan(width, height, demands, payload, accounted, arrays, fullFramePlane)
        }

        private fun checkedArraySize(value: Long): Int {
            require(value in 1..Int.MAX_VALUE.toLong()) { "mask scratch array exceeds JVM extent" }
            return value.toInt()
        }

        private fun checkedInt(value: Long): Int {
            require(value in 1..Int.MAX_VALUE.toLong()) { "mask scratch row extent overflow" }
            return value.toInt()
        }

        private fun checkedAdd(a: Long, b: Long): Long = try {
            Math.addExact(a, b)
        } catch (_: ArithmeticException) {
            throw IllegalArgumentException("mask scratch byte extent overflow")
        }

        private fun checkedMul(a: Long, b: Long): Long = try {
            Math.multiplyExact(a, b)
        } catch (_: ArithmeticException) {
            throw IllegalArgumentException("mask scratch byte extent overflow")
        }
    }
}

internal class MaskPipelineScratch private constructor(
    val radius: Int,
    val delayRows: Int,
    val rings: Array<FloatArray>,
    val sums: Array<FloatArray>,
    val rows: Array<FloatArray>,
    val outputDelay: FloatArray,
) {
    companion object {
        fun allocate(
            plan: MaskScratchPlan,
            demand: MaskRadiusDemand,
            allocator: MaskFloatArrayAllocator,
        ): MaskPipelineScratch {
            val ringRows = minOf(plan.height.toLong(), 2L * demand.radius + 1L)
            val ringSize = Math.multiplyExact(ringRows, plan.width.toLong()).toInt()
            val rings = Array(3) { allocator.allocate(ringSize) }
            val sums = Array(3) { allocator.allocate(plan.width) }
            val rows = Array(6) { allocator.allocate(plan.width) }
            val outputDelay = allocator.allocate(Math.multiplyExact(demand.delayRows, plan.width))
            return MaskPipelineScratch(demand.radius, demand.delayRows, rings, sums, rows, outputDelay)
        }
    }
}

/** Exact-once reservation owner for all reusable managed mask scratch. */
internal class MaskTileScratch private constructor(
    val plan: MaskScratchPlan,
    private val reservation: AutoCloseable,
    val sourceLuma: FloatArray,
    val alpha: FloatArray,
    val rgb: FloatArray,
    val pipelines: List<MaskPipelineScratch>,
) : AutoCloseable {
    private val closed = AtomicBoolean(false)

    fun pipeline(radius: Int): MaskPipelineScratch =
        pipelines.firstOrNull { it.radius == radius }
            ?: error("mask pipeline radius $radius was not reserved")

    override fun close() {
        if (closed.compareAndSet(false, true)) reservation.close()
    }

    companion object {
        private val DEFAULT_ALLOCATOR = MaskFloatArrayAllocator(::FloatArray)

        fun open(
            plan: MaskScratchPlan,
            bridge: MemoryBudgetBridge? = null,
            allocator: MaskFloatArrayAllocator = DEFAULT_ALLOCATOR,
        ): MaskTileScratch {
            val reservation: AutoCloseable = if (bridge != null) {
                JvmMemoryReservation.acquire(plan.reservedBytes, MemoryBudgetStage.SPATIAL, bridge)
            } else if (System.getProperty("java.vm.name") == "Dalvik") {
                // ART reports the historical VM name "Dalvik". Android must always use the native
                // process-wide coordinator; a missing JNI bridge is a hard failure, never a bypass.
                JvmMemoryReservation.acquire(plan.reservedBytes, MemoryBudgetStage.SPATIAL)
            } else {
                // Plain-JVM golden tests have no Android JNI library. They exercise the same checked
                // plan/allocator, while explicit fake bridges cover denial and exact-once ownership.
                AutoCloseable { }
            }
            return try {
                val source = allocator.allocate(plan.width)
                val alpha = allocator.allocate(plan.width)
                val rgb = allocator.allocate(3)
                val pipelines = plan.demands.map { MaskPipelineScratch.allocate(plan, it, allocator) }
                MaskTileScratch(plan, reservation, source, alpha, rgb, pipelines)
            } catch (failure: Throwable) {
                try {
                    reservation.close()
                } catch (releaseFailure: Throwable) {
                    failure.addSuppressed(releaseFailure)
                }
                throw failure
            }
        }
    }
}
