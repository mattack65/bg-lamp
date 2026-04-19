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
  StartupTestMode,
  BgMode,
  WhiteMode
};

struct BgState {
  bool valid = false;
  double mmol = 0.0;
  String trend;
  unsigned long timestamp = 0;
};

struct ColorPoint {
  float mmol;
  CRGB color;
};

const ColorPoint colorMap[] = {
  {2.0f,  CRGB(0xFF, 0x00, 0x00)},
  {3.0f,  CRGB(0xFF, 0x90, 0x00)},
  {4.0f,  CRGB(0x40, 0xFF, 0x00)},
  {5.0f,  CRGB(0x00, 0xFF, 0x00)},
  {6.0f,  CRGB(0x00, 0xFF, 0x08)},
  {7.0f,  CRGB(0x00, 0xF0, 0x80)},
  {8.0f,  CRGB(0x00, 0x70, 0xFF)},
  {9.0f,  CRGB(0x8E, 0x00, 0xFF)},
  {10.0f, CRGB(0xFF, 0x00, 0xF6)}
};

const float testValues[] = {
  2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 4.5f, 5.0f, 5.5f,
  6.0f, 6.5f, 7.0f, 7.5f, 8.0f, 8.5f, 9.0f, 9.5f, 10.0f
};

LampMode mode = LampMode::StartupTestMode;
BgState bg;

// outside US = true
Follower follower(true, DEXCOM_USER, DEXCOM_PASS);

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

uint8_t readBrightness() {
  int raw = analogRead(POT_PIN);
  raw = constrain(raw, 0, 4095);

  float x = raw / 4095.0f;
  float y = powf(x, 2.2f);
  // here we can limit the max brightness
  // float max_brightness = 255.0f;
  float max_brightness = 255.0f; // limit the power consumption to 3 A max
  uint8_t brightness = (uint8_t)(y * max_brightness + 0.5f);

  // Serial.print("Brightness set to: ");
  // Serial.println(brightness);
  return brightness;
}

CRGB interpolateColor(const CRGB& a, const CRGB& b, float t) {
  t = constrain(t, 0.0f, 1.0f);

  uint8_t r = (uint8_t)(a.r + (b.r - a.r) * t + 0.5f);
  uint8_t g = (uint8_t)(a.g + (b.g - a.g) * t + 0.5f);
  uint8_t bl = (uint8_t)(a.b + (b.b - a.b) * t + 0.5f);

  return CRGB(r, g, bl);
}

CRGB colorForBg(double mmol) {
  const int count = sizeof(colorMap) / sizeof(colorMap[0]);

  if (mmol <= colorMap[0].mmol) {
    return colorMap[0].color;
  }

  if (mmol >= colorMap[count - 1].mmol) {
    return colorMap[count - 1].color;
  }

  for (int i = 0; i < count - 1; i++) {
    float x0 = colorMap[i].mmol;
    float x1 = colorMap[i + 1].mmol;

    if (mmol >= x0 && mmol <= x1) {
      float t = (mmol - x0) / (x1 - x0);
      return interpolateColor(colorMap[i].color, colorMap[i + 1].color, t);
    }
  }

  return CRGB::Black;
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
        if (mode == LampMode::StartupTestMode) {
          mode = LampMode::BgMode;
          Serial.println("Leaving StartupTestMode -> BgMode");
        } else {
          mode = (mode == LampMode::BgMode) ? LampMode::WhiteMode : LampMode::BgMode;
          Serial.printf("Mode changed to: %s\n",
                        (mode == LampMode::BgMode) ? "BgMode" : "WhiteMode");
        }
      }
    }
  }

  lastButtonState = currentState;
}

void runStartupTestMode() {
  static unsigned long lastStepTime = 0;
  static int testIndex = 0;
  static CRGB currentTestColor = CRGB::Black;
  const unsigned long stepMs = 1500;

  if (millis() - lastStepTime >= stepMs) {
    lastStepTime = millis();

    float mmol = testValues[testIndex];
    currentTestColor = colorForBg(mmol);

    Serial.print("Test mode: ");
    Serial.print(mmol, 1);
    Serial.print(" mmol/L -> RGB(");
    Serial.print(currentTestColor.r);
    Serial.print(", ");
    Serial.print(currentTestColor.g);
    Serial.print(", ");
    Serial.print(currentTestColor.b);
    Serial.println(")");

    testIndex++;
    const int count = sizeof(testValues) / sizeof(testValues[0]);
    if (testIndex >= count) {
      testIndex = 0;
    }
  }

  showColor(currentTestColor);
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

  if (mode == LampMode::StartupTestMode) {
    runStartupTestMode();
  } else if (mode == LampMode::WhiteMode) {
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