#pragma once

#include "esphome/components/button/button.h"
#include "esphome/core/component.h"
#include "../hcp2bridge.h"

namespace esphome {
namespace hcp2bridge {

class HCP2BridgeButtonBase : public button::Button, public Component {
 public:
  void set_hcp2bridge_parent(HCP2Bridge *parent) { this->parent_ = parent; }
  void press_action() override;

 protected:
  virtual bool press_() = 0;
  virtual const char *button_name_() const = 0;
  HCP2Bridge *parent_{nullptr};
};

class HCP2BridgeOpenButton : public HCP2BridgeButtonBase {
 protected:
  bool press_() override { return this->parent_->action_open(); }
  const char *button_name_() const override { return "open"; }
};

class HCP2BridgeCloseButton : public HCP2BridgeButtonBase {
 protected:
  bool press_() override { return this->parent_->action_close(); }
  const char *button_name_() const override { return "close"; }
};

class HCP2BridgeStopButton : public HCP2BridgeButtonBase {
 protected:
  bool press_() override { return this->parent_->action_stop(); }
  const char *button_name_() const override { return "stop"; }
};

class HCP2BridgeHalfButton : public HCP2BridgeButtonBase {
 protected:
  bool press_() override { return this->parent_->action_half(); }
  const char *button_name_() const override { return "half"; }
};

class HCP2BridgeVentButton : public HCP2BridgeButtonBase {
 protected:
  bool press_() override { return this->parent_->action_vent(); }
  const char *button_name_() const override { return "vent"; }
};

class HCP2BridgeLightButton : public HCP2BridgeButtonBase {
 protected:
  bool press_() override { return this->parent_->action_light(); }
  const char *button_name_() const override { return "light"; }
};

}  // namespace hcp2bridge
}  // namespace esphome
