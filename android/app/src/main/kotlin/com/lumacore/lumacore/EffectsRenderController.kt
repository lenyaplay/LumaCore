package com.lumacore.lumacore

import android.content.Context
import android.graphics.SurfaceTexture
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.PowerManager
import android.view.Surface
import com.lumacore.native.LumaCoreBridge

/**
 * Owns the render session for the camera's entire lifecycle (created in
 * startCamera, destroyed in stopCamera) — NOT recreated per recording, same
 * principle as iOS's EffectsRenderController.swift.
 *
 * Also owns the single dedicated GL thread every EGL/GLES-touching native
 * call must run on: GLRenderBackend's EGLContext is bound to whichever
 * thread creates it and stays there for its whole lifetime (unlike Metal —
 * see ai_plans/04-android-camerax-gl-pipeline.md §B.2). RenderPipeline/
 * IRenderBackend enforce none of this themselves; it's entirely this
 * class's job.
 */
class EffectsRenderController(private val context: Context) {
    private val bridge = LumaCoreBridge()
    private val glThread = HandlerThread("com.lumacore.render.gl").apply { start() }
    val glHandler = Handler(glThread.looper)
    private val mainHandler = Handler(Looper.getMainLooper())

    @Volatile
    var session: Long = -1
        private set

    private var powerManager: PowerManager? = null
    private var thermalListener: PowerManager.OnThermalStatusChangedListener? = null

    /**
     * Mirrors iOS's start(width:height:) -> Int64, but Android additionally
     * needs the destination Surface (Flutter's preview texture) up front to
     * create the EGLSurface, and hands back the camera-input SurfaceTexture
     * the caller must wire into CameraX's Preview.SurfaceProvider (see
     * CameraCaptureController). [onReady] runs on the main thread.
     */
    fun start(surface: Surface, width: Int, height: Int, onReady: (session: Long, cameraSurfaceTexture: SurfaceTexture?) -> Unit) {
        glHandler.post {
            val windowPtr = bridge.nativeAcquireWindow(surface)
            val newSession = if (windowPtr != 0L) bridge.nativeRenderInit(windowPtr, width, height) else -1L
            if (newSession == -1L) {
                mainHandler.post { onReady(-1, null) }
                return@post
            }
            val camTexId = bridge.nativeGetCameraTextureId(newSession)
            if (camTexId < 0) {
                bridge.nativeRelease(newSession)
                mainHandler.post { onReady(-1, null) }
                return@post
            }
            // SurfaceTexture(Int) must be constructed with the owning GL
            // context current on the calling thread — this is that thread.
            val cameraSurfaceTexture = SurfaceTexture(camTexId)
            session = newSession
            observeThermalState()
            mainHandler.post { onReady(newSession, cameraSurfaceTexture) }
        }
    }

    /**
     * Call from the camera-input SurfaceTexture's OnFrameAvailableListener,
     * right after updateTexImage() and before [renderFrame] — refreshes the
     * sensor-orientation correction the color-correction shader applies when
     * sampling the camera texture. Same GL-thread requirement as renderFrame.
     */
    fun setCameraTransform(matrix: FloatArray) {
        val s = session
        if (s == -1L) return
        bridge.nativeSetCameraTransform(s, matrix)
    }

    /**
     * Call from the camera-input SurfaceTexture's OnFrameAvailableListener,
     * which must be registered with [glHandler] so this already runs on the
     * GL thread — no further posting here (one-GL-thread rule above).
     */
    fun renderFrame(ptsUs: Long) {
        val s = session
        if (s == -1L) return
        bridge.nativeRenderFrame(s, ptsUs)
    }

    /**
     * Call from CameraCaptureController's audio callback. Bypasses the GL
     * pipeline entirely (audio has no GPU processing) — forwards straight to
     * the encoder internally if a recording is active, no-op otherwise. Not
     * GL-thread-affine (EncoderSession touches no EGL/GL state), called
     * directly from the audio thread.
     */
    fun submitAudioSample(pcm: ByteArray, numFrames: Int, sampleRate: Int, numChannels: Int, ptsUs: Long) {
        val s = session
        if (s == -1L) return
        bridge.nativeSubmitAudioFrame(s, pcm, numFrames, sampleRate, numChannels, ptsUs)
    }

    /**
     * Debug-only hook (see camera_screen.dart's long-press-to-throttle
     * overlay) — forces the reported thermal state without real device heat.
     */
    fun forceThermalStateForTesting(state: Int) {
        val s = session
        if (s == -1L) return
        glHandler.post { bridge.nativeSetThermalState(s, state) }
    }

    // PowerManager.OnThermalStatusChangedListener/getCurrentThermalStatus
    // need API 29+ — below that, the thermal-throttling ladder simply never
    // engages automatically (forceThermalStateForTesting still works from
    // Settings, this only affects the real-heat auto path).
    private fun observeThermalState() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        val pm = context.getSystemService(Context.POWER_SERVICE) as? PowerManager ?: return
        powerManager = pm

        val listener = PowerManager.OnThermalStatusChangedListener { status ->
            val s = session
            if (s != -1L) glHandler.post { bridge.nativeSetThermalState(s, mapThermalStatus(status)) }
        }
        thermalListener = listener
        pm.addThermalStatusListener(listener)
        val s = session
        glHandler.post { bridge.nativeSetThermalState(s, mapThermalStatus(pm.currentThermalStatus)) }
    }

    // PowerManager.THERMAL_STATUS_* (0..6) collapsed onto the shared 0..3
    // ladder (nominal/fair/serious/critical) RenderPipeline expects — same
    // semantics as iOS's ProcessInfo.ThermalState, coarser granularity.
    private fun mapThermalStatus(status: Int): Int = when (status) {
        PowerManager.THERMAL_STATUS_NONE, PowerManager.THERMAL_STATUS_LIGHT -> 0
        PowerManager.THERMAL_STATUS_MODERATE -> 1
        PowerManager.THERMAL_STATUS_SEVERE -> 2
        else -> 3 // CRITICAL, EMERGENCY, SHUTDOWN
    }

    fun stop() {
        powerManager?.let { pm -> thermalListener?.let { pm.removeThermalStatusListener(it) } }
        thermalListener = null
        powerManager = null
        val s = session
        if (s != -1L) {
            session = -1
            glHandler.post { bridge.nativeRelease(s) }
        }
    }
}
