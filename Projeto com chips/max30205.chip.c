// Chip simulado do MAX30205 (sensor de temperatura corporal) para o Wokwi.
// Responde no endereço I2C 0x48, igual ao sensor real.
// Registrador 0x00: temperatura em 16 bits (1 bit = 1/256 °C), byte mais alto primeiro.

#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>

#define MAX30205_ADDRESS 0x48

#define REG_TEMPERATURE 0x00
#define REG_CONFIG 0x01
#define REG_THYST 0x02
#define REG_TOS 0x03

typedef struct {
  uint32_t temperature_attr;
  uint8_t reg_pointer;
  uint8_t byte_index;
  bool expecting_pointer;
  uint16_t latched_value;
  uint8_t config;
  uint16_t thyst;
  uint16_t tos;
} chip_state_t;

static bool on_i2c_connect(void *user_data, uint32_t address, bool read);
static uint8_t on_i2c_read(void *user_data);
static bool on_i2c_write(void *user_data, uint8_t data);
static void on_i2c_disconnect(void *user_data);

void chip_init(void) {
  chip_state_t *chip = malloc(sizeof(chip_state_t));
  chip->temperature_attr = attr_init_float("temperature", 36.5);
  chip->reg_pointer = REG_TEMPERATURE;
  chip->byte_index = 0;
  chip->expecting_pointer = false;
  chip->latched_value = 0;
  chip->config = 0;
  chip->thyst = 75 * 256;
  chip->tos = 80 * 256;

  const i2c_config_t i2c_config = {
    .user_data = chip,
    .address = MAX30205_ADDRESS,
    .scl = pin_init("SCL", INPUT),
    .sda = pin_init("SDA", INPUT),
    .connect = on_i2c_connect,
    .read = on_i2c_read,
    .write = on_i2c_write,
    .disconnect = on_i2c_disconnect,
  };
  i2c_init(&i2c_config);

  printf("MAX30205 simulado pronto no endereco 0x48\n");
}

static uint16_t read_register16(chip_state_t *chip, uint8_t reg) {
  switch (reg) {
    case REG_TEMPERATURE: {
      float celsius = attr_read_float(chip->temperature_attr);
      return (uint16_t)(int16_t)(celsius * 256.0f + (celsius >= 0 ? 0.5f : -0.5f));
    }
    case REG_THYST:
      return chip->thyst;
    case REG_TOS:
      return chip->tos;
    default:
      return 0;
  }
}

static bool on_i2c_connect(void *user_data, uint32_t address, bool read) {
  chip_state_t *chip = user_data;
  chip->byte_index = 0;
  chip->expecting_pointer = !read;
  return true;
}

static uint8_t on_i2c_read(void *user_data) {
  chip_state_t *chip = user_data;

  if (chip->reg_pointer == REG_CONFIG) {
    return chip->config;
  }

  // Congela o valor no primeiro byte para os dois bytes serem coerentes.
  if (chip->byte_index % 2 == 0) {
    chip->latched_value = read_register16(chip, chip->reg_pointer);
  }
  uint8_t data = (chip->byte_index % 2 == 0) ? (chip->latched_value >> 8) : (chip->latched_value & 0xFF);
  chip->byte_index++;
  return data;
}

static bool on_i2c_write(void *user_data, uint8_t data) {
  chip_state_t *chip = user_data;

  if (chip->expecting_pointer) {
    chip->reg_pointer = data & 0x03;
    chip->expecting_pointer = false;
    chip->byte_index = 0;
    return true;
  }

  if (chip->reg_pointer == REG_CONFIG) {
    chip->config = data;
  } else if (chip->reg_pointer == REG_THYST || chip->reg_pointer == REG_TOS) {
    uint16_t *target = chip->reg_pointer == REG_THYST ? &chip->thyst : &chip->tos;
    if (chip->byte_index == 0) {
      *target = (uint16_t)((data << 8) | (*target & 0x00FF));
    } else {
      *target = (uint16_t)((*target & 0xFF00) | data);
    }
    chip->byte_index++;
  }
  return true;
}

static void on_i2c_disconnect(void *user_data) {
  // Nada a fazer.
}
