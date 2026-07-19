package com.lumacore.lumacore

import android.content.ContentValues
import android.content.Context
import android.os.Build
import android.os.Environment
import android.os.Handler
import android.os.HandlerThread
import android.provider.MediaStore
import com.lumacore.native.LumaCoreBridge
import java.io.File
import java.io.FileInputStream

/**
 * Owns the recording lifecycle: start/stop against a render session it
 * borrows from [EffectsRenderController], and the app-files -> MediaStore
 * handoff on a successful stop. [encodeHandler] is deliberately separate
 * from the camera/audio/GL threads — encoding must never block frame
 * delivery to the live preview. Mirrors iOS's RecordingController.swift.
 */
class RecordingController(private val context: Context) {
    sealed class RecordingError(message: String) : Exception(message) {
        object AlreadyRecording : RecordingError("already recording")
        object NotRecording : RecordingError("not recording")
        object StartFailed : RecordingError("native startRecording failed")
        object StopFailed : RecordingError("native stopRecording failed")
    }

    private val bridge = LumaCoreBridge()
    private val encodeThread = HandlerThread("com.lumacore.recording.encode").apply { start() }
    private val encodeHandler = Handler(encodeThread.looper)

    private var session: Long = -1
    private var outputFile: File? = null

    @Volatile
    private var recording = false

    /**
     * [session] comes from [EffectsRenderController] — this controller never
     * creates or releases it, only borrows it for the duration of a
     * recording (one render session per camera lifecycle, not per recording,
     * same as iOS).
     */
    fun start(session: Long, width: Int, height: Int, bitrateKbps: Int = DEFAULT_BITRATE_KBPS, completion: (Result<String>) -> Unit) {
        encodeHandler.post {
            if (recording) {
                completion(Result.failure(RecordingError.AlreadyRecording))
                return@post
            }

            val file = makeOutputFile()
            val started = bridge.nativeStartRecording(session, file.absolutePath, bitrateKbps, width, height) == 0
            if (!started) {
                completion(Result.failure(RecordingError.StartFailed))
                return@post
            }

            this.session = session
            outputFile = file
            recording = true
            completion(Result.success(file.absolutePath))
        }
    }

    fun stop(completion: (Result<String>) -> Unit) {
        encodeHandler.post {
            val file = outputFile
            if (!recording || file == null) {
                completion(Result.failure(RecordingError.NotRecording))
                return@post
            }
            recording = false
            val stopped = bridge.nativeStopRecording(session) == 0
            session = -1
            outputFile = null

            if (!stopped) {
                completion(Result.failure(RecordingError.StopFailed))
                return@post
            }

            saveToGallery(file)
            completion(Result.success(file.absolutePath))
        }
    }

    private fun makeOutputFile(): File {
        val dir = context.getExternalFilesDir(Environment.DIRECTORY_MOVIES) ?: context.filesDir
        return File(dir, "lumacore_${System.currentTimeMillis()}.mp4")
    }

    // Add-only: inserts into MediaStore.Video via scoped storage (API 29+) —
    // no WRITE_EXTERNAL_STORAGE, mirrors iOS's PHPhotoLibrary add-only save.
    // API 24-28 fallback (needs WRITE_EXTERNAL_STORAGE pre-scoped-storage) is
    // an explicitly deferred gap — ai_plans/04 §D.2 flags this as not
    // blocking the emulator (API 34) path this round.
    private fun saveToGallery(file: File) {
        val values = ContentValues().apply {
            put(MediaStore.Video.Media.DISPLAY_NAME, file.name)
            put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                put(MediaStore.Video.Media.RELATIVE_PATH, Environment.DIRECTORY_MOVIES)
                put(MediaStore.Video.Media.IS_PENDING, 1)
            }
        }
        val resolver = context.contentResolver
        val uri = resolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values) ?: return
        resolver.openOutputStream(uri)?.use { out ->
            FileInputStream(file).use { input -> input.copyTo(out) }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            values.clear()
            values.put(MediaStore.Video.Media.IS_PENDING, 0)
            resolver.update(uri, values, null, null)
        }
    }

    companion object {
        private const val DEFAULT_BITRATE_KBPS = 6000
    }
}
