# VitalSense — firmware para o ESP32 físico (hardware real)

Versão do firmware para **gravar na placa real**, separada da simulação do Wokwi
(que continua intacta em `../sketch.ino`, `../diagram.json` e `../*.chip.*`).

> ⚠️ **Protótipo educacional.** Frequência cardíaca e SpO₂ são **estimativas**
> de um sensor óptico de baixo custo, sem calibração clínica. A temperatura é a
> do ponto de contato do MAX30205. **Não use para diagnóstico. Não substitui
> equipamento médico.**

---

## 1. O que este firmware faz

```
ESP32 liga ──► Wi-Fi 2,4 GHz ──► POST /device/register   (versão, placa, sensores)
              a cada 30 s    ──► POST /device/heartbeat  (online, sinal Wi-Fi, sensores)
              a cada ~3 s    ──► GET  /device/commands/next  ("o app pediu coleta?")

App toca "Sincronizar" ─► backend cria a coleta ─► ESP32 recebe no próximo polling
ESP32 mede 30 s (1 amostra/s) ──► POST /device/readings (lote a cada 3 s)
ESP32 termina ──► POST /device/measurements/:id/complete  (completed | failed + motivo)
```

- O ESP32 **inicia todas as conexões** (polling). Por isso funciona atrás do
  roteador ou do hotspot do celular, sem abrir portas: o backend no Render não
  precisa (e não consegue) "chamar" o ESP32.
- **Duas tarefas** (FreeRTOS): rede no núcleo 0 e sensores no núcleo 1. Um HTTPS
  lento (1 a 30 s) não interrompe a leitura do MAX30102.
- **Nada é inventado:** sem dedo → FC e SpO₂ vão como `null`; sensor ausente → a
  coleta termina como `failed` com o motivo; SpO₂ fora de 70–100 % é descartada.
- Os dados saem marcados como `dataSource: "hardware"` (o app mostra "Sensor físico").
- Tela OLED com valores e status; página local `http://<IP do ESP32>/` (plano B).

### Diferenças em relação ao Wokwi

| Item | Wokwi (`../sketch.ino`) | Hardware real (este) |
|---|---|---|
| Sensores | chips simulados (controles deslizantes) | MAX30102 e MAX30205 reais |
| Leitura da FIFO do MAX30102 | via buffer da SparkFun (guarda só 4 amostras) | direto pelos registradores, até 32 amostras por vez |
| Rede × sensores | mesmo `loop()` (HTTP trava a leitura) | tarefas separadas (núcleo 0 / núcleo 1) |
| Envio | 1 leitura a cada 2 s | lote de amostras a cada 3 s, com `ageMs` de cada uma |
| SpO₂ fora da faixa | forçada para 70 ou 100 (`constrain`) | descartada (inválida) |
| FC | 1ª batida já gera valor | exige ≥ 3 batidas recentes |
| Wi-Fi | `Wokwi-GUEST` | seu Wi-Fi 2,4 GHz, com reconexão controlada |
| Origem dos dados | `simulation` | `hardware` |
| Sensor desconectado | não tenta de novo | procura de novo a cada 10 s |

---

## 2. Material e ligações

Placa considerada: **ESP32 DevKit clássico (ESP32-WROOM-32)**, a mesma família
do Wokwi (`board-esp32-devkit-c-v4`) e do PlatformIO original (`esp32dev`).

> ❓ **Confirme a sua placa.** Os arquivos do projeto indicam ESP32 DevKit com
> I²C em GPIO 21/22. Se a sua for ESP32-C3, S3 ou outra, os pinos podem ser
> diferentes: ajuste `I2C_SDA_PIN` / `I2C_SCL_PIN` no `config.h`.

Todos os módulos no **mesmo barramento I²C**, alimentados em **3,3 V**:

| ESP32 DevKit | MAX30102 | MAX30205 | OLED SSD1306 |
|---|---|---|---|
| 3V3 | VIN | VCC | VCC |
| GND | GND | GND | GND |
| GPIO 21 (SDA) | SDA | SDA | SDA |
| GPIO 22 (SCL) | SCL | SCL | SCL |

Endereços I²C: MAX30102 `0x57` (fixo), MAX30205 `0x48` (pode ir até `0x4F`
conforme os pinos A0–A2 do módulo), OLED `0x3C` (alguns `0x3D`).
**Ao ligar, o firmware lista no monitor serial todos os endereços encontrados**:
use isso para conferir as ligações.

Cuidados com módulos reais:
- Use 3,3 V. Muitos módulos MAX30102 baratos têm resistores de pull-up para
  1,8 V ou 3,3 V; se o ESP32 não enxergar o `0x57`, confira o jumper/solda de
  tensão do módulo.
- Fios curtos na protoboard; I²C a 400 kHz é sensível a mau contato. Se
  aparecerem falhas, reduza `I2C_FREQUENCY_HZ` para `100000`.
- O MAX30205 mede a temperatura **de onde está encostado**. Longe da pele, ele
  mede o ambiente (ex.: 25 °C), e isso é uma leitura real, não corporal.

---

## 3. Configuração (`config.h`)

1. Nesta pasta `VitalSenseESP32/`, copie `config.example.h` para **`config.h`**.
2. Preencha:

| Campo | O que colocar |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | sua rede **2,4 GHz** (roteador ou hotspot do celular) |
| `API_BASE_URL` | `https://vitalsense-backend-6mwv.onrender.com/api/v1` (ou o seu serviço) |
| `DEVICE_ID` | ex.: `esp32-vitals-01` (é o que você digita no app para vincular) |
| `DEVICE_TOKEN` | o valor de `ESP32_DEVICE_TOKEN` do Render **ou** o token individual mostrado pelo app |

O `config.h` está no `.gitignore`: **senha do Wi-Fi e token nunca vão para o Git.**
Se o token vazar, troque o `ESP32_DEVICE_TOKEN` no Render (e no `config.h`).

### Qual Wi-Fi usar?

- O ESP32 clássico só conecta em **2,4 GHz** (não em 5 GHz).
- **Hotspot do celular funciona**, desde que esteja em 2,4 GHz e tenha internet:
  - Android: *Ponto de acesso → Banda AP → 2,4 GHz*;
  - iPhone: *Acesso Pessoal → ativar "Maximizar Compatibilidade"*.
- O ESP32 **não precisa estar na mesma rede do celular** que roda o app: os dois
  só precisam de internet, porque conversam pelo backend no Render (HTTPS).
- Redes com página de login (Wi-Fi de faculdade/hotel com "captive portal") ou
  WPA2-Enterprise **não funcionam**. Prefira o hotspot do celular na apresentação.

---

## 4. Bibliotecas

Arduino IDE → *Ferramentas → Gerenciar bibliotecas*, instale:

| Biblioteca | Autor | Para quê |
|---|---|---|
| **SparkFun MAX3010x Pulse and Proximity Sensor Library** | SparkFun | configurar o MAX30102 e detectar batidas (`heartRate.h`) |
| **Adafruit SSD1306** | Adafruit | tela OLED |
| **Adafruit GFX Library** | Adafruit | texto na tela |
| **ArduinoJson** (versão **7.x**) | Benoit Blanchon | JSON do backend |

`WiFi`, `HTTPClient`, `WiFiClientSecure`, `WebServer` e `Wire` já vêm com o
pacote do ESP32. O MAX30205 é lido direto pelo I²C (sem biblioteca).

Placa: *Ferramentas → Placa → esp32 → **ESP32 Dev Module*** (instale antes o
pacote **esp32 by Espressif Systems** em *Gerenciador de placas*).

---

## 5. Compilar e gravar

### Arduino IDE
1. *Arquivo → Abrir* → `VitalSenseESP32/VitalSenseESP32.ino`.
2. Confira que o `config.h` existe na mesma pasta (abas no topo da IDE).
3. Placa **ESP32 Dev Module**, porta COM do ESP32.
4. **Carregar** (→). Se travar em "Connecting...", segure o botão **BOOT** da placa.
5. *Monitor serial* em **115200**.

**Compilação verificada** (PlatformIO, core Arduino-ESP32 3.3.12, placa `esp32dev`,
SparkFun MAX3010x 1.1.2, Adafruit SSD1306 2.5.17, Adafruit GFX 1.13.0,
ArduinoJson 7.4.3): RAM 16,7 %, Flash 83,9 % da partição padrão. Se um dia
faltar espaço, use *Ferramentas → Partition Scheme → Huge APP (3MB No OTA)*.
O teste foi de compilação: o funcionamento com os sensores reais ainda precisa
ser conferido na placa (monitor serial).

### PlatformIO (opcional)
```bash
cd "Projeto com chips/hardware_real"
pio run -t upload
pio device monitor
```

### Saída esperada no monitor serial
```
 VitalSense ESP32 2.0.0-hw  (DEVICE_ID: esp32-vitals-01)
[i2c]   0x3C (OLED)
[i2c]   0x48 (MAX30205)
[i2c]   0x57 (MAX30102)
[sensor] MAX30102 pronto.
[wifi] Conectado. IP 192.168.43.120, sinal -58 dBm.
[api] Registrado no backend como esp32-vitals-01. Ainda NAO vinculado: no app, Configuracoes > Sincronizacao ESP32.
[sensor] IR=104532 dedo=sim FC=74.2 SpO2=97.3 T=33.81 ...
[coleta] Iniciada pelo app: 6f1c... (30 s). Coloque o dedo no sensor.
[coleta] Lote de 3 -> HTTP 201 (gravadas 3, rejeitadas 0)
[coleta] Finalizada -> HTTP 200 (completed). Amostras: 30, validas: 30.
```

---

## 6. HTTPS

Por padrão a conexão é **criptografada** mas o ESP32 **não confere o
certificado** do servidor (`setInsecure()`), como no Wokwi. Para conferir:

1. No navegador, abra `https://vitalsense-backend-6mwv.onrender.com/health`,
   clique no cadeado → certificado → aba de **hierarquia** → selecione o
   certificado **raiz** (o do topo) → exportar em **Base64/PEM**.
2. No `config.h`, tire o bloco `#define ROOT_CA_PEM` de dentro do comentário
   `/* ... */` e cole o conteúdo (cada linha entre aspas terminando em `\n`).
3. Se o Render trocar de autoridade certificadora, a conexão passa a falhar
   (`HTTP -1`): exporte o novo certificado raiz.

---

## 7. Problemas comuns

| Sintoma no serial / OLED | Causa provável | O que fazer |
|---|---|---|
| `Rede nao encontrada ... 2,4 GHz?` | nome errado ou rede em 5 GHz | confira `WIFI_SSID`; mude o hotspot para 2,4 GHz |
| `Falha de autenticacao` | senha errada | confira `WIFI_PASSWORD` |
| `Servidor acordando` / `tempo esgotado` | Render gratuito dormindo | aguarde ~1 min; o firmware tenta de novo sozinho |
| `token recusado (401)` | `DEVICE_TOKEN` diferente do Render, ou token individual trocado | confira o `ESP32_DEVICE_TOKEN` no Render / gere o token de novo no app |
| `Vincule no app:` + DEVICE_ID | ESP32 registrado, mas sem conta | no app: Configurações → Sincronização ESP32 → digite o DEVICE_ID |
| `MAX30102 NAO encontrado (0x57)` | ligação, tensão ou módulo | confira 3V3/GND/SDA/SCL e o scan I²C; o firmware tenta de novo a cada 10 s |
| `MAX30205 NAO encontrado` + aparece `0x49`…`0x4F` no scan | endereço diferente | ajuste `MAX30205_ADDRESS` |
| `dedo=nao` com o dedo no sensor | limiar alto para o seu módulo | veja o `IR=` no serial e ajuste `FINGER_IR_THRESHOLD` |
| FC `--` com dedo | menos de 3 batidas detectadas, movimento, pressão demais | dedo parado, pressão leve, ambiente sem luz forte, aguarde ~10 s |
| SpO₂ `--` | janela de 4 s ainda incompleta ou sinal ruim | mantenha o dedo parado; luz forte e movimento atrapalham |
| Coleta `failed: Dedo nao detectado...` | ninguém pôs o dedo durante os 30 s | repita a coleta com o dedo no sensor |

Se a FC ficar instável com o seu módulo, um ajuste comum (não validado neste
projeto) é usar a configuração do exemplo da SparkFun: `MAX30102_SAMPLE_RATE 400`
com `MAX30102_SAMPLE_AVERAGE 4` (100 amostras/s). Teste e compare antes da
apresentação.

---

## 8. Limitações conhecidas

- SpO₂ calculada pela aproximação linear `110 − 25·R`, **sem calibração** contra
  oxímetro de referência: serve para demonstrar a técnica, não para medir.
- O algoritmo de batimentos da SparkFun foi feito para repouso; acima de ~120 bpm
  e com movimento ele falha.
- Sem relógio (RTC/NTP): o horário de cada amostra é calculado pelo servidor a
  partir do `ageMs` enviado.
- Se a internet cair por mais de ~2 min durante a coleta, as amostras mais
  antigas são descartadas (memória limitada).
- MPU6050 (movimento) ainda não montado.
