#pragma once

#include <stdint.h>

#include "hcp2_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HCP2_REALTIME_RX_FIFO_CAPACITY 128u
#define HCP2_REALTIME_DEFAULT_DEADMAN_US 9000u

typedef struct {
  uint32_t due_us;
  uint32_t tail_margin_us;
  uint8_t frame_type;
  uint8_t len;
  uint8_t data[HCP2_MAX_FRAME_LEN];
} hcp2_realtime_tx_slot_t;

typedef struct {
  uint32_t isr_max_time_us;
  uint32_t tx_underrun_count;
  uint32_t rx_fifo_high_water;
  uint32_t timer_late_us;
  uint32_t slot_drop_count;
  uint32_t de_deadman_count;
} hcp2_realtime_port_counters_t;

typedef struct {
  hcp2_realtime_tx_slot_t slot;
  hcp2_realtime_port_counters_t counters;
  uint8_t slot_valid;
  uint8_t de_enabled;
  uint8_t rx_fifo[HCP2_REALTIME_RX_FIFO_CAPACITY];
  uint8_t rx_flags[HCP2_REALTIME_RX_FIFO_CAPACITY];
  uint16_t rx_head;
  uint16_t rx_tail;
  uint16_t rx_count;
  uint32_t deadman_us;
  uint32_t de_enabled_since_us;
  uint8_t tx_capture[HCP2_MAX_FRAME_LEN];
  uint8_t tx_capture_len;
  uint8_t tx_capture_frame_type;
} hcp2_realtime_port_model_t;

void hcp2_realtime_port_model_init(hcp2_realtime_port_model_t *model, uint32_t deadman_us);
uint8_t hcp2_realtime_tx_slot_init(hcp2_realtime_tx_slot_t *slot, const uint8_t *data, uint8_t len,
                                   uint8_t frame_type, uint32_t due_us, uint32_t tail_margin_us);
uint8_t hcp2_realtime_port_submit_tx_slot(hcp2_realtime_port_model_t *model,
                                          const hcp2_realtime_tx_slot_t *slot);
uint8_t hcp2_realtime_port_service_timer(hcp2_realtime_port_model_t *model, uint32_t now_us);
uint8_t hcp2_realtime_port_push_rx(hcp2_realtime_port_model_t *model, uint8_t byte, uint8_t flags);
uint8_t hcp2_realtime_port_pop_rx(hcp2_realtime_port_model_t *model, uint8_t *byte, uint8_t *flags);
void hcp2_realtime_port_check_de_deadman(hcp2_realtime_port_model_t *model, uint32_t now_us);

#ifdef __cplusplus
}
#endif
