package com.meeting.srt.ui

import androidx.compose.animation.animateColor
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateDp
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.updateTransition
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp

@Composable
fun RecordButton(
    isStreaming: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    val transition = updateTransition(targetState = isStreaming, label = "recordButton")

    val outlineColor by transition.animateColor(
        transitionSpec = {
            spring(dampingRatio = Spring.DampingRatioNoBouncy, stiffness = Spring.StiffnessMedium)
        },
        label = "outlineColor"
    ) { streaming ->
        if (streaming) Color(0xFFE24B4A) else Color.White
    }

    val centerSize by transition.animateDp(
        transitionSpec = {
            spring(dampingRatio = Spring.DampingRatioMediumBouncy, stiffness = Spring.StiffnessLow)
        },
        label = "centerSize"
    ) { streaming ->
        if (streaming) 32.dp else 54.dp
    }

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
        Box(
            modifier = Modifier
                .size(76.dp)
                .border(4.dp, outlineColor, CircleShape)
        )
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
