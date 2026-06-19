#include "hcp2bridge.h"
#include "hcp2bridge_internal.h"

#if defined(USE_ESP32) && defined(USE_ESP32_VARIANT_ESP32C6) && defined(HCP2_EMBED_LP_BLOB)

#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "hal/uart_types.h"
#include "lp_core_uart.h"
#include "ulp_lp_core.h"

#include "hcp2_lp_blob.h"

namespace esphome {
namespace hcp2bridge {

static const char *const TAG = "hcp2bridge";

uint32_t HCP2Bridge::fresh_epoch_() const {
  uint32_t epoch = esp_random();
  if (epoch == 0u) {
    epoch = (uint32_t) esp_timer_get_time() ^ 0xC6000001u;
  }
  return epoch;
}

bool HCP2Bridge::setup_lp_core_() {
  if (this->rx_pin_ == nullptr || this->tx_pin_ == nullptr || this->de_pin_ == nullptr) {
    ESP_LOGE(TAG, "rx_pin, tx_pin, and de_pin are required");
    return false;
  }
  if (this->re_pin_ == nullptr) {
    ESP_LOGE(TAG, "re_pin is required for LP RS-485 mode");
    return false;
  }
  if (this->rx_pin_->get_pin() != 4 || this->tx_pin_->get_pin() != 5) {
    ESP_LOGE(TAG, "ESP32-C6 LP-UART requires rx_pin GPIO4 and tx_pin GPIO5");
    return false;
  }
  if (this->de_pin_->get_pin() != 0 || this->re_pin_->get_pin() != 1) {
    ESP_LOGE(TAG, "HCP2 LP firmware requires de_pin GPIO0 and re_pin GPIO1");
    return false;
  }

  hcp2_hp_supervisor_init(&this->lp_supervisor_, (volatile hcp2_lp_mailbox_t *) HCP2_LP_MAILBOX_ADDR,
                          HCP2_LP_FIRMWARE_VERSION);
  const esp_err_t err = this->start_or_skip_lp_();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "LP-core setup failed: %s", esp_err_to_name(err));
    return false;
  }
  this->backend_ready_ = true;
  ESP_LOGI(TAG, "Started HCP2 LP supervisor mailbox=0x%08x", (unsigned) HCP2_LP_MAILBOX_ADDR);
  return true;
}

esp_err_t HCP2Bridge::init_lp_bus_io_() {
  const gpio_num_t rx_gpio = static_cast<gpio_num_t>(this->rx_pin_->get_pin());
  const gpio_num_t tx_gpio = static_cast<gpio_num_t>(this->tx_pin_->get_pin());
  const gpio_num_t de_gpio = static_cast<gpio_num_t>(this->de_pin_->get_pin());
  const gpio_num_t re_gpio = static_cast<gpio_num_t>(this->re_pin_->get_pin());
  lp_core_uart_cfg_t uart_cfg{};

  uart_cfg.uart_proto_cfg.baud_rate = HCP2BRIDGE_BAUD_RATE;
  uart_cfg.uart_proto_cfg.data_bits = UART_DATA_8_BITS;
  uart_cfg.uart_proto_cfg.parity = UART_PARITY_EVEN;
  uart_cfg.uart_proto_cfg.stop_bits = UART_STOP_BITS_1;
  uart_cfg.uart_proto_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_cfg.uart_proto_cfg.rx_flow_ctrl_thresh = 0;
  uart_cfg.uart_pin_cfg.tx_io_num = tx_gpio;
  uart_cfg.uart_pin_cfg.rx_io_num = rx_gpio;
  uart_cfg.uart_pin_cfg.rts_io_num = GPIO_NUM_NC;
  uart_cfg.uart_pin_cfg.cts_io_num = GPIO_NUM_NC;
  uart_cfg.lp_uart_source_clk =
      this->lp_uart_clock_source_default_ ? LP_UART_SCLK_DEFAULT : LP_UART_SCLK_XTAL_D2;

  esp_err_t err = rtc_gpio_init(de_gpio);
  if (err != ESP_OK) return err;
  err = rtc_gpio_set_direction(de_gpio, RTC_GPIO_MODE_OUTPUT_ONLY);
  if (err != ESP_OK) return err;
  err = rtc_gpio_set_level(de_gpio, 0);
  if (err != ESP_OK) return err;
  err = rtc_gpio_pulldown_en(de_gpio);
  if (err != ESP_OK) return err;

  err = rtc_gpio_init(re_gpio);
  if (err != ESP_OK) return err;
  err = rtc_gpio_set_direction(re_gpio, RTC_GPIO_MODE_OUTPUT_ONLY);
  if (err != ESP_OK) return err;
  err = rtc_gpio_set_level(re_gpio, 0);
  if (err != ESP_OK) return err;
  err = rtc_gpio_pulldown_en(re_gpio);
  if (err != ESP_OK) return err;

  err = gpio_set_pull_mode(tx_gpio, GPIO_PULLUP_ONLY);
  if (err != ESP_OK) return err;
  return lp_core_uart_init(&uart_cfg);
}

void HCP2Bridge::write_lp_config_() {
  volatile hcp2_lp_mailbox_t *mailbox = (volatile hcp2_lp_mailbox_t *) HCP2_LP_MAILBOX_ADDR;
  hcp2_lp_mailbox_write_config(mailbox, &this->config_);
}

hcp2_lp_reload_decision_t HCP2Bridge::probe_lp_health_(hcp2_lp_health_sample_t *before,
                                                       hcp2_lp_health_sample_t *after) {
  hcp2_lp_health_sample_t before_local;
  hcp2_lp_health_sample_t after_local;

  hcp2_hp_supervisor_sample_health(&this->lp_supervisor_, &before_local);
  vTaskDelay(pdMS_TO_TICKS(HCP2BRIDGE_LP_HEARTBEAT_PROBE_MS));
  hcp2_hp_supervisor_sample_health(&this->lp_supervisor_, &after_local);
  if (before != nullptr) {
    *before = before_local;
  }
  if (after != nullptr) {
    *after = after_local;
  }
  return hcp2_hp_supervisor_reload_decision(&this->lp_supervisor_, &before_local, &after_local);
}

bool HCP2Bridge::healthy_lp_running_(hcp2_lp_health_sample_t *before, hcp2_lp_health_sample_t *after) {
  return this->probe_lp_health_(before, after) == HCP2_LP_RELOAD_SKIP;
}

esp_err_t HCP2Bridge::load_and_start_lp_() {
  volatile hcp2_lp_mailbox_t *mailbox = (volatile hcp2_lp_mailbox_t *) HCP2_LP_MAILBOX_ADDR;
  ulp_lp_core_cfg_t cfg = {
      .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU,
  };

  ulp_lp_core_stop();
  esp_err_t err = this->init_lp_bus_io_();
  if (err != ESP_OK) return err;
  err = ulp_lp_core_load_binary((const uint8_t *) hcp2_lp_blob_data, (size_t) hcp2_lp_blob_data_len);
  if (err != ESP_OK) return err;
  hcp2_lp_mailbox_init(mailbox);
  this->write_lp_config_();
  hcp2_hp_supervisor_begin_session(&this->lp_supervisor_, this->fresh_epoch_());
  err = ulp_lp_core_run(&cfg);
  if (err != ESP_OK) return err;
  ESP_LOGI(TAG, "HCP2_LP_LOAD_RELOAD bytes=%u epoch=%" PRIu32, (unsigned) hcp2_lp_blob_data_len,
           this->lp_supervisor_.epoch);
  return ESP_OK;
}

esp_err_t HCP2Bridge::start_or_skip_lp_() {
  hcp2_lp_health_sample_t before;
  hcp2_lp_health_sample_t after;
  this->write_lp_config_();
  const hcp2_lp_reload_decision_t decision = this->probe_lp_health_(&before, &after);
  if (decision == HCP2_LP_RELOAD_SKIP) {
    hcp2_hp_supervisor_begin_session(&this->lp_supervisor_, this->fresh_epoch_());
    this->write_lp_config_();
    ESP_LOGI(TAG,
             "HCP2_LP_SKIP_RELOAD heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32
             " polls_seen=%" PRIu32 " polls_answered=%" PRIu32 " epoch=%" PRIu32,
             before.heartbeat, after.heartbeat, after.polls_seen, after.polls_answered,
             this->lp_supervisor_.epoch);
    return ESP_OK;
  }
  if (decision == HCP2_LP_RELOAD_DEFER) {
    hcp2_hp_supervisor_begin_session(&this->lp_supervisor_, this->fresh_epoch_());
    this->write_lp_config_();
    ESP_LOGW(TAG,
             "HCP2_LP_RELOAD_DEFER heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32
             " state=0x%02" PRIx8 " command=%" PRIu32 "/%" PRIu32 " epoch=%" PRIu32,
             before.heartbeat, after.heartbeat, after.drive_state, after.command_ack_sequence,
             after.command_sequence, this->lp_supervisor_.epoch);
    return ESP_OK;
  }

  ESP_LOGI(TAG, "HCP2_LP_RELOAD_REQUIRED heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32,
           before.heartbeat, after.heartbeat);
  return this->load_and_start_lp_();
}

}  // namespace hcp2bridge
}  // namespace esphome

#elif defined(USE_ESP32)

namespace esphome {
namespace hcp2bridge {

static const char *const TAG = "hcp2bridge";

uint32_t HCP2Bridge::fresh_epoch_() const { return 0u; }

bool HCP2Bridge::setup_lp_core_() {
  ESP_LOGE(TAG, "ESP32-C6 LP backend is not available on this ESP32 variant");
  this->backend_ready_ = false;
  return false;
}

esp_err_t HCP2Bridge::init_lp_bus_io_() { return ESP_FAIL; }

void HCP2Bridge::write_lp_config_() {}

esp_err_t HCP2Bridge::load_and_start_lp_() { return ESP_FAIL; }

esp_err_t HCP2Bridge::start_or_skip_lp_() { return ESP_FAIL; }

hcp2_lp_reload_decision_t HCP2Bridge::probe_lp_health_(hcp2_lp_health_sample_t *before,
                                                       hcp2_lp_health_sample_t *after) {
  if (before != nullptr) {
    *before = {};
  }
  if (after != nullptr) {
    *after = {};
  }
  return HCP2_LP_RELOAD_REQUIRED;
}

bool HCP2Bridge::healthy_lp_running_(hcp2_lp_health_sample_t *before, hcp2_lp_health_sample_t *after) {
  (void) before;
  (void) after;
  return false;
}

}  // namespace hcp2bridge
}  // namespace esphome

#endif
