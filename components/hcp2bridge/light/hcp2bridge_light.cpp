#include "hcp2bridge_light.h"

#include "esphome/core/log.h"

namespace esphome {
namespace hcp2bridge {

static const char *const TAG = "hcp2bridge.light";

light::LightTraits HCP2BridgeLight::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({light::ColorMode::ON_OFF});
  return traits;
}

void HCP2BridgeLight::setup() {
  this->parent_->add_on_state_callback([this]() { this->on_state_(); });
  this->on_state_();
}

void HCP2BridgeLight::dump_config() { ESP_LOGCONFIG(TAG, "HCP2 Bridge Light"); }

void HCP2BridgeLight::write_state(light::LightState *state) {
  bool requested_on = false;
  state->current_values_as_binary(&requested_on);
  if (requested_on != this->parent_->is_light_on()) {
    this->parent_->action_light();
  }
}

void HCP2BridgeLight::on_state_() {
  if (this->state_ == nullptr || !this->parent_->has_valid_broadcast()) {
    return;
  }
  if (this->state_->current_values.is_on() == this->parent_->is_light_on()) {
    return;
  }
  if (this->parent_->is_light_on()) {
    this->state_->turn_on().perform();
  } else {
    this->state_->turn_off().perform();
  }
}

}  // namespace hcp2bridge
}  // namespace esphome
