#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <MAX30105.h>
#include <heartRate.h>
#include <math.h>

#if __has_include("config.h")
#include "config.h"
#else
#define DEVICE_ID "esp32-vitals-01"
#define WIFI_SSID ""
#define WIFI_PASS ""
#define API_PUSH_URL ""
#define API_TOKEN ""
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define MAX30205_ADDRESS 0x48
#define OLED_ADDRESS 0x3C
#define FINGER_IR_THRESHOLD 20000
#define API_PUSH_INTERVAL_MS 10000
#endif

static constexpr uint32_t I2C_FREQUENCY = 400000;
static constexpr uint16_t SCREEN_WIDTH = 128;
static constexpr uint16_t SCREEN_HEIGHT = 64;
static constexpr int8_t OLED_RESET_PIN = -1;
static constexpr uint32_t MAX30102_SAMPLE_INTERVAL_MS = 40;  // 25 Hz
static constexpr uint16_t SPO2_WINDOW_SAMPLES = 100;

WebServer server(80);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET_PIN);
MAX30105 particleSensor;

struct VitalData {
  bool max30102Ok = false;
  bool max30205Ok = false;
  bool oledOk = false;
  bool fingerDetected = false;
  float heartRateBpm = NAN;
  float spo2Pct = NAN;
  float bodyTempC = NAN;
  uint32_t max30102Samples = 0;
  uint32_t lastBeatMs = 0;
  uint32_t updatedAtMs = 0;
  int lastHttpCode = 0;
};

VitalData vitals;

float bpmBuffer[8];
uint8_t bpmBufferIndex = 0;
uint8_t bpmBufferCount = 0;

double redSum = 0.0;
double irSum = 0.0;
double redSqSum = 0.0;
double irSqSum = 0.0;
uint16_t spo2SampleCount = 0;

uint32_t lastMax30102ReadMs = 0;
uint32_t lastTempReadMs = 0;
uint32_t lastDisplayMs = 0;
uint32_t lastPushMs = 0;

String jsonFloat(float value, uint8_t decimals) {
  if (!isfinite(value)) {
    return "null";
  }
  return String(value, decimals);
}

double clampVariance(double value) {
  return value < 0.0 ? 0.0 : value;
}

String jsonString(const String &value) {
  String escaped = value;
  escaped.replace("\\", "\\\\");
  escaped.replace("\"", "\\\"");
  return "\"" + escaped + "\"";
}

String ipAddressString() {
  if (WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  return WiFi.softAPIP().toString();
}

String wifiModeString() {
  if (WiFi.status() == WL_CONNECTED) {
    return "station";
  }
  return "access_point";
}

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type,Authorization");
  server.sendHeader("Cache-Control", "no-store");
}

float readMax30205TemperatureC() {
  Wire.beginTransmission(MAX30205_ADDRESS);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) {
    return NAN;
  }

  if (Wire.requestFrom((uint8_t)MAX30205_ADDRESS, (uint8_t)2) != 2) {
    return NAN;
  }

  int16_t raw = (int16_t)((Wire.read() << 8) | Wire.read());
  return raw / 256.0f;
}

void updateHeartRate(long irValue) {
  if (!checkForBeat(irValue)) {
    return;
  }

  uint32_t now = millis();
  if (vitals.lastBeatMs == 0) {
    vitals.lastBeatMs = now;
    return;
  }

  float deltaSec = (now - vitals.lastBeatMs) / 1000.0f;
  vitals.lastBeatMs = now;

  if (deltaSec <= 0.0f) {
    return;
  }

  float bpm = 60.0f / deltaSec;
  if (bpm < 40.0f || bpm > 180.0f) {
    return;
  }

  bpmBuffer[bpmBufferIndex] = bpm;
  bpmBufferIndex = (bpmBufferIndex + 1) % 8;
  if (bpmBufferCount < 8) {
    bpmBufferCount++;
  }

  float total = 0.0f;
  for (uint8_t i = 0; i < bpmBufferCount; i++) {
    total += bpmBuffer[i];
  }
  vitals.heartRateBpm = total / bpmBufferCount;
}

void updateSpo2Estimate(long redValue, long irValue) {
  redSum += redValue;
  irSum += irValue;
  redSqSum += (double)redValue * redValue;
  irSqSum += (double)irValue * irValue;
  spo2SampleCount++;

  if (spo2SampleCount < SPO2_WINDOW_SAMPLES) {
    return;
  }

  double redDc = redSum / spo2SampleCount;
  double irDc = irSum / spo2SampleCount;
  double redAc = sqrt(clampVariance((redSqSum / spo2SampleCount) - (redDc * redDc)));
  double irAc = sqrt(clampVariance((irSqSum / spo2SampleCount) - (irDc * irDc)));

  if (redDc > 0.0 && irDc > 0.0 && redAc > 0.0 && irAc > 0.0) {
    double ratio = (redAc / redDc) / (irAc / irDc);
    float spo2 = 110.0f - (25.0f * ratio);
    vitals.spo2Pct = constrain(spo2, 70.0f, 100.0f);
  }

  redSum = 0.0;
  irSum = 0.0;
  redSqSum = 0.0;
  irSqSum = 0.0;
  spo2SampleCount = 0;
}

void readMax30102() {
  if (!vitals.max30102Ok) {
    return;
  }

  uint32_t now = millis();
  if (now - lastMax30102ReadMs < MAX30102_SAMPLE_INTERVAL_MS) {
    return;
  }
  lastMax30102ReadMs = now;

  long irValue = particleSensor.getIR();
  long redValue = particleSensor.getRed();

  vitals.fingerDetected = irValue > FINGER_IR_THRESHOLD;
  vitals.max30102Samples++;
  vitals.updatedAtMs = now;

  if (!vitals.fingerDetected) {
    vitals.heartRateBpm = NAN;
    vitals.spo2Pct = NAN;
    vitals.lastBeatMs = 0;
    bpmBufferIndex = 0;
    bpmBufferCount = 0;
    redSum = 0.0;
    irSum = 0.0;
    redSqSum = 0.0;
    irSqSum = 0.0;
    spo2SampleCount = 0;
    return;
  }

  updateHeartRate(irValue);
  updateSpo2Estimate(redValue, irValue);
}

void readTemperature() {
  uint32_t now = millis();
  if (now - lastTempReadMs < 1000) {
    return;
  }
  lastTempReadMs = now;

  float temperature = readMax30205TemperatureC();
  vitals.max30205Ok = isfinite(temperature);
  if (vitals.max30205Ok) {
    vitals.bodyTempC = temperature;
    vitals.updatedAtMs = now;
  }
}

String buildVitalsJson() {
  uint32_t lastBeatAgeMs = vitals.lastBeatMs == 0 ? 0 : millis() - vitals.lastBeatMs;

  String payload = "{";
  payload += "\"deviceId\":" + jsonString(DEVICE_ID) + ",";
  payload += "\"uptimeMs\":" + String(millis()) + ",";
  payload += "\"ip\":" + jsonString(ipAddressString()) + ",";
  payload += "\"wifiMode\":" + jsonString(wifiModeString()) + ",";
  payload += "\"measurements\":{";
  payload += "\"heartRateBpm\":" + jsonFloat(vitals.heartRateBpm, 1) + ",";
  payload += "\"spo2Pct\":" + jsonFloat(vitals.spo2Pct, 1) + ",";
  payload += "\"bodyTempC\":" + jsonFloat(vitals.bodyTempC, 2) + ",";
  payload += "\"fingerDetected\":" + String(vitals.fingerDetected ? "true" : "false");
  payload += "},";
  payload += "\"sensors\":{";
  payload += "\"max30102\":" + String(vitals.max30102Ok ? "true" : "false") + ",";
  payload += "\"max30205\":" + String(vitals.max30205Ok ? "true" : "false") + ",";
  payload += "\"oled\":" + String(vitals.oledOk ? "true" : "false");
  payload += "},";
  payload += "\"quality\":{";
  payload += "\"max30102Samples\":" + String(vitals.max30102Samples) + ",";
  payload += "\"lastBeatAgeMs\":" + String(lastBeatAgeMs) + ",";
  payload += "\"spo2WindowSamples\":" + String(spo2SampleCount);
  payload += "},";
  payload += "\"push\":{";
  payload += "\"enabled\":" + String(strlen(API_PUSH_URL) > 0 ? "true" : "false") + ",";
  payload += "\"lastHttpCode\":" + String(vitals.lastHttpCode);
  payload += "}";
  payload += "}";
  return payload;
}

void handleRoot() {
  addCorsHeaders();
  String body = "ESP32 Vital Signs Sensor\n\n";
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
}

void setupWifi() {
  bool hasCredentials = strlen(WIFI_SSID) > 0;

  if (hasCredentials) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    Serial.print("Conectando ao WiFi");
    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < 15000) {
      Serial.print(".");
      delay(500);
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("WiFi conectado. IP: ");
      Serial.println(WiFi.localIP());
      return;
    }

    Serial.println("Falha no WiFi. Iniciando Access Point.");
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP("VitalSensor-ESP32", "12345678");
  Serial.print("Access Point ativo. IP: ");
  Serial.println(WiFi.softAPIP());
}

void setupMax30102() {
  vitals.max30102Ok = particleSensor.begin(Wire, I2C_SPEED_FAST);
  if (!vitals.max30102Ok) {
    Serial.println("MAX30102 nao encontrado.");
    return;
  }

  byte ledBrightness = 0x24;
  byte sampleAverage = 4;
  byte ledMode = 2;       // RED + IR
  int sampleRate = 100;
  int pulseWidth = 411;
  int adcRange = 4096;

  particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);
  particleSensor.setPulseAmplitudeRed(0x24);
  particleSensor.setPulseAmplitudeIR(0x24);
  particleSensor.setPulseAmplitudeGreen(0);

  Serial.println("MAX30102 pronto.");
}

void setupDisplay() {
  vitals.oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (!vitals.oledOk) {
    Serial.println("OLED SSD1306 nao encontrado.");
    return;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Sensor Sinais Vitais");
  display.println("Inicializando...");
  display.display();
}

void updateDisplay() {
  if (!vitals.oledOk) {
    return;
  }

  uint32_t now = millis();
  if (now - lastDisplayMs < 1000) {
    return;
  }
  lastDisplayMs = now;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Sensor Sinais Vitais");
  display.print("IP: ");
  display.println(ipAddressString());
  display.print("Dedo: ");
  display.println(vitals.fingerDetected ? "sim" : "nao");
  display.print("BPM: ");
  display.println(jsonFloat(vitals.heartRateBpm, 1));
  display.print("SpO2: ");
  display.print(jsonFloat(vitals.spo2Pct, 1));
  display.println("%");
  display.print("Temp: ");
  display.print(jsonFloat(vitals.bodyTempC, 2));
  display.println(" C");
  display.display();
}

void pushToRemoteApi() {
  if (strlen(API_PUSH_URL) == 0) {
    return;
  }

  uint32_t now = millis();
  if (now - lastPushMs < API_PUSH_INTERVAL_MS) {
    return;
  }
  lastPushMs = now;

  if (WiFi.status() != WL_CONNECTED) {
    vitals.lastHttpCode = -1;
    return;
  }

  HTTPClient http;
  http.begin(API_PUSH_URL);
  http.addHeader("Content-Type", "application/json");
  if (strlen(API_TOKEN) > 0) {
    http.addHeader("Authorization", String("Bearer ") + API_TOKEN);
  }

  vitals.lastHttpCode = http.POST(buildVitalsJson());
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("Inicializando sensor de sinais vitais...");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(I2C_FREQUENCY);

  setupDisplay();
  setupMax30102();
  readTemperature();

  setupWifi();
  setupHttpServer();

  Serial.println("API local pronta:");
  Serial.print("http://");
  Serial.print(ipAddressString());
  Serial.println("/api/v1/vitals");
}

void loop() {
  server.handleClient();
  readMax30102();
  readTemperature();
  updateDisplay();
  pushToRemoteApi();
}
