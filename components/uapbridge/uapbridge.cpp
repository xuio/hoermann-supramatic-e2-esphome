#include "uapbridge.h"

namespace esphome {
namespace uapbridge {
static const char *const TAG = "UAPBridge";

void UAPBridge::setup() {
  if (this->rts_pin_ != nullptr) {
    this->rts_pin_->setup();
    this->rts_pin_->pin_mode(gpio::Flags::FLAG_OUTPUT);
    this->rts_pin_->digital_write(false);// LOW(false) = listen, HIGH(true) = transmit
  }
  ESP_LOGCONFIG(TAG, "Garage setup called!");
}

void UAPBridge::dump_config() {
  ESP_LOGCONFIG(TAG, "UAPBridge");
  if (this->rts_pin_ != nullptr) {
    char rts_pin_buf[GPIO_SUMMARY_MAX_LEN];
    this->rts_pin_->dump_summary(rts_pin_buf, sizeof(rts_pin_buf));
    ESP_LOGCONFIG(TAG, "  RTS Pin: %s", rts_pin_buf);
  }
  ESP_LOGCONFIG(TAG, "  Auto Correction: %s", this->auto_correction ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Allow Remote Close: %s", this->allow_remote_close ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Allow Remote Impulse: %s", this->allow_remote_impulse ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Use Unverified Stop Command: %s", this->use_unverified_stop_command ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Require Fresh Broadcast For Commands: %s", this->require_fresh_broadcast_for_commands ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Command Timeout: %ums", (unsigned int) this->command_timeout_ms);
  ESP_LOGCONFIG(TAG, "  Diagnostic Mode: %s", this->diagnostic_mode ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Listen Only: %s", this->listen_only ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Valid Broadcast Timeout: %ums", (unsigned int) this->valid_broadcast_timeout_ms);
  ESP_LOGCONFIG(TAG, "  Trust Light Feedback: %s", this->trust_light_feedback ? "true" : "false");
}

void UAPBridge::add_on_state_callback(std::function<void()> &&callback) {
  this->state_callback_.add(std::move(callback));
}

void UAPBridge::add_on_movement_command_callback(std::function<void()> &&callback) {
  this->movement_command_callback_.add(std::move(callback));
}

}  // namespace uapbridge
}  // namespace esphome
