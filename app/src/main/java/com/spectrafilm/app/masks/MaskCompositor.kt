/*
 * Spektrafilm for Android — bounded local-adjustment compositor. GPLv3.
 * Film modeling powered by spektrafilm.
 */
package com.spectrafilm.app.masks

import android.content.Context
import androidx.annotation.Keep
import com.spectrafilm.app.AppMemoryBudget
import com.spectrafilm.app.ContrastCurve
import com.spectrafilm.app.LocalWhiteBalance
import com.spectrafilm.app.MemoryBudgetBridge
import com.spectrafilm.app.MemoryBudgetDeniedException
import com.spectrafilm.app.Oklab
import com.spectrafilm.app.OutputCctf
import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.MemoryBudgetStage
import com.spectrafilm.engine.RenderCancellation
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.security.MessageDigest
import java.util.concurrent.CancellationException

internal enum class MaskCancellationPoint {
    ADMITTED,
    RASTER,
    LUMA,
    HORIZONTAL_PASS_1,
    VERTICAL_PASS_1,
    HORIZONTAL_PASS_2,
    VERTICAL_PASS_2,
    HORIZONTAL_PASS_3,
    VERTICAL_PASS_3,
    COMPOSE,
}

internal fun interface MaskCancellationSignal {
    fun isCancelled(point: MaskCancellationPoint): Boolean

    companion object {
        val NEVER = MaskCancellationSignal { false }
    }
}

internal class MaskCompositionCancelledException(val point: MaskCancellationPoint) :
    CancellationException("mask composition cancelled at ${point.name}")

internal fun MaskCancellationSignal.checkpoint(point: MaskCancellationPoint) {
    if (isCancelled(point)) throw MaskCompositionCancelledException(point)
}

internal data class MaskCompositorRuntime(
    val cancellation: MaskCancellationSignal = MaskCancellationSignal.NEVER,
    val memoryBridge: MemoryBudgetBridge? = null,
    val allocator: MaskFloatArrayAllocator? = null,
)

object MaskCompositor {

    /** Configure the process-wide coordinator without exposing app-internal policy types to test APKs. */
    @Keep
    @JvmStatic
    fun configureTicket141MemoryBudget(context: Context): String {
        val policy = AppMemoryBudget.configure(context)
        return "memory_tier=${policy.tier.name.lowercase()} physical_bytes=${policy.physicalBytes}"
    }

    /** Run the release fixture while keeping every app-internal model and plan behind one kept ABI. */
    @Keep
    @JvmStatic
    fun runTicket141MemoryProbe(
        data: ByteBuffer,
        w: Int,
        h: Int,
        maskCount: Int,
    ): String {
        require(maskCount in 1..16) { "ticket #141 mask count must be in [1,16]" }
        val maxEdge = maxOf(w, h).toFloat()
        val radii = intArrayOf(
            MaskSpatial.effectiveRadius(w, h, MaskSpatial.RADIUS_FRAC_SHARP * maxEdge),
            MaskSpatial.effectiveRadius(w, h, MaskSpatial.RADIUS_FRAC_TEXTURE * maxEdge),
            MaskSpatial.effectiveRadius(w, h, MaskSpatial.RADIUS_FRAC_CLARITY * maxEdge),
            MaskSpatial.effectiveRadius(w, h, MaskSpatial.RADIUS_FRAC_REGION * maxEdge),
        ).filter { it > 0 }.distinct().sorted().toIntArray()
        val plan = MaskScratchPlan.create(w, h, List(maskCount) { radii })
        val adjustments = List(maskCount) {
            LocalAdjustment(
                mask = Mask(invert = true),
                delta = TierADelta(
                    clarity = 31f,
                    texture = -23f,
                    sharpness = 37f,
                    highlights = -19f,
                    shadows = 29f,
                ),
            )
        }
        applyInPlace(data, w, h, ColorSpace.SRGB, true, adjustments)
        return "scratch_payload_bytes=${plan.floatPayloadBytes} " +
            "scratch_reserved_bytes=${plan.reservedBytes} max_radius=${plan.maxRadius}"
    }

    /**
     * Narrow cross-APK seam for the minified release memory gate. The instrumentation APK must not
     * construct app-internal runtime/allocator types: target R8 cannot see those external call edges.
     */
    @Keep
    @JvmStatic
    fun runTicket141ForcedDenialProbe(
        data: ByteBuffer,
        w: Int,
        h: Int,
        maskCount: Int,
    ): String {
        require(maskCount in 1..16) { "ticket #141 mask count must be in [1,16]" }
        val before = sha256(data)
        var allocations = 0
        var reserveCalls = 0
        var releaseCalls = 0
        val bridge = object : MemoryBudgetBridge {
            override fun configure(limitBytes: Long) = Unit
            override fun reserve(bytes: Long, stage: MemoryBudgetStage): Long {
                reserveCalls++
                return 0L
            }
            override fun release(token: Long): Boolean {
                releaseCalls++
                return false
            }
            override fun snapshotJson(): String = "{}"
        }
        val adjustments = List(maskCount) {
            LocalAdjustment(Mask(invert = true), TierADelta(clarity = 50f, highlights = -25f))
        }
        try {
            applyInPlace(
                data,
                w,
                h,
                ColorSpace.SRGB,
                true,
                adjustments,
                MaskCompositorRuntime(
                    memoryBridge = bridge,
                    allocator = MaskFloatArrayAllocator { size ->
                        allocations++
                        FloatArray(size)
                    },
                ),
            )
            error("forced SPATIAL denial unexpectedly succeeded")
        } catch (failure: MemoryBudgetDeniedException) {
            check(failure.stage == MemoryBudgetStage.SPATIAL)
        }
        check(allocations == 0) { "forced denial allocated $allocations scratch arrays" }
        check(sha256(data) == before) { "forced denial changed output pixels" }
        check(reserveCalls == 1 && releaseCalls == 0) { "forced denial ownership mismatch" }
        return "scratch_allocations=0 reserve_calls=1 release_calls=0 sha256=$before"
    }

    /**
     * Apply active adjustments in document order to interleaved float32 RGB. Mask alpha and all six
     * spatial passes are streamed through radius-bounded rows/rings; no full-frame alpha, luma, or
     * blur plane is retained. The source view begins at [ByteBuffer.position], while the caller's
     * position/limit/order remain unchanged.
     */
    fun applyInPlace(
        data: ByteBuffer,
        w: Int,
        h: Int,
        cs: ColorSpace,
        cctfEncoded: Boolean,
        adjustments: List<LocalAdjustment>,
    ) = applyInPlace(data, w, h, cs, cctfEncoded, adjustments, MaskCompositorRuntime())

    /**
     * Cancellable production route. Once a row has been composed, cancellation leaves [data]
     * partially modified: callers must supply an unpublished/private work buffer and discard it when
     * [MaskCompositionCancelledException] escapes.
     */
    fun applyInPlace(
        data: ByteBuffer,
        w: Int,
        h: Int,
        cs: ColorSpace,
        cctfEncoded: Boolean,
        adjustments: List<LocalAdjustment>,
        cancellation: RenderCancellation,
    ) = applyInPlace(
        data,
        w,
        h,
        cs,
        cctfEncoded,
        adjustments,
        MaskCompositorRuntime(
            cancellation = MaskCancellationSignal { cancellation.isCancellationRequested },
        ),
    )

    internal fun applyInPlace(
        data: ByteBuffer,
        w: Int,
        h: Int,
        cs: ColorSpace,
        cctfEncoded: Boolean,
        adjustments: List<LocalAdjustment>,
        runtime: MaskCompositorRuntime,
    ) {
        val image = checkedImageView(data, w, h)
        val active = adjustments.filter { hasOp(it.delta) && MaskRaster.coverage(it.mask) > 1e-4f }
        if (active.isEmpty()) return

        // Every allocation and numeric extent that can fail is resolved before the first output write.
        val prepared = active.map { prepare(it, w, h, cs) }
        val plan = MaskScratchPlan.create(w, h, prepared.map { it.positiveRadii })
        val scratch = if (runtime.allocator == null) {
            MaskTileScratch.open(plan, runtime.memoryBridge)
        } else {
            MaskTileScratch.open(plan, runtime.memoryBridge, runtime.allocator)
        }
        scratch.use {
            val streamByRadius = scratch.pipelines.associate { pipeline ->
                pipeline.radius to MaskTripleBoxStream(
                    w,
                    h,
                    pipeline.radius,
                    pipeline,
                    runtime.cancellation,
                )
            }
            val runs = prepared.map { item ->
                PreparedRun(item, item.positiveRadii.map { radius -> streamByRadius.getValue(radius) })
            }

            runtime.cancellation.checkpoint(MaskCancellationPoint.ADMITTED)

            for (run in runs) {
                applyPrepared(
                    image.floats,
                    w,
                    h,
                    cs,
                    cctfEncoded,
                    run,
                    scratch,
                    runtime.cancellation,
                )
            }
        }
    }

    private data class CheckedImage(val floats: FloatBuffer)

    private data class PreparedAdjustment(
        val adjustment: LocalAdjustment,
        val whiteBalance: FloatArray?,
        val gain: Float,
        val saturation: Float,
        val levels: Boolean,
        val blackPoint: Float,
        val inverseSpan: Float,
        val clarityRadius: Int,
        val textureRadius: Int,
        val sharpRadius: Int,
        val regionRadius: Int,
        val positiveRadii: IntArray,
    )

    private data class PreparedRun(
        val prepared: PreparedAdjustment,
        val streams: List<MaskTripleBoxStream>,
    ) {
        fun stream(radius: Int): MaskTripleBoxStream? =
            if (radius == 0) null else streams.first { it.radius == radius }
    }

    private fun checkedImageView(data: ByteBuffer, w: Int, h: Int): CheckedImage {
        require(w > 0 && h > 0) { "mask compositor dimensions must be positive" }
        require(!data.isReadOnly) { "mask compositor output is read-only" }
        val pixels = checkedMul(w.toLong(), h.toLong(), "mask compositor pixel extent overflow")
        val floats = checkedMul(pixels, 3L, "mask compositor channel extent overflow")
        val bytes = checkedMul(floats, Float.SIZE_BYTES.toLong(), "mask compositor byte extent overflow")
        require(pixels <= Int.MAX_VALUE.toLong()) { "mask compositor pixel extent exceeds JVM indexing" }
        require(bytes <= data.remaining().toLong()) {
            "mask compositor buffer is undersized: need $bytes bytes, have ${data.remaining()}"
        }
        val view = data.duplicate().order(ByteOrder.nativeOrder()).asFloatBuffer()
        require(floats <= view.remaining().toLong()) { "mask compositor float view is undersized" }
        return CheckedImage(view)
    }

    private fun checkedMul(a: Long, b: Long, message: String): Long = try {
        Math.multiplyExact(a, b)
    } catch (_: ArithmeticException) {
        throw IllegalArgumentException(message)
    }

    private fun prepare(
        adjustment: LocalAdjustment,
        w: Int,
        h: Int,
        cs: ColorSpace,
    ): PreparedAdjustment {
        val d = adjustment.delta
        val maxWH = maxOf(w, h).toFloat()
        val clarity = if (d.clarity != 0f) {
            MaskSpatial.effectiveRadius(w, h, MaskSpatial.RADIUS_FRAC_CLARITY * maxWH)
        } else 0
        val texture = if (d.texture != 0f) {
            MaskSpatial.effectiveRadius(w, h, MaskSpatial.RADIUS_FRAC_TEXTURE * maxWH)
        } else 0
        val sharp = if (d.sharpness != 0f) {
            MaskSpatial.effectiveRadius(w, h, MaskSpatial.RADIUS_FRAC_SHARP * maxWH)
        } else 0
        val region = if (d.highlights != 0f || d.shadows != 0f) {
            MaskSpatial.effectiveRadius(w, h, MaskSpatial.RADIUS_FRAC_REGION * maxWH)
        } else 0
        val levels = d.whites != 0f || d.blacks != 0f
        val blackPoint = -(d.blacks / 100f) * 0.25f
        val whitePoint = 1f - (d.whites / 100f) * 0.25f
        val radii = intArrayOf(clarity, texture, sharp, region).filter { it > 0 }.distinct().sorted().toIntArray()
        return PreparedAdjustment(
            adjustment = adjustment,
            whiteBalance = if (d.temp != 0f || d.tint != 0f) LocalWhiteBalance.matrix(cs, d.temp, d.tint) else null,
            gain = exposureGain(d.exposureEv),
            saturation = d.saturation / 100f,
            levels = levels,
            blackPoint = blackPoint,
            inverseSpan = if (levels) 1f / (whitePoint - blackPoint) else 1f,
            clarityRadius = clarity,
            textureRadius = texture,
            sharpRadius = sharp,
            regionRadius = region,
            positiveRadii = radii,
        )
    }

    private fun applyPrepared(
        f: FloatBuffer,
        w: Int,
        h: Int,
        cs: ColorSpace,
        cctfEncoded: Boolean,
        run: PreparedRun,
        scratch: MaskTileScratch,
        cancellation: MaskCancellationSignal,
    ) {
        val d = run.prepared.adjustment.delta
        if (!d.hasSpatial || run.streams.isEmpty()) {
            var y = 0
            while (y < h) {
                composeRow(f, w, h, y, cs, cctfEncoded, run, scratch, cancellation)
                y++
            }
            return
        }

        run.streams.forEach { it.reset() }
        val maxStream = run.streams.last()
        var nextOutput = 0
        var sourceY = 0
        while (sourceY < h) {
            fillLumaRow(f, w, sourceY, scratch.sourceLuma)
            cancellation.checkpoint(MaskCancellationPoint.LUMA)
            var maxEmitted = -1
            for (stream in run.streams) {
                val emitted = stream.feedOriginal(scratch.sourceLuma)
                if (stream === maxStream) maxEmitted = emitted
            }
            if (maxEmitted >= 0) {
                check(maxEmitted == nextOutput) { "mask blur output order changed" }
                composeRow(
                    f,
                    w,
                    h,
                    nextOutput,
                    cs,
                    cctfEncoded,
                    run,
                    scratch,
                    cancellation,
                )
                nextOutput++
            }
            sourceY++
        }

        while (!maxStream.complete) {
            var maxEmitted = -1
            for (stream in run.streams) {
                val emitted = stream.advanceBottom()
                if (stream === maxStream) maxEmitted = emitted
            }
            if (maxEmitted >= 0) {
                check(maxEmitted == nextOutput) { "mask blur flush order changed" }
                composeRow(
                    f,
                    w,
                    h,
                    nextOutput,
                    cs,
                    cctfEncoded,
                    run,
                    scratch,
                    cancellation,
                )
                nextOutput++
            }
        }
        check(nextOutput == h && run.streams.all { it.complete }) { "mask blur did not emit the full image" }
    }

    private fun fillLumaRow(f: FloatBuffer, w: Int, y: Int, out: FloatArray) {
        var x = 0
        var k = y * w * 3
        while (x < w) {
            out[x] = 0.2126f * f.get(k) + 0.7152f * f.get(k + 1) + 0.0722f * f.get(k + 2)
            x++
            k += 3
        }
    }

    private fun composeRow(
        f: FloatBuffer,
        w: Int,
        h: Int,
        y: Int,
        cs: ColorSpace,
        cctfEncoded: Boolean,
        run: PreparedRun,
        scratch: MaskTileScratch,
        cancellation: MaskCancellationSignal,
    ) {
        val item = run.prepared
        val adj = item.adjustment
        val d = adj.delta
        MaskRaster.rasterizeRow(adj.mask, w, h, y, scratch.alpha)
        cancellation.checkpoint(MaskCancellationPoint.RASTER)
        val lumRange = adj.mask.luminanceRange?.takeIf { it.isActive }
        val colorRange = adj.mask.colorRange?.takeIf { it.isActive }
        val clarityK = d.clarity / 100f * 1.2f
        val textureK = d.texture / 100f * 1.0f
        val sharpK = d.sharpness / 100f * 1.5f
        val highK = d.highlights / 100f * 0.35f
        val shadK = d.shadows / 100f * 0.35f
        val clarity = run.stream(item.clarityRadius)
        val texture = run.stream(item.textureRadius)
        val sharp = run.stream(item.sharpRadius)
        val region = run.stream(item.regionRadius)
        val rgb = scratch.rgb

        var x = 0
        var k = y * w * 3
        while (x < w) {
            var a = scratch.alpha[x]
            if (a > 0f) {
                val or = f.get(k)
                val og = f.get(k + 1)
                val ob = f.get(k + 2)
                val lp = 0.2126f * or + 0.7152f * og + 0.0722f * ob
                if (lumRange != null) a *= lumRange.gate(lp)
                if (a > 0f && colorRange != null) a *= colorRange.gate(or, og, ob)
                if (a > 0f) {
                    rgb[0] = OutputCctf.decode(cs, or, cctfEncoded) * item.gain
                    rgb[1] = OutputCctf.decode(cs, og, cctfEncoded) * item.gain
                    rgb[2] = OutputCctf.decode(cs, ob, cctfEncoded) * item.gain
                    val wbM = item.whiteBalance
                    if (wbM != null) {
                        val r = rgb[0]
                        val g = rgb[1]
                        val b = rgb[2]
                        rgb[0] = wbM[0] * r + wbM[1] * g + wbM[2] * b
                        rgb[1] = wbM[3] * r + wbM[4] * g + wbM[5] * b
                        rgb[2] = wbM[6] * r + wbM[7] * g + wbM[8] * b
                    }
                    if (item.saturation != 0f) Oklab.scaleChromaLinear(rgb, item.saturation, 0f)
                    if (d.hue != 0f) Oklab.rotateHueLinear(rgb, d.hue)
                    var er = OutputCctf.encode(cs, rgb[0], cctfEncoded)
                    var eg = OutputCctf.encode(cs, rgb[1], cctfEncoded)
                    var eb = OutputCctf.encode(cs, rgb[2], cctfEncoded)
                    if (d.contrast != 0f) {
                        er = ContrastCurve.curveAt(er, d.contrast)
                        eg = ContrastCurve.curveAt(eg, d.contrast)
                        eb = ContrastCurve.curveAt(eb, d.contrast)
                    }
                    if (item.levels) {
                        er = ((er - item.blackPoint) * item.inverseSpan).coerceIn(0f, 1f)
                        eg = ((eg - item.blackPoint) * item.inverseSpan).coerceIn(0f, 1f)
                        eb = ((eb - item.blackPoint) * item.inverseSpan).coerceIn(0f, 1f)
                    }
                    if (d.hasSpatial) {
                        var dL = 0f
                        if (d.clarity != 0f) {
                            val blurred = clarity?.outputAt(y, x) ?: lp
                            dL += clarityK * (lp - blurred) * MaskSpatial.midtoneWeight(lp)
                        }
                        if (d.texture != 0f) {
                            val blurred = texture?.outputAt(y, x) ?: lp
                            dL += textureK * (lp - blurred)
                        }
                        if (d.sharpness != 0f) {
                            val blurred = sharp?.outputAt(y, x) ?: lp
                            dL += sharpK * (lp - blurred)
                        }
                        if (d.highlights != 0f || d.shadows != 0f) {
                            val blurred = region?.outputAt(y, x) ?: lp
                            if (highK != 0f) dL += highK * smoothstep((blurred - 0.5f) / 0.5f)
                            if (shadK != 0f) dL += shadK * smoothstep((0.5f - blurred) / 0.5f)
                        }
                        if (dL != 0f && lp > 1e-4f) {
                            val ratio = (lp + dL).coerceIn(0f, 1f) / lp
                            er = (er * ratio).coerceIn(0f, 1f)
                            eg = (eg * ratio).coerceIn(0f, 1f)
                            eb = (eb * ratio).coerceIn(0f, 1f)
                        }
                    }
                    f.put(k, or + a * (er - or))
                    f.put(k + 1, og + a * (eg - og))
                    f.put(k + 2, ob + a * (eb - ob))
                }
            }
            x++
            k += 3
        }
        cancellation.checkpoint(MaskCancellationPoint.COMPOSE)
    }

    private fun hasOp(d: TierADelta): Boolean = !d.isNoOp

    private fun exposureGain(ev: Float): Float =
        if (ev == 0f) 1f else Math.pow(2.0, ev.toDouble()).toFloat()

    private fun sha256(buffer: ByteBuffer): String {
        val digest = MessageDigest.getInstance("SHA-256")
        digest.update(buffer.duplicate().apply { clear() })
        return digest.digest().joinToString("") { byte -> "%02x".format(byte.toInt() and 0xff) }
    }
}
