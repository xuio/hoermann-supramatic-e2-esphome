#pragma once
#include "esphome/core/component.h"
#include "esphome/components/cover/cover.h"
#include "../uapbridge.h"

namespace esphome {
namespace uapbridge {

class UAPBridgeCover : public cover::Cover, public Component {
 public:
  void set_uapbridge_parent(UAPBridge *parent) { this->parent_ = parent; }
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_event_triggered();
  void control(const cover::CoverCall &call) override;
  cover::CoverTraits get_traits() override;
  void set_time_based_position(bool value) { this->time_based_position_ = value; }
  void set_open_duration(uint32_t value);
  uint32_t get_open_duration() const { return this->open_duration_ms_; }
  void set_close_duration(uint32_t value);
  uint32_t get_close_duration() const { return this->close_duration_ms_; }
  void set_open_start_delay(uint32_t value);
  uint32_t get_open_start_delay() const { return this->open_start_delay_ms_; }
  void set_close_start_delay(uint32_t value);
  uint32_t get_close_start_delay() const { return this->close_start_delay_ms_; }
  void set_open_report_delay(uint32_t value);
  uint32_t get_open_report_delay() const { return this->open_report_delay_ms_; }
  void set_close_report_delay(uint32_t value);
  uint32_t get_close_report_delay() const { return this->close_report_delay_ms_; }
  void set_close_obstruction_grace(uint32_t value) { this->close_obstruction_grace_ms_ = value; }
  void set_position_publish_interval(uint32_t value) { this->position_publish_interval_ms_ = value; }
  void set_position_deadband(float value) { this->position_deadband_ = value; }
  void set_venting_position(float value) { this->venting_position_ = value; }
  void set_learn_travel_durations(bool value) { this->learn_travel_durations_ = value; }
  void set_use_motion_curve(bool value) { this->use_motion_curve_ = value; }
 protected:
    UAPBridge *parent_;
    cover::CoverOperation previousOperation_ = cover::COVER_OPERATION_IDLE;
    UAPBridge::hoermann_state_t previousState_ = UAPBridge::hoermann_state_t::hoermann_state_stopped;
    float previousPosition_ = -1.0f;
    bool time_based_position_ = false;
    uint32_t open_duration_ms_ = 18000;
    uint32_t close_duration_ms_ = 18000;
    uint32_t open_start_delay_ms_ = 0;
    uint32_t close_start_delay_ms_ = 0;
    uint32_t open_report_delay_ms_ = 0;
    uint32_t close_report_delay_ms_ = 0;
    uint32_t close_obstruction_grace_ms_ = 5000;
    uint32_t position_publish_interval_ms_ = 1000;
    float position_deadband_ = 0.02f;
    float venting_position_ = 0.2f;
    bool learn_travel_durations_ = true;
    bool use_motion_curve_ = false;
    float target_position_ = 1.0f;
    uint32_t last_recompute_time_ = 0;
    uint32_t last_publish_time_ = 0;
    uint32_t movement_started_ms_ = 0;
    float movement_start_position_ = 0.5f;
    bool pending_movement_ = false;
    cover::CoverOperation pending_operation_ = cover::COVER_OPERATION_IDLE;
    float pending_target_position_ = 1.0f;
    uint32_t pending_command_sequence_ = 0;
    uint32_t pending_started_ms_ = 0;
    bool waiting_for_departure_ = false;
    cover::CoverOperation departure_operation_ = cover::COVER_OPERATION_IDLE;
    float departure_target_position_ = 1.0f;
    UAPBridge::hoermann_state_t departure_old_state_ = UAPBridge::hoermann_state_t::hoermann_state_unknown;
    uint32_t departure_wait_started_ms_ = 0;
    bool waiting_for_end_state_ = false;
    uint32_t end_state_wait_started_ms_ = 0;
    uint32_t movement_start_grace_until_ms_ = 0;
    bool travel_measurement_active_ = false;
    cover::CoverOperation travel_measurement_operation_ = cover::COVER_OPERATION_IDLE;
    uint32_t travel_measurement_started_ms_ = 0;
    float travel_measurement_start_position_ = 0.5f;
    bool setup_complete_ = false;
    ESPPreferenceObject travel_duration_pref_;
    void control_time_based_position_(float target);
    void handle_time_based_event_();
    void arm_pending_movement_(cover::CoverOperation operation, float target, const char *reason);
    void service_pending_movement_();
    void clear_pending_movement_(const char *reason);
    void start_departure_wait_(cover::CoverOperation operation, float target, UAPBridge::hoermann_state_t old_state,
                               const char *reason);
    void service_departure_wait_();
    void clear_departure_wait_(const char *reason);
    bool should_wait_for_departure_(cover::CoverOperation operation, UAPBridge::hoermann_state_t state) const;
    void start_estimated_movement_(cover::CoverOperation operation, float target, const char *reason,
                                   bool movement_confirmed = false);
    bool should_ignore_old_end_state_(UAPBridge::hoermann_state_t state) const;
    void recompute_position_();
    bool is_at_target_() const;
    bool target_is_end_state_() const;
    void latch_close_obstruction_(const char *reason);
    void service_end_state_wait_();
    void complete_estimated_target_();
    void sync_known_position_(float position, const char *reason);
    void stop_estimated_movement_(const char *reason);
    void begin_travel_measurement_(cover::CoverOperation operation);
    void finish_travel_measurement_(UAPBridge::hoermann_state_t end_state);
    void load_travel_durations_();
    void save_travel_durations_();
    bool is_valid_travel_duration_(uint32_t duration_ms) const;
    bool is_valid_delay_(uint32_t delay_ms) const;
    uint32_t start_delay_for_operation_(cover::CoverOperation operation) const;
    uint32_t report_delay_for_operation_(cover::CoverOperation operation) const;
    float curve_time_for_position_(cover::CoverOperation operation, float position) const;
    float curve_position_for_time_(cover::CoverOperation operation, float normalized_time) const;
    void force_non_closed_estimate_(const char *reason);
    void publish_if_changed_(bool force = false, bool save = true);
    bool almost_equal_(float a, float b) const;
    bool is_closed_target_(float target) const;
    bool is_open_target_(float target) const;
};

}  // namespace uapbridge
}  // namespace esphome
