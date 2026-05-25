#include <Arduino.h>
#include <M5Unified.h>
#include <FastLED.h>
#include <WiFi.h>
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

void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);

    Serial.begin(115200);
    Serial.println("Atom Echo initialized with M5Unified on PioArduino!");

    FastLED.addLeds<SK6812, LED_PIN, GRB>(leds, NUM_LEDS);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Attempting to connect to network");

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.print("You're connected to the network: ");
    Serial.println(WiFi.localIP());

    leds[0] = CRGB::Red;
    FastLED.show();
    Serial.println("Hold button to record");
}

void loop()
{
    M5.delay(50);
    M5.update();

    if (M5.BtnA.wasPressed())
    {
        leds[0] = CRGB::Green;
        FastLED.show();
        Serial.println("Button was pressed.");
    }
    if (M5.BtnA.isPressed()) {
        Serial.println("Button is being pressed.");
    }
    if (M5.BtnA.wasReleased())
    {
        Serial.println("Button was released.");

        // simulate API call
        leds[0] = CRGB::Blue;
        FastLED.show();
        M5.delay(5000);

        leds[0] = CRGB::Red;
        FastLED.show();
    }
}