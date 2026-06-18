package com.meeting.srt.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.unit.dp

@Composable
fun MicIcon(isAudioEnabled: Boolean, modifier: Modifier = Modifier) {
    Canvas(modifier = modifier.size(24.dp)) {
        val w = size.width
        val h = size.height
        val activeColor = Color.White
        val inactiveColor = Color(0xFFF38BA8)
        val color = if (isAudioEnabled) activeColor else inactiveColor

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
        if (isAudioEnabled) {
            drawRoundRect(
                color = color.copy(alpha = 0.4f),
                topLeft = Offset(bodyLeft + 3.dp.toPx(), bodyTop + 3.dp.toPx()),
                size = Size(bodyWidth - 6.dp.toPx(), bodyHeight - 6.dp.toPx()),
                cornerRadius = CornerRadius((bodyWidth - 6.dp.toPx()) / 2, (bodyWidth - 6.dp.toPx()) / 2)
            )
        }

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

        drawLine(
            color = color,
            start = Offset(w / 2, h * 0.35f + cradleRadius * 2),
            end = Offset(w / 2, h * 0.88f),
            strokeWidth = 2.dp.toPx(),
            cap = StrokeCap.Round
        )

        val baseWidth = w * 0.3f
        drawLine(
            color = color,
            start = Offset(w / 2 - baseWidth / 2, h * 0.88f),
            end = Offset(w / 2 + baseWidth / 2, h * 0.88f),
            strokeWidth = 2.dp.toPx(),
            cap = StrokeCap.Round
        )

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
