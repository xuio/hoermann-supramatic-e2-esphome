#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "hal/uart_types.h"
#include "hcp2_engine.h"
#include "hcp2_mailbox.h"
#include "soc/lp_uart_reg.h"
#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_uart.h"
#include "ulp_lp_core_utils.h"

#define HCP2_LP_UART_PORT LP_UART_NUM_0
#define HCP2_LP_DE_IO LP_IO_NUM_6
#define HCP2_LP_UART_FIFO_LEN 16u
#define HCP2_LP_LOOP_US 100u
#define HCP2_LP_RX_CHUNK 16u
#define HCP2_LP_TRACE_BOOT 1u
#define HCP2_LP_TRACE_TX 2u
#define HCP2_LP_TRACE_COMMAND 3u
#define HCP2_LP_TX_FLUSH_LIMIT 20000u

static hcp2_engine_t engine;
static uint32_t now_us;
static uint32_t active_epoch;
static uint32_t last_command_sequence;

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

static void port_de_set_(void *user, uint8_t enabled) {
  (void) user;
  ulp_lp_core_gpio_set_level(HCP2_LP_DE_IO, enabled ? 1u : 0u);
}

static void port_tx_(void *user, const uint8_t *data, uint8_t len) {
  uint8_t offset = 0u;
  uint32_t flush_wait = 0u;

  (void) user;
  while (offset < len) {
    const uint8_t accepted = lp_uart_tx_some_(data + offset, (uint8_t) (len - offset));
    if (accepted > 0) {
      offset = (uint8_t) (offset + accepted);
    } else {
      ulp_lp_core_delay_cycles(20u);
    }
  }
  while (!lp_uart_tx_idle_() && flush_wait < HCP2_LP_TX_FLUSH_LIMIT) {
    flush_wait++;
    ulp_lp_core_delay_cycles(20u);
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
  if (!hcp2_lp_mailbox_take_command(mailbox, active_epoch, &last_command_sequence, &command)) {
    return;
  }

  button = button_for_command_(command.command_id);
  if (button != HCP2_BUTTON_NONE) {
    (void) hcp2_engine_press_button(&engine, button);
    trace_(HCP2_LP_TRACE_COMMAND, (uint16_t) command.command_id);
  }
}

static void drain_uart_(void) {
  uint8_t rx[HCP2_LP_RX_CHUNK];
  int received;
  int i;

  do {
    received = lp_core_uart_read_bytes(HCP2_LP_UART_PORT, rx, sizeof(rx), 0);
    if (received > 0) {
      for (i = 0; i < received; i++) {
        hcp2_engine_rx_byte(&engine, rx[i], HCP2_RX_OK);
      }
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
  active_epoch = mailbox->command_epoch;
  last_command_sequence = mailbox->command_ack_sequence;
  trace_(HCP2_LP_TRACE_BOOT, 1u);
}

int main(void) {
  volatile hcp2_lp_mailbox_t *mailbox = mailbox_();

  init_();
  while (1) {
    mailbox->heartbeat++;
    drain_uart_();
    handle_mailbox_command_();
    hcp2_engine_poll(&engine);
    hcp2_lp_mailbox_publish_state(mailbox, hcp2_engine_drive_status(&engine), now_us);
    ulp_lp_core_delay_us(HCP2_LP_LOOP_US);
    now_us += HCP2_LP_LOOP_US;
  }

  return 0;
}
