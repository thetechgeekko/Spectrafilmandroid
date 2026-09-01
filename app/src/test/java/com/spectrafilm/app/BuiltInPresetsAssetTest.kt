/*
 * Spektrafilm for Android — unit tests for the bundled built-in preset asset. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * The built-in presets ship as engine/spektra-core/.../assets/spektra/presets.json and are
 * only applied at runtime (via AssetManager), so they had no automated coverage. This guards
 * the asset's structural invariants on the plain JVM with the real org.json on the test
 * classpath (see app/build.gradle.kts): valid JSON, unique ids, every film/print profile a
 * real catalog entry, and — importantly — that no preset sets a field the engine BAKES per
 * profile or doesn't expose (which would be a silent no-op / cosmetic lie). See
 * spektra.cpp apply_user_halation/apply_user_dir_couplers/apply_user_diffusion_filter.
 */
package com.spectrafilm.app

import org.json.JSONObject
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class BuiltInPresetsAssetTest {

    private val assetBase = "engine/spektra-core/src/main/assets/spektra"

    /** Locate a repo-relative file by walking up from the test working dir (repo root or app/). */
    private fun repoFile(rel: String): File {
        var dir: File? = File(System.getProperty("user.dir") ?: ".").absoluteFile
        while (dir != null) {
            val f = File(dir, rel)
            if (f.exists()) return f
            dir = dir.parentFile
        }
        throw AssertionError("Could not locate $rel from ${System.getProperty("user.dir")}")
    }

    private fun presetsArray() =
        JSONObject(repoFile("$assetBase/presets.json").readText()).getJSONArray("presets")

    @Test
    fun presets_parseHaveMetadataAndReferenceRealProfiles() {
        val catalog = JSONObject(repoFile("$assetBase/catalog.json").readText())
        val stocks = catalog.getJSONObject("stocks").keys().asSequence().toSet()
        val arr = presetsArray()
        assertTrue("expected at least one preset", arr.length() > 0)

        val ids = mutableSetOf<String>()
        for (i in 0 until arr.length()) {
            val p = arr.getJSONObject(i)
            val id = p.optString("id")
            assertTrue("blank id at index $i", id.isNotBlank())
            assertTrue("duplicate preset id: $id", ids.add(id))
            assertTrue("$id missing name", p.optString("name").isNotBlank())
            assertTrue("$id missing group", p.optString("group").isNotBlank())
            assertTrue("$id missing description", p.optString("description").isNotBlank())

            val params = p.getJSONObject("params")
            for (key in listOf("filmProfile", "printProfile")) {
                val ref = params.optString(key, "")
                assertTrue("$id $key='$ref' is not a catalog profile", ref.isEmpty() || ref in stocks)
            }
        }
    }

    @Test
    fun presets_doNotSetEngineIgnoredFields() {
        // Baked per-profile by digest_halation_params / _apply_film_specifics. Diffusion
        // filterFamily is intentionally absent: BuiltInPresets, JNI, and native all honor it.
        val ignored = listOf(
            "halationStrength", "halationFirstSigmaUm",
            "gammaSamelayerRgb", "gammaInterlayerRToGb",
            "gammaInterlayerGToRb", "gammaInterlayerBToRg",
        )
        val arr = presetsArray()
        for (i in 0 until arr.length()) {
            val p = arr.getJSONObject(i)
            val params = p.getJSONObject("params").toString()
            for (k in ignored) {
                assertFalse(
                    "${p.optString("id")} sets engine-ignored field '$k' (no-op)",
                    params.contains("\"$k\""),
                )
            }
        }
    }

    @Test
    fun presets_onlyUseEngineHonoredDiffusionFamilies() {
        validateDiffusionFamilies(presetsArray())
    }

    @Test
    fun diffusionFamilyValidationRejectsUnknownFamily() {
        val arr = presetsArray()
        val preset = arr.getJSONObject(0)
        preset.getJSONObject("params")
            .put("camera", JSONObject().put(
                "diffusionFilter",
                JSONObject().put("filterFamily", "unknown_filter"),
            ))

        val error = assertThrows(AssertionError::class.java) {
            validateDiffusionFamilies(arr)
        }
        assertTrue(error.message.orEmpty().contains("unknown_filter"))
    }

    private fun validateDiffusionFamilies(presets: org.json.JSONArray) {
        val known = DIFFUSION_FAMILIES.toSet()
        for (i in 0 until presets.length()) {
            val preset = presets.getJSONObject(i)
            val params = preset.getJSONObject("params")
            for (stage in listOf("camera", "enlarger")) {
                val diffusion = params.optJSONObject(stage)
                    ?.optJSONObject("diffusionFilter")
                    ?: continue
                if (!diffusion.has("filterFamily")) continue
                val family = diffusion.optString("filterFamily")
                assertTrue(
                    "${preset.optString("id")} $stage.diffusionFilter.filterFamily=" +
                        "'$family' is not engine-honored",
                    family in known,
                )
            }
        }
    }
}
