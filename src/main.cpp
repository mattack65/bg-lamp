#include <Arduino.h>
#include <WiFi.h>
#include <FastLED.h>
#include <time.h>
#include <math.h>

#include "Dexcom_follow.h"
#include "secrets.h"

// Pin assignment and strip size.
// LED_PIN drives the WS2812B data line.
// POT_PIN reads the brightness potentiometer.
// BUTTON_PIN reads the mode toggle button.
static constexpr uint8_t LED_PIN = 18;
static constexpr int NUM_LEDS = 100;
static constexpr uint8_t POT_PIN = 34;
static constexpr uint8_t BUTTON_PIN = 19;

// Pixel buffer for the whole strip.
// Each entry stores one RGB color for one physical LED.
CRGB leds[NUM_LEDS];

// High-level operating modes of the lamp.
// StartupTestMode: cycles through test BG values and colors.
// BgMode: shows live color based on the latest Dexcom reading.
// WhiteMode: acts as a normal white lamp.
enum class LampMode {
  StartupTestMode,
  BgMode,
  WhiteMode
};

// Cached blood glucose state used by the lamp logic.
// valid     = whether the latest reading should currently be trusted.
// mmol      = glucose value in mmol/L.
// trend     = Dexcom trend text such as "Flat", "SingleUp", etc.
// timestamp = Unix timestamp of the reading.
struct BgState {
  bool valid = false;
  double mmol = 0.0;
  String trend;
  unsigned long timestamp = 0;
};

// One anchor point in the BG->color map.
// mmol  = glucose value at this point.
// color = RGB color associated with that value.
struct ColorPoint {
  float mmol;
  CRGB color;
};

// Anchor colors for the BG gradient.
// Intermediate values are calculated by linear interpolation
// between the two nearest anchor points.
// Values below/above the defined range are clamped.
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

// Test values for StartupTestMode.
// The lamp cycles through these values so the color map
// can be judged visually without needing live Dexcom changes.
const float testValues[] = {
  2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 4.5f, 5.0f, 5.5f,
  6.0f, 6.5f, 7.0f, 7.5f, 8.0f, 8.5f, 9.0f, 9.5f, 10.0f
};

// Global application state.
// The lamp starts in StartupTestMode at power-up.
// bg holds the most recent Dexcom reading that was successfully fetched.
LampMode mode = LampMode::StartupTestMode;
BgState bg;

// Dexcom follower object.
// 'true' means "outside US", which selects the OUS Dexcom Share server.
Follower follower(true, DEXCOM_USER, DEXCOM_PASS);

// Convert a Unix timestamp into a human-readable local time string.
// The local timezone is configured in setup() using TZ + tzset().
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

// Connect the ESP32 to Wi-Fi in station mode.
// This function blocks until a connection has been established.
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

// Read the brightness potentiometer and return a stable logical
// brightness value in the range 0..255.
//
// Notes:
// - Multiple ADC samples are averaged to reduce noise.
// - A small hysteresis is used so +/-1 jitter does not cause visible flicker.
// - The current mapping is linear (powf(..., 1.0f)) because low-end dimming
//   is now mainly handled by spatial dimming rather than gamma shaping.
uint8_t readBrightness() {
  const int samples = 8;
  long sum = 0;

  for (int i = 0; i < samples; i++) {
    sum += analogRead(POT_PIN);
    delayMicroseconds(200);
  }

  int raw = sum / samples;
  raw = constrain(raw, 0, 4095);

  float x = raw / 4095.0f;
  float y = powf(x, 1.0f);

  uint8_t newBrightness = (uint8_t)(y * 255.0f + 0.5f);

  static uint8_t stableBrightness = 0;

  if (abs((int)newBrightness - (int)stableBrightness) >= 2) {
    stableBrightness = newBrightness;
  }

  // Serial.print("Brightness set to: ");
  // Serial.println(stableBrightness);

  return stableBrightness;
}


// Linearly interpolate between two RGB colors.
// t = 0.0 returns a
// t = 1.0 returns b
// Values in between produce a mixed color.
CRGB interpolateColor(const CRGB& a, const CRGB& b, float t) {
  t = constrain(t, 0.0f, 1.0f);

  uint8_t r = (uint8_t)(a.r + (b.r - a.r) * t + 0.5f);
  uint8_t g = (uint8_t)(a.g + (b.g - a.g) * t + 0.5f);
  uint8_t bl = (uint8_t)(a.b + (b.b - a.b) * t + 0.5f);

  return CRGB(r, g, bl);
}

// Map a BG value in mmol/L to a color.
// Values outside the defined anchor range are clamped.
// Values inside the range are interpolated between the nearest anchor points.
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

// Fill the whole strip with one solid color and send it out immediately.
// This is the simplest "all LEDs same color" display path.
void showColor(const CRGB& color) {
  fill_solid(leds, NUM_LEDS, color);
  FastLED.show();
}

// Refresh the cached Dexcom reading.
//
// Behavior:
// - Marks bg.valid false at the start so failed updates do not leave stale data active.
// - Reconnects Wi-Fi if needed.
// - Requests a Dexcom session if none is currently available.
// - Fetches the newest glucose reading.
// - Updates the cached bg structure on success.
// - Returns true on success, false on failure.
//
// The rest of the lamp logic uses bg.valid to decide whether BG mode
// should show a real color or go dark.

// Recovery state for Dexcom auth/session handling.
// consecutiveDexcomFailures counts back-to-back failed update attempts.
// lastSessionRefreshMs tracks when we last requested a fresh Dexcom session.
static uint8_t consecutiveDexcomFailures = 0;
static unsigned long lastSessionRefreshMs = 0;

// Refresh the cached Dexcom reading with robust session recovery.
//
// Behavior:
// - Keeps "black on stale data" semantics by setting bg.valid=false at start.
// - Reconnects Wi-Fi if needed.
// - Tries a glucose read with current session first.
// - If that fails, forces a new Dexcom session and retries once immediately.
// - After repeated failures, proactively refreshes session before reading.
// - Optionally refreshes session by age to avoid long-lived stale tokens.
//
// Returns:
// - true  = fresh reading fetched and cached in bg
// - false = no fresh reading (caller may show black/stale state)
bool updateDexcomReading() {
  bg.valid = false;

  // Tunables for resilience:
  static constexpr uint8_t FORCE_REAUTH_AFTER_FAILURES = 3;
  static constexpr unsigned long SESSION_REFRESH_INTERVAL_MS = 30UL * 60UL * 1000UL; // 30 min

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    connectWifi();
  }

  // Proactive refresh: even a "non-default" session can expire server-side.
  bool sessionTooOld = (millis() - lastSessionRefreshMs) >= SESSION_REFRESH_INTERVAL_MS;
  bool forceReauthNow = (consecutiveDexcomFailures >= FORCE_REAUTH_AFTER_FAILURES) || sessionTooOld;

  if (forceReauthNow) {
    Serial.println("Proactive Dexcom session refresh...");
    if (follower.getNewSessionID()) {
      lastSessionRefreshMs = millis();
    } else {
      Serial.println("Failed proactive Dexcom session refresh");
    }
  } else if (!follower.SessionIDnotDefault()) {
    Serial.println("No Dexcom session, requesting one...");
    if (follower.getNewSessionID()) {
      lastSessionRefreshMs = millis();
    } else {
      Serial.println("Failed to get Dexcom session");
      consecutiveDexcomFailures++;
      return false;
    }
  }

  // Attempt 1: read with current (or freshly refreshed) session.
  if (!follower.GlucoseLevelsNow()) {
    Serial.println("Dexcom glucose read failed, forcing re-login and retrying...");

    // Force a fresh session and retry once immediately.
    if (!follower.getNewSessionID()) {
      Serial.println("Failed to refresh Dexcom session after read failure");
      consecutiveDexcomFailures++;
      return false;
    }
    lastSessionRefreshMs = millis();

    if (!follower.GlucoseLevelsNow()) {
      Serial.println("Dexcom glucose read still failing after session refresh");
      consecutiveDexcomFailures++;
      return false;
    }
  }

  // Success path: cache reading and clear failure streak.
  bg.valid = true;
  bg.mmol = follower.GlucoseNow.mmol_l;
  bg.trend = follower.GlucoseNow.trend_description;
  bg.timestamp = follower.GlucoseNow.timestamp;
  consecutiveDexcomFailures = 0;

  Serial.print("BG updated: ");
  Serial.print(bg.mmol, 1);
  Serial.print(" mmol/L, trend ");
  Serial.print(bg.trend);
  Serial.print(", time ");
  Serial.println(formatTimestamp(bg.timestamp));

  return true;
}


// Handle the pushbutton with debounce.
//
// Wiring assumption:
// - BUTTON_PIN uses INPUT_PULLUP
// - unpressed = HIGH
// - pressed   = LOW
//
// Behavior:
// - First press in StartupTestMode exits test mode and enters BgMode.
// - Later presses toggle between BgMode and WhiteMode.
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

// Return a copy of a color scaled to the requested brightness.
// This is used for "baked in" pixel brightness, independent of FastLED's
// global brightness control.
CRGB scaleColor(const CRGB& color, uint8_t brightness) {
  CRGB c = color;
  c.nscale8_video(brightness);
  return c;
}

// Display a color using two different dimming strategies:
//
// 1) Spatial dimming for the very low end:
//    - Only some LEDs are lit.
//    - Lit LEDs are distributed as evenly as possible across the strip.
//    - Each lit LED uses a fixed minimum brightness that preserves hue better.
//
// 2) Normal full-strip dimming for higher brightness:
//    - All LEDs are lit.
//    - Global brightness is ramped from SPATIAL_LED_BRIGHTNESS up to 255.
//
// logicalBrightness is the user brightness from readBrightness() in the range 0..255.
void showDistributedColor(const CRGB& baseColor, uint8_t logicalBrightness) {
  fill_solid(leds, NUM_LEDS, CRGB::Black);

  if (logicalBrightness == 0) {
    FastLED.setBrightness(255);   // no extra global scaling needed
    FastLED.show();
    return;
  }

  constexpr uint8_t SPATIAL_LED_BRIGHTNESS = 6;
   
  if (logicalBrightness <= NUM_LEDS) {
    // Spatial dimming mode:
    // light exactly logicalBrightness LEDs, evenly distributed,
    // each at a stable low brightness of 4
    
    CRGB dimColor = scaleColor(baseColor, SPATIAL_LED_BRIGHTNESS);

    int activeCount = logicalBrightness;

    for (int i = 0; i < activeCount; i++) {
      int idx = (i * NUM_LEDS) / activeCount;
      if (idx >= NUM_LEDS) idx = NUM_LEDS - 1;
      leds[idx] = dimColor;
    }

    FastLED.setBrightness(255);   // already scaled into the pixel values
    FastLED.show();
    return;
  }

  // Normal full-strip brightness mode:
  // all LEDs on, brightness rises from 6 to 255
  uint8_t fullBrightness = map(logicalBrightness, NUM_LEDS + 1, 255, SPATIAL_LED_BRIGHTNESS, 255);

  fill_solid(leds, NUM_LEDS, baseColor);
  FastLED.setBrightness(fullBrightness);
  FastLED.show();
}

// Run the startup color test cycle.
//
// Every stepMs milliseconds, the next test BG value is selected,
// converted to a color, and logged to Serial.
//
// The actual displayed brightness still follows the potentiometer,
// so the test mode can also be judged at different light levels.
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

  uint8_t brightness = readBrightness();
  showDistributedColor(currentTestColor, brightness);
}



// Arduino setup() runs once after boot/reset.
//
// Tasks performed here:
// - start Serial logging
// - configure input pins
// - initialize FastLED
// - connect to Wi-Fi
// - configure local timezone handling for readable timestamps
// - fetch an initial Dexcom reading
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

// Main Arduino loop.
//
// Repeatedly:
// - checks the button
// - periodically polls Dexcom
// - reads the brightness pot
// - renders according to the active mode
//
// A small delay keeps the loop from running unnecessarily fast.
void loop() {
  handleButton();

  static unsigned long lastDexcomPoll = 0;
  const unsigned long dexcomPollMs = 60000;

  if (millis() - lastDexcomPoll >= dexcomPollMs) {
    lastDexcomPoll = millis();
    updateDexcomReading();
  }

  uint8_t brightness = readBrightness();

  if (mode == LampMode::StartupTestMode) {
    runStartupTestMode();
  } else if (mode == LampMode::WhiteMode) {
    showDistributedColor(CRGB::White, brightness);
  } else {
    if (bg.valid) {
      showDistributedColor(colorForBg(bg.mmol), brightness);
    } else {
      showDistributedColor(CRGB::Black, 0);
    }
  }

  delay(20);
}