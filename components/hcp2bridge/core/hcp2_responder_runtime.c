#include "hcp2_responder_runtime.h"

static uint8_t time_reached_(uint32_t now, uint32_t due) {
  return ((int32_t) (now - due)) >= 0 ? 1u : 0u;
}

static hcp2_button_t button_for_command_(uint8_t command_id) {
  switch (command_id) {
    case HCP2_LP_COMMAND_OPEN:
      return HCP2_BUTTON_OPEN;
    case HCP2_LP_COMMAND_CLOSE:
      return HCP2_BUTTON_CLOSE;
    case HCP2_LP_COMMAND_STOP:
      return HCP2_BUTTON_STOP;
    case HCP2_LP_COMMAND_VENT:
      return HCP2_BUTTON_VENT;
    case HCP2_LP_COMMAND_HALF:
      return HCP2_BUTTON_HALF;
    case HCP2_LP_COMMAND_LIGHT:
      return HCP2_BUTTON_LIGHT;
    default:
      return HCP2_BUTTON_NONE;
  }
}

static uint8_t stop_trigger_crossed_(const hcp2_drive_status_t *status, uint8_t target_position) {
  if (status == 0) {
    return 0u;
  }
  switch ((hcp2_drive_state_code_t) status->state) {
    case HCP2_DRIVE_OPENING:
    case HCP2_DRIVE_HALF_OPENING:
    case HCP2_DRIVE_VENT_MOVING:
      return status->current_position >= target_position ? 1u : 0u;
    case HCP2_DRIVE_CLOSING:
      return status->current_position <= target_position ? 1u : 0u;
    default:
      return 0u;
  }
}

void hcp2_responder_runtime_init(hcp2_responder_runtime_t *runtime, hcp2_engine_t *engine,
                                 volatile hcp2_lp_mailbox_t *mailbox) {
  if (runtime == 0) {
    return;
  }
  runtime->engine = engine;
  runtime->mailbox = mailbox;
  runtime->active_epoch = 0u;
  runtime->last_command_sequence = 0u;
  runtime->last_protocol_sequence = 0u;
  runtime->last_status_poll_count = 0u;
  runtime->last_status_poll_us = 0u;
}

void hcp2_responder_runtime_trace(hcp2_responder_runtime_t *runtime, uint16_t event, uint16_t value,
                                  uint32_t now_us) {
  volatile hcp2_lp_mailbox_t *mailbox;
  uint32_t index;

  if (runtime == 0 || runtime->mailbox == 0) {
    return;
  }

  mailbox = runtime->mailbox;
  index = mailbox->trace_head % HCP2_LP_TRACE_CAPACITY;
  mailbox->trace[index].at_us = now_us;
  mailbox->trace[index].event = event;
  mailbox->trace[index].value = value;
  mailbox->trace_head++;
  if ((mailbox->trace_head - mailbox->trace_tail) > HCP2_LP_TRACE_CAPACITY) {
    mailbox->trace_tail = mailbox->trace_head - HCP2_LP_TRACE_CAPACITY;
  }
}

void hcp2_responder_runtime_begin_from_mailbox(hcp2_responder_runtime_t *runtime, uint32_t now_us) {
  volatile hcp2_lp_mailbox_t *mailbox;
  uint32_t pending_sequence;

  if (runtime == 0 || runtime->mailbox == 0) {
    return;
  }

  mailbox = runtime->mailbox;
  pending_sequence = mailbox->command_sequence;
  runtime->active_epoch = mailbox->command_epoch;
  runtime->last_command_sequence = pending_sequence;
  if (pending_sequence != 0u && mailbox->command_ack_sequence != pending_sequence) {
    hcp2_lp_mailbox_ack_command(mailbox, pending_sequence, HCP2_LP_COMMAND_RESULT_EXPIRED);
    hcp2_responder_runtime_trace(runtime, HCP2_LP_TRACE_COMMAND,
                                 (uint16_t) (0xEE00u | (mailbox->command_id & 0xFFu)), now_us);
  }
}

uint8_t hcp2_responder_runtime_publish_protocol_event(hcp2_responder_runtime_t *runtime) {
  hcp2_protocol_event_t event;

  if (runtime == 0 || runtime->engine == 0 || runtime->mailbox == 0) {
    return 0u;
  }
  if (!hcp2_engine_read_protocol_event(runtime->engine, &runtime->last_protocol_sequence, &event)) {
    return 0u;
  }
  hcp2_lp_mailbox_publish_protocol_event(runtime->mailbox, &event);
  return 1u;
}

void hcp2_responder_runtime_note_status_poll(hcp2_responder_runtime_t *runtime, uint32_t now_us) {
  if (runtime == 0 || runtime->engine == 0) {
    return;
  }
  if (runtime->engine->status_polls_received != runtime->last_status_poll_count) {
    runtime->last_status_poll_count = runtime->engine->status_polls_received;
    runtime->last_status_poll_us = now_us;
  }
}

void hcp2_responder_runtime_publish_state(hcp2_responder_runtime_t *runtime, uint32_t now_us) {
  if (runtime == 0 || runtime->engine == 0 || runtime->mailbox == 0) {
    return;
  }
  hcp2_lp_mailbox_publish_state(runtime->mailbox, hcp2_engine_drive_status(runtime->engine), now_us);
}

void hcp2_responder_runtime_publish_counters(hcp2_responder_runtime_t *runtime,
                                             const hcp2_responder_runtime_counters_t *port_counters,
                                             uint32_t now_us) {
  hcp2_lp_counters_t counters;

  if (runtime == 0 || runtime->engine == 0 || runtime->mailbox == 0 || port_counters == 0) {
    return;
  }

  counters.now_us = now_us;
  counters.polls_seen = runtime->engine->status_polls_received;
  counters.polls_answered = runtime->engine->status_responses_sent;
  counters.tx_abort_count = port_counters->tx_abort_count;
  counters.collision_count = port_counters->collision_count;
  counters.max_de_hold_us = port_counters->max_de_hold_us;
  counters.last_poll_us = runtime->last_status_poll_us;
  counters.crc_error_count = runtime->engine->crc_errors;
  counters.rx_error_count = runtime->engine->rx_errors + port_counters->port_rx_error_count;
  counters.max_loop_us = port_counters->max_loop_us;
  counters.loop_overrun_count = port_counters->loop_overrun_count;
  counters.rx_starvation_count = port_counters->rx_starvation_count;
  counters.stuck_de_count = port_counters->stuck_de_count;
  counters.mailbox_repair_count = port_counters->mailbox_repair_count;
  counters.health_flags = port_counters->health_flags;
  counters.max_rx_fifo_count = port_counters->max_rx_fifo_count;
  counters.max_poll_rx_to_schedule_us = runtime->engine->max_status_poll_rx_to_schedule_us;
  counters.max_response_schedule_to_tx_start_us = runtime->engine->max_status_response_schedule_to_tx_start_us;
  counters.max_response_tx_us = runtime->engine->max_status_response_tx_us;
  hcp2_lp_mailbox_publish_counters(runtime->mailbox, &counters);
}

hcp2_lp_command_result_t hcp2_responder_runtime_handle_mailbox_command(hcp2_responder_runtime_t *runtime,
                                                                       uint32_t now_us) {
  hcp2_lp_command_t command;
  hcp2_button_t button;
  volatile hcp2_lp_mailbox_t *mailbox;

  if (runtime == 0 || runtime->engine == 0 || runtime->mailbox == 0) {
    return HCP2_LP_COMMAND_RESULT_NONE;
  }

  mailbox = runtime->mailbox;
  if (mailbox->command_sequence == 0u) {
    runtime->active_epoch = mailbox->command_epoch;
    runtime->last_command_sequence = 0u;
    return HCP2_LP_COMMAND_RESULT_NONE;
  }
  if (!hcp2_lp_mailbox_take_command(mailbox, runtime->active_epoch, &runtime->last_command_sequence, now_us,
                                    &command)) {
    return HCP2_LP_COMMAND_RESULT_NONE;
  }

  button = button_for_command_(command.command_id);
  if (button == HCP2_BUTTON_NONE) {
    hcp2_lp_mailbox_ack_command(mailbox, command.sequence, HCP2_LP_COMMAND_RESULT_UNKNOWN);
    return HCP2_LP_COMMAND_RESULT_UNKNOWN;
  }
  if (!hcp2_engine_press_button(runtime->engine, button)) {
    hcp2_lp_mailbox_ack_command(mailbox, command.sequence, HCP2_LP_COMMAND_RESULT_BUSY);
    return HCP2_LP_COMMAND_RESULT_BUSY;
  }

  hcp2_lp_mailbox_ack_command(mailbox, command.sequence, HCP2_LP_COMMAND_RESULT_EXECUTED);
  hcp2_responder_runtime_trace(runtime, HCP2_LP_TRACE_COMMAND, (uint16_t) command.command_id, now_us);
  return HCP2_LP_COMMAND_RESULT_EXECUTED;
}

uint8_t hcp2_responder_runtime_handle_stop_trigger(hcp2_responder_runtime_t *runtime, uint32_t now_us) {
  hcp2_lp_stop_trigger_t trigger;

  if (runtime == 0 || runtime->engine == 0 || runtime->mailbox == 0) {
    return 0u;
  }
  if (!hcp2_lp_mailbox_read_stop_trigger(runtime->mailbox, &trigger)) {
    return 0u;
  }
  if (trigger.epoch != runtime->active_epoch) {
    hcp2_lp_mailbox_disarm_stop_trigger(runtime->mailbox);
    hcp2_responder_runtime_trace(runtime, HCP2_LP_TRACE_STOP_TRIGGER, 0xE000u, now_us);
    return 0u;
  }
  if (trigger.deadline_us != 0u && time_reached_(now_us, trigger.deadline_us)) {
    hcp2_lp_mailbox_disarm_stop_trigger(runtime->mailbox);
    hcp2_responder_runtime_trace(runtime, HCP2_LP_TRACE_STOP_TRIGGER,
                                 (uint16_t) (0xE100u | trigger.target_position), now_us);
    return 0u;
  }
  if (!stop_trigger_crossed_(hcp2_engine_drive_status(runtime->engine), trigger.target_position)) {
    return 0u;
  }
  if (!hcp2_engine_press_button(runtime->engine, HCP2_BUTTON_STOP)) {
    return 0u;
  }

  hcp2_lp_mailbox_mark_stop_trigger_fired(runtime->mailbox);
  hcp2_responder_runtime_trace(runtime, HCP2_LP_TRACE_STOP_TRIGGER,
                               (uint16_t) (0x5000u | trigger.target_position), now_us);
  return 1u;
}
