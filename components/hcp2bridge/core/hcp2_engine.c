#include "hcp2_engine.h"

#include <string.h>

static uint32_t now_us_(const hcp2_engine_t *engine) {
  if (engine->port.now_us == 0) {
    return 0u;
  }
  return engine->port.now_us(engine->port.user);
}

static uint8_t time_reached_(uint32_t now, uint32_t due) {
  return ((int32_t) (now - due)) >= 0 ? 1u : 0u;
}

static void rx_drop_(hcp2_engine_t *engine, uint8_t count) {
  if (count >= engine->rx_len) {
    engine->rx_len = 0u;
    return;
  }
  memmove(engine->rx, engine->rx + count, (uint16_t) (engine->rx_len - count));
  engine->rx_len = (uint8_t) (engine->rx_len - count);
}

enum {
  HCP2_PENDING_TX_OTHER = 0,
  HCP2_PENDING_TX_STATUS = 1,
};

static void record_protocol_event_(hcp2_engine_t *engine, uint8_t event_type, uint8_t frame_type,
                                   const uint8_t *data, uint8_t len, uint32_t at_us) {
#if HCP2_ENABLE_PROTOCOL_EVENTS
  hcp2_protocol_event_t *event;

  if (engine == 0) {
    return;
  }
  event = &engine->protocol_event;
  event->sequence++;
  if (event->sequence == 0u) {
    event->sequence = 1u;
  }
  event->at_us = at_us;
  event->event_type = event_type;
  event->frame_type = frame_type;
  event->reserved = 0u;
  if (data == 0 || len == 0u) {
    event->len = 0u;
    return;
  }
  if (len > HCP2_MAX_FRAME_LEN) {
    len = HCP2_MAX_FRAME_LEN;
  }
  memcpy(event->data, data, len);
  event->len = len;
#else
  (void) engine;
  (void) event_type;
  (void) frame_type;
  (void) data;
  (void) len;
  (void) at_us;
#endif
}

static void record_max_(uint32_t *dest, uint32_t value) {
  if (dest != 0 && value > *dest) {
    *dest = value;
  }
}

static void schedule_tx_(hcp2_engine_t *engine, const uint8_t *data, uint8_t len, uint8_t kind,
                         uint8_t frame_type, uint32_t rx_complete_us) {
  const uint32_t scheduled_us = now_us_(engine);

  if (len == 0u || len > HCP2_MAX_FRAME_LEN) {
    return;
  }
  memcpy(engine->pending_tx, data, len);
  engine->pending_tx_len = len;
  engine->pending_tx_kind = kind;
  engine->pending_tx_frame_type = frame_type;
  engine->pending_tx_scheduled_us = scheduled_us;
  engine->pending_tx_due_us = scheduled_us + engine->config.response_delay_us;
  engine->pending_tx_ready = 1u;
  if (kind == HCP2_PENDING_TX_STATUS && rx_complete_us != 0u) {
    record_max_(&engine->max_status_poll_rx_to_schedule_us, scheduled_us - rx_complete_us);
  }
}

static hcp2_button_t current_button_phase_(hcp2_engine_t *engine, uint8_t *release_phase) {
  const uint32_t now = now_us_(engine);
  hcp2_button_t button = engine->active_button;

  *release_phase = 0u;
  if (button == HCP2_BUTTON_NONE) {
    return HCP2_BUTTON_NONE;
  }
  if (!time_reached_(now, engine->press_until_us)) {
    return button;
  }
  if (engine->release_pending) {
    *release_phase = 1u;
    engine->release_pending = 0u;
    engine->active_button = HCP2_BUTTON_NONE;
    return button;
  }
  engine->active_button = HCP2_BUTTON_NONE;
  return HCP2_BUTTON_NONE;
}

static void handle_decoded_(hcp2_engine_t *engine, const hcp2_decoded_frame_t *frame, uint32_t rx_complete_us) {
  uint8_t tx[HCP2_MAX_FRAME_LEN];
  uint8_t tx_len = 0u;
  uint8_t release_phase = 0u;
  hcp2_button_t button;

  switch (frame->type) {
    case HCP2_FRAME_BROADCAST_STATUS:
      engine->drive_status = frame->drive_status;
      engine->broadcasts_received++;
      break;
    case HCP2_FRAME_BUS_SCAN:
      tx_len = hcp2_frame_build_scan_response(engine->config.slave_id, engine->config.signature, tx);
      schedule_tx_(engine, tx, tx_len, HCP2_PENDING_TX_OTHER, (uint8_t) frame->type, rx_complete_us);
      break;
    case HCP2_FRAME_STATUS_POLL:
      engine->status_polls_received++;
      button = current_button_phase_(engine, &release_phase);
      tx_len = hcp2_frame_build_status_response(engine->config.slave_id, frame->counter, frame->command, button,
                                                release_phase, tx);
      schedule_tx_(engine, tx, tx_len, HCP2_PENDING_TX_STATUS, (uint8_t) frame->type, rx_complete_us);
      break;
    case HCP2_FRAME_COMMAND_ARG:
      tx_len = hcp2_frame_build_command_response(engine->config.slave_id, frame->counter, frame->command, tx);
      schedule_tx_(engine, tx, tx_len, HCP2_PENDING_TX_OTHER, (uint8_t) frame->type, rx_complete_us);
      break;
    case HCP2_FRAME_OTHER_VALID:
    case HCP2_FRAME_NONE:
    default:
      break;
  }
}

static void process_rx_(hcp2_engine_t *engine) {
  while (engine->rx_len > 0u) {
    uint8_t expected_len = 0u;
    hcp2_decoded_frame_t frame;
    const uint8_t expected_ok = hcp2_frame_master_expected_len(engine->rx, engine->rx_len, &expected_len);

    if (!expected_ok) {
      rx_drop_(engine, 1u);
      continue;
    }
    if (expected_len == 0u || engine->rx_len < expected_len) {
      return;
    }

    switch (hcp2_frame_parse_master(engine->rx, expected_len, engine->config.slave_id, &frame)) {
      case HCP2_PARSE_OK:
      {
        const uint32_t rx_complete_us = now_us_(engine);
        engine->valid_frames++;
        record_protocol_event_(engine, (uint8_t) HCP2_PROTOCOL_EVENT_RX, (uint8_t) frame.type, engine->rx,
                               expected_len, rx_complete_us);
        handle_decoded_(engine, &frame, rx_complete_us);
        rx_drop_(engine, expected_len);
        break;
      }
      case HCP2_PARSE_BAD_CRC:
        engine->crc_errors++;
        record_protocol_event_(engine, (uint8_t) HCP2_PROTOCOL_EVENT_BAD_CRC, (uint8_t) HCP2_FRAME_NONE,
                               engine->rx, expected_len, now_us_(engine));
        rx_drop_(engine, 1u);
        break;
      case HCP2_PARSE_INVALID:
        rx_drop_(engine, 1u);
        break;
      case HCP2_PARSE_INCOMPLETE:
      default:
        return;
    }
  }
}

void hcp2_engine_config_default(hcp2_engine_config_t *config) {
  if (config == 0) {
    return;
  }
  config->slave_id = HCP2_DEFAULT_SLAVE_ID;
  hcp2_default_signature(config->signature);
  config->response_delay_us = HCP2_DEFAULT_RESPONSE_DELAY_US;
  config->button_press_us = HCP2_DEFAULT_BUTTON_PRESS_US;
}

void hcp2_engine_init(hcp2_engine_t *engine, const hcp2_port_t *port, const hcp2_engine_config_t *config) {
  hcp2_engine_config_t default_config;

  if (engine == 0) {
    return;
  }
  memset(engine, 0, sizeof(*engine));
  if (port != 0) {
    engine->port = *port;
  }
  hcp2_engine_config_default(&default_config);
  engine->config = config != 0 ? *config : default_config;
  if (engine->config.slave_id == 0u) {
    engine->config.slave_id = HCP2_DEFAULT_SLAVE_ID;
  }
}

void hcp2_engine_rx_byte(hcp2_engine_t *engine, uint8_t byte, uint8_t flags) {
  if (engine == 0) {
    return;
  }
  if ((flags & (uint8_t) (HCP2_RX_PARITY_ERROR | HCP2_RX_FRAMING_ERROR)) != 0u) {
    engine->rx_errors++;
    engine->rx_len = 0u;
    record_protocol_event_(engine, (uint8_t) HCP2_PROTOCOL_EVENT_RX_ERROR, (uint8_t) HCP2_FRAME_NONE, 0, 0u,
                           now_us_(engine));
    return;
  }

  if (engine->rx_len >= HCP2_MAX_FRAME_LEN) {
    rx_drop_(engine, 1u);
  }
  engine->rx[engine->rx_len++] = byte;
  process_rx_(engine);
}

void hcp2_engine_poll(hcp2_engine_t *engine) {
  uint32_t tx_start_us;
  uint32_t tx_done_us;
  uint8_t pending_kind;

  if (engine == 0 || !engine->pending_tx_ready) {
    return;
  }
  if (!time_reached_(now_us_(engine), engine->pending_tx_due_us)) {
    return;
  }

  pending_kind = engine->pending_tx_kind;
  tx_start_us = now_us_(engine);
  record_protocol_event_(engine, (uint8_t) HCP2_PROTOCOL_EVENT_TX, engine->pending_tx_frame_type,
                         engine->pending_tx, engine->pending_tx_len, tx_start_us);
  if (pending_kind == HCP2_PENDING_TX_STATUS) {
    record_max_(&engine->max_status_response_schedule_to_tx_start_us, tx_start_us - engine->pending_tx_scheduled_us);
  }
  if (engine->port.de_set != 0) {
    engine->port.de_set(engine->port.user, 1u);
  }
  if (engine->port.tx != 0) {
    engine->port.tx(engine->port.user, engine->pending_tx, engine->pending_tx_len);
  }
  if (engine->port.de_set != 0) {
    engine->port.de_set(engine->port.user, 0u);
  }
  tx_done_us = now_us_(engine);
  if (pending_kind == HCP2_PENDING_TX_STATUS) {
    record_max_(&engine->max_status_response_tx_us, tx_done_us - tx_start_us);
  }

  engine->responses_sent++;
  if (pending_kind == HCP2_PENDING_TX_STATUS) {
    engine->status_responses_sent++;
  }
  engine->pending_tx_ready = 0u;
  engine->pending_tx_len = 0u;
  engine->pending_tx_kind = HCP2_PENDING_TX_OTHER;
  engine->pending_tx_frame_type = (uint8_t) HCP2_FRAME_NONE;
}

uint8_t hcp2_engine_press_button(hcp2_engine_t *engine, hcp2_button_t button) {
  if (engine == 0 || button == HCP2_BUTTON_NONE) {
    return 0u;
  }
  if (engine->active_button != HCP2_BUTTON_NONE) {
    return 0u;
  }
  engine->active_button = button;
  engine->release_pending = 1u;
  engine->press_until_us = now_us_(engine) + engine->config.button_press_us;
  return 1u;
}

const hcp2_drive_status_t *hcp2_engine_drive_status(const hcp2_engine_t *engine) {
  if (engine == 0) {
    return 0;
  }
  return &engine->drive_status;
}

uint8_t hcp2_engine_read_protocol_event(const hcp2_engine_t *engine, uint32_t *last_sequence,
                                        hcp2_protocol_event_t *out) {
#if HCP2_ENABLE_PROTOCOL_EVENTS
  if (engine == 0 || out == 0) {
    return 0u;
  }
  if (last_sequence != 0 && engine->protocol_event.sequence == *last_sequence) {
    return 0u;
  }
  *out = engine->protocol_event;
  if (last_sequence != 0) {
    *last_sequence = out->sequence;
  }
  return out->sequence != 0u ? 1u : 0u;
#else
  (void) engine;
  (void) last_sequence;
  (void) out;
  return 0u;
#endif
}
