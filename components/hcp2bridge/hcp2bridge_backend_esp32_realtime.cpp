#include "hcp2bridge.h"
#include "hcp2bridge_internal.h"

#ifdef USE_ESP32

#include <cstring>
#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/rtc_io.h"
#include "esp_intr_alloc.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "hal/uart_ll.h"
#include "soc/soc_caps.h"
#include "soc/uart_periph.h"
#include "soc/uart_struct.h"

#ifdef USE_ESP32_VARIANT_ESP32C6
#include "driver/uhci.h"
#include "ulp_lp_core.h"
#endif

namespace esphome {
namespace hcp2bridge {

static const char *const TAG = "hcp2bridge";

#if defined(USE_ESP32_VARIANT_ESP32) || defined(USE_ESP32_VARIANT_ESP32C6)
static constexpr uint32_t HCP2BRIDGE_REALTIME_TASK_TICK_MS = 20;
static constexpr uint32_t HCP2BRIDGE_REALTIME_C6_TASK_IDLE_MS = 5;
static constexpr uint32_t HCP2BRIDGE_REALTIME_TIMER_HZ = 1000000u;
static constexpr uint32_t HCP2BRIDGE_REALTIME_TAIL_MARGIN_US = 250u;
static constexpr uint32_t HCP2BRIDGE_REALTIME_TX_TIMER_GUARD_US = 700u;
static constexpr uint32_t HCP2BRIDGE_REALTIME_RXFIFO_FULL_THR = 1u;
static constexpr uint32_t HCP2BRIDGE_C6_UHCI_RX_IDLE_BITS = 16u;
static constexpr uint32_t HCP2BRIDGE_C6_UHCI_RX_BUF_SIZE = 64u;
static constexpr uint32_t HCP2BRIDGE_C6_UHCI_RX_BUF_COUNT = 4u;
static constexpr uint32_t HCP2BRIDGE_C6_UHCI_TX_BUF_SIZE = 64u;
static constexpr uint32_t HCP2BRIDGE_C6_UHCI_TX_LEAD_US = 40u;
static constexpr uint32_t HCP2BRIDGE_C6_UHCI_DE_TAIL_US = 60u;
static constexpr uint32_t HCP2BRIDGE_C6_UHCI_TX_TIMER_GUARD_US = 700u;
static constexpr uint32_t HCP2BRIDGE_REALTIME_UART_RX_INTR_MASK =
    UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT | UART_INTR_PARITY_ERR | UART_INTR_FRAM_ERR | UART_INTR_RXFIFO_OVF;
static constexpr uint32_t HCP2BRIDGE_REALTIME_UART_INTR_MASK =
    HCP2BRIDGE_REALTIME_UART_RX_INTR_MASK | UART_INTR_TX_DONE;
static constexpr uint32_t HCP2BRIDGE_REALTIME_BITS_PER_UART_BYTE = 11u;  // 8E1

static uart_dev_t *esp32_realtime_uart_hw_for_port_(uart_port_t uart_num) {
#if defined(SOC_UART_HP_NUM)
  if ((int) uart_num < 0 || uart_num >= SOC_UART_HP_NUM) {
    return nullptr;
  }
#endif
  return UART_LL_GET_HW(uart_num);
}

void IRAM_ATTR HCP2Bridge::esp32_realtime_set_de_from_isr_(HCP2Bridge *self, bool enabled) {
  if (enabled) {
    if (self->esp32_realtime_de_gpio_ != GPIO_NUM_NC) {
      gpio_set_level(self->esp32_realtime_de_gpio_, 1);
    }
    if (self->esp32_realtime_re_gpio_ != GPIO_NUM_NC) {
      gpio_set_level(self->esp32_realtime_re_gpio_, 1);
    }
  } else {
    if (self->esp32_realtime_re_gpio_ != GPIO_NUM_NC) {
      gpio_set_level(self->esp32_realtime_re_gpio_, 0);
    }
    if (self->esp32_realtime_de_gpio_ != GPIO_NUM_NC) {
      gpio_set_level(self->esp32_realtime_de_gpio_, 0);
    }
  }
  self->esp32_realtime_de_enabled_ = enabled;
  self->esp32_realtime_de_enabled_since_us_ = enabled ? HCP2Bridge::esp32_realtime_now_us_cb_(self) : 0u;
}

bool IRAM_ATTR HCP2Bridge::esp32_realtime_schedule_alarm_from_isr_(HCP2Bridge *self, uint32_t alarm_us) {
  if (self == nullptr || self->esp32_realtime_timer_ == nullptr) {
    return false;
  }
  gptimer_alarm_config_t alarm_config = {};
  alarm_config.alarm_count = alarm_us;
  alarm_config.reload_count = 0u;
  alarm_config.flags.auto_reload_on_alarm = 0u;
  return gptimer_set_alarm_action(self->esp32_realtime_timer_, &alarm_config) == ESP_OK;
}

void IRAM_ATTR HCP2Bridge::esp32_realtime_schedule_timer_from_isr_(HCP2Bridge *self) {
  if (self == nullptr || self->esp32_realtime_tx_active_ || !hcp2_engine_pending_tx_ready(&self->engine_)) {
    return;
  }
  const uint32_t due_us = hcp2_engine_pending_tx_due_us(&self->engine_);
  uint32_t now_us = HCP2Bridge::esp32_realtime_now_us_cb_(self);
  const int32_t wait_us = (int32_t) (due_us - now_us);
  if (wait_us <= 0) {
    (void) HCP2Bridge::esp32_realtime_send_due_tx_from_isr_(self, now_us);
    return;
  }
  if (wait_us <= (int32_t) HCP2BRIDGE_REALTIME_TX_TIMER_GUARD_US) {
    while ((int32_t) (now_us - due_us) < 0) {
      now_us = HCP2Bridge::esp32_realtime_now_us_cb_(self);
    }
    if (HCP2Bridge::esp32_realtime_send_due_tx_from_isr_(self, now_us)) {
      return;
    }
  }
  (void) HCP2Bridge::esp32_realtime_schedule_alarm_from_isr_(self, due_us - HCP2BRIDGE_REALTIME_TX_TIMER_GUARD_US);
}

void IRAM_ATTR HCP2Bridge::esp32_realtime_finish_tx_from_isr_(HCP2Bridge *self, uint32_t now_us, bool deadman) {
  if (self == nullptr || !self->esp32_realtime_tx_active_) {
    return;
  }

  uart_ll_disable_intr_mask(self->esp32_realtime_uart_hw_, UART_INTR_TX_DONE);
  HCP2Bridge::esp32_realtime_set_de_from_isr_(self, false);
  self->esp32_realtime_tx_active_ = false;
  self->esp32_realtime_tx_tail_due_us_ = 0u;
  if (deadman) {
    self->esp32_realtime_counters_.stuck_de_count++;
  }
  hcp2_engine_mark_tx_done(&self->engine_, now_us);
  HCP2Bridge::esp32_realtime_schedule_timer_from_isr_(self);
}

bool IRAM_ATTR HCP2Bridge::esp32_realtime_send_due_tx_from_isr_(HCP2Bridge *self, uint32_t now_us) {
  uint8_t tx[HCP2_MAX_FRAME_LEN];
  uint8_t tx_len = 0u;
  hcp2_pending_tx_meta_t meta;

  if (self == nullptr || self->esp32_realtime_uart_hw_ == nullptr || self->esp32_realtime_tx_active_) {
    return false;
  }

  if (!hcp2_engine_claim_due_tx(&self->engine_, now_us, tx, &tx_len, &meta)) {
    return false;
  }
  if (!hcp2_realtime_tx_slot_init(&self->esp32_realtime_tx_slot_, tx, tx_len, meta.frame_type, meta.due_us,
                                  HCP2BRIDGE_REALTIME_TAIL_MARGIN_US)) {
    self->esp32_realtime_counters_.tx_abort_count++;
    hcp2_engine_mark_tx_done(&self->engine_, now_us);
    return true;
  }

  HCP2Bridge::esp32_realtime_set_de_from_isr_(self, true);
  uart_ll_clr_intsts_mask(self->esp32_realtime_uart_hw_, UART_INTR_TX_DONE);
  hcp2_engine_mark_tx_started(&self->engine_, now_us);
  uart_ll_write_txfifo(self->esp32_realtime_uart_hw_, self->esp32_realtime_tx_slot_.data,
                       self->esp32_realtime_tx_slot_.len);
  self->esp32_realtime_tx_active_ = true;
  self->esp32_realtime_tx_tail_due_us_ =
      now_us + (((uint32_t) self->esp32_realtime_tx_slot_.len * HCP2BRIDGE_REALTIME_BITS_PER_UART_BYTE * 1000000u +
                 HCP2BRIDGE_BAUD_RATE - 1u) /
                HCP2BRIDGE_BAUD_RATE) +
      HCP2BRIDGE_REALTIME_TAIL_MARGIN_US;
  uint32_t finish_now = now_us;
  while (!uart_ll_is_tx_idle(self->esp32_realtime_uart_hw_) &&
         (int32_t) (finish_now - self->esp32_realtime_tx_tail_due_us_) < 0) {
    finish_now = HCP2Bridge::esp32_realtime_now_us_cb_(self);
  }
  HCP2Bridge::esp32_realtime_finish_tx_from_isr_(self, finish_now, false);
  return true;
}

#ifdef USE_ESP32_VARIANT_ESP32C6
esp_err_t HCP2Bridge::esp32c6_realtime_uhci_start_rx_(bool record_failure) {
  if (this->esp32c6_realtime_uhci_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  uint32_t index = HCP2BRIDGE_C6_UHCI_RX_BUF_COUNT;
  for (uint32_t i = 0; i < HCP2BRIDGE_C6_UHCI_RX_BUF_COUNT; i++) {
    const uint32_t candidate = this->esp32c6_realtime_uhci_rx_buf_index_++ % HCP2BRIDGE_C6_UHCI_RX_BUF_COUNT;
    if (!this->esp32c6_realtime_uhci_rx_buf_busy_[candidate]) {
      index = candidate;
      break;
    }
  }
  if (index >= HCP2BRIDGE_C6_UHCI_RX_BUF_COUNT) {
    if (record_failure) {
      this->esp32_realtime_counters_.rx_starvation_count++;
      this->esp32_realtime_counters_.health_flags |= HCP2_LP_HEALTH_FLAG_RX_STARVATION;
    }
    return ESP_ERR_NO_MEM;
  }
  this->esp32c6_realtime_uhci_rx_buf_busy_[index] = true;
  const esp_err_t err = uhci_receive(this->esp32c6_realtime_uhci_,
                                     this->esp32c6_realtime_uhci_rx_bufs_[index],
                                     HCP2BRIDGE_C6_UHCI_RX_BUF_SIZE);
  if (err != ESP_OK) {
    this->esp32c6_realtime_uhci_rx_buf_busy_[index] = false;
    if (record_failure) {
      this->esp32_realtime_counters_.rx_starvation_count++;
      this->esp32_realtime_counters_.health_flags |= HCP2_LP_HEALTH_FLAG_RX_STARVATION;
    }
  }
  return err;
}

bool IRAM_ATTR HCP2Bridge::esp32c6_realtime_uhci_rx_cb_(uhci_controller_handle_t uhci,
                                                        const uhci_rx_event_data_t *edata, void *user_ctx) {
  (void) uhci;
  auto *self = static_cast<HCP2Bridge *>(user_ctx);
  if (self == nullptr || self->esp32c6_realtime_uhci_rx_queue_ == nullptr) {
    return false;
  }

  Esp32RealtimeUhciRxEvent event{};
  uint32_t buffer_index = HCP2BRIDGE_C6_UHCI_RX_BUF_COUNT;
  if (edata != nullptr && edata->data != nullptr) {
    for (uint32_t i = 0; i < HCP2BRIDGE_C6_UHCI_RX_BUF_COUNT; i++) {
      if (edata->data == self->esp32c6_realtime_uhci_rx_bufs_[i]) {
        buffer_index = i;
        break;
      }
    }
  }
  if (buffer_index >= HCP2BRIDGE_C6_UHCI_RX_BUF_COUNT) {
    self->esp32_realtime_counters_.rx_starvation_count++;
    self->esp32_realtime_counters_.health_flags |= HCP2_LP_HEALTH_FLAG_RX_STARVATION;
    return false;
  }

  uint32_t len = edata != nullptr ? edata->recv_size : 0u;
  if (len > HCP2BRIDGE_C6_UHCI_RX_BUF_SIZE) {
    len = HCP2BRIDGE_C6_UHCI_RX_BUF_SIZE;
    event.flags = HCP2_RX_FRAMING_ERROR;
  } else {
    event.flags = HCP2_RX_OK;
  }
  event.len = (uint8_t) len;
  event.buffer_index = (uint8_t) buffer_index;
  if (len > self->esp32_realtime_counters_.max_rx_fifo_count) {
    self->esp32_realtime_counters_.max_rx_fifo_count = (uint16_t) len;
  }

  BaseType_t high_task_woken = pdFALSE;
  if (xQueueSendFromISR(self->esp32c6_realtime_uhci_rx_queue_, &event, &high_task_woken) != pdTRUE) {
    self->esp32c6_realtime_uhci_rx_buf_busy_[buffer_index] = false;
    self->esp32_realtime_counters_.rx_starvation_count++;
    self->esp32_realtime_counters_.health_flags |= HCP2_LP_HEALTH_FLAG_RX_STARVATION;
  }
  return high_task_woken == pdTRUE;
}

bool IRAM_ATTR HCP2Bridge::esp32c6_realtime_uhci_tx_cb_(uhci_controller_handle_t uhci,
                                                        const uhci_tx_done_event_data_t *edata, void *user_ctx) {
  (void) uhci;
  auto *self = static_cast<HCP2Bridge *>(user_ctx);
  if (self == nullptr) {
    return false;
  }
  if (self->esp32c6_realtime_uhci_tx_done_pending_) {
    self->esp32c6_realtime_uhci_tx_done_size_ = edata != nullptr ? edata->sent_size : 0u;
    self->esp32c6_realtime_uhci_tx_done_seen_ = true;
    self->esp32c6_realtime_uhci_tx_done_pending_ = false;
  }
  BaseType_t high_task_woken = pdFALSE;
  if (self->esp32_realtime_task_handle_ != nullptr) {
    vTaskNotifyGiveFromISR(self->esp32_realtime_task_handle_, &high_task_woken);
  }
  return high_task_woken == pdTRUE;
}

bool IRAM_ATTR HCP2Bridge::esp32c6_realtime_due_timer_alarm_(gptimer_handle_t timer,
                                                             const gptimer_alarm_event_data_t *edata,
                                                             void *user_ctx) {
  (void) timer;
  (void) edata;
  auto *self = static_cast<HCP2Bridge *>(user_ctx);
  if (self == nullptr || self->esp32c6_realtime_due_sem_ == nullptr) {
    return false;
  }

  BaseType_t high_task_woken = pdFALSE;
  xSemaphoreGiveFromISR(self->esp32c6_realtime_due_sem_, &high_task_woken);
  return high_task_woken == pdTRUE;
}

void HCP2Bridge::esp32c6_realtime_uhci_wait_until_(uint32_t due_us) {
  for (;;) {
    const uint32_t now_us = (uint32_t) esp_timer_get_time();
    const int32_t remaining_us = (int32_t) (due_us - now_us);
    if (remaining_us <= (int32_t) HCP2BRIDGE_C6_UHCI_TX_TIMER_GUARD_US) {
      break;
    }
    if (this->esp32_realtime_timer_ == nullptr || this->esp32c6_realtime_due_sem_ == nullptr) {
      taskYIELD();
      continue;
    }

    while (xSemaphoreTake(this->esp32c6_realtime_due_sem_, 0) == pdTRUE) {
    }

    uint64_t timer_count = 0u;
    if (gptimer_get_raw_count(this->esp32_realtime_timer_, &timer_count) != ESP_OK) {
      taskYIELD();
      continue;
    }

    const uint32_t wait_us = (uint32_t) remaining_us - HCP2BRIDGE_C6_UHCI_TX_TIMER_GUARD_US;
    gptimer_alarm_config_t alarm_config = {};
    alarm_config.alarm_count = timer_count + wait_us;
    alarm_config.reload_count = 0u;
    alarm_config.flags.auto_reload_on_alarm = 0u;
    if (gptimer_set_alarm_action(this->esp32_realtime_timer_, &alarm_config) != ESP_OK) {
      taskYIELD();
      continue;
    }

    const TickType_t timeout_ticks = pdMS_TO_TICKS((wait_us + 999u) / 1000u + 2u);
    (void) xSemaphoreTake(this->esp32c6_realtime_due_sem_, timeout_ticks);
  }
  while ((int32_t) (due_us - (uint32_t) esp_timer_get_time()) > 0) {
    asm volatile("nop");
  }
}

void HCP2Bridge::esp32c6_realtime_uhci_send_response_(const uint8_t *data, uint8_t len,
                                                      const hcp2_pending_tx_meta_t &meta) {
  if (data == nullptr || len == 0u || len > HCP2BRIDGE_C6_UHCI_TX_BUF_SIZE ||
      this->esp32c6_realtime_uhci_ == nullptr) {
    this->esp32_realtime_counters_.tx_abort_count++;
    return;
  }

  std::memcpy(this->esp32c6_realtime_uhci_tx_buf_, data, len);

  if (this->esp32_realtime_de_gpio_ != GPIO_NUM_NC) {
    gpio_set_level(this->esp32_realtime_de_gpio_, 1);
  }
  if (this->esp32_realtime_re_gpio_ != GPIO_NUM_NC) {
    gpio_set_level(this->esp32_realtime_re_gpio_, 1);
  }
  this->esp32_realtime_de_enabled_ = true;
  this->esp32_realtime_de_enabled_since_us_ = (uint32_t) esp_timer_get_time();
  esp_rom_delay_us(HCP2BRIDGE_C6_UHCI_TX_LEAD_US);

  const uint32_t start_us = (uint32_t) esp_timer_get_time();
  const uint32_t deadline_us = start_us + HCP2_REALTIME_DEFAULT_DEADMAN_US;
  hcp2_engine_mark_tx_started(&this->engine_, start_us);
  this->esp32c6_realtime_uhci_tx_done_pending_ = true;
  this->esp32c6_realtime_uhci_tx_done_seen_ = false;
  this->esp32c6_realtime_uhci_tx_done_size_ = 0u;
  (void) ulTaskNotifyTake(pdTRUE, 0);

  const esp_err_t err = uhci_transmit(this->esp32c6_realtime_uhci_, this->esp32c6_realtime_uhci_tx_buf_, len);
  if (err != ESP_OK) {
    this->esp32_realtime_counters_.tx_abort_count++;
    this->esp32c6_realtime_uhci_tx_done_pending_ = false;
  } else {
    while (!this->esp32c6_realtime_uhci_tx_done_seen_) {
      const uint32_t now_us = (uint32_t) esp_timer_get_time();
      if ((int32_t) (deadline_us - now_us) <= 0) {
        break;
      }
      (void) ulTaskNotifyTake(pdTRUE, 1);
    }
    if (!this->esp32c6_realtime_uhci_tx_done_seen_) {
      this->esp32c6_realtime_uhci_tx_done_pending_ = false;
      this->esp32_realtime_counters_.tx_abort_count++;
      this->esp32_realtime_counters_.stuck_de_count++;
      this->esp32_realtime_counters_.health_flags |= HCP2_LP_HEALTH_FLAG_STUCK_DE;
    }
    while (!uart_ll_is_tx_idle(this->esp32_realtime_uart_hw_) &&
           (int32_t) (deadline_us - (uint32_t) esp_timer_get_time()) > 0) {
      asm volatile("nop");
    }
  }

  esp_rom_delay_us(HCP2BRIDGE_C6_UHCI_DE_TAIL_US);
  if (this->esp32_realtime_re_gpio_ != GPIO_NUM_NC) {
    gpio_set_level(this->esp32_realtime_re_gpio_, 0);
  }
  if (this->esp32_realtime_de_gpio_ != GPIO_NUM_NC) {
    gpio_set_level(this->esp32_realtime_de_gpio_, 0);
  }
  this->esp32_realtime_de_enabled_ = false;

  const uint32_t done_us = (uint32_t) esp_timer_get_time();
  const uint32_t hold_us = done_us - start_us;
  if (hold_us > this->esp32_realtime_counters_.max_de_hold_us) {
    this->esp32_realtime_counters_.max_de_hold_us = hold_us;
  }
  if (meta.is_status_response) {
    const uint32_t tx_start_delay_us = start_us - meta.scheduled_us;
    if (tx_start_delay_us > this->engine_.max_status_response_schedule_to_tx_start_us) {
      this->engine_.max_status_response_schedule_to_tx_start_us = tx_start_delay_us;
    }
  }
  hcp2_engine_mark_tx_done(&this->engine_, done_us);
}

void HCP2Bridge::esp32c6_realtime_uhci_service_pending_tx_() {
  uint8_t tx[HCP2_MAX_FRAME_LEN];
  uint8_t tx_len = 0u;
  hcp2_pending_tx_meta_t meta{};

  while (hcp2_engine_pending_tx_ready(&this->engine_)) {
    const uint32_t due_us = hcp2_engine_pending_tx_due_us(&this->engine_);
    this->esp32c6_realtime_uhci_wait_until_(due_us);
    if (!hcp2_engine_claim_due_tx(&this->engine_, (uint32_t) esp_timer_get_time(), tx, &tx_len, &meta)) {
      return;
    }
    this->esp32c6_realtime_uhci_send_response_(tx, tx_len, meta);
  }
}
#endif

bool HCP2Bridge::setup_esp32_realtime_() {
#ifdef USE_ESP32_VARIANT_ESP32C6
  if (this->backend_kind_ == HCP2BackendKind::ESP32C6_HP_REALTIME) {
    ulp_lp_core_stop();
    ESP_LOGW(TAG, "Stopped LP core before starting ESP32-C6 HP realtime backend");
  }
#endif

  const bool use_c6_uhci =
#ifdef USE_ESP32_VARIANT_ESP32C6
      this->backend_kind_ == HCP2BackendKind::ESP32C6_HP_REALTIME;
#else
      false;
#endif

  const hcp2_port_t port = {
      .user = this,
      .now_us = use_c6_uhci || !this->bench_enable_realtime_uart_ ? HCP2Bridge::now_us_cb_
                                                                  : HCP2Bridge::esp32_realtime_now_us_cb_,
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

  if (this->bench_enable_realtime_uart_ && use_c6_uhci) {
#ifdef USE_ESP32_VARIANT_ESP32C6
    if (!this->setup_esp32c6_realtime_uhci_()) {
      return false;
    }
#endif
  } else if (this->bench_enable_realtime_uart_ && !this->setup_esp32_realtime_uart_timer_()) {
    return false;
  }

  const uint32_t start_us = use_c6_uhci || !this->bench_enable_realtime_uart_
                                ? HCP2Bridge::now_us_cb_(this)
                                : HCP2Bridge::esp32_realtime_now_us_cb_(this);
  hcp2_responder_runtime_begin_from_mailbox(&this->esp32_realtime_runtime_, start_us);
  hcp2_responder_runtime_trace(&this->esp32_realtime_runtime_, HCP2_LP_TRACE_BOOT, 0u,
                               start_us);

  this->backend_ready_ = true;
  this->protocol_log_append_control_(
      this->bench_enable_realtime_uart_
          ? (this->backend_kind_ == HCP2BackendKind::ESP32C6_HP_REALTIME ? "esp32c6_hp_realtime_uart_ready"
                                                                          : "esp32_realtime_phase_e_uart_ready")
          : "esp32_realtime_phase_d_ready");
  if (this->bench_enable_realtime_uart_) {
    ESP_LOGW(TAG, "%s is enabled for bench/HIL only",
             use_c6_uhci ? "ESP32-C6 realtime UHCI/GDMA path" : "ESP32 realtime UART/timer path");
  } else {
    ESP_LOGW(TAG, "ESP32 realtime backend Phase D is mailbox/debug only; UART/ISR TX is not active");
  }
  return true;
}

bool HCP2Bridge::setup_esp32_realtime_uart_timer_() {
  if (this->rx_pin_ == nullptr || this->tx_pin_ == nullptr) {
    ESP_LOGE(TAG, "rx_pin and tx_pin are required for ESP32 realtime UART");
    return false;
  }
  if (this->rs485_mode_ == HCP2RS485Mode::DE_RE && (this->de_pin_ == nullptr || this->re_pin_ == nullptr)) {
    ESP_LOGE(TAG, "de_pin and re_pin are required for ESP32 realtime rs485_mode: de_re");
    return false;
  }

  this->uart_num_ = static_cast<uart_port_t>(this->uart_num_config_);
  this->esp32_realtime_uart_hw_ = esp32_realtime_uart_hw_for_port_(this->uart_num_);
  if (this->esp32_realtime_uart_hw_ == nullptr) {
    ESP_LOGE(TAG, "Unsupported UART%u for ESP32 realtime", (unsigned int) this->uart_num_config_);
    return false;
  }
  if (UART_HW_FIFO_LEN(this->uart_num_) < HCP2_MAX_FRAME_LEN) {
    ESP_LOGE(TAG, "UART%u FIFO is too small for complete-frame HCP2 TX", (unsigned int) this->uart_num_config_);
    return false;
  }

#ifdef USE_ESP32_VARIANT_ESP32C6
  rtc_gpio_deinit(static_cast<gpio_num_t>(this->rx_pin_->get_pin()));
  rtc_gpio_deinit(static_cast<gpio_num_t>(this->tx_pin_->get_pin()));
  if (this->de_pin_ != nullptr) {
    rtc_gpio_deinit(static_cast<gpio_num_t>(this->de_pin_->get_pin()));
  }
  if (this->re_pin_ != nullptr) {
    rtc_gpio_deinit(static_cast<gpio_num_t>(this->re_pin_->get_pin()));
  }
#endif

  if (this->rs485_mode_ == HCP2RS485Mode::DE_RE) {
    this->esp32_realtime_de_gpio_ = static_cast<gpio_num_t>(this->de_pin_->get_pin());
    this->esp32_realtime_re_gpio_ = static_cast<gpio_num_t>(this->re_pin_->get_pin());
    this->de_pin_->setup();
    this->de_pin_->pin_mode(gpio::Flags::FLAG_OUTPUT | gpio::Flags::FLAG_PULLDOWN);
    this->re_pin_->setup();
    this->re_pin_->pin_mode(gpio::Flags::FLAG_OUTPUT | gpio::Flags::FLAG_PULLDOWN);
    gpio_set_level(this->esp32_realtime_de_gpio_, 0);
    gpio_set_level(this->esp32_realtime_re_gpio_, 0);
  } else {
    this->esp32_realtime_de_gpio_ = GPIO_NUM_NC;
    this->esp32_realtime_re_gpio_ = GPIO_NUM_NC;
  }

  if (uart_is_driver_installed(this->uart_num_)) {
    ESP_LOGW(TAG, "UART%u already has a driver; replacing it for hcp2bridge realtime",
             (unsigned int) this->uart_num_config_);
    uart_driver_delete(this->uart_num_);
  }

  uart_config_t uart_config{};
  uart_config.baud_rate = HCP2BRIDGE_BAUD_RATE;
  uart_config.data_bits = UART_DATA_8_BITS;
  uart_config.parity = UART_PARITY_EVEN;
  uart_config.stop_bits = UART_STOP_BITS_1;
  uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_config.rx_flow_ctrl_thresh = 0;
#ifdef USE_ESP32_VARIANT_ESP32C6
  uart_config.source_clk = UART_SCLK_XTAL;
#else
  uart_config.source_clk = UART_SCLK_DEFAULT;
#endif

  esp_err_t err = uart_param_config(this->uart_num_, &uart_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
    return false;
  }
  err = uart_set_pin(this->uart_num_, this->tx_pin_->get_pin(), this->rx_pin_->get_pin(), UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
    return false;
  }
  // This path owns the UART through low-level FIFO/interrupt access instead of
  // the ESP-IDF UART driver. uart_set_mode() requires installed driver state on
  // ESP32-C6/IDF 5.5 and fails with ESP_ERR_INVALID_STATE here; plain UART mode
  // is already the hardware reset/default mode after uart_param_config().
  uart_set_tx_idle_num(this->uart_num_, 2u);
  uart_ll_disable_intr_mask(this->esp32_realtime_uart_hw_, HCP2BRIDGE_REALTIME_UART_INTR_MASK);
  uart_ll_clr_intsts_mask(this->esp32_realtime_uart_hw_, HCP2BRIDGE_REALTIME_UART_INTR_MASK);
  uart_ll_rxfifo_rst(this->esp32_realtime_uart_hw_);
  uart_ll_txfifo_rst(this->esp32_realtime_uart_hw_);
  uart_ll_set_rxfifo_full_thr(this->esp32_realtime_uart_hw_, HCP2BRIDGE_REALTIME_RXFIFO_FULL_THR);
  uart_ll_set_rx_tout(this->esp32_realtime_uart_hw_, 2u);

  gptimer_config_t timer_config{};
  timer_config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
  timer_config.direction = GPTIMER_COUNT_UP;
  timer_config.resolution_hz = HCP2BRIDGE_REALTIME_TIMER_HZ;
  timer_config.intr_priority = 3;
  err = gptimer_new_timer(&timer_config, &this->esp32_realtime_timer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gptimer_new_timer failed: %s", esp_err_to_name(err));
    return false;
  }
  gptimer_event_callbacks_t callbacks{};
  callbacks.on_alarm = HCP2Bridge::esp32_realtime_timer_alarm_;
  err = gptimer_register_event_callbacks(this->esp32_realtime_timer_, &callbacks, this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gptimer_register_event_callbacks failed: %s", esp_err_to_name(err));
    return false;
  }
  err = gptimer_enable(this->esp32_realtime_timer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gptimer_enable failed: %s", esp_err_to_name(err));
    return false;
  }
  err = gptimer_start(this->esp32_realtime_timer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gptimer_start failed: %s", esp_err_to_name(err));
    return false;
  }

  err = esp_intr_alloc_intrstatus(uart_periph_signal[this->uart_num_].irq, ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3,
                                  (uint32_t) uart_ll_get_intr_status_reg(this->esp32_realtime_uart_hw_),
                                  HCP2BRIDGE_REALTIME_UART_INTR_MASK, HCP2Bridge::esp32_realtime_uart_isr_, this,
                                  &this->esp32_realtime_uart_intr_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_intr_alloc_intrstatus UART failed: %s", esp_err_to_name(err));
    return false;
  }
  uart_ll_ena_intr_mask(this->esp32_realtime_uart_hw_, HCP2BRIDGE_REALTIME_UART_RX_INTR_MASK);
  this->uart_ready_ = true;
  this->esp32_realtime_uart_active_ = true;
  return true;
}

#ifdef USE_ESP32_VARIANT_ESP32C6
bool HCP2Bridge::setup_esp32c6_realtime_uhci_() {
  if (this->rx_pin_ == nullptr || this->tx_pin_ == nullptr) {
    ESP_LOGE(TAG, "rx_pin and tx_pin are required for ESP32-C6 realtime UHCI");
    return false;
  }
  if (this->rs485_mode_ != HCP2RS485Mode::DE_RE || this->de_pin_ == nullptr || this->re_pin_ == nullptr) {
    ESP_LOGE(TAG, "ESP32-C6 realtime UHCI currently requires rs485_mode: de_re");
    return false;
  }

  this->uart_num_ = static_cast<uart_port_t>(this->uart_num_config_);
  this->esp32_realtime_uart_hw_ = esp32_realtime_uart_hw_for_port_(this->uart_num_);
  if (this->esp32_realtime_uart_hw_ == nullptr) {
    ESP_LOGE(TAG, "Unsupported UART%u for ESP32-C6 realtime UHCI", (unsigned int) this->uart_num_config_);
    return false;
  }

  rtc_gpio_deinit(static_cast<gpio_num_t>(this->rx_pin_->get_pin()));
  rtc_gpio_deinit(static_cast<gpio_num_t>(this->tx_pin_->get_pin()));
  rtc_gpio_deinit(static_cast<gpio_num_t>(this->de_pin_->get_pin()));
  rtc_gpio_deinit(static_cast<gpio_num_t>(this->re_pin_->get_pin()));

  this->esp32_realtime_de_gpio_ = static_cast<gpio_num_t>(this->de_pin_->get_pin());
  this->esp32_realtime_re_gpio_ = static_cast<gpio_num_t>(this->re_pin_->get_pin());
  this->de_pin_->setup();
  this->de_pin_->pin_mode(gpio::Flags::FLAG_OUTPUT | gpio::Flags::FLAG_PULLDOWN);
  this->re_pin_->setup();
  this->re_pin_->pin_mode(gpio::Flags::FLAG_OUTPUT | gpio::Flags::FLAG_PULLDOWN);
  gpio_set_level(this->esp32_realtime_de_gpio_, 0);
  gpio_set_level(this->esp32_realtime_re_gpio_, 0);

  if (uart_is_driver_installed(this->uart_num_)) {
    ESP_LOGW(TAG, "UART%u already has a driver; replacing it for hcp2bridge C6 realtime UHCI",
             (unsigned int) this->uart_num_config_);
    uart_driver_delete(this->uart_num_);
  }

  uart_config_t uart_config{};
  uart_config.baud_rate = HCP2BRIDGE_BAUD_RATE;
  uart_config.data_bits = UART_DATA_8_BITS;
  uart_config.parity = UART_PARITY_EVEN;
  uart_config.stop_bits = UART_STOP_BITS_1;
  uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_config.rx_flow_ctrl_thresh = 0;
  uart_config.source_clk = UART_SCLK_XTAL;

  esp_err_t err = uart_param_config(this->uart_num_, &uart_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
    return false;
  }
  err = uart_set_pin(this->uart_num_, this->tx_pin_->get_pin(), this->rx_pin_->get_pin(), UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
    return false;
  }
  gpio_set_pull_mode(static_cast<gpio_num_t>(this->rx_pin_->get_pin()), GPIO_PULLUP_ONLY);
  uart_set_tx_idle_num(this->uart_num_, 2u);
  uart_ll_disable_intr_mask(this->esp32_realtime_uart_hw_, UINT32_MAX);
  uart_ll_clr_intsts_mask(this->esp32_realtime_uart_hw_, UINT32_MAX);
  uart_ll_rxfifo_rst(this->esp32_realtime_uart_hw_);
  uart_ll_txfifo_rst(this->esp32_realtime_uart_hw_);
  uart_ll_set_rx_idle_thr(this->esp32_realtime_uart_hw_, HCP2BRIDGE_C6_UHCI_RX_IDLE_BITS);
  uart_ll_update(this->esp32_realtime_uart_hw_);

  this->esp32c6_realtime_uhci_rx_queue_ =
      xQueueCreate(6, sizeof(HCP2Bridge::Esp32RealtimeUhciRxEvent));
  if (this->esp32c6_realtime_uhci_rx_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create ESP32-C6 realtime UHCI RX queue");
    return false;
  }
  this->esp32c6_realtime_due_sem_ = xSemaphoreCreateBinary();
  if (this->esp32c6_realtime_due_sem_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create ESP32-C6 realtime due semaphore");
    return false;
  }

  gptimer_config_t timer_config{};
  timer_config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
  timer_config.direction = GPTIMER_COUNT_UP;
  timer_config.resolution_hz = HCP2BRIDGE_REALTIME_TIMER_HZ;
  timer_config.intr_priority = 3;
  err = gptimer_new_timer(&timer_config, &this->esp32_realtime_timer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gptimer_new_timer failed: %s", esp_err_to_name(err));
    return false;
  }
  gptimer_event_callbacks_t timer_callbacks{};
  timer_callbacks.on_alarm = HCP2Bridge::esp32c6_realtime_due_timer_alarm_;
  err = gptimer_register_event_callbacks(this->esp32_realtime_timer_, &timer_callbacks, this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gptimer_register_event_callbacks failed: %s", esp_err_to_name(err));
    return false;
  }
  err = gptimer_enable(this->esp32_realtime_timer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gptimer_enable failed: %s", esp_err_to_name(err));
    return false;
  }
  err = gptimer_start(this->esp32_realtime_timer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gptimer_start failed: %s", esp_err_to_name(err));
    return false;
  }

  uhci_controller_config_t uhci_config{};
  uhci_config.uart_port = this->uart_num_;
  uhci_config.tx_trans_queue_depth = 2;
  uhci_config.max_transmit_size = HCP2BRIDGE_C6_UHCI_TX_BUF_SIZE;
  uhci_config.max_receive_internal_mem = HCP2BRIDGE_C6_UHCI_RX_BUF_SIZE * HCP2BRIDGE_C6_UHCI_RX_BUF_COUNT;
  uhci_config.dma_burst_size = 0;
  uhci_config.max_packet_receive = 0;
  uhci_config.rx_eof_flags.idle_eof = 1;

  err = uhci_new_controller(&uhci_config, &this->esp32c6_realtime_uhci_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uhci_new_controller failed: %s", esp_err_to_name(err));
    return false;
  }
  uhci_event_callbacks_t callbacks{};
  callbacks.on_rx_trans_event = HCP2Bridge::esp32c6_realtime_uhci_rx_cb_;
  callbacks.on_tx_trans_done = HCP2Bridge::esp32c6_realtime_uhci_tx_cb_;
  err = uhci_register_event_callbacks(this->esp32c6_realtime_uhci_, &callbacks, this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uhci_register_event_callbacks failed: %s", esp_err_to_name(err));
    return false;
  }

  this->uart_ready_ = true;
  this->esp32_realtime_uart_active_ = true;
  this->esp32c6_realtime_uhci_active_ = true;
  return true;
}
#endif

void HCP2Bridge::start_esp32_realtime_task_() {
  if (this->esp32_realtime_task_handle_ != nullptr) {
    return;
  }
  const bool use_c6_uhci =
#ifdef USE_ESP32_VARIANT_ESP32C6
      this->esp32c6_realtime_uhci_active_;
#else
      false;
#endif
  const UBaseType_t task_priority = use_c6_uhci ? (configMAX_PRIORITIES - 1) : 5;
  const BaseType_t ok = xTaskCreatePinnedToCore(HCP2Bridge::esp32_realtime_task_trampoline_, "hcp2_rt_proto",
                                                HCP2BRIDGE_REALTIME_TASK_STACK_BYTES, this, task_priority,
                                                &this->esp32_realtime_task_handle_, tskNO_AFFINITY);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to start HCP2 ESP32 realtime prototype task");
    this->esp32_realtime_task_handle_ = nullptr;
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "Started HCP2 ESP32 realtime prototype task priority=%u stack=%u bytes",
           (unsigned int) task_priority, (unsigned int) HCP2BRIDGE_REALTIME_TASK_STACK_BYTES);
  if (use_c6_uhci) {
    this->start_esp32_realtime_maintenance_task_();
  }
}

void HCP2Bridge::start_esp32_realtime_maintenance_task_() {
  if (this->esp32_realtime_maintenance_task_handle_ != nullptr) {
    return;
  }
  const BaseType_t ok =
      xTaskCreatePinnedToCore(HCP2Bridge::esp32_realtime_maintenance_task_trampoline_, "hcp2_rt_maint",
                              HCP2BRIDGE_REALTIME_MAINT_TASK_STACK_BYTES, this, 3,
                              &this->esp32_realtime_maintenance_task_handle_, tskNO_AFFINITY);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to start HCP2 ESP32 realtime maintenance task");
    this->esp32_realtime_maintenance_task_handle_ = nullptr;
    return;
  }
  ESP_LOGI(TAG, "Started HCP2 ESP32 realtime maintenance task stack=%u bytes",
           (unsigned int) HCP2BRIDGE_REALTIME_MAINT_TASK_STACK_BYTES);
}

void HCP2Bridge::esp32_realtime_task_trampoline_(void *arg) {
  auto *self = static_cast<HCP2Bridge *>(arg);
  if (self == nullptr) {
    vTaskDelete(nullptr);
    return;
  }
  self->esp32_realtime_task_loop_();
}

void HCP2Bridge::esp32_realtime_maintenance_task_trampoline_(void *arg) {
  auto *self = static_cast<HCP2Bridge *>(arg);
  if (self == nullptr) {
    vTaskDelete(nullptr);
    return;
  }
  self->esp32_realtime_maintenance_task_loop_();
}

void HCP2Bridge::esp32_realtime_task_loop_() {
#ifdef USE_ESP32_VARIANT_ESP32C6
  if (this->esp32c6_realtime_uhci_active_) {
    this->esp32c6_realtime_uhci_task_loop_();
    return;
  }
#endif

  for (;;) {
    const uint32_t loop_start_us =
        this->bench_enable_realtime_uart_ ? HCP2Bridge::esp32_realtime_now_us_cb_(this) : HCP2Bridge::now_us_cb_(this);

    this->esp32_realtime_mailbox_.heartbeat++;
    hcp2_responder_runtime_handle_mailbox_command(&this->esp32_realtime_runtime_, loop_start_us);
    hcp2_responder_runtime_handle_stop_trigger(&this->esp32_realtime_runtime_, loop_start_us);
    hcp2_responder_runtime_note_status_poll(&this->esp32_realtime_runtime_, loop_start_us);
    hcp2_responder_runtime_publish_state(&this->esp32_realtime_runtime_, loop_start_us);
    hcp2_responder_runtime_publish_counters(&this->esp32_realtime_runtime_, &this->esp32_realtime_counters_,
                                            loop_start_us);

    while (hcp2_responder_runtime_publish_protocol_event(&this->esp32_realtime_runtime_) != 0u) {
    }

    if (this->esp32_realtime_de_enabled_ &&
        (uint32_t) (loop_start_us - this->esp32_realtime_de_enabled_since_us_) >= HCP2_REALTIME_DEFAULT_DEADMAN_US) {
      HCP2Bridge::esp32_realtime_finish_tx_from_isr_(this, loop_start_us, true);
    }

    this->update_state_from_mailbox_();
    this->drain_lp_protocol_event_();
    this->drain_lp_trace_();

    const uint32_t loop_end_us =
        this->bench_enable_realtime_uart_ ? HCP2Bridge::esp32_realtime_now_us_cb_(this) : HCP2Bridge::now_us_cb_(this);
    const uint32_t loop_us = loop_end_us - loop_start_us;
    if (loop_us > this->esp32_realtime_counters_.max_loop_us) {
      this->esp32_realtime_counters_.max_loop_us = loop_us;
    }
    vTaskDelay(pdMS_TO_TICKS(HCP2BRIDGE_REALTIME_TASK_TICK_MS));
  }
}

#ifdef USE_ESP32_VARIANT_ESP32C6
void HCP2Bridge::esp32c6_realtime_uhci_task_loop_() {
  Esp32RealtimeUhciRxEvent event{};

  ESP_LOGI(TAG, "Started ESP32-C6 HCP2 realtime UHCI/GDMA task");
  if (this->esp32c6_realtime_uhci_start_rx_() != ESP_OK) {
    ESP_LOGE(TAG, "Failed to arm initial ESP32-C6 realtime UHCI RX");
    this->mark_failed();
  }

  for (;;) {
    const BaseType_t received =
        xQueueReceive(this->esp32c6_realtime_uhci_rx_queue_, &event,
                      pdMS_TO_TICKS(HCP2BRIDGE_REALTIME_C6_TASK_IDLE_MS));
    const uint32_t loop_start_us = HCP2Bridge::now_us_cb_(this);

    if (received == pdTRUE) {
      const bool event_buffer_valid = event.buffer_index < HCP2BRIDGE_C6_UHCI_RX_BUF_COUNT &&
                                      this->esp32c6_realtime_uhci_rx_buf_busy_[event.buffer_index];
      const uint32_t rearm_start_us = HCP2Bridge::now_us_cb_(this);
      esp_err_t rearm_err = this->esp32c6_realtime_uhci_start_rx_(false);
      const uint32_t rearm_us = HCP2Bridge::now_us_cb_(this) - rearm_start_us;
      if (rearm_us > this->esp32_realtime_counters_.max_loop_us) {
        this->esp32_realtime_counters_.max_loop_us = rearm_us;
      }
      if ((event.flags & (HCP2_RX_PARITY_ERROR | HCP2_RX_FRAMING_ERROR)) != 0u) {
        this->esp32_realtime_counters_.port_rx_error_count++;
      }
      if (event_buffer_valid) {
        const uint8_t *rx = this->esp32c6_realtime_uhci_rx_bufs_[event.buffer_index];
        for (uint8_t i = 0; i < event.len; i++) {
          hcp2_engine_rx_byte(&this->engine_, rx[i], event.flags);
        }
        this->esp32c6_realtime_uhci_rx_buf_busy_[event.buffer_index] = false;
        if (rearm_err != ESP_OK) {
          rearm_err = this->esp32c6_realtime_uhci_start_rx_(false);
        }
      } else {
        this->esp32_realtime_counters_.rx_starvation_count++;
        this->esp32_realtime_counters_.health_flags |= HCP2_LP_HEALTH_FLAG_RX_STARVATION;
      }
      if (event_buffer_valid && rearm_err != ESP_OK) {
        this->esp32_realtime_counters_.rx_starvation_count++;
        this->esp32_realtime_counters_.health_flags |= HCP2_LP_HEALTH_FLAG_RX_STARVATION;
      }
      this->esp32c6_realtime_uhci_service_pending_tx_();
    }

    const uint32_t now_us = HCP2Bridge::now_us_cb_(this);
    this->esp32_realtime_mailbox_.heartbeat++;
    hcp2_responder_runtime_handle_mailbox_command(&this->esp32_realtime_runtime_, now_us);
    hcp2_responder_runtime_handle_stop_trigger(&this->esp32_realtime_runtime_, now_us);
    hcp2_responder_runtime_note_status_poll(&this->esp32_realtime_runtime_, now_us);

    if (this->esp32_realtime_de_enabled_ &&
        (uint32_t) (now_us - this->esp32_realtime_de_enabled_since_us_) >= HCP2_REALTIME_DEFAULT_DEADMAN_US) {
      if (this->esp32_realtime_re_gpio_ != GPIO_NUM_NC) {
        gpio_set_level(this->esp32_realtime_re_gpio_, 0);
      }
      if (this->esp32_realtime_de_gpio_ != GPIO_NUM_NC) {
        gpio_set_level(this->esp32_realtime_de_gpio_, 0);
      }
      this->esp32_realtime_de_enabled_ = false;
      this->esp32_realtime_counters_.stuck_de_count++;
      this->esp32_realtime_counters_.health_flags |= HCP2_LP_HEALTH_FLAG_STUCK_DE;
    }

    const uint32_t loop_us = HCP2Bridge::now_us_cb_(this) - loop_start_us;
    if (loop_us > this->esp32_realtime_counters_.max_loop_us) {
      this->esp32_realtime_counters_.max_loop_us = loop_us;
    }
    if (loop_us > HCP2BRIDGE_REALTIME_TASK_TICK_MS * 1000u) {
      this->esp32_realtime_counters_.loop_overrun_count++;
      this->esp32_realtime_counters_.health_flags |= HCP2_LP_HEALTH_FLAG_LOOP_OVERRUN;
    }
  }
}
#endif

void HCP2Bridge::esp32_realtime_maintenance_task_loop_() {
  uint32_t last_health_log_ms = 0u;

  ESP_LOGI(TAG, "Started HCP2 ESP32 realtime maintenance task");
  for (;;) {
    const uint32_t now_us = HCP2Bridge::now_us_cb_(this);

    hcp2_responder_runtime_publish_state(&this->esp32_realtime_runtime_, now_us);
    hcp2_responder_runtime_publish_counters(&this->esp32_realtime_runtime_, &this->esp32_realtime_counters_, now_us);
    while (hcp2_responder_runtime_publish_protocol_event(&this->esp32_realtime_runtime_) != 0u) {
    }

    this->update_state_from_mailbox_();
    this->drain_lp_protocol_event_();
    this->drain_lp_trace_();

    const uint32_t now_ms = millis();
    const bool use_c6_uhci =
#ifdef USE_ESP32_VARIANT_ESP32C6
        this->esp32c6_realtime_uhci_active_;
#else
        false;
#endif
    if (use_c6_uhci && this->engine_.status_polls_received != 0u &&
        now_ms - last_health_log_ms >= HCP2BRIDGE_LP_HEALTH_LOG_INTERVAL_MS) {
      last_health_log_ms = now_ms;
      ESP_LOGI(TAG,
               "c6_uhci poll=%" PRIu32 " ans=%" PRIu32 " crc=%" PRIu32 " rxerr=%" PRIu32
               " tx_abort=%" PRIu32 " max_de=%" PRIu32 " max_loop=%" PRIu32
               " max_rx=%" PRIu16 " drops=%" PRIu32,
               this->engine_.status_polls_received, this->engine_.status_responses_sent,
               this->engine_.crc_errors, this->engine_.rx_errors,
               this->esp32_realtime_counters_.tx_abort_count,
               this->esp32_realtime_counters_.max_de_hold_us,
               this->esp32_realtime_counters_.max_loop_us,
               this->esp32_realtime_counters_.max_rx_fifo_count,
               this->esp32_realtime_counters_.rx_starvation_count + this->engine_.pending_tx_drop_count);
    }

    vTaskDelay(pdMS_TO_TICKS(HCP2BRIDGE_REALTIME_TASK_TICK_MS));
  }
}

void IRAM_ATTR HCP2Bridge::esp32_realtime_uart_isr_(void *arg) {
  auto *self = static_cast<HCP2Bridge *>(arg);
  if (self == nullptr || self->esp32_realtime_uart_hw_ == nullptr) {
    return;
  }

  uart_dev_t *hw = self->esp32_realtime_uart_hw_;
  const uint32_t status = uart_ll_get_intsts_mask(hw);
  const uint32_t now = HCP2Bridge::esp32_realtime_now_us_cb_(self);
  uint8_t rx_flags = HCP2_RX_OK;

  if (self->esp32_realtime_tx_active_ && (int32_t) (now - self->esp32_realtime_tx_tail_due_us_) >= 0) {
    HCP2Bridge::esp32_realtime_finish_tx_from_isr_(self, now, false);
  }

  if ((status & UART_INTR_PARITY_ERR) != 0u) {
    rx_flags |= HCP2_RX_PARITY_ERROR;
    self->esp32_realtime_counters_.port_rx_error_count++;
  }
  if ((status & UART_INTR_FRAM_ERR) != 0u) {
    rx_flags |= HCP2_RX_FRAMING_ERROR;
    self->esp32_realtime_counters_.port_rx_error_count++;
  }
  if ((status & UART_INTR_RXFIFO_OVF) != 0u) {
    self->esp32_realtime_counters_.port_rx_error_count++;
    self->esp32_realtime_counters_.rx_starvation_count++;
  }

  if ((status & (UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT | UART_INTR_PARITY_ERR | UART_INTR_FRAM_ERR |
                 UART_INTR_RXFIFO_OVF)) != 0u) {
    for (;;) {
      uint32_t rx_len = uart_ll_get_rxfifo_len(hw);
      if (rx_len == 0u) {
        break;
      }
      if (rx_len > self->esp32_realtime_counters_.max_rx_fifo_count) {
        self->esp32_realtime_counters_.max_rx_fifo_count = (uint16_t) rx_len;
      }
      while (rx_len > 0u) {
        uint8_t byte = 0u;
        uart_ll_read_rxfifo(hw, &byte, 1u);
        hcp2_engine_rx_byte(&self->engine_, byte, rx_flags);
        rx_len--;
      }
    }
    if ((status & UART_INTR_RXFIFO_OVF) != 0u) {
      uart_ll_rxfifo_rst(hw);
    }
    HCP2Bridge::esp32_realtime_schedule_timer_from_isr_(self);
  }

  if ((status & UART_INTR_TX_DONE) != 0u) {
    uart_ll_disable_intr_mask(hw, UART_INTR_TX_DONE);
    if (self->esp32_realtime_tx_active_) {
      HCP2Bridge::esp32_realtime_finish_tx_from_isr_(self, now, false);
    }
  }

  uart_ll_clr_intsts_mask(hw, status & HCP2BRIDGE_REALTIME_UART_INTR_MASK);
}

bool IRAM_ATTR HCP2Bridge::esp32_realtime_timer_alarm_(gptimer_handle_t timer,
                                                       const gptimer_alarm_event_data_t *edata, void *user_ctx) {
  auto *self = static_cast<HCP2Bridge *>(user_ctx);
  const uint32_t now = edata != nullptr ? (uint32_t) edata->count_value : HCP2Bridge::esp32_realtime_now_us_cb_(self);

  (void) timer;
  if (self == nullptr || self->esp32_realtime_uart_hw_ == nullptr) {
    return false;
  }

  if (self->esp32_realtime_tx_active_) {
    if ((int32_t) (now - self->esp32_realtime_tx_tail_due_us_) < 0) {
      (void) HCP2Bridge::esp32_realtime_schedule_alarm_from_isr_(self, self->esp32_realtime_tx_tail_due_us_);
      return false;
    }
    HCP2Bridge::esp32_realtime_finish_tx_from_isr_(self, now, false);
    return false;
  }

  if (!HCP2Bridge::esp32_realtime_send_due_tx_from_isr_(self, now)) {
    HCP2Bridge::esp32_realtime_schedule_timer_from_isr_(self);
  }
  return false;
}

uint32_t IRAM_ATTR HCP2Bridge::esp32_realtime_now_us_cb_(void *user) {
  auto *self = static_cast<HCP2Bridge *>(user);
  uint64_t now = 0u;
  if (self == nullptr || self->esp32_realtime_timer_ == nullptr) {
    return 0u;
  }
  if (gptimer_get_raw_count(self->esp32_realtime_timer_, &now) != ESP_OK) {
    return 0u;
  }
  return (uint32_t) now;
}
#else
bool HCP2Bridge::setup_esp32_realtime_() {
  ESP_LOGE(TAG, "ESP32 realtime backend requires a classic ESP32 build");
  this->backend_ready_ = false;
  return false;
}

void HCP2Bridge::start_esp32_realtime_task_() {}
void HCP2Bridge::start_esp32_realtime_maintenance_task_() {}

void HCP2Bridge::esp32_realtime_task_trampoline_(void *arg) {
  (void) arg;
  vTaskDelete(nullptr);
}

void HCP2Bridge::esp32_realtime_maintenance_task_trampoline_(void *arg) {
  (void) arg;
  vTaskDelete(nullptr);
}

void HCP2Bridge::esp32_realtime_task_loop_() { vTaskDelete(nullptr); }
void HCP2Bridge::esp32_realtime_maintenance_task_loop_() { vTaskDelete(nullptr); }
bool HCP2Bridge::setup_esp32_realtime_uart_timer_() { return false; }
void HCP2Bridge::esp32_realtime_uart_isr_(void *arg) { (void) arg; }
bool HCP2Bridge::esp32_realtime_timer_alarm_(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata,
                                             void *user_ctx) {
  (void) timer;
  (void) edata;
  (void) user_ctx;
  return false;
}
uint32_t HCP2Bridge::esp32_realtime_now_us_cb_(void *user) {
  (void) user;
  return 0u;
}
#endif

}  // namespace hcp2bridge
}  // namespace esphome

#endif
