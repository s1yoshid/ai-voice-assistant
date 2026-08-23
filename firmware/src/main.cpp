#include <Arduino.h>
#include <M5Unified.h>
#include <FastLED.h>
#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include "freertos/ringbuf.h"
#include "secrets.h"

// Only 1 RGB LED on Atom Echo
#define NUM_LEDS 1
// Atom Echo RGB LED (SK6812) on GPIO 27
#define LED_PIN 27

// Define the array of leds
CRGB leds[NUM_LEDS];

const char *WIFI_SSID = SECRET_SSID;
const char *WIFI_PASSWORD = SECRET_PASS;
const char *SERVER_URL = SECRET_URL;

using namespace websockets;
WebsocketsClient client;

// ─── Audio constants ───────────────────────────────────────────────────────────
#define SAMPLE_RATE          16000
#define NUM_SAMPLES_RECORD   128                                // Samples per I2S DMA block
#define WS_BUFFER_SAMPLES    2048                               // Samples per outbound WebSocket send
#define WS_BUFFER_SIZE       (WS_BUFFER_SAMPLES * sizeof(int16_t))
#define AUDIO_CHUNK_SIZE     2048                               // Bytes per inbound playback chunk

// ─── Recording: double-buffer ──────────────────────────────────────────────────
#define NUM_WS_BUFFERS 2
int16_t ws_buffer[NUM_WS_BUFFERS][WS_BUFFER_SAMPLES];
int     active_buf       = 0;
size_t  recorded_samples = 0;

// ─── Playback: FreeRTOS No-Split ring buffer ───────────────────────────────────
// Holds 4 chunks of AUDIO_CHUNK_SIZE bytes each.
// No-Split guarantees each item is contiguous → can pass pointer directly to playRaw.
#define PLAYBACK_RING_SLOTS 4
// FreeRTOS No-Split ring buffer needs a small overhead per item (8 bytes header).
#define PLAYBACK_RING_SIZE  ((AUDIO_CHUNK_SIZE + 8) * PLAYBACK_RING_SLOTS)

RingbufHandle_t playback_rb = nullptr;
bool            speaker_ready = false; // gate enableSpeaker() to one call per interaction

// ─── Helpers ───────────────────────────────────────────────────────────────────
void enableMic()
{
    M5.Speaker.end();
    speaker_ready = false;

    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate = SAMPLE_RATE;
    mic_cfg.stereo      = false;
    M5.Mic.config(mic_cfg);
    M5.Mic.begin();
}

void enableSpeaker()
{
    if (speaker_ready) return; // already initialised for this interaction
    M5.Mic.end();

    auto spk_cfg = M5.Speaker.config();
    // sample_rate intentionally omitted — let it default to 48000
    // spk_cfg.sample_rate = SAMPLE_RATE;
    spk_cfg.stereo      = false;
    M5.Speaker.config(spk_cfg);
    M5.Speaker.begin();

    speaker_ready = true;
}

// ─── WebSocket callback ────────────────────────────────────────────────────────
// Runs inside client.poll() — must return quickly.
// Only responsibility: push raw bytes into the ring buffer.
void onMessageCallback(WebsocketsMessage message)
{
    if (message.isBinary())
    {
        const uint8_t *rawData   = (const uint8_t *)message.c_str();
        size_t         totalBytes = message.length();

        Serial.printf("[RX] %d bytes from server\n", totalBytes);

        // Enable speaker on first chunk of a new response
        enableSpeaker();

        // Non-blocking send into ring buffer (timeout = 0).
        // If the buffer is full we drop the chunk rather than blocking poll().
        BaseType_t sent = xRingbufferSend(playback_rb, rawData, totalBytes, 0);
        if (sent != pdTRUE)
        {
            Serial.println("[WARN] Playback ring buffer full — chunk dropped!");
        }
    }
    else if (message.isText())
    {
        String msg = message.data();
        if (msg == "PLAYBACK_DONE")
        {
            Serial.println("[INFO] Server finished streaming.");
            // Don't call Speaker.end() here — let loop() drain the ring buffer first.
            // Speaker.end() is called after the ring buffer empties (see loop()).
        }
        else
        {
            Serial.printf("[MSG] %s\n", msg.c_str());
        }
    }
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);

    Serial.begin(115200);
    Serial.println("Atom Echo initialised");

    FastLED.addLeds<SK6812, LED_PIN, GRB>(leds, NUM_LEDS);

    // Create the No-Split ring buffer before connecting
    playback_rb = xRingbufferCreate(PLAYBACK_RING_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (playback_rb == nullptr)
    {
        Serial.println("[ERROR] Failed to create playback ring buffer!");
    }

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\nConnected: %s\n", WiFi.localIP().toString().c_str());

    client.onMessage(onMessageCallback);

    Serial.println("Connecting to WebSocket server...");
    if (client.connect(SERVER_URL))
        Serial.println("WebSocket connected!");
    else
        Serial.println("[ERROR] WebSocket connection failed.");

    leds[0] = CRGB::Red;
    FastLED.show();
    Serial.println("Hold button to record.");
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop()
{
    M5.update();

    // Keep WebSocket alive and drain TCP receive buffer
    if (client.available())
        client.poll();

    // ── Playback: feed ring buffer → speaker ──────────────────────────────────
    // Only attempt if speaker is initialised and not currently playing a chunk.
    if (speaker_ready && !M5.Speaker.isPlaying())
    {
        size_t   itemSize = 0;
        void    *item     = xRingbufferReceive(playback_rb, &itemSize, 0); // non-blocking

        if (item != nullptr)
        {
            M5.Speaker.playRaw((const int16_t *)item, itemSize / sizeof(int16_t),
                               SAMPLE_RATE, false, 1, 0);

            // Return item to ring buffer *after* handing pointer to DMA.
            // playRaw with stop_current_audio=false queues internally, so the
            // DMA will keep reading this memory until the chunk finishes.
            // We must NOT return the item until playback of this chunk is done.
            // So we wait here (briefly, in loop — not in callback).
            while (M5.Speaker.isPlaying())
                vTaskDelay(pdMS_TO_TICKS(1));

            vRingbufferReturnItem(playback_rb, item);
        }
        else
        {
            // Ring buffer empty and speaker idle — all chunks drained
            if (speaker_ready)
            {
                // Check if server signalled done (no more chunks expected)
                // speaker_ready stays true until next mic session,
                // so we only end the speaker when the buffer has been fully drained.
                // (PLAYBACK_DONE handler already printed the log; nothing else needed here.)
            }
        }
    }

    // ── Recording: button held ────────────────────────────────────────────────
    if (M5.BtnA.wasPressed())
    {
        leds[0] = CRGB::Green;
        FastLED.show();
        Serial.println("[REC] Started.");
        enableMic();
    }

    if (M5.BtnA.isPressed())
    {
        if (M5.Mic.record(&ws_buffer[active_buf][recorded_samples], NUM_SAMPLES_RECORD))
        {
            recorded_samples += NUM_SAMPLES_RECORD;

            if (recorded_samples >= WS_BUFFER_SAMPLES)
            {
                int send_buf = active_buf;
                active_buf   = 1 - active_buf; // swap before sending

                client.sendBinary((char *)ws_buffer[send_buf],
                                  WS_BUFFER_SAMPLES * sizeof(int16_t));
                recorded_samples = 0;
            }
        }
    }

    if (M5.BtnA.wasReleased())
    {
        Serial.println("[REC] Stopped.");
        leds[0] = CRGB::Red;
        FastLED.show();

        // Flush any remaining partial buffer
        if (recorded_samples > 0)
        {
            client.sendBinary((char *)ws_buffer[active_buf],
                              recorded_samples * sizeof(int16_t));
            recorded_samples = 0;
        }

        // Signal end-of-stream to server
        client.send("STOP_STREAM");
    }
}
