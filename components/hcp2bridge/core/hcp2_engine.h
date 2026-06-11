#pragma once

#include <stdint.h>

#include "hcp2_frame.h"
#include "hcp2_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HCP2_DEFAULT_RESPONSE_DELAY_US 4500u
#define HCP2_DEFAULT_BUTTON_PRESS_US 100000u

typedef struct {
  uint8_t slave_id;
  uint8_t signature[HCP2_SIGNATURE_LEN];
  uint32_t response_delay_us;
  uint32_t button_press_us;
} hcp2_engine_config_t;

typedef struct {
  hcp2_port_t port;
  hcp2_engine_config_t config;
  uint8_t rx[HCP2_MAX_FRAME_LEN];
  uint8_t rx_len;
  uint8_t pending_tx[HCP2_MAX_FRAME_LEN];
  uint8_t pending_tx_len;
  uint32_t pending_tx_due_us;
  uint8_t pending_tx_ready;
  hcp2_button_t active_button;
  uint8_t release_pending;
  uint32_t press_until_us;
  hcp2_drive_status_t drive_status;
  uint32_t valid_frames;
  uint32_t crc_errors;
  uint32_t rx_errors;
  uint32_t responses_sent;
} hcp2_engine_t;

void hcp2_engine_config_default(hcp2_engine_config_t *config);
void hcp2_engine_init(hcp2_engine_t *engine, const hcp2_port_t *port, const hcp2_engine_config_t *config);
void hcp2_engine_rx_byte(hcp2_engine_t *engine, uint8_t byte, uint8_t flags);
void hcp2_engine_poll(hcp2_engine_t *engine);
uint8_t hcp2_engine_press_button(hcp2_engine_t *engine, hcp2_button_t button);
const hcp2_drive_status_t *hcp2_engine_drive_status(const hcp2_engine_t *engine);

#ifdef __cplusplus
}
#endif
