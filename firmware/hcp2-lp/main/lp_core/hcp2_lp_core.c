#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "hal/uart_types.h"
#include "hcp2_engine.h"
#include "hcp2_mailbox.h"
#include "sdkconfig.h"
#include "soc/lp_uart_reg.h"
#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_uart.h"
#include "ulp_lp_core_utils.h"

#define HCP2_LP_UART_PORT LP_UART_NUM_0
#define HCP2_LP_UART_RX_IO LP_IO_NUM_4
#define HCP2_LP_DE_IO LP_IO_NUM_2
#define HCP2_LP_RE_IO LP_IO_NUM_3
#define HCP2_LP_UART_FIFO_LEN 16u
#define HCP2_LP_LOOP_US 100u
#define HCP2_LP_RX_CHUNK 16u
#define HCP2_LP_TX_FLUSH_LIMIT 20000u

static hcp2_engine_t engine;
static uint32_t now_us;
static uint32_t active_epoch;
static uint32_t last_command_sequence;
static uint8_t last_rx_gpio_level = 0xFFu;

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

  mailbox->trace[index].at_us = now_us;
  mailbox->trace[index].event = event;
  mailbox->trace[index].value = value;
  mailbox->trace_head++;
  if ((mailbox->trace_head - mailbox->trace_tail) > HCP2_LP_TRACE_CAPACITY) {
    mailbox->trace_tail = mailbox->trace_head - HCP2_LP_TRACE_CAPACITY;
  }
}

static uint32_t port_now_us_(void *user) {
  (void) user;
  return now_us;
}

static uint32_t reg_read_(uint32_t addr) {
  return *(volatile uint32_t *) addr;
}

static void reg_write_(uint32_t addr, uint32_t value) {
  *(volatile uint32_t *) addr = value;
}

static void delay_us_(uint32_t us) {
  ulp_lp_core_delay_us(us);
  now_us += us;
}

static uint8_t lp_uart_txfifo_count_(void) {
  return (uint8_t) ((reg_read_(LP_UART_STATUS_REG) >> LP_UART_TXFIFO_CNT_S) & LP_UART_TXFIFO_CNT_V);
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

static void drain_uart_echo_(const uint8_t *expected, uint8_t len, uint8_t *offset) {
  uint8_t rx[HCP2_LP_RX_CHUNK];
  int received;
  int i;

  do {
    received = lp_core_uart_read_bytes(HCP2_LP_UART_PORT, rx, sizeof(rx), 0);
    if (received > 0) {
      for (i = 0; i < received; i++) {
        const uint8_t byte = rx[i];
        if (*offset < len && byte == expected[*offset]) {
          *offset = (uint8_t) (*offset + 1u);
          trace_(HCP2_LP_TRACE_RX_ECHO, byte);
        } else {
          trace_(HCP2_LP_TRACE_RX_ERROR, (uint16_t) (0xE000u | byte));
        }
      }
    } else if (received < 0) {
      trace_(HCP2_LP_TRACE_RX_ERROR, (uint16_t) (reg_read_(LP_UART_INT_RAW_REG) & 0xFFFFu));
    }
  } while (received > 0);
}

static void flush_stale_uart_rx_(void) {
  uint8_t rx[HCP2_LP_RX_CHUNK];
  int received;

  do {
    received = lp_core_uart_read_bytes(HCP2_LP_UART_PORT, rx, sizeof(rx), 0);
    if (received > 0) {
      trace_(HCP2_LP_TRACE_RX_ECHO, (uint16_t) (0xF000u | (uint8_t) received));
    } else if (received < 0) {
      trace_(HCP2_LP_TRACE_RX_ERROR, (uint16_t) (reg_read_(LP_UART_INT_RAW_REG) & 0xFFFFu));
    }
  } while (received > 0);
}

static void port_de_set_(void *user, uint8_t enabled) {
  (void) user;
  ulp_lp_core_gpio_set_level(HCP2_LP_DE_IO, enabled ? 1u : 0u);
  trace_(HCP2_LP_TRACE_DE, enabled ? 1u : 0u);
}

static void port_tx_(void *user, const uint8_t *data, uint8_t len) {
  uint8_t offset = 0u;
  uint8_t echo_offset = 0u;
  uint32_t flush_wait = 0u;

  (void) user;
  while (offset < len) {
    const uint8_t accepted = lp_uart_tx_some_(data + offset, (uint8_t) (len - offset));
    if (accepted > 0) {
      offset = (uint8_t) (offset + accepted);
    } else {
      drain_uart_echo_(data, len, &echo_offset);
      ulp_lp_core_delay_cycles(20u);
    }
    drain_uart_echo_(data, len, &echo_offset);
  }
  while (!lp_uart_tx_idle_() && flush_wait < HCP2_LP_TX_FLUSH_LIMIT) {
    drain_uart_echo_(data, len, &echo_offset);
    flush_wait++;
    ulp_lp_core_delay_cycles(20u);
  }
  drain_uart_echo_(data, len, &echo_offset);
  delay_us_(100u);
  drain_uart_echo_(data, len, &echo_offset);
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
  if (!hcp2_lp_mailbox_take_command(mailbox, active_epoch, &last_command_sequence, &command)) {
    return;
  }

  button = button_for_command_(command.command_id);
  if (button != HCP2_BUTTON_NONE) {
    (void) hcp2_engine_press_button(&engine, button);
    trace_(HCP2_LP_TRACE_COMMAND, (uint16_t) command.command_id);
  }
}

static void trace_rx_gpio_(void) {
  const uint8_t level = (uint8_t) (ulp_lp_core_gpio_get_level(HCP2_LP_UART_RX_IO) & 1u);

  if (level == last_rx_gpio_level) {
    return;
  }
  last_rx_gpio_level = level;
  trace_(HCP2_LP_TRACE_GPIO_RX, level);
}

#if CONFIG_HCP2_BRINGUP_WOKWI_LP_TX_PROBE
static void run_tx_probe_(void) {
  if (tx_probe_sent || now_us < 500000u) {
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
    received = lp_core_uart_read_bytes(HCP2_LP_UART_PORT, rx, sizeof(rx), 0);
    if (received > 0) {
      for (i = 0; i < received; i++) {
        trace_(HCP2_LP_TRACE_RX, rx[i]);
        hcp2_engine_rx_byte(&engine, rx[i], HCP2_RX_OK);
      }
    } else if (received < 0) {
      trace_(HCP2_LP_TRACE_RX_ERROR, (uint16_t) (reg_read_(LP_UART_INT_RAW_REG) & 0xFFFFu));
    }
  } while (received > 0);
}

static void init_(void) {
  hcp2_port_t port;
  hcp2_engine_config_t config;
  volatile hcp2_lp_mailbox_t *mailbox = mailbox_();

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
  last_command_sequence = mailbox->command_ack_sequence;
  trace_(HCP2_LP_TRACE_BOOT, 1u);
}

int main(void) {
  volatile hcp2_lp_mailbox_t *mailbox = mailbox_();

  init_();
  while (1) {
    mailbox->heartbeat++;
    trace_rx_gpio_();
    drain_uart_();
#if CONFIG_HCP2_BRINGUP_WOKWI_LP_TX_PROBE
    run_tx_probe_();
#endif
    handle_mailbox_command_();
    hcp2_engine_poll(&engine);
    hcp2_lp_mailbox_publish_state(mailbox, hcp2_engine_drive_status(&engine), now_us);
    delay_us_(HCP2_LP_LOOP_US);
  }

  return 0;
}
