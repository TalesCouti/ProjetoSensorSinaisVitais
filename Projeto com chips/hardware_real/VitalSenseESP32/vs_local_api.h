#pragma once
// =============================================================================
// PÁGINA E JSON LOCAIS (PLANO B): com o celular/computador na MESMA rede do
// ESP32 (ou na rede própria "VitalSense-..."), abra http://<IP do ESP32>/
// para ver os valores atuais sem depender da internet nem do backend.
//   GET /                -> página simples que se atualiza sozinha
//   GET /api/v1/vitals   -> JSON com os valores atuais
//   GET /api/v1/health   -> status do aparelho
// Desligue com ENABLE_LOCAL_API 0 se não quiser expor isso na rede local.
// =============================================================================
#include <WebServer.h>
#include <ArduinoJson.h>
#include "vs_shared.h"

#if ENABLE_LOCAL_API
static WebServer localServer(80);

static void putNullable(JsonDocument &doc, const char *key, float value) {
  if (isfinite(value)) doc[key] = value;
  else doc[key] = nullptr;
}

static void handleVitals() {
  LiveVitals v = readVitals();
  JsonDocument doc;
  doc["deviceId"] = DEVICE_ID;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  doc["dataSource"] = DATA_SOURCE;
  doc["uptimeMs"] = millis();
  putNullable(doc, "heartRateBpm", v.heartRateBpm);
  putNullable(doc, "spo2Pct", v.spo2Pct);
  putNullable(doc, "bodyTempC", v.bodyTempC);
  doc["fingerDetected"] = v.fingerDetected;
  doc["collecting"] = (bool)gCollecting;
  JsonObject sensors = doc["sensors"].to<JsonObject>();
  sensors["max30102"] = (bool)gSensors.max30102;
  sensors["max30205"] = (bool)gSensors.max30205;
  sensors["oled"] = (bool)gSensors.oled;
  JsonObject net = doc["network"].to<JsonObject>();
  net["wifi"] = (bool)gNet.wifiConnected;
  net["backend"] = (bool)gNet.backendOk;
  net["lastHttpCode"] = (int)gNet.lastHttpCode;
  net["status"] = gNet.message;
  String body;
  serializeJson(doc, body);
  localServer.sendHeader("Cache-Control", "no-store");
  localServer.send(200, "application/json", body);
}

static void handleHealth() {
  String body = String("{\"ok\":true,\"deviceId\":\"") + DEVICE_ID + "\",\"uptimeMs\":" + millis() + "}";
  localServer.send(200, "application/json", body);
}

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="pt-BR"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>VitalSense ESP32</title>
<style>body{font-family:system-ui,sans-serif;margin:16px;background:#f6f7f9;color:#1d2433}
.card{background:#fff;border-radius:12px;padding:16px;margin-bottom:12px;box-shadow:0 1px 3px #0001}
.v{font-size:2.2em;font-weight:700}.u{color:#667}.s{font-size:.9em;color:#556}
.warn{font-size:.8em;color:#8a5a00}</style></head><body>
<h2>VitalSense &middot; leitura local</h2>
<div class="card"><div class="u">Frequência cardíaca</div><div class="v" id="hr">--</div></div>
<div class="card"><div class="u">SpO&#8322;</div><div class="v" id="ox">--</div></div>
<div class="card"><div class="u">Temperatura</div><div class="v" id="tp">--</div></div>
<div class="card s" id="st">carregando...</div>
<p class="warn">Protótipo educacional: estimativas sem calibração clínica. Não substitui equipamento médico.</p>
<script>
const f=(v,d,u)=>v==null?'--':v.toFixed(d)+' '+u;
async function tick(){try{const r=await fetch('/api/v1/vitals',{cache:'no-store'});const j=await r.json();
hr.textContent=f(j.heartRateBpm,0,'bpm');ox.textContent=f(j.spo2Pct,1,'%');tp.textContent=f(j.bodyTempC,2,'°C');
st.textContent='Dedo: '+(j.fingerDetected?'sim':'não')+' · MAX30102: '+(j.sensors.max30102?'ok':'ausente')+
' · MAX30205: '+(j.sensors.max30205?'ok':'ausente')+' · Backend: '+j.network.status+(j.collecting?' · COLETANDO':'');}
catch(e){st.textContent='sem resposta do ESP32';}}
tick();setInterval(tick,2000);
</script></body></html>)HTML";

static void handleRoot() {
  localServer.send_P(200, "text/html; charset=utf-8", PAGE);
}

void setupLocalApi() {
  localServer.on("/", HTTP_GET, handleRoot);
  localServer.on("/api/v1/vitals", HTTP_GET, handleVitals);
  localServer.on("/api/v1/health", HTTP_GET, handleHealth);
  localServer.onNotFound([]() { localServer.send(404, "application/json", "{\"error\":\"not_found\"}"); });
  localServer.begin();
  Serial.println("[local] Pagina local ativa na porta 80 (http://<IP do ESP32>/).");
}

void localApiTick() {
  localServer.handleClient();
}
#else
void setupLocalApi() {}
void localApiTick() {}
#endif
