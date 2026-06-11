#pragma once

#include "esphome/components/light/light_output.h"
#include "esphome/core/component.h"
#include "../hcp2bridge.h"

namespace esphome {
namespace hcp2bridge {

class HCP2BridgeLight : public light::LightOutput, public Component {
 public:
  void set_hcp2bridge_parent(HCP2Bridge *parent) { this->parent_ = parent; }
  light::LightTraits get_traits() override;
  void setup_state(light::LightState *state) override { this->state_ = state; }
  void setup() override;
  void dump_config() override;
  void write_state(light::LightState *state) override;
  void on_state_();

 protected:
  HCP2Bridge *parent_{nullptr};
  light::LightState *state_{nullptr};
};

}  // namespace hcp2bridge
}  // namespace esphome
