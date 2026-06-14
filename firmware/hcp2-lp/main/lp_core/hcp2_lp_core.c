#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "hal/uart_types.h"
#include "hcp2_engine.h"
#include "hcp2_mailbox.h"
#include "riscv/csr.h"
#include "sdkconfig.h"
#include "soc/lp_uart_reg.h"
#include "soc/lp_wdt_reg.h"
#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_uart.h"
#include "ulp_lp_core_utils.h"

#define HCP2_LP_UART_PORT LP_UART_NUM_0
#define HCP2_LP_UART_RX_IO LP_IO_NUM_4
#define HCP2_LP_DE_IO LP_IO_NUM_0
#define HCP2_LP_RE_IO LP_IO_NUM_1
#define HCP2_LP_UART_FIFO_LEN 16u
#define HCP2_LP_UART_BAUD 57600u
#define HCP2_LP_UART_BITS_PER_BYTE 11u
#define HCP2_LP_CPU_CYCLES_PER_US 20u
#define HCP2_LP_LOOP_US 75u
#define HCP2_LP_RX_CHUNK 16u
#define HCP2_LP_TX_DEADMAN_US 8000u
#define HCP2_LP_TX_TAIL_MARGIN_US 250u
#define HCP2_LP_LOOP_OVERRUN_US 7000u
#define HCP2_LP_DE_STUCK_US 9000u
#define HCP2_LP_RX_STUCK_LOW_US 5000u
#define HCP2_LP_RX_FIFO_STARVATION_LEVEL 15u
#define HCP2_LP_WDT_WKEY 0x50D83AA1u
#define HCP2_LP_WDT_STAGE_RESET_CPU 2u
#define HCP2_LP_WDT_RESET_LENGTH_800_NS 5u

static hcp2_engine_t engine;
static uint32_t active_epoch;
static uint32_t last_command_sequence;
static uint8_t last_rx_gpio_level = 0xFFu;
static uint32_t tx_abort_count;
static uint32_t collision_count;
static uint32_t max_de_hold_us;
static uint32_t last_status_poll_count;
static uint32_t last_status_poll_us;
static uint32_t rx_error_count;
static uint8_t de_enabled;
static uint32_t de_asserted_us;
static uint8_t rx_line_seen_high;
static uint32_t rx_low_since_us;
static uint8_t rx_fifo_starvation_latched;
static uint16_t health_flags;
static uint16_t max_rx_fifo_count;
static uint32_t max_loop_us;
static uint32_t loop_overrun_count;
static uint32_t rx_starvation_count;
static uint32_t stuck_de_count;
static uint32_t mailbox_repair_count;
#if CONFIG_HCP2_BRINGUP_LP_WDT_ENABLE
static uint32_t lp_wdt_stall_after_us;
#endif

uint32_t hcp2_lp_now_us(void);
static uint32_t port_now_us_(void *user);
static uint8_t time_reached_(uint32_t now, uint32_t due);

#if CONFIG_HCP2_BRINGUP_WOKWI_LP_TX_PROBE
static uint8_t tx_probe_sent;
static const uint8_t k_wokwi_tx_probe_frame[] = {
    0x02, 0x17, 0x0A, 0x00, 0x00, 0x02, 0x05, 0x04, 0x30, 0x10, 0xFF, 0xA8, 0x55, 0x0F, 0x13,
};
#endif

static volatile hcp2_lp_mailbox_t *mailbox_(void) {
  return (volatile hcp2_lp_mailbox_t *) HCP2_LP_MAILBOX_ADDR;
}

static void trace_(uint16_t event, uint16_t value) {
  volatile hcp2_lp_mailbox_t *mailbox = mailbox_();
  const uint32_t index = mailbox->trace_head % HCP2_LP_TRACE_CAPACITY;

  mailbox->trace[index].at_us = port_now_us_(NULL);
  mailbox->trace[index].event = event;
  mailbox->trace[index].value = value;
  mailbox->trace_head++;
  if ((mailbox->trace_head - mailbox->trace_tail) > HCP2_LP_TRACE_CAPACITY) {
    mailbox->trace_tail = mailbox->trace_head - HCP2_LP_TRACE_CAPACITY;
  }
}

static uint64_t cycle_count_(void) {
  uint32_t high_before;
  uint32_t low;
  uint32_t high_after;

  do {
    high_before = RV_READ_CSR(mcycleh);
    low = RV_READ_CSR(mcycle);
    high_after = RV_READ_CSR(mcycleh);
  } while (high_before != high_after);

  return ((uint64_t) high_before << 32) | low;
}

__attribute__((noinline)) uint32_t hcp2_lp_now_us(void) {
  return (uint32_t) (cycle_count_() / HCP2_LP_CPU_CYCLES_PER_US);
}

static uint32_t port_now_us_(void *user) {
  (void) user;
  return hcp2_lp_now_us();
}

static uint32_t reg_read_(uint32_t addr) {
  return *(volatile uint32_t *) addr;
}

static void reg_write_(uint32_t addr, uint32_t value) {
  *(volatile uint32_t *) addr = value;
}

static void delay_us_(uint32_t us) {
  ulp_lp_core_delay_us(us);
}

#if CONFIG_HCP2_BRINGUP_LP_WDT_ENABLE
static void lp_wdt_unlock_(void) {
  reg_write_(LP_WDT_WPROTECT_REG, HCP2_LP_WDT_WKEY);
}

static void lp_wdt_lock_(void) {
  reg_write_(LP_WDT_WPROTECT_REG, 0u);
}

static void lp_wdt_feed_(void) {
  lp_wdt_unlock_();
  reg_write_(LP_WDT_FEED_REG, LP_WDT_RTC_WDT_FEED);
  lp_wdt_lock_();
}

static void lp_wdt_init_(void) {
  const uint32_t config0 = LP_WDT_WDT_EN |
                           (HCP2_LP_WDT_STAGE_RESET_CPU << LP_WDT_WDT_STG0_S) |
                           (HCP2_LP_WDT_RESET_LENGTH_800_NS << LP_WDT_WDT_CPU_RESET_LENGTH_S);

  lp_wdt_unlock_();
  reg_write_(LP_WDT_CONFIG0_REG, 0u);
  reg_write_(LP_WDT_CONFIG1_REG, (uint32_t) CONFIG_HCP2_BRINGUP_LP_WDT_TIMEOUT_TICKS);
  reg_write_(LP_WDT_CONFIG2_REG, 0u);
  reg_write_(LP_WDT_CONFIG3_REG, 0u);
  reg_write_(LP_WDT_CONFIG4_REG, 0u);
  reg_write_(LP_WDT_FEED_REG, LP_WDT_RTC_WDT_FEED);
  reg_write_(LP_WDT_CONFIG0_REG, config0);
  lp_wdt_lock_();
  if (CONFIG_HCP2_BRINGUP_LP_WDT_STALL_AFTER_MS > 0) {
    lp_wdt_stall_after_us = port_now_us_(NULL) + ((uint32_t) CONFIG_HCP2_BRINGUP_LP_WDT_STALL_AFTER_MS * 1000u);
  }
  trace_(HCP2_LP_TRACE_WDT, 1u);
}

static void lp_wdt_poll_(void) {
  if (lp_wdt_stall_after_us != 0u && time_reached_(port_now_us_(NULL), lp_wdt_stall_after_us)) {
    trace_(HCP2_LP_TRACE_WDT, 0xDEADu);
    return;
  }
  lp_wdt_feed_();
}
#endif

static uint8_t lp_uart_txfifo_count_(void) {
  return (uint8_t) ((reg_read_(LP_UART_STATUS_REG) >> LP_UART_TXFIFO_CNT_S) & LP_UART_TXFIFO_CNT_V);
}

static uint8_t lp_uart_rxfifo_count_(void) {
  return (uint8_t) ((reg_read_(LP_UART_STATUS_REG) >> LP_UART_RXFIFO_CNT_S) & LP_UART_RXFIFO_CNT_V);
}

static uint8_t lp_uart_tx_some_(const uint8_t *data, uint8_t len) {
  uint8_t count = lp_uart_txfifo_count_();
  uint8_t available;
  uint8_t accepted;
  uint8_t i;

  if (count > HCP2_LP_UART_FIFO_LEN) {
    count = HCP2_LP_UART_FIFO_LEN;
  }
  available = (uint8_t) (HCP2_LP_UART_FIFO_LEN - count);
  accepted = len < available ? len : available;
  for (i = 0u; i < accepted; i++) {
    reg_write_(LP_UART_FIFO_REG, data[i]);
  }
  return accepted;
}

static uint8_t lp_uart_tx_idle_(void) {
  const uint32_t fsm = (reg_read_(LP_UART_FSM_STATUS_REG) >> LP_UART_ST_UTX_OUT_S) & LP_UART_ST_UTX_OUT_V;
  return lp_uart_txfifo_count_() == 0u && fsm == 0u;
}

static uint8_t time_reached_(uint32_t now, uint32_t due) {
  return ((int32_t) (now - due)) >= 0 ? 1u : 0u;
}

static uint32_t tx_min_hold_us_(uint8_t len) {
  const uint32_t bits = (uint32_t) len * HCP2_LP_UART_BITS_PER_BYTE;
  return ((bits * 1000000u) + (HCP2_LP_UART_BAUD - 1u)) / HCP2_LP_UART_BAUD + HCP2_LP_TX_TAIL_MARGIN_US;
}

static void flush_stale_uart_rx_(void) {
  uint8_t rx[HCP2_LP_RX_CHUNK];
  int received;

  do {
    received = lp_core_uart_read_bytes(HCP2_LP_UART_PORT, rx, sizeof(rx), 0);
    if (received > 0) {
      trace_(HCP2_LP_TRACE_RX_ECHO, (uint16_t) (0xF000u | (uint8_t) received));
    } else if (received < 0) {
      rx_error_count++;
      trace_(HCP2_LP_TRACE_RX_ERROR, (uint16_t) (reg_read_(LP_UART_INT_RAW_REG) & 0xFFFFu));
    }
  } while (received > 0);
}

static void repair_mailbox_header_if_needed_(void) {
  volatile hcp2_lp_mailbox_t *mailbox = mailbox_();

  if (mailbox->magic == HCP2_LP_MAILBOX_MAGIC &&
      mailbox->abi_version == HCP2_LP_MAILBOX_ABI_VERSION &&
      mailbox->struct_size == HCP2_LP_MAILBOX_SIZE &&
      mailbox->firmware_version == HCP2_LP_FIRMWARE_VERSION) {
    return;
  }

  hcp2_lp_mailbox_repair_header(mailbox);
  mailbox_repair_count++;
  health_flags = (uint16_t) (health_flags | HCP2_LP_HEALTH_FLAG_MAILBOX_REPAIR);
  trace_(HCP2_LP_TRACE_HEALTH, 0xA000u);
}

static void sample_rx_fifo_health_(void) {
  const uint8_t count = lp_uart_rxfifo_count_();

  if (count > max_rx_fifo_count) {
    max_rx_fifo_count = count;
  }
  if (count >= HCP2_LP_RX_FIFO_STARVATION_LEVEL) {
    if (!rx_fifo_starvation_latched) {
      rx_fifo_starvation_latched = 1u;
      rx_starvation_count++;
      health_flags = (uint16_t) (health_flags | HCP2_LP_HEALTH_FLAG_RX_STARVATION);
      trace_(HCP2_LP_TRACE_HEALTH, (uint16_t) (0xB000u | count));
    }
  } else if (count < (HCP2_LP_RX_FIFO_STARVATION_LEVEL - 1u)) {
    rx_fifo_starvation_latched = 0u;
  }
}

static void check_de_deadman_(uint32_t now) {
  if (!de_enabled) {
    return;
  }
  if (!time_reached_(now, de_asserted_us + HCP2_LP_DE_STUCK_US)) {
    return;
  }

  stuck_de_count++;
  health_flags = (uint16_t) (health_flags | HCP2_LP_HEALTH_FLAG_STUCK_DE);
  trace_(HCP2_LP_TRACE_HEALTH, 0xD000u);
  ulp_lp_core_gpio_set_level(HCP2_LP_RE_IO, 0u);
  ulp_lp_core_gpio_set_level(HCP2_LP_DE_IO, 0u);
  de_enabled = 0u;
  trace_(HCP2_LP_TRACE_DE, 0u);
}

static void port_de_set_(void *user, uint8_t enabled) {
  (void) user;
  if (enabled) {
    flush_stale_uart_rx_();
    ulp_lp_core_gpio_set_level(HCP2_LP_DE_IO, 1u);
    ulp_lp_core_gpio_set_level(HCP2_LP_RE_IO, 1u);
    de_enabled = 1u;
    de_asserted_us = port_now_us_(NULL);
  } else {
    flush_stale_uart_rx_();
    ulp_lp_core_gpio_set_level(HCP2_LP_RE_IO, 0u);
    ulp_lp_core_gpio_set_level(HCP2_LP_DE_IO, 0u);
    de_enabled = 0u;
  }
  trace_(HCP2_LP_TRACE_DE, enabled ? 1u : 0u);
}

static void port_tx_(void *user, const uint8_t *data, uint8_t len) {
  uint8_t offset = 0u;
  const uint32_t start_us = port_now_us_(NULL);
  const uint32_t deadline_us = start_us + HCP2_LP_TX_DEADMAN_US;
  const uint32_t min_done_us = start_us + tx_min_hold_us_(len);
  uint8_t aborted = 0u;

  (void) user;
  while (offset < len) {
    if (time_reached_(port_now_us_(NULL), deadline_us)) {
      aborted = 1u;
      break;
    }
    const uint8_t accepted = lp_uart_tx_some_(data + offset, (uint8_t) (len - offset));
    if (accepted > 0) {
      offset = (uint8_t) (offset + accepted);
    } else {
      ulp_lp_core_delay_cycles(20u);
    }
  }
  while (!aborted && !time_reached_(port_now_us_(NULL), min_done_us)) {
    if (time_reached_(port_now_us_(NULL), deadline_us)) {
      aborted = 1u;
      break;
    }
    ulp_lp_core_delay_cycles(20u);
  }
  while (!aborted && !lp_uart_tx_idle_()) {
    if (time_reached_(port_now_us_(NULL), deadline_us)) {
      aborted = 1u;
      break;
    }
    ulp_lp_core_delay_cycles(20u);
  }
  if (aborted) {
    tx_abort_count++;
    trace_(HCP2_LP_TRACE_TX_ABORT, len);
    port_de_set_(NULL, 0u);
  }
  const uint32_t held_us = port_now_us_(NULL) - start_us;
  if (held_us > max_de_hold_us) {
    max_de_hold_us = held_us;
  }
  reg_write_(LP_UART_INT_CLR_REG, LP_UART_TX_DONE_INT_RAW | LP_UART_PARITY_ERR_INT_RAW | LP_UART_FRM_ERR_INT_RAW);
  trace_(HCP2_LP_TRACE_TX, len);
}

static hcp2_button_t button_for_command_(uint8_t command_id) {
  switch (command_id) {
    case HCP2_LP_COMMAND_OPEN:
      return HCP2_BUTTON_OPEN;
    case HCP2_LP_COMMAND_CLOSE:
      return HCP2_BUTTON_CLOSE;
    case HCP2_LP_COMMAND_STOP:
      return HCP2_BUTTON_STOP;
    case HCP2_LP_COMMAND_VENT:
      return HCP2_BUTTON_VENT;
    case HCP2_LP_COMMAND_HALF:
      return HCP2_BUTTON_HALF;
    case HCP2_LP_COMMAND_LIGHT:
      return HCP2_BUTTON_LIGHT;
    default:
      return HCP2_BUTTON_NONE;
  }
}

static void handle_mailbox_command_(void) {
  hcp2_lp_command_t command;
  hcp2_button_t button;
  volatile hcp2_lp_mailbox_t *mailbox = mailbox_();

  if (mailbox->command_sequence == 0u) {
    active_epoch = mailbox->command_epoch;
    last_command_sequence = 0u;
    return;
  }
  if (!hcp2_lp_mailbox_take_command(mailbox, active_epoch, &last_command_sequence, port_now_us_(NULL), &command)) {
    return;
  }

  button = button_for_command_(command.command_id);
  if (button != HCP2_BUTTON_NONE) {
    if (hcp2_engine_press_button(&engine, button)) {
      hcp2_lp_mailbox_ack_command(mailbox, command.sequence, HCP2_LP_COMMAND_RESULT_EXECUTED);
      trace_(HCP2_LP_TRACE_COMMAND, (uint16_t) command.command_id);
    } else {
      hcp2_lp_mailbox_ack_command(mailbox, command.sequence, HCP2_LP_COMMAND_RESULT_BUSY);
    }
  } else {
    hcp2_lp_mailbox_ack_command(mailbox, command.sequence, HCP2_LP_COMMAND_RESULT_UNKNOWN);
  }
}

static void trace_rx_gpio_(uint32_t now) {
  const uint8_t level = (uint8_t) (ulp_lp_core_gpio_get_level(HCP2_LP_UART_RX_IO) & 1u);

  if (level != last_rx_gpio_level) {
    last_rx_gpio_level = level;
    trace_(HCP2_LP_TRACE_GPIO_RX, level);
  }
  if (level != 0u) {
    rx_line_seen_high = 1u;
  }
  if (!rx_line_seen_high) {
    return;
  }
  if (level == 0u) {
    if (rx_low_since_us == 0u) {
      rx_low_since_us = now;
    } else if (time_reached_(now, rx_low_since_us + HCP2_LP_RX_STUCK_LOW_US)) {
      rx_low_since_us = now;
      rx_starvation_count++;
      health_flags = (uint16_t) (health_flags | HCP2_LP_HEALTH_FLAG_RX_STARVATION);
      trace_(HCP2_LP_TRACE_HEALTH, 0xB100u);
    }
  } else {
    rx_low_since_us = 0u;
  }
}

#if CONFIG_HCP2_BRINGUP_WOKWI_LP_TX_PROBE
static void run_tx_probe_(void) {
  if (tx_probe_sent || port_now_us_(NULL) < 500000u) {
    return;
  }
  tx_probe_sent = 1u;
  port_de_set_(NULL, 1u);
  port_tx_(NULL, k_wokwi_tx_probe_frame, sizeof(k_wokwi_tx_probe_frame));
  port_de_set_(NULL, 0u);
}
#endif

static void drain_uart_(void) {
  uint8_t rx[HCP2_LP_RX_CHUNK];
  int received;
  int i;

  do {
    sample_rx_fifo_health_();
    received = lp_core_uart_read_bytes(HCP2_LP_UART_PORT, rx, sizeof(rx), 0);
    if (received > 0) {
      for (i = 0; i < received; i++) {
        trace_(HCP2_LP_TRACE_RX, rx[i]);
        hcp2_engine_rx_byte(&engine, rx[i], HCP2_RX_OK);
      }
    } else if (received < 0) {
      rx_error_count++;
      trace_(HCP2_LP_TRACE_RX_ERROR, (uint16_t) (reg_read_(LP_UART_INT_RAW_REG) & 0xFFFFu));
    }
  } while (received > 0);
  sample_rx_fifo_health_();
}

static uint8_t stop_trigger_crossed_(const hcp2_drive_status_t *status, uint8_t target_position) {
  if (status == NULL) {
    return 0u;
  }
  switch ((hcp2_drive_state_code_t) status->state) {
    case HCP2_DRIVE_OPENING:
    case HCP2_DRIVE_HALF_OPENING:
    case HCP2_DRIVE_VENT_MOVING:
      return status->current_position >= target_position ? 1u : 0u;
    case HCP2_DRIVE_CLOSING:
      return status->current_position <= target_position ? 1u : 0u;
    default:
      return 0u;
  }
}

static void handle_stop_trigger_(void) {
  hcp2_lp_stop_trigger_t trigger;
  volatile hcp2_lp_mailbox_t *mailbox = mailbox_();
  const uint32_t now = port_now_us_(NULL);

  if (!hcp2_lp_mailbox_read_stop_trigger(mailbox, &trigger)) {
    return;
  }
  if (trigger.epoch != active_epoch) {
    hcp2_lp_mailbox_disarm_stop_trigger(mailbox);
    trace_(HCP2_LP_TRACE_STOP_TRIGGER, 0xE000u);
    return;
  }
  if (trigger.deadline_us != 0u && time_reached_(now, trigger.deadline_us)) {
    hcp2_lp_mailbox_disarm_stop_trigger(mailbox);
    trace_(HCP2_LP_TRACE_STOP_TRIGGER, (uint16_t) (0xE100u | trigger.target_position));
    return;
  }
  if (!stop_trigger_crossed_(hcp2_engine_drive_status(&engine), trigger.target_position)) {
    return;
  }
  if (hcp2_engine_press_button(&engine, HCP2_BUTTON_STOP)) {
    hcp2_lp_mailbox_mark_stop_trigger_fired(mailbox);
    trace_(HCP2_LP_TRACE_STOP_TRIGGER, (uint16_t) (0x5000u | trigger.target_position));
  }
}

static void init_(void) {
  hcp2_port_t port;
  hcp2_engine_config_t config;
  volatile hcp2_lp_mailbox_t *mailbox = mailbox_();
  const uint32_t pending_sequence = mailbox->command_sequence;

  memset(&port, 0, sizeof(port));
  port.now_us = port_now_us_;
  port.tx = port_tx_;
  port.de_set = port_de_set_;

  hcp2_engine_config_default(&config);
  hcp2_engine_init(&engine, &port, &config);
  ulp_lp_core_gpio_init(HCP2_LP_DE_IO);
  ulp_lp_core_gpio_output_enable(HCP2_LP_DE_IO);
  ulp_lp_core_gpio_set_level(HCP2_LP_DE_IO, 0u);
  ulp_lp_core_gpio_init(HCP2_LP_RE_IO);
  ulp_lp_core_gpio_output_enable(HCP2_LP_RE_IO);
  ulp_lp_core_gpio_set_level(HCP2_LP_RE_IO, 0u);
  flush_stale_uart_rx_();
  active_epoch = mailbox->command_epoch;
  last_command_sequence = pending_sequence;
  if (pending_sequence != 0u && mailbox->command_ack_sequence != pending_sequence) {
    hcp2_lp_mailbox_ack_command(mailbox, pending_sequence, HCP2_LP_COMMAND_RESULT_EXPIRED);
    trace_(HCP2_LP_TRACE_COMMAND, (uint16_t) (0xEE00u | (mailbox->command_id & 0xFFu)));
  }
  mailbox->lp_reset_count++;
  trace_(HCP2_LP_TRACE_BOOT, 1u);
#if CONFIG_HCP2_BRINGUP_LP_WDT_ENABLE
  lp_wdt_init_();
#endif
}

int main(void) {
  volatile hcp2_lp_mailbox_t *mailbox = mailbox_();

  init_();
  while (1) {
    const uint32_t loop_start_us = port_now_us_(NULL);
    mailbox->heartbeat++;
    repair_mailbox_header_if_needed_();
    trace_rx_gpio_(loop_start_us);
    drain_uart_();
    if (engine.status_polls_received != last_status_poll_count) {
      last_status_poll_count = engine.status_polls_received;
      last_status_poll_us = port_now_us_(NULL);
    }
#if CONFIG_HCP2_BRINGUP_WOKWI_LP_TX_PROBE
    run_tx_probe_();
#endif
    handle_stop_trigger_();
    handle_mailbox_command_();
    hcp2_engine_poll(&engine);
    hcp2_lp_mailbox_publish_state(mailbox, hcp2_engine_drive_status(&engine), port_now_us_(NULL));
    check_de_deadman_(port_now_us_(NULL));
    const uint32_t loop_active_us = port_now_us_(NULL) - loop_start_us;
    if (loop_active_us > max_loop_us) {
      max_loop_us = loop_active_us;
    }
    if (loop_active_us > HCP2_LP_LOOP_OVERRUN_US) {
      loop_overrun_count++;
      health_flags = (uint16_t) (health_flags | HCP2_LP_HEALTH_FLAG_LOOP_OVERRUN);
      trace_(HCP2_LP_TRACE_HEALTH, (uint16_t) (0xC000u | (loop_active_us & 0x0FFFu)));
    }
    hcp2_lp_mailbox_publish_counters(mailbox, port_now_us_(NULL), engine.status_polls_received,
                                     engine.status_responses_sent, tx_abort_count, collision_count, max_de_hold_us,
                                     last_status_poll_us, engine.crc_errors, engine.rx_errors + rx_error_count,
                                     max_loop_us, loop_overrun_count, rx_starvation_count, stuck_de_count,
                                     mailbox_repair_count, health_flags, max_rx_fifo_count,
                                     engine.max_status_poll_rx_to_schedule_us,
                                     engine.max_status_response_schedule_to_tx_start_us,
                                     engine.max_status_response_tx_us);
#if CONFIG_HCP2_BRINGUP_LP_WDT_ENABLE
    lp_wdt_poll_();
#endif
    delay_us_(HCP2_LP_LOOP_US);
  }

  return 0;
}
