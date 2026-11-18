#pragma once
// =============================================================================
// REDE: Wi-Fi 2,4 GHz + HTTPS com o backend no Render. Roda numa tarefa
// FreeRTOS própria (núcleo 0), então chamadas lentas não travam os sensores.
//
// Protocolo (o ESP32 sempre inicia a conversa; funciona atrás de NAT/hotspot):
//   POST /device/register                  ao conectar: versão, placa, sensores
//   POST /device/heartbeat                 a cada heartbeatIntervalMs (padrão 30 s)
//   GET  /device/commands/next             a cada pollIntervalMs (padrão 3 s)
//   POST /device/readings                  durante a coleta: lote a cada BATCH_INTERVAL_MS
//   POST /device/measurements/:id/complete fim da coleta (completed ou failed + motivo)
//
// Cabeçalhos: Authorization: Bearer <DEVICE_TOKEN> e X-Device-Id: <DEVICE_ID>.
// =============================================================================
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "vs_shared.h"

static WiFiClientSecure tlsClient;
static WiFiClient plainClient;

static constexpr uint32_t WIFI_RETRY_MIN_MS = 5000;
static constexpr uint32_t WIFI_RETRY_MAX_MS = 60000;
static constexpr uint32_t AP_FALLBACK_AFTER_MS = 60000;
static constexpr uint32_t REGISTER_RETRY_MAX_MS = 60000;
static constexpr uint16_t PENDING_CAPACITY = 120;  // até 2 min de coleta guardados se a rede cair
static constexpr uint8_t READINGS_PER_REQUEST = 30;
static constexpr uint32_t FINISH_GRACE_MS = 90000;  // tempo máximo para conseguir entregar o fim da coleta

struct Collection {
  bool active = false;
  bool finishing = false;
  char sessionId[40] = "";
  uint32_t endMs = 0;
  uint32_t lastSendMs = 0;
  uint32_t snapshots = 0;
  uint32_t validSnapshots = 0;
  uint32_t fingerSnapshots = 0;
  uint32_t sentSnapshots = 0;
};

static Collection collection;
static Snapshot pending[PENDING_CAPACITY];
static uint16_t pendingCount = 0;

static bool registered = false;
static bool everConnected = false;
static bool wasConnected = false;
static uint32_t bootMs = 0;
static uint32_t lastWifiAttemptMs = 0;
static uint32_t wifiRetryMs = WIFI_RETRY_MIN_MS;
static uint32_t nextRegisterMs = 0;
static uint32_t registerRetryMs = 5000;
static uint32_t lastPollMs = 0;
static uint32_t lastHeartbeatMs = 0;
static uint32_t pollIntervalMs = 3000;
static uint32_t heartbeatIntervalMs = 30000;

// -----------------------------------------------------------------------------
// HTTP
// -----------------------------------------------------------------------------
static bool configLooksFilled() {
  String url = API_BASE_URL;
  String token = DEVICE_TOKEN;
  return url.length() > 10 && url.indexOf("SEU-SERVICO") < 0 && token.length() >= 20 && token.indexOf("COLOQUE") < 0;
}

/** Chama API_BASE_URL + path. Devolve o código HTTP (>0) ou um código negativo de falha. */
static int apiRequest(const char *method, const String &path, const String &body, String *response) {
  if (WiFi.status() != WL_CONNECTED) return -1;

  String url = String(API_BASE_URL) + path;
  HTTPClient http;
  http.setReuse(true);  // mantém a conexão TLS aberta: as próximas chamadas ficam bem mais rápidas
  http.setConnectTimeout(15000);
  http.setTimeout(HTTP_TIMEOUT_MS);

  bool started = url.startsWith("https://") ? http.begin(tlsClient, url) : http.begin(plainClient, url);
  if (!started) return -2;

  http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
  http.addHeader("X-Device-Id", DEVICE_ID);
  http.addHeader("User-Agent", "VitalSenseESP32/" FIRMWARE_VERSION);
  if (body.length() > 0) http.addHeader("Content-Type", "application/json");

  int code = body.length() > 0 ? http.sendRequest(method, (uint8_t *)body.c_str(), body.length())
                               : http.sendRequest(method);
  if (response != nullptr && code > 0) *response = http.getString();
  http.end();

  gNet.lastHttpCode = code;
  gNet.backendOk = code > 0 && code < 500;
  return code;
}

static void addDeviceInfo(JsonDocument &doc) {
  doc["deviceId"] = DEVICE_ID;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  doc["boardModel"] = BOARD_MODEL;
  doc["dataSource"] = DATA_SOURCE;
  doc["wifiMode"] = "station";
  doc["wifiRssi"] = WiFi.RSSI();
  doc["ip"] = gNet.ip;
  JsonObject sensors = doc["sensors"].to<JsonObject>();
  sensors["max30102"] = (bool)gSensors.max30102;
  sensors["max30205"] = (bool)gSensors.max30205;
  sensors["oled"] = (bool)gSensors.oled;
  sensors["mpu6050"] = false;  // ainda não montado neste protótipo
}

static void applyTiming(JsonDocument &doc) {
  uint32_t poll = doc["pollIntervalMs"] | pollIntervalMs;
  uint32_t heartbeat = doc["heartbeatIntervalMs"] | heartbeatIntervalMs;
  pollIntervalMs = constrain(poll, 1000UL, 60000UL);
  heartbeatIntervalMs = constrain(heartbeat, 5000UL, 300000UL);
  if (!doc["claimed"].isNull()) gNet.claimed = doc["claimed"].as<bool>();
}

static void explainHttpError(const char *step, int code) {
  if (code == 401) {
    Serial.printf("[api] %s: token recusado (401). Confira DEVICE_TOKEN e DEVICE_ID no config.h.\n", step);
    setNetMessage("Token recusado");
  } else if (code == 403) {
    Serial.printf("[api] %s: aparelho bloqueado no servidor (403).\n", step);
    setNetMessage("Bloqueado (403)");
  } else if (code == -11 || code == HTTPC_ERROR_READ_TIMEOUT) {
    Serial.printf("[api] %s: tempo esgotado. O Render pode estar acordando (~1 min).\n", step);
    setNetMessage("Servidor acordando");
  } else if (code < 0) {
    Serial.printf("[api] %s: falha de conexao (%d: %s).\n", step, code, HTTPClient::errorToString(code).c_str());
    setNetMessage("Sem acesso ao servidor");
  } else {
    Serial.printf("[api] %s: HTTP %d.\n", step, code);
    char text[24];
    snprintf(text, sizeof(text), "Erro HTTP %d", code);
    setNetMessage(text);
  }
}

// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------
static void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;
  uint8_t reason = info.wifi_sta_disconnected.reason;
  if (reason == WIFI_REASON_NO_AP_FOUND) {
    Serial.println("[wifi] Rede nao encontrada. Nome certo? Ela esta em 2,4 GHz? (5 GHz nao funciona)");
  } else if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
             reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
    Serial.println("[wifi] Falha de autenticacao: confira WIFI_PASSWORD.");
  } else {
    Serial.printf("[wifi] Desconectado (motivo %u).\n", reason);
  }
}

void startWifi() {
  bootMs = millis();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_ID);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWifiEvent);
  Serial.printf("[wifi] Conectando em \"%s\"...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttemptMs = millis();
  setNetMessage("Conectando WiFi");
}

static void maintainWifi(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    gNet.rssi = WiFi.RSSI();
    if (!wasConnected) {
      wasConnected = true;
      everConnected = true;
      gNet.wifiConnected = true;
      strncpy(gNet.ip, WiFi.localIP().toString().c_str(), sizeof(gNet.ip) - 1);
      Serial.printf("[wifi] Conectado. IP %s, sinal %d dBm.\n", gNet.ip, gNet.rssi);
      wifiRetryMs = WIFI_RETRY_MIN_MS;
      registered = false;  // registra de novo (IP pode ter mudado)
      nextRegisterMs = now;
      registerRetryMs = 5000;
      setNetMessage("Conectando servidor");
    }
    return;
  }

  if (wasConnected) {
    wasConnected = false;
    gNet.wifiConnected = false;
    gNet.backendOk = false;
    Serial.println("[wifi] Conexao perdida. Tentando de novo...");
    setNetMessage("Sem WiFi");
  }

  // Reconexão controlada (5 s, 10 s, 20 s... até 60 s), sem travar nada.
  if (now - lastWifiAttemptMs >= wifiRetryMs) {
    lastWifiAttemptMs = now;
    Serial.printf("[wifi] Nova tentativa (proxima em ate %lu s).\n", (unsigned long)(wifiRetryMs / 1000));
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    wifiRetryMs = min(wifiRetryMs * 2, WIFI_RETRY_MAX_MS);
  }

#if ENABLE_AP_FALLBACK
  // Nunca conectou: abre uma rede própria para ver a página local (plano B).
  if (!everConnected && !gNet.apActive && strlen(AP_PASSWORD) >= 8 && now - bootMs >= AP_FALLBACK_AFTER_MS) {
    char apName[32];
    snprintf(apName, sizeof(apName), "VitalSense-%s", DEVICE_ID);
    WiFi.mode(WIFI_AP_STA);
    gNet.apActive = WiFi.softAP(apName, AP_PASSWORD);
    if (gNet.apActive) Serial.printf("[wifi] Rede propria \"%s\" ativa: http://192.168.4.1\n", apName);
  }
#endif
}

// -----------------------------------------------------------------------------
// Registro, heartbeat e comandos
// -----------------------------------------------------------------------------
static void registerDevice(uint32_t now) {
  if (now < nextRegisterMs) return;

  JsonDocument doc;
  addDeviceInfo(doc);
  String body, response;
  serializeJson(doc, body);

  int code = apiRequest("POST", "/device/register", body, &response);
  if (code == 200) {
    JsonDocument reply;
    if (!deserializeJson(reply, response)) applyTiming(reply);
    registered = true;
    lastHeartbeatMs = now;
    Serial.printf("[api] Registrado no backend como %s. %s\n", DEVICE_ID,
                  gNet.claimed ? "Vinculado a uma conta." : "Ainda NAO vinculado: no app, Configuracoes > Sincronizacao ESP32.");
    setNetMessage(gNet.claimed ? "Online" : "Vincule no app:");
    return;
  }

  explainHttpError("register", code);
  nextRegisterMs = now + registerRetryMs;
  registerRetryMs = min(registerRetryMs * 2, REGISTER_RETRY_MAX_MS);
}

static void sendHeartbeat(uint32_t now) {
  lastHeartbeatMs = now;
  JsonDocument doc;
  addDeviceInfo(doc);
  doc["uptimeMs"] = now;
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["collecting"] = (bool)gCollecting;
  String body, response;
  serializeJson(doc, body);

  int code = apiRequest("POST", "/device/heartbeat", body, &response);
  if (code == 200) {
    JsonDocument reply;
    if (!deserializeJson(reply, response)) applyTiming(reply);
    setNetMessage(gNet.claimed ? "Online" : "Vincule no app:");
  } else {
    explainHttpError("heartbeat", code);
    if (code == 401) registered = false;
  }
}

static void finishCollection(const char *status, const char *reason);

static void failCollectionNow(const char *sessionId, const char *reason) {
  JsonDocument doc;
  doc["status"] = "failed";
  doc["reason"] = reason;
  String body;
  serializeJson(doc, body);
  int code = apiRequest("POST", String("/device/measurements/") + sessionId + "/complete", body, nullptr);
  Serial.printf("[coleta] Recusada: %s -> HTTP %d\n", reason, code);
}

static void startCollection(const char *sessionId, uint32_t durationSeconds) {
  // Sem nenhum sensor não há o que medir: avisa o backend em vez de enviar vazio.
  if (!gSensors.max30102 && !gSensors.max30205) {
    failCollectionNow(sessionId, "Nenhum sensor respondeu no I2C (MAX30102 e MAX30205 ausentes).");
    return;
  }

  collection = Collection();
  collection.active = true;
  strncpy(collection.sessionId, sessionId, sizeof(collection.sessionId) - 1);
  collection.endMs = millis() + durationSeconds * 1000UL;
  collection.lastSendMs = millis();
  pendingCount = 0;
  xQueueReset(gSnapshotQueue);
  gSnapshotsDropped = 0;
  gCollectionEndMs = collection.endMs;
  gCollecting = true;

  Serial.printf("[coleta] Iniciada pelo app: %s (%lu s). Coloque o dedo no sensor.\n", sessionId,
                (unsigned long)durationSeconds);
  if (!gSensors.max30102) Serial.println("[coleta] Aviso: MAX30102 ausente -> sem FC/SpO2 nesta coleta.");
  if (!gSensors.max30205) Serial.println("[coleta] Aviso: MAX30205 ausente -> sem temperatura nesta coleta.");
}

static void pollCommand(uint32_t now) {
  lastPollMs = now;
  String response;
  int code = apiRequest("GET", "/device/commands/next", "", &response);
  if (code != 200) {
    explainHttpError("commands/next", code);
    if (code == 401) registered = false;
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, response)) return;
  applyTiming(doc);

  const char *command = doc["command"] | "none";
  if (strcmp(command, "start_measurement") == 0) {
    const char *sessionId = doc["sessionId"] | "";
    uint32_t duration = doc["durationSeconds"] | 30;
    if (strlen(sessionId) > 0) startCollection(sessionId, constrain(duration, 5UL, 300UL));
  }
}

// -----------------------------------------------------------------------------
// Coleta: junta as amostras do loop() e envia em lotes
// -----------------------------------------------------------------------------
static void drainSnapshotQueue() {
  Snapshot snapshot;
  while (xQueueReceive(gSnapshotQueue, &snapshot, 0) == pdTRUE) {
    collection.snapshots++;
    if (snapshot.fingerDetected) collection.fingerSnapshots++;
    if (isfinite(snapshot.heartRateBpm) || isfinite(snapshot.spo2Pct) || isfinite(snapshot.bodyTempC)) {
      collection.validSnapshots++;
    }
    if (pendingCount == PENDING_CAPACITY) {
      memmove(pending, pending + 1, sizeof(Snapshot) * (PENDING_CAPACITY - 1));
      pendingCount--;
      gSnapshotsDropped = gSnapshotsDropped + 1;
    }
    pending[pendingCount++] = snapshot;
  }
}

static void setNullableFloat(JsonObject object, const char *key, float value, uint8_t decimals) {
  if (isfinite(value)) {
    float factor = decimals == 2 ? 100.0f : 10.0f;
    object[key] = roundf(value * factor) / factor;
  } else {
    object[key] = nullptr;
  }
}

/** Envia até READINGS_PER_REQUEST amostras. Devolve false se a coleta foi encerrada no servidor. */
static bool sendPendingBatch() {
  if (pendingCount == 0) return true;
  uint8_t count = pendingCount > READINGS_PER_REQUEST ? READINGS_PER_REQUEST : pendingCount;
  uint32_t now = millis();

  JsonDocument doc;
  doc["sessionId"] = collection.sessionId;
  doc["dataSource"] = DATA_SOURCE;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  JsonObject sensors = doc["sensors"].to<JsonObject>();
  sensors["max30102"] = (bool)gSensors.max30102;
  sensors["max30205"] = (bool)gSensors.max30205;
  JsonArray readings = doc["readings"].to<JsonArray>();
  for (uint8_t i = 0; i < count; i++) {
    const Snapshot &s = pending[i];
    JsonObject item = readings.add<JsonObject>();
    item["ageMs"] = now - s.capturedMs;  // o ESP32 não tem relógio: o servidor calcula a hora
    item["fingerDetected"] = s.fingerDetected;
    setNullableFloat(item, "heartRateBpm", s.heartRateBpm, 1);
    setNullableFloat(item, "spo2Pct", s.spo2Pct, 1);
    setNullableFloat(item, "bodyTempC", s.bodyTempC, 2);
    JsonObject quality = item["quality"].to<JsonObject>();
    quality["lastBeatAgeMs"] = s.lastBeatAgeMs;
    quality["max30102Samples"] = s.samples;
  }

  String body, response;
  serializeJson(doc, body);
  int code = apiRequest("POST", "/device/readings", body, &response);

  if (code == 201 || code == 202) {
    memmove(pending, pending + count, sizeof(Snapshot) * (pendingCount - count));
    pendingCount -= count;
    collection.sentSnapshots += count;
    JsonDocument reply;
    int stored = 0, rejected = 0;
    if (!deserializeJson(reply, response)) {
      stored = reply["stored"] | 0;
      rejected = reply["rejected"] | 0;
    }
    Serial.printf("[coleta] Lote de %u -> HTTP %d (gravadas %d, rejeitadas %d)\n", count, code, stored, rejected);
    return true;
  }
  if (code == 409 || code == 404) {
    Serial.println("[coleta] Coleta encerrada no servidor (cancelada no app ou expirada). Parando.");
    return false;
  }
  explainHttpError("readings", code);  // mantém as amostras para tentar de novo
  return true;
}

static void resetCollection() {
  gCollecting = false;
  collection = Collection();
  pendingCount = 0;
  if (gSnapshotQueue) xQueueReset(gSnapshotQueue);
}

static void finishCollection(const char *status, const char *reason) {
  JsonDocument doc;
  doc["status"] = status;
  if (reason) doc["reason"] = reason;
  doc["snapshots"] = collection.snapshots;
  doc["validSnapshots"] = collection.validSnapshots;
  doc["dropped"] = (uint32_t)gSnapshotsDropped;
  String body, response;
  serializeJson(doc, body);

  int code = apiRequest("POST", String("/device/measurements/") + collection.sessionId + "/complete", body, &response);
  if (code == 200 || code == 404) {
    JsonDocument reply;
    const char *finalStatus = status;
    if (code == 200 && !deserializeJson(reply, response)) finalStatus = reply["status"] | status;
    Serial.printf("[coleta] Finalizada -> HTTP %d (%s). Amostras: %lu, validas: %lu.\n", code, finalStatus,
                  (unsigned long)collection.snapshots, (unsigned long)collection.validSnapshots);
    setNetMessage(strcmp(finalStatus, "completed") == 0 ? "Coleta concluida" : "Coleta falhou");
    resetCollection();
  } else {
    explainHttpError("complete", code);  // tenta de novo na próxima volta
  }
}

static void serviceCollection(uint32_t now) {
  drainSnapshotQueue();

  // Ainda medindo: envia um lote a cada BATCH_INTERVAL_MS.
  if (!collection.finishing) {
    if ((int32_t)(now - collection.endMs) >= 0) {
      gCollecting = false;  // o loop() para de gerar amostras
      collection.finishing = true;
      vTaskDelay(pdMS_TO_TICKS(50));
      drainSnapshotQueue();
    } else if (now - collection.lastSendMs >= BATCH_INTERVAL_MS) {
      collection.lastSendMs = now;
      if (!sendPendingBatch()) resetCollection();
      return;
    } else {
      return;
    }
  }

  // Finalizando: entrega o que falta e avisa o fim.
  if ((int32_t)(now - collection.endMs) > (int32_t)FINISH_GRACE_MS) {
    Serial.println("[coleta] Nao foi possivel entregar o fim da coleta (sem rede). Descartando.");
    resetCollection();
    return;
  }
  if (pendingCount > 0) {
    if (!sendPendingBatch()) resetCollection();
    return;
  }

  if (collection.validSnapshots > 0) {
    finishCollection("completed", nullptr);
  } else {
    const char *reason = !gSensors.max30102 && !gSensors.max30205 ? "Sensores pararam de responder durante a coleta."
                         : collection.fingerSnapshots == 0 && gSensors.max30102
                             ? "Dedo nao detectado no MAX30102 e sem temperatura valida."
                             : "Nenhuma leitura valida durante a coleta.";
    finishCollection("failed", reason);
  }
}

// -----------------------------------------------------------------------------
// Tarefa de rede
// -----------------------------------------------------------------------------
void networkTask(void *) {
#ifdef ROOT_CA_PEM
  tlsClient.setCACert(ROOT_CA_PEM);
  Serial.println("[api] HTTPS com verificacao do certificado (ROOT_CA_PEM).");
#else
  tlsClient.setInsecure();
  Serial.println("[api] HTTPS criptografado SEM verificar o certificado (defina ROOT_CA_PEM para verificar).");
#endif

  if (!configLooksFilled()) {
    Serial.println("[api] ATENCAO: API_BASE_URL/DEVICE_TOKEN parecem nao preenchidos no config.h.");
    setNetMessage("Configure config.h");
  }

  for (;;) {
    uint32_t now = millis();
    maintainWifi(now);

    if (WiFi.status() == WL_CONNECTED) {
      if (!registered) {
        registerDevice(now);
      } else if (collection.active) {
        serviceCollection(now);
      } else {
        if (now - lastPollMs >= pollIntervalMs) pollCommand(millis());
        if (millis() - lastHeartbeatMs >= heartbeatIntervalMs) sendHeartbeat(millis());
      }
    } else if (collection.active) {
      // Sem Wi-Fi: continua guardando as amostras (até 2 min) para enviar depois.
      drainSnapshotQueue();
      if ((int32_t)(now - collection.endMs) >= 0) gCollecting = false;
      if ((int32_t)(now - collection.endMs) > (int32_t)FINISH_GRACE_MS) resetCollection();
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
