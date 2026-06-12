#include "hcp2bridge.h"
#include "hcp2_entity_mapping.h"

#include <cstring>

extern "C" {
#include "hcp2_crc.c"
#include "hcp2_frame.c"
#include "hcp2_engine.c"
#include "hcp2_mailbox.c"
}

#ifdef USE_ESP32
#include "driver/gpio.h"
#include "esp_timer.h"
#endif

namespace esphome {
namespace hcp2bridge {

static const char *const TAG = "hcp2bridge";
static constexpr uint32_t HCP2BRIDGE_BAUD_RATE = 57600;
static constexpr size_t HCP2BRIDGE_RX_BUFFER_SIZE = 256;
static constexpr size_t HCP2BRIDGE_TX_BUFFER_SIZE = 256;
static constexpr size_t HCP2BRIDGE_UART_EVENT_QUEUE_LEN = 32;
static constexpr size_t HCP2BRIDGE_COMMAND_QUEUE_LEN = 8;
static constexpr uint32_t HCP2BRIDGE_BUS_TASK_STACK_BYTES = 6144;
static constexpr uint32_t HCP2BRIDGE_OBSTRUCTION_COMMAND_GRACE_MS = 2000;
static constexpr uint32_t HCP2BRIDGE_OBSTRUCTION_LATCH_MS = 10000;

HCP2Bridge::HCP2Bridge() { hcp2_engine_config_default(&this->config_); }

void HCP2Bridge::setup() {
  if (!this->hp_fallback_) {
    ESP_LOGE(TAG, "LP-core production path is not implemented in the ESPHome component yet");
    this->mark_failed();
    return;
  }

#ifdef USE_ESP32
  ESP_LOGW(TAG, "HP fallback responder is enabled for bring-up/emulation only");
  if (!this->setup_uart_()) {
    this->mark_failed();
    return;
  }
  this->start_hp_fallback_task_();
#else
  ESP_LOGE(TAG, "hcp2bridge requires ESP32/ESP-IDF");
  this->mark_failed();
#endif
}

void HCP2Bridge::loop() {
  bool state_pending = false;
  bool command_pending = false;

#ifdef USE_ESP32
  portENTER_CRITICAL(&this->state_mux_);
#endif
  state_pending = this->state_callback_pending_;
  command_pending = this->command_callback_pending_;
  this->state_callback_pending_ = false;
  this->command_callback_pending_ = false;
#ifdef USE_ESP32
  portEXIT_CRITICAL(&this->state_mux_);
#endif

  if (state_pending) {
    this->state_callback_.call();
  }
  if (command_pending) {
    this->command_callback_.call();
  }
}

void HCP2Bridge::dump_config() {
  ESP_LOGCONFIG(TAG, "HCP2 Bridge:");
  LOG_PIN("  RX Pin: ", this->rx_pin_);
  LOG_PIN("  TX Pin: ", this->tx_pin_);
  LOG_PIN("  DE Pin: ", this->de_pin_);
  LOG_PIN("  /RE Pin: ", this->re_pin_);
  ESP_LOGCONFIG(TAG, "  UART: %u", (unsigned int) this->uart_num_config_);
  ESP_LOGCONFIG(TAG, "  Baud Rate: %u 8E1", (unsigned int) HCP2BRIDGE_BAUD_RATE);
  ESP_LOGCONFIG(TAG, "  Slave ID: %u", (unsigned int) this->config_.slave_id);
  ESP_LOGCONFIG(TAG, "  Response Delay: %uus", (unsigned int) this->config_.response_delay_us);
  ESP_LOGCONFIG(TAG, "  Button Press Duration: %uus", (unsigned int) this->config_.button_press_us);
  ESP_LOGCONFIG(TAG, "  HP Fallback: %s", this->hp_fallback_ ? "enabled" : "disabled");
}

void HCP2Bridge::set_signature_byte(uint8_t index, uint8_t value) {
  if (index >= HCP2_SIGNATURE_LEN) {
    return;
  }
  this->config_.signature[index] = value;
}

bool HCP2Bridge::has_valid_broadcast() const { return this->valid_broadcast_snapshot_(); }

bool HCP2Bridge::is_light_on() const { return hcp2_state_is_light_on(this->drive_status_snapshot_()); }

bool HCP2Bridge::is_moving() const { return hcp2_state_is_moving(this->drive_status_snapshot_()); }

bool HCP2Bridge::is_open() const { return hcp2_state_is_open(this->drive_status_snapshot_()); }

bool HCP2Bridge::is_closed() const { return hcp2_state_is_closed(this->drive_status_snapshot_()); }

bool HCP2Bridge::is_obstructed() const { return this->obstruction_snapshot_(); }

hcp2_drive_state_code_t HCP2Bridge::get_drive_state() const {
  return (hcp2_drive_state_code_t) this->drive_status_snapshot_().state;
}

float HCP2Bridge::get_position() const { return hcp2_state_position(this->drive_status_snapshot_()); }

std::string HCP2Bridge::get_state_string() const { return this->drive_state_name_(); }

uint32_t HCP2Bridge::get_command_sequence() const { return this->counter_snapshot_(&HCP2Bridge::command_sequence_); }

uint32_t HCP2Bridge::get_valid_frame_count() const { return this->counter_snapshot_(&HCP2Bridge::valid_frames_); }

uint32_t HCP2Bridge::get_crc_error_count() const { return this->counter_snapshot_(&HCP2Bridge::crc_errors_); }

uint32_t HCP2Bridge::get_rx_error_count() const { return this->counter_snapshot_(&HCP2Bridge::rx_errors_); }

uint32_t HCP2Bridge::get_response_count() const { return this->counter_snapshot_(&HCP2Bridge::responses_sent_); }

hcp2_drive_status_t HCP2Bridge::drive_status_snapshot_() const {
#ifdef USE_ESP32
  portENTER_CRITICAL(&this->state_mux_);
#endif
  const hcp2_drive_status_t status = this->drive_status_;
#ifdef USE_ESP32
  portEXIT_CRITICAL(&this->state_mux_);
#endif
  return status;
}

bool HCP2Bridge::valid_broadcast_snapshot_() const {
#ifdef USE_ESP32
  portENTER_CRITICAL(&this->state_mux_);
#endif
  const bool value = this->valid_broadcast_;
#ifdef USE_ESP32
  portEXIT_CRITICAL(&this->state_mux_);
#endif
  return value;
}

bool HCP2Bridge::obstruction_snapshot_() const {
#ifdef USE_ESP32
  portENTER_CRITICAL(&this->state_mux_);
#endif
  const bool value = this->obstruction_;
#ifdef USE_ESP32
  portEXIT_CRITICAL(&this->state_mux_);
#endif
  return value;
}

uint32_t HCP2Bridge::counter_snapshot_(uint32_t HCP2Bridge::*field) const {
#ifdef USE_ESP32
  portENTER_CRITICAL(&this->state_mux_);
#endif
  const uint32_t value = this->*field;
#ifdef USE_ESP32
  portEXIT_CRITICAL(&this->state_mux_);
#endif
  return value;
}

const char *HCP2Bridge::drive_state_name_() const {
  return hcp2_state_name(static_cast<hcp2_drive_state_code_t>(this->drive_status_snapshot_().state));
}

bool HCP2Bridge::queue_button_(hcp2_button_t button) {
  if (button == HCP2_BUTTON_NONE) {
    return false;
  }

#ifdef USE_ESP32
  if (this->command_queue_ == nullptr || this->bus_task_handle_ == nullptr) {
    ESP_LOGW(TAG, "Cannot queue command before HP fallback task is running");
    return false;
  }

  CommandEvent event{button};
  if (xQueueSend(this->command_queue_, &event, 0) != pdTRUE) {
    ESP_LOGW(TAG, "HCP2 command queue is full");
    return false;
  }
  xTaskNotifyGive(this->bus_task_handle_);

  portENTER_CRITICAL(&this->state_mux_);
  this->last_commanded_button_ = button;
  this->last_commanded_ms_ = millis();
  this->command_sequence_++;
  this->command_callback_pending_ = true;
  portEXIT_CRITICAL(&this->state_mux_);
  return true;
#else
  return false;
#endif
}

#ifdef USE_ESP32
bool HCP2Bridge::setup_uart_() {
  if (this->rx_pin_ == nullptr || this->tx_pin_ == nullptr || this->de_pin_ == nullptr) {
    ESP_LOGE(TAG, "rx_pin, tx_pin, and de_pin are required");
    return false;
  }

  this->uart_num_ = static_cast<uart_port_t>(this->uart_num_config_);
  this->de_pin_->setup();
  this->de_pin_->pin_mode(gpio::Flags::FLAG_OUTPUT | gpio::Flags::FLAG_PULLDOWN);
  this->de_pin_->digital_write(false);
  if (this->re_pin_ != nullptr) {
    this->re_pin_->setup();
    this->re_pin_->pin_mode(gpio::Flags::FLAG_OUTPUT | gpio::Flags::FLAG_PULLDOWN);
    this->re_pin_->digital_write(false);
  }

  if (uart_is_driver_installed(this->uart_num_)) {
    ESP_LOGW(TAG, "UART%u already has a driver; replacing it for hcp2bridge", (unsigned int) this->uart_num_config_);
    uart_driver_delete(this->uart_num_);
  }

  uart_config_t uart_config{};
  uart_config.baud_rate = HCP2BRIDGE_BAUD_RATE;
  uart_config.data_bits = UART_DATA_8_BITS;
  uart_config.parity = UART_PARITY_EVEN;
  uart_config.stop_bits = UART_STOP_BITS_1;
  uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_config.rx_flow_ctrl_thresh = 0;
  uart_config.source_clk = UART_SCLK_DEFAULT;

  esp_err_t err = uart_driver_install(this->uart_num_, HCP2BRIDGE_RX_BUFFER_SIZE, HCP2BRIDGE_TX_BUFFER_SIZE,
                                      HCP2BRIDGE_UART_EVENT_QUEUE_LEN, &this->uart_event_queue_, ESP_INTR_FLAG_IRAM);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
    return false;
  }
  err = uart_param_config(this->uart_num_, &uart_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
    return false;
  }
  err = uart_set_pin(this->uart_num_, this->tx_pin_->get_pin(), this->rx_pin_->get_pin(), UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
    return false;
  }
  err = uart_set_mode(this->uart_num_, UART_MODE_UART);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uart_set_mode failed: %s", esp_err_to_name(err));
    return false;
  }
  this->uart_ready_ = true;
  return true;
}

void HCP2Bridge::start_hp_fallback_task_() {
  if (this->bus_task_handle_ != nullptr) {
    return;
  }
  this->command_queue_ = xQueueCreate(HCP2BRIDGE_COMMAND_QUEUE_LEN, sizeof(CommandEvent));
  if (this->command_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create HCP2 command queue");
    this->mark_failed();
    return;
  }

  const hcp2_port_t port = {
      .user = this,
      .now_us = HCP2Bridge::now_us_cb_,
      .tx = HCP2Bridge::tx_cb_,
      .de_set = HCP2Bridge::de_set_cb_,
  };
  hcp2_engine_init(&this->engine_, &port, &this->config_);

  const BaseType_t ok = xTaskCreatePinnedToCore(HCP2Bridge::bus_task_trampoline_, "hcp2_hp_bus",
                                                HCP2BRIDGE_BUS_TASK_STACK_BYTES, this,
                                                configMAX_PRIORITIES - 1, &this->bus_task_handle_,
                                                tskNO_AFFINITY);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to start HCP2 HP fallback task");
    this->bus_task_handle_ = nullptr;
    this->mark_failed();
    return;
  }
  this->bus_task_started_ = true;
  ESP_LOGI(TAG, "Started HCP2 HP fallback task priority=%u stack=%u bytes",
           (unsigned int) (configMAX_PRIORITIES - 1), (unsigned int) HCP2BRIDGE_BUS_TASK_STACK_BYTES);
}

void HCP2Bridge::bus_task_trampoline_(void *arg) {
  auto *self = static_cast<HCP2Bridge *>(arg);
  if (self == nullptr) {
    vTaskDelete(nullptr);
    return;
  }
  self->bus_task_loop_();
}

void HCP2Bridge::bus_task_loop_() {
  uart_event_t event;
  uint8_t rx[64];

  for (;;) {
    this->drain_command_queue_();
    hcp2_engine_poll(&this->engine_);
    this->update_state_from_engine_();

    TickType_t wait_ticks = this->pending_tx_wait_ticks_();
    if (xQueueReceive(this->uart_event_queue_, &event, wait_ticks) != pdTRUE) {
      continue;
    }

    switch (event.type) {
      case UART_DATA: {
        size_t remaining = event.size;
        while (remaining > 0) {
          const size_t request = std::min(remaining, sizeof(rx));
          const int got = uart_read_bytes(this->uart_num_, rx, request, 0);
          if (got <= 0) {
            break;
          }
          remaining -= (size_t) got;
          for (int i = 0; i < got; i++) {
            hcp2_engine_rx_byte(&this->engine_, rx[i], HCP2_RX_OK);
          }
        }
        break;
      }
      case UART_FIFO_OVF:
      case UART_BUFFER_FULL:
        ESP_LOGW(TAG, "UART RX overflow; resetting HCP2 parser");
        uart_flush_input(this->uart_num_);
        xQueueReset(this->uart_event_queue_);
        hcp2_engine_rx_byte(&this->engine_, 0, HCP2_RX_FRAMING_ERROR);
        break;
      case UART_PARITY_ERR:
        hcp2_engine_rx_byte(&this->engine_, 0, HCP2_RX_PARITY_ERROR);
        break;
      case UART_FRAME_ERR:
      case UART_BREAK:
        hcp2_engine_rx_byte(&this->engine_, 0, HCP2_RX_FRAMING_ERROR);
        break;
      default:
        break;
    }
  }
}

void HCP2Bridge::drain_command_queue_() {
  if (this->command_queue_ == nullptr) {
    return;
  }
  CommandEvent event{};
  while (xQueueReceive(this->command_queue_, &event, 0) == pdTRUE) {
    if (!hcp2_engine_press_button(&this->engine_, event.button)) {
      ESP_LOGW(TAG, "Dropped HCP2 command because another button press is active");
    }
  }
  ulTaskNotifyTake(pdTRUE, 0);
}

void HCP2Bridge::update_state_from_engine_() {
  const hcp2_drive_status_t *status = hcp2_engine_drive_status(&this->engine_);
  const uint32_t now_ms = millis();
  bool changed = false;

  portENTER_CRITICAL(&this->state_mux_);
  if (status != nullptr && std::memcmp(&this->drive_status_, status, sizeof(this->drive_status_)) != 0) {
    const hcp2_drive_state_code_t previous_state = static_cast<hcp2_drive_state_code_t>(this->drive_status_.state);
    const hcp2_drive_state_code_t current_state = static_cast<hcp2_drive_state_code_t>(status->state);
    hcp2_button_t recent_command = HCP2_BUTTON_NONE;
    if (this->last_commanded_ms_ != 0 &&
        (int32_t) (now_ms - (this->last_commanded_ms_ + HCP2BRIDGE_OBSTRUCTION_COMMAND_GRACE_MS)) <= 0) {
      recent_command = this->last_commanded_button_;
    }
    if (this->valid_broadcast_ &&
        hcp2_is_uncommanded_closing_reversal(previous_state, current_state, recent_command)) {
      this->obstruction_ = true;
      this->obstruction_until_ms_ = now_ms + HCP2BRIDGE_OBSTRUCTION_LATCH_MS;
      changed = true;
    }
    this->drive_status_ = *status;
    changed = true;
  }
  if (!this->valid_broadcast_ && this->engine_.broadcasts_received > 0) {
    this->valid_broadcast_ = true;
    changed = true;
  }
  if (this->obstruction_ && (int32_t) (now_ms - this->obstruction_until_ms_) >= 0) {
    this->obstruction_ = false;
    changed = true;
  }
  this->valid_frames_ = this->engine_.valid_frames;
  this->crc_errors_ = this->engine_.crc_errors;
  this->rx_errors_ = this->engine_.rx_errors;
  this->responses_sent_ = this->engine_.responses_sent;
  if (changed) {
    this->state_callback_pending_ = true;
  }
  portEXIT_CRITICAL(&this->state_mux_);
}

TickType_t HCP2Bridge::pending_tx_wait_ticks_() const {
  if (!this->engine_.pending_tx_ready) {
    return portMAX_DELAY;
  }
  const uint32_t now = now_us_cb_(const_cast<HCP2Bridge *>(this));
  const int32_t remaining_us = (int32_t) (this->engine_.pending_tx_due_us - now);
  if (remaining_us <= 0) {
    return 0;
  }
  const uint32_t remaining_ms = ((uint32_t) remaining_us + 999u) / 1000u;
  return pdMS_TO_TICKS(std::max<uint32_t>(1u, remaining_ms));
}

void HCP2Bridge::set_de_(bool enabled) {
  if (this->de_pin_ != nullptr) {
    this->de_pin_->digital_write(enabled);
  }
}

uint32_t HCP2Bridge::now_us_cb_(void *user) {
  (void) user;
  return (uint32_t) esp_timer_get_time();
}

void HCP2Bridge::tx_cb_(void *user, const uint8_t *data, uint8_t len) {
  auto *self = static_cast<HCP2Bridge *>(user);
  uint8_t written_total = 0;
  while (written_total < len) {
    const int written = uart_write_bytes(self->uart_num_, data + written_total, len - written_total);
    if (written > 0) {
      written_total = (uint8_t) (written_total + written);
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
  uart_wait_tx_done(self->uart_num_, pdMS_TO_TICKS(20));
}

void HCP2Bridge::de_set_cb_(void *user, uint8_t enabled) {
  auto *self = static_cast<HCP2Bridge *>(user);
  self->set_de_(enabled != 0);
}
#endif

}  // namespace hcp2bridge
}  // namespace esphome
