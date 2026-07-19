package com.lumacore.lumacore

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Size
import android.view.Surface
import androidx.camera.core.CameraSelector
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleOwner

/**
 * Owns the CameraX capture session and the raw AudioRecord audio capture
 * (CameraX has no audio-capture use case — unlike iOS's
 * AVCaptureAudioDataOutput, this needs a separate API entirely). Mirrors
 * iOS's CameraCaptureController.swift in spirit, but is necessarily coupled
 * to [EffectsRenderController] here: CameraX's SurfaceRequest only reveals
 * the negotiated frame resolution once binding happens, and that resolution
 * is exactly what the render session needs to initialize — on iOS these two
 * are decoupled because Metal doesn't need an OS window handle at all (see
 * ai_plans/04-android-camerax-gl-pipeline.md §C).
 */
class CameraCaptureController(
    private val activity: Activity,
    private val lifecycleOwner: LifecycleOwner,
) {
    data class StartResult(val width: Int, val height: Int, val sessionId: Long)

    sealed class CaptureError(message: String) : Exception(message) {
        object PermissionDenied : CaptureError("camera/audio permission denied")
        object RenderSessionFailed : CaptureError("native render session failed to initialize")
    }

    /** Delivered on [audioHandler]'s thread — never the main thread. */
    var onAudioSample: ((pcm: ByteArray, numFrames: Int, sampleRate: Int, numChannels: Int, ptsUs: Long) -> Unit)? = null

    private var cameraProvider: ProcessCameraProvider? = null
    private var preview: Preview? = null

    private val audioThread = HandlerThread("com.lumacore.camera.audio").apply { start() }
    private val audioHandler = Handler(audioThread.looper)

    @Volatile
    private var audioRunning = false

    private var pendingPermissionCompletion: ((Boolean) -> Unit)? = null

    // Video, then audio — the camera does not start at all without both
    // grants (no video-only fallback, same simplification iOS makes).
    fun start(
        resolutionPreset: String,
        effectsController: EffectsRenderController,
        flutterSurfaceTexture: android.graphics.SurfaceTexture,
        completion: (Result<StartResult>) -> Unit,
    ) {
        requestPermissionsIfNeeded { granted ->
            if (!granted) {
                completion(Result.failure(CaptureError.PermissionDenied))
                return@requestPermissionsIfNeeded
            }
            val providerFuture = ProcessCameraProvider.getInstance(activity)
            providerFuture.addListener({
                val provider = try {
                    providerFuture.get()
                } catch (e: Exception) {
                    completion(Result.failure(e))
                    return@addListener
                }
                cameraProvider = provider
                bindCamera(provider, resolutionPreset, effectsController, flutterSurfaceTexture, completion)
            }, ContextCompat.getMainExecutor(activity))
        }
    }

    private fun bindCamera(
        provider: ProcessCameraProvider,
        resolutionPreset: String,
        effectsController: EffectsRenderController,
        flutterSurfaceTexture: android.graphics.SurfaceTexture,
        completion: (Result<StartResult>) -> Unit,
    ) {
        @Suppress("DEPRECATION") // ResolutionSelector is the non-deprecated replacement; setTargetResolution keeps this first slice simple.
        val previewUseCase = Preview.Builder()
            .setTargetResolution(resolutionForPreset(resolutionPreset))
            .build()
        preview = previewUseCase

        previewUseCase.setSurfaceProvider { request ->
            val resolution = request.resolution
            // Flutter's own output SurfaceTexture defaults its buffer size to
            // a tiny placeholder until a producer defines one explicitly —
            // without this, the EGLSurface eglCreateWindowSurface derives
            // from it renders into that tiny buffer, which the Texture
            // widget then stretches across the whole preview area (looks
            // like a single flat, slowly-changing color, not a real image).
            flutterSurfaceTexture.setDefaultBufferSize(resolution.width, resolution.height)
            val flutterSurface = Surface(flutterSurfaceTexture)
            effectsController.start(flutterSurface, resolution.width, resolution.height) { session, camSurfaceTexture ->
                if (session == -1L || camSurfaceTexture == null) {
                    request.willNotProvideSurface()
                    completion(Result.failure(CaptureError.RenderSessionFailed))
                    return@start
                }

                camSurfaceTexture.setDefaultBufferSize(resolution.width, resolution.height)
                val cameraSurface = Surface(camSurfaceTexture)
                // Reused across frames — getTransformMatrix() just fills it,
                // no per-frame allocation needed.
                val transformMatrix = FloatArray(16)
                // Registered with the GL thread's Handler so this listener
                // callback already runs there — updateTexImage()/renderFrame()
                // must never run anywhere else (ai_plans/04 §B.2).
                camSurfaceTexture.setOnFrameAvailableListener({ texture ->
                    texture.updateTexImage()
                    texture.getTransformMatrix(transformMatrix)
                    effectsController.setCameraTransform(transformMatrix)
                    effectsController.renderFrame(texture.timestamp / 1000)
                }, effectsController.glHandler)

                request.provideSurface(cameraSurface, ContextCompat.getMainExecutor(activity)) {
                    cameraSurface.release()
                }

                startAudioCapture()
                completion(Result.success(StartResult(resolution.width, resolution.height, session)))
            }
        }

        provider.unbindAll()
        provider.bindToLifecycle(lifecycleOwner, CameraSelector.DEFAULT_BACK_CAMERA, previewUseCase)
    }

    private fun resolutionForPreset(preset: String): Size = when (preset) {
        "hd720" -> Size(1280, 720)
        "uhd4k" -> Size(3840, 2160)
        else -> Size(1920, 1080)
    }

    fun stop() {
        stopAudioCapture()
        cameraProvider?.unbindAll()
        cameraProvider = null
        preview = null
    }

    private fun requestPermissionsIfNeeded(completion: (Boolean) -> Unit) {
        val needed = listOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO).filter {
            ContextCompat.checkSelfPermission(activity, it) != PackageManager.PERMISSION_GRANTED
        }
        if (needed.isEmpty()) {
            completion(true)
            return
        }
        pendingPermissionCompletion = completion
        androidx.core.app.ActivityCompat.requestPermissions(activity, needed.toTypedArray(), PERMISSION_REQUEST_CODE)
    }

    /** Call from MainActivity.onRequestPermissionsResult. */
    fun onPermissionsResult(requestCode: Int, grantResults: IntArray) {
        if (requestCode != PERMISSION_REQUEST_CODE) return
        val completion = pendingPermissionCompletion ?: return
        pendingPermissionCompletion = null
        completion(grantResults.isNotEmpty() && grantResults.all { it == PackageManager.PERMISSION_GRANTED })
    }

    private fun startAudioCapture() {
        audioRunning = true
        audioHandler.post { audioLoop() }
    }

    private fun stopAudioCapture() {
        // Only flip the flag — audioLoop() (on audioHandler's own thread)
        // notices it and stops/releases its AudioRecord itself. AudioRecord
        // isn't safe to stop/release from a thread other than the one
        // driving read(), so this never touches it directly.
        audioRunning = false
    }

    // NOTE: ptsUs here is SystemClock.elapsedRealtimeNanos()-based, while
    // video's ptsUs comes from SurfaceTexture.timestamp (camera HAL
    // timestamp, elapsedRealtime-based on most modern devices but not
    // universally guaranteed) — close enough for a first vertical slice
    // (ai_plans/04 doesn't specify exact av-sync tooling), exact clock
    // reconciliation is a fast-follow if drift shows up on real hardware.
    private fun audioLoop() {
        val sampleRate = 44100
        val channelConfig = AudioFormat.CHANNEL_IN_MONO
        val encoding = AudioFormat.ENCODING_PCM_16BIT
        val minBufferSize = AudioRecord.getMinBufferSize(sampleRate, channelConfig, encoding)
        if (minBufferSize <= 0) {
            audioRunning = false
            return
        }
        val bufferSize = minBufferSize * 2

        val record = try {
            AudioRecord(MediaRecorder.AudioSource.MIC, sampleRate, channelConfig, encoding, bufferSize)
        } catch (e: SecurityException) {
            audioRunning = false
            return
        }
        if (record.state != AudioRecord.STATE_INITIALIZED) {
            record.release()
            audioRunning = false
            return
        }

        record.startRecording()
        val chunk = ByteArray(bufferSize)
        while (audioRunning) {
            val read = record.read(chunk, 0, chunk.size)
            if (read > 0) {
                val ptsUs = SystemClock.elapsedRealtimeNanos() / 1000
                val numFrames = read / 2 // 16-bit mono PCM => 2 bytes/frame
                val payload = if (read == chunk.size) chunk else chunk.copyOf(read)
                onAudioSample?.invoke(payload, numFrames, sampleRate, 1, ptsUs)
            }
        }
        record.stop()
        record.release()
    }

    companion object {
        private const val PERMISSION_REQUEST_CODE = 6417
    }
}
