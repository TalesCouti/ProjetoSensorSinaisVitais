// =============================================================================
// VITALSENSE - FIRMWARE PARA O ESP32 FÍSICO (MAX30102 + MAX30205 + OLED)
// =============================================================================
// Versão para hardware real, derivada do firmware validado no Wokwi
// ("Projeto com chips/sketch.ino"). A simulação continua intacta na pasta acima.
//
// O que faz:
//   - conecta no Wi-Fi 2,4 GHz e mantém a conexão (reconexão controlada);
//   - registra o aparelho no backend (Render) e manda heartbeat;
//   - pergunta ao backend, a cada ~3 s, se o app pediu uma coleta ("Sincronizar");
//   - ao receber o pedido, mede por N segundos e envia lotes de leituras;
//   - avisa o fim da coleta (ou a falha, com o motivo);
//   - mostra tudo na tela OLED e numa página local (plano B sem internet).
//
// Organização:
//   config.h          suas configurações (copie de config.example.h; fora do Git)
//   vs_shared.h       estado compartilhado entre as tarefas
//   vs_sensors.h      MAX30102 (FC/SpO2) e MAX30205 (temperatura)
//   vs_display.h      tela OLED
//   vs_backend.h      Wi-Fi + HTTPS com o backend (tarefa no núcleo 0)
//   vs_local_api.h    página/JSON local
//
// Bibliotecas (Arduino IDE > Gerenciar bibliotecas): veja o README.md.
// Placa: "ESP32 Dev Module" (pacote esp32 da Espressif). Monitor serial: 115200.
//
// ⚠️ Protótipo educacional. FC e SpO2 são estimativas sem calibração clínica.
//    Não use para diagnóstico. Não substitui equipamento médico.
// =============================================================================

#if __has_include("config.h")
#include "config.h"
#else
#error "Crie o arquivo config.h: copie config.example.h para config.h e preencha Wi-Fi, URL, DEVICE_ID e token."
#endif

#include <Wire.h>
#include "vs_shared.h"
#include "vs_sensors.h"
#include "vs_display.h"
#include "vs_backend.h"
#include "vs_local_api.h"

static constexpr uint16_t SNAPSHOT_QUEUE_LENGTH = 60;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("==============================================");
  Serial.printf(" VitalSense ESP32 %s  (DEVICE_ID: %s)\n", FIRMWARE_VERSION, DEVICE_ID);
  Serial.println(" Prototipo educacional - nao e dispositivo medico");
  Serial.println("==============================================");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(I2C_FREQUENCY_HZ);
  scanI2cBus();

  setupDisplay();
  setupSensors();

  gSnapshotQueue = xQueueCreate(SNAPSHOT_QUEUE_LENGTH, sizeof(Snapshot));

  startWifi();
  setupLocalApi();

  // Rede no núcleo 0 (o mesmo da pilha Wi-Fi); sensores ficam no loop() (núcleo 1).
  xTaskCreatePinnedToCore(networkTask, "vs_network", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  sensorsTick();      // lê MAX30102 (FIFO) e MAX30205, atualiza os valores
  snapshotTick();     // durante a coleta: 1 amostra/s para a tarefa de rede
  displayTick();      // tela OLED (1x por segundo)
  localApiTick();     // página local
  printSensorDebug(); // monitor serial (a cada 2 s, se DEBUG_SENSORS = 1)
  delay(2);           // cede o processador (a FIFO do MAX30102 é lida a cada 10 ms)
}
