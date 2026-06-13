#include "hcp2bridge.h"
#include "hcp2_entity_mapping.h"
#include "hcp2_lp_blob.h"

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
  this->record_hp_reset_reason_();
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
  const uint32_t seen = this->lp_polls_seen_;
  const uint32_t answered = this->lp_polls_answered_;
#ifdef USE_ESP32
  portEXIT_CRITICAL(&this->state_mux_);
#endif
  return seen >= answered ? seen - answered : 0u;
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
      return false;
    }
    const hcp2_lp_command_id_t command = lp_command_for_button_(button);
    const uint32_t sequence = hcp2_hp_supervisor_send_command(&this->lp_supervisor_, command, 0u);
    if (sequence == 0u) {
      ESP_LOGW(TAG, "Failed to write HCP2 command to LP mailbox");
      return false;
    }

    portENTER_CRITICAL(&this->state_mux_);
    this->last_commanded_button_ = button;
    this->last_commanded_ms_ = millis();
    this->command_sequence_++;
    this->command_callback_pending_ = true;
    portEXIT_CRITICAL(&this->state_mux_);
    return true;
  }

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
  uart_cfg.lp_uart_source_clk = LP_UART_SCLK_XTAL_D2;

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
    if (decision == HCP2_LP_RELOAD_REQUIRED) {
      ESP_LOGE(TAG,
               "HCP2_LP_HEALTH_FAIL reload heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32
               " polls_seen_before=%" PRIu32 " polls_seen_after=%" PRIu32
               " polls_answered_before=%" PRIu32 " polls_answered_after=%" PRIu32,
               before.heartbeat, after.heartbeat, before.polls_seen, after.polls_seen,
               before.polls_answered, after.polls_answered);
      if (this->load_and_start_lp_() != ESP_OK) {
        ESP_LOGE(TAG, "HCP2_LP_RELOAD_FAILED");
      }
    } else if (decision == HCP2_LP_RELOAD_DEFER) {
      ESP_LOGW(TAG, "HCP2_LP_HEALTH_DEFER state=0x%02" PRIx8 " command=%" PRIu32 "/%" PRIu32,
               after.drive_state, after.command_ack_sequence, after.command_sequence);
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
  const uint32_t last_poll_age_us = last_poll_us == 0u ? 0u : (uint32_t) (lp_time_us - last_poll_us);
  const bool bus_online = last_poll_us != 0u && (int32_t) (last_poll_age_us - HCP2BRIDGE_BUS_ONLINE_TIMEOUT_US) <= 0;
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
  this->rx_errors_ = rx_errors + tx_abort + collision;
  this->lp_heartbeat_ = heartbeat;
  this->lp_reset_count_ = lp_reset_count;
  this->lp_polls_seen_ = polls_seen;
  this->lp_polls_answered_ = polls_answered;
  this->lp_tx_abort_count_ = tx_abort;
  this->lp_collision_count_ = collision;
  this->lp_max_de_hold_us_ = max_de_hold;
  this->lp_last_poll_age_ms_ = last_poll_age_us / 1000u;
  this->lp_crc_error_count_ = crc_errors;
  this->lp_rx_error_count_ = rx_errors;
  this->lp_stop_trigger_fire_count_ = stop_trigger_fires;
  if (this->bus_online_ != bus_online) {
    this->bus_online_ = bus_online;
    changed = true;
  }
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
