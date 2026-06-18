package com.meeting.srt.ui

import android.content.res.Configuration
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.meeting.srt.ConnectionQuality
import com.meeting.srt.StreamState
import com.pedro.library.view.OpenGlView

@Composable
fun MainScreen(
    ip: String,
    port: String,
    name: String,
    selectedProfile: String,
    latencyMs: Int,
    latencyAuto: Boolean,
    streamState: StreamState,
    connectionQuality: ConnectionQuality,
    isAudioEnabled: Boolean,
    allowVertical: Boolean,
    logs: List<String>,
    onIpChange: (String) -> Unit,
    onPortChange: (String) -> Unit,
    onNameChange: (String) -> Unit,
    onProfileChange: (String) -> Unit,
    onLatencyMsChange: (Int) -> Unit,
    onLatencyAutoChange: (Boolean) -> Unit,
    onAllowVerticalChange: (Boolean) -> Unit,
    onToggleStream: () -> Unit,
    onToggleAudio: () -> Unit,
    onSwitchCamera: () -> Unit,
    onToggleOrientation: () -> Unit,
    onLogsClear: () -> Unit,
    openGlView: OpenGlView? = null,
) {
    val isStreaming = streamState is StreamState.Streaming
    val isActive = streamState !is StreamState.Idle
    var showConfigDialog by remember { mutableStateOf(false) }
    var showLogsDialog by remember { mutableStateOf(false) }

    val configuration = LocalConfiguration.current
    val isLandscape = configuration.orientation == Configuration.ORIENTATION_LANDSCAPE

    Box(
        modifier = Modifier
            .fillMaxSize()
            .pointerInput(allowVertical) {
                if (allowVertical) {
                    detectTapGestures(onDoubleTap = { onToggleOrientation() })
                }
            },
        contentAlignment = Alignment.Center
    ) {
        if (openGlView != null) {
            AndroidView(
                factory = { openGlView },
                modifier = Modifier.fillMaxSize()
            )
        }

        // Live Indicator (Top Start)
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
            val dotColor = when (streamState) {
                is StreamState.Streaming    -> Color(0xFFE24B4A)
                is StreamState.Reconnecting -> Color(0xFFE5C890)
                is StreamState.Connecting   -> Color(0xFFCBA6F7)
                else                        -> Color(0xFFBAC2DE)
            }
            val statusText = when (val s = streamState) {
                is StreamState.Streaming    -> "LIVE"
                is StreamState.Reconnecting -> "RETRY ${s.attempt}/${s.maxAttempts}"
                is StreamState.Connecting   -> "CONNECTING"
                is StreamState.Stopping     -> "STOPPING"
                else                        -> "STANDBY"
            }
            Box(
                modifier = Modifier.size(8.dp).clip(CircleShape).background(dotColor)
            )
            Text(
                text = statusText,
                color = Color.White,
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace
            )
            if (isStreaming) {
                val qualityColor = when (connectionQuality) {
                    ConnectionQuality.Good -> Color(0xFFA6E3A1)
                    ConnectionQuality.Fair -> Color(0xFFE5C890)
                    ConnectionQuality.Poor -> Color(0xFFE24B4A)
                }
                Box(
                    modifier = Modifier.size(6.dp).clip(CircleShape).background(qualityColor)
                )
            }
        }

        // Stream URL info (Top End)
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

        // Control Panel
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
                IconButton(onClick = { showConfigDialog = true }, modifier = Modifier.size(48.dp)) {
                    Icon(imageVector = Icons.Default.Settings, contentDescription = "Settings", tint = MaterialTheme.colorScheme.primary)
                }
                IconButton(onClick = onToggleAudio, modifier = Modifier.size(48.dp)) {
                    MicIcon(isAudioEnabled = isAudioEnabled)
                }
                RecordButton(isStreaming = isStreaming, onClick = onToggleStream)
                IconButton(onClick = onSwitchCamera, modifier = Modifier.size(48.dp)) {
                    Icon(imageVector = Icons.Default.Refresh, contentDescription = "Switch Camera", tint = Color.White)
                }
                IconButton(onClick = { showLogsDialog = true }, modifier = Modifier.size(48.dp)) {
                    Icon(imageVector = Icons.AutoMirrored.Filled.List, contentDescription = "Connection Logs", tint = MaterialTheme.colorScheme.secondary)
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
                IconButton(onClick = { showConfigDialog = true }, modifier = Modifier.size(48.dp)) {
                    Icon(imageVector = Icons.Default.Settings, contentDescription = "Settings", tint = MaterialTheme.colorScheme.primary)
                }
                IconButton(onClick = onToggleAudio, modifier = Modifier.size(48.dp)) {
                    MicIcon(isAudioEnabled = isAudioEnabled)
                }
                RecordButton(isStreaming = isStreaming, onClick = onToggleStream)
                IconButton(onClick = onSwitchCamera, modifier = Modifier.size(48.dp)) {
                    Icon(imageVector = Icons.Default.Refresh, contentDescription = "Switch Camera", tint = Color.White)
                }
                IconButton(onClick = { showLogsDialog = true }, modifier = Modifier.size(48.dp)) {
                    Icon(imageVector = Icons.AutoMirrored.Filled.List, contentDescription = "Connection Logs", tint = MaterialTheme.colorScheme.secondary)
                }
            }
        }
    }

    if (showConfigDialog) {
        SettingsDialog(
            ip = ip,
            onIpChange = onIpChange,
            port = port,
            onPortChange = onPortChange,
            name = name,
            onNameChange = onNameChange,
            selectedProfile = selectedProfile,
            onProfileChange = onProfileChange,
            latencyMs = latencyMs,
            latencyAuto = latencyAuto,
            onLatencyMsChange = onLatencyMsChange,
            onLatencyAutoChange = onLatencyAutoChange,
            allowVertical = allowVertical,
            onAllowVerticalChange = onAllowVerticalChange,
            isActive = isActive,
            onDismiss = { showConfigDialog = false }
        )
    }

    if (showLogsDialog) {
        LogsDialog(
            logs = logs,
            onClear = onLogsClear,
            onDismiss = { showLogsDialog = false }
        )
    }
}
