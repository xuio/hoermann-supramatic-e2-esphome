#pragma once

#include <algorithm>

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
         status.state == HCP2_DRIVE_HALF_OPENING;
}

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
      return HCP2CoverOperation::OPENING;
    case HCP2_DRIVE_CLOSING:
      return HCP2CoverOperation::CLOSING;
    default:
      return HCP2CoverOperation::IDLE;
  }
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
  return HCP2_BUTTON_HALF;
}

}  // namespace hcp2bridge
}  // namespace esphome
