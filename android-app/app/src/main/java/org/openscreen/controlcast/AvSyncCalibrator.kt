package org.openscreen.controlcast

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleOwner
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import java.nio.ByteBuffer
import kotlin.math.abs

// Measures the Cast receiver's audio-video sync offset by:
// 1. Casting a flash+beep test pattern (2s interval)
// 2. Using the phone's camera to detect brightness spikes (flash on TV)
// 3. Using the phone's mic to detect audio peaks (beep from TV)
// 4. Computing the average time delta between visual and audio events
//
// Positive result = audio lags video (need to shift audio earlier).
class AvSyncCalibrator(private val context: Context) {

    data class CalibrationResult(
        val offsetMs: Long,
        val numSamples: Int,
        val message: String,
    )

    private val flashTimestamps = mutableListOf<Long>()  // nanos
    private val beepTimestamps = mutableListOf<Long>()   // nanos

    fun hasPermissions(): Boolean {
        return ContextCompat.checkSelfPermission(
            context, Manifest.permission.CAMERA
        ) == PackageManager.PERMISSION_GRANTED &&
            ContextCompat.checkSelfPermission(
                context, Manifest.permission.RECORD_AUDIO
            ) == PackageManager.PERMISSION_GRANTED
    }

    // Run calibration for the given duration while the test pattern is casting.
    // Returns the measured A/V offset.
    suspend fun calibrate(
        lifecycleOwner: LifecycleOwner,
        durationMs: Long = 16000,
    ): CalibrationResult {
        flashTimestamps.clear()
        beepTimestamps.clear()

        // Start camera brightness detection
        val cameraJob = startCameraDetection(lifecycleOwner)

        // Start mic peak detection
        val micJob = startMicDetection()

        // Wait for calibration period
        delay(durationMs)

        // Stop detection
        micJob.stop()
        cameraJob.stop()

        // Match flash and beep events
        return computeOffset()
    }

    private data class StoppableJob(val stop: () -> Unit)

    private suspend fun startCameraDetection(
        lifecycleOwner: LifecycleOwner,
    ): StoppableJob {
        var lastBrightness = 0.0
        var cooldown = 0L

        val provider = withContext(Dispatchers.Main) {
            val future = ProcessCameraProvider.getInstance(context)
            val provider = kotlin.coroutines.suspendCoroutine<ProcessCameraProvider> { cont ->
                future.addListener({ cont.resumeWith(Result.success(future.get())) },
                    ContextCompat.getMainExecutor(context))
            }

            val analysis = ImageAnalysis.Builder()
                .setTargetResolution(android.util.Size(160, 120))
                .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                .build()

            analysis.setAnalyzer(ContextCompat.getMainExecutor(context)) { image ->
                val brightness = computeBrightness(image)
                // Camera sensor timestamp is CLOCK_BOOTTIME.
                // Convert to CLOCK_MONOTONIC to match audio timestamps:
                // offset = BOOTTIME - MONOTONIC (constant for device uptime)
                val sensorTs = image.imageInfo.timestamp  // BOOTTIME nanos
                val now = System.nanoTime()  // MONOTONIC nanos
                val boottime = android.os.SystemClock.elapsedRealtimeNanos()
                val clockOffset = boottime - now  // BOOTTIME - MONOTONIC
                val sensorTsMono = sensorTs - clockOffset  // convert to MONOTONIC

                // Detect brightness spike (flash)
                if (brightness > lastBrightness * 1.5 &&
                    brightness > 40 &&
                    now - cooldown > 1_000_000_000L
                ) {
                    android.util.Log.i("Calibrate",
                        "Flash: brightness=${"%.1f".format(brightness)} camLatency=${(now-sensorTsMono)/1000000}ms")
                    synchronized(flashTimestamps) {
                        flashTimestamps.add(sensorTsMono)
                    }
                    cooldown = now
                }
                lastBrightness = brightness
                image.close()
            }

            provider.unbindAll()
            provider.bindToLifecycle(
                lifecycleOwner,
                CameraSelector.DEFAULT_BACK_CAMERA,
                analysis,
            )
            provider
        }

        return StoppableJob {
            provider.unbindAll()
        }
    }

    private fun computeBrightness(image: ImageProxy): Double {
        val plane = image.planes[0]
        val buffer = plane.buffer
        var sum = 0L
        val size = buffer.remaining().coerceAtMost(1000)  // Sample subset
        val step = buffer.remaining() / size
        for (i in 0 until size) {
            sum += (buffer.get(i * step).toInt() and 0xFF)
        }
        return sum.toDouble() / size
    }

    private fun startMicDetection(): StoppableJob {
        val sampleRate = 44100
        val bufSize = AudioRecord.getMinBufferSize(
            sampleRate,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
        )
        val recorder = AudioRecord(
            MediaRecorder.AudioSource.MIC,
            sampleRate,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
            bufSize,
        )

        val running = java.util.concurrent.atomic.AtomicBoolean(true)
        var cooldown = 0L
        val buffer = ShortArray(bufSize / 2)
        val audioTimestamp = android.media.AudioTimestamp()
        var totalFramesRead = 0L

        recorder.startRecording()
        Thread {
            while (running.get()) {
                val read = recorder.read(buffer, 0, buffer.size)
                if (read > 0) {
                    // Compute hardware capture timestamp from frame position.
                    // AudioRecord.getTimestamp() gives us a mapping from frame
                    // count to nanoseconds, removing audio pipeline latency.
                    val now = System.nanoTime()
                    var captureTs = now  // fallback
                    if (recorder.getTimestamp(audioTimestamp,
                            android.media.AudioTimestamp.TIMEBASE_MONOTONIC) ==
                        android.media.AudioRecord.SUCCESS) {
                        // Extrapolate: timestamp is for audioTimestamp.framePosition,
                        // we want the timestamp for the start of this buffer.
                        val frameDelta = totalFramesRead - audioTimestamp.framePosition
                        captureTs = audioTimestamp.nanoTime +
                            (frameDelta * 1_000_000_000L / sampleRate)
                    }
                    totalFramesRead += read

                    // Find peak amplitude
                    var maxAmp = 0
                    for (i in 0 until read) {
                        val amp = abs(buffer[i].toInt())
                        if (amp > maxAmp) maxAmp = amp
                    }
                    // Detect beep (loud peak)
                    if (maxAmp > 3000 && now - cooldown > 1_000_000_000L) {
                        android.util.Log.i("Calibrate",
                            "Beep: amp=$maxAmp captureTs=$captureTs wallTs=$now delta=${(now-captureTs)/1000000}ms")
                        synchronized(beepTimestamps) {
                            beepTimestamps.add(captureTs)
                        }
                        cooldown = now
                    }
                }
            }
            recorder.stop()
            recorder.release()
        }.start()

        return StoppableJob { running.set(false) }
    }

    private fun computeOffset(): CalibrationResult {
        val flashes = synchronized(flashTimestamps) { flashTimestamps.toList() }
        val beeps = synchronized(beepTimestamps) { beepTimestamps.toList() }

        if (flashes.isEmpty() || beeps.isEmpty()) {
            return CalibrationResult(
                offsetMs = 0,
                numSamples = 0,
                message = "No events detected. Point camera at TV and ensure volume is up.",
            )
        }

        // Match each flash to the nearest beep within 500ms
        val deltas = mutableListOf<Long>()
        for (flash in flashes) {
            val nearestBeep = beeps.minByOrNull { abs(it - flash) } ?: continue
            val delta = nearestBeep - flash  // positive = beep after flash = audio lags
            if (abs(delta) < 500_000_000L) {  // within 500ms
                deltas.add(delta)
            }
        }

        if (deltas.isEmpty()) {
            return CalibrationResult(
                offsetMs = 0,
                numSamples = 0,
                message = "Could not match flash/beep events. Detected ${flashes.size} flashes, ${beeps.size} beeps.",
            )
        }

        val avgDeltaNs = deltas.average().toLong()
        val avgDeltaMs = avgDeltaNs / 1_000_000

        return CalibrationResult(
            offsetMs = avgDeltaMs,
            numSamples = deltas.size,
            message = "Measured offset: ${avgDeltaMs}ms from ${deltas.size} samples " +
                "(${flashes.size} flashes, ${beeps.size} beeps). " +
                if (avgDeltaMs > 0) "Audio lags video." else "Audio leads video.",
        )
    }
}
