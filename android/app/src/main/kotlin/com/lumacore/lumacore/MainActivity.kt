package com.lumacore.lumacore

import android.os.Bundle
import android.provider.Settings
import android.util.Size
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.LifecycleRegistry
import com.lumacore.native.LumaCoreBridge
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import io.flutter.view.TextureRegistry
import java.security.MessageDigest

/**
 * Platform Channel side of the boundary in ARCHITECTURE.md §3: license
 * activation/status, camera lifecycle (open/close/switch), gallery/export,
 * device fingerprint (ANDROID_ID). The render loop itself (camera -> C++ ->
 * GPU -> texture -> encoder) does not go through here — dart:ffi + JNI only.
 * 1:1 method/argument mirror of ios/Runner/AppDelegate.swift — Dart's
 * NativeChannel is platform-independent and unchanged.
 *
 * Implements LifecycleOwner by hand: plain io.flutter.embedding.android.
 * FlutterActivity extends android.app.Activity, not androidx.activity.
 * ComponentActivity, so it has no Lifecycle of its own — CameraX's
 * ProcessCameraProvider.bindToLifecycle() needs one.
 */
class MainActivity : FlutterActivity(), LifecycleOwner {
    private val channelName = "com.lumacore/native"

    private val lifecycleRegistry = LifecycleRegistry(this)
    override val lifecycle: Lifecycle
        get() = lifecycleRegistry

    private lateinit var cameraController: CameraCaptureController
    private lateinit var effectsController: EffectsRenderController
    private lateinit var recordingController: RecordingController

    // Separate from effectsController's internal bridge — license validation
    // is stateless and not tied to a render session's lifecycle.
    private val licenseBridge = LumaCoreBridge()

    private var textureRegistry: TextureRegistry? = null
    private var previewTextureEntry: TextureRegistry.SurfaceTextureEntry? = null

    // Set once in handleStartCamera from the actually-negotiated frame size —
    // recording must encode at the same dimensions the camera is producing.
    private var frameSize: Size? = null
    private var currentSessionId: Long = -1

    // Set via the "setRecordingSettings" channel call (Settings screen),
    // consumed on the next startCamera()/startRecording().
    private var pendingBitrateKbps = 6000
    private var pendingResolutionPreset = "hd1080"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        lifecycleRegistry.currentState = Lifecycle.State.CREATED

        effectsController = EffectsRenderController(applicationContext)
        cameraController = CameraCaptureController(this, this)
        recordingController = RecordingController(applicationContext)
        cameraController.onAudioSample = { pcm, numFrames, sampleRate, numChannels, ptsUs ->
            effectsController.submitAudioSample(pcm, numFrames, sampleRate, numChannels, ptsUs)
        }
    }

    override fun onStart() {
        super.onStart()
        lifecycleRegistry.currentState = Lifecycle.State.STARTED
    }

    override fun onResume() {
        super.onResume()
        lifecycleRegistry.currentState = Lifecycle.State.RESUMED
    }

    override fun onPause() {
        lifecycleRegistry.currentState = Lifecycle.State.STARTED
        super.onPause()
    }

    override fun onStop() {
        lifecycleRegistry.currentState = Lifecycle.State.CREATED
        super.onStop()
    }

    override fun onDestroy() {
        lifecycleRegistry.currentState = Lifecycle.State.DESTROYED
        super.onDestroy()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        cameraController.onPermissionsResult(requestCode, grantResults)
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        textureRegistry = flutterEngine.renderer

        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, channelName)
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "getDeviceFingerprint" -> result.success(getDeviceFingerprint())
                    "validateLicense" -> {
                        val args = call.arguments as? Map<*, *>
                        val tokenBlobJson = args?.get("tokenBlobJson") as? String ?: ""
                        val deviceFingerprint = args?.get("deviceFingerprint") as? String ?: ""
                        result.success(licenseBridge.nativeValidateLicense(tokenBlobJson, deviceFingerprint))
                    }
                    "setRecordingSettings" -> {
                        val args = call.arguments as? Map<*, *>
                        (args?.get("bitrateKbps") as? Int)?.let { pendingBitrateKbps = it }
                        (args?.get("resolutionPreset") as? String)?.let { pendingResolutionPreset = it }
                        result.success(null)
                    }
                    "startCamera" -> handleStartCamera(result)
                    "stopCamera" -> {
                        cameraController.stop()
                        effectsController.stop()
                        currentSessionId = -1
                        frameSize = null
                        result.success(null)
                    }
                    "startRecording" -> handleStartRecording(result)
                    "stopRecording" -> handleStopRecording(result)
                    "forceThermalStateForTesting" -> {
                        val args = call.arguments as? Map<*, *>
                        val state = (args?.get("state") as? Int) ?: 0
                        effectsController.forceThermalStateForTesting(state)
                        result.success(null)
                    }
                    else -> result.notImplemented()
                }
            }
    }

    // SHA256(ANDROID_ID + packageName), see ARCHITECTURE.md §6 — mirrors
    // iOS's SHA256(keychainUUID + bundle_id). No Keychain-style
    // self-persisted UUID needed: ANDROID_ID is already stable across
    // launches until factory reset.
    private fun getDeviceFingerprint(): String {
        val androidId = Settings.Secure.getString(contentResolver, Settings.Secure.ANDROID_ID) ?: ""
        val digest = MessageDigest.getInstance("SHA-256").digest((androidId + packageName).toByteArray())
        return digest.joinToString("") { "%02x".format(it) }
    }

    private fun ensurePreviewTexture(): TextureRegistry.SurfaceTextureEntry {
        previewTextureEntry?.let { return it }
        val entry = textureRegistry!!.createSurfaceTexture()
        previewTextureEntry = entry
        return entry
    }

    private fun handleStartCamera(result: MethodChannel.Result) {
        val entry = ensurePreviewTexture()
        cameraController.start(pendingResolutionPreset, effectsController, entry.surfaceTexture()) { startResult ->
            runOnUiThread {
                startResult.fold(
                    onSuccess = { r ->
                        frameSize = Size(r.width, r.height)
                        currentSessionId = r.sessionId
                        result.success(
                            mapOf(
                                "textureId" to entry.id(),
                                "width" to r.width,
                                "height" to r.height,
                                "sessionId" to r.sessionId,
                            ),
                        )
                    },
                    onFailure = { error -> result.error("CAMERA_START_FAILED", error.message, null) },
                )
            }
        }
    }

    private fun handleStartRecording(result: MethodChannel.Result) {
        val size = frameSize
        if (size == null || currentSessionId == -1L) {
            result.error("CAMERA_NOT_STARTED", "startRecording called before startCamera", null)
            return
        }
        recordingController.start(currentSessionId, size.width, size.height, pendingBitrateKbps) { startResult ->
            runOnUiThread {
                startResult.fold(
                    onSuccess = { path -> result.success(path) },
                    onFailure = { error -> result.error("RECORDING_START_FAILED", error.message, null) },
                )
            }
        }
    }

    private fun handleStopRecording(result: MethodChannel.Result) {
        recordingController.stop { stopResult ->
            runOnUiThread {
                stopResult.fold(
                    onSuccess = { result.success(null) },
                    onFailure = { error -> result.error("RECORDING_STOP_FAILED", error.message, null) },
                )
            }
        }
    }
}
