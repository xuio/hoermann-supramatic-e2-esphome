#pragma once

#include <cstdint>

namespace esphome {
namespace hcp2bridge {

enum class HCP2BackendKind : uint8_t {
  ESP32C6_LP = 0,
  HP_FALLBACK = 1,
  ESP32_REALTIME = 2,
};

inline const char *hcp2_backend_name(HCP2BackendKind kind) {
  switch (kind) {
    case HCP2BackendKind::ESP32C6_LP:
      return "esp32c6_lp";
    case HCP2BackendKind::HP_FALLBACK:
      return "hp_fallback";
    case HCP2BackendKind::ESP32_REALTIME:
      return "esp32_realtime";
    default:
      return "unknown";
  }
}

inline bool hcp2_backend_uses_mailbox(HCP2BackendKind kind) {
  return kind == HCP2BackendKind::ESP32C6_LP;
}

inline bool hcp2_backend_survives_hp_restart(HCP2BackendKind kind) {
  return kind == HCP2BackendKind::ESP32C6_LP;
}

inline bool hcp2_backend_supports_stop_trigger(HCP2BackendKind kind) {
  return kind == HCP2BackendKind::ESP32C6_LP || kind == HCP2BackendKind::HP_FALLBACK;
}

}  // namespace hcp2bridge
}  // namespace esphome
