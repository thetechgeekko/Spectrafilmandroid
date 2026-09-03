/*
 * Spektrafilm for Android — backup/network/manifest policy regression tests. GPLv3.
 */
package com.spectrafilm.app

import java.io.File
import javax.xml.parsers.DocumentBuilderFactory
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.w3c.dom.Document
import org.w3c.dom.Element

class BackupSecurityConfigTest {

    private val allowedBackupData = setOf(
        "sharedpref:spectrafilm_settings.xml",
        "file:datastore/back_exit_hint.preferences_pb",
    )
    private val neverEligibleData = setOf(
        "sharedpref:source_access_v1.xml",
        "sharedpref:pending_media_exports_v1.xml",
        "sharedpref:secrets.xml",
        "file:diag",
        "file:spektra",
        "file:full_res_cache",
        "file:transient_exports",
        "file:uri_grants",
        "file:secrets",
        "file:updater",
        "file:presets",
        "file:recipes",
        "external:.",
    )

    @Test
    fun legacyBackupFallbackRequiresEncryptionAndUsesANarrowAllowlist() {
        val document = xml("app/src/main/res/xml/backup_rules.xml")

        assertEquals(allowedBackupData, rules(document.documentElement, "include"))
        assertTrue(elements(document.documentElement, "exclude").isEmpty())
        assertTrue(allowedBackupData.intersect(neverEligibleData).isEmpty())
        elements(document.documentElement, "include").forEach { include ->
            assertEquals(
                "clientSideEncryption",
                include.getAttribute("requireFlags"),
            )
        }
    }

    @Test
    fun cloudAndDeviceTransferUseTheSameNarrowAllowlist() {
        val document = xml("app/src/main/res/xml/data_extraction_rules.xml")
        val cloud = elements(document.documentElement, "cloud-backup").single()
        val transfer = elements(document.documentElement, "device-transfer").single()

        assertEquals("true", cloud.getAttribute("disableIfNoEncryptionCapabilities"))
        for (section in listOf(cloud, transfer)) {
            assertEquals(allowedBackupData, rules(section, "include"))
            assertTrue(elements(section, "exclude").isEmpty())
            assertTrue(rules(section, "include").intersect(neverEligibleData).isEmpty())
        }
    }

    @Test
    fun manifestDisablesBackupRejectsVersionDowngradeRestoreAndAvoidsPrivilegedAccess() {
        val manifest = xml("app/src/main/AndroidManifest.xml")
        val application = elements(manifest.documentElement, "application").single()
        val android = "http://schemas.android.com/apk/res/android"

        assertEquals("false", application.getAttributeNS(android, "allowBackup"))
        assertEquals("false", application.getAttributeNS(android, "restoreAnyVersion"))
        assertEquals("@xml/backup_rules", application.getAttributeNS(android, "fullBackupContent"))
        assertEquals("@xml/data_extraction_rules", application.getAttributeNS(android, "dataExtractionRules"))
        assertEquals("false", application.getAttributeNS(android, "usesCleartextTraffic"))
        assertEquals("@xml/network_security_config", application.getAttributeNS(android, "networkSecurityConfig"))

        val permissions = elements(manifest.documentElement, "uses-permission")
            .map { it.getAttributeNS(android, "name") }
            .toSet()
        assertTrue("android.permission.INTERNET" in permissions)
        for (forbidden in listOf(
            "android.permission.ACCESS_NETWORK_STATE",
            "android.permission.REQUEST_INSTALL_PACKAGES",
            "android.permission.READ_LOGS",
            "android.permission.READ_MEDIA_IMAGES",
            "android.permission.MANAGE_EXTERNAL_STORAGE",
        )) {
            assertFalse("unexpected permission $forbidden", forbidden in permissions)
        }
    }

    @Test
    fun crossPlatformTransferStaysDisabledUntilARealPlatformIdentityExists() {
        val manifest = xml("app/src/main/AndroidManifest.xml")
        val application = elements(manifest.documentElement, "application").single()
        val android = "http://schemas.android.com/apk/res/android"
        val extraction = xml("app/src/main/res/xml/data_extraction_rules.xml")

        assertEquals("false", application.getAttributeNS(android, "allowBackup"))
        assertTrue(elements(extraction.documentElement, "cross-platform-transfer").isEmpty())
    }

    @Test
    fun networkPolicyDeniesCleartextAndUsesOnlySystemTrust() {
        val document = xml("app/src/main/res/xml/network_security_config.xml")
        val base = elements(document.documentElement, "base-config").single()
        assertEquals("false", base.getAttribute("cleartextTrafficPermitted"))
        val certificates = elements(base, "certificates").single()
        assertEquals("system", certificates.getAttribute("src"))

        val domains = elements(document.documentElement, "domain")
        assertEquals(listOf("api.github.com"), domains.map { it.textContent.trim() })
        assertEquals("false", domains.single().getAttribute("includeSubdomains"))
    }

    private fun rules(section: Element, tag: String): Set<String> =
        elements(section, tag).map { "${it.getAttribute("domain")}:${it.getAttribute("path")}" }.toSet()

    private fun elements(parent: Element, tag: String): List<Element> {
        val nodes = parent.getElementsByTagName(tag)
        return (0 until nodes.length).map { nodes.item(it) as Element }
    }

    private fun xml(relativePath: String): Document =
        DocumentBuilderFactory.newInstance().apply {
            isNamespaceAware = true
            setFeature("http://apache.org/xml/features/disallow-doctype-decl", true)
        }.newDocumentBuilder().parse(repoFile(relativePath))

    private fun repoFile(relativePath: String): File {
        var current: File? = File(System.getProperty("user.dir") ?: ".").absoluteFile
        while (current != null) {
            val candidate = File(current, relativePath)
            if (candidate.isFile) return candidate
            current = current.parentFile
        }
        throw AssertionError("Could not locate $relativePath")
    }
}
