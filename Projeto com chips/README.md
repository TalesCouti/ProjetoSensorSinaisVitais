# Projeto com chips (simulação no Wokwi)

Versão do firmware para rodar no **[Wokwi](https://wokwi.com)** com os sensores
**MAX30102** (batimentos e SpO2) e **MAX30205** (temperatura corporal)
simulados por **chips personalizados** (*custom chips*), já que o Wokwi não tem
essas peças na lista de componentes.

O firmware conversa com os chips exatamente como conversaria com os sensores
reais (mesmos endereços I2C, mesmos registradores). A diferença é só de onde
vêm os números: dos controles deslizantes dos chips, e não de uma pessoa.

> ⚠️ Projeto educacional/protótipo. Não é um dispositivo médico.

---

## Arquivos

| Arquivo | O que é |
|---|---|
| `sketch.ino` | firmware do ESP32 (leitura dos sensores, tela OLED, API local e envio para o backend) |
| `diagram.json` | circuito: ESP32 + OLED + os dois chips, ligados no barramento I2C |
| `libraries.txt` | bibliotecas que o Wokwi instala |
| `max30102.chip.json` | aparência do chip MAX30102: pinos e controles (BPM, SpO2, dedo) |
| `max30102.chip.c` | comportamento do chip MAX30102: registradores, FIFO e onda de pulso |
| `max30205.chip.json` | aparência do chip MAX30205: pinos e controle de temperatura |
| `max30205.chip.c` | comportamento do chip MAX30205: registrador de temperatura |

---

## Circuito

Todos os módulos ficam no mesmo barramento I2C, como no circuito real:

| ESP32 | OLED SSD1306 (0x3C) | MAX30102 (0x57) | MAX30205 (0x48) |
|---|---|---|---|
| 3V3 | VCC | VIN | VCC |
| GND | GND | GND | GND |
| GPIO 21 | SDA | SDA | SDA |
| GPIO 22 | SCL | SCL | SCL |

---

## Como montar no Wokwi

1. Crie um projeto novo: **wokwi.com → New Project → ESP32**.
2. Para cada arquivo desta pasta, crie um arquivo com o **mesmo nome** no
   Wokwi (seta ▾ ao lado das abas → **New file...**) e cole o conteúdo.
   Os arquivos que já existem (`sketch.ino`, `diagram.json`) é só substituir o
   conteúdo.
3. Os nomes dos chips precisam terminar em `.chip.json` e `.chip.c`. O
   `diagram.json` usa os tipos `chip-max30102` e `chip-max30205`, que vêm
   desses nomes.
4. Clique em ▶ (a primeira compilação demora: o Wokwi compila o firmware e os
   dois chips).

### Resultado esperado

Monitor serial:
```
MAX30205 simulado pronto no endereco 0x48
MAX30102 simulado pronto no endereco 0x57
Inicializando sensor de sinais vitais...
MAX30102 pronto.
Conectando ao WiFi......
WiFi conectado. IP: 10.10.0.2
```

Tela OLED: `Dedo: sim`, `BPM: 75.0`, `SpO2: 98.0%`, `Temp: 36.50 C`.

---

## Controles dos chips

Com a simulação rodando, **clique no chip** para abrir os controles:

| Chip | Controle | Faixa | Padrão |
|---|---|---|---|
| MAX30102 | Batimentos (BPM) | 40 a 180 | 75 |
| MAX30102 | SpO2 (%) | 80 a 100 | 98 |
| MAX30102 | Dedo no sensor | 0 = não, 1 = sim | 1 |
| MAX30205 | Temperatura corporal (°C) | 30 a 43 | 36,5 |

O BPM leva alguns segundos para aparecer (precisa de pelo menos duas batidas),
e o SpO2 leva cerca de 4 s (janela de 100 amostras). Acima de ~120 BPM a
detecção da biblioteca SparkFun começa a falhar, porque ela foi feita para
batimentos em repouso.

---

## Como os chips funcionam

### MAX30205 (temperatura)
- Responde no endereço **0x48**.
- O registrador `0x00` devolve a temperatura em 16 bits, onde 1 bit = 1/256 °C.
  Exemplo: 36,5 °C × 256 = 9344, enviado como `0x24` `0x80`.
- O firmware faz a conta inversa: `9344 / 256 = 36,5`.

### MAX30102 (batimentos e SpO2)
- Responde no endereço **0x57** e se identifica com `PART_ID = 0x15`, que é o
  que a biblioteca SparkFun confere no `begin()`.
- Gera **25 amostras por segundo** (100 Hz com média de 4, conforme o firmware
  configura) e guarda numa **FIFO de 32 posições**, igual ao sensor real.
- Cada amostra é uma onda senoidal:
  - **Infravermelho:** `50000 + 250 × sen(fase)`. A fase dá uma volta
    completa a cada batida, conforme o controle de BPM.
  - **Vermelho:** a oscilação é calculada para que a fórmula do firmware
    (`SpO2 = 110 − 25 × R`) dê exatamente o valor do controle de SpO2.
- **Sem dedo:** valores baixos (~800) com ruído, abaixo do
  `FINGER_IR_THRESHOLD` (20000), então o firmware mostra "Dedo: nao".

---

## Diferenças em relação ao `src/main.cpp`

Este `sketch.ino` parte do `src/main.cpp` do repositório, com estas mudanças:

1. **Correção de compilação:** `String(value, decimals)` → `String(value, (unsigned int)decimals)`.
   No ESP32 Arduino 3.x a forma original é ambígua e não compila.
2. **Correção do BPM:** o código original chamava `getIR()` e depois `getRed()`,
   e cada chamada consumia uma amostra nova. Com isso, o `checkForBeat` recebia
   só metade das amostras (12,5 Hz), e o filtro dele praticamente apagava o
   pulso: o BPM ficava sempre `null`. Agora a FIFO é lida amostra por amostra
   (`check()` / `available()` / `getFIFORed()` / `getFIFOIR()` / `nextSample()`),
   e cada amostra recebe o horário estimado em que foi medida. **Essa correção
   vale também para o circuito real.**
3. **Conexão com o backend VitalSense:**
   - envia as leituras para `POST /api/v1/device/readings`;
   - pergunta a cada 3 s em `GET /api/v1/device/commands/next` se o app pediu
     uma medição (botão "Iniciar");
   - ao receber o pedido, mede por N segundos, enviando a cada 2 s, e avisa o
     fim em `POST /api/v1/device/measurements/:id/complete`.
4. **Tela OLED:** mostra `MEDINDO... 28s` durante uma medição do app e o último
   código HTTP do servidor (`Nuvem: HTTP 201`).
5. Wi-Fi padrão `Wokwi-GUEST` (rede da simulação, com acesso à internet).

### Conectar ao backend (opcional)

Sem configuração, o firmware funciona normalmente e a tela mostra
`API nao configurada`. Para ligar ao backend, preencha no começo do `sketch.ino`:

```cpp
#define API_BASE_URL "https://SEU-SERVICO.onrender.com/api/v1"
#define API_TOKEN "TOKEN_GERADO_AO_CADASTRAR_O_ESP32"
```

- O `DEVICE_ID` (`esp32-vitals-01`) precisa ser igual ao cadastrado no backend.
- O Wokwi roda na nuvem: ele **não** acessa o `localhost` do seu computador, então
  use a URL publicada do backend.
- Projetos do Wokwi podem ser vistos por quem tiver o link. Não deixe um token
  real num projeto público; se vazar, gere outro no backend.

---

## Bibliotecas

| Biblioteca | Para quê |
|---|---|
| Adafruit GFX Library | desenho de texto na tela |
| Adafruit SSD1306 | tela OLED |
| SparkFun MAX3010x Pulse and Proximity Sensor Library | MAX30102 (`MAX30105.h` e `heartRate.h`) |
| ArduinoJson | ler as respostas do backend |

O MAX30205 é lido direto pelo I2C (`Wire`), sem biblioteca. As bibliotecas
`DevLab_MAX30102` e `MAX30205` não são usadas por este firmware.

---

## Problemas comuns

| Problema | Solução |
|---|---|
| `MAX30102 nao encontrado` | confira se os arquivos se chamam exatamente `max30102.chip.json` e `max30102.chip.c` |
| BPM `null` | controle "Dedo no sensor" em 1; aguarde alguns segundos |
| Temperatura `null` | confira o chip MAX30205 e os fios SDA/SCL |
| Erro de compilação do chip | o `.chip.c` precisa ter o código em C (não o JSON) |
| `API nao configurada` na tela | normal sem backend; preencha `API_BASE_URL` e `API_TOKEN` para conectar |
| `Nuvem: HTTP -1` ou `-11` | sem conexão ou servidor "acordando" (plano gratuito do Render); aguarde ~1 min |
