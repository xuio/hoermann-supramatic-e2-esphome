#include "hcp2_mailbox.h"

#include <string.h>

static void memory_barrier_(void) {
#if defined(__GNUC__) || defined(__clang__)
  __sync_synchronize();
#else
  (void) 0;
#endif
}

void hcp2_lp_mailbox_init(volatile hcp2_lp_mailbox_t *mailbox) {
  if (mailbox == 0) {
    return;
  }

  memset((void *) mailbox, 0, sizeof(*mailbox));
  mailbox->magic = HCP2_LP_MAILBOX_MAGIC;
  mailbox->abi_version = HCP2_LP_MAILBOX_ABI_VERSION;
  mailbox->struct_size = HCP2_LP_MAILBOX_SIZE;
  mailbox->firmware_version = HCP2_LP_FIRMWARE_VERSION;
  memory_barrier_();
}

void hcp2_lp_mailbox_publish_state(volatile hcp2_lp_mailbox_t *mailbox, const hcp2_drive_status_t *state,
                                   uint32_t now_us) {
  uint32_t seq;

  if (mailbox == 0 || state == 0) {
    return;
  }

  seq = mailbox->state_seq + 1u;
  if ((seq & 1u) == 0u) {
    seq++;
  }
  mailbox->state_seq = seq;
  memory_barrier_();
  mailbox->target_position = state->target_position;
  mailbox->current_position = state->current_position;
  mailbox->state = state->state;
  mailbox->light_on = state->light_on;
  mailbox->state_updated_us = now_us;
  memory_barrier_();
  mailbox->state_seq = seq + 1u;
}

uint8_t hcp2_lp_mailbox_read_state(const volatile hcp2_lp_mailbox_t *mailbox, hcp2_lp_state_snapshot_t *out) {
  uint32_t before;
  uint32_t after;
  hcp2_lp_state_snapshot_t snapshot;
  uint8_t attempt;

  if (mailbox == 0 || out == 0) {
    return 0u;
  }

  for (attempt = 0u; attempt < 4u; attempt++) {
    before = mailbox->state_seq;
    memory_barrier_();
    if ((before & 1u) != 0u) {
      continue;
    }
    snapshot.target_position = mailbox->target_position;
    snapshot.current_position = mailbox->current_position;
    snapshot.state = mailbox->state;
    snapshot.light_on = mailbox->light_on;
    snapshot.updated_us = mailbox->state_updated_us;
    memory_barrier_();
    after = mailbox->state_seq;
    if (before == after && (after & 1u) == 0u) {
      *out = snapshot;
      return 1u;
    }
  }

  return 0u;
}

void hcp2_lp_mailbox_send_command(volatile hcp2_lp_mailbox_t *mailbox, uint32_t epoch, uint32_t sequence,
                                  hcp2_lp_command_id_t command_id, uint8_t argument) {
  if (mailbox == 0 || command_id == HCP2_LP_COMMAND_NONE) {
    return;
  }

  mailbox->command_epoch = epoch;
  mailbox->command_id = (uint8_t) command_id;
  mailbox->command_argument = argument;
  memory_barrier_();
  mailbox->command_sequence = sequence;
}

uint8_t hcp2_lp_mailbox_take_command(volatile hcp2_lp_mailbox_t *mailbox, uint32_t expected_epoch,
                                     uint32_t *last_sequence, hcp2_lp_command_t *out) {
  uint32_t sequence;

  if (mailbox == 0 || last_sequence == 0 || out == 0) {
    return 0u;
  }
  if (mailbox->command_epoch != expected_epoch) {
    return 0u;
  }

  sequence = mailbox->command_sequence;
  if (sequence == 0u || sequence == *last_sequence) {
    return 0u;
  }
  if (mailbox->command_id == HCP2_LP_COMMAND_NONE) {
    mailbox->command_ack_sequence = sequence;
    *last_sequence = sequence;
    return 0u;
  }

  out->epoch = mailbox->command_epoch;
  out->sequence = sequence;
  out->command_id = mailbox->command_id;
  out->argument = mailbox->command_argument;
  *last_sequence = sequence;
  mailbox->command_ack_sequence = sequence;
  return 1u;
}

hcp2_lp_reload_decision_t hcp2_lp_mailbox_reload_decision(const volatile hcp2_lp_mailbox_t *mailbox,
                                                          uint32_t expected_firmware_version,
                                                          uint32_t heartbeat_before,
                                                          uint32_t heartbeat_after) {
  if (mailbox == 0) {
    return HCP2_LP_RELOAD_REQUIRED;
  }
  if (mailbox->magic != HCP2_LP_MAILBOX_MAGIC) {
    return HCP2_LP_RELOAD_REQUIRED;
  }
  if (mailbox->abi_version != HCP2_LP_MAILBOX_ABI_VERSION || mailbox->struct_size != HCP2_LP_MAILBOX_SIZE) {
    return HCP2_LP_RELOAD_REQUIRED;
  }
  if (mailbox->firmware_version != expected_firmware_version) {
    return HCP2_LP_RELOAD_REQUIRED;
  }
  if (heartbeat_after == heartbeat_before) {
    return HCP2_LP_RELOAD_REQUIRED;
  }
  return HCP2_LP_RELOAD_SKIP;
}
