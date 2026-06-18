package com.meeting.srt

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.SurfaceHolder
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import com.pedro.common.ConnectChecker
import com.pedro.library.srt.SrtCamera2
import com.pedro.library.view.OpenGlView
import java.text.SimpleDateFormat
import java.util.*
import androidx.compose.animation.animateColor
import androidx.compose.animation.core.*
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.scale
import androidx.compose.foundation.shape.CircleShape
import android.content.res.Configuration
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.foundation.Canvas
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.StrokeCap
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import android.content.pm.ActivityInfo
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.ui.input.pointer.pointerInput

class MainActivity : ComponentActivity(), ConnectChecker {

    private var srtCamera2: SrtCamera2? = null
    private var openGlView: OpenGlView? = null
    private var isSurfaceReady = false
    @Volatile private var encodersReady = false

    // Single source of truth for video quality profile — kept in sync with
    // the Composable's rememberSaveable state via LaunchedEffect, so it
    // survives both runtime changes and Activity recreation correctly.
    private val activeProfile = mutableStateOf("High")

    private fun dimensionsForProfile(profile: String): Triple<Int, Int, Int> = when (profile) {
        "Medium" -> Triple(854, 480, 1000 * 1024)
        "Low"    -> Triple(640, 360, 500 * 1024)
        else     -> Triple(1280, 720, 2000 * 1024) // "High" default
    }

    private val isStreamingState = mutableStateOf(false)
    private val isProcessingState = mutableStateOf(false)
    private val isAudioEnabledState = mutableStateOf(true)
    private val logs = mutableStateListOf<String>()
    // Orientation control — landscape is the default; portrait is opt-in per session
    private val allowVerticalState = mutableStateOf(false)
    private val isPortraitState = mutableStateOf(false)

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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Default to landscape; portrait must be explicitly unlocked in Settings
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE

        val view = OpenGlView(this)
        openGlView = view
        srtCamera2 = SrtCamera2(view, this)

        view.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                logMessage("Surface created.")
                isSurfaceReady = true
                // Only start preview here if permissions are already granted;
                // otherwise the permission callback will trigger it.
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
                colorScheme = darkColorScheme(
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
                    MainScreen()
                }
            }
        }

        checkAndRequestPermissions()
    }

    override fun onResume() {
        super.onResume()
        if (hasPermissions() && srtCamera2?.isStreaming == false && srtCamera2?.isOnPreview == false) {
            startCameraPreview()
        }
    }

    override fun onPause() {
        super.onPause()
        if (srtCamera2?.isStreaming == true) {
            srtCamera2?.stopStream()
            isStreamingState.value = false
        }
        if (srtCamera2?.isOnPreview == true) {
            srtCamera2?.stopPreview()
        }
        encodersReady = false
    }

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
            // Surface may not be ready yet; surfaceCreated callback will call startCameraPreview()
            // once both conditions are met. Calling here is safe — the guard inside handles it.
            startCameraPreview()
        }
    }

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
            srtCamera2?.startPreview()
            encodersReady = prepareEncoders()
            logMessage("Camera preview started.")
        } catch (e: Exception) {
            logMessage("Failed to start preview: ${e.message}")
        }
    }

    private fun logMessage(message: String) {
        android.util.Log.d("MainActivity", message)
        runOnUiThread {
            val timestamp = SimpleDateFormat("HH:mm:ss.SSS", Locale.getDefault()).format(Date())
            logs.add("[$timestamp] $message")
        }
    }

    // ConnectChecker

    override fun onConnectionStarted(url: String) {
        logMessage("Connecting: $url")
    }

    override fun onConnectionSuccess() {
        logMessage("Connection successful!")
        runOnUiThread {
            isStreamingState.value = true
            isProcessingState.value = false
        }
    }

    override fun onConnectionFailed(reason: String) {
        logMessage("Connection failed: $reason")
        runOnUiThread { isProcessingState.value = true }
        lifecycleScope.launch(Dispatchers.IO) {
            srtCamera2?.stopStream()
            srtCamera2?.stopPreview()
            withContext(Dispatchers.Main) {
                isStreamingState.value = false
                encodersReady = false
                startCameraPreview()
                isProcessingState.value = false
                Toast.makeText(this@MainActivity, "Connection Failed: $reason", Toast.LENGTH_LONG).show()
            }
        }
    }

    override fun onNewBitrate(bitrate: Long) {}

    override fun onDisconnect() {
        logMessage("Disconnected from server.")
        runOnUiThread { isProcessingState.value = true }
        lifecycleScope.launch(Dispatchers.IO) {
            srtCamera2?.stopStream()
            srtCamera2?.stopPreview()
            withContext(Dispatchers.Main) {
                isStreamingState.value = false
                encodersReady = false
                startCameraPreview()
                isProcessingState.value = false
            }
        }
    }

    override fun onAuthError() { logMessage("Authentication error.") }
    override fun onAuthSuccess() { logMessage("Authentication successful.") }

    private fun toggleStream(ip: String, portString: String, name: String) {
        if (srtCamera2 == null) return
        if (isProcessingState.value) {
            logMessage("Ignoring tap: processing previous action")
            return
        }

        if (isStreamingState.value) {
            logMessage("Stopping stream...")
            isProcessingState.value = true
            isStreamingState.value = false
            encodersReady = false
            // Use lifecycleScope so stopStream() runs on IO without racing lifecycle callbacks
            lifecycleScope.launch(Dispatchers.IO) {
                srtCamera2?.stopStream()
                srtCamera2?.stopPreview()
                withContext(Dispatchers.Main) {
                    startCameraPreview()
                    isProcessingState.value = false
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

        val port = portString.toIntOrNull() ?: 9000
        val sanitizedName = name.trim().replace(" ", "_").filter { it.isLetterOrDigit() || it == '_' }
        if (ip.isBlank() || sanitizedName.isBlank()) {
            logMessage("Error: Missing IP or Display Name"); return
        }

        // RootEncoder (pedroSG94) expects the SRT stream ID as a URL path segment,
        // NOT as a `streamid=` query parameter. The correct format is:
        //   srt://ip:port/publish:name?latency=...
        // Do NOT change this to query-parameter style — it will produce
        // "endpoint malformed" errors at connection time.
        val srtUrl = "srt://${ip}:${port}/publish:${sanitizedName}?latency=120000"
        logMessage("Connecting to $srtUrl")
        // Do NOT set isStreamingState=true here — wait for onConnectionSuccess() so
        // the UI never shows LIVE while still connecting or after a failed attempt.
        isProcessingState.value = true
        lifecycleScope.launch(Dispatchers.IO) {
            try {
                srtCamera2?.startStream(srtUrl)
            } catch (e: Exception) {
                logMessage("startStream failed: ${e.message}")
                withContext(Dispatchers.Main) {
                    Toast.makeText(this@MainActivity, "Stream start failed: ${e.message}", Toast.LENGTH_LONG).show()
                    isStreamingState.value = false
                    encodersReady = false
                    isProcessingState.value = false
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
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
            logMessage("Orientation: landscape")
        } else {
            isPortraitState.value = true
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
            logMessage("Orientation: portrait")
        }
    }

    @Composable
    fun RecordButton(
        isStreaming: Boolean,
        onClick: () -> Unit,
        modifier: Modifier = Modifier
    ) {
        val transition = updateTransition(targetState = isStreaming, label = "recordButton")

        // Animate outer ring color (White to Red)
        val outlineColor by transition.animateColor(
            transitionSpec = {
                spring(dampingRatio = Spring.DampingRatioNoBouncy, stiffness = Spring.StiffnessMedium)
            },
            label = "outlineColor"
        ) { streaming ->
            if (streaming) Color(0xFFE24B4A) else Color.White
        }

        // Animate center button size (54.dp to 32.dp)
        val centerSize by transition.animateDp(
            transitionSpec = {
                spring(dampingRatio = Spring.DampingRatioMediumBouncy, stiffness = Spring.StiffnessLow)
            },
            label = "centerSize"
        ) { streaming ->
            if (streaming) 32.dp else 54.dp
        }

        // Animate center button corner radius (27.dp to 8.dp)
        val centerCornerRadius by transition.animateDp(
            transitionSpec = {
                spring(dampingRatio = Spring.DampingRatioMediumBouncy, stiffness = Spring.StiffnessLow)
            },
            label = "centerCornerRadius"
        ) { streaming ->
            if (streaming) 8.dp else 27.dp
        }

        Box(
            modifier = modifier
                .size(80.dp)
                .clickable(
                    interactionSource = remember { MutableInteractionSource() },
                    indication = null,
                    onClick = onClick
                ),
            contentAlignment = Alignment.Center
        ) {
            // Outer ring border
            Box(
                modifier = Modifier
                    .size(76.dp)
                    .border(4.dp, outlineColor, CircleShape)
            )

            // Inner center button
            Box(
                modifier = Modifier
                    .size(centerSize)
                    .background(
                        color = Color(0xFFE24B4A),
                        shape = RoundedCornerShape(centerCornerRadius)
                    )
            )
        }
    }

    @Composable
    fun MicIcon(isAudioEnabled: Boolean, modifier: Modifier = Modifier) {
        Canvas(modifier = modifier.size(24.dp)) {
            val w = size.width
            val h = size.height
            val activeColor = Color.White
            val inactiveColor = Color(0xFFF38BA8)
            val color = if (isAudioEnabled) activeColor else inactiveColor

            // Draw microphone body (rounded rect in the middle)
            val bodyWidth = w * 0.35f
            val bodyHeight = h * 0.55f
            val bodyLeft = (w - bodyWidth) / 2
            val bodyTop = h * 0.12f
            drawRoundRect(
                color = color,
                topLeft = Offset(bodyLeft, bodyTop),
                size = Size(bodyWidth, bodyHeight),
                cornerRadius = CornerRadius(bodyWidth / 2, bodyWidth / 2),
                style = Stroke(width = 2.dp.toPx())
            )
            // Draw solid fill or inner rect for the body
            if (isAudioEnabled) {
                drawRoundRect(
                    color = color.copy(alpha = 0.4f),
                    topLeft = Offset(bodyLeft + 3.dp.toPx(), bodyTop + 3.dp.toPx()),
                    size = Size(bodyWidth - 6.dp.toPx(), bodyHeight - 6.dp.toPx()),
                    cornerRadius = CornerRadius((bodyWidth - 6.dp.toPx()) / 2, (bodyWidth - 6.dp.toPx()) / 2)
                )
            }

            // Draw cradle (semi-circle around bottom)
            val cradleRadius = w * 0.28f
            drawArc(
                color = color,
                startAngle = 0f,
                sweepAngle = 180f,
                useCenter = false,
                topLeft = Offset(w / 2 - cradleRadius, h * 0.35f),
                size = Size(cradleRadius * 2, cradleRadius * 2),
                style = Stroke(width = 2.dp.toPx(), cap = StrokeCap.Round)
            )

            // Draw stand (vertical line down from cradle)
            drawLine(
                color = color,
                start = Offset(w / 2, h * 0.35f + cradleRadius * 2),
                end = Offset(w / 2, h * 0.88f),
                strokeWidth = 2.dp.toPx(),
                cap = StrokeCap.Round
            )

            // Draw base (horizontal line at the bottom)
            val baseWidth = w * 0.3f
            drawLine(
                color = color,
                start = Offset(w / 2 - baseWidth / 2, h * 0.88f),
                end = Offset(w / 2 + baseWidth / 2, h * 0.88f),
                strokeWidth = 2.dp.toPx(),
                cap = StrokeCap.Round
            )

            // Draw slash if muted
            if (!isAudioEnabled) {
                drawLine(
                    color = inactiveColor,
                    start = Offset(w * 0.2f, h * 0.2f),
                    end = Offset(w * 0.8f, h * 0.8f),
                    strokeWidth = 2.dp.toPx(),
                    cap = StrokeCap.Round
                )
            }
        }
    }

    @Composable
    fun MainScreen() {
        var ip by rememberSaveable { mutableStateOf("192.168.1.100") }
        var port by rememberSaveable { mutableStateOf("9000") }
        var name by rememberSaveable { mutableStateOf("Default") }
        var selectedProfile by rememberSaveable { mutableStateOf(activeProfile.value) }

        // Keep the Activity-level activeProfile (read by prepareEncoders) in sync with
        // the Compose state. LaunchedEffect re-runs on every selectedProfile change,
        // including after Activity recreation when rememberSaveable restores the value.
        LaunchedEffect(selectedProfile) {
            activeProfile.value = selectedProfile
        }

        var showConfigDialog by remember { mutableStateOf(false) }
        var showLogsDialog by remember { mutableStateOf(false) }

        val isStreaming by isStreamingState
        val isAudioEnabled by isAudioEnabledState
        val allowVertical by allowVerticalState

        val configuration = LocalConfiguration.current
        val isLandscape = configuration.orientation == Configuration.ORIENTATION_LANDSCAPE

        Box(
            modifier = Modifier
                .fillMaxSize()
                .pointerInput(allowVertical) {
                    // Only capture double-taps when the user has enabled vertical mode.
                    // The key `allowVertical` restarts this block when the toggle changes.
                    if (allowVertical) {
                        detectTapGestures(onDoubleTap = { toggleOrientation() })
                    }
                },
            contentAlignment = Alignment.Center
        ) {
            // Fullscreen OpenGL preview
            AndroidView(
                factory = { openGlView ?: error("OpenGlView not initialized") },
                modifier = Modifier.fillMaxSize()
            )

            // HUD Overlays
            // 1. Live Indicator (Top Start / Left)
            Row(
                modifier = Modifier
                    .align(Alignment.TopStart)
                    .padding(16.dp)
                    .clip(RoundedCornerShape(8.dp))
                    .background(Color.Black.copy(alpha = 0.6f))
                    .padding(horizontal = 12.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Box(
                    modifier = Modifier
                        .size(8.dp)
                        .clip(CircleShape)
                        .background(if (isStreaming) Color(0xFFE24B4A) else Color(0xFFBAC2DE))
                )
                Text(
                    text = if (isStreaming) "LIVE" else "STANDBY",
                    color = Color.White,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace
                )
            }

            // 2. Stream URL info (Top End / Right)
            if (isStreaming) {
                Text(
                    text = "$ip:$port/$name",
                    color = Color.White.copy(alpha = 0.8f),
                    fontSize = 12.sp,
                    fontFamily = FontFamily.Monospace,
                    modifier = Modifier
                        .align(Alignment.TopEnd)
                        .padding(16.dp)
                        .clip(RoundedCornerShape(8.dp))
                        .background(Color.Black.copy(alpha = 0.6f))
                        .padding(horizontal = 12.dp, vertical = 6.dp)
                )
            }

            // 3. Control Panel (Bottom Row for Portrait, Right Column for Landscape)
            if (isLandscape) {
                Column(
                    modifier = Modifier
                        .align(Alignment.CenterEnd)
                        .fillMaxHeight()
                        .width(88.dp)
                        .background(Color.Black.copy(alpha = 0.5f))
                        .padding(vertical = 24.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.SpaceEvenly
                ) {
                    IconButton(
                        onClick = { showConfigDialog = true },
                        modifier = Modifier.size(48.dp)
                    ) {
                        Icon(
                            imageVector = Icons.Default.Settings,
                            contentDescription = "Settings",
                            tint = MaterialTheme.colorScheme.primary
                        )
                    }

                    IconButton(
                        onClick = { toggleAudio() },
                        modifier = Modifier.size(48.dp)
                    ) {
                        MicIcon(isAudioEnabled = isAudioEnabled)
                    }

                    RecordButton(
                        isStreaming = isStreaming,
                        onClick = { toggleStream(ip, port, name) }
                    )

                    IconButton(
                        onClick = { switchCamera() },
                        modifier = Modifier.size(48.dp)
                    ) {
                        Icon(
                            imageVector = Icons.Default.Refresh,
                            contentDescription = "Switch Camera",
                            tint = Color.White
                        )
                    }

                    IconButton(
                        onClick = { showLogsDialog = true },
                        modifier = Modifier.size(48.dp)
                    ) {
                        Icon(
                            imageVector = Icons.AutoMirrored.Filled.List,
                            contentDescription = "Connection Logs",
                            tint = MaterialTheme.colorScheme.secondary
                        )
                    }
                }
            } else {
                Row(
                    modifier = Modifier
                        .align(Alignment.BottomCenter)
                        .fillMaxWidth()
                        .height(100.dp)
                        .background(Color.Black.copy(alpha = 0.5f))
                        .padding(horizontal = 16.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceEvenly
                ) {
                    IconButton(
                        onClick = { showConfigDialog = true },
                        modifier = Modifier.size(48.dp)
                    ) {
                        Icon(
                            imageVector = Icons.Default.Settings,
                            contentDescription = "Settings",
                            tint = MaterialTheme.colorScheme.primary
                        )
                    }

                    IconButton(
                        onClick = { toggleAudio() },
                        modifier = Modifier.size(48.dp)
                    ) {
                        MicIcon(isAudioEnabled = isAudioEnabled)
                    }

                    RecordButton(
                        isStreaming = isStreaming,
                        onClick = { toggleStream(ip, port, name) }
                    )

                    IconButton(
                        onClick = { switchCamera() },
                        modifier = Modifier.size(48.dp)
                    ) {
                        Icon(
                            imageVector = Icons.Default.Refresh,
                            contentDescription = "Switch Camera",
                            tint = Color.White
                        )
                    }

                    IconButton(
                        onClick = { showLogsDialog = true },
                        modifier = Modifier.size(48.dp)
                    ) {
                        Icon(
                            imageVector = Icons.AutoMirrored.Filled.List,
                            contentDescription = "Connection Logs",
                            tint = MaterialTheme.colorScheme.secondary
                        )
                    }
                }
            }
        }

        // Configuration Dialog Popup
        if (showConfigDialog) {
            AlertDialog(
                onDismissRequest = { showConfigDialog = false },
                properties = androidx.compose.ui.window.DialogProperties(usePlatformDefaultWidth = false),
                modifier = Modifier.fillMaxWidth(0.9f),
                title = {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text("Connection Settings", color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.Bold)
                        Button(
                            onClick = { showConfigDialog = false },
                            colors = ButtonDefaults.buttonColors(
                                containerColor = MaterialTheme.colorScheme.primary,
                                contentColor = Color(0xFF11111B)
                            ),
                            contentPadding = PaddingValues(horizontal = 16.dp, vertical = 0.dp),
                            modifier = Modifier.height(32.dp)
                        ) {
                            Text("Done", fontWeight = FontWeight.Bold, fontSize = 14.sp)
                        }
                    }
                },
                text = {
                    Column(
                        modifier = Modifier.verticalScroll(rememberScrollState()),
                        verticalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        OutlinedTextField(
                            value = ip,
                            onValueChange = { if (!isStreaming) ip = it },
                            label = { Text("Broadcaster IP") },
                            enabled = !isStreaming,
                            modifier = Modifier.fillMaxWidth(),
                            colors = OutlinedTextFieldDefaults.colors(
                                focusedBorderColor = MaterialTheme.colorScheme.primary,
                                unfocusedBorderColor = Color(0xFF45475A)
                            )
                        )
                        Row(
                            horizontalArrangement = Arrangement.spacedBy(12.dp)
                        ) {
                            OutlinedTextField(
                                value = port,
                                onValueChange = { if (!isStreaming) port = it },
                                label = { Text("SRT Port") },
                                enabled = !isStreaming,
                                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                                modifier = Modifier.weight(1f),
                                colors = OutlinedTextFieldDefaults.colors(
                                    focusedBorderColor = MaterialTheme.colorScheme.primary,
                                    unfocusedBorderColor = Color(0xFF45475A)
                                )
                            )
                            OutlinedTextField(
                                value = name,
                                onValueChange = { if (!isStreaming) name = it },
                                label = { Text("Display Name") },
                                enabled = !isStreaming,
                                modifier = Modifier.weight(2f),
                                colors = OutlinedTextFieldDefaults.colors(
                                    focusedBorderColor = MaterialTheme.colorScheme.primary,
                                    unfocusedBorderColor = Color(0xFF45475A)
                                )
                            )
                        }
                        Spacer(modifier = Modifier.height(4.dp))
                        Text("Video Quality Profile (Unstable Networks)", color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.Bold, fontSize = 14.sp)
                        Row(
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            val profiles = listOf("High", "Medium", "Low")
                            profiles.forEach { profile ->
                                val isSelected = selectedProfile == profile
                                val containerColor = if (isSelected) MaterialTheme.colorScheme.primary else Color(0xFF313244)
                                val contentColor = if (isSelected) Color(0xFF11111B) else Color(0xFFCDD6F4)
                                Button(
                                    onClick = {
                                        if (!isStreaming) {
                                            selectedProfile = profile
                                            // activeProfile is synced via LaunchedEffect(selectedProfile)
                                            encodersReady = false
                                            val (w, h, br) = dimensionsForProfile(profile)
                                            logMessage("Selected profile: $profile (${w}x${h}, ${br / 1024} Kbps)")
                                        }
                                    },
                                    colors = ButtonDefaults.buttonColors(
                                        containerColor = containerColor,
                                        contentColor = contentColor
                                    ),
                                    modifier = Modifier.weight(1f),
                                    contentPadding = PaddingValues(vertical = 4.dp),
                                    shape = RoundedCornerShape(8.dp)
                                ) {
                                    Text(profile, fontSize = 11.sp, fontWeight = FontWeight.Bold)
                                }
                            }
                        }

                        Spacer(modifier = Modifier.height(4.dp))
                        HorizontalDivider(color = Color(0xFF45475A), thickness = 1.dp)
                        Spacer(modifier = Modifier.height(4.dp))
                        Text(
                            "Display Orientation",
                            color = MaterialTheme.colorScheme.primary,
                            fontWeight = FontWeight.Bold,
                            fontSize = 14.sp
                        )
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Column {
                                Text(
                                    "Allow Vertical Mode",
                                    color = MaterialTheme.colorScheme.onSurface,
                                    fontSize = 13.sp
                                )
                                Text(
                                    "Double-tap preview to rotate",
                                    color = Color(0xFF7F849C),
                                    fontSize = 11.sp
                                )
                            }
                            Switch(
                                checked = allowVertical,
                                onCheckedChange = { enabled ->
                                    allowVerticalState.value = enabled
                                    if (!enabled) {
                                        // Snap back to landscape when vertical is disabled
                                        isPortraitState.value = false
                                        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                                    }
                                },
                                colors = SwitchDefaults.colors(
                                    checkedThumbColor = MaterialTheme.colorScheme.primary,
                                    checkedTrackColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.5f)
                                )
                            )
                        }
                    }
                },
                confirmButton = {},
                containerColor = MaterialTheme.colorScheme.surface,
                shape = RoundedCornerShape(16.dp)
            )
        }

        // Diagnostics Log Dialog Popup
        if (showLogsDialog) {
            AlertDialog(
                onDismissRequest = { showLogsDialog = false },
                title = { Text("Diagnostics & Logs", color = MaterialTheme.colorScheme.secondary, fontWeight = FontWeight.Bold) },
                text = {
                    Column(
                        modifier = Modifier.fillMaxWidth(),
                        verticalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        Box(
                            modifier = Modifier
                                .fillMaxWidth()
                                .height(280.dp)
                                .background(Color(0xFF181825), RoundedCornerShape(8.dp))
                                .border(1.dp, Color(0xFF313244), RoundedCornerShape(8.dp))
                                .padding(8.dp)
                        ) {
                            val scrollState = rememberScrollState()
                            LaunchedEffect(logs.size) {
                                if (logs.isNotEmpty()) scrollState.scrollTo(scrollState.maxValue)
                            }
                            Column(
                                modifier = Modifier
                                    .fillMaxSize()
                                    .verticalScroll(scrollState),
                                verticalArrangement = Arrangement.spacedBy(4.dp)
                            ) {
                                if (logs.isEmpty()) {
                                    Text(
                                        text = "Ready. Configure connection parameters and start streaming.",
                                        color = Color(0xFF7F849C),
                                        fontFamily = FontFamily.Monospace,
                                        fontSize = 12.sp
                                    )
                                } else {
                                    logs.forEach { log ->
                                        Text(
                                            text = log,
                                            color = Color(0xFFA6E3A1),
                                            fontFamily = FontFamily.Monospace,
                                            fontSize = 12.sp
                                        )
                                    }
                                }
                            }
                        }
                    }
                },
                confirmButton = {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween
                    ) {
                        TextButton(
                            onClick = { logs.clear() },
                            colors = ButtonDefaults.textButtonColors(contentColor = Color(0xFFF38BA8))
                        ) {
                            Text("Clear Logs")
                        }
                        Button(
                            onClick = { showLogsDialog = false },
                            colors = ButtonDefaults.buttonColors(
                                containerColor = MaterialTheme.colorScheme.secondary,
                                contentColor = Color(0xFF11111B)
                            )
                        ) {
                            Text("Close", fontWeight = FontWeight.Bold)
                        }
                    }
                },
                containerColor = MaterialTheme.colorScheme.surface,
                shape = RoundedCornerShape(16.dp)
            )
        }
    }
}
