/*
 * Spektrafilm for Android — GPU LUT preview (experimental). GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * An OpenGL ES 3.0 preview surface that applies the current film look to a linear
 * proxy via a baked 3D LUT, the way Lightroom keeps its loupe instant: the CPU
 * engine bakes a 33^3 `.cube` (SpektraEngine.bakeCubeLut) only when look params
 * change, and the GPU trilinearly samples it every frame — so pan/zoom and slider
 * nudges are immediate instead of one full CPU re-render per settle.
 *
 * SCOPE / FIDELITY: a 3D LUT captures only the POINTWISE tone/colour transform.
 * Grain, halation, diffusion glare, DIR-coupler diffusion and scanner unsharp are
 * spatial/stochastic and are forced OFF in the bake (see bakeCubeLut docs), so this
 * path is a fast *look* proxy; the full CPU render (and every export) still applies
 * them exactly. This is why the feature is opt-in and default-OFF.
 *
 * STATUS: compiles against GLES 3.0 (minSdk 24 guarantees it). NOT yet verified on a
 * real GPU in this environment — must be device-tested before it is enabled by
 * default. Wired behind AppSettings.gpuPreview; the editor falls back to the CPU
 * bitmap path whenever this is off or a LUT/proxy is unavailable.
 */
package com.spectrafilm.app

import android.opengl.GLES30
import android.opengl.GLSurfaceView
import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import com.spectrafilm.engine.LinearImage
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.lang.ref.WeakReference
import java.util.concurrent.atomic.AtomicReference
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * Parsed 3D LUT: [size]^3 RGB triples in blue-fastest order (matching .cube and the
 * engine's bakeCubeLut output), as a flat float array length size^3 * 3.
 */
class CubeLut(val size: Int, val rgb: FloatArray) {
    init {
        require(size in 2..256) { "implausible LUT size $size" }
        require(rgb.size == size * size * size * 3) { "LUT data ${rgb.size} != ${size}^3*3" }
    }

    companion object {
        /**
         * Parse Adobe/Resolve `.cube` text (as emitted by SpektraEngine.bakeCubeLut).
         * Tolerates comments (`#`), the `LUT_3D_SIZE N` header, optional `DOMAIN_*`
         * and `TITLE` lines, and N^3 whitespace-separated RGB rows. Returns null on
         * any malformed input so the caller can fall back to the CPU path.
         */
        fun parse(cube: String): CubeLut? = runCatching {
            var size = -1
            val vals = ArrayList<Float>(35937 * 3)
            for (raw in cube.lineSequence()) {
                val line = raw.trim()
                if (line.isEmpty() || line.startsWith("#")) continue
                when {
                    line.startsWith("LUT_3D_SIZE") ->
                        size = line.substringAfter("LUT_3D_SIZE").trim().toInt()
                    line.startsWith("TITLE") || line.startsWith("DOMAIN_") ||
                        line.startsWith("LUT_1D_SIZE") -> { /* metadata: ignore */ }
                    else -> {
                        val p = line.split(Regex("\\s+"))
                        if (p.size >= 3) {
                            vals.add(p[0].toFloat()); vals.add(p[1].toFloat()); vals.add(p[2].toFloat())
                        }
                    }
                }
            }
            if (size < 2 || vals.size != size * size * size * 3) return null
            CubeLut(size, vals.toFloatArray())
        }.getOrNull()
    }
}

/**
 * Compose host for the GPU LUT preview. Re-uploads the proxy/LUT when [proxy] or
 * [lut] identity changes. The caller is responsible for only showing this when both
 * are non-null and the experimental setting is on; otherwise it shows the CPU bitmap.
 *
 * [onUnavailable] is invoked (once, on the main thread) if the GL program fails to
 * build — i.e. the device/driver can't run this path — so the caller can fall back to
 * the CPU bitmap instead of showing a black surface. This is what makes the feature
 * safe to enable by default: a GL failure degrades gracefully to the proven CPU path.
 */
@Composable
fun GpuLutPreview(
    proxy: LinearImage,
    lut: CubeLut,
    modifier: Modifier = Modifier,
    exposureGain: Float = 1f,
    onUnavailable: () -> Unit = {},
) {
    val cb = rememberUpdatedState(onUnavailable)
    val renderer = remember { LutRenderer { cb.value() } }
    AndroidView(
        modifier = modifier,
        factory = { ctx ->
            GLSurfaceView(ctx).apply {
                setEGLContextClientVersion(3)
                setRenderer(renderer)
                renderMode = GLSurfaceView.RENDERMODE_WHEN_DIRTY
            }
        },
        update = { view ->
            renderer.submit(proxy, lut, exposureGain)
            view.requestRender()
        },
    )
}

/** Immutable renderer-owned copy of one proxy's exact logical pixel window. */
internal class ProxySnapshot private constructor(
    val width: Int,
    val height: Int,
    private val pixels: ByteBuffer,
) {
    /** Each GL/test reader gets independent position/limit state. */
    fun pixelsView(): ByteBuffer = pixels.asReadOnlyBuffer()
        .order(ByteOrder.nativeOrder())
        .apply {
            position(0)
            limit(pixels.capacity())
        }

    companion object {
        fun capture(image: LinearImage): ProxySnapshot {
            val limitMessage =
                "GPU proxy ${image.width}x${image.height} exceeds direct-buffer limits"
            val requiredLong = try {
                Math.multiplyExact(
                    Math.multiplyExact(image.width.toLong(), image.height.toLong()),
                    3L * Float.SIZE_BYTES.toLong(),
                )
            } catch (overflow: ArithmeticException) {
                throw IllegalArgumentException(limitMessage, overflow)
            }
            require(requiredLong in 1..Int.MAX_VALUE.toLong()) {
                limitMessage
            }
            val required = requiredLong.toInt()
            return image.acquireDataLease().use { lease ->
                val leased = lease.data
                require(leased.remaining() >= required) {
                    "GPU proxy logical window has ${leased.remaining()} bytes; requires $required"
                }
                val source = leased.slice().order(ByteOrder.nativeOrder()).apply {
                    limit(required)
                }
                val owned = ByteBuffer.allocateDirect(required).order(ByteOrder.nativeOrder())
                owned.put(source)
                owned.flip()
                ProxySnapshot(image.width, image.height, owned)
            }
        }
    }
}

/**
 * One-entry identity cache: Compose may call submit on every recomposition, while
 * a proxy's pixels are immutable. A weak source key avoids retaining a closed
 * LinearImage; the renderer retains only its bounded, independently owned copy.
 */
internal class ProxySnapshotCache {
    private var source: WeakReference<LinearImage>? = null
    private var snapshot: ProxySnapshot? = null

    @Synchronized
    fun snapshotOf(image: LinearImage): ProxySnapshot {
        val cached = snapshot
        if (source?.get() === image && cached != null) return cached
        return ProxySnapshot.capture(image).also {
            source = WeakReference(image)
            snapshot = it
        }
    }
}

/** One immutable GPU upload generation. Its proxy, LUT, and gain are never mixed. */
internal data class GpuSubmission(
    val proxy: ProxySnapshot,
    val lut: CubeLut,
    val exposureGain: Float,
)

/** Single-consumer latest-submission slot with atomic take and context restoration. */
internal class PendingGpuSubmission {
    private val pending = AtomicReference<GpuSubmission?>()

    fun publish(submission: GpuSubmission) {
        pending.set(submission)
    }

    fun restoreIfEmpty(submission: GpuSubmission?) {
        if (submission != null) pending.compareAndSet(null, submission)
    }

    fun take(): GpuSubmission? = pending.getAndSet(null)
}

/**
 * GLES 3.0 renderer: full-screen quad samples the linear proxy texture, looks the
 * colour up in the 3D LUT (trilinear), writes display RGB. Texture uploads are
 * deferred to the GL thread via [submit] + one pending submission.
 */
private class LutRenderer(private val onUnavailable: () -> Unit) : GLSurfaceView.Renderer {
    private val proxySnapshots = ProxySnapshotCache()
    private val pendingSubmission = PendingGpuSubmission()

    // Exposure gain (2^ev) applied to the proxy BEFORE the LUT lookup. The baked
    // LUT carries no auto-exposure — it cannot, since AE meters a whole image and
    // the bake's input is a synthetic lattice — so the gain has to be supplied
    // here or the render sits in the film curve's toe (dark, lifted shadows).
    // Comes from SpektraEngine.exposureGain, i.e. the engine's own metering, so it
    // matches what simulate() would apply. Not a texture, so no upload needed.
    private var gain: Float = 1f

    // Last successfully submitted generation, kept so a recreated GL context (surface
    // destroyed/recreated, e.g. backgrounding) can re-upload instead of staying black.
    @Volatile private var lastSubmission: GpuSubmission? = null

    private val mainHandler = Handler(Looper.getMainLooper())
    private var reportedFail = false

    private var program = 0
    private var proxyTex = 0
    private var lutTex = 0
    private var proxyW = 0
    private var proxyH = 0
    private var viewW = 0
    private var viewH = 0
    private var haveProxy = false
    private var haveLut = false

    fun submit(proxy: LinearImage, lut: CubeLut, exposureGain: Float) {
        // Snapshot synchronously while the caller still owns a valid LinearImage
        // lease. The GL thread and context-recreation path never retain or reopen
        // that image, so closing/swapping its cache lease cannot free pixels that
        // a deferred texture upload is about to read.
        val snapshot = proxySnapshots.snapshotOf(proxy)
        val submission = GpuSubmission(
            proxy = snapshot,
            lut = lut,
            exposureGain = if (exposureGain.isFinite() && exposureGain > 0f) {
                exposureGain
            } else {
                1f
            },
        )
        // Publish the restoration value first. A concurrent context recreation may
        // restore this generation early, while the following atomic set ensures the
        // next requested frame still observes it as the latest pending generation.
        lastSubmission = submission
        pendingSubmission.publish(submission)
    }

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        // A fresh GL context lost every texture: forget the uploads and re-arm the
        // pending slot from the last submission so onDrawFrame re-uploads.
        haveProxy = false
        haveLut = false
        pendingSubmission.restoreIfEmpty(lastSubmission)
        program = buildProgram(VERT, FRAG)
        if (program == 0) {
            // Shader compile/link failed on this device/driver — tell the caller so it can
            // fall back to the CPU bitmap rather than leave a black surface up.
            if (!reportedFail) { reportedFail = true; mainHandler.post(onUnavailable) }
            return
        }
        val tex = IntArray(2)
        GLES30.glGenTextures(2, tex, 0)
        proxyTex = tex[0]
        lutTex = tex[1]
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        viewW = width
        viewH = height
        GLES30.glViewport(0, 0, width, height)
    }

    override fun onDrawFrame(gl: GL10?) {
        pendingSubmission.take()?.let { submission ->
            uploadProxy(submission.proxy)
            uploadLut(submission.lut)
            gain = submission.exposureGain
        }

        GLES30.glClearColor(0f, 0f, 0f, 1f)
        GLES30.glClear(GLES30.GL_COLOR_BUFFER_BIT)
        if (!haveProxy || !haveLut || program == 0) return

        GLES30.glUseProgram(program)
        GLES30.glActiveTexture(GLES30.GL_TEXTURE0)
        GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, proxyTex)
        GLES30.glUniform1i(GLES30.glGetUniformLocation(program, "uProxy"), 0)
        GLES30.glActiveTexture(GLES30.GL_TEXTURE1)
        GLES30.glBindTexture(GLES30.GL_TEXTURE_3D, lutTex)
        GLES30.glUniform1i(GLES30.glGetUniformLocation(program, "uLut"), 1)
        // Letterbox: fit the proxy's aspect into the surface (CLAMP bars stay black) so the
        // image is never stretched — matching the CPU ContentScale.Fit preview.
        var sx = 1f
        var sy = 1f
        if (proxyW > 0 && proxyH > 0 && viewW > 0 && viewH > 0) {
            val imgA = proxyW.toFloat() / proxyH
            val viewA = viewW.toFloat() / viewH
            if (viewA > imgA) sx = imgA / viewA else sy = viewA / imgA
        }
        GLES30.glUniform2f(GLES30.glGetUniformLocation(program, "uScale"), sx, sy)
        GLES30.glUniform1f(GLES30.glGetUniformLocation(program, "uExposureGain"), gain)
        // Full-screen quad (triangle strip) from gl_VertexID — no VBO needed.
        GLES30.glDrawArrays(GLES30.GL_TRIANGLE_STRIP, 0, 4)
    }

    private fun uploadProxy(snapshot: ProxySnapshot) {
        proxyW = snapshot.width
        proxyH = snapshot.height
        // Interleaved RGB float32 -> RGB16F texture (filterable in GLES3). The driver
        // converts GL_FLOAT source to the half-float internal format on upload.
        val fb = snapshot.pixelsView().asFloatBuffer()
        GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, proxyTex)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_LINEAR)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_LINEAR)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE)
        GLES30.glPixelStorei(GLES30.GL_UNPACK_ALIGNMENT, 1)
        GLES30.glTexImage2D(
            GLES30.GL_TEXTURE_2D, 0, GLES30.GL_RGB16F, proxyW, proxyH, 0,
            GLES30.GL_RGB, GLES30.GL_FLOAT, fb,
        )
        haveProxy = true
    }

    private fun uploadLut(lut: CubeLut) {
        // bakeCubeLut emits blue-fastest (B varies fastest, then G, then R). GL's
        // glTexImage3D expects the first axis (width=R here) varying fastest, so we
        // map LUT axes to (width=B, height=G, depth=R) and sample tex(b,g,r) below —
        // keeping the engine's ordering without a CPU re-shuffle.
        val n = lut.size
        val fb: FloatBuffer = ByteBuffer.allocateDirect(lut.rgb.size * 4)
            .order(ByteOrder.nativeOrder()).asFloatBuffer()
        fb.put(lut.rgb); fb.position(0)
        GLES30.glBindTexture(GLES30.GL_TEXTURE_3D, lutTex)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_3D, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_LINEAR)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_3D, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_LINEAR)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_3D, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_3D, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_3D, GLES30.GL_TEXTURE_WRAP_R, GLES30.GL_CLAMP_TO_EDGE)
        GLES30.glPixelStorei(GLES30.GL_UNPACK_ALIGNMENT, 1)
        GLES30.glTexImage3D(
            GLES30.GL_TEXTURE_3D, 0, GLES30.GL_RGB16F, n, n, n, 0,
            GLES30.GL_RGB, GLES30.GL_FLOAT, fb,
        )
        haveLut = true
    }

    private fun buildProgram(vsrc: String, fsrc: String): Int {
        val vs = compile(GLES30.GL_VERTEX_SHADER, vsrc)
        val fs = compile(GLES30.GL_FRAGMENT_SHADER, fsrc)
        val p = GLES30.glCreateProgram()
        GLES30.glAttachShader(p, vs)
        GLES30.glAttachShader(p, fs)
        GLES30.glLinkProgram(p)
        val ok = IntArray(1)
        GLES30.glGetProgramiv(p, GLES30.GL_LINK_STATUS, ok, 0)
        if (ok[0] == 0) { GLES30.glDeleteProgram(p); return 0 }
        GLES30.glDeleteShader(vs); GLES30.glDeleteShader(fs)
        return p
    }

    private fun compile(type: Int, src: String): Int {
        val s = GLES30.glCreateShader(type)
        GLES30.glShaderSource(s, src)
        GLES30.glCompileShader(s)
        return s
    }

    companion object {
        // Full-screen quad from gl_VertexID (triangle strip, 4 verts); flip V so texture
        // row 0 is on top, and apply the letterbox scale so aspect is preserved.
        private const val VERT = """#version 300 es
            uniform vec2 uScale;
            out vec2 vUv;
            void main() {
                float x = float(gl_VertexID & 1);
                float y = float((gl_VertexID >> 1) & 1);
                vUv = vec2(x, 1.0 - y);
                gl_Position = vec4((vec2(x, y) * 2.0 - 1.0) * uScale, 0.0, 1.0);
            }
        """
        private const val FRAG = """#version 300 es
            precision highp float;
            precision highp sampler3D;
            in vec2 vUv;
            uniform sampler2D uProxy;
            uniform sampler3D uLut;
            uniform float uExposureGain;
            out vec4 fragColor;
            // EXACT inverse of shaper_to_linear in spektra.cpp. The LUT's entries are spaced
            // evenly in this encoded space, so the lookup must be indexed through it or
            // every value lands on the wrong lattice cell.
            vec3 shaperEncode(vec3 c) {
                return mix(c * 12.92,
                           1.055 * pow(max(c, 0.0), vec3(1.0 / 2.4)) - 0.055,
                           step(vec3(0.0031308), c));
            }
            void main() {
                // Auto-exposure gain FIRST: the LUT is baked at unity gain (a 3D LUT
                // cannot carry AE), so this reproduces the global scale simulate()
                // applies in preprocess_geometry before the film stage. Then clamp to
                // the LUT's [0,1] linear-ProPhoto domain.
                vec3 lin = texture(uProxy, vUv).rgb * uExposureGain;
                vec3 c = shaperEncode(clamp(lin, 0.0, 1.0));
                // LUT axes are (B,G,R) fastest->slowest (see uploadLut), so index (b,g,r).
                vec3 outc = texture(uLut, vec3(c.b, c.g, c.r)).rgb;
                fragColor = vec4(outc, 1.0);
            }
        """
    }
}
