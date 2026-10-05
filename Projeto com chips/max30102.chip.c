// Chip simulado do MAX30102 (oxímetro e batimentos) para o Wokwi.
// Responde no endereço I2C 0x57, com os mesmos registradores usados pela
// biblioteca SparkFun MAX3010x: ID da peça, configuração e a FIFO de amostras.
// Gera uma onda de pulso (vermelho + infravermelho) conforme os controles:
// batimentos, SpO2 e dedo no sensor.

#include "wokwi-api.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX30102_ADDRESS 0x57
#define FIFO_DEPTH 32
#define TWO_PI 6.283185307179586

#define REG_FIFO_WR_PTR 0x04
#define REG_OVF_COUNTER 0x05
#define REG_FIFO_RD_PTR 0x06
#define REG_FIFO_DATA 0x07
#define REG_FIFO_CONFIG 0x08
#define REG_MODE_CONFIG 0x09
#define REG_SPO2_CONFIG 0x0A
#define REG_REVISION_ID 0xFE
#define REG_PART_ID 0xFF

#define MODE_RESET 0x40
#define MODE_HEART_RATE 0x02

// Nível médio (DC) e oscilação (AC) do sinal com o dedo no sensor.
// A oscilação do infravermelho fica dentro da faixa que o checkForBeat aceita.
#define IR_DC 50000.0
#define IR_AC 250.0
#define RED_DC 40000.0

typedef struct {
  uint32_t red;
  uint32_t ir;
} sample_t;

typedef struct {
  uint8_t regs[256];
  uint8_t reg_pointer;
  bool expecting_pointer;
  sample_t fifo[FIFO_DEPTH];
  uint8_t fifo_byte_index;
  double phase;
  uint32_t sample_timer;
  uint32_t bpm_attr;
  uint32_t spo2_attr;
  uint32_t finger_attr;
} chip_state_t;

static bool on_i2c_connect(void *user_data, uint32_t address, bool read);
static uint8_t on_i2c_read(void *user_data);
static bool on_i2c_write(void *user_data, uint8_t data);
static void on_i2c_disconnect(void *user_data);
static void on_sample_timer(void *user_data);

static void reset_registers(chip_state_t *chip) {
  memset(chip->regs, 0, sizeof(chip->regs));
  chip->regs[REG_REVISION_ID] = 0x03;
  chip->regs[REG_PART_ID] = 0x15;
  chip->fifo_byte_index = 0;
}

static double effective_sample_rate(chip_state_t *chip) {
  static const double rates[] = {50, 100, 200, 400, 800, 1000, 1600, 3200};
  double rate = rates[(chip->regs[REG_SPO2_CONFIG] >> 2) & 0x07];
  uint8_t avg_bits = (chip->regs[REG_FIFO_CONFIG] >> 5) & 0x07;
  if (avg_bits > 5) {
    avg_bits = 5;
  }
  return rate / (double)(1 << avg_bits);
}

static uint8_t bytes_per_sample(chip_state_t *chip) {
  return (chip->regs[REG_MODE_CONFIG] & 0x07) == MODE_HEART_RATE ? 3 : 6;
}

static uint8_t fifo_count(chip_state_t *chip) {
  return (chip->regs[REG_FIFO_WR_PTR] - chip->regs[REG_FIFO_RD_PTR]) & (FIFO_DEPTH - 1);
}

void chip_init(void) {
  chip_state_t *chip = malloc(sizeof(chip_state_t));
  memset(chip, 0, sizeof(chip_state_t));
  reset_registers(chip);

  chip->bpm_attr = attr_init_float("bpm", 75);
  chip->spo2_attr = attr_init_float("spo2", 98);
  chip->finger_attr = attr_init_float("finger", 1);

  const i2c_config_t i2c_config = {
    .user_data = chip,
    .address = MAX30102_ADDRESS,
    .scl = pin_init("SCL", INPUT),
    .sda = pin_init("SDA", INPUT),
    .connect = on_i2c_connect,
    .read = on_i2c_read,
    .write = on_i2c_write,
    .disconnect = on_i2c_disconnect,
  };
  i2c_init(&i2c_config);

  const timer_config_t timer_config = {
    .callback = on_sample_timer,
    .user_data = chip,
  };
  chip->sample_timer = timer_init(&timer_config);
  timer_start(chip->sample_timer, 20000, false);

  printf("MAX30102 simulado pronto no endereco 0x57\n");
}

static void on_sample_timer(void *user_data) {
  chip_state_t *chip = user_data;
  double rate = effective_sample_rate(chip);

  float bpm = attr_read_float(chip->bpm_attr);
  float spo2 = attr_read_float(chip->spo2_attr);
  bool finger = attr_read_float(chip->finger_attr) >= 0.5f;

  chip->phase += TWO_PI * (bpm / 60.0) / rate;
  if (chip->phase >= TWO_PI) {
    chip->phase -= TWO_PI;
  }

  sample_t sample;
  if (finger) {
    // O firmware calcula SpO2 = 110 - 25 * R, com R = (ACred/DCred) / (ACir/DCir).
    double ratio = (110.0 - spo2) / 25.0;
    double red_ac = ratio * IR_AC * RED_DC / IR_DC;
    double wave = sin(chip->phase);
    sample.ir = (uint32_t)(IR_DC + IR_AC * wave);
    sample.red = (uint32_t)(RED_DC + red_ac * wave);
  } else {
    sample.ir = 800 + (rand() % 50);
    sample.red = 600 + (rand() % 50);
  }

  // Com rollover, a FIFO continua gravando por cima das amostras mais antigas.
  uint8_t wr = chip->regs[REG_FIFO_WR_PTR] & (FIFO_DEPTH - 1);
  chip->fifo[wr] = sample;
  chip->regs[REG_FIFO_WR_PTR] = (wr + 1) & (FIFO_DEPTH - 1);
  if (chip->regs[REG_FIFO_WR_PTR] == chip->regs[REG_FIFO_RD_PTR] && chip->regs[REG_OVF_COUNTER] < 0x1F) {
    chip->regs[REG_OVF_COUNTER]++;
  }

  timer_start(chip->sample_timer, (uint32_t)(1000000.0 / rate), false);
}

static bool on_i2c_connect(void *user_data, uint32_t address, bool read) {
  chip_state_t *chip = user_data;
  chip->expecting_pointer = !read;
  return true;
}

static uint8_t read_fifo_byte(chip_state_t *chip) {
  if (fifo_count(chip) == 0) {
    return 0;
  }

  uint8_t rd = chip->regs[REG_FIFO_RD_PTR] & (FIFO_DEPTH - 1);
  sample_t sample = chip->fifo[rd];
  uint8_t index = chip->fifo_byte_index;
  uint32_t value = (index < 3) ? sample.red : sample.ir;

  uint8_t data;
  switch (index % 3) {
    case 0: data = (value >> 16) & 0x03; break;
    case 1: data = (value >> 8) & 0xFF; break;
    default: data = value & 0xFF; break;
  }

  chip->fifo_byte_index++;
  if (chip->fifo_byte_index >= bytes_per_sample(chip)) {
    chip->fifo_byte_index = 0;
    chip->regs[REG_FIFO_RD_PTR] = (rd + 1) & (FIFO_DEPTH - 1);
  }
  return data;
}

static uint8_t on_i2c_read(void *user_data) {
  chip_state_t *chip = user_data;

  if (chip->reg_pointer == REG_FIFO_DATA) {
    return read_fifo_byte(chip);
  }

  uint8_t data = chip->regs[chip->reg_pointer];
  chip->reg_pointer++;
  return data;
}

static bool on_i2c_write(void *user_data, uint8_t data) {
  chip_state_t *chip = user_data;

  if (chip->expecting_pointer) {
    chip->reg_pointer = data;
    chip->expecting_pointer = false;
    if (data == REG_FIFO_DATA) {
      chip->fifo_byte_index = 0;
    }
    return true;
  }

  uint8_t reg = chip->reg_pointer;
  if (reg == REG_MODE_CONFIG && (data & MODE_RESET)) {
    reset_registers(chip);
  } else if (reg != REG_PART_ID && reg != REG_REVISION_ID) {
    chip->regs[reg] = data;
    if (reg == REG_FIFO_WR_PTR || reg == REG_FIFO_RD_PTR) {
      chip->regs[reg] &= (FIFO_DEPTH - 1);
      chip->fifo_byte_index = 0;
    }
  }

  if (reg != REG_FIFO_DATA) {
    chip->reg_pointer++;
  }
  return true;
}

static void on_i2c_disconnect(void *user_data) {
  // Nada a fazer.
}
