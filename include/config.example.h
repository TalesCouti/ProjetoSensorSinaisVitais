#pragma once

// Copie este arquivo para include/config.h e ajuste os valores.

#define DEVICE_ID "esp32-vitals-01"

// Se WIFI_SSID ficar vazio, o ESP32 abre um Access Point:
// SSID: VitalSensor-ESP32
// senha: 12345678
#define WIFI_SSID ""
#define WIFI_PASS ""

// Opcional: URL de um backend para receber POST com as leituras.
// Exemplo: "https://api.seudominio.com/vitals"
#define API_PUSH_URL ""
#define API_TOKEN ""

// Ajustes de hardware comuns no ESP32 DevKit.
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

// Ajuste se o seu módulo MAX30205 usar outro endereço.
#define MAX30205_ADDRESS 0x48

// Ajuste se o OLED usar 0x3D.
#define OLED_ADDRESS 0x3C

// Limite básico para considerar que há dedo no MAX30102.
// Pode variar bastante conforme módulo, dedo e corrente dos LEDs.
#define FINGER_IR_THRESHOLD 20000

// Intervalo do envio opcional para API remota.
#define API_PUSH_INTERVAL_MS 10000

