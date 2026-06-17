#include "hcp2bridge.h"
#include "hcp2bridge_internal.h"

#ifdef USE_ESP32

#include <cstring>

#include "esp_timer.h"

namespace esphome {
namespace hcp2bridge {

static const char *const TAG = "hcp2bridge";

#ifdef USE_ESP32_VARIANT_ESP32
static constexpr uint32_t HCP2BRIDGE_REALTIME_TASK_TICK_MS = 20;

bool HCP2Bridge::setup_esp32_realtime_() {
  const hcp2_port_t port = {
      .user = this,
      .now_us = HCP2Bridge::now_us_cb_,
      .tx = nullptr,
      .de_set = nullptr,
  };

  hcp2_engine_init(&this->engine_, &port, &this->config_);
  hcp2_lp_mailbox_init(&this->esp32_realtime_mailbox_);
  memset(&this->esp32_realtime_counters_, 0, sizeof(this->esp32_realtime_counters_));
  hcp2_responder_runtime_init(&this->esp32_realtime_runtime_, &this->engine_, &this->esp32_realtime_mailbox_);

  hcp2_hp_supervisor_init(&this->lp_supervisor_, &this->esp32_realtime_mailbox_, HCP2_LP_FIRMWARE_VERSION);
  const uint32_t epoch = (uint32_t) esp_timer_get_time() ^ 0x45533248u;
  hcp2_hp_supervisor_begin_session(&this->lp_supervisor_, epoch != 0u ? epoch : 1u);
  hcp2_responder_runtime_begin_from_mailbox(&this->esp32_realtime_runtime_, HCP2Bridge::now_us_cb_(this));
  hcp2_responder_runtime_trace(&this->esp32_realtime_runtime_, HCP2_LP_TRACE_BOOT, 0u,
                               HCP2Bridge::now_us_cb_(this));

  this->backend_ready_ = true;
  this->protocol_log_append_control_("esp32_realtime_phase_d_ready");
  ESP_LOGW(TAG, "ESP32 realtime backend Phase D is mailbox/debug only; UART/ISR TX is not active");
  return true;
}

void HCP2Bridge::start_esp32_realtime_task_() {
  if (this->esp32_realtime_task_handle_ != nullptr) {
    return;
  }
  const BaseType_t ok = xTaskCreatePinnedToCore(HCP2Bridge::esp32_realtime_task_trampoline_, "hcp2_rt_proto",
                                                HCP2BRIDGE_REALTIME_TASK_STACK_BYTES, this, 5,
                                                &this->esp32_realtime_task_handle_, tskNO_AFFINITY);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to start HCP2 ESP32 realtime prototype task");
    this->esp32_realtime_task_handle_ = nullptr;
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "Started HCP2 ESP32 realtime prototype task stack=%u bytes",
           (unsigned int) HCP2BRIDGE_REALTIME_TASK_STACK_BYTES);
}

void HCP2Bridge::esp32_realtime_task_trampoline_(void *arg) {
  auto *self = static_cast<HCP2Bridge *>(arg);
  if (self == nullptr) {
    vTaskDelete(nullptr);
    return;
  }
  self->esp32_realtime_task_loop_();
}

void HCP2Bridge::esp32_realtime_task_loop_() {
  for (;;) {
    const uint32_t loop_start_us = HCP2Bridge::now_us_cb_(this);

    this->esp32_realtime_mailbox_.heartbeat++;
    hcp2_responder_runtime_handle_mailbox_command(&this->esp32_realtime_runtime_, loop_start_us);
    hcp2_responder_runtime_handle_stop_trigger(&this->esp32_realtime_runtime_, loop_start_us);
    hcp2_responder_runtime_note_status_poll(&this->esp32_realtime_runtime_, loop_start_us);
    hcp2_responder_runtime_publish_state(&this->esp32_realtime_runtime_, loop_start_us);
    hcp2_responder_runtime_publish_counters(&this->esp32_realtime_runtime_, &this->esp32_realtime_counters_,
                                            loop_start_us);

    while (hcp2_responder_runtime_publish_protocol_event(&this->esp32_realtime_runtime_) != 0u) {
    }

    this->update_state_from_mailbox_();
    this->drain_lp_protocol_event_();
    this->drain_lp_trace_();

    const uint32_t loop_end_us = HCP2Bridge::now_us_cb_(this);
    const uint32_t loop_us = loop_end_us - loop_start_us;
    if (loop_us > this->esp32_realtime_counters_.max_loop_us) {
      this->esp32_realtime_counters_.max_loop_us = loop_us;
    }
    vTaskDelay(pdMS_TO_TICKS(HCP2BRIDGE_REALTIME_TASK_TICK_MS));
  }
}
#else
bool HCP2Bridge::setup_esp32_realtime_() {
  ESP_LOGE(TAG, "ESP32 realtime backend requires a classic ESP32 build");
  this->backend_ready_ = false;
  return false;
}

void HCP2Bridge::start_esp32_realtime_task_() {}

void HCP2Bridge::esp32_realtime_task_trampoline_(void *arg) {
  (void) arg;
  vTaskDelete(nullptr);
}

void HCP2Bridge::esp32_realtime_task_loop_() { vTaskDelete(nullptr); }
#endif

}  // namespace hcp2bridge
}  // namespace esphome

#endif
