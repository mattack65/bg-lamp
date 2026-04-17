#include <Arduino.h>
#include <WiFi.h>
#include <FastLED.h>
#include <time.h>
#include <math.h>

#include "Dexcom_follow.h"
#include "secrets.h"

static constexpr uint8_t LED_PIN = 18;
static constexpr int NUM_LEDS = 100;
static constexpr uint8_t POT_PIN = 34;
static constexpr uint8_t BUTTON_PIN = 19;

CRGB leds[NUM_LEDS];

enum class LampMode {
  BgMode,
  WhiteMode
};

struct BgState {
  bool valid = false;
  double mmol = 0.0;
  String trend;
  unsigned long timestamp = 0;
};

LampMode mode = LampMode::BgMode;
BgState bg;

// outside US = true
Follower follower(true, DEXCOM_USER, DEXCOM_PASS);

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

String formatTimestamp(unsigned long ts) {
  time_t raw = (time_t)ts;
  struct tm* timeinfo = localtime(&raw);

  if (!timeinfo) {
    return "invalid time";
  }

  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
  return String(buffer);
}

uint8_t readBrightness() {
  int raw = analogRead(POT_PIN);
  raw = constrain(raw, 0, 4095);

  float x = raw / 4095.0f;
  float y = powf(x, 2.2f);

  return (uint8_t)(y * 255.0f + 0.5f);
}

void handleButton() {
  static bool lastButtonState = HIGH;
  static bool stableState = HIGH;
  static unsigned long lastChangeTime = 0;
  const unsigned long debounceMs = 30;

  bool currentState = digitalRead(BUTTON_PIN);

  if (currentState != lastButtonState) {
    lastChangeTime = millis();
  }

  if ((millis() - lastChangeTime) > debounceMs) {
    if (currentState != stableState) {
      stableState = currentState;

      if (stableState == LOW) {
        mode = (mode == LampMode::BgMode) ? LampMode::WhiteMode : LampMode::BgMode;
        Serial.printf("Mode changed to: %s\n",
                      (mode == LampMode::BgMode) ? "BgMode" : "WhiteMode");
      }
    }
  }

  lastButtonState = currentState;
}

CRGB colorForBg(double mmol) {
  if (mmol < 3.0)  return CRGB::Red;
  if (mmol < 3.9)  return CRGB::Orange;
  if (mmol <= 10.0) return CRGB::Green;
  if (mmol <= 13.9) return CRGB::Blue;
  return CRGB::Purple;
}

void showColor(const CRGB& color) {
  fill_solid(leds, NUM_LEDS, color);
  FastLED.show();
}

bool updateDexcomReading() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    connectWifi();
  }

  if (!follower.SessionIDnotDefault()) {
    Serial.println("Requesting Dexcom session...");
    if (!follower.getNewSessionID()) {
      Serial.println("Failed to get Dexcom session");
      return false;
    }
  }

  if (!follower.GlucoseLevelsNow()) {
    Serial.println("Failed to get glucose reading");
    return false;
  }

  bg.valid = true;
  bg.mmol = follower.GlucoseNow.mmol_l;
  bg.trend = follower.GlucoseNow.trend_description;
  bg.timestamp = follower.GlucoseNow.timestamp;

  Serial.print("BG updated: ");
  Serial.print(bg.mmol, 1);
  Serial.print(" mmol/L, trend ");
  Serial.print(bg.trend);
  Serial.print(", time ");
  Serial.println(formatTimestamp(bg.timestamp));

  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(POT_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  showColor(CRGB::Black);

  connectWifi();

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
  tzset();

  updateDexcomReading();
}

void loop() {
  handleButton();

  static unsigned long lastDexcomPoll = 0;
  const unsigned long dexcomPollMs = 60000;

  if (millis() - lastDexcomPoll >= dexcomPollMs) {
    lastDexcomPoll = millis();
    updateDexcomReading();
  }

  uint8_t brightness = readBrightness();
  FastLED.setBrightness(brightness);

  if (mode == LampMode::WhiteMode) {
    showColor(CRGB::White);
  } else {
    if (bg.valid) {
      showColor(colorForBg(bg.mmol));
    } else {
      showColor(CRGB::Black);
    }
  }

  delay(20);
}