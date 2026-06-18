package com.meeting.srt

sealed class StreamState {
    object Idle : StreamState()
    object Connecting : StreamState()
    object Streaming : StreamState()
    object Stopping : StreamState()
    data class Reconnecting(val attempt: Int, val maxAttempts: Int) : StreamState()
}

enum class ConnectionQuality { Good, Fair, Poor }
