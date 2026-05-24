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
  void set_open_duration(uint32_t value) { this->open_duration_ms_ = value; }
  void set_close_duration(uint32_t value) { this->close_duration_ms_ = value; }
  void set_position_publish_interval(uint32_t value) { this->position_publish_interval_ms_ = value; }
  void set_position_deadband(float value) { this->position_deadband_ = value; }
  void set_venting_position(float value) { this->venting_position_ = value; }
  void set_learn_travel_durations(bool value) { this->learn_travel_durations_ = value; }
 protected:
    UAPBridge *parent_;
    cover::CoverOperation previousOperation_ = cover::COVER_OPERATION_IDLE;
    UAPBridge::hoermann_state_t previousState_ = UAPBridge::hoermann_state_t::hoermann_state_stopped;
    float previousPosition_ = -1.0f;
    bool time_based_position_ = false;
    uint32_t open_duration_ms_ = 18000;
    uint32_t close_duration_ms_ = 18000;
    uint32_t position_publish_interval_ms_ = 1000;
    float position_deadband_ = 0.02f;
    float venting_position_ = 0.2f;
    bool learn_travel_durations_ = true;
    float target_position_ = 1.0f;
    uint32_t last_recompute_time_ = 0;
    uint32_t last_publish_time_ = 0;
    bool pending_movement_ = false;
    cover::CoverOperation pending_operation_ = cover::COVER_OPERATION_IDLE;
    float pending_target_position_ = 1.0f;
    uint32_t pending_command_sequence_ = 0;
    uint32_t pending_started_ms_ = 0;
    bool waiting_for_end_state_ = false;
    bool travel_measurement_active_ = false;
    cover::CoverOperation travel_measurement_operation_ = cover::COVER_OPERATION_IDLE;
    uint32_t travel_measurement_started_ms_ = 0;
    float travel_measurement_start_position_ = 0.5f;
    void control_time_based_position_(float target);
    void handle_time_based_event_();
    void arm_pending_movement_(cover::CoverOperation operation, float target, const char *reason);
    void service_pending_movement_();
    void clear_pending_movement_(const char *reason);
    void start_estimated_movement_(cover::CoverOperation operation, float target, const char *reason);
    void recompute_position_();
    bool is_at_target_() const;
    void complete_estimated_target_();
    void sync_known_position_(float position, const char *reason);
    void stop_estimated_movement_(const char *reason);
    void begin_travel_measurement_(cover::CoverOperation operation);
    void finish_travel_measurement_(UAPBridge::hoermann_state_t end_state);
    void force_non_closed_estimate_(const char *reason);
    void publish_if_changed_(bool force = false, bool save = true);
    bool almost_equal_(float a, float b) const;
    bool is_closed_target_(float target) const;
    bool is_open_target_(float target) const;
};

}  // namespace uapbridge
}  // namespace esphome
