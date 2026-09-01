/*
 * Spektrafilm for Android — non-production AImageDecoder experiment facade.
 * GPL-3.0-only.
 */
package com.spectrafilm.aimage

import android.os.Build
import com.spectrafilm.engine.DataLease
import com.spectrafilm.engine.LinearImage
import com.spectrafilm.engine.NativeBufferOwner
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CancellationException
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

sealed interface AImageEncodedSource {
    class FileDescriptor internal constructor(internal val fd: Int) : AImageEncodedSource {
        init {
            require(fd >= 0) { "file descriptor must be non-negative" }
        }
    }

    /**
     * Borrows the caller's remaining direct-buffer byte range without copying.
     * The duplicate isolates position/limit changes, not underlying writes: the
     * caller must keep every encoded byte immutable from construction until its
     * final probe/decode call returns. Qualification deliberately does not hash
     * the whole borrowed input inside a production timing window.
     */
    class DirectBuffer(encoded: ByteBuffer) : AImageEncodedSource {
        internal val bytes: ByteBuffer = encoded.duplicate().run {
            require(isDirect) { "encoded AImageDecoder buffer must be direct" }
            require(hasRemaining()) { "encoded AImageDecoder buffer must not be empty" }
            slice().order(encoded.order())
        }
    }

    companion object {
        fun fromFileDescriptor(fd: Int): AImageEncodedSource = FileDescriptor(fd)
        /** See [DirectBuffer] for the required encoded-byte immutability lifetime. */
        fun fromDirectBuffer(encoded: ByteBuffer): AImageEncodedSource = DirectBuffer(encoded)
    }
}

class AImageDecodeCancellation {
    private val requested = AtomicBoolean(false)
    private val pollCount = AtomicInteger(0)

    /**
     * Qualification-only seam. A null observer has no behavioral effect; the
     * androidTest runner uses it to request cancellation at an exact native
     * cleanup boundary instead of relying on a scheduler race.
     */
    @Volatile
    internal var testPollObserver: ((Int) -> Unit)? = null

    fun cancel() {
        requested.set(true)
    }

    fun isCancellationRequested(): Boolean {
        val count = pollCount.incrementAndGet()
        testPollObserver?.invoke(count)
        return requested.get()
    }

    internal fun throwIfCancelled() {
        if (isCancellationRequested()) throw CancellationException("AImageDecoder cancelled")
    }
}

/** Signals a deliberate fail-closed return to the existing Java decoder route. */
class AImageDecoderFallbackException(message: String) : RuntimeException(message)

/** Release-test canary: retained but deliberately renameable by module-local R8 rules. */
internal class AImageR8RenameCanary {
    fun runtimeClassName(): String = javaClass.name
}

internal const val AIMAGE_R8_CANARY_ORIGINAL_NAME =
    "com.spectrafilm.aimage.AImageR8RenameCanary"

data class AImageDecodeMetadata(
    val width: Int,
    val height: Int,
    val pixelFormat: AImagePixelFormat,
    val alphaFlags: Int,
    val dataSpace: Int,
    val strideBytes: Int,
    val byteCount: Int,
)

/** Exact-once owner for one client-provided AImageDecoder pixel buffer. */
class AImagePixelResult internal constructor(
    val header: AImageHeader,
    val plan: AImageDecodePlan,
    val metadata: AImageDecodeMetadata,
    private val pixels: NativeBufferOwner,
) : AutoCloseable {
    fun acquirePixels(): DataLease = pixels.acquireDataLease()

    /**
     * Convert the only admitted working-contract cell: unpremultiplied sRGB
     * RGBA_8888 to the app's linear ProPhoto RGB float buffer.
     */
    fun toLinearProPhoto(
        cancellation: AImageDecodeCancellation = AImageDecodeCancellation(),
    ): LinearImage {
        if (!plan.canConvertToLinearProPhoto) {
            throw AImageDecoderFallbackException(
                "${plan.pixelFormat} dataspace=${plan.outputDataSpace} is evidence-only; " +
                    "no linear ProPhoto conversion is qualified",
            )
        }
        cancellation.throwIfCancelled()
        val output = NativeBufferOwner.allocate(plan.linearProPhotoByteCount.toLong())
        try {
            acquirePixels().use { sourceLease ->
                output.acquireDataLease().use { outputLease ->
                    AImageDecoderNative.convertSrgb8888ToLinearProPhoto(
                        sourceLease.data,
                        metadata.strideBytes,
                        outputLease.data,
                        metadata.width,
                        metadata.height,
                        cancellation,
                    )
                }
            }
            cancellation.throwIfCancelled()
            return output.transferToLinearImage(
                metadata.width,
                metadata.height,
                colorSpace = "ProPhoto RGB",
            )
        } catch (failure: Throwable) {
            output.close()
            throw failure
        }
    }

    override fun close() {
        pixels.close()
    }
}

object AImageDecoderExperiment {
    val isPlatformAvailable: Boolean
        get() = Build.VERSION.SDK_INT >= 30

    fun probe(
        source: AImageEncodedSource,
        declaredMime: String? = null,
        allowDisplayReferredDngFallback: Boolean = false,
    ): AImageHeader {
        requirePlatform()
        AImageDecoderNative.ensureLoaded()
        val wire = when (source) {
            is AImageEncodedSource.FileDescriptor -> AImageDecoderNative.probeFd(
                source.fd,
                declaredMime,
                allowDisplayReferredDngFallback,
            )
            is AImageEncodedSource.DirectBuffer -> AImageDecoderNative.probeBuffer(
                source.bytes,
                declaredMime,
                allowDisplayReferredDngFallback,
            )
        }
        return parseAImageProbeWire(
            wire,
            declaredMime,
            allowDisplayReferredDngFallback,
        )
    }

    fun decodePixels(
        source: AImageEncodedSource,
        plan: AImageDecodePlan,
        cancellation: AImageDecodeCancellation = AImageDecodeCancellation(),
    ): AImagePixelResult {
        requirePlatform()
        requireCanonicalPlan(plan)
        cancellation.throwIfCancelled()
        AImageDecoderNative.ensureLoaded()
        val owner = NativeBufferOwner.allocate(plan.pixelByteCount.toLong())
        try {
            val response = owner.acquireDataLease().use { lease ->
                val pixels = lease.data.duplicate().order(ByteOrder.nativeOrder())
                when (source) {
                    is AImageEncodedSource.FileDescriptor -> AImageDecoderNative.decodeFd(
                        source.fd,
                        plan.header.normalizedDeclaredMimePolicy(),
                        plan.header.originalDngFallbackPolicy(),
                        plan.header.nativeProbeWire(),
                        plan.targetWidth,
                        plan.targetHeight,
                        plan.pixelFormat.androidBitmapFormat,
                        plan.outputDataSpace,
                        plan.setOutputDataSpace,
                        plan.requireUnpremultiplied,
                        pixels,
                        cancellation,
                    )
                    is AImageEncodedSource.DirectBuffer -> AImageDecoderNative.decodeBuffer(
                        source.bytes,
                        plan.header.normalizedDeclaredMimePolicy(),
                        plan.header.originalDngFallbackPolicy(),
                        plan.header.nativeProbeWire(),
                        plan.targetWidth,
                        plan.targetHeight,
                        plan.pixelFormat.androidBitmapFormat,
                        plan.outputDataSpace,
                        plan.setOutputDataSpace,
                        plan.requireUnpremultiplied,
                        pixels,
                        cancellation,
                    )
                }
            }
            cancellation.throwIfCancelled()
            val metadata = parseDecodeWire(response, plan)
            return AImagePixelResult(plan.header, plan, metadata, owner)
        } catch (failure: Throwable) {
            owner.close()
            throw failure
        }
    }

    /** Instrumentation-only leak counters: [live decoders, live duplicated fds]. */
    fun debugOutstandingNativeResources(): LongArray {
        if (!isPlatformAvailable) return longArrayOf(0L, 0L)
        AImageDecoderNative.ensureLoaded()
        return AImageDecoderNative.debugOutstandingResources()
    }

    private fun requirePlatform() {
        if (!isPlatformAvailable) {
            throw AImageDecoderFallbackException("AImageDecoder requires Android API 30+")
        }
    }

    private fun requireCanonicalPlan(plan: AImageDecodePlan) {
        val maxEdge = maxOf(plan.targetWidth, plan.targetHeight)
        val canonical = runCatching { planAImageDecode(plan.header, maxEdge) }
            .getOrElse { failure ->
                throw AImageDecoderFallbackException(
                    "AImageDecoder plan is outside the canonical bounded contract: " +
                        failure.message,
                )
            }
        if (plan != canonical) {
            throw AImageDecoderFallbackException(
                "AImageDecoder plan was modified after bounded planning",
            )
        }
    }
}

private fun parseDecodeWire(
    wire: String,
    plan: AImageDecodePlan,
): AImageDecodeMetadata {
    val fields = wire.split('\t')
    require(fields.size == 8 && fields[0] == AIMAGE_DECODE_WIRE_VERSION) {
        "unsupported AImageDecoder decode response"
    }
    val metadata = AImageDecodeMetadata(
        width = fields[1].toIntOrNull()
            ?: throw IllegalArgumentException("invalid decoded width"),
        height = fields[2].toIntOrNull()
            ?: throw IllegalArgumentException("invalid decoded height"),
        pixelFormat = when (fields[3].toIntOrNull()) {
            AImagePixelFormat.RGBA_8888.androidBitmapFormat -> AImagePixelFormat.RGBA_8888
            AImagePixelFormat.RGBA_F16.androidBitmapFormat -> AImagePixelFormat.RGBA_F16
            else -> throw IllegalArgumentException("unexpected decoded bitmap format")
        },
        alphaFlags = fields[4].toIntOrNull()
            ?: throw IllegalArgumentException("invalid decoded alpha flags"),
        dataSpace = fields[5].toIntOrNull()
            ?: throw IllegalArgumentException("invalid decoded dataspace"),
        strideBytes = fields[6].toIntOrNull()
            ?: throw IllegalArgumentException("invalid decoded stride"),
        byteCount = fields[7].toIntOrNull()
            ?: throw IllegalArgumentException("invalid decoded byte count"),
    )
    require(metadata.width == plan.targetWidth && metadata.height == plan.targetHeight) {
        "AImageDecoder returned unexpected geometry"
    }
    require(metadata.pixelFormat == plan.pixelFormat) {
        "AImageDecoder returned unexpected pixel format"
    }
    require(metadata.strideBytes == plan.rowStrideBytes &&
        metadata.byteCount == plan.pixelByteCount
    ) { "AImageDecoder returned unexpected buffer layout" }
    require(metadata.dataSpace == plan.outputDataSpace) {
        "AImageDecoder returned unexpected dataspace"
    }
    return metadata
}

internal object AImageDecoderNative {
    private object Library {
        init {
            System.loadLibrary("sfaimage")
        }

        fun ensureLoaded() = Unit
    }

    fun ensureLoaded() = Library.ensureLoaded()

    /** JNI_OnLoad asks the kept bridge for the actual (possibly renamed) class. */
    @JvmStatic
    private fun fallbackExceptionClass(): Class<*> =
        AImageDecoderFallbackException::class.java

    /** Stable release-test bridge; the returned canary name itself must be obfuscated. */
    @JvmStatic
    fun r8CanaryRuntimeClassName(): String = AImageR8RenameCanary().runtimeClassName()

    @JvmStatic external fun probeFd(
        fd: Int,
        declaredMime: String?,
        allowDngFallback: Boolean,
    ): String

    @JvmStatic external fun probeBuffer(
        encoded: ByteBuffer,
        declaredMime: String?,
        allowDngFallback: Boolean,
    ): String

    @JvmStatic external fun decodeFd(
        fd: Int,
        declaredMime: String,
        allowDngFallback: Boolean,
        expectedProbeWire: String,
        targetWidth: Int,
        targetHeight: Int,
        bitmapFormat: Int,
        outputDataSpace: Int,
        setOutputDataSpace: Boolean,
        requireUnpremultiplied: Boolean,
        pixels: ByteBuffer,
        cancellation: AImageDecodeCancellation,
    ): String

    @JvmStatic external fun decodeBuffer(
        encoded: ByteBuffer,
        declaredMime: String,
        allowDngFallback: Boolean,
        expectedProbeWire: String,
        targetWidth: Int,
        targetHeight: Int,
        bitmapFormat: Int,
        outputDataSpace: Int,
        setOutputDataSpace: Boolean,
        requireUnpremultiplied: Boolean,
        pixels: ByteBuffer,
        cancellation: AImageDecodeCancellation,
    ): String

    @JvmStatic external fun convertSrgb8888ToLinearProPhoto(
        rgba: ByteBuffer,
        rgbaStride: Int,
        rgbFloat: ByteBuffer,
        width: Int,
        height: Int,
        cancellation: AImageDecodeCancellation,
    )

    @JvmStatic external fun debugOutstandingResources(): LongArray
}
