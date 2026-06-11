#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/rtc_io.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_types.h"
#include "hal/uart_types.h"
#include "hcp2_lp.h"
#include "nvs_flash.h"
#include "lp_core_uart.h"
#include "ulp_lp_core.h"

#include "hcp2_engine.h"
#include "hcp2_mailbox.h"
#include "hcp2_port.h"
#include "hcp2_supervisor.h"

#define HCP2_BRINGUP_UART UART_NUM_1
#define HCP2_BRINGUP_UART_RX GPIO_NUM_4
#define HCP2_BRINGUP_UART_TX GPIO_NUM_5
#define HCP2_BRINGUP_DE GPIO_NUM_6
#define HCP2_BRINGUP_BAUD 57600
#define HCP2_BRINGUP_RX_BUF 256
#define HCP2_BRINGUP_TX_BUF 256
#define HCP2_BRINGUP_HEARTBEAT_PROBE_MS 20
#define HCP2_BRINGUP_MAILBOX_TIMEOUT_MS 250

static const char *TAG = "hcp2_bringup";

extern const uint8_t hcp2_lp_bin_start[] asm("_binary_hcp2_lp_bin_start");
extern const uint8_t hcp2_lp_bin_end[] asm("_binary_hcp2_lp_bin_end");

typedef struct {
  hcp2_engine_t engine;
} hcp2_bringup_ctx_t;

#if CONFIG_HCP2_BRINGUP_HP_FALLBACK
static hcp2_bringup_ctx_t s_ctx;
#endif

#if CONFIG_HCP2_BRINGUP_MAILBOX_TEST_DOUBLE && CONFIG_HCP2_BRINGUP_HP_FALLBACK
static volatile hcp2_lp_mailbox_t s_mailbox_double;
#endif

static volatile hcp2_lp_mailbox_t *lp_mailbox_(void) {
  return (volatile hcp2_lp_mailbox_t *) HCP2_LP_MAILBOX_ADDR;
}

#if CONFIG_HCP2_BRINGUP_LP_IN_LOOP
static hcp2_hp_supervisor_t s_lp_supervisor;
#endif

#if CONFIG_HCP2_BRINGUP_HP_FALLBACK
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
#endif

#if CONFIG_HCP2_BRINGUP_MAILBOX_TEST_DOUBLE && CONFIG_HCP2_BRINGUP_HP_FALLBACK
static bool run_mailbox_test_double(void) {
  uint32_t heartbeat_before;
  uint32_t heartbeat_after;
  uint32_t last_sequence = 0;
  hcp2_lp_command_t command;
  hcp2_hp_supervisor_t supervisor;
  uint32_t sequence;

  hcp2_lp_mailbox_init(&s_mailbox_double);
  hcp2_hp_supervisor_init(&supervisor, &s_mailbox_double, HCP2_LP_FIRMWARE_VERSION);
  heartbeat_before = s_mailbox_double.heartbeat;
  s_mailbox_double.heartbeat = heartbeat_before + 1u;
  heartbeat_after = s_mailbox_double.heartbeat;

  if (!hcp2_hp_supervisor_is_healthy(&supervisor, heartbeat_before, heartbeat_after)) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_SKIP_RELOAD_FAIL");
    return false;
  }
  ESP_LOGI(TAG, "HCP2_SUPERVISOR_SKIP_RELOAD_OK heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32,
           heartbeat_before, heartbeat_after);

  if (hcp2_hp_supervisor_reload_decision(&supervisor, heartbeat_after, heartbeat_after) !=
      HCP2_LP_RELOAD_REQUIRED) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_STALE_HEARTBEAT_FAIL");
    return false;
  }
  ESP_LOGI(TAG, "HCP2_SUPERVISOR_STALE_HEARTBEAT_OK");

  hcp2_hp_supervisor_begin_session(&supervisor, 0x22220000u);
  hcp2_lp_mailbox_send_command(&s_mailbox_double, 0x11110000u, 1u, HCP2_LP_COMMAND_OPEN, 0u);
  if (hcp2_lp_mailbox_take_command(&s_mailbox_double, 0x22220000u, &last_sequence, &command)) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_EPOCH_REPLAY_FAIL");
    return false;
  }

  sequence = hcp2_hp_supervisor_send_command(&supervisor, HCP2_LP_COMMAND_CLOSE, 0u);
  if (!hcp2_lp_mailbox_take_command(&s_mailbox_double, 0x22220000u, &last_sequence, &command) ||
      command.command_id != HCP2_LP_COMMAND_CLOSE || !hcp2_hp_supervisor_ack_received(&supervisor, sequence)) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_EPOCH_ACK_FAIL");
    return false;
  }
  ESP_LOGI(TAG, "HCP2_SUPERVISOR_EPOCH_OK epoch=%" PRIu32 " sequence=%" PRIu32, command.epoch,
           command.sequence);
  return true;
}
#endif

#if CONFIG_HCP2_BRINGUP_HP_FALLBACK
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
#endif

#if CONFIG_HCP2_BRINGUP_LP_IN_LOOP
static uint32_t fresh_epoch_(void) {
  uint32_t epoch = esp_random();

  if (epoch == 0u) {
    epoch = (uint32_t) esp_timer_get_time() ^ 0xC6000001u;
  }
  return epoch;
}

static esp_err_t init_lp_bus_io_(void) {
  lp_core_uart_cfg_t uart_cfg = LP_CORE_UART_DEFAULT_CONFIG();

  uart_cfg.uart_proto_cfg.baud_rate = HCP2_BRINGUP_BAUD;
  uart_cfg.uart_proto_cfg.data_bits = UART_DATA_8_BITS;
  uart_cfg.uart_proto_cfg.parity = UART_PARITY_EVEN;
  uart_cfg.uart_proto_cfg.stop_bits = UART_STOP_BITS_1;
  uart_cfg.uart_proto_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_cfg.uart_proto_cfg.rx_flow_ctrl_thresh = 0;
  uart_cfg.uart_pin_cfg.tx_io_num = HCP2_BRINGUP_UART_TX;
  uart_cfg.uart_pin_cfg.rx_io_num = HCP2_BRINGUP_UART_RX;
  uart_cfg.uart_pin_cfg.rts_io_num = -1;
  uart_cfg.uart_pin_cfg.cts_io_num = -1;

  ESP_RETURN_ON_ERROR(rtc_gpio_init(HCP2_BRINGUP_DE), TAG, "init LP DE GPIO");
  ESP_RETURN_ON_ERROR(rtc_gpio_set_direction(HCP2_BRINGUP_DE, RTC_GPIO_MODE_OUTPUT_ONLY), TAG,
                      "set LP DE output");
  ESP_RETURN_ON_ERROR(rtc_gpio_set_level(HCP2_BRINGUP_DE, 0), TAG, "drive LP DE low");
  ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_en(HCP2_BRINGUP_DE), TAG, "enable LP DE pulldown");

#if CONFIG_HCP2_BRINGUP_WOKWI_LP_UART_BYPASS
  ESP_LOGW(TAG, "HCP2_WOKWI_LP_UART_BYPASS");
  ESP_RETURN_ON_ERROR(rtc_gpio_init(HCP2_BRINGUP_UART_TX), TAG, "init LP UART TX GPIO");
  ESP_RETURN_ON_ERROR(rtc_gpio_set_direction(HCP2_BRINGUP_UART_TX, RTC_GPIO_MODE_OUTPUT_ONLY), TAG,
                      "set LP UART TX output");
  ESP_RETURN_ON_ERROR(rtc_gpio_init(HCP2_BRINGUP_UART_RX), TAG, "init LP UART RX GPIO");
  ESP_RETURN_ON_ERROR(rtc_gpio_set_direction(HCP2_BRINGUP_UART_RX, RTC_GPIO_MODE_INPUT_ONLY), TAG,
                      "set LP UART RX input");
  return ESP_OK;
#else
  ESP_RETURN_ON_ERROR(lp_core_uart_init(&uart_cfg), TAG, "init LP UART");
  return ESP_OK;
#endif
}

static bool healthy_lp_running_(uint32_t *heartbeat_before, uint32_t *heartbeat_after) {
  volatile hcp2_lp_mailbox_t *mailbox = lp_mailbox_();
  const uint32_t before = mailbox->heartbeat;
  uint32_t after;

  vTaskDelay(pdMS_TO_TICKS(HCP2_BRINGUP_HEARTBEAT_PROBE_MS));
  after = mailbox->heartbeat;
  if (heartbeat_before != NULL) {
    *heartbeat_before = before;
  }
  if (heartbeat_after != NULL) {
    *heartbeat_after = after;
  }
  return hcp2_hp_supervisor_is_healthy(&s_lp_supervisor, before, after);
}

static esp_err_t load_and_start_lp_(void) {
  volatile hcp2_lp_mailbox_t *mailbox = lp_mailbox_();
  const size_t blob_size = (size_t) (hcp2_lp_bin_end - hcp2_lp_bin_start);
  ulp_lp_core_cfg_t cfg = {
      .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU,
  };

  ulp_lp_core_stop();
  ESP_RETURN_ON_ERROR(init_lp_bus_io_(), TAG, "LP bus IO init failed");
  ESP_RETURN_ON_ERROR(ulp_lp_core_load_binary(hcp2_lp_bin_start, blob_size), TAG, "LP binary load failed");
  hcp2_lp_mailbox_init(mailbox);
  hcp2_hp_supervisor_begin_session(&s_lp_supervisor, fresh_epoch_());
  ESP_RETURN_ON_ERROR(ulp_lp_core_run(&cfg), TAG, "LP core start failed");
  ESP_LOGI(TAG, "HCP2_LP_LOAD_RELOAD bytes=%u mailbox=0x%08x epoch=%" PRIu32, (unsigned) blob_size,
           (unsigned) HCP2_LP_MAILBOX_ADDR, s_lp_supervisor.epoch);
  return ESP_OK;
}

static esp_err_t start_or_skip_lp_(void) {
  uint32_t heartbeat_before = 0;
  uint32_t heartbeat_after = 0;

  if (healthy_lp_running_(&heartbeat_before, &heartbeat_after)) {
    ESP_LOGI(TAG, "HCP2_LP_SKIP_RELOAD heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32,
             heartbeat_before, heartbeat_after);
    return ESP_OK;
  }

  ESP_LOGI(TAG, "HCP2_LP_RELOAD_REQUIRED heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32,
           heartbeat_before, heartbeat_after);
  return load_and_start_lp_();
}

static bool wait_for_ack_(uint32_t sequence) {
  const int attempts = HCP2_BRINGUP_MAILBOX_TIMEOUT_MS / HCP2_BRINGUP_HEARTBEAT_PROBE_MS;

  for (int i = 0; i < attempts; i++) {
    if (hcp2_hp_supervisor_ack_received(&s_lp_supervisor, sequence)) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(HCP2_BRINGUP_HEARTBEAT_PROBE_MS));
  }
  return hcp2_hp_supervisor_ack_received(&s_lp_supervisor, sequence);
}

static bool verify_real_mailbox_(void) {
  volatile hcp2_lp_mailbox_t *mailbox = lp_mailbox_();
  uint32_t heartbeat_before = 0;
  uint32_t heartbeat_after = 0;
  const uint32_t epoch = fresh_epoch_();
  const uint32_t stale_epoch = epoch ^ 0xA5A55A5Au;
  const int health_attempts = HCP2_BRINGUP_MAILBOX_TIMEOUT_MS / HCP2_BRINGUP_HEARTBEAT_PROBE_MS;
  bool healthy = false;

  for (int i = 0; i < health_attempts; i++) {
    if (healthy_lp_running_(&heartbeat_before, &heartbeat_after)) {
      healthy = true;
      break;
    }
  }

  if (!healthy) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_REAL_SKIP_RELOAD_FAIL heartbeat_before=%" PRIu32
                  " heartbeat_after=%" PRIu32,
             heartbeat_before, heartbeat_after);
    return false;
  }
  ESP_LOGI(TAG, "HCP2_SUPERVISOR_REAL_SKIP_RELOAD_OK heartbeat_before=%" PRIu32
                " heartbeat_after=%" PRIu32,
           heartbeat_before, heartbeat_after);

  hcp2_hp_supervisor_begin_session(&s_lp_supervisor, epoch);
  vTaskDelay(pdMS_TO_TICKS(HCP2_BRINGUP_HEARTBEAT_PROBE_MS * 2));

  hcp2_lp_mailbox_send_command(mailbox, stale_epoch, 1u, HCP2_LP_COMMAND_OPEN, 0u);
  vTaskDelay(pdMS_TO_TICKS(HCP2_BRINGUP_HEARTBEAT_PROBE_MS * 2));
  if (mailbox->command_ack_sequence != 0u) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_REAL_EPOCH_REPLAY_FAIL ack=%" PRIu32, mailbox->command_ack_sequence);
    return false;
  }

  const uint32_t sequence = hcp2_hp_supervisor_send_command(&s_lp_supervisor, HCP2_LP_COMMAND_STOP, 0u);
  if (!wait_for_ack_(sequence)) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_REAL_EPOCH_ACK_FAIL ack=%" PRIu32, mailbox->command_ack_sequence);
    return false;
  }

  ESP_LOGI(TAG, "HCP2_SUPERVISOR_REAL_EPOCH_OK epoch=%" PRIu32 " sequence=%" PRIu32, epoch, sequence);
  ESP_LOGI(TAG, "HCP2_SUPERVISOR_REAL_MAILBOX_OK");
  return true;
}

static void lp_supervisor_task(void *arg) {
  (void) arg;
  if (!verify_real_mailbox_()) {
#if CONFIG_HCP2_BRINGUP_WOKWI_LP_UART_BYPASS
    ESP_LOGW(TAG, "HCP2_WOKWI_LP_EXEC_UNSUPPORTED");
#else
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_REAL_MAILBOX_FAIL");
#endif
  }

  for (;;) {
    uint32_t heartbeat_before = 0;
    uint32_t heartbeat_after = 0;
    if (!healthy_lp_running_(&heartbeat_before, &heartbeat_after)) {
      ESP_LOGE(TAG, "HCP2_LP_HEARTBEAT_STALE heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32,
               heartbeat_before, heartbeat_after);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
#endif

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

#if CONFIG_HCP2_BRINGUP_MAILBOX_TEST_DOUBLE && CONFIG_HCP2_BRINGUP_HP_FALLBACK
  if (!run_mailbox_test_double()) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_TEST_DOUBLE_FAIL");
  }
#endif

#if CONFIG_HCP2_BRINGUP_LP_IN_LOOP
  ESP_LOGI(TAG, "HCP2_BRINGUP_MODE lp-in-loop");
  hcp2_hp_supervisor_init(&s_lp_supervisor, lp_mailbox_(), HCP2_LP_FIRMWARE_VERSION);
  ESP_ERROR_CHECK(start_or_skip_lp_());
  xTaskCreate(lp_supervisor_task, "hcp2_lp_supervisor", 4096, NULL, 5, NULL);
#else
  ESP_LOGI(TAG, "HCP2_BRINGUP_MODE hp-fallback");
  init_uart();
  xTaskCreate(responder_task, "hcp2_responder", 4096, &s_ctx, configMAX_PRIORITIES - 1, NULL);
#endif

#if CONFIG_HCP2_BRINGUP_RESTART_INTERVAL_MS > 0
  xTaskCreate(restart_task, "hcp2_restart", 2048, NULL, 1, NULL);
#endif
}
