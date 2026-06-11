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

static void schedule_tx_(hcp2_engine_t *engine, const uint8_t *data, uint8_t len) {
  if (len == 0u || len > HCP2_MAX_FRAME_LEN) {
    return;
  }
  memcpy(engine->pending_tx, data, len);
  engine->pending_tx_len = len;
  engine->pending_tx_due_us = now_us_(engine) + engine->config.response_delay_us;
  engine->pending_tx_ready = 1u;
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

static void handle_decoded_(hcp2_engine_t *engine, const hcp2_decoded_frame_t *frame) {
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
      schedule_tx_(engine, tx, tx_len);
      break;
    case HCP2_FRAME_STATUS_POLL:
      button = current_button_phase_(engine, &release_phase);
      tx_len = hcp2_frame_build_status_response(engine->config.slave_id, frame->counter, frame->command, button,
                                                release_phase, tx);
      schedule_tx_(engine, tx, tx_len);
      break;
    case HCP2_FRAME_COMMAND_ARG:
      tx_len = hcp2_frame_build_command_response(engine->config.slave_id, frame->counter, frame->command, tx);
      schedule_tx_(engine, tx, tx_len);
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
        engine->valid_frames++;
        handle_decoded_(engine, &frame);
        rx_drop_(engine, expected_len);
        break;
      case HCP2_PARSE_BAD_CRC:
        engine->crc_errors++;
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
    return;
  }

  if (engine->rx_len >= HCP2_MAX_FRAME_LEN) {
    rx_drop_(engine, 1u);
  }
  engine->rx[engine->rx_len++] = byte;
  process_rx_(engine);
}

void hcp2_engine_poll(hcp2_engine_t *engine) {
  if (engine == 0 || !engine->pending_tx_ready) {
    return;
  }
  if (!time_reached_(now_us_(engine), engine->pending_tx_due_us)) {
    return;
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

  engine->responses_sent++;
  engine->pending_tx_ready = 0u;
  engine->pending_tx_len = 0u;
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
