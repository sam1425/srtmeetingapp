package com.meeting.srt.ui

import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import kotlin.math.sin

private val barBg = Color(0x33000000)

@Composable
fun AudioBar(
    level: Float,
    modifier: Modifier = Modifier,
) {
    var breathPhase by remember { mutableStateOf(0f) }
    LaunchedEffect(Unit) {
        while (true) {
            breathPhase += 0.04f
            delay(30)
        }
    }
    val idleBreath = 0.05f + 0.04f * sin(breathPhase)

    val rawTarget = if (level < 0.01f) idleBreath else level * 5f
    val displayLevel by animateFloatAsState(
        targetValue = rawTarget.coerceIn(0f, 1f),
        animationSpec = spring(
            dampingRatio = Spring.DampingRatioMediumBouncy,
            stiffness = Spring.StiffnessVeryLow,
        ),
        label = "audioLevel",
    )

    Box(modifier = modifier) {
        Box(
            modifier = Modifier
                .width(28.dp)
                .fillMaxHeight(0.45f)
                .clip(RoundedCornerShape(14.dp))
                .background(barBg),
            contentAlignment = Alignment.BottomCenter,
        ) {
            Box(
                modifier = Modifier
                    .width(28.dp)
                    .fillMaxHeight(displayLevel.coerceAtLeast(0.001f))
                    .clip(RoundedCornerShape(14.dp))
                    .background(Color(0xFF00E676)),
            )
        }
    }
}
