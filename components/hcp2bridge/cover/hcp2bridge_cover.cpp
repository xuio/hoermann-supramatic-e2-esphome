#include "hcp2bridge_cover.h"

#include "../hcp2_entity_mapping.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace hcp2bridge {

static const char *const TAG = "hcp2bridge.cover";
static constexpr float HCP2_GOTO_TOLERANCE = 0.02f;
static constexpr uint32_t HCP2_GOTO_TIMEOUT_MS = 45000;
static constexpr uint32_t HCP2_GOTO_COMMAND_INTERVAL_MS = 1000;

void HCP2BridgeCover::setup() {
  this->parent_->add_on_state_callback([this]() { this->on_state_(); });
  this->on_state_();
}

void HCP2BridgeCover::loop() { this->service_goto_(); }

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
  const float target = has_position ? std::min(1.0f, std::max(0.0f, *call.get_position())) : 0.0f;
  switch (hcp2_cover_button_for_control(call.get_stop(), has_position, target)) {
    case HCP2_BUTTON_STOP:
      this->goto_active_ = false;
      this->parent_->disarm_stop_trigger();
      this->parent_->action_stop();
      break;
    case HCP2_BUTTON_CLOSE:
      this->goto_active_ = false;
      this->parent_->disarm_stop_trigger();
      this->parent_->action_close();
      break;
    case HCP2_BUTTON_OPEN:
      this->goto_active_ = false;
      this->parent_->disarm_stop_trigger();
      this->parent_->action_open();
      break;
    case HCP2_BUTTON_HALF:
      this->goto_active_ = false;
      this->parent_->disarm_stop_trigger();
      this->parent_->action_half();
      break;
    default:
      if (has_position) {
        this->start_goto_(target);
      }
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

bool HCP2BridgeCover::send_direction_(hcp2_button_t button) {
  if (button == HCP2_BUTTON_OPEN) {
    return this->parent_->action_open();
  }
  if (button == HCP2_BUTTON_CLOSE) {
    return this->parent_->action_close();
  }
  return false;
}

void HCP2BridgeCover::start_goto_(float target) {
  if (!this->parent_->has_valid_broadcast()) {
    ESP_LOGW(TAG, "Cannot start HCP2 goto before a valid broadcast");
    return;
  }

  const float current = this->parent_->get_position();
  const hcp2_button_t button = hcp2_direction_button_for_target(true, current, target, HCP2_GOTO_TOLERANCE);
  if (button == HCP2_BUTTON_NONE) {
    this->goto_active_ = false;
    return;
  }

  const uint32_t now = millis();
  if (!this->send_direction_(button)) {
    ESP_LOGW(TAG, "Failed to queue HCP2 goto command");
    return;
  }
  if (!this->parent_->arm_stop_trigger(target, HCP2_GOTO_TIMEOUT_MS)) {
    ESP_LOGW(TAG, "Failed to arm LP stop trigger for HCP2 goto");
  }
  this->goto_active_ = true;
  this->goto_target_ = target;
  this->goto_started_ms_ = now;
  this->last_goto_command_ms_ = now;
}

void HCP2BridgeCover::service_goto_() {
  if (!this->goto_active_) {
    return;
  }
  const uint32_t now = millis();
  if ((int32_t) (now - (this->goto_started_ms_ + HCP2_GOTO_TIMEOUT_MS)) >= 0) {
    ESP_LOGW(TAG, "HCP2 goto timed out");
    this->goto_active_ = false;
    this->parent_->action_stop();
    return;
  }
  if (!this->parent_->has_valid_broadcast()) {
    return;
  }

  const float current = this->parent_->get_position();
  const HCP2CoverOperation operation = hcp2_cover_operation(this->parent_->get_drive_state());
  if (hcp2_should_stop_at_target(this->goto_target_, current, operation, HCP2_GOTO_TOLERANCE)) {
    this->goto_active_ = false;
    if (operation != HCP2CoverOperation::IDLE) {
      this->parent_->action_stop();
    }
    return;
  }

  const hcp2_button_t desired = hcp2_direction_button_for_target(true, current, this->goto_target_,
                                                                 HCP2_GOTO_TOLERANCE);
  if (desired == HCP2_BUTTON_NONE) {
    this->goto_active_ = false;
    return;
  }

  const HCP2CoverOperation desired_operation =
      desired == HCP2_BUTTON_OPEN ? HCP2CoverOperation::OPENING : HCP2CoverOperation::CLOSING;
  if (operation != HCP2CoverOperation::IDLE && operation != desired_operation) {
    this->last_goto_command_ms_ = now;
    this->parent_->action_stop();
    return;
  }
  if (operation == HCP2CoverOperation::IDLE &&
      (int32_t) (now - (this->last_goto_command_ms_ + HCP2_GOTO_COMMAND_INTERVAL_MS)) >= 0) {
    if (this->send_direction_(desired)) {
      this->last_goto_command_ms_ = now;
    }
  }
}

}  // namespace hcp2bridge
}  // namespace esphome
