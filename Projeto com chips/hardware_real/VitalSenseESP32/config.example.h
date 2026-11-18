#pragma once
// =============================================================================
// CONFIGURAÇÃO DO FIRMWARE VITALSENSE (HARDWARE REAL) - MODELO
// =============================================================================
// 1. Copie este arquivo para "config.h" (mesma pasta).
// 2. Preencha o config.h. Ele NÃO vai para o Git (.gitignore).
// 3. Nunca coloque senha de Wi-Fi ou token reais neste config.example.h.
// =============================================================================

// -----------------------------------------------------------------------------
// Wi-Fi (rede 2,4 GHz: o ESP32 clássico NÃO conecta em 5 GHz)
// -----------------------------------------------------------------------------
// Pode ser o roteador ou o hotspot do celular, desde que esteja em 2,4 GHz
// (no Android: Ponto de acesso > Banda > 2,4 GHz; no iPhone: "Maximizar
// compatibilidade") e tenha internet.
#define WIFI_SSID     "NOME_DA_REDE_2_4GHZ"
#define WIFI_PASSWORD "SENHA_DA_REDE"

// -----------------------------------------------------------------------------
// Backend (Render) - URL base terminando em /api/v1, com https://
// -----------------------------------------------------------------------------
#define API_BASE_URL "https://SEU-SERVICO.onrender.com/api/v1"

// -----------------------------------------------------------------------------
// Identificação do aparelho
// -----------------------------------------------------------------------------
// DEVICE_ID: letras, números, ".", "_" ou "-" (3 a 64). É o que você digita
// no app em Configurações > Sincronização ESP32 para vincular o aparelho.
#define DEVICE_ID "esp32-vitals-01"

// DEVICE_TOKEN: use UM dos dois:
//   a) o mesmo valor da variável ESP32_DEVICE_TOKEN do Render (o ESP32 se
//      registra sozinho e você só vincula no app pelo DEVICE_ID); ou
//   b) o "token individual" que o app mostra ao vincular o ESP32.
#define DEVICE_TOKEN "COLOQUE_O_TOKEN_AQUI"

// Modelo da placa (só informativo; aparece no app e no banco).
#define BOARD_MODEL "ESP32 DevKit (ESP32-WROOM-32)"

// -----------------------------------------------------------------------------
// HTTPS
// -----------------------------------------------------------------------------
// Sem ROOT_CA_PEM a conexão é criptografada, mas o ESP32 NÃO confere o
// certificado do servidor (setInsecure). Para conferir, tire o bloco abaixo
// do comentário e cole o certificado RAIZ (PEM) da autoridade que assina o
// seu domínio .onrender.com (veja o README, seção HTTPS).
/*
#define ROOT_CA_PEM \
  "-----BEGIN CERTIFICATE-----\n" \
  "...\n" \
  "-----END CERTIFICATE-----\n"
*/

// -----------------------------------------------------------------------------
// Ligações I2C (todos os módulos no mesmo barramento, alimentados em 3,3 V)
// -----------------------------------------------------------------------------
// ESP32 DevKit clássico: SDA = GPIO 21, SCL = GPIO 22 (mesmo do Wokwi).
// Se a sua placa for outra (ESP32-C3, S3...), confira os pinos e mude aqui.
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define I2C_FREQUENCY_HZ 400000

// Endereços I2C. O firmware mostra no monitor serial tudo o que encontrou.
#define MAX30102_ADDRESS 0x57   // fixo no MAX30102
#define MAX30205_ADDRESS 0x48   // 0x48 a 0x4F conforme os pinos A0-A2 do módulo
#define OLED_ADDRESS     0x3C   // alguns módulos usam 0x3D
#define OLED_ENABLED     1      // 0 se o circuito não tiver a tela

// -----------------------------------------------------------------------------
// MAX30102 (mesmos valores validados no Wokwi: 100 Hz com média de 4 = 25 amostras/s)
// -----------------------------------------------------------------------------
#define MAX30102_LED_BRIGHTNESS 0x24  // corrente dos LEDs (0x00-0xFF; 0x24 ~ 7 mA)
#define MAX30102_SAMPLE_AVERAGE 4     // 1, 2, 4, 8, 16, 32
#define MAX30102_SAMPLE_RATE    100   // 50, 100, 200, 400...
#define MAX30102_PULSE_WIDTH    411   // 69, 118, 215, 411
#define MAX30102_ADC_RANGE      4096  // 2048, 4096, 8192, 16384

// Acima deste valor de infravermelho considera-se que há dedo no sensor.
// Varia com o módulo, o dedo e a corrente dos LEDs: ajuste olhando o valor
// "IR=" que o monitor serial mostra com e sem o dedo.
#define FINGER_IR_THRESHOLD 20000

// -----------------------------------------------------------------------------
// Coleta e comunicação
// -----------------------------------------------------------------------------
#define SNAPSHOT_INTERVAL_MS 1000   // uma amostra consolidada por segundo durante a coleta
#define BATCH_INTERVAL_MS    3000   // envia um lote ao backend a cada 3 s
#define HTTP_TIMEOUT_MS      30000  // o Render gratuito pode levar ~1 min para "acordar"

// Página/JSON local no IP do ESP32 (plano B sem internet). 0 = desligado.
#define ENABLE_LOCAL_API 1

// Se nunca conectar no Wi-Fi, cria uma rede própria para ver a página local.
// Fica desligado se AP_PASSWORD tiver menos de 8 caracteres.
#define ENABLE_AP_FALLBACK 1
#define AP_PASSWORD ""

// Mostra no monitor serial IR, FC, SpO2 e temperatura a cada 2 s.
#define DEBUG_SENSORS 1
