/*
 * Spektrafilm for Android — capability-gated camera capture (#196). GPLv3.
 *
 * Two honest routes, decided by CameraRouteDecision at runtime:
 *  - RAW (DNG): a plain Camera2 session (CameraX cannot deliver RAW_SENSOR) writing the
 *    sensor image through DngCreator into a transactional MediaStore pending row. The DNG
 *    then enters the editor through the exact importer path a picked document uses, so the
 *    #190 precision descriptor and LibRaw route own its exactness — this screen never
 *    processes image data itself.
 *  - JPEG fallback: a CameraX preview/ImageCapture shell, labelled honestly — preview and
 *    JPEG output are display-referred platform processing, never called RAW parity.
 *
 * The destination is only offered when a probe succeeds AND the device runs API 29+ —
 * the MediaStore pending-row contract is the transactional primitive this capture uses,
 * and a camera tile that cannot capture is worse than no tile.
 */
package com.spectrafilm.app.camera

import android.Manifest
import android.content.ContentValues
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.DngCreator
import android.hardware.camera2.TotalCaptureResult
import android.graphics.ImageFormat
import android.media.Image
import android.media.ImageReader
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.provider.MediaStore
import android.view.Surface
import android.view.TextureView
import androidx.annotation.RequiresApi
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageCapture
import androidx.camera.core.ImageCaptureException
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import com.spectrafilm.app.Diag
import java.util.concurrent.atomic.AtomicBoolean

/** Camera capture is offered only where its transactional publication contract exists. */
fun cameraCaptureAvailable(context: Context): Boolean =
    Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q &&
        CameraRouteDecision.probe(context) != null

@Composable
fun CameraScreen(
    onClose: () -> Unit,
    onCaptured: (Uri) -> Unit,
) {
    val ctx = LocalContext.current
    var granted by remember {
        mutableStateOf(
            ContextCompat.checkSelfPermission(ctx, Manifest.permission.CAMERA) ==
                PackageManager.PERMISSION_GRANTED,
        )
    }
    var permanentlyDenied by remember { mutableStateOf(false) }
    val permission = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { ok ->
        granted = ok
        if (!ok) permanentlyDenied = true
    }
    val probe = remember { CameraRouteDecision.probe(ctx) }

    Box(Modifier.fillMaxSize().background(Color.Black)) {
        when {
            probe == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.Q -> CameraMessage(
                "Camera capture isn't available on this device.",
                onClose,
            )
            !granted -> Column(
                Modifier.align(Alignment.Center).padding(24.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text(
                    "Spektrafilm needs camera access to capture photos.",
                    color = Color.White,
                    style = MaterialTheme.typography.titleMedium,
                )
                if (permanentlyDenied) {
                    Text(
                        "Permission was declined. Grant Camera access in system Settings, then return here.",
                        color = Color.White.copy(alpha = 0.8f),
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
                Button(onClick = { permission.launch(Manifest.permission.CAMERA) }) {
                    Text("Grant camera access")
                }
                Button(onClick = onClose) { Text("Back") }
            }
            probe.second.rawSupported -> DngCameraShell(
                cameraId = probe.first,
                decision = probe.second,
                onClose = onClose,
                onCaptured = onCaptured,
            )
            else -> JpegCameraShell(
                routeLabel = probe.second.routeLabel,
                onClose = onClose,
                onCaptured = onCaptured,
            )
        }
    }
}

@Composable
private fun CameraMessage(text: String, onClose: () -> Unit) {
    Column(
        Modifier.fillMaxSize().padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(text, color = Color.White, style = MaterialTheme.typography.titleMedium)
        Button(onClick = onClose, modifier = Modifier.padding(top = 12.dp)) { Text("Back") }
    }
}

// ---------------------------------------------------------------------------------------
// JPEG fallback shell (CameraX) — honest label, platform processing.
// ---------------------------------------------------------------------------------------

@RequiresApi(Build.VERSION_CODES.Q)
@Composable
private fun JpegCameraShell(
    routeLabel: String,
    onClose: () -> Unit,
    onCaptured: (Uri) -> Unit,
) {
    val ctx = LocalContext.current
    @Suppress("DEPRECATION")
    val lifecycleOwner = androidx.compose.ui.platform.LocalLifecycleOwner.current
    val imageCapture = remember {
        ImageCapture.Builder()
            .setCaptureMode(ImageCapture.CAPTURE_MODE_MAXIMIZE_QUALITY)
            .build()
    }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }

    Box(Modifier.fillMaxSize()) {
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { viewCtx ->
                PreviewView(viewCtx).also { view ->
                    val future = ProcessCameraProvider.getInstance(viewCtx)
                    future.addListener({
                        runCatching {
                            val provider = future.get()
                            val preview = Preview.Builder().build()
                                .also { it.setSurfaceProvider(view.surfaceProvider) }
                            provider.unbindAll()
                            provider.bindToLifecycle(
                                lifecycleOwner,
                                CameraSelector.DEFAULT_BACK_CAMERA,
                                preview,
                                imageCapture,
                            )
                        }.onFailure { failure ->
                            Diag.w("camera preview bind failed: ${failure.message}")
                            error = "Camera unavailable: ${failure.message}"
                        }
                    }, ContextCompat.getMainExecutor(viewCtx))
                }
            },
        )
        CameraChrome(
            routeLabel = routeLabel,
            busy = busy,
            error = error,
            onClose = onClose,
            onShutter = {
                busy = true
                error = null
                val values = ContentValues().apply {
                    put(
                        MediaStore.Images.Media.DISPLAY_NAME,
                        "spektrafilm_capture_${System.currentTimeMillis()}.jpg",
                    )
                    put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
                    put(MediaStore.Images.Media.RELATIVE_PATH, "DCIM/Spektrafilm")
                }
                val output = ImageCapture.OutputFileOptions.Builder(
                    ctx.contentResolver,
                    MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
                    values,
                ).build()
                imageCapture.takePicture(
                    output,
                    ContextCompat.getMainExecutor(ctx),
                    object : ImageCapture.OnImageSavedCallback {
                        override fun onImageSaved(result: ImageCapture.OutputFileResults) {
                            busy = false
                            val uri = result.savedUri
                            if (uri != null) {
                                Diag.i("camera capture route=JPEG published")
                                onCaptured(uri)
                            } else {
                                error = "Capture saved but its location was lost"
                            }
                        }

                        override fun onError(exception: ImageCaptureException) {
                            busy = false
                            Diag.w("camera JPEG capture failed: ${exception.message}")
                            error = "Capture failed: ${exception.message}"
                        }
                    },
                )
            },
        )
    }
}

// ---------------------------------------------------------------------------------------
// RAW (DNG) shell — plain Camera2, transactional MediaStore pending row.
// ---------------------------------------------------------------------------------------

@RequiresApi(Build.VERSION_CODES.Q)
@Composable
private fun DngCameraShell(
    cameraId: String,
    decision: CameraRouteDecision,
    onClose: () -> Unit,
    onCaptured: (Uri) -> Unit,
) {
    val ctx = LocalContext.current
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    val controller = remember {
        DngCaptureController(
            context = ctx.applicationContext,
            cameraId = cameraId,
            rawWidth = decision.maxRawWidth,
            rawHeight = decision.maxRawHeight,
            onError = { message -> error = message; busy = false },
        )
    }
    DisposableEffect(Unit) {
        onDispose { controller.close() }
    }
    Box(Modifier.fillMaxSize()) {
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { viewCtx ->
                TextureView(viewCtx).also { view ->
                    view.surfaceTextureListener = controller.surfaceListener
                }
            },
        )
        CameraChrome(
            routeLabel = decision.routeLabel,
            busy = busy,
            error = error,
            onClose = onClose,
            onShutter = {
                busy = true
                error = null
                controller.captureDng { uri ->
                    busy = false
                    if (uri != null) {
                        Diag.i("camera capture route=DNG published")
                        onCaptured(uri)
                    }
                }
            },
        )
    }
}

/** Shared shutter/label/error chrome; the route label is always visible and honest. */
@Composable
private fun CameraChrome(
    routeLabel: String,
    busy: Boolean,
    error: String?,
    onClose: () -> Unit,
    onShutter: () -> Unit,
) {
    Column(
        Modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(
            routeLabel,
            color = Color.White,
            style = MaterialTheme.typography.labelLarge,
            modifier = Modifier
                .background(Color.Black.copy(alpha = 0.55f))
                .padding(horizontal = 8.dp, vertical = 4.dp),
        )
        Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
            error?.let {
                Text(
                    it,
                    color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier
                        .background(Color.Black.copy(alpha = 0.55f))
                        .padding(8.dp),
                )
            }
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Button(onClick = onClose, enabled = !busy) { Text("Back") }
                Button(onClick = onShutter, enabled = !busy) {
                    if (busy) {
                        CircularProgressIndicator(Modifier.padding(2.dp), color = Color.White)
                    } else {
                        Text("Capture")
                    }
                }
            }
        }
    }
}

/**
 * Plain Camera2 RAW capture. Owns the device/session/reader lifecycle; every failure path
 * closes what it opened and deletes any pending MediaStore row, so a cancelled or dying
 * capture leaks neither a file nor a stuck camera.
 */
@RequiresApi(Build.VERSION_CODES.Q)
internal class DngCaptureController(
    private val context: Context,
    private val cameraId: String,
    private val rawWidth: Int,
    private val rawHeight: Int,
    private val onError: (String) -> Unit,
) {
    private val thread = HandlerThread("spk-dng-capture").apply { start() }
    private val handler = Handler(thread.looper)
    private val closed = AtomicBoolean(false)

    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var reader: ImageReader? = null
    private var previewSurface: Surface? = null
    private var pendingCompletion: ((Uri?) -> Unit)? = null
    private var latchedResult: TotalCaptureResult? = null
    private var latchedImage: Image? = null

    val surfaceListener = object : TextureView.SurfaceTextureListener {
        override fun onSurfaceTextureAvailable(
            texture: android.graphics.SurfaceTexture,
            width: Int,
            height: Int,
        ) {
            texture.setDefaultBufferSize(width, height)
            previewSurface = Surface(texture)
            openCamera()
        }

        override fun onSurfaceTextureSizeChanged(
            texture: android.graphics.SurfaceTexture,
            width: Int,
            height: Int,
        ) = Unit

        override fun onSurfaceTextureDestroyed(texture: android.graphics.SurfaceTexture): Boolean {
            close()
            return true
        }

        override fun onSurfaceTextureUpdated(texture: android.graphics.SurfaceTexture) = Unit
    }

    private fun fail(message: String) {
        Diag.w("camera DNG route: $message")
        pendingCompletion?.invoke(null)
        pendingCompletion = null
        Handler(context.mainLooper).post { onError(message) }
    }

    @Suppress("MissingPermission") // The shell only composes this after CAMERA is granted.
    private fun openCamera() {
        if (closed.get()) return
        val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        reader = ImageReader.newInstance(rawWidth, rawHeight, ImageFormat.RAW_SENSOR, 2).also {
            it.setOnImageAvailableListener({ r ->
                latchedImage?.close()
                latchedImage = runCatching { r.acquireLatestImage() }.getOrNull()
                maybeWriteDng()
            }, handler)
        }
        runCatching {
            manager.openCamera(cameraId, object : CameraDevice.StateCallback() {
                override fun onOpened(camera: CameraDevice) {
                    if (closed.get()) { camera.close(); return }
                    device = camera
                    createSession(camera)
                }

                override fun onDisconnected(camera: CameraDevice) {
                    camera.close()
                    device = null
                    fail("camera disconnected")
                }

                override fun onError(camera: CameraDevice, code: Int) {
                    camera.close()
                    device = null
                    fail("camera error $code")
                }
            }, handler)
        }.onFailure { fail("camera open failed: ${it.message}") }
    }

    private fun createSession(camera: CameraDevice) {
        val preview = previewSurface ?: return fail("preview surface missing")
        val raw = reader?.surface ?: return fail("raw reader missing")
        runCatching {
            @Suppress("DEPRECATION") // The list-based overload is the minSdk-safe API here.
            camera.createCaptureSession(
                listOf(preview, raw),
                object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(configured: CameraCaptureSession) {
                        if (closed.get()) return
                        session = configured
                        runCatching {
                            val request =
                                camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW)
                                    .apply { addTarget(preview) }
                                    .build()
                            configured.setRepeatingRequest(request, null, handler)
                        }.onFailure { fail("preview start failed: ${it.message}") }
                    }

                    override fun onConfigureFailed(failed: CameraCaptureSession) {
                        fail("camera session configuration failed")
                    }
                },
                handler,
            )
        }.onFailure { fail("session creation failed: ${it.message}") }
    }

    /** One RAW still. Completion runs on the main thread with the published URI or null. */
    fun captureDng(completion: (Uri?) -> Unit) {
        val camera = device ?: return run { onError("camera not ready"); completion(null) }
        val activeSession = session ?: return run { onError("camera not ready"); completion(null) }
        val raw = reader?.surface ?: return run { onError("camera not ready"); completion(null) }
        pendingCompletion = { uri -> Handler(context.mainLooper).post { completion(uri) } }
        latchedResult = null
        latchedImage?.close()
        latchedImage = null
        runCatching {
            val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE)
                .apply {
                    addTarget(raw)
                    previewSurface?.let(::addTarget)
                }
                .build()
            activeSession.capture(
                request,
                object : CameraCaptureSession.CaptureCallback() {
                    override fun onCaptureCompleted(
                        completedSession: CameraCaptureSession,
                        completedRequest: android.hardware.camera2.CaptureRequest,
                        result: TotalCaptureResult,
                    ) {
                        latchedResult = result
                        maybeWriteDng()
                    }

                    override fun onCaptureFailed(
                        failedSession: CameraCaptureSession,
                        failedRequest: android.hardware.camera2.CaptureRequest,
                        failure: android.hardware.camera2.CaptureFailure,
                    ) {
                        fail("RAW capture failed (reason ${failure.reason})")
                    }
                },
                handler,
            )
        }.onFailure { fail("RAW capture request failed: ${it.message}") }
    }

    /** Both halves (sensor image + total result) must exist before DngCreator can write. */
    private fun maybeWriteDng() {
        val image = latchedImage ?: return
        val result = latchedResult ?: return
        val completion = pendingCompletion ?: run { image.close(); latchedImage = null; return }
        pendingCompletion = null
        latchedResult = null
        latchedImage = null

        val resolver = context.contentResolver
        val values = ContentValues().apply {
            put(
                MediaStore.Images.Media.DISPLAY_NAME,
                "spektrafilm_capture_${System.currentTimeMillis()}.dng",
            )
            put(MediaStore.Images.Media.MIME_TYPE, "image/x-adobe-dng")
            put(MediaStore.Images.Media.RELATIVE_PATH, "DCIM/Spektrafilm")
            put(MediaStore.Images.Media.IS_PENDING, 1)
        }
        var row: Uri? = null
        try {
            val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
            val characteristics = manager.getCameraCharacteristics(cameraId)
            row = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
                ?: error("MediaStore refused the pending row")
            resolver.openOutputStream(row)?.use { output ->
                DngCreator(characteristics, result).use { dng ->
                    val sensorOrientation =
                        characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0
                    dng.setOrientation(exifOrientationForSensor(sensorOrientation))
                    dng.writeImage(output, image)
                }
            } ?: error("MediaStore row is not writable")
            resolver.update(
                row,
                ContentValues().apply { put(MediaStore.Images.Media.IS_PENDING, 0) },
                null,
                null,
            )
            completion(row)
        } catch (failure: Exception) {
            // Transactional: a partial DNG never becomes visible — the pending row dies here.
            row?.let { runCatching { resolver.delete(it, null, null) } }
            pendingCompletion = null
            completion(null)
            Handler(context.mainLooper).post { onError("DNG publish failed: ${failure.message}") }
        } finally {
            image.close()
        }
    }

    fun close() {
        if (!closed.compareAndSet(false, true)) return
        runCatching { session?.close() }
        runCatching { device?.close() }
        runCatching { reader?.close() }
        runCatching { latchedImage?.close() }
        runCatching { previewSurface?.release() }
        session = null
        device = null
        reader = null
        latchedImage = null
        pendingCompletion = null
        thread.quitSafely()
    }

    companion object {
        /** Sensor orientation (degrees) → the EXIF orientation constant DngCreator wants. */
        internal fun exifOrientationForSensor(sensorOrientationDegrees: Int): Int =
            when (((sensorOrientationDegrees % 360) + 360) % 360) {
                90 -> android.media.ExifInterface.ORIENTATION_ROTATE_90
                180 -> android.media.ExifInterface.ORIENTATION_ROTATE_180
                270 -> android.media.ExifInterface.ORIENTATION_ROTATE_270
                else -> android.media.ExifInterface.ORIENTATION_NORMAL
            }
    }
}
