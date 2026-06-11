#pragma once

#include <stdint.h>

#include "hcp2_mailbox.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  volatile hcp2_lp_mailbox_t *mailbox;
  uint32_t expected_firmware_version;
  uint32_t epoch;
  uint32_t next_sequence;
} hcp2_hp_supervisor_t;

void hcp2_hp_supervisor_init(hcp2_hp_supervisor_t *supervisor, volatile hcp2_lp_mailbox_t *mailbox,
                             uint32_t expected_firmware_version);
void hcp2_hp_supervisor_begin_session(hcp2_hp_supervisor_t *supervisor, uint32_t epoch);
hcp2_lp_reload_decision_t hcp2_hp_supervisor_reload_decision(const hcp2_hp_supervisor_t *supervisor,
                                                             uint32_t heartbeat_before,
                                                             uint32_t heartbeat_after);
uint8_t hcp2_hp_supervisor_is_healthy(const hcp2_hp_supervisor_t *supervisor, uint32_t heartbeat_before,
                                      uint32_t heartbeat_after);
uint32_t hcp2_hp_supervisor_send_command(hcp2_hp_supervisor_t *supervisor,
                                         hcp2_lp_command_id_t command_id, uint8_t argument);
uint8_t hcp2_hp_supervisor_ack_received(const hcp2_hp_supervisor_t *supervisor, uint32_t sequence);
uint8_t hcp2_hp_supervisor_read_state(const hcp2_hp_supervisor_t *supervisor,
                                      hcp2_lp_state_snapshot_t *out);

#ifdef __cplusplus
}
#endif
