#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/rtc_io.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/wdt_hal.h"
#include "hal/gpio_types.h"
#include "hal/uart_types.h"
#include "hal/wdt_types.h"
#include "hcp2_lp.h"
#include "nvs_flash.h"
#include "lp_core_uart.h"
#include "soc/rtc.h"
#if CONFIG_HCP2_BRINGUP_LP_UART_STATUS_LOG
#include "soc/lp_uart_reg.h"
#endif
#include "ulp_lp_core.h"

#include "hcp2_engine.h"
#include "hcp2_mailbox.h"
#include "hcp2_port.h"
#include "hcp2_supervisor.h"

#define HCP2_BRINGUP_UART UART_NUM_1
#define HCP2_BRINGUP_UART_RX GPIO_NUM_4
#define HCP2_BRINGUP_UART_TX GPIO_NUM_5
#define HCP2_BRINGUP_DE GPIO_NUM_2
#define HCP2_BRINGUP_RE GPIO_NUM_3
#define HCP2_BRINGUP_BAUD 57600
#define HCP2_BRINGUP_RX_BUF 256
#define HCP2_BRINGUP_TX_BUF 256
#define HCP2_BRINGUP_HEARTBEAT_PROBE_MS 20
#define HCP2_BRINGUP_MAILBOX_TIMEOUT_MS 250
#if CONFIG_HCP2_BRINGUP_WOKWI_UART_NO_PARITY
#define HCP2_BRINGUP_UART_PARITY UART_PARITY_DISABLE
#define HCP2_BRINGUP_UART_PARITY_NAME "none"
#else
#define HCP2_BRINGUP_UART_PARITY UART_PARITY_EVEN
#define HCP2_BRINGUP_UART_PARITY_NAME "even"
#endif

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

#if CONFIG_HCP2_BRINGUP_LP_IN_LOOP
static volatile hcp2_lp_mailbox_t *lp_mailbox_(void) {
  return (volatile hcp2_lp_mailbox_t *) HCP2_LP_MAILBOX_ADDR;
}

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
      .parity = HCP2_BRINGUP_UART_PARITY,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  const gpio_config_t de_config = {
      .pin_bit_mask = (1ULL << HCP2_BRINGUP_DE) | (1ULL << HCP2_BRINGUP_RE),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_ENABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };

  ESP_ERROR_CHECK(gpio_config(&de_config));
  gpio_set_level(HCP2_BRINGUP_DE, 0);
  gpio_set_level(HCP2_BRINGUP_RE, 0);
  ESP_ERROR_CHECK(uart_driver_install(HCP2_BRINGUP_UART, HCP2_BRINGUP_RX_BUF, HCP2_BRINGUP_TX_BUF, 0, NULL, 0));
  ESP_ERROR_CHECK(uart_param_config(HCP2_BRINGUP_UART, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(HCP2_BRINGUP_UART, HCP2_BRINGUP_UART_TX, HCP2_BRINGUP_UART_RX, UART_PIN_NO_CHANGE,
                               UART_PIN_NO_CHANGE));
}
#endif

#if CONFIG_HCP2_BRINGUP_MAILBOX_TEST_DOUBLE && CONFIG_HCP2_BRINGUP_HP_FALLBACK
static bool run_mailbox_test_double(void) {
  hcp2_lp_health_sample_t health_before;
  hcp2_lp_health_sample_t health_after;
  uint32_t last_sequence = 0;
  hcp2_lp_command_t command;
  hcp2_hp_supervisor_t supervisor;
  uint32_t sequence;

  hcp2_lp_mailbox_init(&s_mailbox_double);
  hcp2_hp_supervisor_init(&supervisor, &s_mailbox_double, HCP2_LP_FIRMWARE_VERSION);
  hcp2_hp_supervisor_sample_health(&supervisor, &health_before);
  s_mailbox_double.heartbeat = health_before.heartbeat + 1u;
  hcp2_hp_supervisor_sample_health(&supervisor, &health_after);

  if (!hcp2_hp_supervisor_is_healthy(&supervisor, &health_before, &health_after)) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_SKIP_RELOAD_FAIL");
    return false;
  }
  ESP_LOGI(TAG, "HCP2_SUPERVISOR_SKIP_RELOAD_OK heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32,
           health_before.heartbeat, health_after.heartbeat);

  if (hcp2_hp_supervisor_reload_decision(&supervisor, &health_after, &health_after) !=
      HCP2_LP_RELOAD_REQUIRED) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_STALE_HEARTBEAT_FAIL");
    return false;
  }
  ESP_LOGI(TAG, "HCP2_SUPERVISOR_STALE_HEARTBEAT_OK");

  hcp2_hp_supervisor_begin_session(&supervisor, 0x22220000u);
  hcp2_lp_mailbox_send_command(&s_mailbox_double, 0x11110000u, 1u, HCP2_LP_COMMAND_OPEN, 0u, 0u);
  if (hcp2_lp_mailbox_take_command(&s_mailbox_double, 0x22220000u, &last_sequence, 0u, &command)) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_EPOCH_REPLAY_FAIL");
    return false;
  }

  sequence = hcp2_hp_supervisor_send_command_at(&supervisor, HCP2_LP_COMMAND_CLOSE, 0u, 0u, 1000u);
  if (!hcp2_lp_mailbox_take_command(&s_mailbox_double, 0x22220000u, &last_sequence, 1u, &command) ||
      command.command_id != HCP2_LP_COMMAND_CLOSE) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_EPOCH_ACK_FAIL");
    return false;
  }
  hcp2_lp_mailbox_ack_command(&s_mailbox_double, sequence, HCP2_LP_COMMAND_RESULT_EXECUTED);
  if (hcp2_hp_supervisor_ack_result(&supervisor, sequence) != HCP2_LP_COMMAND_RESULT_EXECUTED) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_EPOCH_ACK_RESULT_FAIL");
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
  ESP_LOGI(TAG, "HCP2_BRINGUP_READY uart=1 rx_gpio=4 tx_gpio=5 baud=57600 parity=%s",
           HCP2_BRINGUP_UART_PARITY_NAME);

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
  uart_cfg.uart_proto_cfg.parity = HCP2_BRINGUP_UART_PARITY;
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
  ESP_RETURN_ON_ERROR(rtc_gpio_init(HCP2_BRINGUP_RE), TAG, "init LP /RE GPIO");
  ESP_RETURN_ON_ERROR(rtc_gpio_set_direction(HCP2_BRINGUP_RE, RTC_GPIO_MODE_OUTPUT_ONLY), TAG,
                      "set LP /RE output");
  ESP_RETURN_ON_ERROR(rtc_gpio_set_level(HCP2_BRINGUP_RE, 0), TAG, "drive LP /RE low");
  ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_en(HCP2_BRINGUP_RE), TAG, "enable LP /RE pulldown");

  ESP_RETURN_ON_ERROR(lp_core_uart_init(&uart_cfg), TAG, "init LP UART");
  return ESP_OK;
}

static hcp2_lp_reload_decision_t probe_lp_health_(hcp2_lp_health_sample_t *before,
                                                  hcp2_lp_health_sample_t *after) {
  hcp2_lp_health_sample_t before_local;
  hcp2_lp_health_sample_t after_local;

  hcp2_hp_supervisor_sample_health(&s_lp_supervisor, &before_local);
  vTaskDelay(pdMS_TO_TICKS(HCP2_BRINGUP_HEARTBEAT_PROBE_MS));
  hcp2_hp_supervisor_sample_health(&s_lp_supervisor, &after_local);
  if (before != NULL) {
    *before = before_local;
  }
  if (after != NULL) {
    *after = after_local;
  }
  return hcp2_hp_supervisor_reload_decision(&s_lp_supervisor, &before_local, &after_local);
}

static bool healthy_lp_running_(hcp2_lp_health_sample_t *before, hcp2_lp_health_sample_t *after) {
  return probe_lp_health_(before, after) == HCP2_LP_RELOAD_SKIP;
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
  hcp2_lp_health_sample_t before;
  hcp2_lp_health_sample_t after;
  hcp2_lp_reload_decision_t decision;

#if CONFIG_HCP2_BRINGUP_FORCE_LP_RELOAD
  ESP_LOGW(TAG, "HCP2_LP_FORCE_RELOAD");
  return load_and_start_lp_();
#endif

  decision = probe_lp_health_(&before, &after);
  if (decision == HCP2_LP_RELOAD_SKIP) {
    ESP_LOGI(TAG,
             "HCP2_LP_SKIP_RELOAD heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32
             " polls_seen=%" PRIu32 " polls_answered=%" PRIu32,
             before.heartbeat, after.heartbeat, after.polls_seen, after.polls_answered);
    return ESP_OK;
  }
  if (decision == HCP2_LP_RELOAD_DEFER) {
    ESP_LOGW(TAG,
             "HCP2_LP_RELOAD_DEFER heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32
             " state=0x%02" PRIx8 " command=%" PRIu32 "/%" PRIu32,
             before.heartbeat, after.heartbeat, after.drive_state, after.command_ack_sequence,
             after.command_sequence);
    return ESP_OK;
  }

  ESP_LOGI(TAG, "HCP2_LP_RELOAD_REQUIRED heartbeat_before=%" PRIu32 " heartbeat_after=%" PRIu32,
           before.heartbeat, after.heartbeat);
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

#if CONFIG_HCP2_BRINGUP_SCRIPTED_COMMANDS
static const char *command_name_(hcp2_lp_command_id_t command_id) {
  switch (command_id) {
    case HCP2_LP_COMMAND_OPEN:
      return "open";
    case HCP2_LP_COMMAND_CLOSE:
      return "close";
    case HCP2_LP_COMMAND_STOP:
      return "stop";
    case HCP2_LP_COMMAND_VENT:
      return "vent";
    case HCP2_LP_COMMAND_HALF:
      return "half";
    case HCP2_LP_COMMAND_LIGHT:
      return "light";
    case HCP2_LP_COMMAND_NONE:
    default:
      return "none";
  }
}
#endif

static const char *trace_event_name_(uint16_t event) {
  switch (event) {
    case HCP2_LP_TRACE_BOOT:
      return "boot";
    case HCP2_LP_TRACE_TX:
      return "tx";
    case HCP2_LP_TRACE_COMMAND:
      return "command";
    case HCP2_LP_TRACE_RX:
      return "rx";
    case HCP2_LP_TRACE_RX_ERROR:
      return "rx-error";
    case HCP2_LP_TRACE_DE:
      return "de";
    case HCP2_LP_TRACE_GPIO_RX:
      return "gpio-rx";
    case HCP2_LP_TRACE_RX_ECHO:
      return "rx-echo";
    case HCP2_LP_TRACE_TX_ABORT:
      return "tx-abort";
    case HCP2_LP_TRACE_COLLISION:
      return "collision";
    default:
      return "unknown";
  }
}

static void log_lp_trace_(void) {
  static uint32_t last_trace_head;
  volatile hcp2_lp_mailbox_t *mailbox = lp_mailbox_();
  const uint32_t head = mailbox->trace_head;
  const uint32_t tail = mailbox->trace_tail;

  if (last_trace_head < tail || last_trace_head > head) {
    last_trace_head = tail;
  }
  if ((head - last_trace_head) > HCP2_LP_TRACE_CAPACITY) {
    last_trace_head = head - HCP2_LP_TRACE_CAPACITY;
  }

  while (last_trace_head < head) {
    const hcp2_lp_trace_entry_t entry = mailbox->trace[last_trace_head % HCP2_LP_TRACE_CAPACITY];
    ESP_LOGI(TAG, "HCP2_LP_TRACE event=%s event_id=%" PRIu16 " at_us=%" PRIu32 " value=0x%04" PRIx16,
             trace_event_name_(entry.event), entry.event, entry.at_us, entry.value);
    last_trace_head++;
  }
}

#if CONFIG_HCP2_BRINGUP_LP_UART_STATUS_LOG
static uint32_t lp_uart_reg_read_(uint32_t addr) {
  return *(volatile uint32_t *) addr;
}

static void log_lp_uart_status_(void) {
  const uint32_t status = lp_uart_reg_read_(LP_UART_STATUS_REG);
  const uint32_t raw = lp_uart_reg_read_(LP_UART_INT_RAW_REG);
  const uint32_t conf0 = lp_uart_reg_read_(LP_UART_CONF0_SYNC_REG);
  const uint32_t conf1 = lp_uart_reg_read_(LP_UART_CONF1_REG);
  const uint32_t clk = lp_uart_reg_read_(LP_UART_CLK_CONF_REG);
  const uint32_t fsm = lp_uart_reg_read_(LP_UART_FSM_STATUS_REG);
  const uint32_t rx_count = (status >> LP_UART_RXFIFO_CNT_S) & LP_UART_RXFIFO_CNT_V;
  const uint32_t tx_count = (status >> LP_UART_TXFIFO_CNT_S) & LP_UART_TXFIFO_CNT_V;
  const uint32_t rxd = (status >> LP_UART_RXD_S) & LP_UART_RXD_V;
  const uint32_t txd = (status >> LP_UART_TXD_S) & LP_UART_TXD_V;

  ESP_LOGI(TAG,
           "HCP2_LP_UART_STATUS raw=0x%08" PRIx32 " status=0x%08" PRIx32
           " rx_count=%" PRIu32 " tx_count=%" PRIu32 " rxd=%" PRIu32 " txd=%" PRIu32
           " conf0=0x%08" PRIx32 " conf1=0x%08" PRIx32 " clk=0x%08" PRIx32
           " fsm=0x%08" PRIx32,
           raw, status, rx_count, tx_count, rxd, txd, conf0, conf1, clk, fsm);
}
#endif

static bool verify_real_mailbox_(void) {
  volatile hcp2_lp_mailbox_t *mailbox = lp_mailbox_();
  hcp2_lp_health_sample_t health_before;
  hcp2_lp_health_sample_t health_after;
  const uint32_t epoch = fresh_epoch_();
  const uint32_t stale_epoch = epoch ^ 0xA5A55A5Au;
  const int health_attempts = HCP2_BRINGUP_MAILBOX_TIMEOUT_MS / HCP2_BRINGUP_HEARTBEAT_PROBE_MS;
  bool healthy = false;

  for (int i = 0; i < health_attempts; i++) {
    if (healthy_lp_running_(&health_before, &health_after)) {
      healthy = true;
      break;
    }
  }

  if (!healthy) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_REAL_SKIP_RELOAD_FAIL heartbeat_before=%" PRIu32
                  " heartbeat_after=%" PRIu32,
             health_before.heartbeat, health_after.heartbeat);
    return false;
  }
  ESP_LOGI(TAG, "HCP2_SUPERVISOR_REAL_SKIP_RELOAD_OK heartbeat_before=%" PRIu32
                " heartbeat_after=%" PRIu32 " polls_seen=%" PRIu32 " polls_answered=%" PRIu32,
           health_before.heartbeat, health_after.heartbeat, health_after.polls_seen, health_after.polls_answered);

  hcp2_hp_supervisor_begin_session(&s_lp_supervisor, epoch);
  vTaskDelay(pdMS_TO_TICKS(HCP2_BRINGUP_HEARTBEAT_PROBE_MS * 2));

  hcp2_lp_mailbox_send_command(mailbox, stale_epoch, 1u, HCP2_LP_COMMAND_OPEN, 0u, mailbox->lp_time_us + 100000u);
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
  if (hcp2_hp_supervisor_ack_result(&s_lp_supervisor, sequence) != HCP2_LP_COMMAND_RESULT_EXECUTED) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_REAL_EPOCH_ACK_RESULT_FAIL result=%u",
             (unsigned) hcp2_hp_supervisor_ack_result(&s_lp_supervisor, sequence));
    return false;
  }

  ESP_LOGI(TAG, "HCP2_SUPERVISOR_REAL_EPOCH_OK epoch=%" PRIu32 " sequence=%" PRIu32, epoch, sequence);
  ESP_LOGI(TAG, "HCP2_SUPERVISOR_REAL_MAILBOX_OK");
  return true;
}

static void lp_supervisor_task(void *arg) {
  (void) arg;
  if (!verify_real_mailbox_()) {
    ESP_LOGE(TAG, "HCP2_SUPERVISOR_REAL_MAILBOX_FAIL");
  }
  log_lp_trace_();

  for (;;) {
    hcp2_lp_health_sample_t health_before;
    hcp2_lp_health_sample_t health_after;
    const hcp2_lp_reload_decision_t decision = probe_lp_health_(&health_before, &health_after);
    log_lp_trace_();
#if CONFIG_HCP2_BRINGUP_LP_UART_STATUS_LOG
    log_lp_uart_status_();
#endif
    if (decision != HCP2_LP_RELOAD_SKIP) {
      ESP_LOGE(TAG,
               "HCP2_LP_HEALTH_FAIL decision=%u heartbeat_before=%" PRIu32
               " heartbeat_after=%" PRIu32 " polls_seen_before=%" PRIu32
               " polls_seen_after=%" PRIu32 " polls_answered_before=%" PRIu32
               " polls_answered_after=%" PRIu32,
               (unsigned) decision, health_before.heartbeat, health_after.heartbeat,
               health_before.polls_seen, health_after.polls_seen, health_before.polls_answered,
               health_after.polls_answered);
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

#if CONFIG_HCP2_BRINGUP_SCRIPTED_COMMANDS
static void scripted_command_task(void *arg) {
  static const hcp2_lp_command_id_t commands[] = {
      HCP2_LP_COMMAND_OPEN,
      HCP2_LP_COMMAND_CLOSE,
      HCP2_LP_COMMAND_LIGHT,
  };
  size_t index = 0;

  (void) arg;
  vTaskDelay(pdMS_TO_TICKS(1500));
  for (;;) {
    const hcp2_lp_command_id_t command_id = commands[index % (sizeof(commands) / sizeof(commands[0]))];
    const uint32_t sequence = hcp2_hp_supervisor_send_command(&s_lp_supervisor, command_id, 0u);
    const bool ack = wait_for_ack_(sequence);
    const hcp2_lp_command_result_t result = hcp2_hp_supervisor_ack_result(&s_lp_supervisor, sequence);

    ESP_LOGI(TAG, "HCP2_SCRIPTED_COMMAND name=%s sequence=%" PRIu32 " ack=%d result=%u",
             command_name_(command_id), sequence, ack ? 1 : 0, (unsigned) result);
    index++;
    vTaskDelay(pdMS_TO_TICKS(CONFIG_HCP2_BRINGUP_SCRIPTED_COMMAND_INTERVAL_MS));
  }
}
#endif

#if CONFIG_HCP2_BRINGUP_PANIC_INTERVAL_MS > 0
static void panic_task(void *arg) {
  (void) arg;
  ESP_LOGI(TAG, "HCP2_PANIC_LOOP interval_ms=%d", CONFIG_HCP2_BRINGUP_PANIC_INTERVAL_MS);
  vTaskDelay(pdMS_TO_TICKS(CONFIG_HCP2_BRINGUP_PANIC_INTERVAL_MS));
  ESP_LOGI(TAG, "HCP2_PANIC_NOW");
  fflush(stdout);
  abort();
}
#endif

#if CONFIG_HCP2_BRINGUP_TASK_WDT_INTERVAL_MS > 0
static void task_wdt_task(void *arg) {
  const esp_task_wdt_config_t twdt_config = {
      .timeout_ms = 1000,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };
  esp_err_t status;

  (void) arg;
  ESP_LOGI(TAG, "HCP2_TASK_WDT_LOOP interval_ms=%d", CONFIG_HCP2_BRINGUP_TASK_WDT_INTERVAL_MS);
  vTaskDelay(pdMS_TO_TICKS(CONFIG_HCP2_BRINGUP_TASK_WDT_INTERVAL_MS));
  ESP_LOGI(TAG, "HCP2_TASK_WDT_ARM");
  status = esp_task_wdt_reconfigure(&twdt_config);
  if (status == ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(esp_task_wdt_init(&twdt_config));
  } else {
    ESP_ERROR_CHECK(status);
  }
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
  ESP_LOGI(TAG, "HCP2_TASK_WDT_WAIT");
  fflush(stdout);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
#endif

#if CONFIG_HCP2_BRINGUP_RTC_WDT_INTERVAL_MS > 0
static void rtc_wdt_task(void *arg) {
  wdt_hal_context_t rtc_wdt_ctx = RWDT_HAL_CONTEXT_DEFAULT();

  (void) arg;
  ESP_LOGI(TAG, "HCP2_RTC_WDT_LOOP interval_ms=%d", CONFIG_HCP2_BRINGUP_RTC_WDT_INTERVAL_MS);
  vTaskDelay(pdMS_TO_TICKS(CONFIG_HCP2_BRINGUP_RTC_WDT_INTERVAL_MS));
  ESP_LOGI(TAG, "HCP2_RTC_WDT_ARM");
  wdt_hal_init(&rtc_wdt_ctx, WDT_RWDT, 0, false);
  wdt_hal_write_protect_disable(&rtc_wdt_ctx);
  wdt_hal_config_stage(&rtc_wdt_ctx, WDT_STAGE0, rtc_clk_slow_freq_get_hz() / 10u,
                       WDT_STAGE_ACTION_RESET_SYSTEM);
  wdt_hal_set_flashboot_en(&rtc_wdt_ctx, true);
  wdt_hal_enable(&rtc_wdt_ctx);
  wdt_hal_write_protect_enable(&rtc_wdt_ctx);
  ESP_LOGI(TAG, "HCP2_RTC_WDT_WAIT");
  fflush(stdout);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
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
  ulp_lp_core_stop();
  init_uart();
  xTaskCreate(responder_task, "hcp2_responder", 4096, &s_ctx, configMAX_PRIORITIES - 1, NULL);
#endif

#if CONFIG_HCP2_BRINGUP_RESTART_INTERVAL_MS > 0
  xTaskCreate(restart_task, "hcp2_restart", 2048, NULL, 1, NULL);
#endif
#if CONFIG_HCP2_BRINGUP_SCRIPTED_COMMANDS
  xTaskCreate(scripted_command_task, "hcp2_scripted", 3072, NULL, 4, NULL);
#endif
#if CONFIG_HCP2_BRINGUP_PANIC_INTERVAL_MS > 0
  xTaskCreate(panic_task, "hcp2_panic", 2048, NULL, 1, NULL);
#endif
#if CONFIG_HCP2_BRINGUP_TASK_WDT_INTERVAL_MS > 0
  xTaskCreate(task_wdt_task, "hcp2_task_wdt", 3072, NULL, 1, NULL);
#endif
#if CONFIG_HCP2_BRINGUP_RTC_WDT_INTERVAL_MS > 0
  xTaskCreate(rtc_wdt_task, "hcp2_rtc_wdt", 3072, NULL, 1, NULL);
#endif
}
