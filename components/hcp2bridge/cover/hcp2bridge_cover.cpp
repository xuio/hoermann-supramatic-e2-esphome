#include "hcp2bridge_cover.h"

#include "../hcp2_entity_mapping.h"
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
  const bool has_position = call.get_position().has_value();
  const float target = has_position ? *call.get_position() : 0.0f;
  switch (hcp2_cover_button_for_control(call.get_stop(), has_position, target)) {
    case HCP2_BUTTON_STOP:
      this->parent_->action_stop();
      break;
    case HCP2_BUTTON_CLOSE:
      this->parent_->action_close();
      break;
    case HCP2_BUTTON_OPEN:
      this->parent_->action_open();
      break;
    case HCP2_BUTTON_HALF:
      this->parent_->action_half();
      break;
    default:
      break;
  }
}

void HCP2BridgeCover::on_state_() {
  if (!this->parent_->has_valid_broadcast()) {
    return;
  }

  this->position = this->parent_->get_position();
  switch (hcp2_cover_operation(this->parent_->get_drive_state())) {
    case HCP2CoverOperation::OPENING:
      this->current_operation = cover::COVER_OPERATION_OPENING;
      break;
    case HCP2CoverOperation::CLOSING:
      this->current_operation = cover::COVER_OPERATION_CLOSING;
      break;
    case HCP2CoverOperation::IDLE:
      this->current_operation = cover::COVER_OPERATION_IDLE;
      break;
  }
  this->publish_state();
}

}  // namespace hcp2bridge
}  // namespace esphome
