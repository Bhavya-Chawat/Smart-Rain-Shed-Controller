/*
 * =====================================================================================
 * 🌧️ Smart Rain Shed Controller (ESP32)
 * =====================================================================================
 *
 * Automates garden shed roofing based on weather and sensor inputs.
 * Integrates rain/motion sensors, WiFi, Blynk IoT, LCD, and Google Sheets logging.
 * Manual override is available via app or physical switch.
 *
 * Core features:
 * - Real-time rain/light sensing with automatic roof open/close
 * - Remote monitoring and control via Blynk app
 * - Weather data fetched from Tomorrow.io API
 * - Google Sheets logging for analysis and debugging
 * - Rich local UI (LCD) and cloud connectivity
 * - Fail-safe & cooldown logic for motors/sensors
 *
 * Author: Bhavya
 * ESP32 | Blynk | Arduino IDE
 * =====================================================================================
 */

// ===================== GLOBAL CONFIGURATION & LIBRARIES ======================
// Uncomment to revert to previous template
// #define BLYNK_TEMPLATE_ID      "TMPL3JKDmJBIr"
// #define BLYNK_TEMPLATE_NAME    "Rain Shed Controller"
// #define BLYNK_AUTH_TOKEN       "rfNBbrarl5diKdFoBvtmZy9QgpIKtPoT"

// Blynk Template and Authentication (Replace with YOUR template details)
#define BLYNK_TEMPLATE_ID "TMPL3T16cudsA"
#define BLYNK_TEMPLATE_NAME "Smart Rain Shed Controller"
#define BLYNK_AUTH_TOKEN "BD9i1KUlSYtPhBdC-SBf6E2THSnl-h0U"

// Test/Simulation Mode: 1 = test (simulate sensors), 0 = real sensors
#define TEST_MODE 0

// Required libraries for WiFi, Blynk, HTTP requests, JSON parsing, I2C LCD, and time
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>

// ===================== GOOGLE SHEETS LOGGING SETUP =====================
// Google Apps Script endpoint for logging data (no debug print, we only send)
const char* sheetURL = "https://script.google.com/macros/s/AKfycbxVX10OVKofaB80p4CPpypEoe1MXkKtaEYHXv5AJtbO5L6f7JbMt4YOltPjc4otkhfvbg/exec";

// ===================== NETWORK & API CONFIGURATION =====================
// WiFi credentials (replace with your SSID and password)
char ssid[] = "Bhavya";
char pass[] = "@password420";

// Tomorrow.io Weather API settings (replace with your API key and location)
const char* tomorrowAPIKey = "bcshaRAOmkS7DA5kQeqgu3kkW4IrhvqN";
float latitude = 12.9716, longitude = 77.5946;  // Bangalore, India

// Weather condition strings (updated by API fetch)
char weatherCondition[16] = "Init";
char lastWeatherCondition[16] = "";

// ===================== HARDWARE CONNECTION DETAILS =====================
// I2C LCD (16x2, address typically 0x27 for most modules)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Sensor/Motor GPIO pin assignments
#define RAIN_SENSOR_PIN   34  // Digital rain sensor input
#define LDR_SENSOR_PIN    35  // LDR (light sensor) input
#define SWITCH_PIN        25  // Manual roof toggle switch (pull-up)
#define MOTOR_IN1         26  // Motor driver IN1 (roof open)
#define MOTOR_IN2         27  // Motor driver IN2 (roof close)
#define RAIN_ANALOG_PIN   32  // Analog rain sensor input (intensity)

// ===================== BLYNK IoT CLOUD VIRTUAL PINS =====================
// Virtual pins used for Blynk app widgets (see template for UI mapping)
#define VPIN_MANUAL_MODE      V0  // Toggle manual/auto mode
#define VPIN_SHED_STATUS      V1  // Display shed open/closed status
#define VPIN_RAIN_STATUS      V2  // Display rain status
#define VPIN_LIGHT_STATUS     V3  // Display light status
#define VPIN_MANUAL_OPEN      V4  // Button to manually open shed
#define VPIN_MANUAL_CLOSE     V5  // Button to manually close shed
#define VPIN_WEATHER_STATUS   V6  // Display weather from API
#define VPIN_START_HOUR       V7  // Set start of automation window (hour)
#define VPIN_END_HOUR         V8  // Set end of automation window (hour)
#define VPIN_RAIN_INTENSITY   V9  // Display analog rain intensity (%)

// ===================== TIME & SCHEDULING SUPPORT =====================
// Time structure for NTP-synced local time
struct tm timeinfo;
// Default automation window: 6AM to 8PM
int startHour = 6, endHour = 20;

// ===================== SYSTEM STATE VARIABLES =====================
// Shed mechanical state
enum ShedState { IDLE, OPENING, CLOSING };
ShedState shedState = IDLE;

bool shedOpen = true;               // True if shed is open
bool manualMode = false;            // True if manual override is active
bool lcdStarted = false;            // True after LCD init succeeds
bool prevWiFiStatus = true;         // Previous WiFi connection state
bool prevBlynkStatus = true;        // Previous Blynk connection state

unsigned long motorStartTime = 0;   // When motor last started (ms)
unsigned long lastMotorActionTime = 0;  // Last time motor was activated (ms)
const unsigned long motorRunDuration = 5000;  // Max motor runtime (ms)
const unsigned long motorCooldown = 3000;     // Motor cooldown period (ms)

unsigned long startupTime;          // When system booted (ms)
bool pendingUpdate = false;         // True if UI needs refresh

// Previous values for change detection
bool lastManualMode = false;
bool lastShedOpen = true;
int lastRainVal = -1;
int lastLightVal = -1;

bool lastTimeWindowStatus = true;   // Previous auto-window state
BlynkTimer timer;                   // Timer for periodic tasks

// Rain sensor debounce
bool rainCheckActive = false;
unsigned long rainCheckStart = 0;
const unsigned long rainCheckDuration = 5000;  // Rain confirmation period (ms)
unsigned long autoModeLastCheck = 0;
const unsigned long autoCheckInterval = 2000;  // Auto-mode check interval (ms)

// LCD fail-safe status display
bool lcdFailNotified = false;

// Loop timing for watchdog
static unsigned long lastLoopTime = 0;

// ===================== TEST/SIMULATION OVERRIDES =====================
#if TEST_MODE
// Simulated sensor values for testing without hardware
int fakeRainDigital = 1;
int fakeLdr = 0;
int fakeRainAnalog = 2000;

// Save original digitalRead/analogRead functions
int digitalReadOrig(int pin) { return ::digitalRead(pin); }
int analogReadOrig(int pin)  { return ::analogRead(pin); }

// Override digitalRead/analogRead for testing
#undef digitalRead
#undef analogRead
#define digitalRead(PIN) \
  (PIN == RAIN_SENSOR_PIN ? fakeRainDigital : \
  (PIN == LDR_SENSOR_PIN ? fakeLdr : digitalReadOrig(PIN)))
#define analogRead(PIN) \
  (PIN == RAIN_ANALOG_PIN ? fakeRainAnalog : analogReadOrig(PIN))

// Serial commands to simulate sensor inputs
void serialTestControl() {
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n'); cmd.trim();
        if (cmd.startsWith("rain ")) {
            fakeRainDigital = cmd.substring(5).toInt();
            Serial.print("[TEST] Digital Rain: "); Serial.println(fakeRainDigital ? "No Rain" : "Rain Detected");
        }
        if (cmd.startsWith("ldr ")) {
            fakeLdr = cmd.substring(4).toInt();
            Serial.print("[TEST] LDR: "); Serial.println(fakeLdr ? "Dark" : "Bright");
        }
        if (cmd.startsWith("intensity ")) {
            fakeRainAnalog = cmd.substring(10).toInt();
            Serial.print("[TEST] Rain Intensity (ADC): "); Serial.println(fakeRainAnalog);
        }
    }
}
#endif

// ===================== HELPER FUNCTIONS =====================

// Returns human-readable rain sensor status
const char* getRainStatus() { return digitalRead(RAIN_SENSOR_PIN) == 0 ? "Rain Detected" : "No Rain"; }

// Returns human-readable light sensor status
const char* getLightStatus() { return digitalRead(LDR_SENSOR_PIN) == 0 ? "Bright" : "Dark"; }

// Returns true if weatherAPI reports rain
bool isWeatherAPIRain() {
    return strcmp(weatherCondition, "HvRain") == 0 ||
           strcmp(weatherCondition, "LtRain") == 0 ||
           strcmp(weatherCondition, "Drzl") == 0;
}

// Convert Tomorrow.io weather code to text
const char* getWeatherDescription(int code) {
    switch (code) {
        case 1000: return "Clear";
        case 1001: return "Cloudy";
        case 4200: return "LtRain";
        case 4201: return "HvRain";
        case 4000: return "Drzl";
        case 5000: return "Snow";
        case 5100: return "LtSnow";
        case 6000: return "FrzDrz";
        default:   return "Unknown";
    }
}

// Log an event to Blynk timeline and Serial
void logEvent(const char *event, const char *msg) {
    Blynk.logEvent(event, msg);
    Serial.printf("[BLYNK] %s\n", msg);
}

// Check if WiFi is connected
bool isWiFiConnected() { return WiFi.status() == WL_CONNECTED; }

// Check if Blynk is connected
bool isBlynkConnected() { return Blynk.connected(); }

// Update Blynk virtual pin if value changed (optimize cloud traffic)
void updateBlynkIfChanged(int vpin, char* prev, const char* current) {
    if (strcmp(prev, current) != 0) {
        Blynk.virtualWrite(vpin, current);
        strncpy(prev, current, 15);
        prev[15] = '\0';
    }
}

// Convert ESP32 reset reason to text for debugging
const char* resetReasonToText(int reason) {
    switch (reason) {
        case 1: return "POWERON_RESET";
        case 3: return "SW_RESET";
        case 4: return "WDT_RESET";
        default: return "Other";
    }
}

// Return current timestamp as string (YYYY-MM-DD HH:MM:SS)
String getTimestamp() {
    if (getLocalTime(&timeinfo))
        return String(timeinfo.tm_year+1900) + "-" + String(timeinfo.tm_mon+1) + "-" +
               String(timeinfo.tm_mday) + " " + String(timeinfo.tm_hour) + ":" +
               String(timeinfo.tm_min) + ":" + String(timeinfo.tm_sec);
    else
        return String(millis()/1000) + "s";
}

// Send a row of sensor/system data to Google Sheets
// Only sends when WiFi is connected; no debug prints.
void logToGoogleSheets(const char* rainStatus, const char* lightStatus, const char* weather,
                       const char* shedState, const char* mode, const char* event) {
    if(WiFi.status() != WL_CONNECTED) return;
    String payload = "{";
    payload += "\"timestamp\":\"" + getTimestamp() + "\",";
    payload += "\"rain_status\":\"" + String(rainStatus) + "\",";
    payload += "\"ldr_status\":\"" + String(lightStatus) + "\",";
    payload += "\"weather\":\"" + String(weather) + "\",";
    payload += "\"shed_state\":\"" + String(shedState) + "\",";
    payload += "\"mode\":\"" + String(mode) + "\",";
    payload += "\"event\":\"" + String(event ? event : "") + "\"";
    payload += "}";
    HTTPClient http;
    http.setTimeout(5000);
    http.begin(sheetURL);
    http.addHeader("Content-Type", "application/json");
    http.POST(payload);
    http.end();
}

// Read analog rain intensity
int readRainAnalog() { return analogRead(RAIN_ANALOG_PIN); }

// Convert raw analog value to rain intensity percent
// Calibrate map() values for your sensor!
int getRainPercent() {
    int val = readRainAnalog();
    int percent = map(val, 4095, 250, 0, 100);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return percent;
}

// ===================== SETUP FUNCTIONS =====================

// Connect to WiFi network
void setupWiFi() {
    WiFi.begin(ssid, pass);
    int retries = 0;
    Serial.print("[WIFI] Connecting ");
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        Serial.print(".");
        retries++;
    }
    Serial.println(WiFi.status() == WL_CONNECTED ? " CONNECTED!" : " FAILED!");
}

// Initialize Blynk cloud connection
void setupBlynk() {
    Blynk.config(BLYNK_AUTH_TOKEN);
    Blynk.connect(5000);
    Serial.println("[BLYNK] Initialization complete.");
}

// Initialize 16x2 I2C LCD
void setupLCD() {
    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("  Rain Shed  ");
    lcd.setCursor(0, 1); lcd.print("   Controller   ");
    delay(1200);
    lcd.clear();
    lcd.setCursor(3,0); lcd.print("Smart Shed");
    lcd.setCursor(0,1); lcd.print("Init sensors...");
    delay(900);
    lcd.clear();
    lcdStarted = true;
}

// Configure all sensor and switch pins
void setupSensors() {
    pinMode(RAIN_SENSOR_PIN, INPUT);
    pinMode(RAIN_ANALOG_PIN, INPUT);
    pinMode(LDR_SENSOR_PIN, INPUT);
    pinMode(SWITCH_PIN, INPUT_PULLUP); // switch toggles to GND
}

// Configure motor control pins
void setupMotor() {
    pinMode(MOTOR_IN1, OUTPUT);
    pinMode(MOTOR_IN2, OUTPUT);
    digitalWrite(MOTOR_IN1, LOW);
    digitalWrite(MOTOR_IN2, LOW);
}

// ===================== TIMER FUNCTIONS =====================

// Refresh UI and send sensor values to Blynk
void sendUIandBlynk() {
    updateBlynkStatus();
    updateLCDDisplay();
    int rainPercent = getRainPercent();
    Blynk.virtualWrite(VPIN_RAIN_INTENSITY, rainPercent);
}

// Send sensor/system data to Google Sheets periodically
void logDataToSheet() {
    logToGoogleSheets(
        getRainStatus(), getLightStatus(), weatherCondition,
        shedOpen ? "Open" : "Closed",
        manualMode ? "Manual" : "Auto", "SAMPLE"
    );
}

// Set up periodic tasks (UI, logging, weather, LCD fail-safe)
void setupTimers() {
    timer.setInterval(1000L, sendUIandBlynk);         // UI/Blynk status every 1s
    timer.setInterval(30000L, logDataToSheet);        // Google Sheets log every 30s
    timer.setInterval(600000L, fetchWeather);         // Weather API update every 10 min
    timer.setInterval(2000L, showFailSafeStatusOnLCD); // LCD fail-safe check every 2s
}

// ===================== WEATHER API INTEGRATION =====================

// Fetch current weather from Tomorrow.io and update weatherCondition
void fetchWeather() {
    Serial.println("\n=== [WEATHER API] FETCH ===");
    if (!isWiFiConnected()) {
        Serial.println("[WEATHER] WiFi missing! Weather: Unknown");
        strncpy(weatherCondition, "Unknown", 15);
        return;
    }
    HTTPClient http;
    String url = String("https://api.tomorrow.io/v4/weather/realtime?location=") +
                String(latitude, 6) + "," + String(longitude, 6) +
                "&apikey=" + tomorrowAPIKey;
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode != 200) {
        Serial.printf("[WEATHER] HTTP error: %d\n", httpCode);
        strncpy(weatherCondition, "Unknown", 15); http.end(); return;
    }
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        Serial.println("[WEATHER] JSON failed"); Serial.println(payload);
        strncpy(weatherCondition, "Unknown", 15); http.end(); return;
    }
    int code = doc["data"]["values"]["weatherCode"];
    strncpy(weatherCondition, getWeatherDescription(code), 15);
    weatherCondition[15] = '\0';
    Serial.printf("[WEATHER] Code: %d | Condition: %s\n", code, weatherCondition);

    // Log event to Blynk and update last weather
    if (strcmp(weatherCondition, lastWeatherCondition) != 0) {
        String msg = String("Weather Update: ") + weatherCondition;
        logEvent("weather_update", msg.c_str());
        strncpy(lastWeatherCondition, weatherCondition, 15); lastWeatherCondition[15] = '\0';
    }
    http.end();
}

// ===================== LCD & USER INTERFACE =====================

// Update the 16x2 LCD display with sensor/system state
void updateLCDDisplay() {
    static char prevWeather[16] = "";
    static bool prevShedOpen = true;
    static bool prevManualMode = false;
    static char prevRain[12] = "";
    const char* rainText = digitalRead(RAIN_SENSOR_PIN) == 0 ? "Rain" : "NoRn";
    const char* modeText = manualMode ? "MAN " : "AUTO";
    const char* lightText = digitalRead(LDR_SENSOR_PIN) == 0 ? "Br" : "Dr";
    int rainPercent = getRainPercent();
    if (
        strcmp(prevWeather, weatherCondition) != 0 ||
        shedOpen != prevShedOpen ||
        manualMode != prevManualMode ||
        strcmp(prevRain, rainText) != 0
    ) {
        lcd.clear();
        lcd.setCursor(0,0);
        lcd.print("Wx:"); lcd.print(weatherCondition);
        lcd.print(" S:"); lcd.print(shedOpen ? "Op" : "Cl");
        lcd.setCursor(0,1);
        lcd.print(modeText);
        lcd.print(" R:"); lcd.print(rainText);
        lcd.print(" I:"); lcd.print(rainPercent); lcd.print("%");
        Serial.printf("[LCD] Wx:%s S:%s %s R:%s Int:%d%% L:%s\n",
            weatherCondition, shedOpen ? "Op":"Cl", modeText, rainText, rainPercent, lightText);
        strncpy(prevWeather, weatherCondition, 15); prevWeather[15] = '\0';
        prevShedOpen = shedOpen; prevManualMode = manualMode;
        strncpy(prevRain, rainText, 11); prevRain[11] = '\0';
    }
}

// Send latest sensor/system state to Blynk app
void updateBlynkStatus() {
    static char prevShedStatus[8] = "", prevWeatherStatus[16] = "", prevRainStatus[16] = "", prevLightStatus[8] = "";
    const char* shedStat = shedOpen ? "Open" : "Closed";
    updateBlynkIfChanged(VPIN_WEATHER_STATUS, prevWeatherStatus, weatherCondition);
    updateBlynkIfChanged(VPIN_SHED_STATUS, prevShedStatus, shedStat);
    updateBlynkIfChanged(VPIN_RAIN_STATUS, prevRainStatus, getRainStatus());
    updateBlynkIfChanged(VPIN_LIGHT_STATUS, prevLightStatus, getLightStatus());
}

// ===================== FAIL-SAFE & CLOUD STATUS =====================

// Show WiFi/Blynk connection status on LCD (fail-safe)
void showFailSafeStatusOnLCD() {
    bool nowWifi = isWiFiConnected(), nowBlynk = isBlynkConnected();
    if ((!nowWifi || !nowBlynk) && !lcdFailNotified) {
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print(nowWifi ? " Blynk Lost " : " WiFi Lost ");
        lcd.setCursor(0, 1); lcd.print("    Cloud Down   ");
        lcdFailNotified = true;
        Serial.println("[LCD] Cloud Fault Shown");
    } else if (nowWifi && nowBlynk && lcdFailNotified) {
        lcdFailNotified = false;
        lcd.clear();
        Serial.println("[LCD] Cloud Restored");
    }
    prevWiFiStatus = nowWifi; prevBlynkStatus = nowBlynk;
}

// ===================== MOTOR CONTROL LOGIC =====================

// Return true if motor is in cooldown (cannot start yet)
bool motorCooldownActive() { return millis() - lastMotorActionTime < motorCooldown; }

// Set new shed mechanical state (OPENING, CLOSING, IDLE)
void setMotorState(ShedState newState) {
    if (shedState != IDLE) { Serial.println("[MOTOR] Ignored: Motor not idle"); return; }
    if (motorCooldownActive()) { Serial.println("[MOTOR] Ignored: Cooldown active"); return; }
    if (newState == OPENING && shedOpen) { Serial.println("[MOTOR] Ignored: Shed already open"); return; }
    if (newState == CLOSING && !shedOpen) { Serial.println("[MOTOR] Ignored: Shed already closed"); return; }
    if (newState == OPENING) {
        digitalWrite(MOTOR_IN1, HIGH); digitalWrite(MOTOR_IN2, LOW);
        Serial.println("[ACTION] 🚪 Shed Opening...");
    } else if (newState == CLOSING) {
        digitalWrite(MOTOR_IN1, LOW); digitalWrite(MOTOR_IN2, HIGH);
        Serial.println("[ACTION] 🚪 Shed Closing...");
    }
    motorStartTime = millis();
    lastMotorActionTime = millis();
    shedState = newState;
    pendingUpdate = true;
}

// Convenience functions to open/close shed
void openShed() { setMotorState(OPENING); }
void closeShed() { setMotorState(CLOSING); }

// ===================== RAIN SENSOR DEBOUNCE LOGIC =====================

// Check for rain sensor triggers, with confirmation period to avoid false positives
void falseTriggerPrevention() {
    int rainSensor = digitalRead(RAIN_SENSOR_PIN); // 0 = rain
    if (rainCheckActive) {
        if (millis() - rainCheckStart >= rainCheckDuration) {
            if (digitalRead(RAIN_SENSOR_PIN) == 0) {
                closeShed();
                Serial.println("[FALSE_TRIG] Sensor steady 5s - Closing shed.");
            } else {
                Serial.println("[FALSE_TRIG] False alarm - Not closing.");
            }
            rainCheckActive = false;
        }
        return;
    }
    if (rainSensor == 0 && isWeatherAPIRain()) {
        closeShed();
        Serial.println("[FALSE_TRIG] Rain sensor+API detected rain. Shed closing.");
    } else if (rainSensor == 0 && !isWeatherAPIRain()) {
        Serial.println("[FALSE_TRIG] Rain sensor only, confirming 5s...");
        rainCheckActive = true; rainCheckStart = millis();
    }
}

// ===================== BLYNK CALLBACKS =====================

// Blynk app writes to V0 (Manual Mode toggle): set manualMode variable
BLYNK_WRITE(VPIN_MANUAL_MODE) {
    manualMode = param.asInt();
    Serial.print("[BLYNK] Manual Mode toggled ");
    Serial.println(manualMode ? "ON" : "OFF");
}

// Blynk app writes to V4 (Manual Open button): open shed if in manual mode
BLYNK_WRITE(VPIN_MANUAL_OPEN) {
    if (manualMode && !shedOpen && param.asInt() == 1) {
        Serial.println("[BLYNK] Manual Open button pressed");
        openShed();
    }
}

// Blynk app writes to V5 (Manual Close button): close shed if in manual mode
BLYNK_WRITE(VPIN_MANUAL_CLOSE) {
    if (manualMode && shedOpen && param.asInt() == 1) {
        Serial.println("[BLYNK] Manual Close button pressed");
        closeShed();
    }
}

// Blynk app writes to V7 (Start Hour): update automation window start time
BLYNK_WRITE(VPIN_START_HOUR) {
    startHour = param.asInt();
    Serial.print("[BLYNK] Start Hour set to: "); Serial.println(startHour);
}

// Blynk app writes to V8 (End Hour): update automation window end time
BLYNK_WRITE(VPIN_END_HOUR) {
    endHour = param.asInt();
    Serial.print("[BLYNK] End Hour set to: "); Serial.println(endHour);
}

// ===================== ARDUINO SETUP & LOOP =====================

// Initialize all hardware, networking, and start background tasks
void setup() {
    Serial.begin(115200);
    Serial.println("\n==============================");
    Serial.println(" 🌧️  Smart Rain Shed Controller [REAL SENSOR]");
    Serial.println("==============================");
    Serial.printf("Boot Reason: %d (%s)\n", esp_reset_reason(), resetReasonToText(esp_reset_reason()));
    Serial.println("----------------------------------");
    setupSensors(); setupMotor(); Wire.begin();
    setupLCD();
    shedOpen = true;
    Serial.println("[BOOT] Shed state = OPEN");
    setupWiFi(); setupBlynk();

    // Sync ESP32 clock via NTP
    configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
    int ntpTries = 0;
    Serial.print("[TIME] Syncing time");
    while (!getLocalTime(&timeinfo) && ntpTries < 10) {
        Serial.print("."); delay(500); ntpTries++;
    }
    Serial.println(getLocalTime(&timeinfo) ? " ✅" : " ❌ Using defaults.");
    setupTimers(); fetchWeather(); startupTime = millis();
    lastLoopTime = millis();
    Serial.println("=== SETUP DONE ===\n");
}

// Main control loop: process sensor inputs, manage automation, handle UI
void loop() {
    #if TEST_MODE
    serialTestControl();
    #endif

    // Monitor loop timing (watchdog for unexpected delays)
    unsigned long nowLoopTime = millis();
    unsigned long loopDuration = nowLoopTime - lastLoopTime;
    if (loopDuration > 500) {
        Serial.printf("[WDT] Loop slow: %lu ms\n", loopDuration);
    }
    lastLoopTime = nowLoopTime;

    // Run Blynk background tasks and timer handlers
    Blynk.run(); timer.run();

    // ----------- MANUAL SWITCH (PHYSICAL BUTTON) LOGIC ----------
    static bool lastSwitchState = HIGH;
    bool switchState = digitalRead(SWITCH_PIN);

    // Edge detection with debouncing for physical switch
    static unsigned long lastSwitchTrig = 0;
    if (lastSwitchState == HIGH && switchState == LOW && millis() - lastSwitchTrig > 500) {
        if (shedOpen) {
            closeShed();
            logEvent("shed_status", "Switch closed shed");
            logToGoogleSheets(getRainStatus(), getLightStatus(), weatherCondition,
                shedOpen ? "Open" : "Closed", manualMode ? "Manual" : "Auto", "SW_CLOSED");
            Serial.println("[SWITCH] Shed Closed (by switch)");
        } else {
            openShed();
            logEvent("shed_status", "Switch opened shed");
            logToGoogleSheets(getRainStatus(), getLightStatus(), weatherCondition,
                shedOpen ? "Open" : "Closed", manualMode ? "Manual" : "Auto", "SW_OPENED");
            Serial.println("[SWITCH] Shed Opened (by switch)");
        }
        lastSwitchTrig = millis();
    }
    lastSwitchState = switchState;
    // ----------- END PHYSICAL SWITCH SECTION -----------

    // ----- AUTOMATION LOGIC -----
    int rainVal = digitalRead(RAIN_SENSOR_PIN);
    int lightVal = digitalRead(LDR_SENSOR_PIN);

    // Only automate if: manual mode off, switch not toggled, shed idle
    if (!manualMode && switchState == HIGH && shedState == IDLE) {
        // Don't check too often (limit rate)
        if (millis() - autoModeLastCheck > autoCheckInterval) {
            autoModeLastCheck = millis();
            // Only allow automation after system settles
            if (millis() - startupTime > 5000 && getLocalTime(&timeinfo)) {
                int hourNow = timeinfo.tm_hour;
                bool inTimeWindow = hourNow >= startHour && hourNow <= endHour;
                if (inTimeWindow) {
                    if (!lastTimeWindowStatus) {
                        Serial.println("[AUTO] 🚦 Time window entered: Automation active.");
                        lastTimeWindowStatus = true;
                    }
                    // If shed is open, check for rain (may need to close)
                    if (shedOpen) falseTriggerPrevention();
                    // If shed is closed, check for safe conditions to open
                    else if (!shedOpen && rainVal == 1 && lightVal == 0) {
                        openShed();
                        Serial.println("[AUTO] ✅ Bright/No Rain, Shed Opening!");
                    }
                } else {
                    if (lastTimeWindowStatus) {
                        Serial.println("[AUTO] 📴 Out of auto time window, shed paused.");
                        lastTimeWindowStatus = false;
                    }
                }
            }
        }
    }
    // Continue rain confirmation logic if active
    if (rainCheckActive) falseTriggerPrevention();

    // ----- MOTOR STOP LOGIC -----
    // After motor has run for motorRunDuration, stop it
    if ((shedState == OPENING || shedState == CLOSING) &&
            millis() - motorStartTime >= motorRunDuration) {
        digitalWrite(MOTOR_IN1, LOW); digitalWrite(MOTOR_IN2, LOW);
        shedOpen = (shedState == OPENING);
        shedState = IDLE;
        updateLCDDisplay();
        // Log event to Blynk and Google Sheets
        String event = shedOpen ? "SHED_OPENED" : "SHED_CLOSED";
        String details = String("Shed ") + (shedOpen ? "Opened" : "Closed") + " via " + (manualMode ? "Manual" : "Auto");
        logEvent("shed_status", details.c_str());
        logToGoogleSheets(getRainStatus(), getLightStatus(), weatherCondition,
            shedOpen ? "Open" : "Closed", manualMode ? "Manual" : "Auto", event.c_str());
        Serial.printf("[ACTION] 🏠 Shed %s!\n", shedOpen ? "OPENED" : "CLOSED");
        pendingUpdate = false;
    }

    // ----- STATUS SNAPSHOTS -----
    // Periodically print sensor/system state to Serial for debugging
    static unsigned long lastStatusPrint = 0;
    if (millis() - lastStatusPrint > 4000 &&
        (rainVal != lastRainVal || lightVal != lastLightVal ||
         manualMode != lastManualMode || shedOpen != lastShedOpen)
    ) {
        Serial.println("\n---- [STATUS SNAPSHOT] ----");
        Serial.print("Rain: ");   Serial.println(rainVal ? "No" : "YES");
        Serial.print("Light: ");  Serial.println(lightVal ? "Dark" : "Bright");
        Serial.print("Mode: ");   Serial.println(manualMode ? "MANUAL" : "AUTO");
        Serial.print("Shed: ");   Serial.println(shedOpen ? "OPEN" : "CLOSED");
        Serial.print("Rain %: "); Serial.println(getRainPercent());
        Serial.println("---------------------------\n");
        lastRainVal = rainVal;
        lastLightVal = lightVal;
        lastManualMode = manualMode;
        lastShedOpen = shedOpen;
        lastStatusPrint = millis();
    }
}
