#pragma once
// =============================================================================
// TELA OLED SSD1306 128x64 (opcional). Mostra os valores atuais e a situação
// da rede/coleta. Se a tela não existir, o firmware funciona normalmente.
// =============================================================================
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "vs_shared.h"

// clkDuring/clkAfter = 400 kHz: a biblioteca não derruba o I2C para 100 kHz
// depois de cada atualização (o MAX30102 continua sendo lido rápido).
static Adafruit_SSD1306 display(128, 64, &Wire, -1, 400000UL, 400000UL);

void setupDisplay() {
#if OLED_ENABLED
  // periphBegin = false: não reinicia o Wire (mantém os pinos do config.h).
  gSensors.oled = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, true, false);
  if (!gSensors.oled) {
    Serial.printf("[oled] Tela nao encontrada (0x%02X). Seguindo sem ela.\n", OLED_ADDRESS);
    return;
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("VitalSense");
  display.println("Iniciando...");
  display.println();
  display.println(DEVICE_ID);
  display.display();
#endif
}

void displayTick() {
#if OLED_ENABLED
  static uint32_t lastDrawMs = 0;
  if (!gSensors.oled) return;
  uint32_t now = millis();
  if (now - lastDrawMs < 1000) return;
  lastDrawMs = now;

  LiveVitals v = readVitals();
  char hr[12], spo2[12], temp[12];

  display.clearDisplay();
  display.setCursor(0, 0);
  display.print("VitalSense ");
  if (gNet.wifiConnected) display.printf("%ddBm", gNet.rssi);
  else display.print("sem WiFi");
  display.println();
  display.println(gNet.wifiConnected ? gNet.ip : (gNet.apActive ? "AP 192.168.4.1" : "-"));
  display.printf("FC  %s bpm  dedo:%s\n", formatValue(hr, sizeof(hr), v.heartRateBpm, 0), v.fingerDetected ? "S" : "N");
  display.printf("SpO2 %s %%\n", formatValue(spo2, sizeof(spo2), v.spo2Pct, 1));
  display.printf("Temp %s C\n", formatValue(temp, sizeof(temp), v.bodyTempC, 2));
  display.printf("Sens: %s %s\n", gSensors.max30102 ? "HR:ok" : "HR:--", gSensors.max30205 ? "T:ok" : "T:--");

  if (gCollecting) {
    int32_t remaining = (int32_t)(gCollectionEndMs - now);
    display.printf("COLETANDO %lds\n", (long)(remaining > 0 ? (remaining + 999) / 1000 : 0));
  } else {
    display.println(gNet.message);
  }
  if (!gNet.claimed && gNet.backendOk) display.print(DEVICE_ID);
  display.display();
#endif
}
