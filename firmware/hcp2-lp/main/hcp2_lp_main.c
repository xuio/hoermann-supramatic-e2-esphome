#include <stdint.h>

#include "driver/rtc_io.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_types.h"
#include "hal/uart_types.h"
#include "hcp2_lp.h"
#include "hcp2_mailbox.h"
#include "lp_core_uart.h"
#include "ulp_lp_core.h"

#define HCP2_LP_DE_GPIO GPIO_NUM_0
#define HCP2_LP_RE_GPIO GPIO_NUM_1

static const char *TAG = "hcp2-lp";

extern const uint8_t hcp2_lp_bin_start[] asm("_binary_hcp2_lp_bin_start");
extern const uint8_t hcp2_lp_bin_end[] asm("_binary_hcp2_lp_bin_end");

static volatile hcp2_lp_mailbox_t *mailbox_(void) {
  return (volatile hcp2_lp_mailbox_t *) HCP2_LP_MAILBOX_ADDR;
}

static esp_err_t init_bus_io_(void) {
  lp_core_uart_cfg_t uart_cfg = LP_CORE_UART_DEFAULT_CONFIG();

  uart_cfg.uart_proto_cfg.baud_rate = 57600;
  uart_cfg.uart_proto_cfg.data_bits = UART_DATA_8_BITS;
  uart_cfg.uart_proto_cfg.parity = UART_PARITY_EVEN;
  uart_cfg.uart_proto_cfg.stop_bits = UART_STOP_BITS_1;
  uart_cfg.uart_proto_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_cfg.uart_proto_cfg.rx_flow_ctrl_thresh = 0;
  uart_cfg.uart_pin_cfg.tx_io_num = GPIO_NUM_5;
  uart_cfg.uart_pin_cfg.rx_io_num = GPIO_NUM_4;
  uart_cfg.uart_pin_cfg.rts_io_num = -1;
  uart_cfg.uart_pin_cfg.cts_io_num = -1;

  ESP_RETURN_ON_ERROR(rtc_gpio_init(HCP2_LP_DE_GPIO), TAG, "init DE GPIO");
  ESP_RETURN_ON_ERROR(rtc_gpio_set_direction(HCP2_LP_DE_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY), TAG, "set DE output");
  ESP_RETURN_ON_ERROR(rtc_gpio_set_level(HCP2_LP_DE_GPIO, 0), TAG, "drive DE low");
  ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_en(HCP2_LP_DE_GPIO), TAG, "enable DE pulldown");
  ESP_RETURN_ON_ERROR(rtc_gpio_init(HCP2_LP_RE_GPIO), TAG, "init /RE GPIO");
  ESP_RETURN_ON_ERROR(rtc_gpio_set_direction(HCP2_LP_RE_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY), TAG, "set /RE output");
  ESP_RETURN_ON_ERROR(rtc_gpio_set_level(HCP2_LP_RE_GPIO, 0), TAG, "drive /RE low");
  ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_en(HCP2_LP_RE_GPIO), TAG, "enable /RE pulldown");
  ESP_RETURN_ON_ERROR(lp_core_uart_init(&uart_cfg), TAG, "init LP UART");
  return ESP_OK;
}

static bool healthy_lp_running_(void) {
  volatile hcp2_lp_mailbox_t *mailbox = mailbox_();
  hcp2_lp_health_sample_t before;
  hcp2_lp_health_sample_t after;

  hcp2_lp_mailbox_sample_health(mailbox, &before);
  vTaskDelay(pdMS_TO_TICKS(20));
  hcp2_lp_mailbox_sample_health(mailbox, &after);

  return hcp2_lp_mailbox_reload_decision(mailbox, HCP2_LP_FIRMWARE_VERSION, &before, &after) ==
         HCP2_LP_RELOAD_SKIP;
}

static esp_err_t load_and_start_lp_(void) {
  volatile hcp2_lp_mailbox_t *mailbox = mailbox_();
  const size_t blob_size = (size_t) (hcp2_lp_bin_end - hcp2_lp_bin_start);
  ulp_lp_core_cfg_t cfg = {
      .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU,
  };

  ulp_lp_core_stop();
  ESP_RETURN_ON_ERROR(init_bus_io_(), TAG, "bus IO init failed");
  ESP_RETURN_ON_ERROR(ulp_lp_core_load_binary(hcp2_lp_bin_start, blob_size), TAG, "LP binary load failed");
  hcp2_lp_mailbox_init(mailbox);
  ESP_RETURN_ON_ERROR(ulp_lp_core_run(&cfg), TAG, "LP core start failed");
  ESP_LOGI(TAG, "loaded hcp2_lp blob (%u bytes), mailbox=0x%08x", (unsigned) blob_size,
           (unsigned) HCP2_LP_MAILBOX_ADDR);
  return ESP_OK;
}

void app_main(void) {
  if (healthy_lp_running_()) {
    ESP_LOGI(TAG, "healthy hcp2_lp already running; skipping reload");
    return;
  }

  ESP_ERROR_CHECK(load_and_start_lp_());
}
