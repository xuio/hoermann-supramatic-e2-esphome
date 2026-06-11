#include "hcp2_supervisor.h"

static void memory_barrier_(void) {
#if defined(__GNUC__) || defined(__clang__)
  __sync_synchronize();
#else
  (void) 0;
#endif
}

static uint32_t nonzero_epoch_(uint32_t epoch) {
  return epoch == 0u ? 1u : epoch;
}

void hcp2_hp_supervisor_init(hcp2_hp_supervisor_t *supervisor, volatile hcp2_lp_mailbox_t *mailbox,
                             uint32_t expected_firmware_version) {
  if (supervisor == 0) {
    return;
  }
  supervisor->mailbox = mailbox;
  supervisor->expected_firmware_version = expected_firmware_version;
  supervisor->epoch = 0u;
  supervisor->next_sequence = 0u;
}

void hcp2_hp_supervisor_begin_session(hcp2_hp_supervisor_t *supervisor, uint32_t epoch) {
  volatile hcp2_lp_mailbox_t *mailbox;

  if (supervisor == 0 || supervisor->mailbox == 0) {
    return;
  }

  mailbox = supervisor->mailbox;
  supervisor->epoch = nonzero_epoch_(epoch);
  supervisor->next_sequence = 0u;

  mailbox->command_sequence = 0u;
  memory_barrier_();
  mailbox->command_id = (uint8_t) HCP2_LP_COMMAND_NONE;
  mailbox->command_argument = 0u;
  mailbox->command_ack_sequence = 0u;
  mailbox->command_epoch = supervisor->epoch;
  memory_barrier_();
}

hcp2_lp_reload_decision_t hcp2_hp_supervisor_reload_decision(const hcp2_hp_supervisor_t *supervisor,
                                                             uint32_t heartbeat_before,
                                                             uint32_t heartbeat_after) {
  if (supervisor == 0) {
    return HCP2_LP_RELOAD_REQUIRED;
  }
  return hcp2_lp_mailbox_reload_decision(supervisor->mailbox, supervisor->expected_firmware_version,
                                         heartbeat_before, heartbeat_after);
}

uint8_t hcp2_hp_supervisor_is_healthy(const hcp2_hp_supervisor_t *supervisor, uint32_t heartbeat_before,
                                      uint32_t heartbeat_after) {
  return hcp2_hp_supervisor_reload_decision(supervisor, heartbeat_before, heartbeat_after) ==
         HCP2_LP_RELOAD_SKIP;
}

uint32_t hcp2_hp_supervisor_send_command(hcp2_hp_supervisor_t *supervisor,
                                         hcp2_lp_command_id_t command_id, uint8_t argument) {
  uint32_t sequence;

  if (supervisor == 0 || supervisor->mailbox == 0 || command_id == HCP2_LP_COMMAND_NONE) {
    return 0u;
  }

  sequence = supervisor->next_sequence + 1u;
  if (sequence == 0u) {
    sequence = 1u;
  }
  supervisor->next_sequence = sequence;
  hcp2_lp_mailbox_send_command(supervisor->mailbox, supervisor->epoch, sequence, command_id, argument);
  return sequence;
}

uint8_t hcp2_hp_supervisor_ack_received(const hcp2_hp_supervisor_t *supervisor, uint32_t sequence) {
  if (supervisor == 0 || supervisor->mailbox == 0 || sequence == 0u) {
    return 0u;
  }
  return supervisor->mailbox->command_ack_sequence == sequence ? 1u : 0u;
}

uint8_t hcp2_hp_supervisor_read_state(const hcp2_hp_supervisor_t *supervisor,
                                      hcp2_lp_state_snapshot_t *out) {
  if (supervisor == 0) {
    return 0u;
  }
  return hcp2_lp_mailbox_read_state(supervisor->mailbox, out);
}
