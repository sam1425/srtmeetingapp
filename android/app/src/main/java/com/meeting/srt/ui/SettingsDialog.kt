package com.meeting.srt.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@Composable
fun SettingsDialog(
    ip: String,
    onIpChange: (String) -> Unit,
    port: String,
    onPortChange: (String) -> Unit,
    name: String,
    onNameChange: (String) -> Unit,
    selectedProfile: String,
    onProfileChange: (String) -> Unit,
    latencyMs: Int,
    latencyAuto: Boolean,
    onLatencyMsChange: (Int) -> Unit,
    onLatencyAutoChange: (Boolean) -> Unit,
    allowVertical: Boolean,
    onAllowVerticalChange: (Boolean) -> Unit,
    isActive: Boolean,
    onDismiss: () -> Unit,
    onConfirm: () -> Unit = onDismiss
) {
    var localIp by remember(ip) { mutableStateOf(ip) }
    var localPort by remember(port) { mutableStateOf(port) }
    var localName by remember(name) { mutableStateOf(name) }
    var localProfile by remember(selectedProfile) { mutableStateOf(selectedProfile) }
    var localLatencyMs by remember(latencyMs) { mutableStateOf(latencyMs) }
    var localLatencyAuto by remember(latencyAuto) { mutableStateOf(latencyAuto) }
    var localAllowVertical by remember(allowVertical) { mutableStateOf(allowVertical) }

    fun commit() {
        onIpChange(localIp)
        onPortChange(localPort)
        onNameChange(localName)
        onProfileChange(localProfile)
        onLatencyMsChange(localLatencyMs)
        onLatencyAutoChange(localLatencyAuto)
        onAllowVerticalChange(localAllowVertical)
        onConfirm()
    }
    AlertDialog(
        onDismissRequest = onDismiss,
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
                    onClick = { commit() },
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
                    value = localIp,
                    onValueChange = { if (!isActive) localIp = it },
                    label = { Text("Broadcaster IP") },
                    enabled = !isActive,
                    modifier = Modifier.fillMaxWidth(),
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = MaterialTheme.colorScheme.primary,
                        unfocusedBorderColor = Color(0xFF45475A)
                    )
                )
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    OutlinedTextField(
                        value = localPort,
                        onValueChange = { if (!isActive) localPort = it },
                        label = { Text("SRT Port") },
                        enabled = !isActive,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        modifier = Modifier.weight(1f),
                        colors = OutlinedTextFieldDefaults.colors(
                            focusedBorderColor = MaterialTheme.colorScheme.primary,
                            unfocusedBorderColor = Color(0xFF45475A)
                        )
                    )
                    OutlinedTextField(
                        value = localName,
                        onValueChange = { if (!isActive) localName = it },
                        label = { Text("Display Name") },
                        enabled = !isActive,
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
                        val isSelected = localProfile == profile
                        val containerColor = if (isSelected) MaterialTheme.colorScheme.primary else Color(0xFF313244)
                        val contentColor = if (isSelected) Color(0xFF11111B) else Color(0xFFCDD6F4)
                        Button(
                            onClick = {
                                if (!isActive) {
                                    localProfile = profile
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
                Text("Display Orientation", color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.Bold, fontSize = 14.sp)
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column {
                        Text("Allow Vertical Mode", color = MaterialTheme.colorScheme.onSurface, fontSize = 13.sp)
                        Text("Double-tap preview to rotate", color = Color(0xFF7F849C), fontSize = 11.sp)
                    }
                    Switch(
                        checked = localAllowVertical,
                        onCheckedChange = { localAllowVertical = it },
                        colors = SwitchDefaults.colors(
                            checkedThumbColor = MaterialTheme.colorScheme.primary,
                            checkedTrackColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.5f)
                        )
                    )
                }

                Spacer(modifier = Modifier.height(4.dp))
                HorizontalDivider(color = Color(0xFF45475A), thickness = 1.dp)
                Spacer(modifier = Modifier.height(4.dp))
                Text("SRT Latency", color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.Bold, fontSize = 14.sp)
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column {
                        Text("Automatic", color = MaterialTheme.colorScheme.onSurface, fontSize = 13.sp)
                        Text("Use recommended value", color = Color(0xFF7F849C), fontSize = 11.sp)
                    }
                    Switch(
                        checked = localLatencyAuto,
                        onCheckedChange = { localLatencyAuto = it },
                        colors = SwitchDefaults.colors(
                            checkedThumbColor = MaterialTheme.colorScheme.primary,
                            checkedTrackColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.5f)
                        )
                    )
                }
                if (!localLatencyAuto) {
                    val presets = listOf(100, 120, 200, 300, 500, 800, 1000, 1500, 2000)
                    Text("Latency: ${localLatencyMs} ms", color = MaterialTheme.colorScheme.onSurface, fontSize = 13.sp, fontWeight = FontWeight.Bold)
                    Row(
                        horizontalArrangement = Arrangement.spacedBy(6.dp),
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        presets.forEach { preset ->
                            val selected = localLatencyMs == preset
                            Button(
                                onClick = { localLatencyMs = preset },
                                colors = ButtonDefaults.buttonColors(
                                    containerColor = if (selected) MaterialTheme.colorScheme.primary else Color(0xFF313244),
                                    contentColor = if (selected) Color(0xFF11111B) else Color(0xFFCDD6F4)
                                ),
                                contentPadding = PaddingValues(horizontal = 6.dp, vertical = 2.dp),
                                shape = RoundedCornerShape(6.dp),
                                modifier = Modifier.height(28.dp)
                            ) {
                                Text("${preset}", fontSize = 10.sp, fontWeight = FontWeight.Bold)
                            }
                        }
                    }
                }
            }
        },
        confirmButton = {},
        containerColor = MaterialTheme.colorScheme.surface,
        shape = RoundedCornerShape(16.dp)
    )
}
