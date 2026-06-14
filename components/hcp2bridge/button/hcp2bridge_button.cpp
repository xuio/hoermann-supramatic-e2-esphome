#include "hcp2bridge_button.h"

#include "esphome/core/log.h"

namespace esphome {
namespace hcp2bridge {

static const char *const TAG = "hcp2bridge.button";

void HCP2BridgeButtonBase::press_action() {
  if (this->parent_ == nullptr) {
    ESP_LOGW(TAG, "Cannot send HCP2 %s command before parent is attached", this->button_name_());
    return;
  }
  if (!this->press_()) {
    ESP_LOGW(TAG, "Failed to queue HCP2 %s command", this->button_name_());
  }
}

}  // namespace hcp2bridge
}  // namespace esphome
