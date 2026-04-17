#include <Arduino.h>
#include <WiFi.h>
#include "Dexcom_follow.h"
#include "secrets.h"

Follower follower(true, DEXCOM_USER, DEXCOM_PASS);   // true = outside US

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

String formatTimestamp(unsigned long ts)
{
    time_t raw = (time_t)ts;
    struct tm *timeinfo = localtime(&raw);

    if (!timeinfo) {
        return "invalid time";
    }

    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    return String(buffer);
}

void printReading() {
  Serial.print("BG: ");
  Serial.print(follower.GlucoseNow.mg_dl);
  Serial.print(" mg/dL   ");

  Serial.print(follower.GlucoseNow.mmol_l, 1);
  Serial.print(" mmol/L   ");

  Serial.print("Trend: ");
  Serial.print(follower.GlucoseNow.trend_description);
  Serial.print(" ");
  Serial.print(follower.GlucoseNow.trend_Symbol);
  Serial.print("   ");

  Serial.print("Timestamp: ");
  Serial.println(formatTimestamp(follower.GlucoseNow.timestamp));
}


void setup() {
  Serial.begin(115200);
  delay(1000);

  connectWifi();

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
  tzset();

  Serial.println("Requesting Dexcom session...");
  if (!follower.getNewSessionID()) {
    Serial.println("Failed to get Dexcom session ID");
    return;
  }

  Serial.println("Requesting latest glucose...");
  if (!follower.GlucoseLevelsNow()) {
    Serial.println("Failed to get glucose reading");
    return;
  }

  printReading();
}

void loop() {
  static unsigned long lastPoll = 0;
  const unsigned long pollMs = 60000;   // 1 minute

  if (millis() - lastPoll >= pollMs) {
    lastPoll = millis();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost, reconnecting...");
      connectWifi();
    }

    if (follower.GlucoseLevelsNow()) {
      printReading();
    } else {
      Serial.println("Reading failed");
    }
  }
}