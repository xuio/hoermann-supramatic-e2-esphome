#pragma once

#include <cstdint>

namespace esphome {
namespace hcp2bridge {

enum class HCP2BackendKind : uint8_t {
  ESP32C6_LP = 0,
  HP_FALLBACK = 1,
  ESP32_REALTIME = 2,
  ESP32C6_HP_REALTIME = 3,
  ESP32C6_HP_ASM_DMA = 4,
};

enum class HCP2RS485Mode : uint8_t {
  DE_RE = 0,
  AUTO_DIRECTION = 1,
};

enum class HCP2RealtimeBoardProfile : uint8_t {
  ESP32_WROOM_NO_PSRAM = 0,
};

enum class HCP2RestartPolicy : uint8_t {
  NO_AUTO_RESTART = 0,
  AUTO_RESTART = 1,
};

inline const char *hcp2_backend_name(HCP2BackendKind kind) {
  switch (kind) {
    case HCP2BackendKind::ESP32C6_LP:
      return "esp32c6_lp";
    case HCP2BackendKind::HP_FALLBACK:
      return "hp_fallback";
    case HCP2BackendKind::ESP32_REALTIME:
      return "esp32_realtime";
    case HCP2BackendKind::ESP32C6_HP_REALTIME:
      return "esp32c6_hp_realtime";
    case HCP2BackendKind::ESP32C6_HP_ASM_DMA:
      return "esp32c6_hp_asm_dma";
    default:
      return "unknown";
  }
}

inline const char *hcp2_rs485_mode_name(HCP2RS485Mode mode) {
  switch (mode) {
    case HCP2RS485Mode::DE_RE:
      return "de_re";
    case HCP2RS485Mode::AUTO_DIRECTION:
      return "auto_direction";
    default:
      return "unknown";
  }
}

inline const char *hcp2_realtime_board_profile_name(HCP2RealtimeBoardProfile profile) {
  switch (profile) {
    case HCP2RealtimeBoardProfile::ESP32_WROOM_NO_PSRAM:
      return "esp32_wroom_no_psram";
    default:
      return "unknown";
  }
}

inline const char *hcp2_restart_policy_name(HCP2RestartPolicy policy) {
  switch (policy) {
    case HCP2RestartPolicy::NO_AUTO_RESTART:
      return "no_auto_restart";
    case HCP2RestartPolicy::AUTO_RESTART:
      return "auto_restart";
    default:
      return "unknown";
  }
}

inline bool hcp2_backend_uses_mailbox(HCP2BackendKind kind) {
  return kind == HCP2BackendKind::ESP32C6_LP || kind == HCP2BackendKind::ESP32_REALTIME ||
         kind == HCP2BackendKind::ESP32C6_HP_REALTIME;
}

inline bool hcp2_backend_survives_hp_restart(HCP2BackendKind kind) {
  return kind == HCP2BackendKind::ESP32C6_LP;
}

inline bool hcp2_backend_supports_stop_trigger(HCP2BackendKind kind) {
  return kind == HCP2BackendKind::ESP32C6_LP || kind == HCP2BackendKind::HP_FALLBACK ||
         kind == HCP2BackendKind::ESP32_REALTIME || kind == HCP2BackendKind::ESP32C6_HP_REALTIME;
}

}  // namespace hcp2bridge
}  // namespace esphome
