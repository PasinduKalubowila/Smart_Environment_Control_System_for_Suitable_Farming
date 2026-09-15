#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ==========================================
//   WIFI CREDENTIALS
// ==========================================
const char* ssid     = "Pasindu's A36";
const char* password = "Pmk@1234";

// ==========================================
//   FIREBASE CONFIG
// ==========================================
#define FIREBASE_HOST    "data-monitoring-509a1-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_API_KEY "AIzaSyAbvw_jK1h0_wnHOs5YC5vjUFE-vNqnUvE"
#define FIREBASE_URL     "https://data-monitoring-509a1-default-rtdb.asia-southeast1.firebasedatabase.app"

// ==========================================
//   PIN SETUP
// ==========================================
#define DHT_PIN           4
#define LIGHT_SENSOR_PIN 34    // 3-pin LDR  →  DO pin
#define SOIL_MOISTURE_PIN 35
#define FAN_PIN           27
#define PUMP_PIN          26
#define HUMIDIFIER_PIN    14
#define LIGHT_RELAY_PIN   33
#define LED_PIN            2
#define DHT_TYPE DHT22

// I2C LCD Pins
#define LCD_SDA_PIN      18
#define LCD_SCL_PIN      19
#define LCD_I2C_ADDR     0x27   // Try 0x3F if display shows nothing
#define LCD_COLS         16
#define LCD_ROWS          2

// ==========================================
//   THRESHOLDS & CALIBRATION
// ==========================================
#define TEMP_FAN_ON        30.0
#define TEMP_FAN_OFF       28.0
#define HUMIDITY_ON        60
#define HUMIDITY_OFF       85
#define SOIL_MOISTURE_DRY  3000
#define SOIL_MOISTURE_WET  1500
#define SOIL_MOISTURE_MIN  1000
#define SOIL_MOISTURE_MAX  4095

// ==========================================
//   SENSOR CALIBRATION
// ==========================================
#define SOIL_SAMPLE_SIZE   3
#define SENSOR_DELAY       100
#define DHT_TIMEOUT_MS     10000   // warn after 10 s with no good DHT read

// ==========================================
//   FIREBASE INTERVALS
// ==========================================
#define FIREBASE_SENSOR_INTERVAL  30000
#define FIREBASE_COMMAND_INTERVAL  5000

// ==========================================
//   LCD INTERVAL
// ==========================================
#define LCD_SCREEN_INTERVAL  3000   // rotate screen every 3 seconds

// ==========================================
//   SENSOR OBJECTS
// ==========================================
DHT dht(DHT_PIN, DHT_TYPE);
WebServer server(80);
WiFiClientSecure secureClient;
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);

// ==========================================
//   GLOBAL VARIABLES
// ==========================================
float temperature = 0;
float humidity    = 0;
int   soilMoisture = 0;
int   soilPercent  = 0;

// DHT22 last-known-good cache
float lastValidTemp     = 0;
float lastValidHumidity = 0;
unsigned long lastDHTSuccess = 0;

// 3-pin LDR  →  LOW = bright, HIGH = dark
int  lightRaw = 1;
bool isBright = false;

bool fanMode_Auto        = true;
bool pumpMode_Auto       = true;
bool humidifierMode_Auto = true;
bool lightMode_Auto      = false;

bool fanState        = false;
bool pumpState       = false;
bool humidifierState = false;
bool lightState      = false;

unsigned long lastSensorRead   = 0;
unsigned long lastFirebasePush = 0;
unsigned long lastCommandCheck = 0;
unsigned long lastLCDUpdate    = 0;

int lcdScreen = 0;   // current LCD screen index (0–3)

const unsigned long SENSOR_READ_INTERVAL = 2000;

// ==========================================
//   FORWARD DECLARATIONS
// ==========================================
void readSensors();
void autoControl();
String getHTML();
void handleRoot();
void handleFanOn();      void handleFanOff();      void handleFanMode();
void handlePumpOn();     void handlePumpOff();     void handlePumpMode();
void handleHumidifierOn(); void handleHumidifierOff(); void handleHumidifierMode();
void handleLightOn();    void handleLightOff();    void handleLightMode();
void handleNotFound();
float readDHTTemperature();
float readDHTHumidity();
int   readSoilMoisture();
void  readLightSensor();
bool  firebasePut(String path, String jsonData);
String firebaseGet(String path);
void pushSensorData();
void pushDeviceStates();
void checkFirebaseCommands();
void updateLCD();

// ==========================================
//   LCD UPDATE  —  4 rotating screens
// ==========================================
void updateLCD() {
  lcd.clear();
  bool dhtOk = (millis() - lastDHTSuccess < DHT_TIMEOUT_MS);

  switch (lcdScreen) {
    case 0:
      // Screen 0: Temperature & Humidity
      lcd.setCursor(0, 0);
      lcd.print("Temp: ");
      if (dhtOk) {
        lcd.print(String(temperature, 1));
        lcd.print((char)223);   // degree symbol
        lcd.print("C");
      } else {
        lcd.print("ERR");
      }
      lcd.setCursor(0, 1);
      lcd.print("Humidity: ");
      if (dhtOk) {
        lcd.print(String((int)humidity));
        lcd.print("%");
      } else {
        lcd.print("ERR");
      }
      break;

    case 1:
      // Screen 1: Soil Moisture
      lcd.setCursor(0, 0);
      lcd.print("Soil Moisture:");
      lcd.setCursor(0, 1);
      lcd.print(String(soilPercent));
      lcd.print("% ");
      if (soilPercent < 30) {
        lcd.print("[DRY]");
      } else if (soilPercent > 70) {
        lcd.print("[WET]");
      } else {
        lcd.print("[OK] ");
      }
      break;

    case 2:
      // Screen 2: Light & WiFi status
      lcd.setCursor(0, 0);
      lcd.print("Light: ");
      lcd.print(isBright ? "BRIGHT" : "DARK  ");
      lcd.setCursor(0, 1);
      if (WiFi.status() == WL_CONNECTED) {
        lcd.print("WiFi: Connected ");
      } else {
        lcd.print("WiFi: No Conn.  ");
      }
      break;

    case 3:
      // Screen 3: Device states
      lcd.setCursor(0, 0);
      lcd.print("F:");
      lcd.print(fanState        ? "ON " : "OFF");
      lcd.print(" P:");
      lcd.print(pumpState       ? "ON " : "OFF");
      lcd.setCursor(0, 1);
      lcd.print("H:");
      lcd.print(humidifierState ? "ON " : "OFF");
      lcd.print(" L:");
      lcd.print(lightState      ? "ON " : "OFF");
      break;
  }

  lcdScreen = (lcdScreen + 1) % 4;
}

// ==========================================
//   FIREBASE  —  PUT
// ==========================================
bool firebasePut(String path, String jsonData) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  String url = String(FIREBASE_URL) + path + ".json?auth=" + FIREBASE_API_KEY;
  secureClient.setInsecure();
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);
  int httpCode = http.PUT(jsonData);
  bool success = (httpCode == 200 || httpCode == 204);
  if (!success) { Serial.print("Firebase PUT failed. HTTP: "); Serial.println(httpCode); }
  http.end();
  return success;
}

// ==========================================
//   FIREBASE  —  GET
// ==========================================
String firebaseGet(String path) {
  if (WiFi.status() != WL_CONNECTED) return "null";
  HTTPClient http;
  String url = String(FIREBASE_URL) + path + ".json?auth=" + FIREBASE_API_KEY;
  secureClient.setInsecure();
  http.begin(secureClient, url);
  http.setTimeout(3000);
  int httpCode = http.GET();
  String payload = "null";
  if (httpCode == 200) { payload = http.getString(); }
  else { Serial.print("Firebase GET failed. HTTP: "); Serial.println(httpCode); }
  http.end();
  return payload;
}

// ==========================================
//   FIREBASE  —  PUSH SENSOR DATA
// ==========================================
void pushSensorData() {
  StaticJsonDocument<256> doc;
  doc["temperature"]           = String(temperature, 2);
  doc["humidity"]              = String(humidity, 1);
  doc["soil_moisture_percent"] = soilPercent;
  doc["light_status"]          = isBright ? "Bright" : "Dark";
  doc["dht_ok"]                = (millis() - lastDHTSuccess < DHT_TIMEOUT_MS);
  String json;
  serializeJson(doc, json);
  bool ok = firebasePut("/sensors", json);
  Serial.println(ok ? "Firebase: Sensor data pushed OK" : "Firebase: Sensor push FAILED");
}

// ==========================================
//   FIREBASE  —  PUSH DEVICE STATES
// ==========================================
void pushDeviceStates() {
  StaticJsonDocument<384> doc;
  doc["fan"]["state"]        = fanState;
  doc["fan"]["mode"]         = fanMode_Auto        ? "auto" : "manual";
  doc["pump"]["state"]       = pumpState;
  doc["pump"]["mode"]        = pumpMode_Auto       ? "auto" : "manual";
  doc["humidifier"]["state"] = humidifierState;
  doc["humidifier"]["mode"]  = humidifierMode_Auto ? "auto" : "manual";
  doc["light"]["state"]      = lightState;
  doc["light"]["mode"]       = lightMode_Auto      ? "auto" : "manual";
  String json;
  serializeJson(doc, json);
  bool ok = firebasePut("/devices", json);
  Serial.println(ok ? "Firebase: Device states pushed OK" : "Firebase: Device push FAILED");
}

// ==========================================
//   FIREBASE  —  CHECK COMMANDS
// ==========================================
void checkFirebaseCommands() {
  String payload = firebaseGet("/commands");
  if (payload == "null" || payload == "") return;

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) { Serial.print("Firebase command parse error: "); Serial.println(err.c_str()); return; }

  bool stateChanged = false;

  if (!doc["fan"].isNull()) {
    bool cmd = doc["fan"].as<bool>();
    if (cmd != fanState) {
      fanState = cmd; fanMode_Auto = false;
      digitalWrite(FAN_PIN, fanState ? HIGH : LOW);
      Serial.print("Firebase CMD: Fan -> "); Serial.println(fanState ? "ON" : "OFF");
      stateChanged = true;
    }
  }
  if (!doc["pump"].isNull()) {
    bool cmd = doc["pump"].as<bool>();
    if (cmd != pumpState) {
      pumpState = cmd; pumpMode_Auto = false;
      digitalWrite(PUMP_PIN, pumpState ? HIGH : LOW);
      Serial.print("Firebase CMD: Pump -> "); Serial.println(pumpState ? "ON" : "OFF");
      stateChanged = true;
    }
  }
  if (!doc["humidifier"].isNull()) {
    bool cmd = doc["humidifier"].as<bool>();
    if (cmd != humidifierState) {
      humidifierState = cmd; humidifierMode_Auto = false;
      digitalWrite(HUMIDIFIER_PIN, humidifierState ? HIGH : LOW);
      Serial.print("Firebase CMD: Humidifier -> "); Serial.println(humidifierState ? "ON" : "OFF");
      stateChanged = true;
    }
  }
  if (!doc["light"].isNull()) {
    bool cmd = doc["light"].as<bool>();
    if (cmd != lightState) {
      lightState = cmd; lightMode_Auto = false;
      digitalWrite(LIGHT_RELAY_PIN, lightState ? HIGH : LOW);
      Serial.print("Firebase CMD: Light -> "); Serial.println(lightState ? "ON" : "OFF");
      stateChanged = true;
    }
  }
  if (stateChanged) { server.handleClient(); pushDeviceStates(); }
}

// ==========================================
//   DHT22  —  TEMPERATURE
// ==========================================
float readDHTTemperature() {
  float t = dht.readTemperature();
  if (!isnan(t) && t > -40.0 && t < 80.0) {
    lastValidTemp    = t;
    lastDHTSuccess   = millis();
    return t;
  }
  Serial.println("DHT22: bad temperature read, using last good value");
  return lastValidTemp;
}

// ==========================================
//   DHT22  —  HUMIDITY
// ==========================================
float readDHTHumidity() {
  float h = dht.readHumidity();
  if (!isnan(h) && h >= 0.0 && h <= 100.0) {
    lastValidHumidity = h;
    return h;
  }
  Serial.println("DHT22: bad humidity read, using last good value");
  return lastValidHumidity;
}

// ==========================================
//   SOIL MOISTURE
// ==========================================
int readSoilMoisture() {
  int sum = 0;
  for (int i = 0; i < SOIL_SAMPLE_SIZE; i++) {
    sum += analogRead(SOIL_MOISTURE_PIN);
    delay(SENSOR_DELAY);
  }
  return sum / SOIL_SAMPLE_SIZE;
}

// ==========================================
//   LIGHT SENSOR  (3-pin digital LDR)
// ==========================================
void readLightSensor() {
  lightRaw = digitalRead(LIGHT_SENSOR_PIN);
  isBright = (lightRaw == LOW);
}

// ==========================================
//   READ ALL SENSORS
// ==========================================
void readSensors() {
  if (millis() - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = millis();

    temperature  = readDHTTemperature();
    humidity     = readDHTHumidity();
    soilMoisture = readSoilMoisture();
    soilPercent  = constrain(map(soilMoisture, SOIL_MOISTURE_MAX, SOIL_MOISTURE_MIN, 0, 100), 0, 100);
    readLightSensor();

    if (millis() - lastDHTSuccess > DHT_TIMEOUT_MS) {
      Serial.println("WARNING: DHT22 not responding — check wiring!");
    }

    Serial.println("\n=== SENSOR DATA ===");
    Serial.print("Temp: ");     Serial.print(temperature, 2); Serial.println(" C");
    Serial.print("Humidity: "); Serial.print(humidity, 1);    Serial.println(" %");
    Serial.print("Soil: ");     Serial.print(soilPercent);    Serial.println(" %");
    Serial.print("Light: ");    Serial.println(isBright ? "Bright" : "Dark");
  }
}

// ==========================================
//   AUTO CONTROL
// ==========================================
void autoControl() {
  if (fanMode_Auto) {
    if (temperature >= TEMP_FAN_ON && !fanState) {
      digitalWrite(FAN_PIN, HIGH); fanState = true;
      Serial.println("FAN ON  - Temp high");
    } else if (temperature <= TEMP_FAN_OFF && fanState) {
      digitalWrite(FAN_PIN, LOW);  fanState = false;
      Serial.println("FAN OFF - Temp normal");
    }
  }

  if (humidifierMode_Auto) {
    if (humidity <= HUMIDITY_ON && !humidifierState) {
      digitalWrite(HUMIDIFIER_PIN, HIGH); humidifierState = true;
      Serial.println("HUMIDIFIER ON  - Humidity low");
      if (!fanState) { digitalWrite(FAN_PIN, HIGH); fanState = true; Serial.println("FAN ON - With humidifier"); }
    } else if (humidity >= HUMIDITY_OFF && humidifierState) {
      digitalWrite(HUMIDIFIER_PIN, LOW);  humidifierState = false;
      Serial.println("HUMIDIFIER OFF - Humidity good");
    }
  }

  if (pumpMode_Auto) {
    if (soilMoisture > SOIL_MOISTURE_DRY && !pumpState) {
      digitalWrite(PUMP_PIN, HIGH); pumpState = true;
      Serial.println("PUMP ON  - Soil dry");
    } else if (soilMoisture <= SOIL_MOISTURE_WET && pumpState) {
      digitalWrite(PUMP_PIN, LOW);  pumpState = false;
      Serial.println("PUMP OFF - Soil wet");
    }
  }
}

// ==========================================
//   HTML PAGE
// ==========================================
String getHTML() {
  String fanStatus        = fanState        ? "ON" : "OFF";
  String pumpStatus       = pumpState       ? "ON" : "OFF";
  String humidifierStatus = humidifierState ? "ON" : "OFF";
  String lightStatus      = lightState      ? "ON" : "OFF";
  String fanModeText        = fanMode_Auto        ? "AUTO" : "MANUAL";
  String pumpModeText       = pumpMode_Auto       ? "AUTO" : "MANUAL";
  String humidifierModeText = humidifierMode_Auto ? "AUTO" : "MANUAL";
  String lightModeText      = lightMode_Auto      ? "AUTO" : "MANUAL";

  bool   dhtOk      = (millis() - lastDHTSuccess < DHT_TIMEOUT_MS);
  String lightLabel = isBright ? "Bright" : "Dark";
  String lightIcon  = isBright ? "&#9728;" : "&#9790;";
  String lightBg    = isBright ? "#FFF9C4" : "#263238";
  String lightTxt   = isBright ? "#F57F17" : "#B0BEC5";

  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<title>Smart Ginger Plant Monitor</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>";
  html += "* { margin:0; padding:0; box-sizing:border-box; }";
  html += "body { font-family:Arial,sans-serif; background:linear-gradient(135deg,#667eea 0%,#764ba2 100%); min-height:100vh; padding:20px; }";
  html += ".container { max-width:1400px; margin:0 auto; }";
  html += ".header { text-align:center; color:white; margin-bottom:30px; }";
  html += ".header h1 { font-size:2.5em; margin-bottom:10px; }";
  html += ".header p  { font-size:1.1em; opacity:0.9; }";
  html += ".grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(250px,1fr)); gap:20px; margin-bottom:30px; }";
  html += ".card { background:white; border-radius:15px; padding:20px; box-shadow:0 8px 32px rgba(0,0,0,0.1); text-align:center; }";
  html += ".card h3 { color:#667eea; font-size:0.9em; margin-bottom:10px; }";
  html += ".card .value { font-size:2.2em; font-weight:bold; color:#667eea; }";
  html += ".card .value.warn { color:#cc0000; }";
  html += ".light-card { border-radius:15px; padding:20px; box-shadow:0 8px 32px rgba(0,0,0,0.15); text-align:center; }";
  html += ".light-card h3 { font-size:0.9em; margin-bottom:10px; }";
  html += ".light-card .light-icon  { font-size:2em; margin-bottom:6px; }";
  html += ".light-card .light-value { font-size:2em; font-weight:bold; }";
  html += ".device-section { display:grid; grid-template-columns:repeat(auto-fit,minmax(280px,1fr)); gap:20px; margin-bottom:30px; }";
  html += ".device-card { background:white; border-radius:15px; padding:20px; box-shadow:0 8px 32px rgba(0,0,0,0.1); }";
  html += ".device-header { display:flex; justify-content:space-between; align-items:center; margin-bottom:15px; padding-bottom:10px; border-bottom:2px solid #f0f0f0; }";
  html += ".device-name { font-size:1.3em; font-weight:bold; color:#333; }";
  html += ".status-badge { padding:5px 15px; border-radius:20px; font-weight:bold; font-size:0.9em; }";
  html += ".status-on  { background:#90EE90; color:#155724; }";
  html += ".status-off { background:#FFB6C6; color:#721c24; }";
  html += ".buttons { display:flex; gap:10px; margin-bottom:15px; }";
  html += ".btn { flex:1; padding:12px 20px; border:none; border-radius:8px; cursor:pointer; font-weight:bold; font-size:1em; transition:all 0.2s; text-align:center; display:block; }";
  html += ".btn-on  { background:#667eea; color:white; }";
  html += ".btn-on:hover { background:#5568d3; transform:scale(1.02); }";
  html += ".btn-off { background:#e8e8e8; color:#333; border:2px solid #ddd; }";
  html += ".switch-container { margin-top:15px; }";
  html += ".switch { position:relative; display:inline-block; width:80px; height:34px; }";
  html += ".switch input { opacity:0; width:0; height:0; }";
  html += ".slider { position:absolute; cursor:pointer; top:0; left:0; right:0; bottom:0; background-color:#ccc; transition:0.4s; border-radius:34px; }";
  html += ".slider:before { position:absolute; content:''; height:26px; width:26px; left:4px; bottom:4px; background-color:white; transition:0.4s; border-radius:50%; }";
  html += "input:checked + .slider { background-color:#17a2b8; }";
  html += "input:checked + .slider:before { transform:translateX(46px); }";
  html += ".mode-text { display:inline-block; margin-left:10px; font-weight:bold; color:#333; }";
  html += ".badge { padding:8px 15px; border-radius:8px; font-size:0.85em; text-align:center; margin-bottom:20px; }";
  html += ".badge-info  { background:#FFF3CD; border:1px solid #ffc107; color:#856404; }";
  html += ".badge-error { background:#FFCCCC; border:1px solid #cc0000; color:#cc0000; }";
  html += ".ip-section { background:white; padding:20px; border-radius:15px; text-align:center; box-shadow:0 8px 32px rgba(0,0,0,0.1); color:#333; }";
  html += ".ip-address { font-size:1.3em; font-weight:bold; color:#667eea; font-family:monospace; margin-top:10px; }";
  html += "@media(max-width:768px){ .header h1{font-size:1.8em;} .grid,.device-section{grid-template-columns:1fr;} }";
  html += "</style>";
  html += "<script>";
  html += "function controlDevice(d,a){var x=new XMLHttpRequest();x.open('POST','/'+d+'/'+a,true);x.send();setTimeout(function(){location.reload();},500);}";
  html += "function toggleMode(d){var x=new XMLHttpRequest();x.open('POST','/'+d+'/mode',true);x.send();setTimeout(function(){location.reload();},500);}";
  html += "setInterval(function(){location.reload();},10000);";
  html += "</script>";
  html += "</head><body><div class='container'>";

  html += "<div class='header'><h1>Smart Ginger Plant Monitor</h1><p>Real-time monitoring and control system</p></div>";

  html += "<div class='badge badge-info'>Firebase Realtime Database: data-monitoring-509a1 | Syncing every 30s</div>";

  if (!dhtOk) {
    html += "<div class='badge badge-error'>&#9888; DHT22 sensor not responding — check wiring on GPIO 4!</div>";
  }

  html += "<div class='grid'>";

  html += "<div class='card'><h3>Temperature</h3>";
  html += "<div class='value" + String(!dhtOk ? " warn" : "") + "'>";
  html += String(temperature, 2) + "&#176;C";
  if (!dhtOk) html += "<div style='font-size:0.4em;margin-top:4px;'>last known value</div>";
  html += "</div></div>";

  html += "<div class='card'><h3>Humidity</h3>";
  html += "<div class='value" + String(!dhtOk ? " warn" : "") + "'>";
  html += String((int)humidity) + "%";
  if (!dhtOk) html += "<div style='font-size:0.4em;margin-top:4px;'>last known value</div>";
  html += "</div></div>";

  html += "<div class='card'><h3>Soil Moisture</h3><div class='value'>" + String(soilPercent) + "%</div></div>";

  html += "<div class='light-card' style='background:" + lightBg + ";'>";
  html += "<h3 style='color:" + lightTxt + ";'>Light Level</h3>";
  html += "<div class='light-icon'  style='color:" + lightTxt + ";'>" + lightIcon + "</div>";
  html += "<div class='light-value' style='color:" + lightTxt + ";'>" + lightLabel + "</div>";
  html += "</div>";

  html += "</div>";

  html += "<div class='device-section'>";

  auto deviceCard = [&](String name, String key, bool state, bool autoMode, String statusText, String modeText) -> String {
    String c = "<div class='device-card'>";
    c += "<div class='device-header'><div class='device-name'>" + name + "</div>";
    c += "<div class='status-badge status-" + String(state ? "on" : "off") + "'>" + statusText + "</div></div>";
    c += "<div class='buttons'>";
    c += "<button class='btn btn-on'  onclick='controlDevice(\"" + key + "\",\"on\")'>ON</button>";
    c += "<button class='btn btn-off' onclick='controlDevice(\"" + key + "\",\"off\")'>OFF</button>";
    c += "</div><div class='switch-container'>";
    c += "<label class='switch'><input type='checkbox' " + String(autoMode ? "checked" : "") + " onclick='toggleMode(\"" + key + "\")'><span class='slider'></span></label>";
    c += "<span class='mode-text'>" + modeText + "</span></div></div>";
    return c;
  };

  html += deviceCard("FAN",        "fan",        fanState,        fanMode_Auto,        fanStatus,        fanModeText);
  html += deviceCard("PUMP",       "pump",       pumpState,       pumpMode_Auto,       pumpStatus,       pumpModeText);
  html += deviceCard("HUMIDIFIER", "humidifier", humidifierState, humidifierMode_Auto, humidifierStatus, humidifierModeText);
  html += deviceCard("LIGHT",      "light",      lightState,      lightMode_Auto,      lightStatus,      lightModeText);

  html += "</div>";
  html += "<div class='ip-section'><strong>Device IP Address:</strong><div class='ip-address'>http://" + WiFi.localIP().toString() + "</div></div>";
  html += "</div></body></html>";
  return html;
}

// ==========================================
//   ROUTE HANDLERS
// ==========================================
void handleRoot() { server.send(200, "text/html", getHTML()); }

void handleFanOn()   { digitalWrite(FAN_PIN,HIGH);  fanState=true;  fanMode_Auto=false; server.handleClient(); pushDeviceStates(); server.send(200,"text/plain","OK"); }
void handleFanOff()  { digitalWrite(FAN_PIN,LOW);   fanState=false; fanMode_Auto=false; server.handleClient(); pushDeviceStates(); server.send(200,"text/plain","OK"); }
void handleFanMode() { fanMode_Auto=!fanMode_Auto;  server.handleClient(); pushDeviceStates(); server.send(200,"text/plain","OK"); }

void handlePumpOn()   { digitalWrite(PUMP_PIN,HIGH);  pumpState=true;  pumpMode_Auto=false; server.handleClient(); pushDeviceStates(); server.send(200,"text/plain","OK"); }
void handlePumpOff()  { digitalWrite(PUMP_PIN,LOW);   pumpState=false; pumpMode_Auto=false; server.handleClient(); pushDeviceStates(); server.send(200,"text/plain","OK"); }
void handlePumpMode() { pumpMode_Auto=!pumpMode_Auto; server.handleClient(); pushDeviceStates(); server.send(200,"text/plain","OK"); }

void handleHumidifierOn()   { digitalWrite(HUMIDIFIER_PIN,HIGH);  humidifierState=true;  humidifierMode_Auto=false; server.handleClient(); pushDeviceStates(); server.send(200,"text/plain","OK"); }
void handleHumidifierOff()  { digitalWrite(HUMIDIFIER_PIN,LOW);   humidifierState=false; humidifierMode_Auto=false; server.handleClient(); pushDeviceStates(); server.send(200,"text/plain","OK"); }
void handleHumidifierMode() { humidifierMode_Auto=!humidifierMode_Auto; server.handleClient(); pushDeviceStates(); server.send(200,"text/plain","OK"); }

void handleLightOn()   { digitalWrite(LIGHT_RELAY_PIN,HIGH);  lightState=true;  lightMode_Auto=false; server.handleClient(); pushDeviceStates(); server.send(200,"text/plain","OK"); }
void handleLightOff()  { digitalWrite(LIGHT_RELAY_PIN,LOW);   lightState=false; lightMode_Auto=false; server.handleClient(); pushDeviceStates(); server.send(200,"text/plain","OK"); }
void handleLightMode() { lightMode_Auto=!lightMode_Auto; server.handleClient(); pushDeviceStates(); server.send(200,"text/plain","OK"); }

void handleNotFound() {
  server.send(404,"text/html","<html><body style='text-align:center;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);color:white;padding:60px;min-height:100vh;display:flex;flex-direction:column;justify-content:center;'><h1>404</h1><p>Page not found</p><a href='/' style='color:white;text-decoration:underline;'>Go Back</a></body></html>");
}

// ==========================================
//   SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // ---- I2C LCD Init ----
  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Ginger Monitor");
  lcd.setCursor(0, 1);
  lcd.print("Starting up...");

  pinMode(LED_PIN,          OUTPUT);
  pinMode(FAN_PIN,          OUTPUT);
  pinMode(PUMP_PIN,         OUTPUT);
  pinMode(HUMIDIFIER_PIN,   OUTPUT);
  pinMode(LIGHT_RELAY_PIN,  OUTPUT);
  pinMode(LIGHT_SENSOR_PIN, INPUT);

  digitalWrite(LED_PIN,         LOW);
  digitalWrite(FAN_PIN,         LOW);
  digitalWrite(PUMP_PIN,        LOW);
  digitalWrite(HUMIDIFIER_PIN,  LOW);
  digitalWrite(LIGHT_RELAY_PIN, LOW);

  dht.begin();

  Serial.println("\n================================");
  Serial.println("SMART GINGER PLANT MONITOR");
  Serial.println("================================");
  Serial.println("Waiting 2s for DHT22 warm-up...");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("DHT22 warm-up..");
  delay(2000);

  // Startup read — seed the last-good cache
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t) && t > -40.0 && t < 80.0) { lastValidTemp     = t; temperature = t; }
  if (!isnan(h) && h >= 0.0  && h <= 100.0) { lastValidHumidity = h; humidity    = h; }
  lastDHTSuccess = millis();

  Serial.print("DHT22 startup  Temp: "); Serial.print(t);   Serial.print(" C");
  Serial.print("   Humidity: ");          Serial.print(h);   Serial.println(" %");

  // Show DHT22 result on LCD
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("T:");
  lcd.print(String(t, 1));
  lcd.print((char)223);
  lcd.print("C H:");
  lcd.print(String((int)h));
  lcd.print("%");
  lcd.setCursor(0, 1);
  lcd.print("Connecting WiFi.");

  Serial.print("WiFi: "); Serial.println(ssid);
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500); Serial.print("."); attempts++;
    lcd.setCursor(attempts % 16, 1);
    lcd.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi Connected!");
    Serial.print("IP: http://"); Serial.println(WiFi.localIP());
    Serial.println("================================");

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Connected!");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
    delay(2000);

    digitalWrite(LED_PIN, HIGH); delay(200);
    digitalWrite(LED_PIN, LOW);  delay(200);
    digitalWrite(LED_PIN, HIGH); delay(200);
    digitalWrite(LED_PIN, LOW);

    Serial.println("Pushing initial data to Firebase...");
    pushSensorData();
    pushDeviceStates();
  } else {
    Serial.println("WiFi Failed!");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi FAILED!");
    lcd.setCursor(0, 1);
    lcd.print("Check credentials");
    delay(2000);
  }

  server.on("/", HTTP_GET, handleRoot);

  server.on("/fan/on",   HTTP_POST, handleFanOn);
  server.on("/fan/off",  HTTP_POST, handleFanOff);
  server.on("/fan/mode", HTTP_POST, handleFanMode);

  server.on("/pump/on",   HTTP_POST, handlePumpOn);
  server.on("/pump/off",  HTTP_POST, handlePumpOff);
  server.on("/pump/mode", HTTP_POST, handlePumpMode);

  server.on("/humidifier/on",   HTTP_POST, handleHumidifierOn);
  server.on("/humidifier/off",  HTTP_POST, handleHumidifierOff);
  server.on("/humidifier/mode", HTTP_POST, handleHumidifierMode);

  server.on("/light/on",   HTTP_POST, handleLightOn);
  server.on("/light/off",  HTTP_POST, handleLightOff);
  server.on("/light/mode", HTTP_POST, handleLightMode);

  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("Web server started!");
  Serial.println("Firebase sync: every 30s sensors, every 5s commands");
  Serial.println("LCD: rotating 4 screens every 3s");
  Serial.println("================================\n");

  // First LCD update
  updateLCD();
  lastLCDUpdate = millis();
}

// ==========================================
//   LOOP
// ==========================================
void loop() {
  server.handleClient();
  server.handleClient();
  server.handleClient();

  readSensors();
  autoControl();

  unsigned long now = millis();

  // LCD rotation
  if (now - lastLCDUpdate >= LCD_SCREEN_INTERVAL) {
    lastLCDUpdate = now;
    updateLCD();
  }

  if (now - lastFirebasePush >= FIREBASE_SENSOR_INTERVAL) {
    lastFirebasePush = now;
    server.handleClient();
    pushSensorData();
    server.handleClient();
    pushDeviceStates();
    server.handleClient();
  }

  if (now - lastCommandCheck >= FIREBASE_COMMAND_INTERVAL) {
    lastCommandCheck = now;
    server.handleClient();
    checkFirebaseCommands();
    server.handleClient();
  }

  static unsigned long lastReconnectAttempt = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastReconnectAttempt >= 5000) {
      lastReconnectAttempt = now;
      Serial.println("WiFi reconnecting...");
      WiFi.reconnect();
    }
  }
}
