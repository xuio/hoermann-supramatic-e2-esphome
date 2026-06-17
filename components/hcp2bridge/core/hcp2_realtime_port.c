#include "hcp2_realtime_port.h"

#include <string.h>

static uint8_t time_reached_(uint32_t now, uint32_t due) {
  return ((int32_t) (now - due)) >= 0 ? 1u : 0u;
}

static void record_max_(uint32_t *field, uint32_t value) {
  if (field != 0 && value > *field) {
    *field = value;
  }
}

void hcp2_realtime_port_model_init(hcp2_realtime_port_model_t *model, uint32_t deadman_us) {
  if (model == 0) {
    return;
  }
  memset(model, 0, sizeof(*model));
  model->deadman_us = deadman_us != 0u ? deadman_us : HCP2_REALTIME_DEFAULT_DEADMAN_US;
}

uint8_t hcp2_realtime_tx_slot_init(hcp2_realtime_tx_slot_t *slot, const uint8_t *data, uint8_t len,
                                   uint8_t frame_type, uint32_t due_us, uint32_t tail_margin_us) {
  if (slot == 0 || data == 0 || len == 0u || len > HCP2_MAX_FRAME_LEN) {
    return 0u;
  }
  memset(slot, 0, sizeof(*slot));
  slot->due_us = due_us;
  slot->tail_margin_us = tail_margin_us;
  slot->frame_type = frame_type;
  slot->len = len;
  memcpy(slot->data, data, len);
  return 1u;
}

uint8_t hcp2_realtime_port_submit_tx_slot(hcp2_realtime_port_model_t *model,
                                          const hcp2_realtime_tx_slot_t *slot) {
  if (model == 0 || slot == 0 || slot->len == 0u || slot->len > HCP2_MAX_FRAME_LEN) {
    return 0u;
  }
  if (model->slot_valid != 0u || model->de_enabled != 0u) {
    model->counters.slot_drop_count++;
    return 0u;
  }
  model->slot = *slot;
  model->slot_valid = 1u;
  return 1u;
}

uint8_t hcp2_realtime_port_service_timer(hcp2_realtime_port_model_t *model, uint32_t now_us) {
  uint32_t late_us;

  if (model == 0 || model->slot_valid == 0u) {
    return 0u;
  }
  if (!time_reached_(now_us, model->slot.due_us)) {
    return 0u;
  }

  late_us = now_us - model->slot.due_us;
  record_max_(&model->counters.timer_late_us, late_us);
  record_max_(&model->counters.isr_max_time_us, 1u);
  if (model->slot.tail_margin_us != 0u && late_us > model->slot.tail_margin_us) {
    model->counters.tx_underrun_count++;
    model->slot_valid = 0u;
    return 0u;
  }

  model->de_enabled = 1u;
  model->de_enabled_since_us = now_us;
  model->tx_capture_len = model->slot.len;
  model->tx_capture_frame_type = model->slot.frame_type;
  memcpy(model->tx_capture, model->slot.data, model->slot.len);
  model->slot_valid = 0u;
  model->de_enabled = 0u;
  return 1u;
}

uint8_t hcp2_realtime_port_push_rx(hcp2_realtime_port_model_t *model, uint8_t byte, uint8_t flags) {
  if (model == 0) {
    return 0u;
  }
  if (model->rx_count >= HCP2_REALTIME_RX_FIFO_CAPACITY) {
    return 0u;
  }
  model->rx_fifo[model->rx_head] = byte;
  model->rx_flags[model->rx_head] = flags;
  model->rx_head = (uint16_t) ((model->rx_head + 1u) % HCP2_REALTIME_RX_FIFO_CAPACITY);
  model->rx_count++;
  record_max_(&model->counters.rx_fifo_high_water, model->rx_count);
  return 1u;
}

uint8_t hcp2_realtime_port_pop_rx(hcp2_realtime_port_model_t *model, uint8_t *byte, uint8_t *flags) {
  if (model == 0 || model->rx_count == 0u || byte == 0 || flags == 0) {
    return 0u;
  }
  *byte = model->rx_fifo[model->rx_tail];
  *flags = model->rx_flags[model->rx_tail];
  model->rx_tail = (uint16_t) ((model->rx_tail + 1u) % HCP2_REALTIME_RX_FIFO_CAPACITY);
  model->rx_count--;
  return 1u;
}

void hcp2_realtime_port_check_de_deadman(hcp2_realtime_port_model_t *model, uint32_t now_us) {
  if (model == 0 || model->de_enabled == 0u) {
    return;
  }
  if (model->deadman_us != 0u && time_reached_(now_us, model->de_enabled_since_us + model->deadman_us)) {
    model->counters.de_deadman_count++;
    model->de_enabled = 0u;
  }
}
