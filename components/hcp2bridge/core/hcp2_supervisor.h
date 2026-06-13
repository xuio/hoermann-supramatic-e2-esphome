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
  uint32_t command_ttl_us;
} hcp2_hp_supervisor_t;

#define HCP2_HP_DEFAULT_COMMAND_TTL_US 250000u
#define HCP2_HP_DEFAULT_STOP_TRIGGER_TTL_US 30000000u

void hcp2_hp_supervisor_init(hcp2_hp_supervisor_t *supervisor, volatile hcp2_lp_mailbox_t *mailbox,
                             uint32_t expected_firmware_version);
void hcp2_hp_supervisor_begin_session(hcp2_hp_supervisor_t *supervisor, uint32_t epoch);
void hcp2_hp_supervisor_sample_health(const hcp2_hp_supervisor_t *supervisor, hcp2_lp_health_sample_t *out);
hcp2_lp_reload_decision_t hcp2_hp_supervisor_reload_decision(const hcp2_hp_supervisor_t *supervisor,
                                                             const hcp2_lp_health_sample_t *before,
                                                             const hcp2_lp_health_sample_t *after);
uint8_t hcp2_hp_supervisor_is_healthy(const hcp2_hp_supervisor_t *supervisor,
                                      const hcp2_lp_health_sample_t *before,
                                      const hcp2_lp_health_sample_t *after);
uint32_t hcp2_hp_supervisor_send_command(hcp2_hp_supervisor_t *supervisor,
                                         hcp2_lp_command_id_t command_id, uint8_t argument);
uint32_t hcp2_hp_supervisor_send_command_at(hcp2_hp_supervisor_t *supervisor,
                                            hcp2_lp_command_id_t command_id, uint8_t argument,
                                            uint32_t now_us, uint32_t ttl_us);
uint8_t hcp2_hp_supervisor_ack_received(const hcp2_hp_supervisor_t *supervisor, uint32_t sequence);
hcp2_lp_command_result_t hcp2_hp_supervisor_ack_result(const hcp2_hp_supervisor_t *supervisor,
                                                       uint32_t sequence);
uint8_t hcp2_hp_supervisor_read_state(const hcp2_hp_supervisor_t *supervisor,
                                      hcp2_lp_state_snapshot_t *out);
uint8_t hcp2_hp_supervisor_arm_stop_trigger(hcp2_hp_supervisor_t *supervisor, uint8_t target_position,
                                            uint32_t ttl_us);
uint8_t hcp2_hp_supervisor_arm_stop_trigger_at(hcp2_hp_supervisor_t *supervisor, uint8_t target_position,
                                               uint32_t now_us, uint32_t ttl_us);
void hcp2_hp_supervisor_disarm_stop_trigger(hcp2_hp_supervisor_t *supervisor);

#ifdef __cplusplus
}
#endif
