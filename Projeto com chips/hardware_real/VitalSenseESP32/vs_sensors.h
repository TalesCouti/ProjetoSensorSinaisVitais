#pragma once
// =============================================================================
// SENSORES: MAX30102 (FC e SpO2 por luz vermelha/infravermelha) e MAX30205
// (temperatura de contato). Roda só no loop() (núcleo 1): é a única tarefa
// que usa o barramento I2C.
//
// Mesma lógica validada no Wokwi ("Projeto com chips/sketch.ino"):
//   - dedo: IR > FINGER_IR_THRESHOLD;
//   - FC: checkForBeat (SparkFun) -> intervalo entre batidas -> 40-180 bpm ->
//     média das últimas 8;
//   - SpO2: "razão das razões" numa janela de 100 amostras (~4 s):
//     R = (AC_verm/DC_verm)/(AC_IR/DC_IR), SpO2 ~ 110 - 25*R;
//   - temperatura: registrador 0x00 do MAX30205 (1 bit = 1/256 °C).
//
// Ajustes para o hardware real (diferenças em relação ao Wokwi):
//   1. A FIFO do MAX30102 é lida DIRETO pelos registradores (amostra por
//      amostra, vermelho e IR da mesma amostra). O buffer interno da biblioteca
//      SparkFun guarda só 4 amostras; com mais pendentes ele perdia dados.
//   2. Nada é "inventado" ou forçado para dentro da faixa: SpO2 fora de 70-100
//      vira inválido (o original usava constrain e mostrava 70 ou 100); FC só
//      vale com pelo menos 3 batidas recentes; sem dedo, FC e SpO2 = inválido.
//   3. Sensor ausente é procurado de novo a cada 10 s (fio solto na protoboard).
// =============================================================================
#include <Wire.h>
#include <MAX30105.h>
#include <heartRate.h>
#include "vs_shared.h"

static MAX30105 particleSensor;

static constexpr uint8_t REG_FIFO_WR_PTR = 0x04;
static constexpr uint8_t REG_OVF_COUNTER = 0x05;
static constexpr uint8_t REG_FIFO_RD_PTR = 0x06;
static constexpr uint8_t REG_FIFO_DATA = 0x07;
static constexpr uint8_t MAX30102_FIFO_DEPTH = 32;
static constexpr uint8_t FIFO_SAMPLES_PER_READ = 20;  // 20 x 6 bytes = 120 (buffer I2C do ESP32 = 128)

static constexpr uint32_t SAMPLE_PERIOD_MS = (1000UL * MAX30102_SAMPLE_AVERAGE) / MAX30102_SAMPLE_RATE;
static constexpr uint32_t FIFO_POLL_MS = 10;
static constexpr uint32_t TEMP_READ_MS = 1000;
static constexpr uint32_t SENSOR_RETRY_MS = 10000;
static constexpr uint16_t SPO2_WINDOW_SAMPLES = 100;
static constexpr uint8_t BPM_BUFFER_SIZE = 8;
static constexpr uint8_t BPM_MIN_BEATS = 3;        // batidas válidas antes de mostrar a FC
static constexpr uint32_t BEAT_STALE_MS = 3000;    // sem batida há 3 s: FC inválida
static constexpr uint32_t SPO2_STALE_MS = 10000;   // SpO2 calculada há mais de 10 s: inválida
static constexpr uint32_t TEMP_STALE_MS = 3000;

struct OpticalState {
  float bpm[BPM_BUFFER_SIZE];
  uint8_t bpmIndex = 0;
  uint8_t bpmCount = 0;
  uint32_t lastBeatMs = 0;
  double redSum = 0, irSum = 0, redSqSum = 0, irSqSum = 0;
  uint16_t windowSamples = 0;
  float spo2 = NAN;
  uint32_t spo2AtMs = 0;
  bool finger = false;
  uint32_t ir = 0;
  uint32_t samples = 0;
  uint32_t overflows = 0;
  uint8_t readErrors = 0;
};

static OpticalState opt;
static float lastTempC = NAN;
static uint32_t lastTempOkMs = 0;
static uint8_t tempErrors = 0;
static uint32_t lastFifoPollMs = 0;
static uint32_t lastTempReadMs = 0;
static uint32_t lastSensorRetryMs = 0;

// -----------------------------------------------------------------------------
// I2C
// -----------------------------------------------------------------------------
bool i2cDevicePresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

/** Lista no serial tudo o que responde no barramento (ajuda a conferir ligações e endereços). */
void scanI2cBus() {
  Serial.printf("[i2c] Procurando dispositivos (SDA=%d, SCL=%d)...\n", I2C_SDA_PIN, I2C_SCL_PIN);
  uint8_t found = 0;
  for (uint8_t address = 0x08; address < 0x78; address++) {
    if (!i2cDevicePresent(address)) continue;
    found++;
    const char *hint = "";
    if (address == MAX30102_ADDRESS) hint = " (MAX30102)";
    else if (address == MAX30205_ADDRESS) hint = " (MAX30205)";
    else if (address == OLED_ADDRESS) hint = " (OLED)";
    else if (address >= 0x48 && address <= 0x4F) hint = " (talvez o MAX30205 em outro endereco: ajuste MAX30205_ADDRESS)";
    else if (address == 0x3C || address == 0x3D) hint = " (talvez o OLED: ajuste OLED_ADDRESS)";
    Serial.printf("[i2c]   0x%02X%s\n", address, hint);
  }
  if (found == 0) Serial.println("[i2c]   Nenhum dispositivo. Confira 3V3, GND, SDA e SCL.");
}

static bool readRegister8(uint8_t address, uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)address, (uint8_t)1) != 1) return false;
  value = Wire.read();
  return true;
}

// -----------------------------------------------------------------------------
// MAX30102: cálculo de FC e SpO2
// -----------------------------------------------------------------------------
static void resetOptical() {
  opt.bpmIndex = 0;
  opt.bpmCount = 0;
  opt.lastBeatMs = 0;
  opt.redSum = opt.irSum = opt.redSqSum = opt.irSqSum = 0;
  opt.windowSamples = 0;
  opt.spo2 = NAN;
  opt.spo2AtMs = 0;
}

static void updateHeartRate(uint32_t irValue, uint32_t sampleMs) {
  if (!checkForBeat((long)irValue)) return;

  if (opt.lastBeatMs != 0) {
    float deltaSec = (sampleMs - opt.lastBeatMs) / 1000.0f;
    if (deltaSec > 0.0f) {
      float bpm = 60.0f / deltaSec;
      if (bpm >= 40.0f && bpm <= 180.0f) {
        opt.bpm[opt.bpmIndex] = bpm;
        opt.bpmIndex = (opt.bpmIndex + 1) % BPM_BUFFER_SIZE;
        if (opt.bpmCount < BPM_BUFFER_SIZE) opt.bpmCount++;
      }
    }
  }
  opt.lastBeatMs = sampleMs;
}

static void updateSpo2(uint32_t redValue, uint32_t irValue, uint32_t sampleMs) {
  opt.redSum += redValue;
  opt.irSum += irValue;
  opt.redSqSum += (double)redValue * redValue;
  opt.irSqSum += (double)irValue * irValue;
  opt.windowSamples++;
  if (opt.windowSamples < SPO2_WINDOW_SAMPLES) return;

  double n = opt.windowSamples;
  double redDc = opt.redSum / n;
  double irDc = opt.irSum / n;
  double redVar = opt.redSqSum / n - redDc * redDc;
  double irVar = opt.irSqSum / n - irDc * irDc;
  double redAc = redVar > 0 ? sqrt(redVar) : 0;
  double irAc = irVar > 0 ? sqrt(irVar) : 0;

  if (redDc > 0 && irDc > 0 && redAc > 0 && irAc > 0) {
    double ratio = (redAc / redDc) / (irAc / irDc);
    float spo2 = 110.0f - 25.0f * (float)ratio;
    // Fora da faixa plausível = sinal ruim (movimento, luz, mau contato): descarta.
    if (spo2 >= 70.0f && spo2 <= 100.0f) {
      opt.spo2 = spo2;
      opt.spo2AtMs = sampleMs;
    } else {
      opt.spo2 = NAN;
    }
  }
  opt.redSum = opt.irSum = opt.redSqSum = opt.irSqSum = 0;
  opt.windowSamples = 0;
}

static void processOpticalSample(uint32_t redValue, uint32_t irValue, uint32_t sampleMs) {
  opt.samples++;
  opt.ir = irValue;
  bool finger = irValue > FINGER_IR_THRESHOLD;

  if (!finger) {
    if (opt.finger) resetOptical();
    opt.finger = false;
    return;
  }
  if (!opt.finger) {
    resetOptical();  // dedo acabou de chegar: começa do zero
    opt.finger = true;
  }

  updateHeartRate(irValue, sampleMs);
  updateSpo2(redValue, irValue, sampleMs);
}

/** Lê todas as amostras novas da FIFO do MAX30102. */
static void readMax30102Fifo(uint32_t now) {
  uint8_t writePtr, readPtr, overflow = 0;
  if (!readRegister8(MAX30102_ADDRESS, REG_FIFO_WR_PTR, writePtr) ||
      !readRegister8(MAX30102_ADDRESS, REG_FIFO_RD_PTR, readPtr)) {
    if (++opt.readErrors >= 5 && gSensors.max30102) {
      gSensors.max30102 = false;
      Serial.println("[sensor] MAX30102 parou de responder.");
    }
    return;
  }
  opt.readErrors = 0;
  readRegister8(MAX30102_ADDRESS, REG_OVF_COUNTER, overflow);

  uint8_t pending = (writePtr - readPtr) & (MAX30102_FIFO_DEPTH - 1);
  if (pending == 0 && overflow > 0) pending = MAX30102_FIFO_DEPTH;  // FIFO cheia
  if (overflow > 0) {
    // Amostras perdidas: o intervalo até a próxima batida ficaria errado.
    opt.overflows += overflow;
    opt.lastBeatMs = 0;
  }
  if (pending == 0) return;

  const uint8_t total = pending;
  uint8_t processed = 0;
  while (pending > 0) {
    uint8_t chunk = pending > FIFO_SAMPLES_PER_READ ? FIFO_SAMPLES_PER_READ : pending;
    uint8_t bytes = chunk * 6;  // modo SpO2: 3 bytes vermelho + 3 bytes IR por amostra

    Wire.beginTransmission(MAX30102_ADDRESS);
    Wire.write(REG_FIFO_DATA);
    if (Wire.endTransmission(false) != 0) return;
    if (Wire.requestFrom((uint8_t)MAX30102_ADDRESS, bytes) != bytes) return;

    for (uint8_t i = 0; i < chunk; i++) {
      uint32_t red = ((uint32_t)Wire.read() << 16) | ((uint32_t)Wire.read() << 8) | Wire.read();
      uint32_t ir = ((uint32_t)Wire.read() << 16) | ((uint32_t)Wire.read() << 8) | Wire.read();
      red &= 0x3FFFF;
      ir &= 0x3FFFF;
      // A amostra mais nova foi medida "agora"; as anteriores, um período antes cada.
      uint32_t sampleMs = now - (uint32_t)(total - 1 - processed) * SAMPLE_PERIOD_MS;
      processOpticalSample(red, ir, sampleMs);
      processed++;
    }
    pending -= chunk;
  }
}

bool setupMax30102() {
  if (!particleSensor.begin(Wire, I2C_FREQUENCY_HZ, MAX30102_ADDRESS)) return false;
  particleSensor.setup(MAX30102_LED_BRIGHTNESS, MAX30102_SAMPLE_AVERAGE, 2 /* vermelho + IR */,
                       MAX30102_SAMPLE_RATE, MAX30102_PULSE_WIDTH, MAX30102_ADC_RANGE);
  particleSensor.setPulseAmplitudeRed(MAX30102_LED_BRIGHTNESS);
  particleSensor.setPulseAmplitudeIR(MAX30102_LED_BRIGHTNESS);
  particleSensor.setPulseAmplitudeGreen(0);
  particleSensor.clearFIFO();
  resetOptical();
  opt.finger = false;
  opt.readErrors = 0;
  return true;
}

// -----------------------------------------------------------------------------
// MAX30205
// -----------------------------------------------------------------------------
static float readMax30205C() {
  Wire.beginTransmission(MAX30205_ADDRESS);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return NAN;
  if (Wire.requestFrom((uint8_t)MAX30205_ADDRESS, (uint8_t)2) != 2) return NAN;
  int16_t raw = (int16_t)((Wire.read() << 8) | Wire.read());
  float celsius = raw / 256.0f;
  return (celsius > -20.0f && celsius < 70.0f) ? celsius : NAN;  // leitura absurda = erro de barramento
}

static void readTemperature(uint32_t now) {
  if (now - lastTempReadMs < TEMP_READ_MS) return;
  lastTempReadMs = now;

  float celsius = readMax30205C();
  if (isfinite(celsius)) {
    lastTempC = celsius;
    lastTempOkMs = now;
    tempErrors = 0;
    if (!gSensors.max30205) Serial.println("[sensor] MAX30205 respondendo.");
    gSensors.max30205 = true;
  } else if (++tempErrors >= 3 && gSensors.max30205) {
    gSensors.max30205 = false;
    Serial.println("[sensor] MAX30205 parou de responder.");
  }
}

// -----------------------------------------------------------------------------
// API usada pelo loop()
// -----------------------------------------------------------------------------
void setupSensors() {
  gSensors.max30102 = setupMax30102();
  Serial.println(gSensors.max30102 ? "[sensor] MAX30102 pronto." : "[sensor] MAX30102 NAO encontrado (0x57).");
  lastTempReadMs = 0;
  readTemperature(millis());
  if (!gSensors.max30205) Serial.printf("[sensor] MAX30205 NAO encontrado (0x%02X).\n", MAX30205_ADDRESS);
  lastSensorRetryMs = millis();
}

/** Chamar a cada volta do loop(): lê os sensores e atualiza gVitals. */
void sensorsTick() {
  uint32_t now = millis();

  if (gSensors.max30102 && now - lastFifoPollMs >= FIFO_POLL_MS) {
    lastFifoPollMs = now;
    readMax30102Fifo(now);
  }
  readTemperature(now);

  // Sensor ausente: tenta de novo de tempos em tempos (fio solto, mau contato).
  if (!gSensors.max30102 && now - lastSensorRetryMs >= SENSOR_RETRY_MS) {
    lastSensorRetryMs = now;
    if (setupMax30102()) {
      gSensors.max30102 = true;
      Serial.println("[sensor] MAX30102 encontrado.");
    }
  }

  // Valores "publicáveis": só o que é válido e recente.
  float heartRate = NAN;
  if (opt.finger && opt.bpmCount >= BPM_MIN_BEATS && opt.lastBeatMs != 0 && now - opt.lastBeatMs <= BEAT_STALE_MS) {
    float total = 0;
    for (uint8_t i = 0; i < opt.bpmCount; i++) total += opt.bpm[i];
    heartRate = total / opt.bpmCount;
  }
  float spo2 = (opt.finger && isfinite(opt.spo2) && now - opt.spo2AtMs <= SPO2_STALE_MS) ? opt.spo2 : NAN;
  float temperature = (gSensors.max30205 && now - lastTempOkMs <= TEMP_STALE_MS) ? lastTempC : NAN;
  bool finger = gSensors.max30102 && opt.finger;

  portENTER_CRITICAL(&gVitalsMux);
  gVitals.heartRateBpm = gSensors.max30102 ? heartRate : NAN;
  gVitals.spo2Pct = gSensors.max30102 ? spo2 : NAN;
  gVitals.bodyTempC = temperature;
  gVitals.fingerDetected = finger;
  gVitals.irValue = opt.ir;
  gVitals.samples = opt.samples;
  gVitals.lastBeatAgeMs = opt.lastBeatMs ? now - opt.lastBeatMs : 0;
  gVitals.fifoOverflows = opt.overflows;
  portEXIT_CRITICAL(&gVitalsMux);
}

/** Durante a coleta, gera 1 amostra consolidada por SNAPSHOT_INTERVAL_MS para a tarefa de rede. */
void snapshotTick() {
  static uint32_t lastSnapshotMs = 0;
  if (!gCollecting || gSnapshotQueue == nullptr) return;

  uint32_t now = millis();
  if (now - lastSnapshotMs < SNAPSHOT_INTERVAL_MS) return;
  lastSnapshotMs = now;

  LiveVitals vitals = readVitals();
  Snapshot snapshot = {now, vitals.heartRateBpm, vitals.spo2Pct, vitals.bodyTempC,
                       vitals.fingerDetected, vitals.lastBeatAgeMs, vitals.samples};

  if (xQueueSend(gSnapshotQueue, &snapshot, 0) != pdTRUE) {
    Snapshot oldest;
    xQueueReceive(gSnapshotQueue, &oldest, 0);  // fila cheia (rede travada): descarta a mais antiga
    xQueueSend(gSnapshotQueue, &snapshot, 0);
    gSnapshotsDropped = gSnapshotsDropped + 1;
  }
}

/** Formata um valor ou "--" se for inválido (NAN). */
const char *formatValue(char *buffer, size_t size, float value, uint8_t decimals) {
  if (isfinite(value)) snprintf(buffer, size, "%.*f", decimals, value);
  else snprintf(buffer, size, "--");
  return buffer;
}

void printSensorDebug() {
#if DEBUG_SENSORS
  static uint32_t lastPrintMs = 0;
  uint32_t now = millis();
  if (now - lastPrintMs < 2000) return;
  lastPrintMs = now;
  LiveVitals v = readVitals();
  char hr[12], spo2[12], temp[12];
  Serial.printf("[sensor] IR=%lu dedo=%s FC=%s SpO2=%s T=%s amostras=%lu perdidas=%lu\n",
                (unsigned long)v.irValue, v.fingerDetected ? "sim" : "nao",
                formatValue(hr, sizeof(hr), v.heartRateBpm, 1), formatValue(spo2, sizeof(spo2), v.spo2Pct, 1),
                formatValue(temp, sizeof(temp), v.bodyTempC, 2), (unsigned long)v.samples,
                (unsigned long)v.fifoOverflows);
#endif
}
