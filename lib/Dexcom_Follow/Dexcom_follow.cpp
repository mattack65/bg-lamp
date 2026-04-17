#include "Arduino.h"
#include "Dexcom_follow.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Dexcom base API urls
const char *DEXCOM_BASE_URL = "https://share2.dexcom.com/ShareWebServices/Services";
const char *DEXCOM_BASE_URL_OUS = "https://shareous1.dexcom.com/ShareWebServices/Services";

const char *DEFAULT_SESSION_ID = "00000000-0000-0000-0000-000000000000";

// Trend directions mapping
const char *DEXCOM_TREND_DIRECTIONS[] = {
    "None", // unconfirmed
    "DoubleUp",
    "SingleUp",
    "FortyFiveUp",
    "Flat",
    "FortyFiveDown",
    "SingleDown",
    "DoubleDown",
    "NotComputable", // unconfirmed
    "RateOutOfRange" // unconfirmed
};

// Trend arrows
const char *DEXCOM_TREND_ARROWS[] = {
    "",
    "↑↑",
    "↑",
    "↗",
    "→",
    "↘",
    "↓",
    "↓↓",
    "?",
    "-"};

// String applicationId = "d8665ade-9673-4e27-9ff6-92db4ce13d13";
String applicationId = "d89443d2-327c-4a6f-89e5-496bbb0317db";
const char *DEXCOM_APPLICATION_ID = "d89443d2-327c-4a6f-89e5-496bbb0317db"; // alternative

/*
    dsfefsef
*/
Follower::Follower(bool ous, String user, String pass, String sessionID)
{
    Username = user;
    Password = pass;
    SessionID = sessionID;
    update_json_string();
    if (ous)
    {
        DexcomServer = DEXCOM_BASE_URL_OUS;
    }
    else
    {
        DexcomServer = DEXCOM_BASE_URL;
    }
};

void Follower::update_json_string()
{
    String jsonString = "{";
    jsonString += "\"accountName\": \"" + Username + "\",";
    jsonString += "\"applicationId\": \"" + applicationId + "\",";
    jsonString += "\"password\": \"" + Password + "\"";
    jsonString += "}";
    jsonStr = jsonString;
};

void Follower::Set_user_pass(String user, String pass)
{
    Username = user;
    Password = pass;
    update_json_string();
};

void Follower::Set_sessionID(String sessionID)
{
    SessionID = sessionID;
};

// ChatGPT-corrected Version
bool Follower::getNewSessionID()
{
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    HTTPClient http;
    String response;

    // Step 1: authenticate username/password -> account ID
    String authJson = "{";
    authJson += "\"accountName\": \"" + Username + "\",";
    authJson += "\"applicationId\": \"" + applicationId + "\",";
    authJson += "\"password\": \"" + Password + "\"";
    authJson += "}";

    String authUrl = DexcomServer + "/General/AuthenticatePublisherAccount";
    http.begin(authUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept-Encoding", "application/json");

    int httpCode = http.POST(authJson);

    Serial.print("Auth HTTP code: ");
    Serial.println(httpCode);

    if (httpCode != HTTP_CODE_OK) {
        Serial.println("AuthenticatePublisherAccount failed");
        http.end();
        return false;
    }

    response = removeCharacterFromString(http.getString(), '\"');
    http.end();

    Serial.println("Account ID response:");
    Serial.println(response);

    if (response.length() == 0 || response == DEFAULT_SESSION_ID) {
        Serial.println("Invalid account ID");
        return false;
    }

    AccountID = response;

    // Step 2: account ID -> session ID
    String loginJson = "{";
    loginJson += "\"accountId\": \"" + AccountID + "\",";
    loginJson += "\"applicationId\": \"" + applicationId + "\",";
    loginJson += "\"password\": \"" + Password + "\"";
    loginJson += "}";

    String loginUrl = DexcomServer + "/General/LoginPublisherAccountById";
    http.begin(loginUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept-Encoding", "application/json");

    httpCode = http.POST(loginJson);

    Serial.print("Login HTTP code: ");
    Serial.println(httpCode);

    if (httpCode != HTTP_CODE_OK) {
        Serial.println("LoginPublisherAccountById failed");
        http.end();
        return false;
    }

    response = removeCharacterFromString(http.getString(), '\"');
    http.end();

    Serial.println("Session ID response:");
    Serial.println(response);

    if (response.length() == 0 || response == DEFAULT_SESSION_ID) {
        Serial.println("Invalid session ID");
        return false;
    }

    SessionID = response;
    return true;
}

bool Follower::SessionIDnotDefault(){
    if (SessionID == "00000000-0000-0000-0000-000000000000"){
        return false;
    }
    else if (SessionID == ""){
        return false;
    }
    else{
        return true;
    }
}

// ChatGPT-corrected version
bool Follower::GlucoseLevelsNow()
{
    bool result = false;
    if (WiFi.status() == WL_CONNECTED)
    {
        HTTPClient http;

        String url = DexcomServer + "/Publisher/ReadPublisherLatestGlucoseValues?sessionId=";
        url += SessionID;
        url += "&minutes=1440&maxCount=1";

        http.begin(url);
        http.addHeader("Accept-Encoding", "application/json");

        int httpResponseCode = http.GET();
        Serial.print("Glucose HTTP code: ");
        Serial.println(httpResponseCode);

        String response = http.getString();
        Serial.println("Glucose raw response:");
        Serial.println(response);

        if (httpResponseCode == HTTP_CODE_OK)
        {
            StaticJsonDocument<512> doc;
            DeserializationError error = deserializeJson(doc, response);
            if (error)
            {
                Serial.print("Error parsing JSON response: ");
                Serial.println(error.c_str());
                http.end();
                return false;
            }

            GlucoseNow.mg_dl = doc[0]["Value"];
            GlucoseNow.mmol_l = convertToMmol(GlucoseNow.mg_dl);
            GlucoseNow.trend_description = doc[0]["Trend"].as<const char *>();
            GlucoseNow.trend_Symbol = getTrendSymbol(GlucoseNow.trend_description.c_str());
            GlucoseNow.timestamp = convertToUnixTimestamp(doc[0]["DT"].as<const char *>());

            result = true;
        }
        else
        {
            Serial.print("HTTP GET request failed, error code: ");
            Serial.println(httpResponseCode);
            result = false;
        }

        http.end();
    }
    return result;
}


double Follower::convertToMmol(int mgdl)
{
    return mgdl / 18.01559;
};

String Follower::removeCharacterFromString(String input, char characterToRemove)
{
    String result;
    for (char c : input)
    {
        if (c != characterToRemove)
        {
            result += c;
        }
    }
    return result;
};

unsigned long Follower::convertToUnixTimestamp(const char *dtValue)
{
    // Expected format: Date(1776433081000+0200)

    const char *start = strchr(dtValue, '(');
    if (!start) {
        return 0;
    }
    start++; // move past '('

    // Find first non-digit character after the milliseconds value
    const char *end = start;
    while (*end && isdigit(*end)) {
        end++;
    }

    if (end == start) {
        return 0;
    }

    char buffer[20];
    size_t len = end - start;
    if (len >= sizeof(buffer)) {
        return 0;
    }

    strncpy(buffer, start, len);
    buffer[len] = '\0';

    unsigned long long ms = strtoull(buffer, nullptr, 10);
    return (unsigned long)(ms / 1000ULL);
}

const char *Follower::getTrendSymbol(const char *trendDescription)
{
    if (strcmp(trendDescription, "None") == 0)
    {
        return "";
    }
    else if (strcmp(trendDescription, "DoubleUp") == 0)
    {
        return "^^";
    }
    else if (strcmp(trendDescription, "SingleUp") == 0)
    {
        return "^";
    }
    else if (strcmp(trendDescription, "FortyFiveUp") == 0)
    {
        return "/^";
    }
    else if (strcmp(trendDescription, "Flat") == 0)
    {
        return "->";
    }
    else if (strcmp(trendDescription, "FortyFiveDown") == 0)
    {
        return "\\v";
    }
    else if (strcmp(trendDescription, "SingleDown") == 0)
    {
        return "v";
    }
    else if (strcmp(trendDescription, "DoubleDown") == 0)
    {
        return "vv";
    }
    else if (strcmp(trendDescription, "NotComputable") == 0)
    {
        return "?";
    }
    else if (strcmp(trendDescription, "RateOutOfRange") == 0)
    {
        return "-";
    }
    else
    {
        return ""; // Default to an empty string for unknown trends
    }
};
