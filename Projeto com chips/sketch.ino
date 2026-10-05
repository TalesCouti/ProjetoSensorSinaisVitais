// =============================================================================
// FIRMWARE DO ESP32 VITALSENSE (WOKWI) CONECTADO AO BACKEND NO RENDER
// =============================================================================
// Baseado no src/main.cpp do repositório ProjetoSensorSinaisVitais, com:
//   - correção do String(float, decimals) (erro de compilação no ESP32 3.x);
//   - leitura da FIFO do MAX30102 amostra por amostra (BPM passa a funcionar);
//   - envio das leituras para o backend (POST /api/v1/device/readings);
//   - botão "Iniciar" do app: o ESP32 pergunta ao backend se há medição
//     pendente (GET /api/v1/device/commands/next) e mede durante N segundos.
//
// ANTES DE RODAR: preencha API_BASE_URL e API_TOKEN logo abaixo.
// Bibliotecas no Wokwi (libraries.txt):
//   Adafruit GFX Library
//   Adafruit SSD1306
//   SparkFun MAX3010x Pulse and Proximity Sensor Library
//   ArduinoJson
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <MAX30105.h>
#include <heartRate.h>
#include <math.h>

#if __has_include("config.h")
#include "config.h"
#else
#define DEVICE_ID "esp32-vitals-01"
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASS ""

// [PREENCHER] Endereço do backend no Render, terminando em /api/v1
// Exemplo: "https://vitalsense-api.onrender.com/api/v1"
#define API_BASE_URL ""

// [PREENCHER] Token gerado ao cadastrar o ESP32 no backend (POST /api/v1/devices).
// É o "apiToken" da resposta. Ele aparece uma única vez.
#define API_TOKEN ""

#define FIRMWARE_VERSION "1.1.0-wokwi"
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define MAX30205_ADDRESS 0x48
#define OLED_ADDRESS 0x3C
#define FINGER_IR_THRESHOLD 20000

// Envio automático fora das medições (também mostra o ESP32 como "conectado").
#define API_PUSH_INTERVAL_MS 60000
// Envio durante uma medição iniciada pelo app.
#define MEASUREMENT_PUSH_INTERVAL_MS 2000
#endif

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "1.1.0"
#endif
#ifndef MEASUREMENT_PUSH_INTERVAL_MS
#define MEASUREMENT_PUSH_INTERVAL_MS 2000
#endif

static constexpr uint32_t I2C_FREQUENCY = 400000;
static constexpr uint16_t SCREEN_WIDTH = 128;
static constexpr uint16_t SCREEN_HEIGHT = 64;
static constexpr int8_t OLED_RESET_PIN = -1;
static constexpr uint32_t MAX30102_SAMPLE_INTERVAL_MS = 40;  // de quanto em quanto tempo lemos a FIFO
static constexpr uint32_t MAX30102_SAMPLE_PERIOD_MS = 40;    // 100 Hz com média de 4 = 25 amostras/s
static constexpr uint16_t SPO2_WINDOW_SAMPLES = 100;
static constexpr uint32_t HTTP_TIMEOUT_MS = 15000;           // o Render gratuito pode demorar ao "acordar"

WebServer server(80);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET_PIN);
MAX30105 particleSensor;
WiFiClientSecure secureClient;
WiFiClient plainClient;

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

// Medição iniciada pelo botão "Iniciar" do app
struct MeasurementState {
  bool active = false;
  String sessionId;
  uint32_t endMs = 0;
};

MeasurementState measurement;

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
uint32_t lastMeasurementPushMs = 0;
uint32_t lastCommandPollMs = 0;
uint32_t commandPollIntervalMs = 3000;

String jsonFloat(float value, uint8_t decimals) {
  if (!isfinite(value)) {
    return "null";
  }
  return String(value, (unsigned int)decimals);
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

bool apiConfigured() {
  return strlen(API_BASE_URL) > 0 && strlen(API_TOKEN) > 0;
}

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type,Authorization");
  server.sendHeader("Cache-Control", "no-store");
}

// -----------------------------------------------------------------------------
// Sensores
// -----------------------------------------------------------------------------

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

void updateHeartRate(long irValue, uint32_t sampleMs) {
  if (!checkForBeat(irValue)) {
    return;
  }

  if (vitals.lastBeatMs == 0) {
    vitals.lastBeatMs = sampleMs;
    return;
  }

  float deltaSec = (sampleMs - vitals.lastBeatMs) / 1000.0f;
  vitals.lastBeatMs = sampleMs;

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

void resetMax30102Estimates() {
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
}

void processMax30102Sample(long redValue, long irValue, uint32_t sampleMs) {
  vitals.fingerDetected = irValue > FINGER_IR_THRESHOLD;
  vitals.max30102Samples++;
  vitals.updatedAtMs = sampleMs;

  if (!vitals.fingerDetected) {
    resetMax30102Estimates();
    return;
  }

  updateHeartRate(irValue, sampleMs);
  updateSpo2Estimate(redValue, irValue);
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

  // Lê todas as amostras novas da FIFO, com vermelho e IR da mesma amostra.
  // Depois de um envio HTTP podem chegar várias de uma vez: cada uma recebe o
  // horário estimado em que foi medida, para o cálculo do BPM não distorcer.
  particleSensor.check();
  int pending = particleSensor.available();
  while (particleSensor.available()) {
    pending--;
    long redValue = particleSensor.getFIFORed();
    long irValue = particleSensor.getFIFOIR();
    particleSensor.nextSample();
    processMax30102Sample(redValue, irValue, now - (uint32_t)pending * MAX30102_SAMPLE_PERIOD_MS);
  }
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

// -----------------------------------------------------------------------------
// JSON das medições (mesmo formato do /api/v1/vitals local + sessionId)
// -----------------------------------------------------------------------------

// sessionId vazio = leitura fora de uma medição do app.
// (Sem valor padrão no parâmetro: o Arduino gera protótipos automáticos e
// repetiria o valor padrão, causando erro de compilação.)
String buildVitalsJson(const String &sessionId) {
  uint32_t lastBeatAgeMs = vitals.lastBeatMs == 0 ? 0 : millis() - vitals.lastBeatMs;

  String payload = "{";
  payload += "\"deviceId\":" + jsonString(DEVICE_ID) + ",";
  payload += "\"firmwareVersion\":" + jsonString(FIRMWARE_VERSION) + ",";
  if (sessionId.length() > 0) {
    payload += "\"sessionId\":" + jsonString(sessionId) + ",";
  }
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
  payload += "\"oled\":" + String(vitals.oledOk ? "true" : "false") + ",";
  payload += "\"mpu6050\":false";
  payload += "},";
  payload += "\"quality\":{";
  payload += "\"max30102Samples\":" + String(vitals.max30102Samples) + ",";
  payload += "\"lastBeatAgeMs\":" + String(lastBeatAgeMs) + ",";
  payload += "\"spo2WindowSamples\":" + String(spo2SampleCount);
  payload += "},";
  payload += "\"push\":{";
  payload += "\"enabled\":" + String(apiConfigured() ? "true" : "false") + ",";
  payload += "\"lastHttpCode\":" + String(vitals.lastHttpCode);
  payload += "}";
  payload += "}";
  return payload;
}

// -----------------------------------------------------------------------------
// Comunicação com o backend (Render)
// -----------------------------------------------------------------------------

// Faz uma chamada para API_BASE_URL + path com o token do ESP32.
// Devolve o código HTTP (200, 201...) ou um número negativo em caso de falha.
int apiRequest(const char *method, const String &path, const String &body, String *response) {
  if (!apiConfigured()) {
    return -10;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return -1;
  }

  String url = String(API_BASE_URL) + path;
  HTTPClient http;
  http.setReuse(true);  // reaproveita a conexão segura: chamadas seguintes ficam bem mais rápidas
  http.setTimeout(HTTP_TIMEOUT_MS);

  bool started = url.startsWith("https://") ? http.begin(secureClient, url) : http.begin(plainClient, url);
  if (!started) {
    return -2;
  }

  http.addHeader("Authorization", String("Bearer ") + API_TOKEN);
  if (body.length() > 0) {
    http.addHeader("Content-Type", "application/json");
  }

  int code = http.sendRequest(method, body);
  if (response != nullptr && code > 0) {
    *response = http.getString();
  }
  http.end();

  vitals.lastHttpCode = code;
  return code;
}

int sendReading(const String &sessionId) {
  int code = apiRequest("POST", "/device/readings", buildVitalsJson(sessionId), nullptr);
  Serial.printf("[api] leitura enviada%s -> HTTP %d\n", sessionId.length() ? " (medicao)" : "", code);
  return code;
}

void startMeasurement(const String &sessionId, uint32_t durationSeconds) {
  measurement.active = true;
  measurement.sessionId = sessionId;
  measurement.endMs = millis() + durationSeconds * 1000UL;
  lastMeasurementPushMs = 0;
  Serial.printf("[medicao] Iniciada pelo app: %s (%lu s)\n", sessionId.c_str(), (unsigned long)durationSeconds);
}

void finishMeasurement() {
  sendReading(measurement.sessionId);  // última leitura antes de encerrar

  String body = "{\"status\":\"completed\"}";
  int code = apiRequest("POST", "/device/measurements/" + measurement.sessionId + "/complete", body, nullptr);
  Serial.printf("[medicao] Concluida -> HTTP %d\n", code);

  measurement.active = false;
  measurement.sessionId = "";
  lastPushMs = millis();
}

// Pergunta ao backend se o app pediu uma medição (botão "Iniciar").
void pollMeasurementCommand() {
  if (!apiConfigured() || measurement.active) {
    return;
  }

  uint32_t now = millis();
  if (now - lastCommandPollMs < commandPollIntervalMs) {
    return;
  }
  lastCommandPollMs = now;

  String response;
  int code = apiRequest("GET", "/device/commands/next", "", &response);
  if (code != 200) {
    if (code == 401) {
      Serial.println("[api] Token recusado (401). Confira o API_TOKEN.");
    }
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, response)) {
    return;
  }

  uint32_t pollMs = doc["pollIntervalMs"] | 3000;
  commandPollIntervalMs = constrain(pollMs, 1000UL, 60000UL);

  const char *command = doc["command"] | "none";
  if (strcmp(command, "start_measurement") == 0) {
    startMeasurement(doc["sessionId"].as<String>(), doc["durationSeconds"] | 30);
  }
}

// Durante a medição: envia leituras a cada MEASUREMENT_PUSH_INTERVAL_MS e
// encerra quando o tempo acabar. Fora dela: envio periódico de manutenção.
void pushToRemoteApi() {
  if (!apiConfigured()) {
    return;
  }

  uint32_t now = millis();

  if (measurement.active) {
    if ((int32_t)(now - measurement.endMs) >= 0) {
      finishMeasurement();
      return;
    }
    if (lastMeasurementPushMs == 0 || now - lastMeasurementPushMs >= MEASUREMENT_PUSH_INTERVAL_MS) {
      lastMeasurementPushMs = now;
      sendReading(measurement.sessionId);
    }
    return;
  }

  if (now - lastPushMs >= API_PUSH_INTERVAL_MS) {
    lastPushMs = now;
    sendReading("");
  }
}

// -----------------------------------------------------------------------------
// API local do ESP32 (igual ao repositório)
// -----------------------------------------------------------------------------

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
  server.send(200, "application/json", buildVitalsJson(measurement.sessionId));
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

// -----------------------------------------------------------------------------
// Inicialização
// -----------------------------------------------------------------------------

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
  byte ledMode = 2;  // RED + IR
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

  if (measurement.active) {
    int32_t remainingMs = (int32_t)(measurement.endMs - now);
    display.print("MEDINDO... ");
    display.print(remainingMs > 0 ? (remainingMs + 999) / 1000 : 0);
    display.println("s");
  } else if (!apiConfigured()) {
    display.println("API nao configurada");
  } else {
    display.print("Nuvem: HTTP ");
    display.println(vitals.lastHttpCode);
  }
  display.display();
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

  // Conexão HTTPS sem conferir o certificado do servidor: simples para o
  // protótipo/Wokwi. Para produção, use secureClient.setCACert(...).
  secureClient.setInsecure();

  Serial.println("API local pronta:");
  Serial.print("http://");
  Serial.print(ipAddressString());
  Serial.println("/api/v1/vitals");

  if (apiConfigured()) {
    Serial.print("Backend: ");
    Serial.println(API_BASE_URL);
    Serial.println("Aguardando o botao Iniciar do app...");
  } else {
    Serial.println("Backend NAO configurado: preencha API_BASE_URL e API_TOKEN.");
  }
}

void loop() {
  server.handleClient();
  readMax30102();
  readTemperature();
  updateDisplay();
  pollMeasurementCommand();
  pushToRemoteApi();
}
