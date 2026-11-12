# AI Voice Assistant (Audio Streaming Layer)

Author: Shuusei Yoshida

An ESP32-based voice device that records audio from its microphone, streams it over a WebSocket to a FastAPI server running in Docker, and plays the audio the server sends back through its speaker. Built on the M5Stack Atom Echo.

**Current status:** the full audio path works end to end (mic -> server -> speaker). The server buffers the recording until it receives `STOP_STREAM`, then sends the same audio back in chunks without processing it. Speech-to-text, LLM and text-to-speech stages are not implemented yet; see [Roadmap](#roadmap).

## How it works

```
Atom Echo (ESP32-PICO-D4)                          Server (Docker, Python)
  SPM1423 mic -> I2S DMA -> double buffer  ---->   WebSocket /ws  (port 8080)
  FreeRTOS ring buffer -> I2S -> speaker   <----   audio chunks back to the device
```

**Push to record.** Hold the button to record. Release it and the device sends a `STOP_STREAM` command over the WebSocket to end the recording. While the button is not pressed, the device is in speaker mode and plays whatever the server sends. The microphone and speaker share one I2S port, so the device switches between the two modes instead of running both at once.

### Audio parameters

| Item | Value |
|---|---|
| Sample rate | 16,000 Hz |
| Channels | Mono |
| Sample format | 16-bit signed PCM (`int16_t`) |
| Capture buffering | Double buffer, 2,048 samples per buffer, one WebSocket send per filled buffer |
| I2S DMA block size | 128 samples |
| Playback chunk | 2,048 bytes per inbound chunk |
| Playback buffering | FreeRTOS no-split ring buffer, 4 slots |

## Repository layout

```
firmware/        ESP32 firmware (PlatformIO project)
server/          FastAPI WebSocket server and Dockerfile
*.code-workspace VS Code workspace file
```

## Hardware

- M5Stack Atom Echo (ESP32-PICO-D4, SPM1423 PDM microphone, built-in speaker)
- USB cable for flashing
- A computer on the same Wi-Fi network to run the server

## Running it

### 1. Server (requires Docker)

```bash
cd server
docker build -t server:latest .
docker run -d -p 8080:8080 --name my-running-server server:latest
```

The device connects to `ws://<server-ip>:8080/ws`. Use the LAN IP of the machine running Docker, and allow port 8080 through that machine's firewall.

### 2. Firmware (VS Code + PlatformIO)

1. Open the workspace file in VS Code with the PlatformIO extension installed.
2. In `firmware/src/`, copy `secrets.h.example` to `secrets.h` and fill in your values:
   ```c
   #define SECRET_SSID "{wifi_id}"
   #define SECRET_PASS "{wifi_pass}"
   #define SECRET_URL  "ws://{ip_of_backend_server:port_of_server}/ws"
   ```
3. Build and upload to the Atom Echo from PlatformIO.

### Library pinning (important)

`platformio.ini` pins `M5Unified@0.2.15` and `M5GFX@0.2.27`. With M5Unified alone, PlatformIO pulls in M5GFX 0.3.0, which detected the Atom Echo as an Atom Lite on my unit and left the microphone unable to record. Pinning M5GFX below 0.3.0 fixes it. Check this before updating dependencies.

## Known limitations

- Playback is slightly stuttery. I have not yet determined whether the cause is the speaker hardware or my playback buffering design (chunk size, ring buffer depth, or I2S timing).
- No speech processing yet. The server only plays the recording back after you release the button, so there is no transcription, no response generation and no voice synthesis.
- Tested on one Atom Echo and one Windows development machine.

## Roadmap

- Fix playback stutter (tune ring buffer slots and chunk sizes, then isolate speaker versus firmware).
- Server-side speech-to-text (Whisper).
- Local LLM for response generation.
- Text-to-speech (Kokoro-82M) streamed back to the device.
- Measure end-to-end latency once the pipeline exists.

## Tech

C++ (Arduino framework on PlatformIO), M5Unified / M5GFX, ArduinoWebsockets, FastLED, FreeRTOS, I2S, Python, FastAPI, WebSockets, Docker.
