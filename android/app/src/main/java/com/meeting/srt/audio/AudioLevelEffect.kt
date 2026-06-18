package com.meeting.srt.audio

import com.pedro.encoder.input.audio.CustomAudioEffect
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlin.math.abs

class AudioLevelEffect : CustomAudioEffect() {

    private val _level = MutableStateFlow(0f)
    val level: StateFlow<Float> = _level

    override fun process(pcmBuffer: ByteArray): ByteArray {
        var peak = 0
        for (i in pcmBuffer.indices step 2) {
            val sample = ((pcmBuffer[i + 1].toInt() shl 8) or (pcmBuffer[i].toInt() and 0xFF)).toShort().toInt()
            val amplitude = abs(sample)
            if (amplitude > peak) peak = amplitude
        }
        _level.value = (peak / Short.MAX_VALUE.toFloat()).coerceIn(0f, 1f)
        return pcmBuffer
    }
}
