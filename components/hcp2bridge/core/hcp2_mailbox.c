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
  mailbox->lp_time_us = now_us;
  memory_barrier_();
  mailbox->state_seq = seq + 1u;
}

void hcp2_lp_mailbox_publish_counters(volatile hcp2_lp_mailbox_t *mailbox, uint32_t now_us, uint32_t polls_seen,
                                      uint32_t polls_answered, uint32_t tx_abort_count,
                                      uint32_t collision_count, uint32_t max_de_hold_us) {
  if (mailbox == 0) {
    return;
  }

  mailbox->lp_time_us = now_us;
  mailbox->polls_seen = polls_seen;
  mailbox->polls_answered = polls_answered;
  mailbox->tx_abort_count = tx_abort_count;
  mailbox->collision_count = collision_count;
  if (max_de_hold_us > mailbox->max_de_hold_us) {
    mailbox->max_de_hold_us = max_de_hold_us;
  }
  memory_barrier_();
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
                                  hcp2_lp_command_id_t command_id, uint8_t argument, uint32_t deadline_us) {
  if (mailbox == 0 || command_id == HCP2_LP_COMMAND_NONE) {
    return;
  }

  mailbox->command_epoch = epoch;
  mailbox->command_id = (uint8_t) command_id;
  mailbox->command_argument = argument;
  mailbox->command_deadline_us = deadline_us;
  mailbox->command_ack_result = (uint8_t) HCP2_LP_COMMAND_RESULT_NONE;
  memory_barrier_();
  mailbox->command_sequence = sequence;
}

uint8_t hcp2_lp_mailbox_take_command(volatile hcp2_lp_mailbox_t *mailbox, uint32_t expected_epoch,
                                     uint32_t *last_sequence, uint32_t now_us, hcp2_lp_command_t *out) {
  uint32_t sequence;
  uint8_t command_id;

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

  command_id = mailbox->command_id;
  if (command_id == (uint8_t) HCP2_LP_COMMAND_NONE ||
      command_id > (uint8_t) HCP2_LP_COMMAND_LIGHT) {
    hcp2_lp_mailbox_ack_command(mailbox, sequence, HCP2_LP_COMMAND_RESULT_UNKNOWN);
    *last_sequence = sequence;
    return 0u;
  }
  if (mailbox->command_deadline_us != 0u && ((int32_t) (now_us - mailbox->command_deadline_us)) > 0) {
    hcp2_lp_mailbox_ack_command(mailbox, sequence, HCP2_LP_COMMAND_RESULT_EXPIRED);
    *last_sequence = sequence;
    return 0u;
  }

  out->epoch = mailbox->command_epoch;
  out->sequence = sequence;
  out->deadline_us = mailbox->command_deadline_us;
  out->command_id = command_id;
  out->argument = mailbox->command_argument;
  *last_sequence = sequence;
  return 1u;
}

void hcp2_lp_mailbox_ack_command(volatile hcp2_lp_mailbox_t *mailbox, uint32_t sequence,
                                 hcp2_lp_command_result_t result) {
  if (mailbox == 0 || sequence == 0u) {
    return;
  }

  mailbox->command_ack_result = (uint8_t) result;
  memory_barrier_();
  mailbox->command_ack_sequence = sequence;
}

void hcp2_lp_mailbox_sample_health(const volatile hcp2_lp_mailbox_t *mailbox, hcp2_lp_health_sample_t *out) {
  if (out == 0) {
    return;
  }
  memset(out, 0, sizeof(*out));
  if (mailbox == 0) {
    return;
  }

  out->heartbeat = mailbox->heartbeat;
  out->polls_seen = mailbox->polls_seen;
  out->polls_answered = mailbox->polls_answered;
  out->command_sequence = mailbox->command_sequence;
  out->command_ack_sequence = mailbox->command_ack_sequence;
  out->drive_state = mailbox->state;
}

static uint8_t state_is_moving_(uint8_t state) {
  return state == (uint8_t) HCP2_DRIVE_OPENING || state == (uint8_t) HCP2_DRIVE_CLOSING ||
         state == (uint8_t) HCP2_DRIVE_HALF_OPENING;
}

static uint8_t command_pending_(const hcp2_lp_health_sample_t *sample) {
  return sample != 0 && sample->command_sequence != 0u &&
         sample->command_sequence != sample->command_ack_sequence;
}

static hcp2_lp_reload_decision_t reload_or_defer_(const hcp2_lp_health_sample_t *after) {
  if (after != 0 && (state_is_moving_(after->drive_state) || command_pending_(after))) {
    return HCP2_LP_RELOAD_DEFER;
  }
  return HCP2_LP_RELOAD_REQUIRED;
}

hcp2_lp_reload_decision_t hcp2_lp_mailbox_reload_decision(const volatile hcp2_lp_mailbox_t *mailbox,
                                                          uint32_t expected_firmware_version,
                                                          const hcp2_lp_health_sample_t *before,
                                                          const hcp2_lp_health_sample_t *after) {
  if (mailbox == 0) {
    return HCP2_LP_RELOAD_REQUIRED;
  }
  if (mailbox->magic != HCP2_LP_MAILBOX_MAGIC) {
    return HCP2_LP_RELOAD_REQUIRED;
  }
  if (after == 0 || before == 0 || after->heartbeat == before->heartbeat) {
    return HCP2_LP_RELOAD_REQUIRED;
  }
  if (mailbox->abi_version != HCP2_LP_MAILBOX_ABI_VERSION || mailbox->struct_size != HCP2_LP_MAILBOX_SIZE) {
    return reload_or_defer_(after);
  }
  if (mailbox->firmware_version != expected_firmware_version) {
    return reload_or_defer_(after);
  }
  if (before->polls_seen != 0u && after->polls_seen == before->polls_seen) {
    return HCP2_LP_RELOAD_REQUIRED;
  }
  if (after->polls_seen > before->polls_seen && after->polls_answered == before->polls_answered) {
    return HCP2_LP_RELOAD_REQUIRED;
  }
  return HCP2_LP_RELOAD_SKIP;
}
