#include "hcp2bridge_binary_sensor.h"

#include "esphome/core/log.h"

namespace esphome {
namespace hcp2bridge {

static const char *const TAG = "hcp2bridge.binary_sensor";

void HCP2BridgeBinarySensorBase::setup() {
  this->parent_->add_on_state_callback([this]() { this->on_state_(); });
  this->on_state_();
}

void HCP2BridgeBinarySensorBase::dump_config() {
  ESP_LOGCONFIG(TAG, "HCP2 Bridge %s Sensor", this->sensor_name_());
}

void HCP2BridgeBinarySensorBase::on_state_() {
  const bool value = this->read_state_();
  if (!this->has_state() || this->state != value) {
    this->publish_state(value);
  }
}

}  // namespace hcp2bridge
}  // namespace esphome
