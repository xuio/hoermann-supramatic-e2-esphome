#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "hcp2_engine.h"
#include "hcp2_mailbox.h"
#include "hcp2_port.h"

#define HCP2_BRINGUP_UART UART_NUM_1
#define HCP2_BRINGUP_UART_RX GPIO_NUM_4
#define HCP2_BRINGUP_UART_TX GPIO_NUM_5
#define HCP2_BRINGUP_DE GPIO_NUM_6
#define HCP2_BRINGUP_BAUD 57600
#define HCP2_BRINGUP_RX_BUF 256
#define HCP2_BRINGUP_TX_BUF 256

static const char *TAG = "hcp2_bringup";

typedef struct {
  hcp2_engine_t engine;
} hcp2_bringup_ctx_t;

static hcp2_bringup_ctx_t s_ctx;
static volatile hcp2_lp_mailbox_t s_mailbox_double;

static uint32_t now_us_cb(void *user) {
  (void) user;
  return (uint32_t) esp_timer_get_time();
}

static void de_set_cb(void *user, uint8_t enabled) {
  (void) user;
  gpio_set_level(HCP2_BRINGUP_DE, enabled ? 1 : 0);
}

static void tx_cb(void *user, const uint8_t *data, uint8_t len) {
  uint8_t written_total = 0;

  (void) user;
  while (written_total < len) {
    const int written =
        uart_write_bytes(HCP2_BRINGUP_UART, (const char *) (data + written_total), len - written_total);
    if (written > 0) {
      written_total = (uint8_t) (written_total + written);
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  ESP_ERROR_CHECK(uart_wait_tx_done(HCP2_BRINGUP_UART, pdMS_TO_TICKS(20)));
}

static void init_uart(void) {
  const uart_config_t uart_config = {
      .baud_rate = HCP2_BRINGUP_BAUD,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_EVEN,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  const gpio_config_t de_config = {
      .pin_bit_mask = 1ULL << HCP2_BRINGUP_DE,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_ENABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };

  ESP_ERROR_CHECK(gpio_config(&de_config));
  gpio_set_level(HCP2_BRINGUP_DE, 0);
  ESP_ERROR_CHECK(uart_driver_install(HCP2_BRINGUP_UART, HCP2_BRINGUP_RX_BUF, HCP2_BRINGUP_TX_BUF, 0, NULL, 0));
  ESP_ERROR_CHECK(uart_param_config(HCP2_BRINGUP_UART, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(HCP2_BRINGUP_UART, HCP2_BRINGUP_UART_TX, HCP2_BRINGUP_UART_RX, UART_PIN_NO_CHANGE,
                               UART_PIN_NO_CHANGE));
}

static bool run_mailbox_test_double(void) {
  uint32_t heartbeat_before;
  uint32_t heartbeat_after;
  uint32_t last_sequence = 0;
  hcp2_lp_command_t command;

  hcp2_lp_mailbox_init(&s_mailbox_double);
  heartbeat_before = s_mailbox_double.heartbeat;
  s_mailbox_double.heartbeat = heartbeat_before + 1u;
  heartbeat_after = s_mailbox_double.heartbeat;

  if (hcp2_lp_mailbox_reload_decision(&s_mailbox_double, HCP2_LP_FIRMWARE_VERSION, heartbeat_before,
                                      heartbeat_after) != HCP2_LP_RELOAD_SKIP) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_SKIP_RELOAD_FAIL");
    return false;
  }
  ESP_LOGI(TAG, "HCP2_SUPERVISOR_SKIP_RELOAD_OK heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32,
           heartbeat_before, heartbeat_after);

  if (hcp2_lp_mailbox_reload_decision(&s_mailbox_double, HCP2_LP_FIRMWARE_VERSION, heartbeat_after,
                                      heartbeat_after) != HCP2_LP_RELOAD_REQUIRED) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_STALE_HEARTBEAT_FAIL");
    return false;
  }
  ESP_LOGI(TAG, "HCP2_SUPERVISOR_STALE_HEARTBEAT_OK");

  hcp2_lp_mailbox_send_command(&s_mailbox_double, 0x11110000u, 1u, HCP2_LP_COMMAND_OPEN, 0u);
  if (hcp2_lp_mailbox_take_command(&s_mailbox_double, 0x22220000u, &last_sequence, &command)) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_EPOCH_REPLAY_FAIL");
    return false;
  }

  hcp2_lp_mailbox_send_command(&s_mailbox_double, 0x22220000u, 1u, HCP2_LP_COMMAND_CLOSE, 0u);
  if (!hcp2_lp_mailbox_take_command(&s_mailbox_double, 0x22220000u, &last_sequence, &command) ||
      command.command_id != HCP2_LP_COMMAND_CLOSE || s_mailbox_double.command_ack_sequence != 1u) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_EPOCH_ACK_FAIL");
    return false;
  }
  ESP_LOGI(TAG, "HCP2_SUPERVISOR_EPOCH_OK epoch=%" PRIu32 " sequence=%" PRIu32, command.epoch,
           command.sequence);
  return true;
}

static void responder_task(void *arg) {
  hcp2_bringup_ctx_t *ctx = (hcp2_bringup_ctx_t *) arg;
  hcp2_engine_config_t config;
  const hcp2_port_t port = {
      .user = ctx,
      .now_us = now_us_cb,
      .tx = tx_cb,
      .de_set = de_set_cb,
  };
  uint8_t rx[32];

  hcp2_engine_config_default(&config);
  hcp2_engine_init(&ctx->engine, &port, &config);
  ESP_LOGI(TAG, "HCP2_BRINGUP_READY uart=1 rx_gpio=4 tx_gpio=5 baud=57600 parity=even");

  for (;;) {
    const int got = uart_read_bytes(HCP2_BRINGUP_UART, rx, sizeof(rx), 0);
    for (int i = 0; i < got; i++) {
      hcp2_engine_rx_byte(&ctx->engine, rx[i], HCP2_RX_OK);
    }
    hcp2_engine_poll(&ctx->engine);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

#if CONFIG_HCP2_BRINGUP_RESTART_INTERVAL_MS > 0
static void restart_task(void *arg) {
  (void) arg;
  ESP_LOGI(TAG, "HCP2_RESTART_LOOP interval_ms=%d", CONFIG_HCP2_BRINGUP_RESTART_INTERVAL_MS);
  vTaskDelay(pdMS_TO_TICKS(CONFIG_HCP2_BRINGUP_RESTART_INTERVAL_MS));
  ESP_LOGI(TAG, "HCP2_RESTART_NOW");
  fflush(stdout);
  esp_restart();
}
#endif

void app_main(void) {
  esp_err_t nvs_status;

  nvs_status = nvs_flash_init();
  if (nvs_status == ESP_ERR_NVS_NO_FREE_PAGES || nvs_status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  } else {
    ESP_ERROR_CHECK(nvs_status);
  }

#if CONFIG_HCP2_BRINGUP_MAILBOX_TEST_DOUBLE
  if (!run_mailbox_test_double()) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_TEST_DOUBLE_FAIL");
  }
#endif

  init_uart();
  xTaskCreate(responder_task, "hcp2_responder", 4096, &s_ctx, configMAX_PRIORITIES - 1, NULL);

#if CONFIG_HCP2_BRINGUP_RESTART_INTERVAL_MS > 0
  xTaskCreate(restart_task, "hcp2_restart", 2048, NULL, 1, NULL);
#endif
}
