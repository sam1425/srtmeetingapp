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
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.List

class MainActivity : ComponentActivity(), ConnectChecker {

    private var srtCamera2: SrtCamera2? = null
    private var openGlView: OpenGlView? = null
    private var isSurfaceReady = false
    private var encodersReady = false

    private val isStreamingState = mutableStateOf(false)
    private val isAudioEnabledState = mutableStateOf(true)
    private val logs = mutableStateListOf<String>()

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
        if (srtCamera2?.prepareVideo(1280, 720, 30, 2000 * 1024, 0) == true &&
            srtCamera2?.prepareAudio(128 * 1024, 44100, true, true, true) == true) {
                logMessage("Encoders ready.")
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
        runOnUiThread { isStreamingState.value = true }
    }

    override fun onConnectionFailed(reason: String) {
        logMessage("Connection failed: $reason")
        runOnUiThread {
            isStreamingState.value = false
            encodersReady = false
            Toast.makeText(this@MainActivity, "Connection Failed: $reason", Toast.LENGTH_LONG).show()
        }
    }

    override fun onNewBitrate(bitrate: Long) {}

    override fun onDisconnect() {
        logMessage("Disconnected from server.")
        runOnUiThread {
            isStreamingState.value = false
            encodersReady = false
        }
    }

    override fun onAuthError() { logMessage("Authentication error.") }
    override fun onAuthSuccess() { logMessage("Authentication successful.") }

    private fun toggleStream(ip: String, portString: String, name: String) {
        if (srtCamera2 == null) return

        if (isStreamingState.value) {
            logMessage("Stopping stream...")
            isStreamingState.value = false
            encodersReady = false
            Thread { srtCamera2?.stopStream() }.start()
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

        val srtUrl = "srt://$ip:$port/publish:$sanitizedName?latency=120000"
        logMessage("Connecting to $srtUrl")
        isStreamingState.value = true
        srtCamera2?.startStream(srtUrl)
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
    fun MainScreen() {
        var ip by remember { mutableStateOf("192.168.1.100") }
        var port by remember { mutableStateOf("9000") }
        var name by remember { mutableStateOf("Default") }
        // Read directly from the stable MutableState objects — no extra remember wrapper needed
        val isStreaming by isStreamingState
        val isAudioEnabled by isAudioEnabledState

        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(16.dp)
                .verticalScroll(rememberScrollState()),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            Text(
                text = "OBS SRT Meeting Client",
                color = MaterialTheme.colorScheme.primary,
                fontSize = 22.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(vertical = 8.dp)
            )

            // Camera preview (16:9)
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .aspectRatio(16f / 9f)
                    .clip(RoundedCornerShape(12.dp))
                    .background(Color.Black)
                    .border(2.dp, MaterialTheme.colorScheme.primary, RoundedCornerShape(12.dp))
            ) {
                AndroidView(
                    factory = { openGlView!! },
                    modifier = Modifier.fillMaxSize()
                )
            }

            // Input fields
            Card(
                modifier = Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                shape = RoundedCornerShape(12.dp)
            ) {
                Column(
                    modifier = Modifier.padding(16.dp),
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
                        modifier = Modifier.fillMaxWidth(),
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
                }
            }
            RecordButton(
                isStreaming = isStreaming,
                onClick = { toggleStream(ip, port, name) },
                modifier = Modifier.align(Alignment.CenterHorizontally)
            )
            // Action buttons — single row, single connect button
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Button(
                    onClick = { toggleStream(ip, port, name) },
                    modifier = Modifier.weight(1.5f),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (isStreaming) Color(0xFFF38BA8) else Color(0xFFA6E3A1),
                        contentColor = Color(0xFF11111B)
                    ),
                    shape = RoundedCornerShape(8.dp)
                ) {
                    Text(
                        text = if (isStreaming) "Disconnect" else "Connect",
                        fontWeight = FontWeight.Bold,
                        fontSize = 15.sp
                    )
                }

                Button(
                    onClick = { switchCamera() },
                    modifier = Modifier.weight(1f),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = Color(0xFF45475A),
                        contentColor = MaterialTheme.colorScheme.onBackground
                    ),
                    shape = RoundedCornerShape(8.dp)
                ) {
                    Text("Switch Cam", fontSize = 13.sp)
                }

                Button(
                    onClick = { toggleAudio() },
                    modifier = Modifier.weight(1f),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (isAudioEnabled) Color(0xFF89B4FA) else Color(0xFFF38BA8),
                        contentColor = Color(0xFF11111B)
                    ),
                    shape = RoundedCornerShape(8.dp)
                ) {
                    Text(if (isAudioEnabled) "Mute Mic" else "Unmute Mic", fontSize = 13.sp)
                }
            }

            // Logs
            Text(
                text = "Connection Logs & Diagnostics",
                color = MaterialTheme.colorScheme.secondary,
                fontSize = 14.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.align(Alignment.Start)
            )

            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(180.dp)
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
                            text = "Ready. Configure broadcaster connection and tap Connect.",
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
    }
}
