# SRT Meeting App

Android streaming client + OBS Studio plugin for real-time participant video via SRT (Secure Reliable Transport).

## Overview

- **Android app** streams camera + mic to an SRT broker using RTMP-style publish paths.
- **OBS plugin** runs an SRT broker on a UDP port, accepts incoming streams, relays them to loopback ports, and dynamically creates/removes `ffmpeg_source` (Media Source) OBS sources for each connected participant.
- **Latency**: Client sends `?latency=<μs>` (manual mode) or `?latency=auto` (OBS decides). The broker parses the StreamID query params and configures the loopback latency per-participant.

## Features

- Dynamic/static participant source management
- Per-participant relay threads with automatic cleanup
- Latency slider (20–1000ms) + auto toggle on the client
- OBS dock UI with default latency spinner for auto clients
- Connection quality indicator (green/yellow/red based on SRT RTT)
- Port allocation from a pool (10000–10100)
- Automatic reconnection handling

## Project Structure

```
├── android/              # Android app (Kotlin, Jetpack Compose, RootEncoder)
├── src/
│   ├── plugin-main.cpp   # OBS plugin: SRT broker, dock UI, source management
│   └── client/           # Standalone Qt desktop client (minimal)
├── cmake/                # CMake build system config per platform
├── .github/              # CI/CD workflows + build/packaging scripts
└── cmake/windows/installer.iss.in  # Inno Setup installer template
```

## Build

### OBS Plugin

```bash
cmake --preset windows-x64   # Windows (VS 17 2022)
cmake --preset macos-universal   # macOS (Xcode, universal binary)
cmake --preset ubuntu-x86_64     # Linux (Ninja)
cmake --build --preset <preset>
```

### Android Client

Open `android/` in Android Studio, or:

```bash
cd android && ./gradlew assembleRelease
```

## Installation

### Windows
The CI produces a signed Inno Setup installer (`.exe`) that installs per-user to `%APPDATA%\obs-studio\plugins\srt-meeting-app\bin\64bit\`. A `.zip` is also provided for manual install.

### Linux
Copy `build_x86_64/rundir/RelWithDebInfo/srt-meeting-app/` to `~/.config/obs-studio/plugins/` or use the `.deb` package.

### macOS
Use the `.pkg` installer from the release.

## Documentation

- [OBS SRT Meeting Specification](docs/obs_srt_meeting_specification.md) — system design and SRT broker architecture
- [Android Client Specification](docs/android_client_specification.md) — app design and streaming setup
- [Design Updates](docs/design_updates.md) — UI/UX changes

## CI/CD

GitHub Actions build for all three platforms on push/PR/tag. Tagged releases (`1.2.3`, `1.2.3-beta1`) create draft releases with signed installers (Windows `.exe`, macOS `.pkg`, Ubuntu `.deb`).

Windows signing requires `WINDOWS_SIGNING_CERT` (base64 PFX) and `WINDOWS_SIGNING_PASSWORD` secrets.

## License

GPLv2 — see [LICENSE](LICENSE).
