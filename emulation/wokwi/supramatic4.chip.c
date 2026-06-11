#include "wokwi-api.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcp2_protocol.h"

#define HCP2_WOKWI_MAX_FRAME 32u
#define HCP2_WOKWI_START_DELAY_US 2500000u
#define HCP2_WOKWI_SCAN_TO_CYCLE_US 50000u

typedef enum {
  MODE_STEADY = 0,
  MODE_RESTART = 1,
} sim_mode_t;

typedef enum {
  PHASE_START = 0,
  PHASE_WAIT_SCAN,
  PHASE_BEFORE_POLL,
  PHASE_WAIT_POLL,
  PHASE_NEXT_CYCLE,
  PHASE_DONE,
  PHASE_FAILED,
} sim_phase_t;

typedef struct {
  uart_dev_t uart;
  timer_t timer;
  pin_t pass_pin;
  pin_t fail_pin;
  uint32_t cycles_attr;
  uint32_t mode_attr;
  uint32_t missed_threshold_attr;
  uint32_t require_recovery_attr;
  uint32_t cycles_target;
  sim_mode_t mode;
  sim_phase_t phase;
  uint8_t rx[HCP2_WOKWI_MAX_FRAME];
  uint8_t rx_len;
  uint32_t polls_sent;
  uint32_t replies;
  uint32_t misses;
  uint32_t consecutive_misses;
  uint32_t max_consecutive_misses;
  uint32_t cycles_done;
  uint32_t command_replies;
  uint32_t missed_threshold;
  uint32_t latency_min_us;
  uint32_t latency_max_us;
  uint64_t latency_sum_us;
  uint64_t poll_started_us;
  bool scan_ok;
  bool saw_miss;
  bool recovered_after_miss;
  bool verdict_printed;
} chip_state_t;

static uint16_t crc16_modbus(const uint8_t *data, uint32_t len) {
  uint16_t crc = 0xFFFFu;
  for (uint32_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8u; bit++) {
      if ((crc & 1u) != 0u) {
        crc = (uint16_t) ((crc >> 1u) ^ 0xA001u);
      } else {
        crc = (uint16_t) (crc >> 1u);
      }
    }
  }
  return crc;
}

static uint64_t now_us(void) {
  return get_sim_nanos() / 1000u;
}

static void set_verdict(chip_state_t *chip, bool ok, const char *reason) {
  if (chip->verdict_printed) {
    return;
  }
  chip->verdict_printed = true;
  chip->phase = ok ? PHASE_DONE : PHASE_FAILED;
  pin_write(chip->pass_pin, ok ? HIGH : LOW);
  pin_write(chip->fail_pin, ok ? LOW : HIGH);
  printf("HCP2_WOKWI_VERDICT_%s reason=%s polls=%u replies=%u misses=%u max_consecutive_misses=%u "
         "latency_min_us=%u latency_max_us=%u\n",
         ok ? "OK" : "FAIL", reason, chip->polls_sent, chip->replies, chip->misses,
         chip->max_consecutive_misses, chip->latency_min_us, chip->latency_max_us);
}

static bool send_frame(chip_state_t *chip, const uint8_t *frame, uint32_t len) {
  if (!uart_write(chip->uart, (uint8_t *) frame, len)) {
    set_verdict(chip, false, "uart-busy");
    return false;
  }
  return true;
}

static bool send_scan(chip_state_t *chip) {
  chip->rx_len = 0;
  chip->phase = PHASE_WAIT_SCAN;
  if (!send_frame(chip, HCP2_WOKWI_SCAN_REQUEST, HCP2_WOKWI_SCAN_REQUEST_LEN)) {
    return false;
  }
  timer_start(chip->timer, HCP2_WOKWI_RESPONSE_TIMEOUT_US, false);
  return true;
}

static bool send_broadcast(chip_state_t *chip) {
  chip->phase = PHASE_BEFORE_POLL;
  if (!send_frame(chip, HCP2_WOKWI_BROADCAST_CLOSED, HCP2_WOKWI_BROADCAST_CLOSED_LEN)) {
    return false;
  }
  timer_start(chip->timer, HCP2_WOKWI_BROADCAST_TO_POLL_GAP_US, false);
  return true;
}

static bool send_status_poll(chip_state_t *chip) {
  uint8_t frame[HCP2_WOKWI_STATUS_POLL_COUNTER0_LEN];
  uint16_t crc;

  memcpy(frame, HCP2_WOKWI_STATUS_POLL_COUNTER0, sizeof(frame));
  frame[11] = (uint8_t) (chip->cycles_done & 0xFFu);
  crc = crc16_modbus(frame, sizeof(frame) - 2u);
  frame[sizeof(frame) - 2u] = (uint8_t) (crc & 0xFFu);
  frame[sizeof(frame) - 1u] = (uint8_t) ((crc >> 8u) & 0xFFu);

  chip->rx_len = 0;
  chip->polls_sent++;
  chip->poll_started_us = now_us();
  chip->phase = PHASE_WAIT_POLL;
  if (!send_frame(chip, frame, sizeof(frame))) {
    return false;
  }
  timer_start(chip->timer, HCP2_WOKWI_RESPONSE_TIMEOUT_US, false);
  return true;
}

static void record_poll_reply(chip_state_t *chip) {
  const uint32_t latency_us = (uint32_t) (now_us() - chip->poll_started_us);
  if (chip->replies == 0u || latency_us < chip->latency_min_us) {
    chip->latency_min_us = latency_us;
  }
  if (latency_us > chip->latency_max_us) {
    chip->latency_max_us = latency_us;
  }
  chip->latency_sum_us += latency_us;
  chip->replies++;
  chip->consecutive_misses = 0;
  if (chip->saw_miss) {
    chip->recovered_after_miss = true;
  }
}

static void record_poll_miss(chip_state_t *chip) {
  chip->misses++;
  chip->saw_miss = true;
  chip->consecutive_misses++;
  if (chip->consecutive_misses > chip->max_consecutive_misses) {
    chip->max_consecutive_misses = chip->consecutive_misses;
  }
  if (chip->consecutive_misses >= chip->missed_threshold) {
    set_verdict(chip, false, "miss-threshold");
  }
}

static void finish_if_done(chip_state_t *chip) {
  if (chip->phase == PHASE_FAILED || chip->phase == PHASE_DONE) {
    return;
  }
  if (chip->cycles_done < chip->cycles_target) {
    return;
  }
  if (!chip->scan_ok) {
    set_verdict(chip, false, "scan-missing");
    return;
  }
  if (chip->mode == MODE_STEADY && chip->misses != 0u) {
    set_verdict(chip, false, "steady-miss");
    return;
  }
  if (attr_read(chip->require_recovery_attr) != 0u && !chip->recovered_after_miss) {
    set_verdict(chip, false, "restart-no-recovery");
    return;
  }
  set_verdict(chip, true, chip->mode == MODE_RESTART ? "restart-recovered" : "steady-state");
}

static uint8_t response_expected_len(const uint8_t *buffer, uint8_t len) {
  uint8_t expected;

  if (len < 3u) {
    return 0u;
  }
  if (buffer[0] != HCP2_WOKWI_SLAVE_ID || buffer[1] != 0x17u) {
    return 0xFFu;
  }
  expected = (uint8_t) (buffer[2] + 5u);
  if (expected < 5u || expected > HCP2_WOKWI_MAX_FRAME) {
    return 0xFFu;
  }
  return expected;
}

static void process_response(chip_state_t *chip, const uint8_t *frame, uint8_t len) {
  if (crc16_modbus(frame, len) != 0u) {
    return;
  }
  if (len == HCP2_WOKWI_SCAN_RESPONSE_LEN && memcmp(frame, HCP2_WOKWI_SCAN_RESPONSE, len) == 0 &&
      chip->phase == PHASE_WAIT_SCAN) {
    chip->scan_ok = true;
    chip->phase = PHASE_NEXT_CYCLE;
    timer_start(chip->timer, HCP2_WOKWI_SCAN_TO_CYCLE_US, false);
    return;
  }
  if (frame[2] == 16u && chip->phase == PHASE_WAIT_POLL) {
    record_poll_reply(chip);
    chip->cycles_done++;
    finish_if_done(chip);
    if (chip->phase != PHASE_DONE && chip->phase != PHASE_FAILED) {
      chip->phase = PHASE_NEXT_CYCLE;
      timer_start(chip->timer, HCP2_WOKWI_POLL_TO_NEXT_BROADCAST_GAP_US, false);
    }
    return;
  }
  if (frame[2] == 4u) {
    chip->command_replies++;
  }
}

static void on_uart_rx_data(void *user_data, uint8_t byte) {
  chip_state_t *chip = (chip_state_t *) user_data;
  uint8_t expected;

  if (chip->phase == PHASE_DONE || chip->phase == PHASE_FAILED) {
    return;
  }
  if (chip->rx_len >= HCP2_WOKWI_MAX_FRAME) {
    memmove(chip->rx, chip->rx + 1, HCP2_WOKWI_MAX_FRAME - 1u);
    chip->rx_len = HCP2_WOKWI_MAX_FRAME - 1u;
  }
  chip->rx[chip->rx_len++] = byte;

  while (chip->rx_len > 0u) {
    expected = response_expected_len(chip->rx, chip->rx_len);
    if (expected == 0u) {
      return;
    }
    if (expected == 0xFFu) {
      memmove(chip->rx, chip->rx + 1, chip->rx_len - 1u);
      chip->rx_len--;
      continue;
    }
    if (chip->rx_len < expected) {
      return;
    }
    process_response(chip, chip->rx, expected);
    if (chip->rx_len == expected) {
      chip->rx_len = 0u;
      return;
    }
    memmove(chip->rx, chip->rx + expected, chip->rx_len - expected);
    chip->rx_len = (uint8_t) (chip->rx_len - expected);
  }
}

static void on_uart_write_done(void *user_data) {
  (void) user_data;
}

static void on_timer(void *user_data) {
  chip_state_t *chip = (chip_state_t *) user_data;

  switch (chip->phase) {
    case PHASE_START:
      (void) send_scan(chip);
      break;
    case PHASE_WAIT_SCAN:
      set_verdict(chip, false, "scan-timeout");
      break;
    case PHASE_BEFORE_POLL:
      (void) send_status_poll(chip);
      break;
    case PHASE_WAIT_POLL:
      record_poll_miss(chip);
      if (chip->phase != PHASE_FAILED) {
        chip->cycles_done++;
        finish_if_done(chip);
        if (chip->phase != PHASE_DONE && chip->phase != PHASE_FAILED) {
          chip->phase = PHASE_NEXT_CYCLE;
          timer_start(chip->timer, HCP2_WOKWI_POLL_TO_NEXT_BROADCAST_GAP_US, false);
        }
      }
      break;
    case PHASE_NEXT_CYCLE:
      finish_if_done(chip);
      if (chip->phase != PHASE_DONE && chip->phase != PHASE_FAILED) {
        (void) send_broadcast(chip);
      }
      break;
    case PHASE_DONE:
    case PHASE_FAILED:
    default:
      break;
  }
}

void chip_init(void) {
  chip_state_t *chip;
  uart_config_t uart_config;
  timer_config_t timer_config;

  setvbuf(stdout, NULL, _IOLBF, 1024);
  chip = calloc(1, sizeof(*chip));
  if (chip == NULL) {
    printf("HCP2_WOKWI_VERDICT_FAIL reason=alloc\n");
    return;
  }

  chip->cycles_attr = attr_init("cycles", 200);
  chip->mode_attr = attr_init("mode", MODE_STEADY);
  chip->missed_threshold_attr = attr_init("missedThreshold", HCP2_WOKWI_DEFAULT_MISSED_POLL_THRESHOLD);
  chip->require_recovery_attr = attr_init("requireRecovery", 0);
  chip->pass_pin = pin_init("PASS", OUTPUT_LOW);
  chip->fail_pin = pin_init("FAIL", OUTPUT_LOW);

  memset(&uart_config, 0, sizeof(uart_config));
  uart_config.rx = pin_init("RX", INPUT);
  uart_config.tx = pin_init("TX", INPUT_PULLUP);
  uart_config.baud_rate = 57600;
  uart_config.rx_data = on_uart_rx_data;
  uart_config.write_done = on_uart_write_done;
  uart_config.user_data = chip;
  chip->uart = uart_init(&uart_config);

  memset(&timer_config, 0, sizeof(timer_config));
  timer_config.callback = on_timer;
  timer_config.user_data = chip;
  chip->timer = timer_init(&timer_config);

  chip->cycles_target = attr_read(chip->cycles_attr);
  if (chip->cycles_target == 0u) {
    chip->cycles_target = 200u;
  }
  chip->mode = attr_read(chip->mode_attr) == MODE_RESTART ? MODE_RESTART : MODE_STEADY;
  chip->missed_threshold = attr_read(chip->missed_threshold_attr);
  if (chip->missed_threshold == 0u) {
    chip->missed_threshold = HCP2_WOKWI_DEFAULT_MISSED_POLL_THRESHOLD;
  }
  chip->phase = PHASE_START;

  printf("HCP2_WOKWI_MASTER_READY mode=%u cycles=%u missed_threshold=%u\n", (unsigned) chip->mode,
         chip->cycles_target, chip->missed_threshold);
  timer_start(chip->timer, HCP2_WOKWI_START_DELAY_US, false);
}
