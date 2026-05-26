#pragma once

#include "esphome/core/component.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/output/binary_output.h"
#include "../uapbridge.h"

namespace esphome {
namespace uapbridge {

class UAPBridgeLight : public light::LightOutput, public Component {
  public:
    void set_uapbridge_parent(UAPBridge *parent) { this->parent_ = parent; }
    light::LightTraits get_traits() override;
    void set_output(output::BinaryOutput *output) { output_ = output; }
    void write_state(light::LightState *state) override;
    void setup() override;
    void loop() override;
    void on_event_triggered();
    void dump_config() override;
    void setup_state(light::LightState *state) { state_ = state; }
    void set_auto_courtesy_light(bool value) { this->auto_courtesy_light_ = value; }
    void set_courtesy_light_duration(uint32_t value) { this->courtesy_light_duration_ms_ = value; }

  protected:
    UAPBridge *parent_{nullptr};
    output::BinaryOutput *output_{nullptr};
    light::LightState *state_{nullptr};
    bool auto_courtesy_light_{false};
    uint32_t courtesy_light_duration_ms_{120000};
    bool estimated_courtesy_light_active_{false};
    uint32_t estimated_courtesy_light_started_ms_{0};
    bool is_motion_state_(UAPBridge::hoermann_state_t state) const;
    void handle_auto_courtesy_light_(const char *reason);
    void publish_estimated_state_(bool on, const char *reason);
};

}  // namespace uapbridge
}  // namespace esphome
