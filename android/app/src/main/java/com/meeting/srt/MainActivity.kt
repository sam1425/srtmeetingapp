package com.meeting.srt

import android.Manifest
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.os.Bundle
import android.view.SurfaceHolder
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.core.content.ContextCompat
import com.meeting.srt.ui.MainScreen
import com.pedro.common.ConnectChecker
import com.pedro.library.srt.SrtCamera2
import com.pedro.library.view.OpenGlView
import com.pedro.library.util.SensorRotationManager
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.SimpleDateFormat
import java.util.*

class MainActivity : ComponentActivity(), ConnectChecker {

    private var srtCamera2: SrtCamera2? = null
    private var openGlView: OpenGlView? = null
    private var sensorRotationManager: SensorRotationManager? = null
    @Volatile private var isSurfaceReady = false
    @Volatile private var encodersReady = false

    // ── Reactive state ──────────────────────────────────────────────────
    private val activeProfile = mutableStateOf("High")
    val streamState = mutableStateOf<StreamState>(StreamState.Idle)
    val isAudioEnabledState = mutableStateOf(true)
    val logs = mutableStateListOf<String>()
    val allowVerticalState = mutableStateOf(false)
    val isPortraitState = mutableStateOf(false)
    val connectionQuality = mutableStateOf(ConnectionQuality.Good)

    // ── Connection / form state (survives recomposition) ────────────────
    private var ip by mutableStateOf("iduai.duckdns.org")
    private var port by mutableStateOf("9000")
    private var name by mutableStateOf("Default")
    private var selectedProfile by mutableStateOf("High")
    private var latencyMs by mutableStateOf(120)
    private var latencyAuto by mutableStateOf(true)

    private fun dimensionsForProfile(profile: String): Triple<Int, Int, Int> = when (profile) {
        "Medium" -> Triple(854, 480, 1000 * 1024)
        "Low"    -> Triple(640, 360, 500 * 1024)
        else     -> Triple(1280, 720, 2000 * 1024)
    }

    // ── Reconnection infrastructure ────────────────────────────────────
    private var reconnectJob: Job? = null
    @Volatile private var lastSrtUrl: String? = null

    // ── Network quality tracking ────────────────────────────────────────
    private var qualityPollJob: Job? = null
    @Volatile private var lastActualBitrate = 0L
    private fun targetBitrate(): Int = dimensionsForProfile(activeProfile.value).third
    companion object {
        private val dateFormat = SimpleDateFormat("HH:mm:ss.SSS", Locale.getDefault())
        private const val MAX_RECONNECT_ATTEMPTS = 5
        private const val INITIAL_BACKOFF_MS = 1000L
        private const val MAX_BACKOFF_MS = 15_000L
    }

    private val requestPermissionsLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        val cameraGranted = permissions[Manifest.permission.CAMERA] ?: false
        val audioGranted = permissions[Manifest.permission.RECORD_AUDIO] ?: false
        if (cameraGranted && audioGranted) {
            logMessage("Permissions granted. Starting preview...")
            startCameraPreview()
        } else {
            logMessage("Permissions denied! Camera and microphone permissions are required.")
            Toast.makeText(this, "Permissions are required for streaming", Toast.LENGTH_LONG).show()
        }
    }

    // ── Lifecycle ───────────────────────────────────────────────────────

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestedOrientation = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE

        val view = OpenGlView(this)
        openGlView = view
        srtCamera2 = SrtCamera2(view, this)

        sensorRotationManager = SensorRotationManager(
            this, /* avoidDuplicated = */ true, /* followUI = */ true
        ) { rotation, _ ->
            srtCamera2?.glInterface?.setRotation(rotation)
        }

        view.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                logMessage("Surface created.")
                isSurfaceReady = true
                if (hasPermissions()) startCameraPreview()
            }
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                logMessage("Surface destroyed.")
                isSurfaceReady = false
            }
        })

        setContent {
            MaterialTheme(
                colorScheme = androidx.compose.material3.darkColorScheme(
                    primary = Color(0xFFCBA6F7),
                    secondary = Color(0xFFB4BEFE),
                    background = Color(0xFF1E1E2E),
                    surface = Color(0xFF252538),
                    onBackground = Color(0xFFCDD6F4),
                    onSurface = Color(0xFFBAC2DE)
                )
            ) {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    MainScreen(
                        ip = ip,
                        port = port,
                        name = name,
                        selectedProfile = selectedProfile,
                        latencyMs = latencyMs,
                        latencyAuto = latencyAuto,
                        streamState = streamState.value,
                        connectionQuality = connectionQuality.value,
                        isAudioEnabled = isAudioEnabledState.value,
                        allowVertical = allowVerticalState.value,
                        logs = logs.toList(),
                        onIpChange = { ip = it },
                        onPortChange = { port = it },
                        onNameChange = { name = it },
                        onProfileChange = {
                            selectedProfile = it
                            activeProfile.value = it
                            encodersReady = false
                            val (w, h, br) = dimensionsForProfile(it)
                            logMessage("Selected profile: $it (${w}x${h}, ${br / 1024} Kbps)")
                        },
                        onLatencyMsChange = { latencyMs = it },
                        onLatencyAutoChange = { latencyAuto = it },
                        onAllowVerticalChange = { enabled ->
                            allowVerticalState.value = enabled
                            if (!enabled) {
                                isPortraitState.value = false
                                requestedOrientation = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                            }
                        },
                        onToggleStream = { toggleStream(ip, port, name) },
                        onToggleAudio = { toggleAudio() },
                        onSwitchCamera = { switchCamera() },
                        onToggleOrientation = { toggleOrientation() },
                        onLogsClear = { logs.clear() },
                        openGlView = openGlView
                    )
                }
            }
        }

        checkAndRequestPermissions()
    }

    override fun onResume() {
        super.onResume()
        sensorRotationManager?.start()
        if (hasPermissions() && srtCamera2?.isStreaming == false && srtCamera2?.isOnPreview == false) {
            startCameraPreview()
        }
    }

    override fun onPause() {
        super.onPause()
        sensorRotationManager?.start()
        stopQualityPolling()
        cancelReconnect()
        if (srtCamera2?.isStreaming == true) {
            srtCamera2?.stopStream()
            streamState.value = StreamState.Idle
        }
        if (srtCamera2?.isOnPreview == true) {
            srtCamera2?.stopPreview()
        }
        encodersReady = false
    }

    // ── Permissions ────────────────────────────────────────────────────

    private fun hasPermissions(): Boolean {
        return ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED &&
               ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
    }

    private fun checkAndRequestPermissions() {
        if (!hasPermissions()) {
            requestPermissionsLauncher.launch(
                arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)
            )
        } else {
            startCameraPreview()
        }
    }

    // ── Camera & encoders ──────────────────────────────────────────────

    private fun prepareEncoders(): Boolean {
        val (w, h, br) = dimensionsForProfile(activeProfile.value)
        if (srtCamera2?.prepareVideo(w, h, 30, br, 0) == true &&
            srtCamera2?.prepareAudio(128 * 1024, 44100, true, true, true) == true) {
            logMessage("Encoders ready: ${w}x${h} @ ${br / 1024} Kbps.")
            return true
        }
        logMessage("Failed to prepare encoders.")
        return false
    }

    private fun startCameraPreview() {
        if (!isSurfaceReady || !hasPermissions()) return
        if (srtCamera2?.isOnPreview == true) return
        try {
            encodersReady = prepareEncoders()
            srtCamera2?.startPreview()
            logMessage("Camera preview started.")
        } catch (e: Exception) {
            logMessage("Failed to start preview: ${e.message}")
        }
    }

    // ── Logging ────────────────────────────────────────────────────────

    private fun logMessage(message: String) {
        android.util.Log.d("MainActivity", message)
        runOnUiThread {
            logs.add("[${dateFormat.format(Date())}] $message")
        }
    }

    // ── ConnectChecker callbacks ───────────────────────────────────────

    override fun onConnectionStarted(url: String) {
        logMessage("Connecting: $url")
    }

    override fun onConnectionSuccess() {
        logMessage("Connection successful!")
        runOnUiThread {
            if (currentState is StreamState.Stopping || StreamState.Idle) return
            streamState.value = StreamState.Streaming
            startQualityPolling()
        }
    }

    override fun onConnectionFailed(reason: String) {
        logMessage("Connection failed: $reason")
        val currentState = streamState.value
        if (currentState is StreamState.Reconnecting) {
            return
        }
        if (currentState is StreamState.Stopping || currentState is StreamState.Idle) {
            if (currentState is StreamState.Stopping) streamState.value = StreamState.Idle
            return
        }
        stopQualityPolling()
        lifecycleScope.launch(Dispatchers.IO) {
            srtCamera2?.stopStream()
            withContext(Dispatchers.Main) {
                encodersReady = false
                Toast.makeText(this@MainActivity, "Connection Failed: $reason", Toast.LENGTH_SHORT).show()
                startReconnect()
            }
        }
    }

    override fun onNewBitrate(bitrate: Long) {
        lastActualBitrate = bitrate
    }

    override fun onDisconnect() {
        logMessage("Disconnected from server.")
        val currentState = streamState.value
        if (currentState is StreamState.Reconnecting ||
            currentState is StreamState.Stopping ||
            currentState is StreamState.Idle) {
            logMessage("Disconnect received (already handled by onConnectionFailed)")
            return
        }
        stopQualityPolling()
        lifecycleScope.launch(Dispatchers.IO) {
            srtCamera2?.stopStream()
            withContext(Dispatchers.Main) {
                encodersReady = false
                startReconnect()
            }
        }
    }

    override fun onAuthError() { logMessage("Authentication error.") }
    override fun onAuthSuccess() { logMessage("Authentication successful.") }

    // ── Reconnection ───────────────────────────────────────────────────

    private fun startReconnect() {
        val url = lastSrtUrl
        if (url == null) {
            logMessage("Cannot reconnect: no previous URL")
            streamState.value = StreamState.Idle
            return
        }
        cancelReconnect()
        reconnectJob = lifecycleScope.launch {
            var attempt = 1
            var backoff = INITIAL_BACKOFF_MS
            while (attempt <= MAX_RECONNECT_ATTEMPTS) {
                streamState.value = StreamState.Reconnecting(attempt, MAX_RECONNECT_ATTEMPTS)
                logMessage("Reconnect attempt $attempt/$MAX_RECONNECT_ATTEMPTS in ${backoff / 1000}s...")
                delay(backoff)
                if (streamState.value is StreamState.Stopping || streamState.value is StreamState.Idle) return@launch
                if (!encodersReady) {
                    logMessage("Preparing encoders for reconnect...")
                    withContext(Dispatchers.Main) {
                        encodersReady = prepareEncoders()
                    }
                    if (!encodersReady) {
                        logMessage("Reconnect: encoders not ready, waiting...")
                        delay(2000)
                        attempt++
                        backoff = (backoff * 2).coerceAtMost(MAX_BACKOFF_MS)
                        continue
                    }
                }
                logMessage("Reconnecting to $url")
                withContext(Dispatchers.IO) {
                    try {
                        srtCamera2?.startStream(url)
                    } catch (e: Exception) {
                        logMessage("Reconnect startStream failed: ${e.message}")
                    }
                }
                delay(3000)
                if (streamState.value is StreamState.Streaming) {
                    logMessage("Reconnection successful!")
                    return@launch
                }
                attempt++
                backoff = (backoff * 2).coerceAtMost(MAX_BACKOFF_MS)
            }
            logMessage("Reconnection failed after $MAX_RECONNECT_ATTEMPTS attempts.")
            withContext(Dispatchers.Main) {
                streamState.value = StreamState.Idle
                Toast.makeText(this@MainActivity, "Reconnection failed after $MAX_RECONNECT_ATTEMPTS attempts", Toast.LENGTH_LONG).show()
            }
        }
    }

    private fun cancelReconnect() {
        reconnectJob?.cancel()
        reconnectJob = null
    }

    // ── Network quality polling ─────────────────────────────────────────

    private fun startQualityPolling() {
        qualityPollJob?.cancel()
        qualityPollJob = lifecycleScope.launch {
            var prevDropped = 0L
            var prevSent = 0L
            while (isActive) {
                delay(4000)
                if (streamState.value !is StreamState.Streaming) break
                val client = srtCamera2?.getStreamClient() ?: break
                val congestion = client.hasCongestion(0.5f)
                val cacheSize = client.getCacheSize()
                val cachePct = if (cacheSize > 0) client.getItemsInCache().toFloat() / cacheSize else 0f
                val dropped = client.getDroppedVideoFrames() + client.getDroppedAudioFrames()
                val sent = client.getSentVideoFrames() + client.getSentAudioFrames()
                val dropDelta = dropped - prevDropped
                val sentDelta = sent - prevSent
                prevDropped = dropped
                prevSent = sent
                val dropRate = if (sentDelta > 0) dropDelta.toFloat() / sentDelta else 0f
                val bitrateRatio = if (targetBitrate() > 0) lastActualBitrate.toFloat() / targetBitrate() else 1f
                connectionQuality.value = when {
                    congestion || dropRate > 0.3f || cachePct > 0.8f || bitrateRatio < 0.3f -> ConnectionQuality.Poor
                    dropRate > 0.05f || cachePct > 0.4f || bitrateRatio < 0.7f -> ConnectionQuality.Fair
                    else -> ConnectionQuality.Good
                }
            }
        }
    }

    private fun stopQualityPolling() {
        qualityPollJob?.cancel()
        qualityPollJob = null
        connectionQuality.value = ConnectionQuality.Good
    }

    // ── Stream control ─────────────────────────────────────────────────

    private fun toggleStream(ip: String, portString: String, name: String) {
        if (srtCamera2 == null) return
        val currentState = streamState.value
        if (currentState is StreamState.Connecting || currentState is StreamState.Stopping) {
            logMessage("Ignoring tap: transition in progress")
            return
        }
        if (currentState is StreamState.Streaming || currentState is StreamState.Reconnecting) {
            logMessage("Stopping stream...")
            stopQualityPolling()
            cancelReconnect()
            streamState.value = StreamState.Stopping
            lifecycleScope.launch(Dispatchers.IO) {
                srtCamera2?.stopStream()
                withContext(Dispatchers.Main) {
                    streamState.value = StreamState.Idle
                    encodersReady = false
                }
            }
            return
        }
        if (!encodersReady) {
            logMessage("Encoders not ready, retrying...")
            encodersReady = prepareEncoders()
            if (!encodersReady) {
                Toast.makeText(this, "Failed to prepare encoders", Toast.LENGTH_SHORT).show()
                return
            }
        }
        val portInt = portString.toIntOrNull() ?: 9000
        val sanitizedName = name.trim().replace(" ", "_").filter { it.isLetterOrDigit() || it == '_' }
        if (ip.isBlank() || sanitizedName.isBlank()) {
            logMessage("Error: Missing IP or Display Name"); return
        }

        val latencyParam = if (latencyAuto) "auto" else "${latencyMs * 1000}"
        val srtUrl = "srt://${ip}:${portInt}/publish:${sanitizedName}?latency=${latencyParam}"
        lastSrtUrl = srtUrl
        logMessage("Connecting to $srtUrl")
        streamState.value = StreamState.Connecting
        lifecycleScope.launch(Dispatchers.IO) {
            try {
                srtCamera2?.startStream(srtUrl)
            } catch (e: Exception) {
                logMessage("startStream failed: ${e.message}")
                withContext(Dispatchers.Main) {
                    Toast.makeText(this@MainActivity, "Stream start failed: ${e.message}", Toast.LENGTH_LONG).show()
                    streamState.value = StreamState.Idle
                    encodersReady = false
                    startCameraPreview()
                }
            }
        }
    }

    private fun switchCamera() {
        try {
            srtCamera2?.switchCamera()
            logMessage("Switched camera")
        } catch (e: Exception) {
            logMessage("Error switching camera: ${e.message}")
        }
    }

    private fun toggleAudio() {
        if (srtCamera2 == null) return
        if (isAudioEnabledState.value) {
            srtCamera2?.disableAudio()
            isAudioEnabledState.value = false
            logMessage("Audio input muted")
        } else {
            srtCamera2?.enableAudio()
            isAudioEnabledState.value = true
            logMessage("Audio input unmuted")
        }
    }

    private fun toggleOrientation() {
        if (isPortraitState.value) {
            isPortraitState.value = false
            requestedOrientation = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
            logMessage("Orientation: landscape")
        } else {
            isPortraitState.value = true
            requestedOrientation = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
            logMessage("Orientation: portrait")
        }
    }
}
