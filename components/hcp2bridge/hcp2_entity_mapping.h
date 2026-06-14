#pragma once

#include <algorithm>
#include <cmath>

extern "C" {
#include "hcp2_engine.h"
}

namespace esphome {
namespace hcp2bridge {

enum class HCP2CoverOperation {
  IDLE,
  OPENING,
  CLOSING,
};

inline bool hcp2_state_is_light_on(const hcp2_drive_status_t &status) { return status.light_on != 0; }

inline bool hcp2_state_is_moving(const hcp2_drive_status_t &status) {
  return status.state == HCP2_DRIVE_OPENING || status.state == HCP2_DRIVE_CLOSING ||
         status.state == HCP2_DRIVE_HALF_OPENING || status.state == HCP2_DRIVE_VENT_MOVING;
}

inline bool hcp2_state_code_is_opening(hcp2_drive_state_code_t state) {
  return state == HCP2_DRIVE_OPENING || state == HCP2_DRIVE_HALF_OPENING || state == HCP2_DRIVE_VENT_MOVING;
}

inline bool hcp2_state_code_is_closing(hcp2_drive_state_code_t state) { return state == HCP2_DRIVE_CLOSING; }

inline bool hcp2_state_is_open(const hcp2_drive_status_t &status) { return status.state == HCP2_DRIVE_OPEN; }

inline bool hcp2_state_is_closed(const hcp2_drive_status_t &status) { return status.state == HCP2_DRIVE_CLOSED; }

inline float hcp2_state_position(const hcp2_drive_status_t &status) {
  return std::min(1.0f, static_cast<float>(status.current_position) / 200.0f);
}

inline const char *hcp2_state_name(hcp2_drive_state_code_t state) {
  switch (state) {
    case HCP2_DRIVE_STOPPED:
      return "stopped";
    case HCP2_DRIVE_OPENING:
      return "opening";
    case HCP2_DRIVE_CLOSING:
      return "closing";
    case HCP2_DRIVE_HALF_OPENING:
      return "half_opening";
    case HCP2_DRIVE_VENT_MOVING:
      return "vent_moving";
    case HCP2_DRIVE_VENT:
      return "vent";
    case HCP2_DRIVE_OPEN:
      return "open";
    case HCP2_DRIVE_CLOSED:
      return "closed";
    case HCP2_DRIVE_PART_OPEN:
      return "part_open";
    default:
      return "unknown";
  }
}

inline HCP2CoverOperation hcp2_cover_operation(hcp2_drive_state_code_t state) {
  switch (state) {
    case HCP2_DRIVE_OPENING:
    case HCP2_DRIVE_HALF_OPENING:
    case HCP2_DRIVE_VENT_MOVING:
      return HCP2CoverOperation::OPENING;
    case HCP2_DRIVE_CLOSING:
      return HCP2CoverOperation::CLOSING;
    default:
      return HCP2CoverOperation::IDLE;
  }
}

inline HCP2CoverOperation hcp2_cover_operation_from_delta(hcp2_drive_state_code_t state, float current_position,
                                                          float previous_position) {
  if (state == HCP2_DRIVE_HALF_OPENING || state == HCP2_DRIVE_VENT_MOVING) {
    return previous_position > current_position ? HCP2CoverOperation::CLOSING : HCP2CoverOperation::OPENING;
  }
  return hcp2_cover_operation(state);
}

inline hcp2_button_t hcp2_cover_button_for_control(bool stop, bool has_position, float target_position) {
  if (stop) {
    return HCP2_BUTTON_STOP;
  }
  if (!has_position) {
    return HCP2_BUTTON_NONE;
  }
  if (target_position <= 0.01f) {
    return HCP2_BUTTON_CLOSE;
  }
  if (target_position >= 0.99f) {
    return HCP2_BUTTON_OPEN;
  }
  if (target_position >= 0.45f && target_position <= 0.55f) {
    return HCP2_BUTTON_HALF;
  }
  return HCP2_BUTTON_NONE;
}

inline hcp2_button_t hcp2_direction_button_for_target(bool has_valid_position, float current_position,
                                                       float target_position, float tolerance) {
  if (!has_valid_position) {
    return HCP2_BUTTON_NONE;
  }
  if (std::fabs(current_position - target_position) <= tolerance) {
    return HCP2_BUTTON_NONE;
  }
  return target_position > current_position ? HCP2_BUTTON_OPEN : HCP2_BUTTON_CLOSE;
}

inline bool hcp2_should_stop_at_target(float target_position, float current_position,
                                       HCP2CoverOperation operation, float tolerance) {
  switch (operation) {
    case HCP2CoverOperation::OPENING:
      return current_position + tolerance >= target_position;
    case HCP2CoverOperation::CLOSING:
      return current_position <= target_position + tolerance;
    case HCP2CoverOperation::IDLE:
      return std::fabs(current_position - target_position) <= tolerance;
  }
  return false;
}

inline bool hcp2_is_uncommanded_closing_reversal(hcp2_drive_state_code_t previous_state,
                                                 hcp2_drive_state_code_t current_state,
                                                 hcp2_button_t recent_command) {
  if (!hcp2_state_code_is_closing(previous_state) || !hcp2_state_code_is_opening(current_state)) {
    return false;
  }
  return recent_command != HCP2_BUTTON_OPEN && recent_command != HCP2_BUTTON_HALF;
}

}  // namespace hcp2bridge
}  // namespace esphome
