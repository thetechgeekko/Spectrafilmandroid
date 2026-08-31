/*
 * Spektrafilm for Android — lib:libraw Coil 3 decoder (secondary integration point).
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Uses statically included, dual-offered LibRaw; distribution is governed by the
 * bundled decision record and fail-closed release audit.
 *
 * This is the "full-res RAW in the gallery" integration point from docs/RAW_DNG.md:
 * a Coil 3 Decoder.Factory that decodes camera RAW / DNG sensor data rather than
 * relying only on an embedded preview (ImageToolbox's existing NefDecoder stays
 * available for fast thumbnails).
 *
 * It reuses RawDecoder for the sensor decode, then makes an unmanaged 8-bit
 * channel-wise preview for Coil. The engine path keeps the float32 linear ProPhoto
 * buffer; this preview path is not a color-managed ProPhoto-to-sRGB conversion.
 *
 * --------------------------------------------------------------------------------
 * WHERE TO REGISTER (host app)
 * --------------------------------------------------------------------------------
 * In ImageToolbox:
 *   core/data/src/main/java/.../core/data/di/ImageLoaderModule.kt
 *   -> provideComponentRegistry(...): ComponentRegistry.Builder()
 *        ...
 *        add(NefDecoder.Factory())          // existing: NEF preview (thumbnails)
 *        add(RawCoilDecoder.Factory())      // ADD: full-res LibRaw RAW/DNG open
 *        ...
 * Register it *before* generic bitmap decoders so RAW extensions are claimed here.
 * The class would live alongside NefDecoder in core/data/.../coil/ in the host;
 * this copy in lib:libraw is the reference sketch the host wires up.
 * --------------------------------------------------------------------------------
 *
 * NOTE: imports are written against Coil 3's API. They resolve once this module
 * (or the host) depends on coil3 (already present in ImageToolbox). Marked with
 * TODO where host-side tone-mapping choices are made.
 */
package com.spectrafilm.libraw

import android.graphics.Bitmap
import coil3.ImageLoader
import coil3.asImage
import coil3.decode.DecodeResult
import coil3.decode.Decoder
import coil3.decode.ImageSource
import coil3.fetch.SourceFetchResult
import coil3.request.Options

class RawCoilDecoder private constructor(
    private val source: ImageSource,
    private val options: Options,
) : Decoder {

    override suspend fun decode(): DecodeResult? {
        val bitmap = runRawCoilDecode(onLateResult = Bitmap::recycle) { cancellation ->
            val bytes = source.source().peek().inputStream().use { input ->
                RawInputLimits.readBounded(input) {
                    cancellation.isCancellationRequested
                }
            }

            val linear =
                // Gallery open uses as-shot WB (what the camera intended); the editor
                // screen lets the user pick other modes through RawDecoder.Settings.
                RawDecoder.decodeToLinear(
                    bytes,
                    RawDecoder.Settings(WhiteBalance.AS_SHOT),
                    cancellation,
                )

            // The decode result owns a native (off-heap) buffer the GC never sees; the
            // display bitmap is a managed copy, so free the native side unconditionally.
            linear.use {
                val pixels = rawCoilPreviewPixels(it, cancellation)
                cancellation.throwIfCancellationRequested()
                val created = Bitmap.createBitmap(
                    pixels,
                    it.width,
                    it.height,
                    Bitmap.Config.ARGB_8888,
                )
                try {
                    cancellation.throwIfCancellationRequested()
                    created
                } catch (failure: Throwable) {
                    created.recycle()
                    throw failure
                }
            }
        } ?: return null

        return try {
            DecodeResult(
                image = bitmap.asImage(),
                isSampled = false,
            )
        } catch (failure: Throwable) {
            bitmap.recycle()
            throw failure
        }
    }

    class Factory : Decoder.Factory {
        override fun create(
            result: SourceFetchResult,
            options: Options,
            imageLoader: ImageLoader,
        ): Decoder? {
            // Claim only files whose name/Uri ends in a supported RAW extension.
            val name = options.diskCacheKey
                ?: result.source.file().name
            return if (RawDecoder.isRawFileName(name)) {
                RawCoilDecoder(result.source, options)
            } else {
                null
            }
        }
    }
}
