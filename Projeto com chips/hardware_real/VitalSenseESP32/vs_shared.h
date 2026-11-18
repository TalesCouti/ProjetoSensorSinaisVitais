#pragma once
// =============================================================================
// Estado compartilhado entre as duas tarefas do firmware:
//   - loop() (núcleo 1): sensores, tela OLED e página local;
//   - networkTask (núcleo 0): Wi-Fi e conversa com o backend.
// Separar as duas coisas evita que um HTTPS lento (1 a 30 s) pare a leitura do
// MAX30102, cuja FIFO guarda só 32 amostras (~1,3 s a 25 amostras/s).
// =============================================================================
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <math.h>

#define FIRMWARE_VERSION "2.0.0-hw"
#define DATA_SOURCE "hardware"  // placa real: os dados aparecem no app como medição do sensor

// Sensores encontrados no barramento I2C (atualizado pelo loop, lido pela rede).
struct SensorStatus {
  volatile bool max30102 = false;
  volatile bool max30205 = false;
  volatile bool oled = false;
};

// Valores atuais calculados a partir dos sensores. NAN = sem valor válido.
struct LiveVitals {
  float heartRateBpm = NAN;
  float spo2Pct = NAN;
  float bodyTempC = NAN;
  bool fingerDetected = false;
  uint32_t irValue = 0;
  uint32_t samples = 0;
  uint32_t lastBeatAgeMs = 0;
  uint32_t fifoOverflows = 0;
};

// Uma amostra consolidada (1 por segundo) enviada ao backend durante a coleta.
struct Snapshot {
  uint32_t capturedMs;
  float heartRateBpm;
  float spo2Pct;
  float bodyTempC;
  bool fingerDetected;
  uint32_t lastBeatAgeMs;
  uint32_t samples;
};

// Situação da rede, mostrada na tela e na página local.
struct NetStatus {
  volatile bool wifiConnected = false;
  volatile bool backendOk = false;
  volatile bool claimed = false;
  volatile bool apActive = false;
  volatile int lastHttpCode = 0;
  volatile int rssi = 0;
  char ip[16] = "0.0.0.0";
  char message[24] = "Iniciando...";
};

SensorStatus gSensors;
LiveVitals gVitals;
NetStatus gNet;
portMUX_TYPE gVitalsMux = portMUX_INITIALIZER_UNLOCKED;

// Coleta em andamento: a rede liga/desliga, o loop gera as amostras.
volatile bool gCollecting = false;
volatile uint32_t gCollectionEndMs = 0;
QueueHandle_t gSnapshotQueue = nullptr;
volatile uint32_t gSnapshotsDropped = 0;

/** Copia os valores atuais com segurança (as duas tarefas acessam gVitals). */
LiveVitals readVitals() {
  portENTER_CRITICAL(&gVitalsMux);
  LiveVitals copy = gVitals;
  portEXIT_CRITICAL(&gVitalsMux);
  return copy;
}

void setNetMessage(const char *text) {
  strncpy(gNet.message, text, sizeof(gNet.message) - 1);
  gNet.message[sizeof(gNet.message) - 1] = '\0';
}
