/*
 * Spektrafilm for Android — unit tests for output color management. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Guards the pure per-output-space mappings that drive display tagging (Bitmap color space) and
 * export ICC embedding. Android Bitmap tagging remains device-only; the descriptor decisions and
 * fail-closed ICC loader they depend on are covered here on the plain JVM.
 */
package com.spectrafilm.app

import com.spectrafilm.engine.ColorSpace
import java.nio.file.Files
import java.nio.file.Path
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ColorManagementTest {

    @Test
    fun displayName_mapsEachSpaceToItsAndroidNamedConstant() {
        // Strings must be exact android.graphics.ColorSpace.Named constant names (resolved by
        // valueOf at the call site). Transfers verified to match the engine's output_cctf_encode.
        assertEquals("SRGB", ColorManagement.displayColorSpaceName(ColorSpace.SRGB))
        assertEquals("ADOBE_RGB", ColorManagement.displayColorSpaceName(ColorSpace.ADOBE_RGB))
        assertEquals("PRO_PHOTO_RGB", ColorManagement.displayColorSpaceName(ColorSpace.PROPHOTO))
        assertEquals("BT2020", ColorManagement.displayColorSpaceName(ColorSpace.REC2020))
        assertEquals("LINEAR_SRGB", ColorManagement.displayColorSpaceName(ColorSpace.LINEAR_SRGB))
    }

    @Test
    fun displayName_isNullForAces() {
        // ACES2065_1 (AP0, linear) exceeds [0,1] → no faithful 8-bit tag; preview is left untagged.
        assertNull(ColorManagement.displayColorSpaceName(ColorSpace.ACES2065_1))
    }

    @Test
    fun iccAssetPath_pointsAtBundledProfilePerSpace() {
        assertEquals("spektra/icc/saucecontrol/sRGB-v4.icc",
            ColorManagement.iccAssetPath(ColorSpace.SRGB))
        assertEquals("spektra/icc/saucecontrol/AdobeCompat-v4.icc",
            ColorManagement.iccAssetPath(ColorSpace.ADOBE_RGB))
        assertEquals("spektra/icc/saucecontrol/ProPhoto-v4.icc",
            ColorManagement.iccAssetPath(ColorSpace.PROPHOTO))
        assertEquals("spektra/icc/saucecontrol/Rec2020-v4.icc",
            ColorManagement.iccAssetPath(ColorSpace.REC2020))
        assertEquals("spektra/icc/ellelstone/ACES-elle-V4-g10.icc",
            ColorManagement.iccAssetPath(ColorSpace.ACES2065_1))
        assertEquals("spektra/icc/ellelstone/sRGB-elle-V4-g10.icc",
            ColorManagement.iccAssetPath(ColorSpace.LINEAR_SRGB))
    }

    @Test
    fun everySpaceHasADistinctBundledIccPath() {
        // Exhaustive over the enum: no space falls through to a blank/duplicate profile path.
        val paths = ColorSpace.entries.map { ColorManagement.iccAssetPath(it) }
        for (p in paths) {
            assertTrue("path under spektra/icc and .icc: $p",
                p.startsWith("spektra/icc/") && p.endsWith(".icc"))
        }
        assertEquals("all six spaces map to a distinct profile",
            ColorSpace.entries.size, paths.toSet().size)
    }

    @Test
    fun everySpaceEitherTagsOrIsTheKnownAcesException() {
        // Exactly one space (ACES2065_1) has no 8-bit display tag; all others do.
        val untagged = ColorSpace.entries.filter { ColorManagement.displayColorSpaceName(it) == null }
        assertEquals(listOf(ColorSpace.ACES2065_1), untagged)
    }

    @Test
    fun requiredProfileFailure_isRejectedInsteadOfProducingAnUntaggedFile() {
        val descriptor = OutputDescriptor.rendered(
            ExportFormat.TIFF,
            ColorSpace.PROPHOTO,
            outputCctfEncoding = true,
            bitDepth = OutputBitDepth.UINT16,
        )

        assertTrue(runCatching {
            ColorManagement.requireIccBytes(descriptor) { error("missing") }
        }.isFailure)
        assertTrue(runCatching {
            ColorManagement.requireIccBytes(descriptor) { ByteArray(0) }
        }.isFailure)

        val bitmapDescriptor = OutputDescriptor.rendered(
            ExportFormat.JPEG,
            ColorSpace.SRGB,
            outputCctfEncoding = true,
            bitDepth = OutputBitDepth.UINT8,
        )
        var loaderCalled = false
        assertNull(ColorManagement.requireIccBytes(bitmapDescriptor) {
            loaderCalled = true
            error("Bitmap encoder has no direct ICC payload")
        })
        assertEquals(false, loaderCalled)
    }

    @Test
    fun everyDescriptorIccPath_isPackagedAsANonEmptyIccAsset() {
        val assetRoot = findRepositoryRoot().resolve("engine/spektra-core/src/main/assets")
        for (space in ColorSpace.entries) {
            val descriptor = OutputDescriptor.rendered(
                ExportFormat.TIFF,
                space,
                outputCctfEncoding = space != ColorSpace.LINEAR_SRGB,
                bitDepth = OutputBitDepth.UINT16,
            )
            val relativePath = checkNotNull(descriptor.metadata.iccAssetPath)
            val bytes = Files.readAllBytes(assetRoot.resolve(relativePath))
            assertTrue("$relativePath must contain an ICC header", bytes.size >= 128)
            assertEquals("$relativePath ICC signature", "acsp", String(bytes, 36, 4, Charsets.US_ASCII))
        }
    }

    private fun findRepositoryRoot(): Path {
        var current = Path.of("").toAbsolutePath().normalize()
        repeat(8) {
            if (Files.isDirectory(current.resolve("engine/spektra-core/src/main/assets"))) {
                return current
            }
            current = current.parent
                ?: error("Could not locate repository root from test working directory")
        }
        error("Could not locate repository root from test working directory")
    }
}
