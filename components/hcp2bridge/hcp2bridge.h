#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "hcp2_backend.h"

extern "C" {
#include "hcp2_engine.h"
#include "hcp2_supervisor.h"
}

#ifdef USE_ESP32
#include "esphome/components/socket/socket.h"
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
  void set_backend_kind(HCP2BackendKind kind) {
    this->backend_kind_ = kind;
    this->hp_fallback_ = kind == HCP2BackendKind::HP_FALLBACK;
  }
  void set_hp_fallback(bool value) {
    this->set_backend_kind(value ? HCP2BackendKind::HP_FALLBACK : HCP2BackendKind::ESP32C6_LP);
  }
  void set_lp_uart_clock_source_default(bool value) { this->lp_uart_clock_source_default_ = value; }
  void set_rs485_mode(HCP2RS485Mode mode) { this->rs485_mode_ = mode; }
  void set_esp32_realtime_board_profile(HCP2RealtimeBoardProfile profile) {
    this->esp32_realtime_board_profile_ = profile;
  }
  void set_restart_policy(HCP2RestartPolicy policy) { this->restart_policy_ = policy; }
  void set_bench_allow_destructive_debug_actions(bool value) { this->bench_allow_destructive_debug_actions_ = value; }
  void set_http_debug_port(uint16_t port) { this->http_debug_port_ = port; }
  void set_protocol_log_enabled(bool enabled) { this->protocol_log_enabled_ = enabled; }

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
  bool is_continuity_healthy() const;
  bool is_safe_for_ota_restart() const;
  bool has_continuity_problem() const { return !this->is_continuity_healthy(); }
  bool has_continuity_diagnostic_warning() const;
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
  uint32_t get_lp_raw_missed_poll_count() const;
  bool has_lp_pending_response() const;
  uint32_t get_lp_tx_abort_count() const;
  uint32_t get_lp_collision_count() const;
  uint32_t get_lp_max_de_hold_us() const;
  uint32_t get_lp_last_poll_age_ms() const;
  uint32_t get_lp_crc_error_count() const;
  uint32_t get_lp_rx_error_count() const;
  uint32_t get_lp_stop_trigger_fire_count() const;
  uint32_t get_lp_health_flags() const;
  uint32_t get_lp_max_rx_fifo_count() const;
  uint32_t get_lp_max_loop_us() const;
  uint32_t get_lp_max_poll_rx_to_schedule_us() const;
  uint32_t get_lp_max_response_schedule_to_tx_start_us() const;
  uint32_t get_lp_max_response_tx_us() const;
  uint32_t get_lp_loop_overrun_count() const;
  uint32_t get_lp_rx_starvation_count() const;
  uint32_t get_lp_stuck_de_count() const;
  uint32_t get_lp_mailbox_repair_count() const;
  uint32_t get_continuity_good_ms() const;
  uint32_t get_hp_reset_count() const;
  uint32_t get_hp_panic_reset_count() const;
  uint32_t get_hp_wdt_reset_count() const;
  uint32_t get_hp_brownout_reset_count() const;

 protected:
#ifdef USE_ESP32
  struct CommandEvent {
    hcp2_button_t button;
  };

  enum class HttpDebugHpAction : uintptr_t {
    RESTART = 1,
    CPU_RESET = 2,
    PANIC = 3,
  };

  static void bus_task_trampoline_(void *arg);
  void bus_task_loop_();
  void start_hp_fallback_task_();
  void start_lp_supervisor_task_();
  bool setup_uart_();
  bool setup_esp32_realtime_();
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
  void setup_protocol_log_();
  bool http_debug_enabled_() const { return this->http_debug_port_ != 0; }
  void setup_http_debug_server_();
  void start_http_debug_task_();
  static void http_debug_task_trampoline_(void *arg);
  void http_debug_task_loop_();
  bool http_debug_network_ready_() const;
  void http_debug_shutdown_server_(const char *reason);
  void maybe_setup_http_debug_server_();
  void http_debug_accept_client_();
  void http_debug_service_pending_client_();
  void http_debug_service_log_ws_();
  void http_debug_close_log_ws_(const char *reason);
  void http_debug_handle_request_(std::unique_ptr<socket::Socket> client, const std::string &request);
  bool http_debug_schedule_hp_action_(HttpDebugHpAction action);
  static void http_debug_hp_action_task_(void *arg);
  void http_debug_send_response_(std::unique_ptr<socket::Socket> client, const char *status, const char *content_type,
                                 const std::string &body);
  void http_debug_send_log_binary_response_(std::unique_ptr<socket::Socket> client);
  void http_debug_upgrade_log_ws_(std::unique_ptr<socket::Socket> client, const std::string &request);
  bool http_debug_send_ws_text_(socket::Socket *client, const std::string &payload, uint32_t timeout_ms);
  std::string http_debug_ws_log_json_(const std::string &body);
  std::string http_debug_ws_health_json_();
  static std::string http_debug_json_string_(const std::string &value);
  std::string http_debug_header_value_(const std::string &request, const char *name);
  bool http_debug_write_all_(socket::Socket *client, const std::string &payload, uint32_t timeout_ms);
  bool http_debug_request_complete_() const;
  std::string http_debug_path_from_request_(const std::string &request);
  std::string http_debug_index_html_();
  std::string http_debug_health_json_();
  std::string http_debug_stats_json_();
  std::string http_debug_support_json_();
  std::string http_debug_door_json_();
  std::string http_debug_lp_json_();
  std::string http_debug_hp_json_();
  std::string protocol_log_summary_json_() const;
  std::string protocol_log_body_();
  std::string protocol_log_body_since_(uint32_t cursor_seq, uint32_t *next_cursor_seq, size_t max_bytes);
  uint32_t protocol_log_next_seq_snapshot_() const;
  static uint32_t protocol_log_line_seq_(const std::string &line);
  void protocol_log_clear_();
  void protocol_log_append_line_(const std::string &line, bool force = false);
  void protocol_log_discard_oldest_line_locked_();
  void protocol_log_append_control_(const char *action);
  void protocol_log_append_command_(const char *phase, hcp2_button_t button, bool ok, const char *reason);
  void protocol_log_append_state_(const hcp2_drive_status_t &status, bool bus_online, bool obstruction,
                                  const char *source);
  void protocol_log_append_protocol_event_(uint32_t event_us, uint8_t event_type, uint8_t frame_type,
                                           const uint8_t *data, uint8_t len, const char *source);
  void protocol_log_append_lp_trace_(const hcp2_lp_trace_entry_t &entry);
  void protocol_log_append_lp_trace_overflow_(uint32_t dropped);
  static bool lp_trace_should_log_(const hcp2_lp_trace_entry_t &entry);
  void drain_lp_protocol_event_();
  void drain_lp_trace_();
  void drain_hp_protocol_event_();
  bool backend_uses_mailbox_() const { return hcp2_backend_uses_mailbox(this->backend_kind_); }
  bool backend_survives_hp_restart_() const { return hcp2_backend_survives_hp_restart(this->backend_kind_); }
  bool backend_supports_stop_trigger_() const { return hcp2_backend_supports_stop_trigger(this->backend_kind_); }
  bool backend_is_hp_fallback_() const { return this->backend_kind_ == HCP2BackendKind::HP_FALLBACK; }
  bool mailbox_backend_ready_() const { return this->backend_uses_mailbox_() && this->backend_ready_; }
  std::string hex_encode_(const uint8_t *data, size_t len) const;
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
  bool apply_drive_status_locked_(const hcp2_drive_status_t *status, bool valid_broadcast_seen, uint32_t now_ms);
  static const char *button_name_(hcp2_button_t button);
  static const char *protocol_event_name_(uint8_t event_type);
  static const char *frame_type_name_(uint8_t frame_type);
  static const char *lp_trace_event_name_(uint16_t event);

  InternalGPIOPin *rx_pin_{nullptr};
  InternalGPIOPin *tx_pin_{nullptr};
  InternalGPIOPin *de_pin_{nullptr};
  InternalGPIOPin *re_pin_{nullptr};
  hcp2_engine_config_t config_{};
  HCP2BackendKind backend_kind_{HCP2BackendKind::ESP32C6_LP};
  HCP2RS485Mode rs485_mode_{HCP2RS485Mode::DE_RE};
  HCP2RealtimeBoardProfile esp32_realtime_board_profile_{HCP2RealtimeBoardProfile::ESP32_WROOM_NO_PSRAM};
  HCP2RestartPolicy restart_policy_{HCP2RestartPolicy::NO_AUTO_RESTART};
  bool hp_fallback_{false};
  bool lp_uart_clock_source_default_{false};
  uint8_t uart_num_config_{1};
  uint16_t http_debug_port_{0};
  bool protocol_log_enabled_{false};
  bool bench_allow_destructive_debug_actions_{false};

  hcp2_drive_status_t drive_status_{};
  bool valid_broadcast_{false};
  bool obstruction_{false};
  bool bus_online_{false};
  bool continuity_healthy_{false};
  bool continuity_diagnostic_warning_{false};
  bool continuity_sample_seen_{false};
  uint32_t continuity_good_since_ms_{0};
  uint32_t continuity_good_ms_{0};
  uint32_t continuity_prev_polls_seen_{0};
  uint32_t continuity_prev_polls_answered_{0};
  uint32_t continuity_prev_health_flags_{0};
  uint32_t continuity_prev_tx_abort_count_{0};
  uint32_t continuity_prev_collision_count_{0};
  uint32_t continuity_prev_max_de_hold_us_{0};
  uint32_t continuity_prev_loop_overrun_count_{0};
  uint32_t continuity_prev_rx_starvation_count_{0};
  uint32_t continuity_prev_stuck_de_count_{0};
  uint32_t continuity_prev_mailbox_repair_count_{0};
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
  uint32_t lp_raw_missed_polls_{0};
  bool lp_pending_response_{false};
  uint32_t lp_tx_abort_count_{0};
  uint32_t lp_collision_count_{0};
  uint32_t lp_max_de_hold_us_{0};
  uint32_t lp_last_poll_age_ms_{0};
  uint32_t lp_crc_error_count_{0};
  uint32_t lp_rx_error_count_{0};
  uint32_t lp_stop_trigger_fire_count_{0};
  uint32_t lp_health_flags_{0};
  uint32_t lp_max_rx_fifo_count_{0};
  uint32_t lp_max_loop_us_{0};
  uint32_t lp_max_poll_rx_to_schedule_us_{0};
  uint32_t lp_max_response_schedule_to_tx_start_us_{0};
  uint32_t lp_max_response_tx_us_{0};
  uint32_t lp_loop_overrun_count_{0};
  uint32_t lp_rx_starvation_count_{0};
  uint32_t lp_stuck_de_count_{0};
  uint32_t lp_mailbox_repair_count_{0};
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
  TaskHandle_t http_debug_task_handle_{nullptr};
  mutable portMUX_TYPE state_mux_ = portMUX_INITIALIZER_UNLOCKED;
  hcp2_engine_t engine_{};
  hcp2_hp_supervisor_t lp_supervisor_{};
  bool uart_ready_{false};
  bool bus_task_started_{false};
  bool backend_ready_{false};
  uint32_t lp_last_health_log_ms_{0};
  static constexpr size_t PROTOCOL_LOG_CAPACITY = 49152;
  bool protocol_log_ready_{false};
  uint8_t protocol_log_buffer_[PROTOCOL_LOG_CAPACITY]{};
  size_t protocol_log_start_{0};
  size_t protocol_log_used_{0};
  uint32_t protocol_log_next_seq_{1};
  uint32_t protocol_log_dropped_records_{0};
  uint32_t protocol_log_dropped_bytes_{0};
  uint32_t last_lp_protocol_sequence_{0};
  uint32_t last_lp_trace_head_{0};
  uint32_t last_hp_protocol_sequence_{0};
  mutable portMUX_TYPE protocol_log_mux_ = portMUX_INITIALIZER_UNLOCKED;
  std::unique_ptr<socket::ListenSocket> http_debug_server_;
  std::unique_ptr<socket::Socket> http_debug_pending_client_;
  std::unique_ptr<socket::Socket> http_debug_log_ws_client_;
  char http_debug_request_buffer_[1024]{};
  size_t http_debug_request_buffer_len_{0};
  uint32_t http_debug_pending_client_started_ms_{0};
  uint32_t http_debug_next_setup_ms_{0};
  uint32_t http_debug_log_ws_next_seq_{0};
  uint32_t http_debug_log_ws_last_send_ms_{0};
  uint32_t http_debug_log_ws_last_status_ms_{0};
  uint32_t http_debug_log_ws_connect_count_{0};
  uint32_t http_debug_log_ws_disconnect_count_{0};
  uint32_t http_debug_log_ws_reject_count_{0};
  uint32_t http_debug_log_ws_peer_close_count_{0};
  uint32_t http_debug_log_ws_read_fail_count_{0};
  uint32_t http_debug_log_ws_write_fail_count_{0};
  int http_debug_log_ws_last_errno_{0};
  const char *http_debug_log_ws_last_close_reason_{"none"};
#endif
};

}  // namespace hcp2bridge
}  // namespace esphome
