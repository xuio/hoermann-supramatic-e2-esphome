#pragma once

#include <cstdint>
#include <string>

#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

extern "C" {
#include "hcp2_engine.h"
#include "hcp2_supervisor.h"
}

#ifdef USE_ESP32
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#endif

namespace esphome {
namespace hcp2bridge {

class HCP2Bridge : public Component {
 public:
  HCP2Bridge();
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BUS; }

  void set_rx_pin(InternalGPIOPin *pin) { this->rx_pin_ = pin; }
  void set_tx_pin(InternalGPIOPin *pin) { this->tx_pin_ = pin; }
  void set_de_pin(InternalGPIOPin *pin) { this->de_pin_ = pin; }
  void set_re_pin(InternalGPIOPin *pin) { this->re_pin_ = pin; }
  void set_uart_num(uint8_t uart_num) { this->uart_num_config_ = uart_num; }
  void set_slave_id(uint8_t slave_id) { this->config_.slave_id = slave_id; }
  void set_signature_byte(uint8_t index, uint8_t value);
  void set_response_delay_us(uint32_t value) { this->config_.response_delay_us = value; }
  void set_button_press_us(uint32_t value) { this->config_.button_press_us = value; }
  void set_hp_fallback(bool value) { this->hp_fallback_ = value; }
  void set_lp_uart_clock_source_default(bool value) { this->lp_uart_clock_source_default_ = value; }

  bool action_open() { return this->queue_button_(HCP2_BUTTON_OPEN); }
  bool action_close() { return this->queue_button_(HCP2_BUTTON_CLOSE); }
  bool action_stop() { return this->queue_button_(HCP2_BUTTON_STOP); }
  bool action_vent() { return this->queue_button_(HCP2_BUTTON_VENT); }
  bool action_half() { return this->queue_button_(HCP2_BUTTON_HALF); }
  bool action_light() { return this->queue_button_(HCP2_BUTTON_LIGHT); }
  bool arm_stop_trigger(float target_position, uint32_t ttl_ms);
  void disarm_stop_trigger();

  void add_on_state_callback(std::function<void()> &&callback) { this->state_callback_.add(std::move(callback)); }
  void add_on_command_callback(std::function<void()> &&callback) { this->command_callback_.add(std::move(callback)); }

  bool has_valid_broadcast() const;
  bool is_light_on() const;
  bool is_moving() const;
  bool is_open() const;
  bool is_closed() const;
  bool is_obstructed() const;
  bool is_bus_online() const;
  float get_position() const;
  hcp2_drive_state_code_t get_drive_state() const;
  std::string get_state_string() const;
  uint32_t get_command_sequence() const;
  uint32_t get_valid_frame_count() const;
  uint32_t get_crc_error_count() const;
  uint32_t get_rx_error_count() const;
  uint32_t get_response_count() const;
  uint32_t get_lp_heartbeat() const;
  uint32_t get_lp_reset_count() const;
  uint32_t get_lp_poll_count() const;
  uint32_t get_lp_response_count() const;
  uint32_t get_lp_missed_poll_count() const;
  uint32_t get_lp_tx_abort_count() const;
  uint32_t get_lp_collision_count() const;
  uint32_t get_lp_max_de_hold_us() const;
  uint32_t get_lp_last_poll_age_ms() const;
  uint32_t get_lp_crc_error_count() const;
  uint32_t get_lp_rx_error_count() const;
  uint32_t get_lp_stop_trigger_fire_count() const;
  uint32_t get_hp_reset_count() const;
  uint32_t get_hp_panic_reset_count() const;
  uint32_t get_hp_wdt_reset_count() const;
  uint32_t get_hp_brownout_reset_count() const;

 protected:
#ifdef USE_ESP32
  struct CommandEvent {
    hcp2_button_t button;
  };

  static void bus_task_trampoline_(void *arg);
  void bus_task_loop_();
  void start_hp_fallback_task_();
  void start_lp_supervisor_task_();
  bool setup_uart_();
  bool setup_lp_core_();
  esp_err_t init_lp_bus_io_();
  esp_err_t load_and_start_lp_();
  esp_err_t start_or_skip_lp_();
  hcp2_lp_reload_decision_t probe_lp_health_(hcp2_lp_health_sample_t *before, hcp2_lp_health_sample_t *after);
  bool healthy_lp_running_(hcp2_lp_health_sample_t *before, hcp2_lp_health_sample_t *after);
  static void lp_supervisor_task_trampoline_(void *arg);
  void lp_supervisor_task_loop_();
  void drain_command_queue_();
  void update_state_from_engine_();
  void update_state_from_mailbox_();
  TickType_t pending_tx_wait_ticks_() const;
  void set_de_(bool enabled);
  uint32_t fresh_epoch_() const;
  static uint32_t now_us_cb_(void *user);
  static void tx_cb_(void *user, const uint8_t *data, uint8_t len);
  static void de_set_cb_(void *user, uint8_t enabled);
  void record_hp_reset_reason_();
#endif

  bool queue_button_(hcp2_button_t button);
  static hcp2_lp_command_id_t lp_command_for_button_(hcp2_button_t button);
  const char *drive_state_name_() const;
  hcp2_drive_status_t drive_status_snapshot_() const;
  bool valid_broadcast_snapshot_() const;
  bool obstruction_snapshot_() const;
  uint32_t counter_snapshot_(uint32_t HCP2Bridge::*field) const;

  InternalGPIOPin *rx_pin_{nullptr};
  InternalGPIOPin *tx_pin_{nullptr};
  InternalGPIOPin *de_pin_{nullptr};
  InternalGPIOPin *re_pin_{nullptr};
  hcp2_engine_config_t config_{};
  bool hp_fallback_{true};
  bool lp_uart_clock_source_default_{false};
  uint8_t uart_num_config_{1};

  hcp2_drive_status_t drive_status_{};
  bool valid_broadcast_{false};
  bool obstruction_{false};
  bool bus_online_{false};
  bool state_callback_pending_{false};
  bool command_callback_pending_{false};
  hcp2_button_t last_commanded_button_{HCP2_BUTTON_NONE};
  uint32_t last_commanded_ms_{0};
  uint32_t obstruction_until_ms_{0};
  uint32_t command_sequence_{0};
  uint32_t valid_frames_{0};
  uint32_t crc_errors_{0};
  uint32_t rx_errors_{0};
  uint32_t responses_sent_{0};
  uint32_t last_bus_activity_ms_{0};
  uint32_t lp_heartbeat_{0};
  uint32_t lp_reset_count_{0};
  uint32_t lp_polls_seen_{0};
  uint32_t lp_polls_answered_{0};
  uint32_t lp_tx_abort_count_{0};
  uint32_t lp_collision_count_{0};
  uint32_t lp_max_de_hold_us_{0};
  uint32_t lp_last_poll_age_ms_{0};
  uint32_t lp_crc_error_count_{0};
  uint32_t lp_rx_error_count_{0};
  uint32_t lp_stop_trigger_fire_count_{0};
  uint32_t hp_reset_count_{0};
  uint32_t hp_panic_reset_count_{0};
  uint32_t hp_wdt_reset_count_{0};
  uint32_t hp_brownout_reset_count_{0};

  CallbackManager<void()> state_callback_;
  CallbackManager<void()> command_callback_;

#ifdef USE_ESP32
  uart_port_t uart_num_{UART_NUM_1};
  QueueHandle_t uart_event_queue_{nullptr};
  QueueHandle_t command_queue_{nullptr};
  TaskHandle_t bus_task_handle_{nullptr};
  TaskHandle_t lp_supervisor_task_handle_{nullptr};
  mutable portMUX_TYPE state_mux_ = portMUX_INITIALIZER_UNLOCKED;
  hcp2_engine_t engine_{};
  hcp2_hp_supervisor_t lp_supervisor_{};
  bool uart_ready_{false};
  bool bus_task_started_{false};
  bool lp_ready_{false};
  uint32_t lp_last_health_log_ms_{0};
#endif
};

}  // namespace hcp2bridge
}  // namespace esphome
