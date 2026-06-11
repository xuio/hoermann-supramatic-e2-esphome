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
  void dump_config() override;
  cover::CoverTraits get_traits() override;
  void control(const cover::CoverCall &call) override;
  void on_state_();

 protected:
  HCP2Bridge *parent_{nullptr};
};

}  // namespace hcp2bridge
}  // namespace esphome
