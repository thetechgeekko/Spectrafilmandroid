/*
 * Spektrafilm for Android — on-device profile-load test (PR1). GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Enumerates the bundled profile ids the engine reports and runs a 1x1 scan_film simulate for
 * EACH as the film stock, proving every committed profile JSON parses and drives a render on
 * the real device without throwing. Failures are collected so a single bad profile names itself
 * rather than aborting the sweep on the first.
 *
 * scan_film=true (scan the negative directly) is used so every stock — negatives and slide
 * positives alike — loads through one universally valid path with no print-stage mismatch.
 */
package com.spectrafilm.app

import com.spectrafilm.engine.LinearImage
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import androidx.test.ext.junit.runners.AndroidJUnit4

@RunWith(AndroidJUnit4::class)
class ProfilesLoadTest {

    @Test
    fun everyBundledProfile_simulates1x1_noThrow() {
        val engine = DeviceTestSupport.newEngine()
        try {
            val profiles = engine.listProfiles()
            // The suite was authored against 28 bundled profiles (assets/spektra/profiles/); assert
            // the floor and print the actual list so an add/remove is a visible finding, not silent.
            assertTrue(
                "expected >= 28 bundled profiles, got ${profiles.size}: $profiles",
                profiles.size >= 28,
            )
            assertTrue("kodak_portra_400 present", profiles.contains("kodak_portra_400"))

            val failures = ArrayList<String>()
            for (id in profiles) {
                try {
                    LinearImage(DeviceTestSupport.uniformImage(1, 1, 0.18f), 1, 1, "ProPhoto RGB").use { img ->
                        engine.simulate(img, DeviceTestSupport.scanParams(film = id)).use { /* freed */ }
                    }
                } catch (t: Throwable) {
                    failures.add("$id -> ${t.javaClass.simpleName}: ${t.message}")
                }
            }
            assertTrue("profiles that failed to simulate: $failures", failures.isEmpty())
        } finally {
            engine.close()
        }
    }
}
