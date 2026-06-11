#include "hcp2bridge_cover.h"

#include "esphome/core/log.h"

namespace esphome {
namespace hcp2bridge {

static const char *const TAG = "hcp2bridge.cover";

void HCP2BridgeCover::setup() {
  this->parent_->add_on_state_callback([this]() { this->on_state_(); });
  this->on_state_();
}

void HCP2BridgeCover::dump_config() { LOG_COVER("", "HCP2 Bridge Cover", this); }

cover::CoverTraits HCP2BridgeCover::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_supports_position(true);
  traits.set_supports_stop(true);
  traits.set_supports_tilt(false);
  traits.set_supports_toggle(false);
  traits.set_is_assumed_state(false);
  return traits;
}

void HCP2BridgeCover::control(const cover::CoverCall &call) {
  if (call.get_stop()) {
    this->parent_->action_stop();
    return;
  }

  if (call.get_position().has_value()) {
    const float target = *call.get_position();
    if (target <= 0.01f) {
      this->parent_->action_close();
    } else if (target >= 0.99f) {
      this->parent_->action_open();
    } else {
      this->parent_->action_half();
    }
  }
}

void HCP2BridgeCover::on_state_() {
  if (!this->parent_->has_valid_broadcast()) {
    return;
  }

  this->position = this->parent_->get_position();
  switch (this->parent_->get_drive_state()) {
    case HCP2_DRIVE_OPENING:
    case HCP2_DRIVE_HALF_OPENING:
      this->current_operation = cover::COVER_OPERATION_OPENING;
      break;
    case HCP2_DRIVE_CLOSING:
      this->current_operation = cover::COVER_OPERATION_CLOSING;
      break;
    default:
      this->current_operation = cover::COVER_OPERATION_IDLE;
      break;
  }
  this->publish_state();
}

}  // namespace hcp2bridge
}  // namespace esphome
