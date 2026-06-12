#pragma once

#include "esphome/components/cover/cover.h"
#include "esphome/core/component.h"
#include "../hcp2bridge.h"

namespace esphome {
namespace hcp2bridge {

class HCP2BridgeCover : public cover::Cover, public Component {
 public:
  void set_hcp2bridge_parent(HCP2Bridge *parent) { this->parent_ = parent; }
  void setup() override;
  void loop() override;
  void dump_config() override;
  cover::CoverTraits get_traits() override;
  void control(const cover::CoverCall &call) override;
  void on_state_();

 protected:
  void start_goto_(float target);
  void service_goto_();
  bool send_direction_(hcp2_button_t button);

  HCP2Bridge *parent_{nullptr};
  bool goto_active_{false};
  float goto_target_{0.0f};
  uint32_t goto_started_ms_{0};
  uint32_t last_goto_command_ms_{0};
};

}  // namespace hcp2bridge
}  // namespace esphome
