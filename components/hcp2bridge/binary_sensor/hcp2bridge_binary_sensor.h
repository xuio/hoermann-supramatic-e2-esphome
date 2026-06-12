#pragma once

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/core/component.h"
#include "../hcp2bridge.h"

namespace esphome {
namespace hcp2bridge {

class HCP2BridgeBinarySensorBase : public binary_sensor::BinarySensor, public Component {
 public:
  void set_hcp2bridge_parent(HCP2Bridge *parent) { this->parent_ = parent; }
  void setup() override;
  void dump_config() override;
  void on_state_();

 protected:
  virtual bool read_state_() const = 0;
  virtual const char *sensor_name_() const = 0;
  HCP2Bridge *parent_{nullptr};
};

class HCP2BridgeValidBroadcastSensor : public HCP2BridgeBinarySensorBase {
 protected:
  bool read_state_() const override { return this->parent_->has_valid_broadcast(); }
  const char *sensor_name_() const override { return "Valid Broadcast"; }
};

class HCP2BridgeMovingSensor : public HCP2BridgeBinarySensorBase {
 protected:
  bool read_state_() const override { return this->parent_->is_moving(); }
  const char *sensor_name_() const override { return "Moving"; }
};

class HCP2BridgeOpenSensor : public HCP2BridgeBinarySensorBase {
 protected:
  bool read_state_() const override { return this->parent_->is_open(); }
  const char *sensor_name_() const override { return "Open"; }
};

class HCP2BridgeClosedSensor : public HCP2BridgeBinarySensorBase {
 protected:
  bool read_state_() const override { return this->parent_->is_closed(); }
  const char *sensor_name_() const override { return "Closed"; }
};

class HCP2BridgeLightSensor : public HCP2BridgeBinarySensorBase {
 protected:
  bool read_state_() const override { return this->parent_->is_light_on(); }
  const char *sensor_name_() const override { return "Light"; }
};

class HCP2BridgeObstructionSensor : public HCP2BridgeBinarySensorBase {
 protected:
  bool read_state_() const override { return this->parent_->is_obstructed(); }
  const char *sensor_name_() const override { return "Obstruction"; }
};

}  // namespace hcp2bridge
}  // namespace esphome
