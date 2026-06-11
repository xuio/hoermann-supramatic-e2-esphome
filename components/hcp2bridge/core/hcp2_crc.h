#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t hcp2_crc16_modbus(const uint8_t *data, uint16_t len);
uint8_t hcp2_crc16_check(const uint8_t *data, uint16_t len);
void hcp2_crc16_append(uint8_t *data, uint16_t payload_len);

#ifdef __cplusplus
}
#endif
