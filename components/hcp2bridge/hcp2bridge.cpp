#include "hcp2bridge.h"
#include "hcp2_entity_mapping.h"
#include "hcp2_lp_blob.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <inttypes.h>

extern "C" {
#include "hcp2_crc.h"
#include "hcp2_engine.h"
#include "hcp2_frame.h"
#include "hcp2_mailbox.h"
#include "hcp2_supervisor.h"
}

#ifdef USE_ESP32
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hal/uart_types.h"
#include "lp_core_uart.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"
#include "ulp_lp_core.h"
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
static constexpr uint32_t HCP2BRIDGE_LP_TASK_STACK_BYTES = 4096;
static constexpr uint32_t HCP2BRIDGE_LP_HEARTBEAT_PROBE_MS = 20;
static constexpr uint32_t HCP2BRIDGE_LP_MAILBOX_TIMEOUT_MS = 250;
static constexpr uint32_t HCP2BRIDGE_LP_HEALTH_LOG_INTERVAL_MS = 5000;
static constexpr uint32_t HCP2BRIDGE_BUS_ONLINE_TIMEOUT_US = 1000000;
static constexpr uint32_t HCP2BRIDGE_OBSTRUCTION_COMMAND_GRACE_MS = 2000;
static constexpr uint32_t HCP2BRIDGE_OBSTRUCTION_LATCH_MS = 10000;
static constexpr uint32_t HCP2BRIDGE_HTTP_SETUP_DELAY_MS = 5000;
static constexpr uint32_t HCP2BRIDGE_HTTP_SETUP_RETRY_MS = 5000;
static constexpr uint32_t HCP2BRIDGE_HTTP_PENDING_TIMEOUT_MS = 3000;
static constexpr uint32_t HCP2BRIDGE_HTTP_WS_SEND_INTERVAL_MS = 250;
static constexpr uint32_t HCP2BRIDGE_HTTP_WS_HEALTH_INTERVAL_MS = 500;
static constexpr size_t HCP2BRIDGE_HTTP_WS_MAX_CHUNK_BYTES = 2048;
static constexpr uint32_t HCP2BRIDGE_MAX_DE_HIGH_US = 9000;
static constexpr uint32_t HCP2BRIDGE_PENDING_REPLY_GRACE_MS = 20;
static constexpr const char *HCP2BRIDGE_WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static bool hcp2_ascii_iequals(const std::string &left, const char *right) {
  if (right == nullptr) {
    return left.empty();
  }
  const size_t right_len = std::strlen(right);
  if (left.size() != right_len) {
    return false;
  }
  for (size_t i = 0; i < left.size(); i++) {
    if (std::tolower((unsigned char) left[i]) != std::tolower((unsigned char) right[i])) {
      return false;
    }
  }
  return true;
}

static uint32_t hcp2_raw_missed_polls(uint32_t polls_seen, uint32_t polls_answered) {
  return polls_seen >= polls_answered ? polls_seen - polls_answered : 0u;
}

static bool hcp2_pending_reply(uint32_t polls_seen, uint32_t polls_answered, uint32_t last_poll_age_ms) {
  return hcp2_raw_missed_polls(polls_seen, polls_answered) == 1u &&
         last_poll_age_ms <= HCP2BRIDGE_PENDING_REPLY_GRACE_MS;
}

static uint32_t hcp2_effective_missed_polls(uint32_t polls_seen, uint32_t polls_answered,
                                            uint32_t last_poll_age_ms, bool *pending_response) {
  const bool pending = hcp2_pending_reply(polls_seen, polls_answered, last_poll_age_ms);
  if (pending_response != nullptr) {
    *pending_response = pending;
  }
  return pending ? 0u : hcp2_raw_missed_polls(polls_seen, polls_answered);
}

#ifdef USE_ESP32
RTC_DATA_ATTR static uint32_t hcp2_hp_reset_count;
RTC_DATA_ATTR static uint32_t hcp2_hp_panic_reset_count;
RTC_DATA_ATTR static uint32_t hcp2_hp_wdt_reset_count;
RTC_DATA_ATTR static uint32_t hcp2_hp_brownout_reset_count;
static bool hcp2_hp_reset_recorded;
#endif

HCP2Bridge::HCP2Bridge() { hcp2_engine_config_default(&this->config_); }

void HCP2Bridge::setup() {
#ifdef USE_ESP32
  this->setup_protocol_log_();
  if (this->http_debug_enabled_()) {
    this->http_debug_next_setup_ms_ = millis() + HCP2BRIDGE_HTTP_SETUP_DELAY_MS;
  }
  this->record_hp_reset_reason_();
  this->protocol_log_append_control_("boot");
  if (!this->hp_fallback_) {
    if (!this->setup_lp_core_()) {
      this->mark_failed();
      return;
    }
    this->start_lp_supervisor_task_();
    return;
  }

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
  if (this->http_debug_enabled_()) {
    this->maybe_setup_http_debug_server_();
    this->http_debug_accept_client_();
    this->http_debug_service_pending_client_();
    this->http_debug_service_log_ws_();
  }
#endif

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
  ESP_LOGCONFIG(TAG, "  HTTP Debug Port: %u", (unsigned int) this->http_debug_port_);
  ESP_LOGCONFIG(TAG, "  Protocol Log: %s", this->protocol_log_enabled_ ? "enabled" : "disabled");
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

bool HCP2Bridge::is_bus_online() const {
#ifdef USE_ESP32
  portENTER_CRITICAL(&this->state_mux_);
#endif
  const bool value = this->bus_online_;
#ifdef USE_ESP32
  portEXIT_CRITICAL(&this->state_mux_);
#endif
  return value;
}

bool HCP2Bridge::is_continuity_healthy() const {
#ifdef USE_ESP32
  portENTER_CRITICAL(&this->state_mux_);
#endif
  const bool value = this->continuity_healthy_;
#ifdef USE_ESP32
  portEXIT_CRITICAL(&this->state_mux_);
#endif
  return value;
}

bool HCP2Bridge::is_safe_for_ota_restart() const { return this->is_continuity_healthy(); }

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

uint32_t HCP2Bridge::get_lp_heartbeat() const { return this->counter_snapshot_(&HCP2Bridge::lp_heartbeat_); }

uint32_t HCP2Bridge::get_lp_reset_count() const { return this->counter_snapshot_(&HCP2Bridge::lp_reset_count_); }

uint32_t HCP2Bridge::get_lp_poll_count() const { return this->counter_snapshot_(&HCP2Bridge::lp_polls_seen_); }

uint32_t HCP2Bridge::get_lp_response_count() const { return this->counter_snapshot_(&HCP2Bridge::lp_polls_answered_); }

uint32_t HCP2Bridge::get_lp_missed_poll_count() const {
#ifdef USE_ESP32
  portENTER_CRITICAL(&this->state_mux_);
#endif
  const uint32_t raw_missed = this->lp_raw_missed_polls_;
  const bool pending_response = this->lp_pending_response_;
#ifdef USE_ESP32
  portEXIT_CRITICAL(&this->state_mux_);
#endif
  return pending_response ? 0u : raw_missed;
}

uint32_t HCP2Bridge::get_lp_raw_missed_poll_count() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_raw_missed_polls_);
}

bool HCP2Bridge::has_lp_pending_response() const {
#ifdef USE_ESP32
  portENTER_CRITICAL(&this->state_mux_);
#endif
  const bool value = this->lp_pending_response_;
#ifdef USE_ESP32
  portEXIT_CRITICAL(&this->state_mux_);
#endif
  return value;
}

uint32_t HCP2Bridge::get_lp_tx_abort_count() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_tx_abort_count_);
}

uint32_t HCP2Bridge::get_lp_collision_count() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_collision_count_);
}

uint32_t HCP2Bridge::get_lp_max_de_hold_us() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_max_de_hold_us_);
}

uint32_t HCP2Bridge::get_lp_last_poll_age_ms() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_last_poll_age_ms_);
}

uint32_t HCP2Bridge::get_lp_crc_error_count() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_crc_error_count_);
}

uint32_t HCP2Bridge::get_lp_rx_error_count() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_rx_error_count_);
}

uint32_t HCP2Bridge::get_lp_stop_trigger_fire_count() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_stop_trigger_fire_count_);
}

uint32_t HCP2Bridge::get_lp_health_flags() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_health_flags_);
}

uint32_t HCP2Bridge::get_lp_max_rx_fifo_count() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_max_rx_fifo_count_);
}

uint32_t HCP2Bridge::get_lp_max_loop_us() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_max_loop_us_);
}

uint32_t HCP2Bridge::get_lp_max_poll_rx_to_schedule_us() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_max_poll_rx_to_schedule_us_);
}

uint32_t HCP2Bridge::get_lp_max_response_schedule_to_tx_start_us() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_max_response_schedule_to_tx_start_us_);
}

uint32_t HCP2Bridge::get_lp_max_response_tx_us() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_max_response_tx_us_);
}

uint32_t HCP2Bridge::get_lp_loop_overrun_count() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_loop_overrun_count_);
}

uint32_t HCP2Bridge::get_lp_rx_starvation_count() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_rx_starvation_count_);
}

uint32_t HCP2Bridge::get_lp_stuck_de_count() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_stuck_de_count_);
}

uint32_t HCP2Bridge::get_lp_mailbox_repair_count() const {
  return this->counter_snapshot_(&HCP2Bridge::lp_mailbox_repair_count_);
}

uint32_t HCP2Bridge::get_hp_reset_count() const { return this->counter_snapshot_(&HCP2Bridge::hp_reset_count_); }

uint32_t HCP2Bridge::get_hp_panic_reset_count() const {
  return this->counter_snapshot_(&HCP2Bridge::hp_panic_reset_count_);
}

uint32_t HCP2Bridge::get_hp_wdt_reset_count() const {
  return this->counter_snapshot_(&HCP2Bridge::hp_wdt_reset_count_);
}

uint32_t HCP2Bridge::get_hp_brownout_reset_count() const {
  return this->counter_snapshot_(&HCP2Bridge::hp_brownout_reset_count_);
}

hcp2_lp_command_id_t HCP2Bridge::lp_command_for_button_(hcp2_button_t button) {
  switch (button) {
    case HCP2_BUTTON_OPEN:
      return HCP2_LP_COMMAND_OPEN;
    case HCP2_BUTTON_CLOSE:
      return HCP2_LP_COMMAND_CLOSE;
    case HCP2_BUTTON_STOP:
      return HCP2_LP_COMMAND_STOP;
    case HCP2_BUTTON_VENT:
      return HCP2_LP_COMMAND_VENT;
    case HCP2_BUTTON_HALF:
      return HCP2_LP_COMMAND_HALF;
    case HCP2_BUTTON_LIGHT:
      return HCP2_LP_COMMAND_LIGHT;
    case HCP2_BUTTON_NONE:
    default:
      return HCP2_LP_COMMAND_NONE;
  }
}

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

const char *HCP2Bridge::button_name_(hcp2_button_t button) {
  switch (button) {
    case HCP2_BUTTON_OPEN:
      return "open";
    case HCP2_BUTTON_CLOSE:
      return "close";
    case HCP2_BUTTON_STOP:
      return "stop";
    case HCP2_BUTTON_VENT:
      return "vent";
    case HCP2_BUTTON_HALF:
      return "half";
    case HCP2_BUTTON_LIGHT:
      return "light";
    case HCP2_BUTTON_NONE:
    default:
      return "none";
  }
}

const char *HCP2Bridge::protocol_event_name_(uint8_t event_type) {
  switch ((hcp2_protocol_event_type_t) event_type) {
    case HCP2_PROTOCOL_EVENT_RX:
      return "rx";
    case HCP2_PROTOCOL_EVENT_TX:
      return "tx";
    case HCP2_PROTOCOL_EVENT_BAD_CRC:
      return "bad_crc";
    case HCP2_PROTOCOL_EVENT_RX_ERROR:
      return "rx_error";
    case HCP2_PROTOCOL_EVENT_NONE:
    default:
      return "none";
  }
}

const char *HCP2Bridge::frame_type_name_(uint8_t frame_type) {
  switch ((hcp2_frame_type_t) frame_type) {
    case HCP2_FRAME_STATUS_POLL:
      return "status_poll";
    case HCP2_FRAME_BUS_SCAN:
      return "bus_scan";
    case HCP2_FRAME_COMMAND_ARG:
      return "command_arg";
    case HCP2_FRAME_BROADCAST_STATUS:
      return "broadcast_status";
    case HCP2_FRAME_OTHER_VALID:
      return "other_valid";
    case HCP2_FRAME_NONE:
    default:
      return "none";
  }
}

const char *HCP2Bridge::lp_trace_event_name_(uint16_t event) {
  switch (event) {
    case HCP2_LP_TRACE_BOOT:
      return "boot";
    case HCP2_LP_TRACE_TX:
      return "tx";
    case HCP2_LP_TRACE_COMMAND:
      return "command";
    case HCP2_LP_TRACE_RX:
      return "rx";
    case HCP2_LP_TRACE_RX_ERROR:
      return "rx_error";
    case HCP2_LP_TRACE_DE:
      return "de";
    case HCP2_LP_TRACE_GPIO_RX:
      return "gpio_rx";
    case HCP2_LP_TRACE_RX_ECHO:
      return "rx_echo";
    case HCP2_LP_TRACE_TX_ABORT:
      return "tx_abort";
    case HCP2_LP_TRACE_COLLISION:
      return "collision";
    case HCP2_LP_TRACE_WDT:
      return "wdt";
    case HCP2_LP_TRACE_STOP_TRIGGER:
      return "stop_trigger";
    case HCP2_LP_TRACE_HEALTH:
      return "health";
    default:
      return "unknown";
  }
}

bool HCP2Bridge::arm_stop_trigger(float target_position, uint32_t ttl_ms) {
#ifdef USE_ESP32
  if (this->hp_fallback_) {
    return true;
  }
  if (!this->lp_ready_) {
    return false;
  }
  float clamped = target_position;
  if (clamped < 0.0f) {
    clamped = 0.0f;
  } else if (clamped > 1.0f) {
    clamped = 1.0f;
  }
  const uint8_t raw_target = static_cast<uint8_t>((clamped * 200.0f) + 0.5f);
  const uint32_t ttl_us = ttl_ms == 0u ? 0u : ttl_ms * 1000u;
  return hcp2_hp_supervisor_arm_stop_trigger(&this->lp_supervisor_, raw_target, ttl_us) != 0u;
#else
  return false;
#endif
}

void HCP2Bridge::disarm_stop_trigger() {
#ifdef USE_ESP32
  if (!this->hp_fallback_ && this->lp_ready_) {
    hcp2_hp_supervisor_disarm_stop_trigger(&this->lp_supervisor_);
  }
#endif
}

bool HCP2Bridge::queue_button_(hcp2_button_t button) {
  if (button == HCP2_BUTTON_NONE) {
    return false;
  }

#ifdef USE_ESP32
  if (!this->hp_fallback_) {
    if (!this->lp_ready_) {
      ESP_LOGW(TAG, "Cannot queue command before LP supervisor is ready");
      this->protocol_log_append_command_("queue", button, false, "lp_not_ready");
      return false;
    }
    const hcp2_lp_command_id_t command = lp_command_for_button_(button);
    const uint32_t sequence = hcp2_hp_supervisor_send_command(&this->lp_supervisor_, command, 0u);
    if (sequence == 0u) {
      ESP_LOGW(TAG, "Failed to write HCP2 command to LP mailbox");
      this->protocol_log_append_command_("queue", button, false, "mailbox_write_failed");
      return false;
    }

    portENTER_CRITICAL(&this->state_mux_);
    this->last_commanded_button_ = button;
    this->last_commanded_ms_ = millis();
    this->command_sequence_++;
    this->command_callback_pending_ = true;
    portEXIT_CRITICAL(&this->state_mux_);
    this->protocol_log_append_command_("queue", button, true, "lp_mailbox");
    return true;
  }

  if (this->command_queue_ == nullptr || this->bus_task_handle_ == nullptr) {
    ESP_LOGW(TAG, "Cannot queue command before HP fallback task is running");
    this->protocol_log_append_command_("queue", button, false, "hp_fallback_not_ready");
    return false;
  }

  CommandEvent event{button};
  if (xQueueSend(this->command_queue_, &event, 0) != pdTRUE) {
    ESP_LOGW(TAG, "HCP2 command queue is full");
    this->protocol_log_append_command_("queue", button, false, "queue_full");
    return false;
  }
  xTaskNotifyGive(this->bus_task_handle_);

  portENTER_CRITICAL(&this->state_mux_);
  this->last_commanded_button_ = button;
  this->last_commanded_ms_ = millis();
  this->command_sequence_++;
  this->command_callback_pending_ = true;
  portEXIT_CRITICAL(&this->state_mux_);
  this->protocol_log_append_command_("queue", button, true, "hp_fallback");
  return true;
#else
  return false;
#endif
}

#ifdef USE_ESP32
void HCP2Bridge::setup_protocol_log_() {
  this->protocol_log_ready_ = true;
  this->protocol_log_clear_();
}

void HCP2Bridge::protocol_log_clear_() {
  portENTER_CRITICAL(&this->protocol_log_mux_);
  this->protocol_log_start_ = 0;
  this->protocol_log_used_ = 0;
  this->protocol_log_next_seq_ = 1;
  this->protocol_log_dropped_records_ = 0;
  this->protocol_log_dropped_bytes_ = 0;
  std::memset(this->protocol_log_buffer_, 0, sizeof(this->protocol_log_buffer_));
  portEXIT_CRITICAL(&this->protocol_log_mux_);
}

void HCP2Bridge::protocol_log_discard_oldest_line_locked_() {
  if (this->protocol_log_used_ == 0u) {
    return;
  }
  size_t discard = this->protocol_log_used_;
  for (size_t i = 0; i < this->protocol_log_used_; i++) {
    const size_t index = (this->protocol_log_start_ + i) % this->PROTOCOL_LOG_CAPACITY;
    if (this->protocol_log_buffer_[index] == '\n') {
      discard = i + 1u;
      break;
    }
  }
  this->protocol_log_start_ = (this->protocol_log_start_ + discard) % this->PROTOCOL_LOG_CAPACITY;
  this->protocol_log_used_ -= discard;
  this->protocol_log_dropped_records_++;
  this->protocol_log_dropped_bytes_ += discard;
}

void HCP2Bridge::protocol_log_append_line_(const std::string &line, bool force) {
  if (!this->protocol_log_ready_) {
    return;
  }
  if (!force && !this->protocol_log_enabled_) {
    return;
  }
  const size_t needed = line.size() + 1u;
  portENTER_CRITICAL(&this->protocol_log_mux_);
  if (needed > this->PROTOCOL_LOG_CAPACITY) {
    this->protocol_log_dropped_records_++;
    this->protocol_log_dropped_bytes_ += needed;
    portEXIT_CRITICAL(&this->protocol_log_mux_);
    return;
  }
  while (needed > this->PROTOCOL_LOG_CAPACITY - this->protocol_log_used_) {
    this->protocol_log_discard_oldest_line_locked_();
  }
  size_t write = (this->protocol_log_start_ + this->protocol_log_used_) % this->PROTOCOL_LOG_CAPACITY;
  for (char c : line) {
    this->protocol_log_buffer_[write] = (uint8_t) c;
    write = (write + 1u) % this->PROTOCOL_LOG_CAPACITY;
  }
  this->protocol_log_buffer_[write] = '\n';
  this->protocol_log_used_ += needed;
  portEXIT_CRITICAL(&this->protocol_log_mux_);
}

void HCP2Bridge::protocol_log_append_control_(const char *action) {
  uint32_t seq;
  portENTER_CRITICAL(&this->protocol_log_mux_);
  seq = this->protocol_log_next_seq_++;
  portEXIT_CRITICAL(&this->protocol_log_mux_);

  std::string line = "{\"seq\":";
  line += std::to_string(seq);
  line += ",\"ms\":";
  line += std::to_string(millis());
  line += ",\"type\":\"control\",\"action\":\"";
  line += action == nullptr ? "unknown" : action;
  line += "\",\"enabled\":";
  line += this->protocol_log_enabled_ ? "true" : "false";
  line += "}";
  this->protocol_log_append_line_(line, true);
}

void HCP2Bridge::protocol_log_append_command_(const char *phase, hcp2_button_t button, bool ok,
                                              const char *reason) {
  uint32_t seq;
  portENTER_CRITICAL(&this->protocol_log_mux_);
  seq = this->protocol_log_next_seq_++;
  portEXIT_CRITICAL(&this->protocol_log_mux_);

  std::string line = "{\"seq\":";
  line += std::to_string(seq);
  line += ",\"ms\":";
  line += std::to_string(millis());
  line += ",\"type\":\"command\",\"phase\":\"";
  line += phase == nullptr ? "unknown" : phase;
  line += "\",\"button\":\"";
  line += button_name_(button);
  line += "\",\"ok\":";
  line += ok ? "true" : "false";
  line += ",\"reason\":\"";
  line += reason == nullptr ? "" : reason;
  line += "\"}";
  this->protocol_log_append_line_(line, false);
}

void HCP2Bridge::protocol_log_append_state_(const hcp2_drive_status_t &status, bool bus_online, bool obstruction,
                                            const char *source) {
  uint32_t seq;
  portENTER_CRITICAL(&this->protocol_log_mux_);
  seq = this->protocol_log_next_seq_++;
  portEXIT_CRITICAL(&this->protocol_log_mux_);

  std::string line = "{\"seq\":";
  line += std::to_string(seq);
  line += ",\"ms\":";
  line += std::to_string(millis());
  line += ",\"type\":\"state\",\"source\":\"";
  line += source == nullptr ? "unknown" : source;
  line += "\",\"target_position\":";
  line += std::to_string(status.target_position);
  line += ",\"current_position\":";
  line += std::to_string(status.current_position);
  line += ",\"state\":\"";
  line += hcp2_state_name(static_cast<hcp2_drive_state_code_t>(status.state));
  line += "\",\"state_raw\":";
  line += std::to_string(status.state);
  line += ",\"light\":";
  line += status.light_on ? "true" : "false";
  line += ",\"bus_online\":";
  line += bus_online ? "true" : "false";
  line += ",\"obstruction\":";
  line += obstruction ? "true" : "false";
  line += "}";
  this->protocol_log_append_line_(line, false);
}

void HCP2Bridge::protocol_log_append_protocol_event_(uint32_t event_us, uint8_t event_type, uint8_t frame_type,
                                                     const uint8_t *data, uint8_t len, const char *source) {
  uint32_t seq;
  if (len > HCP2_MAX_FRAME_LEN) {
    len = HCP2_MAX_FRAME_LEN;
  }
  portENTER_CRITICAL(&this->protocol_log_mux_);
  seq = this->protocol_log_next_seq_++;
  portEXIT_CRITICAL(&this->protocol_log_mux_);

  std::string line = "{\"seq\":";
  line += std::to_string(seq);
  line += ",\"ms\":";
  line += std::to_string(millis());
  line += ",\"type\":\"protocol\",\"source\":\"";
  line += source == nullptr ? "unknown" : source;
  line += "\",\"event_us\":";
  line += std::to_string(event_us);
  line += ",\"event\":\"";
  line += protocol_event_name_(event_type);
  line += "\",\"frame\":\"";
  line += frame_type_name_(frame_type);
  line += "\",\"len\":";
  line += std::to_string(len);
  line += ",\"hex\":\"";
  line += this->hex_encode_(data, len);
  line += "\"}";
  this->protocol_log_append_line_(line, false);
}

void HCP2Bridge::protocol_log_append_lp_trace_(const hcp2_lp_trace_entry_t &entry) {
  uint32_t seq;
  portENTER_CRITICAL(&this->protocol_log_mux_);
  seq = this->protocol_log_next_seq_++;
  portEXIT_CRITICAL(&this->protocol_log_mux_);

  std::string line = "{\"seq\":";
  line += std::to_string(seq);
  line += ",\"ms\":";
  line += std::to_string(millis());
  line += ",\"type\":\"lp_trace\",\"event_us\":";
  line += std::to_string(entry.at_us);
  line += ",\"event\":\"";
  line += lp_trace_event_name_(entry.event);
  line += "\",\"event_id\":";
  line += std::to_string(entry.event);
  line += ",\"value\":";
  line += std::to_string(entry.value);
  line += "}";
  this->protocol_log_append_line_(line, false);
}

void HCP2Bridge::protocol_log_append_lp_trace_overflow_(uint32_t dropped) {
  uint32_t seq;
  portENTER_CRITICAL(&this->protocol_log_mux_);
  seq = this->protocol_log_next_seq_++;
  portEXIT_CRITICAL(&this->protocol_log_mux_);

  std::string line = "{\"seq\":";
  line += std::to_string(seq);
  line += ",\"ms\":";
  line += std::to_string(millis());
  line += ",\"type\":\"lp_trace_overflow\",\"dropped\":";
  line += std::to_string(dropped);
  line += "}";
  this->protocol_log_append_line_(line, false);
}

void HCP2Bridge::drain_lp_protocol_event_() {
  hcp2_lp_protocol_event_t event{};
  if (this->lp_supervisor_.mailbox == nullptr) {
    return;
  }
  if (hcp2_lp_mailbox_read_protocol_event(this->lp_supervisor_.mailbox, &this->last_lp_protocol_sequence_,
                                          &event) == 0u) {
    return;
  }
  this->protocol_log_append_protocol_event_(event.at_us, event.event_type, event.frame_type, event.data, event.len,
                                            "lp");
}

void HCP2Bridge::drain_lp_trace_() {
  volatile hcp2_lp_mailbox_t *mailbox = this->lp_supervisor_.mailbox;
  if (mailbox == nullptr) {
    return;
  }

  const uint32_t head = mailbox->trace_head;
  const uint32_t tail = mailbox->trace_tail;
  uint32_t cursor = this->last_lp_trace_head_;

  if (cursor > head) {
    cursor = tail;
  }
  if (cursor < tail) {
    this->protocol_log_append_lp_trace_overflow_(tail - cursor);
    cursor = tail;
  }
  if ((head - cursor) > HCP2_LP_TRACE_CAPACITY) {
    const uint32_t next_cursor = head - HCP2_LP_TRACE_CAPACITY;
    this->protocol_log_append_lp_trace_overflow_(next_cursor - cursor);
    cursor = next_cursor;
  }

  while (cursor < head) {
    const uint32_t index = cursor % HCP2_LP_TRACE_CAPACITY;
    hcp2_lp_trace_entry_t entry{};
    entry.at_us = mailbox->trace[index].at_us;
    entry.event = mailbox->trace[index].event;
    entry.value = mailbox->trace[index].value;
    this->protocol_log_append_lp_trace_(entry);
    cursor++;
  }
  this->last_lp_trace_head_ = cursor;
}

void HCP2Bridge::drain_hp_protocol_event_() {
  hcp2_protocol_event_t event{};
  if (hcp2_engine_read_protocol_event(&this->engine_, &this->last_hp_protocol_sequence_, &event) == 0u) {
    return;
  }
  this->protocol_log_append_protocol_event_(event.at_us, event.event_type, event.frame_type, event.data, event.len,
                                            "hp");
}

std::string HCP2Bridge::hex_encode_(const uint8_t *data, size_t len) const {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(len * 2u);
  for (size_t i = 0; i < len; i++) {
    out.push_back(hex[(data[i] >> 4u) & 0x0Fu]);
    out.push_back(hex[data[i] & 0x0Fu]);
  }
  return out;
}

std::string HCP2Bridge::protocol_log_summary_json_() const {
  size_t used;
  uint32_t dropped_records;
  uint32_t dropped_bytes;
  uint32_t next_seq;

  portENTER_CRITICAL(&this->protocol_log_mux_);
  used = this->protocol_log_used_;
  dropped_records = this->protocol_log_dropped_records_;
  dropped_bytes = this->protocol_log_dropped_bytes_;
  next_seq = this->protocol_log_next_seq_;
  portEXIT_CRITICAL(&this->protocol_log_mux_);

  std::string json = "{\"enabled\":";
  json += this->protocol_log_enabled_ ? "true" : "false";
  json += ",\"ready\":";
  json += this->protocol_log_ready_ ? "true" : "false";
  json += ",\"storage\":\"internal_ram\",\"mode\":\"ring\",\"flash_writes\":false,\"used\":";
  json += std::to_string(used);
  json += ",\"capacity\":";
  json += std::to_string(this->PROTOCOL_LOG_CAPACITY);
  json += ",\"dropped_records\":";
  json += std::to_string(dropped_records);
  json += ",\"dropped_bytes\":";
  json += std::to_string(dropped_bytes);
  json += ",\"overwritten_records\":";
  json += std::to_string(dropped_records);
  json += ",\"overwritten_bytes\":";
  json += std::to_string(dropped_bytes);
  json += ",\"next_seq\":";
  json += std::to_string(next_seq);
  json += "}";
  return json;
}

std::string HCP2Bridge::protocol_log_body_() {
  std::string body;
  size_t snapshot_len = 0;

  portENTER_CRITICAL(&this->protocol_log_mux_);
  snapshot_len = this->protocol_log_used_;
  portEXIT_CRITICAL(&this->protocol_log_mux_);

  body.resize(snapshot_len);

  portENTER_CRITICAL(&this->protocol_log_mux_);
  const size_t copy_len = std::min(snapshot_len, this->protocol_log_used_);
  for (size_t i = 0; i < copy_len; i++) {
    const size_t index = (this->protocol_log_start_ + i) % this->PROTOCOL_LOG_CAPACITY;
    body[i] = (char) this->protocol_log_buffer_[index];
  }
  portEXIT_CRITICAL(&this->protocol_log_mux_);

  body.resize(copy_len);
  return body;
}

uint32_t HCP2Bridge::protocol_log_next_seq_snapshot_() const {
  uint32_t next_seq;
  portENTER_CRITICAL(&this->protocol_log_mux_);
  next_seq = this->protocol_log_next_seq_;
  portEXIT_CRITICAL(&this->protocol_log_mux_);
  return next_seq;
}

uint32_t HCP2Bridge::protocol_log_line_seq_(const std::string &line) {
  static constexpr const char PREFIX[] = "{\"seq\":";
  static constexpr size_t PREFIX_LEN = sizeof(PREFIX) - 1u;
  if (line.size() <= PREFIX_LEN || line.compare(0, PREFIX_LEN, PREFIX) != 0) {
    return 0;
  }
  uint32_t value = 0;
  for (size_t i = PREFIX_LEN; i < line.size(); i++) {
    const char c = line[i];
    if (c < '0' || c > '9') {
      break;
    }
    value = value * 10u + (uint32_t) (c - '0');
  }
  return value;
}

std::string HCP2Bridge::protocol_log_body_since_(uint32_t cursor_seq, uint32_t *next_cursor_seq, size_t max_bytes) {
  std::string body;
  std::string snapshot;
  std::string line;
  uint32_t next_seq = 1;
  uint32_t last_sent_seq = 0;
  size_t snapshot_len = 0;

  portENTER_CRITICAL(&this->protocol_log_mux_);
  snapshot_len = this->protocol_log_used_;
  portEXIT_CRITICAL(&this->protocol_log_mux_);

  snapshot.resize(snapshot_len);
  line.reserve(256);
  body.reserve(std::min(max_bytes, snapshot_len));

  portENTER_CRITICAL(&this->protocol_log_mux_);
  const size_t copy_len = std::min(snapshot_len, this->protocol_log_used_);
  next_seq = this->protocol_log_next_seq_;
  for (size_t i = 0; i < copy_len; i++) {
    const size_t index = (this->protocol_log_start_ + i) % this->PROTOCOL_LOG_CAPACITY;
    snapshot[i] = (char) this->protocol_log_buffer_[index];
  }
  portEXIT_CRITICAL(&this->protocol_log_mux_);

  snapshot.resize(copy_len);

  if (next_seq < cursor_seq) {
    cursor_seq = 1;
  }
  for (char c : snapshot) {
    if (c != '\n') {
      line.push_back(c);
      continue;
    }
    const uint32_t seq = protocol_log_line_seq_(line);
    if (seq >= cursor_seq) {
      const size_t needed = line.size() + 1u;
      if (!body.empty() && body.size() + needed > max_bytes) {
        break;
      }
      body += line;
      body.push_back('\n');
      last_sent_seq = seq;
    }
    line.clear();
  }

  if (next_cursor_seq != nullptr) {
    *next_cursor_seq = last_sent_seq != 0u ? last_sent_seq + 1u : next_seq;
  }
  return body;
}

std::string HCP2Bridge::http_debug_health_json_() {
  const bool lp_mode = !this->hp_fallback_;
  const uint32_t polls_seen = this->get_lp_poll_count();
  const uint32_t polls_answered = this->get_lp_response_count();
  const uint32_t missed_polls = this->get_lp_missed_poll_count();
  const uint32_t raw_missed_polls = this->get_lp_raw_missed_poll_count();
  const bool pending_response = this->has_lp_pending_response();
  const uint32_t health_flags = this->get_lp_health_flags();
  const uint32_t tx_aborts = this->get_lp_tx_abort_count();
  const uint32_t collisions = this->get_lp_collision_count();
  const uint32_t loop_overruns = this->get_lp_loop_overrun_count();
  const uint32_t rx_starvations = this->get_lp_rx_starvation_count();
  const uint32_t stuck_de = this->get_lp_stuck_de_count();
  const uint32_t last_poll_age_ms = this->get_lp_last_poll_age_ms();
  const uint32_t max_de_hold_us = this->get_lp_max_de_hold_us();
  const bool bus_online = this->is_bus_online();
  const bool valid_broadcast = this->has_valid_broadcast();
  const bool lp_seen = polls_seen > 0u && this->get_lp_heartbeat() > 0u;

  std::string reasons = "[";
  bool first_reason = true;
  const auto add_reason = [&](const char *reason) {
    if (!first_reason) {
      reasons += ",";
    }
    first_reason = false;
    reasons += "\"";
    reasons += reason;
    reasons += "\"";
  };

  if (!lp_mode) {
    add_reason("hp_fallback_enabled");
  }
  if (!lp_seen) {
    add_reason("lp_not_seen");
  }
  if (!bus_online) {
    add_reason("bus_offline");
  }
  if (!valid_broadcast) {
    add_reason("no_broadcast_state");
  }
  if (last_poll_age_ms > (HCP2BRIDGE_BUS_ONLINE_TIMEOUT_US / 1000u)) {
    add_reason("last_poll_stale");
  }
  if (missed_polls != 0u) {
    add_reason("missed_polls");
  }
  if (health_flags != 0u) {
    add_reason("lp_health_flags");
  }
  if (tx_aborts != 0u) {
    add_reason("tx_aborts");
  }
  if (collisions != 0u) {
    add_reason("collisions");
  }
  if (loop_overruns != 0u) {
    add_reason("loop_overruns");
  }
  if (rx_starvations != 0u) {
    add_reason("rx_starvations");
  }
  if (stuck_de != 0u) {
    add_reason("stuck_de_recoveries");
  }
  if (max_de_hold_us > 0u && max_de_hold_us > HCP2BRIDGE_MAX_DE_HIGH_US) {
    add_reason("de_hold_too_long");
  }
  reasons += "]";

  const bool safe_for_ota_restart = first_reason;
  std::string json = "{\"verdict\":\"";
  json += safe_for_ota_restart ? "ok" : "fail";
  json += "\",\"safe_for_ota_restart\":";
  json += safe_for_ota_restart ? "true" : "false";
  json += ",\"reasons\":";
  json += reasons;
  json += ",\"checks\":{\"lp_mode\":";
  json += lp_mode ? "true" : "false";
  json += ",\"lp_seen\":";
  json += lp_seen ? "true" : "false";
  json += ",\"bus_online\":";
  json += bus_online ? "true" : "false";
  json += ",\"valid_broadcast\":";
  json += valid_broadcast ? "true" : "false";
  json += ",\"last_poll_age_ms\":";
  json += std::to_string(last_poll_age_ms);
  json += ",\"polls_seen\":";
  json += std::to_string(polls_seen);
  json += ",\"polls_answered\":";
  json += std::to_string(polls_answered);
  json += ",\"missed_polls\":";
  json += std::to_string(missed_polls);
  json += ",\"raw_missed_polls\":";
  json += std::to_string(raw_missed_polls);
  json += ",\"pending_response\":";
  json += pending_response ? "true" : "false";
  json += ",\"health_flags\":";
  json += std::to_string(health_flags);
  json += ",\"tx_aborts\":";
  json += std::to_string(tx_aborts);
  json += ",\"collisions\":";
  json += std::to_string(collisions);
  json += ",\"loop_overruns\":";
  json += std::to_string(loop_overruns);
  json += ",\"rx_starvations\":";
  json += std::to_string(rx_starvations);
  json += ",\"stuck_de_recoveries\":";
  json += std::to_string(stuck_de);
  json += ",\"max_de_hold_us\":";
  json += std::to_string(max_de_hold_us);
  json += "},\"stats\":";
  json += this->http_debug_stats_json_();
  json += "}";
  return json;
}

std::string HCP2Bridge::http_debug_stats_json_() {
  std::string json = "{\"protocol\":\"hcp2\",\"mode\":\"";
  json += this->hp_fallback_ ? "hp_fallback" : "lp";
  json += "\",\"uptime_ms\":";
  json += std::to_string(millis());
  json += ",\"bus_online\":";
  json += this->is_bus_online() ? "true" : "false";
  json += ",\"valid_broadcast\":";
  json += this->has_valid_broadcast() ? "true" : "false";
  json += ",\"state\":\"";
  json += this->get_state_string();
  json += "\",\"position\":";
  json += std::to_string(this->get_position());
  json += ",\"polls_seen\":";
  json += std::to_string(this->get_lp_poll_count());
  json += ",\"polls_answered\":";
  json += std::to_string(this->get_lp_response_count());
  json += ",\"missed_polls\":";
  json += std::to_string(this->get_lp_missed_poll_count());
  json += ",\"raw_missed_polls\":";
  json += std::to_string(this->get_lp_raw_missed_poll_count());
  json += ",\"pending_response\":";
  json += this->has_lp_pending_response() ? "true" : "false";
  json += ",\"crc_errors\":";
  json += std::to_string(this->get_lp_crc_error_count());
  json += ",\"rx_errors\":";
  json += std::to_string(this->get_lp_rx_error_count());
  json += ",\"tx_aborts\":";
  json += std::to_string(this->get_lp_tx_abort_count());
  json += ",\"collisions\":";
  json += std::to_string(this->get_lp_collision_count());
  json += ",\"lp_heartbeat\":";
  json += std::to_string(this->get_lp_heartbeat());
  json += ",\"lp_resets\":";
  json += std::to_string(this->get_lp_reset_count());
  json += ",\"hp_resets\":";
  json += std::to_string(this->get_hp_reset_count());
  json += ",\"protocol_log\":";
  json += this->protocol_log_summary_json_();
  json += "}";
  return json;
}

std::string HCP2Bridge::http_debug_support_json_() {
  hcp2_drive_status_t status = this->drive_status_snapshot_();
  std::string json = "{\"device\":\"hcp2bridge\",\"target\":\"supramatic_series_4\",\"stats\":";
  json += this->http_debug_stats_json_();
  json += ",\"health\":";
  json += this->http_debug_health_json_();
  json += ",\"door\":{\"target_position_raw\":";
  json += std::to_string(status.target_position);
  json += ",\"current_position_raw\":";
  json += std::to_string(status.current_position);
  json += ",\"state_raw\":";
  json += std::to_string(status.state);
  json += ",\"state\":\"";
  json += hcp2_state_name(static_cast<hcp2_drive_state_code_t>(status.state));
  json += "\",\"light\":";
  json += status.light_on ? "true" : "false";
  json += ",\"obstruction\":";
  json += this->is_obstructed() ? "true" : "false";
  json += "},\"lp\":{\"health_flags\":";
  json += std::to_string(this->get_lp_health_flags());
  json += ",\"max_rx_fifo\":";
  json += std::to_string(this->get_lp_max_rx_fifo_count());
  json += ",\"max_loop_us\":";
  json += std::to_string(this->get_lp_max_loop_us());
  json += ",\"max_poll_rx_to_schedule_us\":";
  json += std::to_string(this->get_lp_max_poll_rx_to_schedule_us());
  json += ",\"max_response_schedule_to_tx_start_us\":";
  json += std::to_string(this->get_lp_max_response_schedule_to_tx_start_us());
  json += ",\"max_response_tx_us\":";
  json += std::to_string(this->get_lp_max_response_tx_us());
  json += ",\"max_de_hold_us\":";
  json += std::to_string(this->get_lp_max_de_hold_us());
  json += ",\"last_poll_age_ms\":";
  json += std::to_string(this->get_lp_last_poll_age_ms());
  json += ",\"loop_overruns\":";
  json += std::to_string(this->get_lp_loop_overrun_count());
  json += ",\"rx_starvations\":";
  json += std::to_string(this->get_lp_rx_starvation_count());
  json += ",\"stuck_de_recoveries\":";
  json += std::to_string(this->get_lp_stuck_de_count());
  json += ",\"mailbox_repairs\":";
  json += std::to_string(this->get_lp_mailbox_repair_count());
  json += "},\"hp\":{\"panic_resets\":";
  json += std::to_string(this->get_hp_panic_reset_count());
  json += ",\"wdt_resets\":";
  json += std::to_string(this->get_hp_wdt_reset_count());
  json += ",\"brownout_resets\":";
  json += std::to_string(this->get_hp_brownout_reset_count());
  json += "}}";
  return json;
}

std::string HCP2Bridge::http_debug_index_html_() {
  return R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HCP2 Bridge Debug</title>
<style>
:root{color-scheme:light dark;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:#111827;color:#e5e7eb}
body{margin:0;padding:18px;line-height:1.35}
h1{font-size:22px;margin:0 0 12px}
h2{font-size:15px;margin:0 0 10px;color:#cbd5e1}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px}
.panel{border:1px solid #334155;border-radius:8px;background:#0f172a;padding:12px}
.status{display:inline-flex;align-items:center;gap:8px;border-radius:999px;padding:7px 10px;font-weight:700}
.ok{background:#064e3b;color:#d1fae5}.fail{background:#7f1d1d;color:#fee2e2}.unknown{background:#374151;color:#f3f4f6}
.row{display:flex;justify-content:space-between;gap:12px;border-bottom:1px solid #1f2937;padding:5px 0;font-size:13px}
.row:last-child{border-bottom:0}
.key{color:#94a3b8}.value{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;text-align:right}
button,a.button{display:inline-block;margin:0 6px 8px 0;border:1px solid #475569;border-radius:6px;background:#1e293b;color:#e5e7eb;padding:7px 10px;text-decoration:none;cursor:pointer}
button:hover,a.button:hover{background:#334155}
label{font-size:13px;color:#cbd5e1}
pre{white-space:pre-wrap;overflow:auto;max-height:48vh;background:#020617;border:1px solid #1f2937;border-radius:6px;padding:10px;font-size:12px}
.muted{color:#94a3b8;font-size:12px}
.reasons{margin-top:8px;color:#fecaca;font-size:13px}
</style>
</head>
<body>
<h1>HCP2 Bridge Debug</h1>
<div class="panel">
  <span id="verdict" class="status unknown">loading</span>
  <span id="updated" class="muted"></span>
  <div id="reasons" class="reasons"></div>
</div>
<div class="grid" style="margin-top:12px">
  <section class="panel"><h2>Continuity</h2><div id="continuity"></div></section>
  <section class="panel"><h2>Door</h2><div id="door"></div></section>
  <section class="panel"><h2>Counters</h2><div id="counters"></div></section>
  <section class="panel"><h2>Timing</h2><div id="timing"></div></section>
</div>
<section class="panel" style="margin-top:12px">
  <h2>RAM Protocol Log</h2>
  <button onclick="controlLog('start')">Start</button>
  <button onclick="controlLog('stop')">Stop</button>
  <button onclick="controlLog('clear')">Clear</button>
  <button onclick="refreshLog()">Refresh Log</button>
  <button onclick="connectLogStream()">Reconnect Stream</button>
  <button onclick="downloadCachedLog()">Download JSON</button>
  <a class="button" href="/hcp2_log" download="hcp2-log.ndjson">Device NDJSON</a>
  <a class="button" href="/hcp2_log.bin" download="hcp2-log.bin">Device Raw</a>
  <div id="logSummary" class="muted"></div>
  <div id="logStream" class="muted">stream disconnected</div>
  <div id="logCache" class="muted">cache empty</div>
  <pre id="log"></pre>
</section>
<section class="panel" style="margin-top:12px">
  <h2>Raw JSON</h2>
  <button onclick="loadRaw('/health')">Health</button>
  <button onclick="loadRaw('/stats')">Stats</button>
  <button onclick="loadRaw('/support')">Support</button>
  <pre id="raw"></pre>
</section>
<script>
const $=id=>document.getElementById(id);
let logSocket=null;
let logReconnectTimer=null;
let logLines=[];
let logCache=[];
let logCacheBytes=0;
let refreshBusy=false;
let lastHealthStreamMs=0;
let logRenderQueued=false;
const maxVisibleLogLines=300;
const maxCacheAgeMs=10*60*1000;
const maxCacheBytes=1024*1024;
function row(k,v){return `<div class="row"><span class="key">${k}</span><span class="value">${v??''}</span></div>`}
function setRows(id,items){$(id).innerHTML=items.map(([k,v])=>row(k,v)).join('')}
async function getJson(path){const r=await fetch(path,{cache:'no-store'});const t=await r.text();try{return JSON.parse(t)}catch(e){throw new Error(path+' returned non-JSON')}}
function applyHealth(health,source='http'){
  const stats=health.stats||{};
  const ok=health.verdict==='ok';
  if(source==='stream')lastHealthStreamMs=Date.now();
  $('verdict').className='status '+(ok?'ok':'fail');
  $('verdict').textContent=ok?'continuity ok':'continuity problem';
  $('updated').textContent=' updated '+new Date().toLocaleTimeString()+` via ${source}`;
  $('reasons').textContent=(health.reasons&&health.reasons.length)?'Reasons: '+health.reasons.join(', '):'';
  const c=health.checks||{};
  setRows('continuity',[
    ['safe for OTA/restart',health.safe_for_ota_restart],
    ['bus online',c.bus_online],
    ['LP seen',c.lp_seen],
    ['valid broadcast',c.valid_broadcast],
    ['last poll age ms',c.last_poll_age_ms],
    ['pending response',c.pending_response],
    ['raw poll delta',c.raw_missed_polls],
    ['missed polls',c.missed_polls],
    ['health flags',c.health_flags]
  ]);
  setRows('door',[
    ['state',stats.state],
    ['position',Number(stats.position).toFixed(3)],
    ['mode',stats.mode],
    ['uptime ms',stats.uptime_ms]
  ]);
  setRows('counters',[
    ['polls seen',stats.polls_seen],
    ['polls answered',stats.polls_answered],
    ['pending response',stats.pending_response],
    ['raw poll delta',stats.raw_missed_polls],
    ['crc errors',stats.crc_errors],
    ['rx errors',stats.rx_errors],
    ['tx aborts',stats.tx_aborts],
    ['collisions',stats.collisions],
    ['LP resets',stats.lp_resets],
    ['HP resets',stats.hp_resets]
  ]);
  const p=stats.protocol_log||{};
  setRows('timing',[
    ['max DE hold us',c.max_de_hold_us],
    ['log mode',p.mode],
    ['log used bytes',p.used],
    ['log capacity',p.capacity],
    ['overwritten records',p.overwritten_records]
  ]);
  $('logSummary').textContent=`log ${p.enabled?'enabled':'disabled'}, ${p.used||0}/${p.capacity||0} bytes, overwritten ${p.overwritten_records||0} records`;
  updateLogCacheSummary();
}
async function refresh(){
  if(refreshBusy)return;
  refreshBusy=true;
  try{
    const health=await getJson('/health');
    applyHealth(health,'http');
  }catch(e){
    $('verdict').className='status unknown';
    $('verdict').textContent='debug fetch failed';
    $('reasons').textContent=e.message;
  }finally{
    refreshBusy=false;
  }
}
function resetLogCache(){logCache=[];logCacheBytes=0;updateLogCacheSummary()}
function pruneLogCache(now=Date.now()){
  const minMs=now-maxCacheAgeMs;
  while(logCache.length&&(logCache[0].received_ms<minMs||logCacheBytes>maxCacheBytes)){
    const removed=logCache.shift();
    logCacheBytes-=removed.bytes||0;
  }
  if(logCacheBytes<0)logCacheBytes=0;
}
function updateLogCacheSummary(){
  pruneLogCache();
  const kb=(logCacheBytes/1024).toFixed(1);
  const oldest=logCache[0]?.received_at||'none';
  const newest=logCache[logCache.length-1]?.received_at||'none';
  $('logCache').textContent=`browser cache ${logCache.length} records, ${kb} KiB, newest 10 min / 1 MiB, ${oldest} to ${newest}`;
}
function cacheLogLine(line,receivedMs=Date.now()){
  if(!line)return;
  let entry=null;
  try{entry=JSON.parse(line)}catch(e){}
  const record={received_at:new Date(receivedMs).toISOString(),received_ms:receivedMs,bytes:line.length,raw:line};
  if(entry&&typeof entry==='object')record.entry=entry;else record.parse_error=true;
  logCache.push(record);
  logCacheBytes+=record.bytes;
  pruneLogCache(receivedMs);
}
function renderLogNow(){const el=$('log');el.textContent=logLines.join('\n');el.scrollTop=el.scrollHeight}
function renderLog(){
  if(logRenderQueued)return;
  logRenderQueued=true;
  requestAnimationFrame(()=>{
    logRenderQueued=false;
    renderLogNow();
  });
}
function appendLogText(text,replace=false){
  const now=Date.now();
  if(replace){logLines=[];resetLogCache()}
  for(const line of text.split('\n')){
    if(!line)continue;
    logLines.push(line);
    cacheLogLine(line,now);
  }
  if(logLines.length>maxVisibleLogLines)logLines=logLines.slice(logLines.length-maxVisibleLogLines);
  renderLog();
  updateLogCacheSummary();
}
async function controlLog(action){
  await fetch('/hcp2_log/'+action,{cache:'no-store'});
  await refresh();
  if(action==='clear'){logLines=[];resetLogCache();renderLog()}else await refreshLog();
}
async function refreshLog(){
  const r=await fetch('/hcp2_log',{cache:'no-store'});
  const text=await r.text();
  appendLogText(text,true);
}
function connectLogStream(){
  if(logReconnectTimer){clearTimeout(logReconnectTimer);logReconnectTimer=null}
  if(logSocket){logSocket.onclose=null;logSocket.close();logSocket=null}
  const scheme=location.protocol==='https:'?'wss':'ws';
  const ws=new WebSocket(`${scheme}://${location.host}/hcp2_log/ws`);
  logSocket=ws;
  $('logStream').textContent='stream connecting';
  ws.onopen=()=>{$('logStream').textContent='stream connected'};
  ws.onmessage=event=>{
    let message=null;
    try{message=JSON.parse(event.data)}catch(e){}
    if(message&&message.type==='health'&&message.health){
      applyHealth(message.health,'stream');
      return;
    }
    if(message&&message.type==='log'&&typeof message.text==='string'){
      appendLogText(message.text);
      return;
    }
    appendLogText(event.data);
  };
  ws.onerror=()=>{try{ws.close()}catch(e){}};
  ws.onclose=()=>{
    if(logSocket===ws)logSocket=null;
    $('logStream').textContent='stream disconnected; reconnecting';
    logReconnectTimer=setTimeout(connectLogStream,2000);
  };
}
function cachedLogExport(){
  pruneLogCache();
  return {
    format:'hcp2-debug-log-cache-v1',
    exported_at:new Date().toISOString(),
    source:location.host,
    cache:{record_count:logCache.length,bytes:logCacheBytes,max_age_ms:maxCacheAgeMs,max_bytes:maxCacheBytes},
    records:logCache.map(({received_ms,bytes,...record})=>record)
  };
}
function downloadCachedLog(){
  const payload=JSON.stringify(cachedLogExport(),null,2);
  const blob=new Blob([payload],{type:'application/json'});
  const url=URL.createObjectURL(blob);
  const a=document.createElement('a');
  const stamp=new Date().toISOString().replace(/[:.]/g,'-');
  a.href=url;
  a.download=`hcp2-debug-log-${stamp}.json`;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(()=>URL.revokeObjectURL(url),1000);
}
async function loadRaw(path){const data=await getJson(path);$('raw').textContent=JSON.stringify(data,null,2)}
setInterval(()=>{if(Date.now()-lastHealthStreamMs>2000)refresh()},1000);
async function init(){await refresh();await refreshLog();connectLogStream()}
init();
</script>
</body>
</html>)HTML";
}

void HCP2Bridge::setup_http_debug_server_() {
  this->http_debug_server_ = socket::socket_ip_loop_monitored(SOCK_STREAM, 0);
  if (this->http_debug_server_ == nullptr) {
    ESP_LOGW(TAG, "Failed to create HCP2 HTTP debug socket, retrying");
    this->http_debug_server_.reset();
    this->http_debug_next_setup_ms_ = millis() + HCP2BRIDGE_HTTP_SETUP_RETRY_MS;
    return;
  }

  int enable = 1;
  int err = this->http_debug_server_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  if (err != 0) {
    ESP_LOGW(TAG, "SO_REUSEADDR failed for HCP2 HTTP debug socket: errno=%d", errno);
  }
  err = this->http_debug_server_->setblocking(false);
  if (err != 0) {
    ESP_LOGW(TAG, "Failed to set HCP2 HTTP debug socket nonblocking: errno=%d, retrying", errno);
    this->http_debug_server_.reset();
    this->http_debug_next_setup_ms_ = millis() + HCP2BRIDGE_HTTP_SETUP_RETRY_MS;
    return;
  }

  struct sockaddr_storage server_addr;
  socklen_t server_addr_len =
      socket::set_sockaddr_any((struct sockaddr *) &server_addr, sizeof(server_addr), this->http_debug_port_);
  if (server_addr_len == 0) {
    ESP_LOGW(TAG, "Failed to build HCP2 HTTP debug bind address, retrying");
    this->http_debug_server_.reset();
    this->http_debug_next_setup_ms_ = millis() + HCP2BRIDGE_HTTP_SETUP_RETRY_MS;
    return;
  }
  err = this->http_debug_server_->bind((struct sockaddr *) &server_addr, server_addr_len);
  if (err != 0) {
    ESP_LOGW(TAG, "Failed to bind HCP2 HTTP debug port %u: errno=%d, retrying", (unsigned int) this->http_debug_port_,
             errno);
    this->http_debug_server_.reset();
    this->http_debug_next_setup_ms_ = millis() + HCP2BRIDGE_HTTP_SETUP_RETRY_MS;
    return;
  }
  err = this->http_debug_server_->listen(1);
  if (err != 0) {
    ESP_LOGW(TAG, "Failed to listen on HCP2 HTTP debug port %u: errno=%d, retrying", (unsigned int) this->http_debug_port_,
             errno);
    this->http_debug_server_.reset();
    this->http_debug_next_setup_ms_ = millis() + HCP2BRIDGE_HTTP_SETUP_RETRY_MS;
    return;
  }
  this->http_debug_next_setup_ms_ = 0;
  ESP_LOGI(TAG, "HCP2 HTTP debug listening on port %u", (unsigned int) this->http_debug_port_);
}

void HCP2Bridge::maybe_setup_http_debug_server_() {
  if (this->http_debug_server_ != nullptr) {
    return;
  }
  const uint32_t now_ms = millis();
  if (this->http_debug_next_setup_ms_ != 0 &&
      static_cast<int32_t>(now_ms - this->http_debug_next_setup_ms_) < 0) {
    return;
  }
  this->setup_http_debug_server_();
}

void HCP2Bridge::http_debug_accept_client_() {
  if (this->http_debug_server_ == nullptr || !this->http_debug_server_->ready()) {
    return;
  }
  while (true) {
    struct sockaddr_storage source_addr;
    socklen_t source_addr_len = sizeof(source_addr);
    auto client = this->http_debug_server_->accept_loop_monitored((struct sockaddr *) &source_addr, &source_addr_len);
    if (!client) {
      break;
    }
    int enable = 1;
    client->setsockopt(IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
    int err = client->setblocking(false);
    if (err != 0) {
      client.reset();
      continue;
    }
    if (this->http_debug_pending_client_ != nullptr) {
      if (millis() - this->http_debug_pending_client_started_ms_ > HCP2BRIDGE_HTTP_PENDING_TIMEOUT_MS) {
        this->http_debug_pending_client_.reset();
        this->http_debug_request_buffer_len_ = 0;
      } else {
        this->http_debug_send_response_(std::move(client), "503 Service Unavailable", "text/plain; charset=utf-8",
                                        "busy\n");
        continue;
      }
    }
    this->http_debug_pending_client_ = std::move(client);
    this->http_debug_request_buffer_len_ = 0;
    this->http_debug_pending_client_started_ms_ = millis();
    break;
  }
}

void HCP2Bridge::http_debug_service_pending_client_() {
  if (this->http_debug_pending_client_ == nullptr) {
    return;
  }
  if (millis() - this->http_debug_pending_client_started_ms_ > HCP2BRIDGE_HTTP_PENDING_TIMEOUT_MS) {
    this->http_debug_pending_client_.reset();
    this->http_debug_request_buffer_len_ = 0;
    return;
  }
  while (true) {
    char buffer[96];
    ssize_t read_len = this->http_debug_pending_client_->read(buffer, sizeof(buffer));
    if (read_len > 0) {
      for (ssize_t i = 0; i < read_len; i++) {
        const char c = buffer[i];
        if (this->http_debug_request_buffer_len_ < sizeof(this->http_debug_request_buffer_) - 1u) {
          this->http_debug_request_buffer_[this->http_debug_request_buffer_len_++] = c;
          if (this->http_debug_request_complete_()) {
            this->http_debug_request_buffer_[this->http_debug_request_buffer_len_] = '\0';
            std::string request(this->http_debug_request_buffer_, this->http_debug_request_buffer_len_);
            auto client = std::move(this->http_debug_pending_client_);
            this->http_debug_request_buffer_len_ = 0;
            this->http_debug_handle_request_(std::move(client), request);
            return;
          }
        } else {
          auto client = std::move(this->http_debug_pending_client_);
          this->http_debug_request_buffer_len_ = 0;
          this->http_debug_send_response_(std::move(client), "414 URI Too Long",
                                          "text/plain; charset=utf-8", "request headers too long\n");
          return;
        }
      }
      continue;
    }
    if (read_len == 0) {
      this->http_debug_pending_client_.reset();
      this->http_debug_request_buffer_len_ = 0;
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    this->http_debug_pending_client_.reset();
    this->http_debug_request_buffer_len_ = 0;
    return;
  }
}

bool HCP2Bridge::http_debug_request_complete_() const {
  if (this->http_debug_request_buffer_len_ >= 4u) {
    for (size_t i = 3; i < this->http_debug_request_buffer_len_; i++) {
      if (this->http_debug_request_buffer_[i - 3u] == '\r' && this->http_debug_request_buffer_[i - 2u] == '\n' &&
          this->http_debug_request_buffer_[i - 1u] == '\r' && this->http_debug_request_buffer_[i] == '\n') {
        return true;
      }
    }
  }
  if (this->http_debug_request_buffer_len_ >= 2u) {
    for (size_t i = 1; i < this->http_debug_request_buffer_len_; i++) {
      if (this->http_debug_request_buffer_[i - 1u] == '\n' && this->http_debug_request_buffer_[i] == '\n') {
        return true;
      }
    }
  }
  return false;
}

void HCP2Bridge::http_debug_service_log_ws_() {
  if (this->http_debug_log_ws_client_ == nullptr) {
    return;
  }

  if (this->http_debug_log_ws_client_->ready()) {
    while (true) {
      char buffer[96];
      ssize_t read_len = this->http_debug_log_ws_client_->read(buffer, sizeof(buffer));
      if (read_len > 0) {
        continue;
      }
      if (read_len == 0) {
        this->http_debug_log_ws_client_.reset();
        return;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      this->http_debug_log_ws_client_.reset();
      return;
    }
  }

  const uint32_t now_ms = millis();
  if (this->http_debug_log_ws_last_status_ms_ == 0 ||
      now_ms - this->http_debug_log_ws_last_status_ms_ >= HCP2BRIDGE_HTTP_WS_HEALTH_INTERVAL_MS) {
    this->http_debug_log_ws_last_status_ms_ = now_ms;
    if (!this->http_debug_send_ws_text_(this->http_debug_log_ws_client_.get(), this->http_debug_ws_health_json_(),
                                        25)) {
      this->http_debug_log_ws_client_.reset();
      return;
    }
  }

  if (this->http_debug_log_ws_last_send_ms_ != 0 &&
      now_ms - this->http_debug_log_ws_last_send_ms_ < HCP2BRIDGE_HTTP_WS_SEND_INTERVAL_MS) {
    return;
  }
  uint32_t next_cursor = this->http_debug_log_ws_next_seq_;
  const std::string body =
      this->protocol_log_body_since_(this->http_debug_log_ws_next_seq_, &next_cursor, HCP2BRIDGE_HTTP_WS_MAX_CHUNK_BYTES);
  this->http_debug_log_ws_next_seq_ = next_cursor;
  this->http_debug_log_ws_last_send_ms_ = now_ms;
  if (body.empty()) {
    return;
  }
  if (!this->http_debug_send_ws_text_(this->http_debug_log_ws_client_.get(), this->http_debug_ws_log_json_(body), 25)) {
    this->http_debug_log_ws_client_.reset();
  }
}

void HCP2Bridge::http_debug_handle_request_(std::unique_ptr<socket::Socket> client,
                                            const std::string &request) {
  const std::string path = this->http_debug_path_from_request_(request);
  if (path.empty()) {
    this->http_debug_send_response_(std::move(client), "400 Bad Request", "text/plain; charset=utf-8",
                                    "bad request\n");
    return;
  }
  if (path == "/hcp2_log/ws") {
    this->http_debug_upgrade_log_ws_(std::move(client), request);
    return;
  }
  if (path == "/" || path == "/index.html") {
    this->http_debug_send_response_(std::move(client), "200 OK", "text/html; charset=utf-8",
                                    this->http_debug_index_html_());
    return;
  }
  if (path == "/stats") {
    this->http_debug_send_response_(std::move(client), "200 OK", "application/json", this->http_debug_stats_json_());
    return;
  }
  if (path == "/health" || path == "/preflight") {
    const std::string body = this->http_debug_health_json_();
    const char *status = body.find("\"verdict\":\"ok\"") != std::string::npos ? "200 OK" : "503 Service Unavailable";
    this->http_debug_send_response_(std::move(client), status, "application/json", body);
    return;
  }
  if (path == "/support") {
    this->http_debug_send_response_(std::move(client), "200 OK", "application/json",
                                    this->http_debug_support_json_());
    return;
  }
  if (path == "/hcp2_log/start") {
    this->protocol_log_enabled_ = true;
    this->protocol_log_append_control_("start");
    this->http_debug_send_response_(std::move(client), "200 OK", "application/json",
                                    this->protocol_log_summary_json_());
    return;
  }
  if (path == "/hcp2_log/stop") {
    this->protocol_log_append_control_("stop");
    this->protocol_log_enabled_ = false;
    this->http_debug_send_response_(std::move(client), "200 OK", "application/json",
                                    this->protocol_log_summary_json_());
    return;
  }
  if (path == "/hcp2_log/clear") {
    this->protocol_log_clear_();
    this->protocol_log_append_control_("clear");
    if (this->http_debug_log_ws_client_ != nullptr) {
      this->http_debug_log_ws_next_seq_ = 1;
      this->http_debug_log_ws_last_send_ms_ = 0;
      this->http_debug_log_ws_last_status_ms_ = 0;
    }
    this->http_debug_send_response_(std::move(client), "200 OK", "application/json",
                                    this->protocol_log_summary_json_());
    return;
  }
  if (path == "/hcp2_log") {
    this->http_debug_send_response_(std::move(client), "200 OK", "application/x-ndjson; charset=utf-8",
                                    this->protocol_log_body_());
    return;
  }
  if (path == "/hcp2_log.bin") {
    this->http_debug_send_log_binary_response_(std::move(client));
    return;
  }
  this->http_debug_send_response_(std::move(client), "404 Not Found", "text/plain; charset=utf-8", "not found\n");
}

void HCP2Bridge::http_debug_upgrade_log_ws_(std::unique_ptr<socket::Socket> client, const std::string &request) {
  const std::string upgrade = this->http_debug_header_value_(request, "Upgrade");
  const std::string key = this->http_debug_header_value_(request, "Sec-WebSocket-Key");
  if (!hcp2_ascii_iequals(upgrade, "websocket") || key.empty()) {
    this->http_debug_send_response_(std::move(client), "400 Bad Request", "text/plain; charset=utf-8",
                                    "bad websocket upgrade\n");
    return;
  }

  std::string accept_source = key;
  accept_source += HCP2BRIDGE_WEBSOCKET_GUID;
  unsigned char digest[20];
  unsigned char encoded[32];
  size_t encoded_len = 0;
  if (mbedtls_sha1((const unsigned char *) accept_source.data(), accept_source.size(), digest) != 0 ||
      mbedtls_base64_encode(encoded, sizeof(encoded), &encoded_len, digest, sizeof(digest)) != 0) {
    this->http_debug_send_response_(std::move(client), "500 Internal Server Error", "text/plain; charset=utf-8",
                                    "websocket handshake failed\n");
    return;
  }

  std::string response = "HTTP/1.1 101 Switching Protocols\r\n";
  response += "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
  response.append((const char *) encoded, encoded_len);
  response += "\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
  if (!this->http_debug_write_all_(client.get(), response, 1000)) {
    return;
  }

  this->http_debug_log_ws_client_.reset();
  this->http_debug_log_ws_client_ = std::move(client);
  this->http_debug_log_ws_next_seq_ = this->protocol_log_next_seq_snapshot_();
  this->http_debug_log_ws_last_send_ms_ = 0;
  this->http_debug_log_ws_last_status_ms_ = 0;
  ESP_LOGI(TAG, "HCP2 HTTP debug log WebSocket connected");
}

bool HCP2Bridge::http_debug_send_ws_text_(socket::Socket *client, const std::string &payload, uint32_t timeout_ms) {
  if (client == nullptr || payload.empty() || payload.size() > 65535u) {
    return false;
  }
  std::string frame;
  frame.reserve(payload.size() + 4u);
  frame.push_back((char) 0x81);
  if (payload.size() <= 125u) {
    frame.push_back((char) payload.size());
  } else {
    frame.push_back((char) 126);
    frame.push_back((char) ((payload.size() >> 8u) & 0xFFu));
    frame.push_back((char) (payload.size() & 0xFFu));
  }
  frame += payload;
  return this->http_debug_write_all_(client, frame, timeout_ms);
}

std::string HCP2Bridge::http_debug_ws_log_json_(const std::string &body) {
  std::string json = "{\"type\":\"log\",\"text\":";
  json += http_debug_json_string_(body);
  json += "}";
  return json;
}

std::string HCP2Bridge::http_debug_ws_health_json_() {
  std::string json = "{\"type\":\"health\",\"health\":";
  json += this->http_debug_health_json_();
  json += "}";
  return json;
}

std::string HCP2Bridge::http_debug_json_string_(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 2u);
  out.push_back('"');
  for (unsigned char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20u) {
          char escaped[7];
          std::snprintf(escaped, sizeof(escaped), "\\u%04X", (unsigned int) c);
          out += escaped;
        } else {
          out.push_back((char) c);
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

std::string HCP2Bridge::http_debug_header_value_(const std::string &request, const char *name) {
  size_t line_start = 0;
  while (line_start < request.size()) {
    size_t line_end = request.find('\n', line_start);
    if (line_end == std::string::npos) {
      line_end = request.size();
    }
    size_t trimmed_end = line_end;
    if (trimmed_end > line_start && request[trimmed_end - 1u] == '\r') {
      trimmed_end--;
    }
    const size_t colon = request.find(':', line_start);
    if (colon != std::string::npos && colon < trimmed_end) {
      const std::string header_name = request.substr(line_start, colon - line_start);
      if (hcp2_ascii_iequals(header_name, name)) {
        size_t value_start = colon + 1u;
        while (value_start < trimmed_end && (request[value_start] == ' ' || request[value_start] == '\t')) {
          value_start++;
        }
        while (trimmed_end > value_start &&
               (request[trimmed_end - 1u] == ' ' || request[trimmed_end - 1u] == '\t')) {
          trimmed_end--;
        }
        return request.substr(value_start, trimmed_end - value_start);
      }
    }
    line_start = line_end + 1u;
  }
  return "";
}

void HCP2Bridge::http_debug_send_response_(std::unique_ptr<socket::Socket> client, const char *status,
                                           const char *content_type, const std::string &body) {
  std::string response = "HTTP/1.1 ";
  response += status;
  response += "\r\nContent-Type: ";
  response += content_type;
  response += "\r\nContent-Length: ";
  response += std::to_string(body.size());
  response += "\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
  response += body;
  if (!this->http_debug_write_all_(client.get(), response, 1000)) {
    ESP_LOGW(TAG, "Failed to write HCP2 HTTP response: errno=%d", errno);
  }
}

void HCP2Bridge::http_debug_send_log_binary_response_(std::unique_ptr<socket::Socket> client) {
  const std::string body = this->protocol_log_body_();
  this->http_debug_send_response_(std::move(client), "200 OK", "application/octet-stream", body);
}

bool HCP2Bridge::http_debug_write_all_(socket::Socket *client, const std::string &payload, uint32_t timeout_ms) {
  size_t offset = 0;
  const uint32_t started = millis();
  while (offset < payload.size()) {
    ssize_t written = client->write(payload.data() + offset, payload.size() - offset);
    if (written > 0) {
      offset += (size_t) written;
      continue;
    }
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      return false;
    }
    if (millis() - started > timeout_ms) {
      return false;
    }
    delay(1);
  }
  return true;
}

std::string HCP2Bridge::http_debug_path_from_request_(const std::string &request) {
  const size_t first_space = request.find(' ');
  if (first_space == std::string::npos) {
    return "";
  }
  const size_t second_space = request.find(' ', first_space + 1);
  if (second_space == std::string::npos || second_space <= first_space + 1) {
    return "";
  }
  std::string path = request.substr(first_space + 1, second_space - first_space - 1);
  const size_t query = path.find('?');
  if (query != std::string::npos) {
    path.resize(query);
  }
  return path;
}

void HCP2Bridge::record_hp_reset_reason_() {
  if (!hcp2_hp_reset_recorded) {
    hcp2_hp_reset_recorded = true;
    hcp2_hp_reset_count++;
    switch (esp_reset_reason()) {
      case ESP_RST_PANIC:
        hcp2_hp_panic_reset_count++;
        break;
      case ESP_RST_INT_WDT:
      case ESP_RST_TASK_WDT:
      case ESP_RST_WDT:
        hcp2_hp_wdt_reset_count++;
        break;
      case ESP_RST_BROWNOUT:
        hcp2_hp_brownout_reset_count++;
        break;
      default:
        break;
    }
  }

  portENTER_CRITICAL(&this->state_mux_);
  this->hp_reset_count_ = hcp2_hp_reset_count;
  this->hp_panic_reset_count_ = hcp2_hp_panic_reset_count;
  this->hp_wdt_reset_count_ = hcp2_hp_wdt_reset_count;
  this->hp_brownout_reset_count_ = hcp2_hp_brownout_reset_count;
  portEXIT_CRITICAL(&this->state_mux_);
}

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

uint32_t HCP2Bridge::fresh_epoch_() const {
  uint32_t epoch = esp_random();
  if (epoch == 0u) {
    epoch = (uint32_t) esp_timer_get_time() ^ 0xC6000001u;
  }
  return epoch;
}

bool HCP2Bridge::setup_lp_core_() {
  if (this->rx_pin_ == nullptr || this->tx_pin_ == nullptr || this->de_pin_ == nullptr) {
    ESP_LOGE(TAG, "rx_pin, tx_pin, and de_pin are required");
    return false;
  }
  if (this->re_pin_ == nullptr) {
    ESP_LOGE(TAG, "re_pin is required for LP RS-485 mode");
    return false;
  }
  if (this->rx_pin_->get_pin() != 4 || this->tx_pin_->get_pin() != 5) {
    ESP_LOGE(TAG, "ESP32-C6 LP-UART requires rx_pin GPIO4 and tx_pin GPIO5");
    return false;
  }

  hcp2_hp_supervisor_init(&this->lp_supervisor_, (volatile hcp2_lp_mailbox_t *) HCP2_LP_MAILBOX_ADDR,
                          HCP2_LP_FIRMWARE_VERSION);
  const esp_err_t err = this->start_or_skip_lp_();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "LP-core setup failed: %s", esp_err_to_name(err));
    return false;
  }
  this->lp_ready_ = true;
  ESP_LOGI(TAG, "Started HCP2 LP supervisor mailbox=0x%08x", (unsigned) HCP2_LP_MAILBOX_ADDR);
  return true;
}

esp_err_t HCP2Bridge::init_lp_bus_io_() {
  const gpio_num_t rx_gpio = static_cast<gpio_num_t>(this->rx_pin_->get_pin());
  const gpio_num_t tx_gpio = static_cast<gpio_num_t>(this->tx_pin_->get_pin());
  const gpio_num_t de_gpio = static_cast<gpio_num_t>(this->de_pin_->get_pin());
  const gpio_num_t re_gpio = static_cast<gpio_num_t>(this->re_pin_->get_pin());
  lp_core_uart_cfg_t uart_cfg{};

  uart_cfg.uart_proto_cfg.baud_rate = HCP2BRIDGE_BAUD_RATE;
  uart_cfg.uart_proto_cfg.data_bits = UART_DATA_8_BITS;
  uart_cfg.uart_proto_cfg.parity = UART_PARITY_EVEN;
  uart_cfg.uart_proto_cfg.stop_bits = UART_STOP_BITS_1;
  uart_cfg.uart_proto_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_cfg.uart_proto_cfg.rx_flow_ctrl_thresh = 0;
  uart_cfg.uart_pin_cfg.tx_io_num = tx_gpio;
  uart_cfg.uart_pin_cfg.rx_io_num = rx_gpio;
  uart_cfg.uart_pin_cfg.rts_io_num = GPIO_NUM_NC;
  uart_cfg.uart_pin_cfg.cts_io_num = GPIO_NUM_NC;
  uart_cfg.lp_uart_source_clk =
      this->lp_uart_clock_source_default_ ? LP_UART_SCLK_DEFAULT : LP_UART_SCLK_XTAL_D2;

  esp_err_t err = rtc_gpio_init(de_gpio);
  if (err != ESP_OK) return err;
  err = rtc_gpio_set_direction(de_gpio, RTC_GPIO_MODE_OUTPUT_ONLY);
  if (err != ESP_OK) return err;
  err = rtc_gpio_set_level(de_gpio, 0);
  if (err != ESP_OK) return err;
  err = rtc_gpio_pulldown_en(de_gpio);
  if (err != ESP_OK) return err;

  err = rtc_gpio_init(re_gpio);
  if (err != ESP_OK) return err;
  err = rtc_gpio_set_direction(re_gpio, RTC_GPIO_MODE_OUTPUT_ONLY);
  if (err != ESP_OK) return err;
  err = rtc_gpio_set_level(re_gpio, 0);
  if (err != ESP_OK) return err;
  err = rtc_gpio_pulldown_en(re_gpio);
  if (err != ESP_OK) return err;

  err = gpio_set_pull_mode(tx_gpio, GPIO_PULLUP_ONLY);
  if (err != ESP_OK) return err;
  return lp_core_uart_init(&uart_cfg);
}

hcp2_lp_reload_decision_t HCP2Bridge::probe_lp_health_(hcp2_lp_health_sample_t *before,
                                                       hcp2_lp_health_sample_t *after) {
  hcp2_lp_health_sample_t before_local;
  hcp2_lp_health_sample_t after_local;

  hcp2_hp_supervisor_sample_health(&this->lp_supervisor_, &before_local);
  vTaskDelay(pdMS_TO_TICKS(HCP2BRIDGE_LP_HEARTBEAT_PROBE_MS));
  hcp2_hp_supervisor_sample_health(&this->lp_supervisor_, &after_local);
  if (before != nullptr) {
    *before = before_local;
  }
  if (after != nullptr) {
    *after = after_local;
  }
  return hcp2_hp_supervisor_reload_decision(&this->lp_supervisor_, &before_local, &after_local);
}

bool HCP2Bridge::healthy_lp_running_(hcp2_lp_health_sample_t *before, hcp2_lp_health_sample_t *after) {
  return this->probe_lp_health_(before, after) == HCP2_LP_RELOAD_SKIP;
}

esp_err_t HCP2Bridge::load_and_start_lp_() {
  volatile hcp2_lp_mailbox_t *mailbox = (volatile hcp2_lp_mailbox_t *) HCP2_LP_MAILBOX_ADDR;
  ulp_lp_core_cfg_t cfg = {
      .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU,
  };

  ulp_lp_core_stop();
  esp_err_t err = this->init_lp_bus_io_();
  if (err != ESP_OK) return err;
  err = ulp_lp_core_load_binary((const uint8_t *) hcp2_lp_blob_data, (size_t) hcp2_lp_blob_data_len);
  if (err != ESP_OK) return err;
  hcp2_lp_mailbox_init(mailbox);
  hcp2_hp_supervisor_begin_session(&this->lp_supervisor_, this->fresh_epoch_());
  err = ulp_lp_core_run(&cfg);
  if (err != ESP_OK) return err;
  ESP_LOGI(TAG, "HCP2_LP_LOAD_RELOAD bytes=%u epoch=%" PRIu32, (unsigned) hcp2_lp_blob_data_len,
           this->lp_supervisor_.epoch);
  return ESP_OK;
}

esp_err_t HCP2Bridge::start_or_skip_lp_() {
  hcp2_lp_health_sample_t before;
  hcp2_lp_health_sample_t after;
  const hcp2_lp_reload_decision_t decision = this->probe_lp_health_(&before, &after);
  if (decision == HCP2_LP_RELOAD_SKIP) {
    hcp2_hp_supervisor_begin_session(&this->lp_supervisor_, this->fresh_epoch_());
    ESP_LOGI(TAG,
             "HCP2_LP_SKIP_RELOAD heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32
             " polls_seen=%" PRIu32 " polls_answered=%" PRIu32 " epoch=%" PRIu32,
             before.heartbeat, after.heartbeat, after.polls_seen, after.polls_answered,
             this->lp_supervisor_.epoch);
    return ESP_OK;
  }
  if (decision == HCP2_LP_RELOAD_DEFER) {
    hcp2_hp_supervisor_begin_session(&this->lp_supervisor_, this->fresh_epoch_());
    ESP_LOGW(TAG,
             "HCP2_LP_RELOAD_DEFER heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32
             " state=0x%02" PRIx8 " command=%" PRIu32 "/%" PRIu32 " epoch=%" PRIu32,
             before.heartbeat, after.heartbeat, after.drive_state, after.command_ack_sequence,
             after.command_sequence, this->lp_supervisor_.epoch);
    return ESP_OK;
  }

  ESP_LOGI(TAG, "HCP2_LP_RELOAD_REQUIRED heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32,
           before.heartbeat, after.heartbeat);
  return this->load_and_start_lp_();
}

void HCP2Bridge::start_lp_supervisor_task_() {
  if (this->lp_supervisor_task_handle_ != nullptr) {
    return;
  }
  const BaseType_t ok = xTaskCreatePinnedToCore(HCP2Bridge::lp_supervisor_task_trampoline_, "hcp2_lp_sup",
                                                HCP2BRIDGE_LP_TASK_STACK_BYTES, this, 5,
                                                &this->lp_supervisor_task_handle_, tskNO_AFFINITY);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to start HCP2 LP supervisor task");
    this->lp_supervisor_task_handle_ = nullptr;
    this->mark_failed();
  }
}

void HCP2Bridge::lp_supervisor_task_trampoline_(void *arg) {
  auto *self = static_cast<HCP2Bridge *>(arg);
  if (self == nullptr) {
    vTaskDelete(nullptr);
    return;
  }
  self->lp_supervisor_task_loop_();
}

void HCP2Bridge::lp_supervisor_task_loop_() {
  for (;;) {
    hcp2_lp_health_sample_t before;
    hcp2_lp_health_sample_t after;
    const hcp2_lp_reload_decision_t decision = this->probe_lp_health_(&before, &after);
    this->update_state_from_mailbox_();
    this->drain_lp_protocol_event_();
    this->drain_lp_trace_();
    if (decision == HCP2_LP_RELOAD_REQUIRED) {
      ESP_LOGE(TAG,
               "HCP2_LP_HEALTH_FAIL reload heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32
               " polls_seen_before=%" PRIu32 " polls_seen_after=%" PRIu32
               " polls_answered_before=%" PRIu32 " polls_answered_after=%" PRIu32,
               before.heartbeat, after.heartbeat, before.polls_seen, after.polls_seen,
               before.polls_answered, after.polls_answered);
      this->protocol_log_append_control_("lp_health_fail");
      if (this->load_and_start_lp_() != ESP_OK) {
        ESP_LOGE(TAG, "HCP2_LP_RELOAD_FAILED");
        this->protocol_log_append_control_("lp_reload_failed");
      } else {
        this->protocol_log_append_control_("lp_reload");
      }
    } else if (decision == HCP2_LP_RELOAD_DEFER) {
      ESP_LOGW(TAG, "HCP2_LP_HEALTH_DEFER state=0x%02" PRIx8 " command=%" PRIu32 "/%" PRIu32,
               after.drive_state, after.command_ack_sequence, after.command_sequence);
      this->protocol_log_append_control_("lp_reload_defer");
    } else if (millis() - this->lp_last_health_log_ms_ >= HCP2BRIDGE_LP_HEALTH_LOG_INTERVAL_MS) {
      this->lp_last_health_log_ms_ = millis();
      ESP_LOGD(TAG, "HCP2_LP_HEALTH_OK heartbeat=%" PRIu32 " polls=%" PRIu32 "/%" PRIu32,
               after.heartbeat, after.polls_answered, after.polls_seen);
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
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
    this->drain_hp_protocol_event_();
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
          this->drain_hp_protocol_event_();
        }
        break;
      }
      case UART_FIFO_OVF:
      case UART_BUFFER_FULL:
        ESP_LOGW(TAG, "UART RX overflow; resetting HCP2 parser");
        uart_flush_input(this->uart_num_);
        xQueueReset(this->uart_event_queue_);
        hcp2_engine_rx_byte(&this->engine_, 0, HCP2_RX_FRAMING_ERROR);
        this->drain_hp_protocol_event_();
        break;
      case UART_PARITY_ERR:
        hcp2_engine_rx_byte(&this->engine_, 0, HCP2_RX_PARITY_ERROR);
        this->drain_hp_protocol_event_();
        break;
      case UART_FRAME_ERR:
      case UART_BREAK:
        hcp2_engine_rx_byte(&this->engine_, 0, HCP2_RX_FRAMING_ERROR);
        this->drain_hp_protocol_event_();
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
      this->protocol_log_append_command_("execute", event.button, false, "busy");
    } else {
      this->protocol_log_append_command_("execute", event.button, true, "hp_fallback");
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
  if (this->engine_.valid_frames != this->valid_frames_) {
    this->last_bus_activity_ms_ = now_ms;
  }
  const bool bus_online = this->last_bus_activity_ms_ != 0u &&
                          (int32_t) (now_ms - (this->last_bus_activity_ms_ + 1000u)) <= 0;
  if (this->bus_online_ != bus_online) {
    this->bus_online_ = bus_online;
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

  if (changed) {
    this->protocol_log_append_state_(this->drive_status_snapshot_(), this->is_bus_online(), this->is_obstructed(),
                                     "hp");
  }
}

void HCP2Bridge::update_state_from_mailbox_() {
  hcp2_lp_state_snapshot_t snapshot{};
  const bool have_state = hcp2_hp_supervisor_read_state(&this->lp_supervisor_, &snapshot) != 0;
  hcp2_drive_status_t status{};
  bool changed = false;

  status.target_position = snapshot.target_position;
  status.current_position = snapshot.current_position;
  status.state = snapshot.state;
  status.light_raw = snapshot.light_on;
  status.light_on = snapshot.light_on;

  volatile hcp2_lp_mailbox_t *mailbox = this->lp_supervisor_.mailbox;
  const uint32_t heartbeat = mailbox != nullptr ? mailbox->heartbeat : 0u;
  const uint32_t polls_seen = mailbox != nullptr ? mailbox->polls_seen : 0u;
  const uint32_t polls_answered = mailbox != nullptr ? mailbox->polls_answered : 0u;
  const uint32_t tx_abort = mailbox != nullptr ? mailbox->tx_abort_count : 0u;
  const uint32_t collision = mailbox != nullptr ? mailbox->collision_count : 0u;
  const uint32_t max_de_hold = mailbox != nullptr ? mailbox->max_de_hold_us : 0u;
  const uint32_t lp_reset_count = mailbox != nullptr ? mailbox->lp_reset_count : 0u;
  const uint32_t lp_time_us = mailbox != nullptr ? mailbox->lp_time_us : 0u;
  const uint32_t last_poll_us = mailbox != nullptr ? mailbox->last_poll_us : 0u;
  const uint32_t crc_errors = mailbox != nullptr ? mailbox->crc_error_count : 0u;
  const uint32_t rx_errors = mailbox != nullptr ? mailbox->rx_error_count : 0u;
  const uint32_t stop_trigger_fires = mailbox != nullptr ? mailbox->stop_trigger_fire_count : 0u;
  const uint32_t health_flags = mailbox != nullptr ? mailbox->health_flags : 0u;
  const uint32_t max_rx_fifo = mailbox != nullptr ? mailbox->max_rx_fifo_count : 0u;
  const uint32_t max_loop_us = mailbox != nullptr ? mailbox->max_loop_us : 0u;
  const uint32_t max_poll_rx_to_schedule_us = mailbox != nullptr ? mailbox->max_poll_rx_to_schedule_us : 0u;
  const uint32_t max_response_schedule_to_tx_start_us =
      mailbox != nullptr ? mailbox->max_response_schedule_to_tx_start_us : 0u;
  const uint32_t max_response_tx_us = mailbox != nullptr ? mailbox->max_response_tx_us : 0u;
  const uint32_t loop_overruns = mailbox != nullptr ? mailbox->loop_overrun_count : 0u;
  const uint32_t rx_starvations = mailbox != nullptr ? mailbox->rx_starvation_count : 0u;
  const uint32_t stuck_de = mailbox != nullptr ? mailbox->stuck_de_count : 0u;
  const uint32_t mailbox_repairs = mailbox != nullptr ? mailbox->mailbox_repair_count : 0u;
  const uint32_t last_poll_age_us = last_poll_us == 0u ? 0u : (uint32_t) (lp_time_us - last_poll_us);
  const bool bus_online = last_poll_us != 0u && (int32_t) (last_poll_age_us - HCP2BRIDGE_BUS_ONLINE_TIMEOUT_US) <= 0;
  const uint32_t last_poll_age_ms = last_poll_age_us / 1000u;
  bool pending_response = false;
  const uint32_t raw_missed_polls = hcp2_raw_missed_polls(polls_seen, polls_answered);
  const uint32_t missed_polls =
      hcp2_effective_missed_polls(polls_seen, polls_answered, last_poll_age_ms, &pending_response);
  const uint32_t now_ms = millis();

  portENTER_CRITICAL(&this->state_mux_);
  if (have_state && std::memcmp(&this->drive_status_, &status, sizeof(this->drive_status_)) != 0) {
    const hcp2_drive_state_code_t previous_state = static_cast<hcp2_drive_state_code_t>(this->drive_status_.state);
    const hcp2_drive_state_code_t current_state = static_cast<hcp2_drive_state_code_t>(status.state);
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
    this->drive_status_ = status;
    changed = true;
  }
  if (!this->valid_broadcast_ && have_state && snapshot.updated_us != 0u) {
    this->valid_broadcast_ = true;
    changed = true;
  }
  if (this->obstruction_ && (int32_t) (now_ms - this->obstruction_until_ms_) >= 0) {
    this->obstruction_ = false;
    changed = true;
  }
  this->valid_frames_ = polls_seen;
  this->responses_sent_ = polls_answered;
  this->crc_errors_ = crc_errors;
  this->rx_errors_ = rx_errors + tx_abort + collision + rx_starvations;
  this->lp_heartbeat_ = heartbeat;
  this->lp_reset_count_ = lp_reset_count;
  this->lp_polls_seen_ = polls_seen;
  this->lp_polls_answered_ = polls_answered;
  this->lp_raw_missed_polls_ = raw_missed_polls;
  this->lp_pending_response_ = pending_response;
  this->lp_tx_abort_count_ = tx_abort;
  this->lp_collision_count_ = collision;
  this->lp_max_de_hold_us_ = max_de_hold;
  this->lp_last_poll_age_ms_ = last_poll_age_ms;
  this->lp_crc_error_count_ = crc_errors;
  this->lp_rx_error_count_ = rx_errors;
  this->lp_stop_trigger_fire_count_ = stop_trigger_fires;
  this->lp_health_flags_ = health_flags;
  this->lp_max_rx_fifo_count_ = max_rx_fifo;
  this->lp_max_loop_us_ = max_loop_us;
  this->lp_max_poll_rx_to_schedule_us_ = max_poll_rx_to_schedule_us;
  this->lp_max_response_schedule_to_tx_start_us_ = max_response_schedule_to_tx_start_us;
  this->lp_max_response_tx_us_ = max_response_tx_us;
  this->lp_loop_overrun_count_ = loop_overruns;
  this->lp_rx_starvation_count_ = rx_starvations;
  this->lp_stuck_de_count_ = stuck_de;
  this->lp_mailbox_repair_count_ = mailbox_repairs;
  if (this->bus_online_ != bus_online) {
    this->bus_online_ = bus_online;
    changed = true;
  }
  const bool continuity_healthy =
      !this->hp_fallback_ && polls_seen > 0u && heartbeat > 0u && this->bus_online_ && this->valid_broadcast_ &&
      missed_polls == 0u && (polls_answered >= polls_seen || pending_response) && health_flags == 0u && tx_abort == 0u &&
      collision == 0u && loop_overruns == 0u && rx_starvations == 0u && stuck_de == 0u &&
      (max_de_hold == 0u || max_de_hold <= HCP2BRIDGE_MAX_DE_HIGH_US);
  if (this->continuity_healthy_ != continuity_healthy) {
    this->continuity_healthy_ = continuity_healthy;
    changed = true;
  }
  if (changed) {
    this->state_callback_pending_ = true;
  }
  portEXIT_CRITICAL(&this->state_mux_);

  if (changed) {
    this->protocol_log_append_state_(this->drive_status_snapshot_(), this->is_bus_online(), this->is_obstructed(),
                                     "lp");
  }
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
  if (enabled) {
    if (this->de_pin_ != nullptr) {
      this->de_pin_->digital_write(true);
    }
    if (this->re_pin_ != nullptr) {
      this->re_pin_->digital_write(true);
    }
  } else {
    if (this->re_pin_ != nullptr) {
      this->re_pin_->digital_write(false);
    }
    if (this->de_pin_ != nullptr) {
      this->de_pin_->digital_write(false);
    }
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
