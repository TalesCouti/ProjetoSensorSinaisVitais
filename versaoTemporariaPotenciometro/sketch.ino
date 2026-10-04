#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define DEVICE_ID "esp32-vitals-sim"

#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

#define OLED_ADDRESS 0x3C

// Potenciômetros
#define BPM_POT_PIN 34
#define SPO2_POT_PIN 35
#define TEMP_POT_PIN 32

static constexpr uint16_t SCREEN_WIDTH = 128;
static constexpr uint16_t SCREEN_HEIGHT = 64;
static constexpr int8_t OLED_RESET_PIN = -1;

static constexpr uint32_t DISPLAY_INTERVAL_MS = 500;
static constexpr uint32_t SENSOR_INTERVAL_MS = 200;

WebServer server(80);

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET_PIN);

struct VitalData {
  bool max30102Ok = true;
  bool max30205Ok = true;
  bool oledOk = false;

  bool fingerDetected = true;

  float heartRateBpm = NAN;
  float spo2Pct = NAN;
  float bodyTempC = NAN;

  uint32_t max30102Samples = 0;
  uint32_t lastBeatMs = 0;
  uint32_t updatedAtMs = 0;

  int lastHttpCode = 0;
};

VitalData vitals;

uint32_t lastSensorReadMs = 0;
uint32_t lastDisplayMs = 0;

// ---------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------

String jsonFloat(float value, uint8_t decimals) {
  if (!isfinite(value)) {
    return "null";
  }
  // Cast explícito para evitar ambiguidade de overload no core ESP32 3.x
  return String((double)value, (unsigned int)decimals);
}

String jsonString(const String &value) {
  String escaped = value;
  escaped.replace("\\", "\\\\");
  escaped.replace("\"", "\\\"");
  return "\"" + escaped + "\"";
}

String ipAddressString() {
  return WiFi.softAPIP().toString();
}

String wifiModeString() {
  return "access_point";
}

// ---------------------------------------------------------------
// Sensores simulados (potenciômetros)
// ---------------------------------------------------------------

float readSimulatedBPM() {
  int raw = analogRead(BPM_POT_PIN);
  // BPM: 40 - 180
  return (float)map(raw, 0, 4095, 40, 180);
}

float readSimulatedSpO2() {
  int raw = analogRead(SPO2_POT_PIN);
  // SpO2: 70 - 100%
  return 70.0f + (raw / 4095.0f) * 30.0f;
}

float readSimulatedTemperature() {
  int raw = analogRead(TEMP_POT_PIN);
  // Temperatura: 35 - 40 C
  return 35.0f + (raw / 4095.0f) * 5.0f;
}

void readSimulatedSensors() {
  uint32_t now = millis();

  if (now - lastSensorReadMs < SENSOR_INTERVAL_MS) {
    return;
  }

  lastSensorReadMs = now;

  vitals.heartRateBpm = readSimulatedBPM();
  vitals.spo2Pct = readSimulatedSpO2();
  vitals.bodyTempC = readSimulatedTemperature();

  vitals.fingerDetected = true;
  vitals.max30102Samples++;
  vitals.updatedAtMs = now;
  vitals.lastBeatMs = now;
}

// ---------------------------------------------------------------
// JSON
// ---------------------------------------------------------------

String buildVitalsJson() {
  uint32_t lastBeatAgeMs =
    vitals.lastBeatMs == 0 ? 0 : millis() - vitals.lastBeatMs;

  String payload = "{";

  payload += "\"deviceId\":" + jsonString(DEVICE_ID) + ",";
  payload += "\"uptimeMs\":" + String(millis()) + ",";
  payload += "\"ip\":" + jsonString(ipAddressString()) + ",";
  payload += "\"wifiMode\":" + jsonString(wifiModeString()) + ",";

  payload += "\"measurements\":{";
  payload += "\"heartRateBpm\":" + jsonFloat(vitals.heartRateBpm, 1) + ",";
  payload += "\"spo2Pct\":" + jsonFloat(vitals.spo2Pct, 1) + ",";
  payload += "\"bodyTempC\":" + jsonFloat(vitals.bodyTempC, 2) + ",";
  payload += "\"fingerDetected\":" +
             String(vitals.fingerDetected ? "true" : "false");
  payload += "},";

  payload += "\"sensors\":{";
  payload += "\"max30102\":" + String(vitals.max30102Ok ? "true" : "false") + ",";
  payload += "\"max30205\":" + String(vitals.max30205Ok ? "true" : "false") + ",";
  payload += "\"oled\":" + String(vitals.oledOk ? "true" : "false");
  payload += "},";

  payload += "\"quality\":{";
  payload += "\"max30102Samples\":" + String(vitals.max30102Samples) + ",";
  payload += "\"lastBeatAgeMs\":" + String(lastBeatAgeMs) + ",";
  payload += "\"spo2WindowSamples\":0";
  payload += "},";

  payload += "\"push\":{";
  payload += "\"enabled\":false,";
  payload += "\"lastHttpCode\":" + String(vitals.lastHttpCode);
  payload += "}";

  payload += "}";

  return payload;
}

// ---------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type,Authorization");
  server.sendHeader("Cache-Control", "no-store");
}

void handleRoot() {
  addCorsHeaders();

  String body = "ESP32 Vital Signs Simulator\n\n";
  body += "GET /api/v1/health\n";
  body += "GET /api/v1/vitals\n";

  server.send(200, "text/plain; charset=utf-8", body);
}

void handleHealth() {
  addCorsHeaders();

  String payload = "{";
  payload += "\"ok\":true,";
  payload += "\"deviceId\":" + jsonString(DEVICE_ID) + ",";
  payload += "\"uptimeMs\":" + String(millis()) + ",";
  payload += "\"ip\":" + jsonString(ipAddressString()) + ",";
  payload += "\"wifiMode\":" + jsonString(wifiModeString());
  payload += "}";

  server.send(200, "application/json", payload);
}

void handleVitals() {
  addCorsHeaders();
  server.send(200, "application/json", buildVitalsJson());
}

void handleOptions() {
  addCorsHeaders();
  server.send(204);
}

void handleNotFound() {
  addCorsHeaders();
  server.send(404, "application/json", "{\"error\":\"not_found\"}");
}

void setupHttpServer() {
  server.on("/", HTTP_GET, handleRoot);

  server.on("/api/v1/health", HTTP_GET, handleHealth);
  server.on("/api/v1/health", HTTP_OPTIONS, handleOptions);

  server.on("/api/v1/vitals", HTTP_GET, handleVitals);
  server.on("/api/v1/vitals", HTTP_OPTIONS, handleOptions);

  server.onNotFound(handleNotFound);

  server.begin();

  Serial.println("Servidor HTTP iniciado.");
}

// ---------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------

void setupWifi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("VitalSensor-ESP32", "12345678");

  Serial.print("Access Point ativo. IP: ");
  Serial.println(WiFi.softAPIP());
}

// ---------------------------------------------------------------
// Display OLED
// ---------------------------------------------------------------

void setupDisplay() {
  vitals.oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);

  if (!vitals.oledOk) {
    Serial.println("OLED nao encontrado.");
    return;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  display.println("SENSOR VITAL");
  display.println();
  display.println("Simulacao Wokwi");
  display.println();
  display.println("Inicializando...");

  display.display();
}

void updateDisplay() {
  if (!vitals.oledOk) {
    return;
  }

  uint32_t now = millis();

  if (now - lastDisplayMs < DISPLAY_INTERVAL_MS) {
    return;
  }

  lastDisplayMs = now;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  display.println("SINAIS VITAIS");

  display.print("BPM: ");
  display.println(jsonFloat(vitals.heartRateBpm, 1));

  display.print("SpO2: ");
  display.print(jsonFloat(vitals.spo2Pct, 1));
  display.println("%");

  display.print("Temp: ");
  display.print(jsonFloat(vitals.bodyTempC, 2));
  display.println(" C");

  display.print("Dedo: ");
  display.println(vitals.fingerDetected ? "SIM" : "NAO");

  display.display();
}

// ---------------------------------------------------------------
// Setup / Loop
// ---------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("================================");
  Serial.println("ESP32 - SIMULADOR DE SINAIS VITAIS");
  Serial.println("================================");

  // ADC
  analogReadResolution(12);

  // I2C
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);

  setupDisplay();
  setupWifi();
  setupHttpServer();

  Serial.println();
  Serial.println("Simulacao pronta!");

  Serial.print("API: http://");
  Serial.print(ipAddressString());
  Serial.println("/api/v1/vitals");
}

void loop() {
  server.handleClient();
  readSimulatedSensors();
  updateDisplay();
  delay(5);
}
