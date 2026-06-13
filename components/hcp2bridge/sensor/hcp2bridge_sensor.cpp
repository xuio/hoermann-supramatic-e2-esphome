#include "hcp2bridge_sensor.h"

#include "esphome/core/log.h"

namespace esphome {
namespace hcp2bridge {

static const char *const TAG = "hcp2bridge.sensor";

void HCP2BridgeDiagnosticSensorBase::update() {
  if (this->parent_ == nullptr) {
    return;
  }
  this->publish_state(static_cast<float>(this->read_value_()));
}

void HCP2BridgeDiagnosticSensorBase::dump_config() {
  ESP_LOGCONFIG(TAG, "HCP2 Bridge %s Sensor", this->sensor_name_());
}

}  // namespace hcp2bridge
}  // namespace esphome
