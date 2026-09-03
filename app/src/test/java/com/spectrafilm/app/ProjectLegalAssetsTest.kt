/*
 * Spektrafilm for Android — project legal-asset tests. GPLv3.
 */
package com.spectrafilm.app

import java.nio.file.Files
import java.nio.file.Path
import org.junit.Assert.assertEquals
import org.junit.Test

class ProjectLegalAssetsTest {
    @Test
    fun bundledProjectLicenseAndNoticeMatchRepositoryDocuments() {
        val root = findRepositoryRoot()

        assertEquals(
            normalizedText(root.resolve("LICENSE")),
            normalizedText(root.resolve("app/src/main/assets/$PROJECT_GPL_ASSET")),
        )
        assertEquals(
            normalizedText(root.resolve("NOTICE.md")),
            normalizedText(root.resolve("app/src/main/assets/$PROJECT_NOTICE_ASSET")),
        )
    }

    private fun normalizedText(path: Path): String =
        String(Files.readAllBytes(path), Charsets.UTF_8)
            .replace("\r\n", "\n")
            .replace('\r', '\n')
            .trimEnd('\n')

    private fun findRepositoryRoot(): Path {
        var current = Path.of("").toAbsolutePath().normalize()
        repeat(8) {
            if (
                Files.isRegularFile(current.resolve("LICENSE")) &&
                Files.isDirectory(current.resolve("app/src/main/assets"))
            ) {
                return current
            }
            current = current.parent
                ?: error("Could not locate the repository root from the test working directory")
        }
        error("Could not locate the repository root from the test working directory")
    }
}
