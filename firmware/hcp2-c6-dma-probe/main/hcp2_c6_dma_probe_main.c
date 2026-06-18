#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/uhci.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/uart_ll.h"
#include "hcp2_engine.h"
#include "soc/soc_caps.h"
#include "ulp_lp_core.h"

#define HCP2_PROBE_UART UART_NUM_1
#define HCP2_PROBE_RX_GPIO GPIO_NUM_4
#define HCP2_PROBE_TX_GPIO GPIO_NUM_5
#define HCP2_PROBE_DE_GPIO GPIO_NUM_0
#define HCP2_PROBE_RE_GPIO GPIO_NUM_1

#define HCP2_PROBE_BAUD 57600
#define HCP2_PROBE_RX_IDLE_BITS 22
#define HCP2_PROBE_RX_BUF_SIZE 64
#define HCP2_PROBE_RX_BUF_COUNT 3
#define HCP2_PROBE_TX_BUF_SIZE 64
#define HCP2_PROBE_TASK_STACK 6144
#define HCP2_PROBE_MAX_DE_US 12000
#define HCP2_PROBE_DE_TAIL_US 60
#define HCP2_PROBE_TX_LEAD_US 40
#define HCP2_PROBE_TX_COARSE_SLEEP_US 250

static const char *const TAG = "hcp2_c6_dma_probe";

typedef struct {
  uint8_t data[HCP2_PROBE_RX_BUF_SIZE];
  uint8_t len;
  uint32_t eof_cycle;
  uint32_t overflow;
} rx_event_t;

typedef struct {
  uint32_t rx_eofs;
  uint32_t rx_events_dropped;
  uint32_t rx_rearm_errors;
  uint32_t rx_bytes;
  uint32_t frames_valid;
  uint32_t frames_broadcast;
  uint32_t frames_scan;
  uint32_t frames_status_poll;
  uint32_t crc_errors;
  uint32_t rx_errors;
  uint32_t tx_attempts;
  uint32_t tx_dma_queued;
  uint32_t tx_dma_done;
  uint32_t tx_dma_errors;
  uint32_t tx_uart_fallback;
  uint32_t tx_deadman;
  uint32_t pending_tx_drops;
  uint32_t max_rx_event_len;
  uint32_t max_rx_rearm_delay_cycles;
  uint32_t max_frame_to_tx_start_us;
  uint32_t max_tx_hold_us;
  uint32_t last_tx_len;
  uint32_t last_tx_err;
} probe_stats_t;

static uhci_controller_handle_t s_uhci;
static TaskHandle_t s_parser_task;
static uart_dev_t *s_uart_hw;
static hcp2_engine_t s_engine;
static volatile bool s_rx_event_pending;
static volatile bool s_tx_done_pending;
static volatile bool s_tx_done_seen;
static volatile uint32_t s_tx_done_size;
static rx_event_t s_rx_event;
static probe_stats_t s_stats;
static uint32_t s_rx_buf_index;
static DRAM_ATTR uint8_t s_rx_bufs[HCP2_PROBE_RX_BUF_COUNT][HCP2_PROBE_RX_BUF_SIZE] __attribute__((aligned(64)));
static DRAM_ATTR uint8_t s_tx_dma_buf[HCP2_PROBE_TX_BUF_SIZE] __attribute__((aligned(64)));

static inline uint32_t cycles_now_(void) {
  return esp_cpu_get_cycle_count();
}

static inline uint32_t now_us32_(void) {
  return (uint32_t) esp_timer_get_time();
}

static uint32_t hcp2_now_us_(void *user) {
  (void) user;
  return now_us32_();
}

static void rs485_rx_mode_(void) {
  gpio_set_level(HCP2_PROBE_DE_GPIO, 0);
  gpio_set_level(HCP2_PROBE_RE_GPIO, 0);
}

static void rs485_tx_mode_(void) {
  gpio_set_level(HCP2_PROBE_RE_GPIO, 1);
  gpio_set_level(HCP2_PROBE_DE_GPIO, 1);
}

static void wait_until_us_(uint32_t due_us) {
  while ((int32_t) (due_us - now_us32_()) > (int32_t) HCP2_PROBE_TX_COARSE_SLEEP_US) {
    vTaskDelay(1);
  }
  while ((int32_t) (due_us - now_us32_()) > 0) {
    __asm__ __volatile__("nop");
  }
}

static void uart_fifo_write_blocking_(const uint8_t *data, uint8_t len) {
  uint8_t offset = 0;

  while (offset < len) {
    const uint32_t room = uart_ll_get_txfifo_len(s_uart_hw);
    if (room == 0) {
      continue;
    }
    const uint32_t chunk = (len - offset) < room ? (len - offset) : room;
    uart_ll_write_txfifo(s_uart_hw, data + offset, chunk);
    offset = (uint8_t) (offset + chunk);
  }
}

static void wait_uart_idle_(uint32_t deadline_us) {
  while (!uart_ll_is_tx_idle(s_uart_hw) && (int32_t) (deadline_us - now_us32_()) > 0) {
    __asm__ __volatile__("nop");
  }
}

static void send_response_(const uint8_t *data, uint8_t len, const hcp2_pending_tx_meta_t *meta) {
  uint32_t start_us;
  uint32_t deadline_us;
  esp_err_t err;

  if (len == 0 || len > HCP2_PROBE_TX_BUF_SIZE) {
    return;
  }

  memcpy(s_tx_dma_buf, data, len);
  s_stats.tx_attempts++;
  s_stats.last_tx_len = len;
  rs485_tx_mode_();
  esp_rom_delay_us(HCP2_PROBE_TX_LEAD_US);
  start_us = now_us32_();
  deadline_us = start_us + HCP2_PROBE_MAX_DE_US;
  if (meta != NULL && meta->is_status_response) {
    const uint32_t frame_to_start = start_us - meta->scheduled_us;
    if (frame_to_start > s_stats.max_frame_to_tx_start_us) {
      s_stats.max_frame_to_tx_start_us = frame_to_start;
    }
  }
  hcp2_engine_mark_tx_started(&s_engine, start_us);

  s_tx_done_pending = true;
  s_tx_done_seen = false;
  s_tx_done_size = 0;
  err = uhci_transmit(s_uhci, s_tx_dma_buf, len);
  if (err == ESP_OK) {
    s_stats.tx_dma_queued++;
    while (!s_tx_done_seen && (int32_t) (deadline_us - now_us32_()) > 0) {
      __asm__ __volatile__("nop");
    }
    if (!s_tx_done_seen) {
      s_stats.tx_deadman++;
    }
    wait_uart_idle_(deadline_us);
  } else {
    s_stats.tx_dma_errors++;
    s_stats.last_tx_err = (uint32_t) err;
    s_stats.tx_uart_fallback++;
    uart_fifo_write_blocking_(data, len);
    wait_uart_idle_(deadline_us);
  }

  esp_rom_delay_us(HCP2_PROBE_DE_TAIL_US);
  rs485_rx_mode_();
  const uint32_t done_us = now_us32_();
  const uint32_t hold_us = done_us - start_us;
  if (hold_us > s_stats.max_tx_hold_us) {
    s_stats.max_tx_hold_us = hold_us;
  }
  hcp2_engine_mark_tx_done(&s_engine, done_us);
}

static esp_err_t start_rx_(void) {
  const uint32_t index = s_rx_buf_index++ % HCP2_PROBE_RX_BUF_COUNT;
  esp_err_t err = uhci_receive(s_uhci, s_rx_bufs[index], sizeof(s_rx_bufs[index]));
  if (err != ESP_OK) {
    s_stats.rx_rearm_errors++;
  }
  return err;
}

static bool IRAM_ATTR uhci_rx_cb_(uhci_controller_handle_t uhci, const uhci_rx_event_data_t *edata, void *user_ctx) {
  (void) uhci;
  (void) user_ctx;
  BaseType_t high_task_woken = pdFALSE;
  uint32_t len = edata != NULL ? edata->recv_size : 0;

  s_stats.rx_eofs++;
  if (s_rx_event_pending) {
    s_stats.rx_events_dropped++;
    return false;
  }
  if (len > HCP2_PROBE_RX_BUF_SIZE) {
    len = HCP2_PROBE_RX_BUF_SIZE;
    s_rx_event.overflow = 1;
  } else {
    s_rx_event.overflow = 0;
  }
  s_rx_event.len = (uint8_t) len;
  s_rx_event.eof_cycle = cycles_now_();
  for (uint32_t i = 0; i < len; i++) {
    s_rx_event.data[i] = edata->data[i];
  }
  if (len > s_stats.max_rx_event_len) {
    s_stats.max_rx_event_len = len;
  }
  s_rx_event_pending = true;
  if (s_parser_task != NULL) {
    vTaskNotifyGiveFromISR(s_parser_task, &high_task_woken);
  }
  return high_task_woken == pdTRUE;
}

static bool IRAM_ATTR uhci_tx_cb_(uhci_controller_handle_t uhci, const uhci_tx_done_event_data_t *edata,
                                  void *user_ctx) {
  (void) uhci;
  (void) user_ctx;
  s_stats.tx_dma_done++;
  if (s_tx_done_pending) {
    s_tx_done_size = edata != NULL ? edata->sent_size : 0;
    s_tx_done_seen = true;
    s_tx_done_pending = false;
  }
  return false;
}

static void feed_rx_event_(const rx_event_t *event) {
  for (uint8_t i = 0; i < event->len; i++) {
    hcp2_engine_rx_byte(&s_engine, event->data[i], HCP2_RX_OK);
  }
  s_stats.rx_bytes += event->len;
  s_stats.frames_valid = s_engine.valid_frames;
  s_stats.frames_broadcast = s_engine.broadcasts_received;
  s_stats.frames_status_poll = s_engine.status_polls_received;
  s_stats.crc_errors = s_engine.crc_errors;
  s_stats.rx_errors = s_engine.rx_errors;
  s_stats.pending_tx_drops = s_engine.pending_tx_drop_count;
}

static void service_pending_tx_(void) {
  uint8_t tx[HCP2_MAX_FRAME_LEN];
  uint8_t tx_len = 0;
  hcp2_pending_tx_meta_t meta;

  while (hcp2_engine_pending_tx_ready(&s_engine)) {
    const uint32_t due_us = hcp2_engine_pending_tx_due_us(&s_engine);
    wait_until_us_(due_us);
    if (!hcp2_engine_claim_due_tx(&s_engine, now_us32_(), tx, &tx_len, &meta)) {
      return;
    }
    if (meta.frame_type == HCP2_FRAME_BUS_SCAN) {
      s_stats.frames_scan++;
    }
    send_response_(tx, tx_len, &meta);
  }
}

static void parser_task_(void *arg) {
  (void) arg;
  rx_event_t local_event;

  ESP_LOGI(TAG, "parser task started");
  ESP_ERROR_CHECK(start_rx_());

  for (;;) {
    (void) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    if (s_rx_event_pending) {
      const uint32_t rearm_start = cycles_now_();
      local_event = s_rx_event;
      s_rx_event_pending = false;
      (void) start_rx_();
      const uint32_t rearm_delay = cycles_now_() - rearm_start;
      if (rearm_delay > s_stats.max_rx_rearm_delay_cycles) {
        s_stats.max_rx_rearm_delay_cycles = rearm_delay;
      }
      feed_rx_event_(&local_event);
      service_pending_tx_();
    }
  }
}

static void stats_task_(void *arg) {
  (void) arg;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG,
             "rx_eof=%" PRIu32 " rx_bytes=%" PRIu32 " valid=%" PRIu32 " scan=%" PRIu32
             " poll=%" PRIu32 " crc=%" PRIu32 " rxerr=%" PRIu32
             " tx=%" PRIu32 " dma_q=%" PRIu32 " dma_done=%" PRIu32 " dma_err=%" PRIu32
             " fallback=%" PRIu32 " deadman=%" PRIu32 " drops=%" PRIu32
             " max_rx_len=%" PRIu32 " max_rearm_cycles=%" PRIu32 " max_tx_start_us=%" PRIu32
             " max_de_us=%" PRIu32 " last_tx_len=%" PRIu32 " last_tx_err=%" PRIu32,
             s_stats.rx_eofs, s_stats.rx_bytes, s_stats.frames_valid, s_stats.frames_scan,
             s_stats.frames_status_poll, s_stats.crc_errors, s_stats.rx_errors, s_stats.tx_attempts,
             s_stats.tx_dma_queued, s_stats.tx_dma_done, s_stats.tx_dma_errors, s_stats.tx_uart_fallback,
             s_stats.tx_deadman, s_stats.rx_events_dropped + s_stats.pending_tx_drops, s_stats.max_rx_event_len,
             s_stats.max_rx_rearm_delay_cycles, s_stats.max_frame_to_tx_start_us, s_stats.max_tx_hold_us,
             s_stats.last_tx_len, s_stats.last_tx_err);
  }
}

static void configure_rs485_pins_(void) {
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << HCP2_PROBE_DE_GPIO) | (1ULL << HCP2_PROBE_RE_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_ENABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&io_conf));
  rs485_rx_mode_();
}

static void configure_uart_(void) {
  uart_config_t uart_config = {
      .baud_rate = HCP2_PROBE_BAUD,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_EVEN,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 0,
      .source_clk = UART_SCLK_XTAL,
  };

  ESP_ERROR_CHECK(uart_param_config(HCP2_PROBE_UART, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(HCP2_PROBE_UART, HCP2_PROBE_TX_GPIO, HCP2_PROBE_RX_GPIO, UART_PIN_NO_CHANGE,
                               UART_PIN_NO_CHANGE));
  ESP_ERROR_CHECK(gpio_set_pull_mode(HCP2_PROBE_RX_GPIO, GPIO_PULLUP_ONLY));
  ESP_ERROR_CHECK(uart_set_tx_idle_num(HCP2_PROBE_UART, 2));

  s_uart_hw = UART_LL_GET_HW(HCP2_PROBE_UART);
  uart_ll_disable_intr_mask(s_uart_hw, UINT32_MAX);
  uart_ll_clr_intsts_mask(s_uart_hw, UINT32_MAX);
  uart_ll_rxfifo_rst(s_uart_hw);
  uart_ll_txfifo_rst(s_uart_hw);
  uart_ll_set_rx_idle_thr(s_uart_hw, HCP2_PROBE_RX_IDLE_BITS);
  uart_ll_update(s_uart_hw);
}

static void configure_uhci_(void) {
  const uhci_controller_config_t uhci_config = {
      .uart_port = HCP2_PROBE_UART,
      .tx_trans_queue_depth = 2,
      .max_transmit_size = HCP2_PROBE_TX_BUF_SIZE,
      .max_receive_internal_mem = HCP2_PROBE_RX_BUF_SIZE * HCP2_PROBE_RX_BUF_COUNT,
      .dma_burst_size = 0,
      .max_packet_receive = 0,
      .rx_eof_flags =
          {
              .rx_brk_eof = 0,
              .idle_eof = 1,
              .length_eof = 0,
          },
  };
  const uhci_event_callbacks_t callbacks = {
      .on_rx_trans_event = uhci_rx_cb_,
      .on_tx_trans_done = uhci_tx_cb_,
  };

  ESP_ERROR_CHECK(uhci_new_controller(&uhci_config, &s_uhci));
  ESP_ERROR_CHECK(uhci_register_event_callbacks(s_uhci, &callbacks, NULL));
}

void app_main(void) {
  hcp2_port_t port = {
      .user = NULL,
      .now_us = hcp2_now_us_,
      .tx = NULL,
      .de_set = NULL,
  };
  hcp2_engine_config_t config;

  ESP_LOGW(TAG, "starting ESP32-C6 HCP2 UHCI/GDMA probe on UART1 rx=%d tx=%d de=%d re=%d", HCP2_PROBE_RX_GPIO,
           HCP2_PROBE_TX_GPIO, HCP2_PROBE_DE_GPIO, HCP2_PROBE_RE_GPIO);
  ulp_lp_core_stop();
  configure_rs485_pins_();
  configure_uart_();
  configure_uhci_();

  hcp2_engine_config_default(&config);
  config.response_delay_us = HCP2_DEFAULT_RESPONSE_DELAY_US;
  hcp2_engine_init(&s_engine, &port, &config);

  if (xTaskCreate(parser_task_, "hcp2_dma_parse", HCP2_PROBE_TASK_STACK, NULL, configMAX_PRIORITIES - 2,
                  &s_parser_task) != pdPASS) {
    ESP_LOGE(TAG, "failed to create parser task");
    abort();
  }
  if (xTaskCreate(stats_task_, "hcp2_dma_stats", 4096, NULL, 3, NULL) != pdPASS) {
    ESP_LOGE(TAG, "failed to create stats task");
    abort();
  }
}
