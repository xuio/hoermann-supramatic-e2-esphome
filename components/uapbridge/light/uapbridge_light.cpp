#include "esphome/core/log.h"
#include "uapbridge_light.h"

namespace esphome {
namespace uapbridge {

static const char* TAG = "uapbridge.light";

light::LightTraits UAPBridgeLight::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({light::ColorMode::ON_OFF});
  return traits;
}

void UAPBridgeLight::setup() {
  ESP_LOGD(TAG, "UAPBridgeLight::setup() - setup method called");
  this->parent_->add_on_state_callback([this]() { this->on_event_triggered(); });
  this->parent_->add_on_movement_command_callback([this]() { this->handle_auto_courtesy_light_("movement command"); });
}

void UAPBridgeLight::write_state(light::LightState* state) {
  this->estimated_courtesy_light_active_ = false;
  this->estimated_courtesy_light_started_ms_ = 0;

  bool binary;
  state->current_values_as_binary(&binary);
  if (binary) {
    this->output_->turn_on();
  } else {
    this->output_->turn_off();
  }
}

void UAPBridgeLight::loop() {
  if (this->parent_ == nullptr || this->parent_->get_trust_light_feedback() || !this->estimated_courtesy_light_active_) {
    return;
  }

  if (millis() - this->estimated_courtesy_light_started_ms_ >= this->courtesy_light_duration_ms_) {
    this->estimated_courtesy_light_active_ = false;
    this->estimated_courtesy_light_started_ms_ = 0;
    this->publish_estimated_state_(false, "estimated courtesy light timeout");
  }
}

void UAPBridgeLight::on_event_triggered() {
  if (!this->parent_->get_trust_light_feedback()) {
    if (this->auto_courtesy_light_ && this->is_motion_state_(this->parent_->get_state())) {
      this->handle_auto_courtesy_light_("door motion");
    } else {
      ESP_LOGVV(TAG, "UAPBridgeLight::on_event_triggered() - ignoring untrusted light feedback");
    }
    return;
  }

  if (this->state_->current_values.is_on() != this->parent_->get_light_enabled()) {
    // Adjust the state of the light based on the external light state
    ESP_LOGD(TAG, "UAPBridgeLight::update() - adjusting state");
    if (this->parent_->get_light_enabled()) {
      this->state_->turn_on().perform();
    } else {
      this->state_->turn_off().perform();
    }
  }
}

void UAPBridgeLight::dump_config() {
  ESP_LOGCONFIG(TAG, "UAPBridgeLight:");
  ESP_LOGCONFIG(TAG, "  Auto Courtesy Light: %s", this->auto_courtesy_light_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Courtesy Light Duration: %ums", (unsigned int) this->courtesy_light_duration_ms_);
}

bool UAPBridgeLight::is_motion_state_(UAPBridge::hoermann_state_t state) const {
  return state == UAPBridge::hoermann_state_t::hoermann_state_opening ||
         state == UAPBridge::hoermann_state_t::hoermann_state_closing;
}

void UAPBridgeLight::handle_auto_courtesy_light_(const char *reason) {
  if (!this->auto_courtesy_light_ || this->parent_ == nullptr || this->parent_->get_trust_light_feedback()) {
    return;
  }

  const bool already_on = this->state_ != nullptr && this->state_->remote_values.is_on();
  if (!already_on) {
    this->estimated_courtesy_light_active_ = true;
    this->estimated_courtesy_light_started_ms_ = millis();
    this->publish_estimated_state_(true, reason);
  }
}

void UAPBridgeLight::publish_estimated_state_(bool on, const char *reason) {
  if (this->state_ == nullptr) {
    return;
  }

  if (this->state_->remote_values.is_on() == on && this->state_->current_values.is_on() == on) {
    return;
  }

  auto values = this->state_->remote_values;
  values.set_color_mode(light::ColorMode::ON_OFF);
  values.set_state(on);
  values.set_brightness(1.0f);
  this->state_->remote_values = values;
  this->state_->current_values = values;
  ESP_LOGI(TAG, "Publishing estimated courtesy light %s from %s", on ? "on" : "off", reason);
  this->state_->publish_state();
}
}  // namespace uapbridge
}  // namespace esphome
