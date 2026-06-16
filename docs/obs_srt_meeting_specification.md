# Technical Specification: OBS SRT Meeting App & Plugin

This document outlines the design, architecture, and implementation roadmap for the **OBS SRT Meeting App & Plugin**. The system enables remote participants to stream high-quality, low-latency video and audio directly into a broadcaster's OBS Studio instance using the Secure Reliable Transport (SRT) protocol, with automated scene and source orchestration.

---

## 1. Supported Platforms
* **Supported**: Windows, Linux
* **Unsupported**: macOS (for now)

---

## 2. System Architecture & Metadata Transmission

Instead of running a separate WebSocket server and exchanging complex JSON payloads, the application uses **SRT StreamID-based signaling**. All participant metadata (such as the participant's display name) is encoded directly into the SRT `StreamID` connection parameter.

### Connection & StreamID Scheme
When a participant connects, they initiate an SRT caller connection using a URL formatted as:
`srt://<broadcaster_ip>:<port>?mode=caller&streamid=username:<display_name>`

* **Example**: `srt://192.168.1.100:9000?mode=caller&streamid=username:Jane_Doe`

### Architectural Diagram

```mermaid
graph TD
    subgraph Participant App [Participant Client App (Windows/Linux)]
        Cap[Camera/Mic Capture] --> Enc[FFmpeg Encoder]
        Enc --> SRT_Send[SRT Stream Sender]
        Settings[Name Input] -->|Inject Name into StreamID| SRT_Send
    end

    subgraph Broadcaster OBS [OBS Studio (Windows/Linux) with Plugin]
        SRT_Listener[libsrt Listener / Broker thread] -->|Detects Connection & parses StreamID| Plugin_Main
        Plugin_Main[Plugin Logic & Qt Dock UI] --> OBS_Auto[OBS Automation Engine]
        OBS_Auto -->|API Calls| OBS_Core[libobs Core / Scenes & Sources]
        
        %% Native OBS SRT Ingestion
        OBS_Core -->|Spawns Native Source| OBS_SRT_Src[OBS Built-in Media/SRT Source]
    end

    SRT_Send -->|SRT Stream with StreamID| SRT_Listener
    SRT_Listener -->|Forward Stream Packets| OBS_SRT_Src
```

---

## 3. Stream Multiplexing & Native OBS Integration

The project relies entirely on **OBS's built-in SRT source capabilities** (via the native Media Source powered by FFmpeg).

To accommodate multiple participants without requiring the broadcaster to open a wide range of firewall ports, we will implement a lightweight **SRT Stream Broker** thread inside the OBS plugin:
1. The OBS plugin opens a single SRT socket in `listener` mode on a designated port (e.g., UDP `9000`).
2. When a participant's client app connects, the Broker accepts the connection, reads the `StreamID` (e.g., `username:Jane_Doe`), and registers the participant name.
3. The Broker opens a local loopback port or assigns a internal port (e.g., `127.0.0.1:10001`) and relays the SRT packets to it.
4. The OBS Automation Engine dynamically spawns a native OBS **Media Source** configured to play from that local loopback port:
   - **Input URL**: `srt://127.0.0.1:10001?mode=listener`
   - **Input Format**: `mpegts`
   - **Settings**: Low latency parameters (`buffering_mb=1`, `timeout=5000000`).
5. When the participant disconnects, the Broker thread closes the local relay, and the Automation Engine deletes/deactivates the OBS source.

---

## 4. OBS Broadcaster Plugin Design

The C++ plugin will compile on Windows and Linux with `ENABLE_QT` and `ENABLE_FRONTEND_API` enabled.

### Components:
1. **`plugin-main.cpp`**: Initializes the OBS module, registers UI docks, and hooks into OBS lifecycle events.
2. **`SRTBroker`**: 
   - A thread utilizing `libsrt` to listen for incoming participant streams on the external port.
   - Demultiplexes incoming connections based on their `StreamID`.
   - Provisions local SRT loopback relays for OBS sources.
3. **`OBSAutomation`**:
   - Spawns native Media Sources for each participant.
   - Places the sources into a specific scene or creates dedicated participant scenes.
   - Automatically positions participant feeds in a grid layout.
4. **Qt Dock UI**:
   - Lists active participants (display names parsed from StreamIDs).
   - Shows network stats (latency, packet loss) per participant.
   - Provides checkboxes to select which participants to include in the active scene.

---

## 5. Participant Client App Design

The client is a standalone desktop application (`obs-srt-meeting-client`) targeting Windows and Linux.

### Key Characteristics:
* **Standalone UI**: Simple Qt interface with device selection (camera, microphone) and connection input fields (Broadcaster IP, Port, Display Name).
* **Direct Capture & Encode**:
  - Uses FFmpeg libraries (`libavcodec`, `libavformat`, `libavdevice`) or Qt Multimedia to capture hardware video/audio.
  - Encodes video to H.264 (low latency preset) and audio to AAC.
  - Streams the muxed MPEG-TS output directly using `libsrt` Caller mode, embedding `streamid=username:<display_name>` in the connection.

---

## 6. Implementation Roadmap

### Phase 1: Specification & Design (Current)
* [x] Define platforms (Windows & Linux).
* [x] Design StreamID metadata connection mechanism.
* [x] Detail OBS native SRT source orchestration.

### Phase 2: Broadcaster Plugin & SRT Broker
* [ ] Integrate `libsrt` library in CMake configuration.
* [ ] Implement the `SRTBroker` listener thread to accept incoming connections and parse `StreamID`.
* [ ] Implement local loopback relaying mechanism for accepted streams.

### Phase 3: OBS Source Automation
* [ ] Implement `OBSAutomation` to dynamically create native OBS Media Sources pointing to loopback streams.
* [ ] Create layout helper functions to auto-arrange sources in active scenes.

### Phase 4: Standalone Participant Client App
* [ ] Build Qt-based camera/microphone selector UI.
* [ ] Implement raw frame capture, encoding (H.264/AAC), and SRT transmission using FFmpeg/libsrt.
* [ ] Package client executable as part of the plugin build artifacts.

### Phase 5: UI & Testing
* [ ] Add Broadcaster Qt Dock panel in OBS Studio.
* [ ] Implement end-to-end testing between Client and Plugin on Linux and Windows.
