/*
 * Spektrafilm for Android — ticket #141 bounded mask-compositor regression tests. GPLv3.
 */
package com.spectrafilm.app.masks

import com.spectrafilm.app.MemoryBudgetBridge
import com.spectrafilm.app.ContrastCurve
import com.spectrafilm.app.LocalWhiteBalance
import com.spectrafilm.app.Oklab
import com.spectrafilm.app.OutputCctf
import com.spectrafilm.app.MemoryBudgetDeniedException
import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.MemoryBudgetStage
import com.spectrafilm.engine.RenderCancellation
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

class MaskTiledCompositorTest {
    @Test
    fun stripedRasterMatchesTheFrozenFullFrameRaster() {
        val w = 37
        val h = 29
        val mask = Mask(
            components = listOf(
                Mask.Component(
                    BlendMode.ADD,
                    MaskComponent.Radial(0.47f, 0.53f, 0.31f, 0.24f, 0.73f, 17f),
                ),
                Mask.Component(
                    BlendMode.SUBTRACT,
                    MaskComponent.Linear(0.13f, 0.07f, 0.91f, 0.88f),
                    invert = true,
                    value = 0.42f,
                ),
            ),
            invert = true,
            opacity = 0.81f,
        )
        val expected = MaskRaster.rasterize(mask, w, h)
        val actual = FloatArray(w * h)
        val row = FloatArray(w)
        for (y in 0 until h) {
            MaskRaster.rasterizeRow(mask, w, h, y, row)
            row.copyInto(actual, y * w)
        }
        assertArrayEquals(expected, actual, 0f)
    }

    @Test
    fun radialFeatherClampMatrixIsFiniteAndBitExactAcrossRowBoundaries() {
        val w = 11
        val h = 9
        val cases = listOf(
            0f to 1e-3f,
            Float.MIN_VALUE to 1e-3f,
            1e-3f to 1e-3f,
            1f to 1f,
            2f to 1f,
            Float.NEGATIVE_INFINITY to 1e-3f,
            Float.POSITIVE_INFINITY to 1f,
            Float.NaN to null,
        )
        for ((inputFeather, effectiveFeather) in cases) {
            fun radial(feather: Float) = Mask(
                components = listOf(
                    Mask.Component(
                        BlendMode.ADD,
                        MaskComponent.Radial(0.43f, 0.57f, 0.37f, 0.29f, feather, 31f),
                    ),
                ),
            )

            val expected = effectiveFeather?.let { MaskRaster.rasterize(radial(it), w, h) }
                ?: FloatArray(w * h) // non-finite metadata fails closed instead of poisoning RGB
            val full = MaskRaster.rasterize(radial(inputFeather), w, h)
            assertArrayEquals("full feather=$inputFeather", expected, full, 0f)

            val streamed = FloatArray(w * h)
            val row = FloatArray(w)
            for (y in 0 until h) {
                MaskRaster.rasterizeRow(radial(inputFeather), w, h, y, row)
                row.copyInto(streamed, y * w)
            }
            assertArrayEquals("row feather=$inputFeather", expected, streamed, 0f)
            assertTrue("finite feather=$inputFeather", streamed.all(Float::isFinite))
        }
    }

    @Test
    fun streamedThreeBoxBlurIsBitExactToFrozenLegacyAcrossPatternsAndBoundaries() {
        val dimensions = listOf(37 to 29, 17 to 11, 8 to 7)
        val radii = listOf(1, 2, 7, 19)
        for ((w, h) in dimensions) {
            val patterns = listOf(
                FloatArray(w * h).also { it[(h / 2) * w + w / 2] = 1f },
                FloatArray(w * h) { i -> if (((i / w) + i % w) and 1 == 0) 1f else 0f },
                FloatArray(w * h) { i -> ((i / w) * 0.017f + (i % w) * 0.031f) % 1f },
            )
            for (radius in radii) {
                for (source in patterns) {
                    val expected = frozenLegacyBlur(source, w, h, radius)
                    val actual = streamedBlur(source, w, h, radius)
                    assertArrayEquals("${w}x$h radius=$radius", expected, actual, 0f)
                }
            }
        }
    }

    @Test
    fun streamedThreeBoxBlurPinsGlobalEdgeClampingForHugeRadius() {
        val w = 7
        val h = 5
        val source = FloatArray(w * h) { i -> (i * 13 % 29) / 29f }
        assertArrayEquals(frozenLegacyBlur(source, w, h, 31), streamedBlur(source, w, h, 31), 0f)
    }

    @Test
    fun concurrentRadiiDelayRingsRetainTheExactSameGlobalRow() {
        val w = 37
        val h = 29
        val radii = intArrayOf(1, 2, 7)
        val source = FloatArray(w * h) { i -> ((i * 47L + i / w * 11L) % 1009L).toFloat() / 1009f }
        val expected = radii.associateWith { frozenLegacyBlur(source, w, h, it) }
        val bridge = RecordingBridge()
        val plan = MaskScratchPlan.create(w, h, listOf(radii))
        MaskTileScratch.open(plan, bridge).use { scratch ->
            val streams = radii.map { MaskTripleBoxStream(w, h, it, scratch.pipeline(it)).also { s -> s.reset() } }
            val maxStream = streams.last()
            var checkedRows = 0
            for (y in 0 until h) {
                source.copyInto(scratch.sourceLuma, 0, y * w, (y + 1) * w)
                var emitted = -1
                for (stream in streams) {
                    val candidate = stream.feedOriginal(scratch.sourceLuma)
                    if (stream === maxStream) emitted = candidate
                }
                if (emitted >= 0) {
                    assertDelayedRows(streams, expected, emitted, w)
                    checkedRows++
                }
            }
            while (!maxStream.complete) {
                var emitted = -1
                for (stream in streams) {
                    val candidate = stream.advanceBottom()
                    if (stream === maxStream) emitted = candidate
                }
                if (emitted >= 0) {
                    assertDelayedRows(streams, expected, emitted, w)
                    checkedRows++
                }
            }
            assertEquals(h, checkedRows)
            assertTrue(streams.all { it.complete })
        }
        assertEquals(1, bridge.releaseCalls)
    }

    @Test
    fun scratchPlanIsRadiusBoundedAndReusedAcrossStackedMasks() {
        val w = 8192
        val h = 6104 // 50,003,968 pixels
        val radii = intArrayOf(12, 49, 245, 410)
        val oneMask = MaskScratchPlan.create(w, h, listOf(radii))
        val stacked = MaskScratchPlan.create(w, h, List(12) { radii })
        assertEquals(oneMask.floatPayloadBytes, stacked.floatPayloadBytes)
        assertEquals(oneMask.reservedBytes, stacked.reservedBytes)
        assertTrue("scratch must stay below two full-frame float planes", oneMask.reservedBytes < 2L * oneMask.fullFramePlaneBytes)
        assertEquals(410, oneMask.maxRadius)
        assertTrue(oneMask.arrayCount > 0)
    }

    @Test
    fun budgetDenialOccursBeforeAnyScratchAllocation() {
        val bridge = RecordingBridge(deny = true)
        var allocations = 0
        val allocator = MaskFloatArrayAllocator { size -> allocations++; FloatArray(size) }
        val plan = MaskScratchPlan.create(32, 24, listOf(intArrayOf(2, 5)))
        try {
            MaskTileScratch.open(plan, bridge, allocator)
            fail("budget denial must throw")
        } catch (_: OutOfMemoryError) {
            // expected
        }
        assertEquals(0, allocations)
        assertEquals(1, bridge.reserveCalls)
        assertEquals(0, bridge.releaseCalls)
        assertEquals(MemoryBudgetStage.SPATIAL, bridge.lastStage)
        assertEquals(plan.reservedBytes, bridge.lastBytes)
    }

    @Test
    fun allocationFailureReleasesAdmissionExactlyOnce() {
        val bridge = RecordingBridge()
        var allocations = 0
        val allocator = MaskFloatArrayAllocator { size ->
            allocations++
            if (allocations == 6) throw OutOfMemoryError("injected scratch allocation failure")
            FloatArray(size)
        }
        val plan = MaskScratchPlan.create(32, 24, listOf(intArrayOf(2, 5)))
        try {
            MaskTileScratch.open(plan, bridge, allocator)
            fail("allocation failure must escape")
        } catch (failure: OutOfMemoryError) {
            assertEquals("injected scratch allocation failure", failure.message)
        }
        assertEquals(1, bridge.reserveCalls)
        assertEquals(1, bridge.releaseCalls)
        assertEquals(0, bridge.liveTokens)
    }

    @Test
    fun repeatedScratchCleanupReleasesExactlyOnce() {
        val bridge = RecordingBridge()
        val plan = MaskScratchPlan.create(19, 13, listOf(intArrayOf(1, 3)))
        val scratch = MaskTileScratch.open(plan, bridge)
        scratch.close()
        scratch.close()
        assertEquals(1, bridge.reserveCalls)
        assertEquals(1, bridge.releaseCalls)
        assertEquals(0, bridge.liveTokens)
    }

    @Test
    fun scratchExtentRejectsAnIndividualArrayOverflowBeforeReservation() {
        try {
            MaskScratchPlan.create(Int.MAX_VALUE, 2, listOf(intArrayOf(1)))
            fail("array overflow must be rejected")
        } catch (expected: IllegalArgumentException) {
            assertTrue(expected.message.orEmpty().contains("extent"))
        }
    }

    @Test
    fun productionCompositorIsBitExactToFrozenLegacyForAllControlsAndStackedMasks() {
        val w = 673
        val h = 41
        val source = rgbBuffer(w, h)
        val adjustments = richAdjustments()
        val expected = cloneBuffer(source)
        frozenLegacyComposite(expected, w, h, ColorSpace.SRGB, true, adjustments)
        val actual = cloneBuffer(source)
        MaskCompositor.applyInPlace(actual, w, h, ColorSpace.SRGB, true, adjustments)
        assertArrayEquals(bufferBytes(expected), bufferBytes(actual))
    }

    @Test
    fun nonZeroPositionIsAnExactImageOriginAndPreservesCallerStateAndGuards() {
        val w = 71
        val h = 19
        val imageBytes = w * h * 3 * Float.SIZE_BYTES
        val prefix = 20
        val suffix = 28
        val container = ByteBuffer.allocateDirect(prefix + imageBytes + suffix).order(ByteOrder.BIG_ENDIAN)
        for (i in 0 until container.capacity()) container.put(i, 0x5a.toByte())
        val source = rgbBuffer(w, h)
        source.duplicate().clear()
        val imageSource = source.duplicate()
        container.position(prefix)
        container.put(imageSource)
        container.position(prefix)
        container.limit(prefix + imageBytes)
        val originalOrder = container.order()
        val expected = cloneBuffer(source)
        val adjustments = richAdjustments().take(1)
        frozenLegacyComposite(expected, w, h, ColorSpace.SRGB, true, adjustments)
        val zeroOrigin = cloneBuffer(source)
        MaskCompositor.applyInPlace(zeroOrigin, w, h, ColorSpace.SRGB, true, adjustments)
        assertArrayEquals("zero-origin production must match the frozen legacy oracle", bufferBytes(expected), bufferBytes(zeroOrigin))

        MaskCompositor.applyInPlace(container, w, h, ColorSpace.SRGB, true, adjustments)

        assertEquals(prefix, container.position())
        assertEquals(prefix + imageBytes, container.limit())
        assertEquals(originalOrder, container.order())
        for (i in 0 until prefix) assertEquals(0x5a.toByte(), container.get(i))
        container.limit(container.capacity())
        for (i in prefix + imageBytes until container.capacity()) assertEquals(0x5a.toByte(), container.get(i))
        val actual = ByteArray(imageBytes)
        container.duplicate().apply { position(prefix); limit(prefix + imageBytes) }.get(actual)
        assertArrayEquals(bufferBytes(expected), actual)
    }

    @Test
    fun invalidOverflowReadOnlyAndUndersizedInputsFailBeforeMutationOrAdmission() {
        val active = listOf(LocalAdjustment(Mask(invert = true), TierADelta(clarity = 20f)))
        val cases = listOf<() -> Unit>(
            { MaskCompositor.applyInPlace(ByteBuffer.allocate(12), 0, 1, ColorSpace.SRGB, true, active) },
            { MaskCompositor.applyInPlace(ByteBuffer.allocate(12), Int.MAX_VALUE, Int.MAX_VALUE, ColorSpace.SRGB, true, active) },
            { MaskCompositor.applyInPlace(ByteBuffer.allocate(11), 1, 1, ColorSpace.SRGB, true, active) },
            { MaskCompositor.applyInPlace(ByteBuffer.allocate(12).asReadOnlyBuffer(), 1, 1, ColorSpace.SRGB, true, active) },
        )
        for (invoke in cases) {
            try {
                invoke()
                fail("invalid compositor input must fail")
            } catch (_: IllegalArgumentException) {
                // expected
            }
        }
    }

    @Test
    fun compositorBudgetDenialLeavesPixelsUntouchedAndAllocatesNothing() {
        val w = 67
        val h = 43
        val buffer = rgbBuffer(w, h)
        val before = bufferBytes(buffer)
        val bridge = RecordingBridge(deny = true)
        var allocations = 0
        try {
            MaskCompositor.applyInPlace(
                buffer, w, h, ColorSpace.SRGB, true, richAdjustments(),
                MaskCompositorRuntime(
                    memoryBridge = bridge,
                    allocator = MaskFloatArrayAllocator { size -> allocations++; FloatArray(size) },
                ),
            )
            fail("budget denial must throw")
        } catch (_: MemoryBudgetDeniedException) {
            // expected
        }
        assertArrayEquals(before, bufferBytes(buffer))
        assertEquals(0, allocations)
        assertEquals(1, bridge.reserveCalls)
        assertEquals(0, bridge.releaseCalls)
    }

    @Test
    fun minifiedReleaseDenialBridgeUsesOnlyItsNarrowStableAbi() {
        val buffer = rgbBuffer(37, 29)
        val before = bufferBytes(buffer)
        val evidence = MaskCompositor.runTicket141ForcedDenialProbe(buffer, 37, 29, 4)
        assertArrayEquals(before, bufferBytes(buffer))
        assertTrue(evidence.contains("scratch_allocations=0"))
        assertTrue(evidence.contains("reserve_calls=1 release_calls=0"))
        assertTrue(evidence.contains("sha256="))
    }

    @Test
    fun compositorAllocationFailureLeavesPixelsUntouchedAndReleasesExactlyOnce() {
        val w = 67
        val h = 43
        val buffer = rgbBuffer(w, h)
        val before = bufferBytes(buffer)
        val bridge = RecordingBridge()
        var allocations = 0
        try {
            MaskCompositor.applyInPlace(
                buffer, w, h, ColorSpace.SRGB, true, richAdjustments(),
                MaskCompositorRuntime(
                    memoryBridge = bridge,
                    allocator = MaskFloatArrayAllocator { size ->
                        allocations++
                        if (allocations == 8) throw OutOfMemoryError("injected compositor scratch failure")
                        FloatArray(size)
                    },
                ),
            )
            fail("allocation failure must throw")
        } catch (failure: OutOfMemoryError) {
            assertEquals("injected compositor scratch failure", failure.message)
        }
        assertArrayEquals(before, bufferBytes(buffer))
        assertEquals(1, bridge.reserveCalls)
        assertEquals(1, bridge.releaseCalls)
        assertEquals(0, bridge.liveTokens)
    }

    @Test
    fun cancellationDuringEveryWorkStageReleasesExactlyOnceAndRequiresDiscard() {
        val w = 67
        val h = 43
        val workPoints = listOf(
            MaskCancellationPoint.RASTER,
            MaskCancellationPoint.LUMA,
            MaskCancellationPoint.HORIZONTAL_PASS_1,
            MaskCancellationPoint.VERTICAL_PASS_1,
            MaskCancellationPoint.HORIZONTAL_PASS_2,
            MaskCancellationPoint.VERTICAL_PASS_2,
            MaskCancellationPoint.HORIZONTAL_PASS_3,
            MaskCancellationPoint.VERTICAL_PASS_3,
            MaskCancellationPoint.COMPOSE,
        )
        val adjustment = LocalAdjustment(
            mask = Mask(invert = true),
            delta = TierADelta(exposureEv = 0.5f, clarity = 40f),
        )
        for (point in workPoints) {
            val buffer = rgbBuffer(w, h)
            val before = bufferBytes(buffer)
            val bridge = RecordingBridge()
            var progressChecks = 0
            try {
                MaskCompositor.applyInPlace(
                    buffer, w, h, ColorSpace.SRGB, true, listOf(adjustment),
                    MaskCompositorRuntime(
                        cancellation = MaskCancellationSignal { observed ->
                            if (observed != point) return@MaskCancellationSignal false
                            progressChecks++
                            progressChecks == 12
                        },
                        memoryBridge = bridge,
                    ),
                )
                fail("cancellation at $point must throw")
            } catch (failure: MaskCompositionCancelledException) {
                assertEquals(point, failure.point)
            }
            assertEquals("work did not progress at $point", 12, progressChecks)
            assertTrue(
                "the unpublished work buffer must be discarded after $point cancellation",
                !before.contentEquals(bufferBytes(buffer)),
            )
            assertEquals("reserve count at $point", 1, bridge.reserveCalls)
            assertEquals("release count at $point", 1, bridge.releaseCalls)
            assertEquals(0, bridge.liveTokens)
        }
    }

    @Test
    fun cancellationImmediatelyAfterAdmissionIsPixelAtomicAndReleasesExactlyOnce() {
        val w = 67
        val h = 43
        val buffer = rgbBuffer(w, h)
        val before = bufferBytes(buffer)
        val bridge = RecordingBridge()
        try {
            MaskCompositor.applyInPlace(
                buffer,
                w,
                h,
                ColorSpace.SRGB,
                true,
                listOf(LocalAdjustment(Mask(invert = true), TierADelta(clarity = 40f))),
                MaskCompositorRuntime(
                    cancellation = MaskCancellationSignal { it == MaskCancellationPoint.ADMITTED },
                    memoryBridge = bridge,
                ),
            )
            fail("admission cancellation must throw")
        } catch (failure: MaskCompositionCancelledException) {
            assertEquals(MaskCancellationPoint.ADMITTED, failure.point)
        }
        assertArrayEquals(before, bufferBytes(buffer))
        assertEquals(1, bridge.reserveCalls)
        assertEquals(1, bridge.releaseCalls)
        assertEquals(0, bridge.liveTokens)
    }

    @Test
    fun publicProductionApiAcceptsTheSharedLiveRenderCancellationToken() {
        val w = 67
        val h = 43
        val buffer = rgbBuffer(w, h)
        val before = bufferBytes(buffer)
        val cancellation = RenderCancellation().also { it.cancel() }
        try {
            MaskCompositor.applyInPlace(
                buffer,
                w,
                h,
                ColorSpace.SRGB,
                true,
                listOf(LocalAdjustment(Mask(invert = true), TierADelta(clarity = 40f))),
                cancellation,
            )
            fail("a cancelled shared render token must stop mask composition")
        } catch (failure: MaskCompositionCancelledException) {
            assertEquals(MaskCancellationPoint.ADMITTED, failure.point)
        }
        assertArrayEquals(before, bufferBytes(buffer))
    }

    @Test
    fun cancellationAfterRealFilterProgressReleasesReservationAndRequiresDiscard() {
        val w = 67
        val h = 43
        val buffer = rgbBuffer(w, h)
        val before = bufferBytes(buffer)
        val bridge = RecordingBridge()
        var verticalPassThreeChecks = 0
        val adjustment = LocalAdjustment(
            mask = Mask(invert = true),
            delta = TierADelta(exposureEv = 0.5f, clarity = 40f),
        )

        try {
            MaskCompositor.applyInPlace(
                buffer,
                w,
                h,
                ColorSpace.SRGB,
                true,
                listOf(adjustment),
                MaskCompositorRuntime(
                    cancellation = MaskCancellationSignal { point ->
                        if (point != MaskCancellationPoint.VERTICAL_PASS_3) return@MaskCancellationSignal false
                        verticalPassThreeChecks++
                        verticalPassThreeChecks == 5
                    },
                    memoryBridge = bridge,
                ),
            )
            fail("in-flight filter cancellation must throw")
        } catch (failure: MaskCompositionCancelledException) {
            assertEquals(MaskCancellationPoint.VERTICAL_PASS_3, failure.point)
        }

        assertEquals("cancellation must happen after repeated V3 progress", 5, verticalPassThreeChecks)
        assertTrue(
            "a private work buffer may be partially mutated and must be discarded after in-flight cancellation",
            !before.contentEquals(bufferBytes(buffer)),
        )
        assertEquals(1, bridge.reserveCalls)
        assertEquals(1, bridge.releaseCalls)
        assertEquals(0, bridge.liveTokens)
    }

    private fun rgbBuffer(w: Int, h: Int): ByteBuffer {
        val buffer = ByteBuffer.allocateDirect(w * h * 3 * Float.SIZE_BYTES).order(ByteOrder.nativeOrder())
        val floats = buffer.asFloatBuffer()
        for (p in 0 until w * h) {
            floats.put(p * 3, ((p * 17L + p / w * 3L) % 997L).toFloat() / 997f)
            floats.put(p * 3 + 1, ((p * 31L + 11L) % 991L).toFloat() / 991f)
            floats.put(p * 3 + 2, ((p * 47L + 23L) % 983L).toFloat() / 983f)
        }
        return buffer
    }

    private fun cloneBuffer(source: ByteBuffer): ByteBuffer {
        val clone = ByteBuffer.allocateDirect(source.capacity()).order(ByteOrder.nativeOrder())
        clone.put(source.duplicate().apply { clear() })
        clone.clear()
        return clone
    }

    private fun bufferBytes(source: ByteBuffer): ByteArray =
        ByteArray(source.capacity()).also { source.duplicate().apply { clear() }.get(it) }

    private fun richAdjustments(): List<LocalAdjustment> {
        val firstMask = Mask(
            components = listOf(
                Mask.Component(
                    BlendMode.ADD,
                    MaskComponent.Radial(0.46f, 0.51f, 0.43f, 0.36f, 0.77f, 23f),
                    value = 0.91f,
                ),
                Mask.Component(
                    BlendMode.SUBTRACT,
                    MaskComponent.Linear(0.08f, 0.14f, 0.92f, 0.81f),
                    invert = true,
                    value = 0.27f,
                ),
            ),
            opacity = 0.83f,
            luminanceRange = LuminanceRange(0.09f, 0.93f, 0.11f),
            colorRange = ColorRange(0.72f, 0.31f, 0.18f, tolerance = 0.72f, feather = 0.22f),
        )
        val secondMask = Mask(
            components = listOf(
                Mask.Component(
                    BlendMode.ADD,
                    MaskComponent.Linear(0.13f, 0.91f, 0.86f, 0.06f),
                    value = 0.74f,
                ),
                Mask.Component(
                    BlendMode.INTERSECT,
                    MaskComponent.Radial(0.57f, 0.43f, 0.49f, 0.41f, 0.88f, -11f),
                ),
            ),
            invert = true,
            opacity = 0.61f,
        )
        return listOf(
            LocalAdjustment(
                firstMask,
                TierADelta(
                    exposureEv = 0.37f,
                    temp = 18f,
                    tint = -13f,
                    saturation = 27f,
                    contrast = 19f,
                    hue = 31f,
                    whites = 14f,
                    blacks = -17f,
                    clarity = 23f,
                    sharpness = 29f,
                    texture = -21f,
                    highlights = -18f,
                    shadows = 26f,
                ),
            ),
            LocalAdjustment(
                secondMask,
                TierADelta(
                    exposureEv = -0.19f,
                    temp = -7f,
                    tint = 9f,
                    saturation = -16f,
                    contrast = -12f,
                    hue = -22f,
                    whites = -8f,
                    blacks = 11f,
                    clarity = -17f,
                    sharpness = 13f,
                    texture = 15f,
                    highlights = 21f,
                    shadows = -14f,
                ),
            ),
        )
    }

    /** Frozen, independent copy of the complete pre-#141 compositor. */
    private fun frozenLegacyComposite(
        data: ByteBuffer,
        w: Int,
        h: Int,
        cs: ColorSpace,
        cctfEncoded: Boolean,
        adjustments: List<LocalAdjustment>,
    ) {
        val active = adjustments.filter { !it.delta.isNoOp && MaskRaster.coverage(it.mask) > 1e-4f }
        if (active.isEmpty()) return
        val f = data.order(ByteOrder.nativeOrder()).asFloatBuffer()
        val rgb = FloatArray(3)
        for (adj in active) {
            val d = adj.delta
            val gain = if (d.exposureEv == 0f) 1f else Math.pow(2.0, d.exposureEv.toDouble()).toFloat()
            val wbM = if (d.temp != 0f || d.tint != 0f) LocalWhiteBalance.matrix(cs, d.temp, d.tint) else null
            val sat = d.saturation / 100f
            val levels = d.whites != 0f || d.blacks != 0f
            val bp = -(d.blacks / 100f) * 0.25f
            val wp = 1f - (d.whites / 100f) * 0.25f
            val invSpan = if (levels) 1f / (wp - bp) else 1f
            val lumRange = adj.mask.luminanceRange?.takeIf { it.isActive }
            val colorRange = adj.mask.colorRange?.takeIf { it.isActive }
            val alpha = MaskRaster.rasterize(adj.mask, w, h)
            val n = w * h
            val maxWH = maxOf(w, h).toFloat()
            val luma = if (d.hasSpatial) FloatArray(n).also {
                var q = 0
                while (q < n) {
                    val k = q * 3
                    it[q] = 0.2126f * f.get(k) + 0.7152f * f.get(k + 1) + 0.0722f * f.get(k + 2)
                    q++
                }
            } else null
            val blurClarity = if (luma != null && d.clarity != 0f) {
                frozenLegacyBlur(luma, w, h, (MaskSpatial.RADIUS_FRAC_CLARITY * maxWH).toInt())
            } else null
            val blurTexture = if (luma != null && d.texture != 0f) {
                frozenLegacyBlur(luma, w, h, (MaskSpatial.RADIUS_FRAC_TEXTURE * maxWH).toInt())
            } else null
            val blurSharp = if (luma != null && d.sharpness != 0f) {
                frozenLegacyBlur(luma, w, h, (MaskSpatial.RADIUS_FRAC_SHARP * maxWH).toInt())
            } else null
            val blurRegion = if (luma != null && (d.highlights != 0f || d.shadows != 0f)) {
                frozenLegacyBlur(luma, w, h, (MaskSpatial.RADIUS_FRAC_REGION * maxWH).toInt())
            } else null
            val clarityK = d.clarity / 100f * 1.2f
            val textureK = d.texture / 100f
            val sharpK = d.sharpness / 100f * 1.5f
            val highK = d.highlights / 100f * 0.35f
            val shadK = d.shadows / 100f * 0.35f

            var p = 0
            while (p < n) {
                var a = alpha[p]
                if (a > 0f) {
                    val k = p * 3
                    val or = f.get(k)
                    val og = f.get(k + 1)
                    val ob = f.get(k + 2)
                    if (lumRange != null) a *= lumRange.gate(0.2126f * or + 0.7152f * og + 0.0722f * ob)
                    if (a > 0f && colorRange != null) a *= colorRange.gate(or, og, ob)
                    if (a > 0f) {
                        rgb[0] = OutputCctf.decode(cs, or, cctfEncoded) * gain
                        rgb[1] = OutputCctf.decode(cs, og, cctfEncoded) * gain
                        rgb[2] = OutputCctf.decode(cs, ob, cctfEncoded) * gain
                        if (wbM != null) {
                            val r = rgb[0]
                            val g = rgb[1]
                            val b = rgb[2]
                            rgb[0] = wbM[0] * r + wbM[1] * g + wbM[2] * b
                            rgb[1] = wbM[3] * r + wbM[4] * g + wbM[5] * b
                            rgb[2] = wbM[6] * r + wbM[7] * g + wbM[8] * b
                        }
                        if (sat != 0f) Oklab.scaleChromaLinear(rgb, sat, 0f)
                        if (d.hue != 0f) Oklab.rotateHueLinear(rgb, d.hue)
                        var er = OutputCctf.encode(cs, rgb[0], cctfEncoded)
                        var eg = OutputCctf.encode(cs, rgb[1], cctfEncoded)
                        var eb = OutputCctf.encode(cs, rgb[2], cctfEncoded)
                        if (d.contrast != 0f) {
                            er = ContrastCurve.curveAt(er, d.contrast)
                            eg = ContrastCurve.curveAt(eg, d.contrast)
                            eb = ContrastCurve.curveAt(eb, d.contrast)
                        }
                        if (levels) {
                            er = ((er - bp) * invSpan).coerceIn(0f, 1f)
                            eg = ((eg - bp) * invSpan).coerceIn(0f, 1f)
                            eb = ((eb - bp) * invSpan).coerceIn(0f, 1f)
                        }
                        if (d.hasSpatial && luma != null) {
                            val lp = luma[p]
                            var dL = 0f
                            if (blurClarity != null) dL += clarityK * (lp - blurClarity[p]) * MaskSpatial.midtoneWeight(lp)
                            if (blurTexture != null) dL += textureK * (lp - blurTexture[p])
                            if (blurSharp != null) dL += sharpK * (lp - blurSharp[p])
                            if (blurRegion != null) {
                                val blurred = blurRegion[p]
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
                p++
            }
        }
    }

    private fun streamedBlur(source: FloatArray, w: Int, h: Int, radius: Int): FloatArray {
        val bridge = RecordingBridge()
        val plan = MaskScratchPlan.create(w, h, listOf(intArrayOf(radius)))
        MaskTileScratch.open(plan, bridge).use { scratch ->
            val stream = MaskTripleBoxStream(w, h, radius, scratch.pipeline(radius))
            stream.reset()
            val result = FloatArray(w * h)
            var y = 0
            while (y < h) {
                source.copyInto(scratch.sourceLuma, 0, y * w, (y + 1) * w)
                val emitted = stream.feedOriginal(scratch.sourceLuma)
                if (emitted >= 0) copyStreamRow(stream, emitted, w, result)
                y++
            }
            while (!stream.complete) {
                val emitted = stream.advanceBottom()
                if (emitted >= 0) copyStreamRow(stream, emitted, w, result)
            }
            assertEquals(1, bridge.reserveCalls)
            return result
        }
    }

    private fun copyStreamRow(stream: MaskTripleBoxStream, y: Int, w: Int, destination: FloatArray) {
        var x = 0
        while (x < w) {
            destination[y * w + x] = stream.outputAt(y, x)
            x++
        }
    }

    private fun assertDelayedRows(
        streams: List<MaskTripleBoxStream>,
        expected: Map<Int, FloatArray>,
        y: Int,
        w: Int,
    ) {
        for (stream in streams) {
            val oracle = expected.getValue(stream.radius)
            for (x in 0 until w) {
                assertEquals("radius=${stream.radius}, x=$x, y=$y", oracle[y * w + x], stream.outputAt(y, x), 0f)
            }
        }
    }

    /** Frozen, independent copy of the pre-#141 full-frame implementation. Do not refactor to production. */
    private fun frozenLegacyBlur(source: FloatArray, w: Int, h: Int, radius: Int): FloatArray {
        // The legacy entry point returned immediately for a sub-pixel radius. Running nominal
        // radius-zero rolling sums is not equivalent: `(a + b) - a` can round away one ULP.
        if (radius < 1 || w < 2 || h < 2 || source.size < w * h) return source.copyOf()
        var current = source.copyOf()
        val temporary = FloatArray(w * h)
        repeat(3) {
            frozenBoxH(current, temporary, w, h, radius)
            frozenBoxV(temporary, current, w, h, radius)
        }
        return current
    }

    private fun frozenBoxH(source: FloatArray, destination: FloatArray, w: Int, h: Int, r: Int) {
        val norm = 1f / (2 * r + 1)
        for (y in 0 until h) {
            val row = y * w
            var sum = 0f
            for (k in -r..r) sum += source[row + k.coerceIn(0, w - 1)]
            destination[row] = sum * norm
            for (x in 1 until w) {
                sum += source[row + (x + r).coerceIn(0, w - 1)] -
                    source[row + (x - r - 1).coerceIn(0, w - 1)]
                destination[row + x] = sum * norm
            }
        }
    }

    private fun frozenBoxV(source: FloatArray, destination: FloatArray, w: Int, h: Int, r: Int) {
        val norm = 1f / (2 * r + 1)
        for (x in 0 until w) {
            var sum = 0f
            for (k in -r..r) sum += source[k.coerceIn(0, h - 1) * w + x]
            destination[x] = sum * norm
            for (y in 1 until h) {
                sum += source[(y + r).coerceIn(0, h - 1) * w + x] -
                    source[(y - r - 1).coerceIn(0, h - 1) * w + x]
                destination[y * w + x] = sum * norm
            }
        }
    }

    private class RecordingBridge(private val deny: Boolean = false) : MemoryBudgetBridge {
        var reserveCalls = 0
        var releaseCalls = 0
        var liveTokens = 0
        var lastBytes = 0L
        var lastStage: MemoryBudgetStage? = null
        private var nextToken = 1L

        override fun configure(limitBytes: Long) = Unit

        override fun reserve(bytes: Long, stage: MemoryBudgetStage): Long {
            reserveCalls++
            lastBytes = bytes
            lastStage = stage
            if (deny) return 0L
            liveTokens++
            return nextToken++
        }

        override fun release(token: Long): Boolean {
            releaseCalls++
            if (liveTokens == 0) return false
            liveTokens--
            return true
        }

        override fun snapshotJson(): String = "{}"
    }
}
