/* Spektrafilm Android — pure decode-plan contract tests. GPL-3.0-only. */
package com.spectrafilm.aimage

import java.lang.reflect.Modifier
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class DecodeContractTest {
    @Test
    fun r8RenameCanarySourceNameIsPinnedForReleaseSmoke() {
        assertEquals(
            AIMAGE_R8_CANARY_ORIGINAL_NAME,
            AImageR8RenameCanary().runtimeClassName(),
        )
    }

    @Test
    fun srgbOpaqueUsesExplicit8888AndLinearBuffer() {
        val plan = planAImageDecode(header(), maxEdge = 2_048)
        assertEquals(AImagePixelFormat.RGBA_8888, plan.pixelFormat)
        assertEquals(AImageDataSpace.SRGB, plan.outputDataSpace)
        assertTrue(plan.setOutputDataSpace)
        assertFalse(plan.requireUnpremultiplied)
        assertTrue(plan.canConvertToLinearProPhoto)
        assertEquals(2_048 * 1_365 * 4, plan.pixelByteCount)
        assertEquals(2_048 * 1_365 * 3 * 4, plan.linearProPhotoByteCount)
    }

    @Test
    fun wideOrHdrInputStaysF16AndCannotEnterLinearContract() {
        val plan = planAImageDecode(
            header(dataSpace = AImageDataSpace.BT2020_PQ, defaultFormat = 9),
            maxEdge = 4_096,
        )
        assertEquals(AImagePixelFormat.RGBA_F16, plan.pixelFormat)
        assertEquals(AImageDataSpace.BT2020_PQ, plan.outputDataSpace)
        assertTrue(plan.setOutputDataSpace)
        assertFalse(plan.canConvertToLinearProPhoto)
    }

    @Test
    fun extendedSrgbSixteenBitContractStaysF16() {
        val plan = planAImageDecode(
            header(dataSpace = AImageDataSpace.SCRGB, defaultFormat = 9),
            maxEdge = 4_096,
        )
        assertEquals(AImagePixelFormat.RGBA_F16, plan.pixelFormat)
        assertEquals(AImageDataSpace.SCRGB, plan.outputDataSpace)
        assertFalse(plan.canConvertToLinearProPhoto)
    }

    @Test
    fun srgbTenBitPlatformDefaultCannotCollapseIntoEightBitWorkingRoute() {
        val plan = planAImageDecode(
            header(
                dataSpace = AImageDataSpace.SRGB,
                // ANDROID_BITMAP_FORMAT_RGBA_1010102
                defaultFormat = 10,
            ),
            maxEdge = 4_096,
        )
        assertEquals(AImagePixelFormat.RGBA_F16, plan.pixelFormat)
        assertEquals(AImageDataSpace.SRGB, plan.outputDataSpace)
        assertFalse(plan.canConvertToLinearProPhoto)
    }

    @Test
    fun displayReferredDngFallbackForcesF16EvidenceOnlyEvenWhenPlatformDefaultsToSrgb8888() {
        val plan = planAImageDecode(
            header(
                inputKind = AImageInputKind.DNG,
                displayReferredDngFallback = true,
            ),
            maxEdge = 4_096,
        )
        assertEquals(AImagePixelFormat.RGBA_F16, plan.pixelFormat)
        assertEquals(AImageDataSpace.SRGB, plan.outputDataSpace)
        assertTrue(plan.setOutputDataSpace)
        assertFalse(plan.canConvertToLinearProPhoto)
    }

    @Test
    fun inconsistentDngFallbackIdentityFailsBeforePlanningOrAllocation() {
        assertThrows(IllegalArgumentException::class.java) {
            planAImageDecode(
                header(
                    inputKind = AImageInputKind.DNG,
                    displayReferredDngFallback = false,
                ),
                maxEdge = 4_096,
            )
        }
    }

    @Test
    fun phonePreviewPlanUsesDeclared2048EdgeExactly() {
        val plan = planAImageDecode(
            header(width = 4_080, height = 3_060),
            maxEdge = 2_048,
        )
        assertEquals(2_048, plan.targetWidth)
        assertEquals(1_536, plan.targetHeight)
        assertEquals(2_048 * 1_536 * 4, plan.pixelByteCount)
    }

    @Test
    fun unknownDataspaceStaysF16WithoutClaimingAColorTransform() {
        val plan = planAImageDecode(
            header(dataSpace = AImageDataSpace.UNKNOWN),
            maxEdge = 4_096,
        )
        assertEquals(AImagePixelFormat.RGBA_F16, plan.pixelFormat)
        assertEquals(AImageDataSpace.UNKNOWN, plan.outputDataSpace)
        assertFalse(plan.setOutputDataSpace)
        assertFalse(plan.canConvertToLinearProPhoto)
    }

    @Test
    fun alphaScalingFailsClosedInsteadOfPremultiplyingHiddenRgb() {
        assertThrows(AImageDecoderFallbackException::class.java) {
            planAImageDecode(header(alphaFlags = 2), maxEdge = 2_048)
        }
    }

    @Test
    fun unscaledAlphaRequestsUnpremultipliedOutput() {
        val plan = planAImageDecode(
            header(width = 800, height = 600, alphaFlags = 2),
            maxEdge = 2_048,
        )
        assertTrue(plan.requireUnpremultiplied)
        assertEquals(AImagePixelFormat.RGBA_8888, plan.pixelFormat)
    }

    @Test
    fun directBufferLimitOverflowFailsBeforeAllocation() {
        val huge = header(width = 16_384, height = 16_384)
        assertThrows(IllegalArgumentException::class.java) {
            planAImageDecode(huge, maxEdge = 16_384)
        }
    }

    @Test
    fun probeWireMarksDngAsDisplayFallbackOnly() {
        val parsed = parseAImageProbeWire(
            "sfaimage.probe.v1\t600\t338\t9\t1\t142671872\t266600\tDNG\timage/x-adobe-dng",
            declaredMime = "image/x-adobe-dng",
            allowDisplayReferredDngFallback = true,
        )
        assertTrue(parsed.displayReferredDngFallback)
        assertEquals(AImageInputKind.DNG, parsed.inputKind)
    }

    @Test
    fun publicContractCannotBeReconstructedOrCopiedByConsumers() {
        listOf(AImageHeader::class.java, AImageDecodePlan::class.java).forEach { type ->
            assertTrue("${type.simpleName} is not sealed", type.isSealed)
            assertTrue(
                "${type.simpleName} exposes a non-private constructor",
                type.declaredConstructors.all { Modifier.isPrivate(it.modifiers) },
            )
            assertTrue(
                "${type.simpleName} permits a public consumer implementation",
                type.permittedSubclasses.orEmpty()
                    .all { !Modifier.isPublic(it.modifiers) },
            )
            assertFalse(
                "${type.simpleName} exposes a data-class copy bypass",
                type.methods.any { it.name == "copy" || it.name.startsWith("copy\$") },
            )
        }
    }

    private fun header(
        width: Int = 6_000,
        height: Int = 4_000,
        defaultFormat: Int = 1,
        alphaFlags: Int = 1,
        dataSpace: Int = AImageDataSpace.SRGB,
        inputKind: AImageInputKind = AImageInputKind.JPEG,
        displayReferredDngFallback: Boolean = false,
    ) = createAImageHeader(
        width = width,
        height = height,
        platformDefaultFormat = defaultFormat,
        alphaFlags = alphaFlags,
        dataSpace = dataSpace,
        encodedByteCount = 4_096,
        inputKind = inputKind,
        platformMime = if (inputKind == AImageInputKind.DNG) {
            "image/x-adobe-dng"
        } else {
            "image/jpeg"
        },
        nativeWire = "test",
        declaredMimePolicy = if (inputKind == AImageInputKind.DNG) {
            "image/x-adobe-dng"
        } else {
            "image/jpeg"
        },
        displayReferredDngFallback = displayReferredDngFallback,
        allowDngFallbackPolicy = displayReferredDngFallback,
    )
}
