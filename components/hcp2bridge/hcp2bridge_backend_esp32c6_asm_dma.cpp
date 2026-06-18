#include "hcp2bridge.h"
#include "hcp2bridge_internal.h"

#ifdef USE_ESP32

#include <cstddef>
#include <cstring>
#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "hal/uart_ll.h"
#include "soc/reg_base.h"
#include "soc/soc_caps.h"
#include "soc/uart_periph.h"
#include "soc/uart_struct.h"

#ifdef USE_ESP32_VARIANT_ESP32C6
#include "ulp_lp_core.h"
#endif

namespace esphome {
namespace hcp2bridge {

static const char *const TAG = "hcp2bridge";

#ifdef USE_ESP32_VARIANT_ESP32C6
static constexpr uint32_t HCP2BRIDGE_C6_ASM_PROBE_TASK_TICK_MS = 50;
static constexpr uint32_t HCP2BRIDGE_C6_ASM_PROBE_RXFIFO_FULL_THR = 1u;
static constexpr uint32_t HCP2BRIDGE_C6_ASM_PROBE_RX_INTR_MASK = UART_INTR_RXFIFO_FULL;
static constexpr uint32_t HCP2BRIDGE_C6_ASM_PROBE_ERROR_INTR_MASK = 0u;

static_assert(offsetof(hcp2_c6_asm_dma_probe_state_t, magic) == HCP2_C6_ASM_DMA_PROBE_OFF_MAGIC);
static_assert(offsetof(hcp2_c6_asm_dma_probe_state_t, version) == HCP2_C6_ASM_DMA_PROBE_OFF_VERSION);
static_assert(offsetof(hcp2_c6_asm_dma_probe_state_t, flags) == HCP2_C6_ASM_DMA_PROBE_OFF_FLAGS);
static_assert(offsetof(hcp2_c6_asm_dma_probe_state_t, irq_count) == HCP2_C6_ASM_DMA_PROBE_OFF_IRQ_COUNT);
static_assert(offsetof(hcp2_c6_asm_dma_probe_state_t, rx_irq_count) == HCP2_C6_ASM_DMA_PROBE_OFF_RX_IRQ_COUNT);
static_assert(offsetof(hcp2_c6_asm_dma_probe_state_t, other_irq_count) ==
              HCP2_C6_ASM_DMA_PROBE_OFF_OTHER_IRQ_COUNT);
static_assert(offsetof(hcp2_c6_asm_dma_probe_state_t, drained_bytes) == HCP2_C6_ASM_DMA_PROBE_OFF_DRAINED_BYTES);
static_assert(offsetof(hcp2_c6_asm_dma_probe_state_t, max_irq_gap_cycles) ==
              HCP2_C6_ASM_DMA_PROBE_OFF_MAX_IRQ_GAP_CYCLES);
static_assert(offsetof(hcp2_c6_asm_dma_probe_state_t, last_status) == HCP2_C6_ASM_DMA_PROBE_OFF_LAST_STATUS);
static_assert(offsetof(hcp2_c6_asm_dma_probe_state_t, max_fifo_count) ==
              HCP2_C6_ASM_DMA_PROBE_OFF_MAX_FIFO_COUNT);
static_assert(offsetof(hcp2_c6_asm_dma_probe_state_t, uart_int_st_addr) ==
              HCP2_C6_ASM_DMA_PROBE_OFF_UART_INT_ST_ADDR);
static_assert(offsetof(hcp2_c6_asm_dma_probe_state_t, uart_fifo_addr) ==
              HCP2_C6_ASM_DMA_PROBE_OFF_UART_FIFO_ADDR);
static_assert(offsetof(hcp2_c6_asm_dma_probe_state_t, error_intr_mask) ==
              HCP2_C6_ASM_DMA_PROBE_OFF_ERROR_INTR_MASK);
static_assert(offsetof(hcp2_c6_asm_dma_probe_state_t, tx_desc_addr) == HCP2_C6_ASM_DMA_PROBE_OFF_TX_DESC_ADDR);
static_assert(sizeof(hcp2_c6_asm_dma_probe_state_t) == HCP2_C6_ASM_DMA_PROBE_SIZE);

static uart_dev_t *esp32c6_asm_dma_probe_uart_hw_for_port_(uart_port_t uart_num) {
#if defined(SOC_UART_HP_NUM)
  if ((int) uart_num < 0 || uart_num >= SOC_UART_HP_NUM) {
    return nullptr;
  }
#endif
  return UART_LL_GET_HW(uart_num);
}

#define HCP2_C6_ASM_PROBE_STR_INNER(x) #x
#define HCP2_C6_ASM_PROBE_STR(x) HCP2_C6_ASM_PROBE_STR_INNER(x)

extern "C" void IRAM_ATTR __attribute__((noinline, used)) hcp2_c6_asm_dma_probe_isr(void *arg) {
  auto *state = static_cast<hcp2_c6_asm_dma_probe_state_t *>(arg);
  if (state == nullptr || state->uart_int_st_addr == 0u) {
    return;
  }
  uint32_t now_cycles = 0;
  asm volatile("csrr %0, mcycle" : "=r"(now_cycles));
  const uint32_t last_cycles = state->last_mcycle;
  state->last_mcycle = now_cycles;
  if (last_cycles != 0u) {
    const uint32_t gap = now_cycles - last_cycles;
    if (gap > state->max_irq_gap_cycles) {
      state->max_irq_gap_cycles = gap;
    }
  }
  state->irq_count++;

  auto *int_st = reinterpret_cast<volatile uint32_t *>(state->uart_int_st_addr);
  auto *int_clr = reinterpret_cast<volatile uint32_t *>(state->uart_int_clr_addr);
  const uint32_t status = *int_st;
  state->last_status = status;
  if (status == 0u) {
    state->zero_status_count++;
    return;
  }
  if ((status & state->error_intr_mask) != 0u) {
    state->error_status_count++;
  }
  if ((status & state->rx_intr_mask) != 0u && state->uart_status_addr != 0u && state->uart_fifo_addr != 0u) {
    state->rx_irq_count++;
    auto *uart_status = reinterpret_cast<volatile uint32_t *>(state->uart_status_addr);
    auto *uart_fifo = reinterpret_cast<volatile uint32_t *>(state->uart_fifo_addr);
    uint32_t fifo_count = *uart_status & state->status_fifo_mask;
    state->last_fifo_count = fifo_count;
    if (fifo_count > state->max_fifo_count) {
      state->max_fifo_count = fifo_count;
    }
    while (fifo_count-- != 0u) {
      (void) *uart_fifo;
      state->drained_bytes++;
    }
  } else {
    state->other_irq_count++;
  }
  if (int_clr != nullptr) {
    *int_clr = status & state->clear_intr_mask;
  }
  asm volatile("fence" ::: "memory");
}

#undef HCP2_C6_ASM_PROBE_STR
#undef HCP2_C6_ASM_PROBE_STR_INNER
#endif

bool HCP2Bridge::setup_esp32c6_asm_dma_probe_() {
#ifdef USE_ESP32_VARIANT_ESP32C6
  if (!this->bench_enable_asm_dma_probe_) {
    ESP_LOGE(TAG, "ESP32-C6 asm/DMA probe backend requires bench_enable_asm_dma_probe");
    return false;
  }
  if (this->rx_pin_ == nullptr || this->tx_pin_ == nullptr) {
    ESP_LOGE(TAG, "rx_pin and tx_pin are required for ESP32-C6 asm/DMA probe");
    return false;
  }
  if (this->rs485_mode_ == HCP2RS485Mode::DE_RE && (this->de_pin_ == nullptr || this->re_pin_ == nullptr)) {
    ESP_LOGE(TAG, "de_pin and re_pin are required for ESP32-C6 asm/DMA probe rs485_mode: de_re");
    return false;
  }

  this->uart_num_ = static_cast<uart_port_t>(this->uart_num_config_);
  this->esp32c6_asm_dma_probe_uart_hw_ = esp32c6_asm_dma_probe_uart_hw_for_port_(this->uart_num_);
  if (this->esp32c6_asm_dma_probe_uart_hw_ == nullptr) {
    ESP_LOGE(TAG, "Unsupported UART%u for ESP32-C6 asm/DMA probe", (unsigned int) this->uart_num_config_);
    return false;
  }

  ulp_lp_core_stop();
  ESP_LOGW(TAG, "Stopped LP core before starting ESP32-C6 HP asm/DMA probe backend");

  rtc_gpio_deinit(static_cast<gpio_num_t>(this->rx_pin_->get_pin()));
  rtc_gpio_deinit(static_cast<gpio_num_t>(this->tx_pin_->get_pin()));
  if (this->de_pin_ != nullptr) {
    rtc_gpio_deinit(static_cast<gpio_num_t>(this->de_pin_->get_pin()));
  }
  if (this->re_pin_ != nullptr) {
    rtc_gpio_deinit(static_cast<gpio_num_t>(this->re_pin_->get_pin()));
  }

  if (this->rs485_mode_ == HCP2RS485Mode::DE_RE) {
    this->de_pin_->setup();
    this->de_pin_->pin_mode(gpio::Flags::FLAG_OUTPUT | gpio::Flags::FLAG_PULLDOWN);
    this->re_pin_->setup();
    this->re_pin_->pin_mode(gpio::Flags::FLAG_OUTPUT | gpio::Flags::FLAG_PULLDOWN);
    gpio_set_level(static_cast<gpio_num_t>(this->de_pin_->get_pin()), 0);
    gpio_set_level(static_cast<gpio_num_t>(this->re_pin_->get_pin()), 0);
  }

  if (uart_is_driver_installed(this->uart_num_)) {
    ESP_LOGW(TAG, "UART%u already has a driver; replacing it for hcp2bridge asm/DMA probe",
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

  uart_dev_t *hw = this->esp32c6_asm_dma_probe_uart_hw_;
  uart_ll_disable_intr_mask(hw, HCP2BRIDGE_C6_ASM_PROBE_RX_INTR_MASK);
  uart_ll_clr_intsts_mask(hw, HCP2BRIDGE_C6_ASM_PROBE_RX_INTR_MASK);
  uart_ll_rxfifo_rst(hw);
  uart_ll_txfifo_rst(hw);
  uart_ll_set_rxfifo_full_thr(hw, HCP2BRIDGE_C6_ASM_PROBE_RXFIFO_FULL_THR);
  uart_ll_set_rx_tout(hw, 2u);

  std::memset(&this->esp32c6_asm_dma_probe_state_, 0, sizeof(this->esp32c6_asm_dma_probe_state_));
  this->esp32c6_asm_dma_probe_state_.magic = HCP2_C6_ASM_DMA_PROBE_MAGIC;
  this->esp32c6_asm_dma_probe_state_.version = HCP2_C6_ASM_DMA_PROBE_VERSION;
  this->esp32c6_asm_dma_probe_state_.uart_int_st_addr = (uint32_t) uart_ll_get_intr_status_reg(hw);
  this->esp32c6_asm_dma_probe_state_.uart_int_clr_addr = (uint32_t) &hw->int_clr.val;
  this->esp32c6_asm_dma_probe_state_.uart_status_addr = (uint32_t) &hw->status.val;
  this->esp32c6_asm_dma_probe_state_.uart_fifo_addr = (uint32_t) &hw->fifo.val;
  this->esp32c6_asm_dma_probe_state_.rx_intr_mask = HCP2BRIDGE_C6_ASM_PROBE_RX_INTR_MASK;
  this->esp32c6_asm_dma_probe_state_.clear_intr_mask = HCP2BRIDGE_C6_ASM_PROBE_RX_INTR_MASK;
  this->esp32c6_asm_dma_probe_state_.status_fifo_mask = 0xFFu;
  this->esp32c6_asm_dma_probe_state_.error_intr_mask = HCP2BRIDGE_C6_ASM_PROBE_ERROR_INTR_MASK;
  this->esp32c6_asm_dma_probe_state_.uhci_base_addr = DR_REG_UHCI0_BASE;
  this->esp32c6_asm_dma_probe_state_.gdma_base_addr = DR_REG_GDMA_BASE;

  err = esp_intr_alloc_intrstatus(uart_periph_signal[this->uart_num_].irq, ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL4,
                                  this->esp32c6_asm_dma_probe_state_.uart_int_st_addr,
                                  HCP2BRIDGE_C6_ASM_PROBE_RX_INTR_MASK, hcp2_c6_asm_dma_probe_isr,
                                  &this->esp32c6_asm_dma_probe_state_, &this->esp32c6_asm_dma_probe_intr_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_intr_alloc_intrstatus level-3 UART probe failed: %s", esp_err_to_name(err));
    return false;
  }

  uart_ll_ena_intr_mask(hw, HCP2BRIDGE_C6_ASM_PROBE_RX_INTR_MASK);
  this->uart_ready_ = true;
  this->backend_ready_ = true;
  this->protocol_log_append_control_("esp32c6_hp_asm_dma_probe_ready");
  ESP_LOGW(TAG, "ESP32-C6 HP asm/DMA probe is enabled for bench latency experiments only");
  return true;
#else
  ESP_LOGE(TAG, "ESP32-C6 asm/DMA probe backend requires an ESP32-C6 build");
  this->backend_ready_ = false;
  return false;
#endif
}

void HCP2Bridge::start_esp32c6_asm_dma_probe_task_() {
#ifdef USE_ESP32_VARIANT_ESP32C6
  if (this->esp32c6_asm_dma_probe_task_handle_ != nullptr) {
    return;
  }
  const BaseType_t ok = xTaskCreatePinnedToCore(HCP2Bridge::esp32c6_asm_dma_probe_task_trampoline_,
                                                "hcp2_c6_asm_probe", HCP2BRIDGE_REALTIME_TASK_STACK_BYTES, this, 4,
                                                &this->esp32c6_asm_dma_probe_task_handle_, tskNO_AFFINITY);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to start HCP2 ESP32-C6 asm/DMA probe task");
    this->esp32c6_asm_dma_probe_task_handle_ = nullptr;
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "Started HCP2 ESP32-C6 asm/DMA probe task stack=%u bytes",
           (unsigned int) HCP2BRIDGE_REALTIME_TASK_STACK_BYTES);
#endif
}

void HCP2Bridge::esp32c6_asm_dma_probe_task_trampoline_(void *arg) {
#ifdef USE_ESP32_VARIANT_ESP32C6
  auto *self = static_cast<HCP2Bridge *>(arg);
  if (self == nullptr) {
    vTaskDelete(nullptr);
    return;
  }
  self->esp32c6_asm_dma_probe_task_loop_();
#else
  (void) arg;
  vTaskDelete(nullptr);
#endif
}

void HCP2Bridge::esp32c6_asm_dma_probe_task_loop_() {
#ifdef USE_ESP32_VARIANT_ESP32C6
  uint32_t last_logged_irq_count = 0u;
  for (;;) {
    const uint32_t irq_count = this->esp32c6_asm_dma_probe_state_.irq_count;
    portENTER_CRITICAL(&this->state_mux_);
    this->lp_heartbeat_++;
    this->lp_max_rx_fifo_count_ = this->esp32c6_asm_dma_probe_state_.max_fifo_count;
    this->lp_rx_error_count_ = this->esp32c6_asm_dma_probe_state_.error_status_count;
    this->rx_errors_ = this->esp32c6_asm_dma_probe_state_.error_status_count;
    portEXIT_CRITICAL(&this->state_mux_);

    const uint32_t now_ms = millis();
    if (irq_count != last_logged_irq_count && (now_ms - this->lp_last_health_log_ms_) >= HCP2BRIDGE_LP_HEALTH_LOG_INTERVAL_MS) {
      this->lp_last_health_log_ms_ = now_ms;
      last_logged_irq_count = irq_count;
      ESP_LOGI(TAG, "asm/DMA probe irq=%" PRIu32 " rx_irq=%" PRIu32 " drained=%" PRIu32
                    " max_fifo=%" PRIu32 " max_gap_cycles=%" PRIu32 " errors=%" PRIu32,
               irq_count, this->esp32c6_asm_dma_probe_state_.rx_irq_count,
               this->esp32c6_asm_dma_probe_state_.drained_bytes,
               this->esp32c6_asm_dma_probe_state_.max_fifo_count,
               this->esp32c6_asm_dma_probe_state_.max_irq_gap_cycles,
               this->esp32c6_asm_dma_probe_state_.error_status_count);
    }
    vTaskDelay(pdMS_TO_TICKS(HCP2BRIDGE_C6_ASM_PROBE_TASK_TICK_MS));
  }
#else
  vTaskDelete(nullptr);
#endif
}

}  // namespace hcp2bridge
}  // namespace esphome

#endif
