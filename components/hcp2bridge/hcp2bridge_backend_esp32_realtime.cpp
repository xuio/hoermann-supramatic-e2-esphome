#include "hcp2bridge.h"

#ifdef USE_ESP32

namespace esphome {
namespace hcp2bridge {

static const char *const TAG = "hcp2bridge";

bool HCP2Bridge::setup_esp32_realtime_() {
  ESP_LOGE(TAG, "ESP32 realtime backend is an inert compile-only stub");
  ESP_LOGE(TAG, "No UART, timer, ISR, RS-485 pin, or protocol responder is started");
  this->protocol_log_append_control_("esp32_realtime_unavailable");
  this->backend_ready_ = false;
  return false;
}

}  // namespace hcp2bridge
}  // namespace esphome

#endif
