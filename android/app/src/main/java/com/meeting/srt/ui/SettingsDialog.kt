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
    allowVertical: Boolean,
    onAllowVerticalChange: (Boolean) -> Unit,
    isActive: Boolean,
    onDismiss: () -> Unit,
    onLogProfileChange: (String) -> Unit
) {
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
                    onClick = onDismiss,
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
                    onValueChange = { if (!isActive) onIpChange(it) },
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
                        value = port,
                        onValueChange = { if (!isActive) onPortChange(it) },
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
                        value = name,
                        onValueChange = { if (!isActive) onNameChange(it) },
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
                        val isSelected = selectedProfile == profile
                        val containerColor = if (isSelected) MaterialTheme.colorScheme.primary else Color(0xFF313244)
                        val contentColor = if (isSelected) Color(0xFF11111B) else Color(0xFFCDD6F4)
                        Button(
                            onClick = {
                                if (!isActive) {
                                    onProfileChange(profile)
                                    onLogProfileChange(profile)
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
                        checked = allowVertical,
                        onCheckedChange = onAllowVerticalChange,
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
