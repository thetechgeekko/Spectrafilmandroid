/*
 * Spektrafilm for Android — film-emulation screen logic.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Decompose-style logic holder for the film-emulation screen. Mirrors the
 * ImageToolbox component idiom (BaseComponent + Hilt assisted-inject) and drives
 * a SpektraEngine off the main thread via coroutines.
 */
package com.spectrafilm.feature.film_emulation.presentation.screenLogic

import android.graphics.Bitmap
import android.net.Uri
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import com.arkivanov.decompose.ComponentContext
import com.spectrafilm.engine.AppRenderOutcome
import com.spectrafilm.engine.SpektraEngine
import com.spectrafilm.engine.SpektraParams
import com.t8rin.imagetoolbox.core.domain.coroutines.DispatchersHolder
import com.t8rin.imagetoolbox.core.domain.image.ImageGetter
import com.t8rin.imagetoolbox.core.domain.utils.runSuspendCatching
import com.t8rin.imagetoolbox.core.ui.utils.BaseComponent
import com.t8rin.imagetoolbox.core.ui.utils.helper.AppToastHost
import com.t8rin.imagetoolbox.core.ui.utils.state.update
import dagger.assisted.Assisted
import dagger.assisted.AssistedFactory
import dagger.assisted.AssistedInject
import kotlinx.coroutines.CancellationException

/** Quality mode for a run: interactive downscaled preview vs full-resolution scan. */
enum class RenderMode { Preview, FullScan }

class FilmEmulationComponent @AssistedInject internal constructor(
    @Assisted componentContext: ComponentContext,
    @Assisted val initialUri: Uri?,
    @Assisted val onGoBack: () -> Unit,
    private val engine: SpektraEngine,
    private val imageGetter: ImageGetter<Bitmap>,
    dispatchersHolder: DispatchersHolder
) : BaseComponent(dispatchersHolder, componentContext) {

    init {
        debounce {
            initialUri?.let(::pickImage)
        }
    }

    // --- source image ----------------------------------------------------------

    private val _uri: MutableState<Uri?> = mutableStateOf(null)
    val uri: Uri? by _uri

    private val _sourceBitmap: MutableState<Bitmap?> = mutableStateOf(null)
    val sourceBitmap: Bitmap? by _sourceBitmap

    // --- profiles --------------------------------------------------------------

    /** Profile ids bundled in spektra-core assets (film + paper). */
    val availableProfiles: List<String> by lazy {
        runCatching { engine.listProfiles() }.getOrDefault(emptyList())
    }

    private val _filmProfile: MutableState<String> =
        mutableStateOf(availableProfiles.firstOrNull() ?: "kodak_portra_400")
    val filmProfile: String by _filmProfile

    private val _printProfile: MutableState<String> =
        mutableStateOf(availableProfiles.firstOrNull() ?: "kodak_endura_premier")
    val printProfile: String by _printProfile

    // --- params + result -------------------------------------------------------

    private val _params: MutableState<SpektraParams> = mutableStateOf(
        SpektraParams(filmProfile = _filmProfile.value, printProfile = _printProfile.value)
    )
    val params: SpektraParams by _params

    private val _renderMode: MutableState<RenderMode> = mutableStateOf(RenderMode.Preview)
    val renderMode: RenderMode by _renderMode

    private val _previewBitmap: MutableState<Bitmap?> = mutableStateOf(null)
    val previewBitmap: Bitmap? by _previewBitmap

    private val _isProcessing: MutableState<Boolean> = mutableStateOf(false)
    val isProcessing: Boolean by _isProcessing

    /** True while a before/after compare gesture is active (driven from the UI). */
    private val _showOriginal: MutableState<Boolean> = mutableStateOf(false)
    val showOriginal: Boolean by _showOriginal

    // --- intents ---------------------------------------------------------------

    fun pickImage(uri: Uri) {
        _uri.update { uri }
        componentScope.launch {
            runSuspendCatching {
                // TODO(host): for RAW/DNG this routes through the LibRaw-backed
                //  RawDecoder registered in core:data/coil; ImageGetter resolves it.
                _sourceBitmap.update { imageGetter.getImage(data = uri, size = 4000) }
            }.onFailure(AppToastHost::showFailureToast)
            runPreview()
        }
    }

    fun setFilmProfile(profile: String) {
        _filmProfile.update { profile }
        updateParams(params.copy(filmProfile = profile))
    }

    fun setPrintProfile(profile: String) {
        _printProfile.update { profile }
        updateParams(params.copy(printProfile = profile))
    }

    fun updateParams(newParams: SpektraParams) {
        _params.update { newParams }
        registerChanges()
        // Re-render the interactive preview as the user tunes sliders.
        debouncedImageCalculation(delay = 350L) { simulate(RenderMode.Preview) }
    }

    fun setRenderMode(mode: RenderMode) {
        _renderMode.update { mode }
    }

    fun setShowOriginal(show: Boolean) {
        _showOriginal.update { show }
    }

    fun runPreview() {
        _renderMode.update { RenderMode.Preview }
        debouncedImageCalculation(delay = 0L) { simulate(RenderMode.Preview) }
    }

    fun runFullScan() {
        _renderMode.update { RenderMode.FullScan }
        debouncedImageCalculation(delay = 0L) { simulate(RenderMode.FullScan) }
    }

    fun export(onResult: (Bitmap) -> Unit) {
        componentScope.launch {
            _isProcessing.update { true }
            runSuspendCatching {
                val result = simulateBlocking(RenderMode.FullScan)
                result?.let(onResult)
            }.onFailure(AppToastHost::showFailureToast)
            _isProcessing.update { false }
            // TODO(host): hand the resulting Bitmap to core:data ImageSaver /
            //  the standard save sheet (EXIF + ICC) like other feature screens.
        }
    }

    // --- engine ---------------------------------------------------------------

    private suspend fun simulate(mode: RenderMode) {
        val result = simulateBlocking(mode) ?: return
        _previewBitmap.update { result }
    }

    /**
     * Runs the native engine on [defaultDispatcher]. Returns null if there is no
     * source image yet. Converts the source Bitmap into a linear scene-referred
     * buffer, calls simulate/simulatePreview, and decodes the result to a Bitmap.
     */
    private suspend fun simulateBlocking(mode: RenderMode): Bitmap? {
        val source = _sourceBitmap.value ?: return null
        _isProcessing.update { true }
        return try {
            // TODO(host): convert Bitmap -> LinearImage (direct float ByteBuffer)
            //  via a core:data helper, and SimResult -> Bitmap on the way back.
            //  Kept abstract here so the screen compiles independently of that glue.
            val linear = source.toLinearImage(params)
            val sim = when (mode) {
                RenderMode.Preview -> engine.simulatePreview(linear, params)
                RenderMode.FullScan -> engine.simulate(linear, params)
            }
            sim.use { result ->
                try {
                    result.toBitmap().also {
                        result.reportOutcome(AppRenderOutcome.CONSUMED)
                    }
                } catch (cancelled: CancellationException) {
                    result.reportOutcome(AppRenderOutcome.CANCELLED)
                    throw cancelled
                } catch (failure: Throwable) {
                    result.reportOutcome(AppRenderOutcome.FAILED)
                    throw failure
                }
            }
        } finally {
            _isProcessing.update { false }
        }
    }

    @AssistedFactory
    fun interface Factory {
        operator fun invoke(
            componentContext: ComponentContext,
            initialUri: Uri?,
            onGoBack: () -> Unit,
        ): FilmEmulationComponent
    }
}

// --- bridging stubs --------------------------------------------------------------
// TODO(host): replace with real core:data converters once the host is seeded (M1).
//  These keep the screen self-contained and compilable in isolation.

private fun Bitmap.toLinearImage(
    params: SpektraParams
): com.spectrafilm.engine.LinearImage =
    throw NotImplementedError("Bitmap -> LinearImage conversion lands with core:data glue (M1)")

private fun com.spectrafilm.engine.SimResult.toBitmap(): Bitmap =
    throw NotImplementedError("SimResult -> Bitmap conversion lands with core:data glue (M1)")
