/*
 * Spektrafilm for Android — built-in preset application tests. GPLv3.
 * Film modeling powered by spektrafilm.
 */
package com.spectrafilm.app

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.File

class BuiltInPresetsApplyTest {

    @Test
    fun dreamyProMistAlwaysSelectsBlackProMistWithoutResettingOmittedControls() {
        val preset = loadPreset("portra400_promist_dreamy")
        assertEquals(
            "the asset must author its promised diffusion family",
            "black_pro_mist",
            preset.params.getJSONObject("camera")
                .getJSONObject("diffusionFilter")
                .getString("filterFamily"),
        )
        val startingStates = listOf(
            "fresh" to ParamsState(),
            "Black Pro-Mist" to ParamsState().apply {
                cameraDiffusionState.family = "black_pro_mist"
            },
            "Glimmerglass" to ParamsState().apply {
                cameraDiffusionState.family = "glimmerglass"
            },
            "CineBloom" to ParamsState().apply {
                cameraDiffusionState.family = "cinebloom"
            },
        )

        for ((startingFamily, state) in startingStates) {
            state.cameraLensBlurUm = 12.345f
            state.crop = true
            state.printExposure = 0.271f
            state.printDiffusionState.family = "cinebloom"

            BuiltInPresets.apply(preset, state)

            assertEquals(
                "$startingFamily state leaked into the authored camera diffusion family",
                "black_pro_mist",
                state.cameraDiffusionState.family,
            )
            assertEquals("omitted camera.lensBlurUm was reset", 12.345f, state.cameraLensBlurUm)
            assertEquals("omitted io.crop was reset", true, state.crop)
            assertEquals("omitted enlarger.printExposure was reset", 0.271f, state.printExposure)
            assertEquals(
                "omitted enlarger.diffusionFilter.family was reset",
                "cinebloom",
                state.printDiffusionState.family,
            )
        }
    }

    private fun loadPreset(id: String): BuiltInPreset {
        val arr = JSONObject(repoFile(
            "engine/spektra-core/src/main/assets/spektra/presets.json",
        ).readText()).getJSONArray("presets")
        for (i in 0 until arr.length()) {
            val value = arr.getJSONObject(i)
            if (value.optString("id") == id) {
                return BuiltInPreset(
                    id = id,
                    name = value.getString("name"),
                    group = value.getString("group"),
                    description = value.getString("description"),
                    params = value.getJSONObject("params"),
                )
            }
        }
        throw AssertionError("Missing built-in preset '$id'")
    }

    /** Locate a repo-relative file by walking up from the test working dir (repo root or app/). */
    private fun repoFile(relativePath: String): File {
        var directory: File? = File(System.getProperty("user.dir") ?: ".").absoluteFile
        while (directory != null) {
            val candidate = File(directory, relativePath)
            if (candidate.exists()) return candidate
            directory = directory.parentFile
        }
        throw AssertionError(
            "Could not locate $relativePath from ${System.getProperty("user.dir")}",
        )
    }
}
