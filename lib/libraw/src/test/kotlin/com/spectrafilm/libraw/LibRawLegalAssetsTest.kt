/*
 * Spektrafilm for Android — pinned LibRaw legal-asset tests. GPLv3.
 */
package com.spectrafilm.libraw

import java.nio.file.Files
import java.nio.file.Path
import java.security.MessageDigest
import org.junit.Assert.assertEquals
import org.junit.Test

class LibRawLegalAssetsTest {
    private val expectedSha256 = mapOf(
        "COPYRIGHT" to "574c88f5dd59414cfc896293a63defbc2d525a418ed413d7ab7dcc43cf79c509",
        "LICENSE.LGPL" to "eea173a556abac0370461e57e12aab266894ea6be3874c2be05fd87871f75449",
        "LICENSE.CDDL" to "0e3098d2d54a12434715f6679ea408d57da5e8d613c385c58ecc6fe5d30cc81f",
    )

    @Test
    fun bundledLegalAssetsMatchThePinnedLibRaw0222Archive() {
        val legalDir = findModuleRoot().resolve("src/main/assets/third_party/libraw")

        expectedSha256.forEach { (name, expected) ->
            val actual = MessageDigest.getInstance("SHA-256")
                .digest(Files.readAllBytes(legalDir.resolve(name)))
                .joinToString("") { byte -> "%02x".format(byte) }
            assertEquals("Unexpected bytes in $name", expected, actual)
        }
    }

    private fun findModuleRoot(): Path {
        var current = Path.of("").toAbsolutePath().normalize()
        repeat(8) {
            if (Files.isDirectory(current.resolve("src/main/assets/third_party/libraw"))) {
                return current
            }
            val nestedModule = current.resolve("lib/libraw")
            if (Files.isDirectory(nestedModule.resolve("src/main/assets/third_party/libraw"))) {
                return nestedModule
            }
            current = current.parent ?: error("Could not locate lib/libraw from the test working directory")
        }
        error("Could not locate lib/libraw from the test working directory")
    }
}
