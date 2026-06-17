#pragma once

#include <stdint.h>

#include "hcp2_engine.h"
#include "hcp2_mailbox.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  hcp2_engine_t *engine;
  volatile hcp2_lp_mailbox_t *mailbox;
  uint32_t active_epoch;
  uint32_t last_command_sequence;
  uint32_t last_protocol_sequence;
  uint32_t last_status_poll_count;
  uint32_t last_status_poll_us;
} hcp2_responder_runtime_t;

typedef struct {
  uint32_t tx_abort_count;
  uint32_t collision_count;
  uint32_t max_de_hold_us;
  uint32_t port_rx_error_count;
  uint32_t max_loop_us;
  uint32_t loop_overrun_count;
  uint32_t rx_starvation_count;
  uint32_t stuck_de_count;
  uint32_t mailbox_repair_count;
  uint16_t health_flags;
  uint16_t max_rx_fifo_count;
} hcp2_responder_runtime_counters_t;

void hcp2_responder_runtime_init(hcp2_responder_runtime_t *runtime, hcp2_engine_t *engine,
                                 volatile hcp2_lp_mailbox_t *mailbox);
void hcp2_responder_runtime_begin_from_mailbox(hcp2_responder_runtime_t *runtime, uint32_t now_us);
void hcp2_responder_runtime_trace(hcp2_responder_runtime_t *runtime, uint16_t event, uint16_t value,
                                  uint32_t now_us);
uint8_t hcp2_responder_runtime_publish_protocol_event(hcp2_responder_runtime_t *runtime);
void hcp2_responder_runtime_note_status_poll(hcp2_responder_runtime_t *runtime, uint32_t now_us);
void hcp2_responder_runtime_publish_state(hcp2_responder_runtime_t *runtime, uint32_t now_us);
void hcp2_responder_runtime_publish_counters(hcp2_responder_runtime_t *runtime,
                                             const hcp2_responder_runtime_counters_t *port_counters,
                                             uint32_t now_us);
hcp2_lp_command_result_t hcp2_responder_runtime_handle_mailbox_command(hcp2_responder_runtime_t *runtime,
                                                                       uint32_t now_us);
uint8_t hcp2_responder_runtime_handle_stop_trigger(hcp2_responder_runtime_t *runtime, uint32_t now_us);

#ifdef __cplusplus
}
#endif
