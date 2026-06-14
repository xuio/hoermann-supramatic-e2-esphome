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
  void service_pending_direction_();
  bool send_direction_(hcp2_button_t button);
  bool request_direction_(hcp2_button_t button, bool for_goto);
  void clear_pending_direction_();

  HCP2Bridge *parent_{nullptr};
  bool goto_active_{false};
  float goto_target_{0.0f};
  uint32_t goto_started_ms_{0};
  uint32_t last_goto_command_ms_{0};
  hcp2_button_t pending_direction_{HCP2_BUTTON_NONE};
  bool pending_direction_for_goto_{false};
  uint32_t pending_direction_since_ms_{0};
  float previous_position_{0.0f};
  bool have_previous_position_{false};
};

}  // namespace hcp2bridge
}  // namespace esphome
