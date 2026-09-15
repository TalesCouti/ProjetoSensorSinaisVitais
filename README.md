# Sensor de Sinais Vitais com ESP32

Firmware para `ESP32 DevKit` com `MAX30102`, `MAX30205` e OLED `SSD1306 0.96"` via I2C. O ESP32 lê batimento, estimativa de SpO2 e temperatura, mostra no display e expõe uma API HTTP local.

> Este projeto é educacional/prototipagem. A estimativa de SpO2 e batimentos com módulos de baixo custo exige calibração, boa fixação do dedo e validação contra equipamento confiável. Não use como dispositivo médico.

## Ligações I2C

Todos os módulos ficam no mesmo barramento I2C:

| ESP32 DevKit | MAX30102 | MAX30205 | SSD1306 |
| --- | --- | --- | --- |
| 3V3 | VIN/VCC | VIN/VCC | VCC |
| GND | GND | GND | GND |
| GPIO 21 | SDA | SDA | SDA |
| GPIO 22 | SCL | SCL | SCL |

Use 3.3 V. Evite 5 V nesses módulos se a sua placa não tiver conversor/regulador compatível.

## Como compilar

1. Instale o PlatformIO.
2. Copie `include/config.example.h` para `include/config.h`.
3. Preencha `WIFI_SSID` e `WIFI_PASS`, se quiser conectar o ESP32 ao seu roteador.
4. Compile e grave:

```bash
pio run -t upload
pio device monitor
```

Se `WIFI_SSID` ficar vazio ou a conexão falhar, o ESP32 cria um Access Point:

- SSID: `VitalSensor-ESP32`
- Senha: `12345678`
- API: `http://192.168.4.1/api/v1/vitals`

## Endpoints

```http
GET /api/v1/health
GET /api/v1/vitals
```

Exemplo de resposta:

```json
{
  "deviceId": "esp32-vitals-01",
  "uptimeMs": 123456,
  "ip": "192.168.1.50",
  "wifiMode": "station",
  "measurements": {
    "heartRateBpm": 72.4,
    "spo2Pct": 97.2,
    "bodyTempC": 36.58,
    "fingerDetected": true
  },
  "sensors": {
    "max30102": true,
    "max30205": true,
    "oled": true
  },
  "quality": {
    "max30102Samples": 4250,
    "lastBeatAgeMs": 220,
    "spo2WindowSamples": 63
  },
  "push": {
    "enabled": false,
    "lastHttpCode": 0
  }
}
```

## Envio para uma API remota

No `include/config.h`, defina:

```cpp
#define API_PUSH_URL "https://api.seudominio.com/vitals"
#define API_TOKEN "seu-token-opcional"
```

O ESP32 fará `POST` com o mesmo JSON de `/api/v1/vitals` a cada `API_PUSH_INTERVAL_MS`.

## Ajustes comuns

- OLED sem imagem: tente `#define OLED_ADDRESS 0x3D`.
- Temperatura não aparece: confirme se o seu MAX30205 usa `0x48`.
- Não detecta dedo: ajuste `FINGER_IR_THRESHOLD`.
- Leitura instável: reduza luz ambiente, mantenha o dedo parado e faça média no backend.

