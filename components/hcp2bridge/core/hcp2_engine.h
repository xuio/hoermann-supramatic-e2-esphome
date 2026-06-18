#pragma once

#include <stdint.h>

#include "hcp2_frame.h"
#include "hcp2_hot.h"
#include "hcp2_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HCP2_ENABLE_PROTOCOL_EVENTS
#define HCP2_ENABLE_PROTOCOL_EVENTS 1
#endif

#define HCP2_DEFAULT_RESPONSE_DELAY_US 4200u
#define HCP2_DEFAULT_BUTTON_PRESS_US 100000u

typedef struct {
  uint8_t slave_id;
  uint8_t signature[HCP2_SIGNATURE_LEN];
  uint32_t response_delay_us;
  uint32_t button_press_us;
} hcp2_engine_config_t;

typedef enum {
  HCP2_PROTOCOL_EVENT_NONE = 0,
  HCP2_PROTOCOL_EVENT_RX = 1,
  HCP2_PROTOCOL_EVENT_TX = 2,
  HCP2_PROTOCOL_EVENT_BAD_CRC = 3,
  HCP2_PROTOCOL_EVENT_RX_ERROR = 4,
} hcp2_protocol_event_type_t;

typedef struct {
  uint32_t sequence;
  uint32_t at_us;
  uint8_t event_type;
  uint8_t frame_type;
  uint8_t len;
  uint8_t reserved;
  uint8_t data[HCP2_MAX_FRAME_LEN];
} hcp2_protocol_event_t;

typedef struct {
  uint32_t scheduled_us;
  uint32_t due_us;
  uint8_t frame_type;
  uint8_t is_status_response;
  uint8_t reserved[2];
} hcp2_pending_tx_meta_t;

typedef struct {
  hcp2_port_t port;
  hcp2_engine_config_t config;
  uint8_t rx[HCP2_MAX_FRAME_LEN];
  uint8_t rx_len;
  uint8_t pending_tx[HCP2_MAX_FRAME_LEN];
  uint8_t pending_tx_len;
  uint8_t pending_tx_kind;
  uint8_t pending_tx_frame_type;
  uint32_t pending_tx_scheduled_us;
  uint32_t pending_tx_due_us;
  uint32_t pending_tx_started_us;
  uint8_t pending_tx_ready;
  uint8_t pending_tx_claimed;
  uint8_t pending_tx_started;
  hcp2_button_t active_button;
  uint8_t release_pending;
  uint32_t press_until_us;
  hcp2_drive_status_t drive_status;
  uint32_t valid_frames;
  uint32_t broadcasts_received;
  uint32_t status_polls_received;
  uint32_t status_responses_sent;
  uint32_t crc_errors;
  uint32_t rx_errors;
  uint32_t responses_sent;
  uint32_t max_status_poll_rx_to_schedule_us;
  uint32_t max_status_response_schedule_to_tx_start_us;
  uint32_t max_status_response_tx_us;
  uint32_t pending_tx_drop_count;
#if HCP2_ENABLE_PROTOCOL_EVENTS
  hcp2_protocol_event_t protocol_event;
#endif
} hcp2_engine_t;

void hcp2_engine_config_default(hcp2_engine_config_t *config);
void hcp2_engine_init(hcp2_engine_t *engine, const hcp2_port_t *port, const hcp2_engine_config_t *config);
void hcp2_engine_rx_byte(hcp2_engine_t *engine, uint8_t byte, uint8_t flags);
void hcp2_engine_poll(hcp2_engine_t *engine);
uint8_t hcp2_engine_pending_tx_ready(const hcp2_engine_t *engine);
uint32_t hcp2_engine_pending_tx_due_us(const hcp2_engine_t *engine);
uint8_t hcp2_engine_claim_due_tx(hcp2_engine_t *engine, uint32_t now_us, uint8_t *out_buf, uint8_t *out_len,
                                 hcp2_pending_tx_meta_t *out_meta);
void hcp2_engine_mark_tx_started(hcp2_engine_t *engine, uint32_t now_us);
void hcp2_engine_mark_tx_done(hcp2_engine_t *engine, uint32_t now_us);
uint8_t hcp2_engine_press_button(hcp2_engine_t *engine, hcp2_button_t button);
const hcp2_drive_status_t *hcp2_engine_drive_status(const hcp2_engine_t *engine);
uint8_t hcp2_engine_read_protocol_event(const hcp2_engine_t *engine, uint32_t *last_sequence,
                                        hcp2_protocol_event_t *out);

#ifdef __cplusplus
}
#endif
