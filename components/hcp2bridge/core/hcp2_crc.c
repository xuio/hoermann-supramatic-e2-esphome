#include "hcp2_crc.h"

uint16_t hcp2_crc16_modbus(const uint8_t *data, uint16_t len) {
  uint16_t crc = 0xFFFFu;
  uint16_t i;

  for (i = 0; i < len; i++) {
    uint8_t bit;
    crc ^= data[i];
    for (bit = 0; bit < 8; bit++) {
      if ((crc & 0x0001u) != 0u) {
        crc = (uint16_t) ((crc >> 1) ^ 0xA001u);
      } else {
        crc = (uint16_t) (crc >> 1);
      }
    }
  }

  return crc;
}

uint8_t hcp2_crc16_check(const uint8_t *data, uint16_t len) {
  if (data == 0 || len < 3u) {
    return 0u;
  }
  return hcp2_crc16_modbus(data, len) == 0u;
}

void hcp2_crc16_append(uint8_t *data, uint16_t payload_len) {
  const uint16_t crc = hcp2_crc16_modbus(data, payload_len);
  data[payload_len] = (uint8_t) (crc & 0xFFu);
  data[payload_len + 1u] = (uint8_t) ((crc >> 8) & 0xFFu);
}
