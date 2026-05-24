#include "uapbridge_cover.h"
namespace esphome {
namespace uapbridge {

static const char* const TAG = "uapbridge.cover";

void UAPBridgeCover::setup() {
  this->parent_->add_on_state_callback([this]() { this->on_event_triggered(); });
}

cover::CoverTraits UAPBridgeCover::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_is_assumed_state(true);
  traits.set_supports_position(false);
  traits.set_supports_stop(true);
  traits.set_supports_tilt(false);
  traits.set_supports_toggle(false);
  return traits;
}

void UAPBridgeCover::control(const cover::CoverCall& call) {
  if (call.get_position().has_value()) {
    float position = *call.get_position();
    if (position == 0) {
      // Close the cover
      parent_->action_close();
      ESP_LOGI(TAG, "Closing the cover");
    } else if (position == 1) {
      // Open the cover
      parent_->action_open();
      ESP_LOGI(TAG, "Opening the cover");
    }
  }
  if (call.get_stop()) {
    // Stop the cover
    parent_->action_stop();
    ESP_LOGI(TAG, "Stopping the cover");
  }
  if (call.get_toggle()) {
    ESP_LOGW(TAG, "Ignoring cover toggle; use the explicit impulse button if needed");
  }
}

void UAPBridgeCover::on_event_triggered() {
  switch (this->parent_->get_state()) {
    case UAPBridge::hoermann_state_t::hoermann_state_opening:
      this->current_operation = cover::COVER_OPERATION_OPENING;
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_open:
      this->current_operation = cover::COVER_OPERATION_IDLE;
      this->position = 1;
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_closed:
      this->current_operation = cover::COVER_OPERATION_IDLE;
      this->position = 0;
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_closing:
      this->current_operation = cover::COVER_OPERATION_CLOSING;
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_error:
    case UAPBridge::hoermann_state_t::hoermann_state_venting:
      this->current_operation = cover::COVER_OPERATION_IDLE;
      this->position = 0.2;
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_stopped:
      this->current_operation = cover::COVER_OPERATION_IDLE;
      this->position = 0.5;
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_unknown:
    default:
      this->current_operation = cover::COVER_OPERATION_IDLE;
      this->position = 1.0;
      break;
  }
  if (this->previousState_ != this->parent_->get_state() || this->previousOperation_ != this->current_operation) {
    ESP_LOGV(TAG, "UAPBridgeCover::update() - position is %f", this->position);
    ESP_LOGV(TAG, "UAPBridgeCover::update() - operation is %d", this->current_operation);
    ESP_LOGD(TAG, "UAPBridgeCover::update() - state changed");
    this->publish_state();
    this->previousState_ = this->parent_->get_state();
    this->previousOperation_ = this->current_operation;
  }
}
}  // namespace uapbridge
}  // namespace esphome
