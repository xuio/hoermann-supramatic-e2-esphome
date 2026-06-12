#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcp2_crc.h"
#include "hcp2_engine.h"
#include "hcp2_frame.h"
#include "hcp2_mailbox.h"

#define ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))

typedef struct {
  uint32_t now_us;
  uint8_t tx[16][HCP2_MAX_FRAME_LEN];
  uint8_t tx_len[16];
  uint8_t tx_count;
  uint8_t de_events[32];
  uint8_t de_event_count;
} test_port_t;

static uint32_t test_now_us(void *user) {
  return ((test_port_t *) user)->now_us;
}

static void test_tx(void *user, const uint8_t *data, uint8_t len) {
  test_port_t *port = (test_port_t *) user;
  assert(port->tx_count < ARRAY_LEN(port->tx));
  assert(len <= HCP2_MAX_FRAME_LEN);
  memcpy(port->tx[port->tx_count], data, len);
  port->tx_len[port->tx_count] = len;
  port->tx_count++;
}

static void test_de_set(void *user, uint8_t enabled) {
  test_port_t *port = (test_port_t *) user;
  assert(port->de_event_count < ARRAY_LEN(port->de_events));
  port->de_events[port->de_event_count++] = enabled;
}

static hcp2_port_t make_port(test_port_t *test_port) {
  hcp2_port_t port;
  memset(&port, 0, sizeof(port));
  port.user = test_port;
  port.now_us = test_now_us;
  port.tx = test_tx;
  port.de_set = test_de_set;
  return port;
}

static uint8_t hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return (uint8_t) (c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return (uint8_t) (10 + c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return (uint8_t) (10 + c - 'A');
  }
  assert(0 && "invalid hex digit");
  return 0;
}

static uint8_t parse_hex(const char *hex, uint8_t *out) {
  uint8_t len = 0;
  while (*hex != '\0') {
    if (*hex == ' ' || *hex == '\t' || *hex == '\n' || *hex == '\r') {
      hex++;
      continue;
    }
    assert(hex[0] != '\0' && hex[1] != '\0');
    assert(len < HCP2_MAX_FRAME_LEN);
    out[len++] = (uint8_t) ((hex_value(hex[0]) << 4) | hex_value(hex[1]));
    hex += 2;
  }
  return len;
}

static void expect_hex(const uint8_t *actual, uint8_t actual_len, const char *expected_hex) {
  uint8_t expected[HCP2_MAX_FRAME_LEN];
  const uint8_t expected_len = parse_hex(expected_hex, expected);
  assert(actual_len == expected_len);
  assert(memcmp(actual, expected, expected_len) == 0);
}

static void feed_hex(hcp2_engine_t *engine, const char *hex) {
  uint8_t data[HCP2_MAX_FRAME_LEN];
  const uint8_t len = parse_hex(hex, data);
  uint8_t i;
  for (i = 0; i < len; i++) {
    hcp2_engine_rx_byte(engine, data[i], HCP2_RX_OK);
  }
}

static uint8_t build_status_poll(uint8_t counter, uint8_t *out) {
  static const uint8_t prefix[] = {
      0x02, 0x17, 0x9C, 0xB9, 0x00, 0x08, 0x9C, 0x41, 0x00, 0x02, 0x04};
  memcpy(out, prefix, sizeof(prefix));
  out[11] = counter;
  out[12] = 0x03;
  out[13] = 0x00;
  out[14] = 0x00;
  hcp2_crc16_append(out, 15);
  return 17;
}

static void feed_bytes(hcp2_engine_t *engine, const uint8_t *data, uint8_t len) {
  uint8_t i;
  for (i = 0; i < len; i++) {
    hcp2_engine_rx_byte(engine, data[i], HCP2_RX_OK);
  }
}

static void test_vector_crc_file(void) {
  FILE *file = fopen("vectors/reference_frames.jsonl", "r");
  char line[512];
  unsigned int count = 0;

  assert(file != NULL);
  while (fgets(line, sizeof(line), file) != NULL) {
    const char *raw = strstr(line, "\"raw\":\"");
    uint8_t frame[HCP2_MAX_FRAME_LEN];
    uint8_t len = 0;

    if (raw == NULL) {
      continue;
    }
    raw += strlen("\"raw\":\"");
    while (*raw != '\0' && *raw != '"') {
      assert(len < HCP2_MAX_FRAME_LEN);
      frame[len++] = (uint8_t) ((hex_value(raw[0]) << 4) | hex_value(raw[1]));
      raw += 2;
    }
    assert(len >= 3);
    assert(hcp2_crc16_check(frame, len));
    count++;
  }
  fclose(file);
  assert(count == 34);
}

static void test_crc_known_frame(void) {
  uint8_t frame[HCP2_MAX_FRAME_LEN];
  const uint8_t len = parse_hex("02179CB900089C410002043E030000EBCC", frame);
  assert(hcp2_crc16_modbus(frame, (uint16_t) (len - 2)) == 0xCCEBu);
  assert(hcp2_crc16_check(frame, len));
}

static void test_builders(void) {
  uint8_t out[HCP2_MAX_FRAME_LEN];
  uint8_t signature[HCP2_SIGNATURE_LEN];
  hcp2_default_signature(signature);

  assert(hcp2_frame_build_scan_response(2, signature, out) == HCP2_SCAN_RESPONSE_LEN);
  expect_hex(out, HCP2_SCAN_RESPONSE_LEN, "02170A00000205043010FFA8550F13");

  assert(hcp2_frame_build_status_response(2, 0x3E, 0x03, HCP2_BUTTON_NONE, 0, out) ==
         HCP2_STATUS_RESPONSE_LEN);
  expect_hex(out, HCP2_STATUS_RESPONSE_LEN, "0217103E000301000000000000000000000000741B");

  assert(hcp2_frame_build_command_response(2, 0x02, 0x04, out) == HCP2_COMMAND_RESPONSE_LEN);
  expect_hex(out, HCP2_COMMAND_RESPONSE_LEN, "021704020004FD08DE");
}

static void test_bus_scan_response(void) {
  test_port_t test_port;
  hcp2_port_t port;
  hcp2_engine_config_t config;
  hcp2_engine_t engine;

  memset(&test_port, 0, sizeof(test_port));
  port = make_port(&test_port);
  hcp2_engine_config_default(&config);
  hcp2_engine_init(&engine, &port, &config);

  feed_hex(&engine, "02179CB900059C41000306000200000102F835");
  assert(test_port.tx_count == 0);
  test_port.now_us = HCP2_DEFAULT_RESPONSE_DELAY_US - 1u;
  hcp2_engine_poll(&engine);
  assert(test_port.tx_count == 0);
  test_port.now_us = HCP2_DEFAULT_RESPONSE_DELAY_US;
  hcp2_engine_poll(&engine);
  assert(test_port.tx_count == 1);
  expect_hex(test_port.tx[0], test_port.tx_len[0], "02170A00000205043010FFA8550F13");
  assert(test_port.de_event_count == 2);
  assert(test_port.de_events[0] == 1);
  assert(test_port.de_events[1] == 0);
}

static void test_status_poll_idle_response(void) {
  test_port_t test_port;
  hcp2_port_t port;
  hcp2_engine_config_t config;
  hcp2_engine_t engine;

  memset(&test_port, 0, sizeof(test_port));
  port = make_port(&test_port);
  hcp2_engine_config_default(&config);
  config.response_delay_us = 0;
  hcp2_engine_init(&engine, &port, &config);

  feed_hex(&engine, "02179CB900089C410002043E030000EBCC");
  assert(engine.status_polls_received == 1u);
  hcp2_engine_poll(&engine);
  assert(engine.status_responses_sent == 1u);
  assert(test_port.tx_count == 1);
  expect_hex(test_port.tx[0], test_port.tx_len[0], "0217103E000301000000000000000000000000741B");
}

static void test_light_command_response(void) {
  test_port_t test_port;
  hcp2_port_t port;
  hcp2_engine_config_t config;
  hcp2_engine_t engine;

  memset(&test_port, 0, sizeof(test_port));
  port = make_port(&test_port);
  hcp2_engine_config_default(&config);
  config.response_delay_us = 0;
  hcp2_engine_init(&engine, &port, &config);

  feed_hex(&engine, "02179CB900029C4100020402041700798D");
  hcp2_engine_poll(&engine);
  assert(test_port.tx_count == 1);
  expect_hex(test_port.tx[0], test_port.tx_len[0], "021704020004FD08DE");
}

static void test_broadcast_decode(void) {
  test_port_t test_port;
  hcp2_port_t port;
  hcp2_engine_t engine;
  const hcp2_drive_status_t *state;

  memset(&test_port, 0, sizeof(test_port));
  port = make_port(&test_port);
  hcp2_engine_init(&engine, &port, NULL);

  feed_hex(&engine, "00109D310009123000C8C8206000000000000000140001000055A1");
  hcp2_engine_poll(&engine);
  assert(test_port.tx_count == 0);
  state = hcp2_engine_drive_status(&engine);
  assert(state != NULL);
  assert(state->target_position == 0xC8);
  assert(state->current_position == 0xC8);
  assert(state->state == HCP2_DRIVE_OPEN);
  assert(state->light_raw == 0x14);
  assert(state->light_on == 1);

  feed_hex(&engine, "00109D310009123D000000406000000000000000000001000008AD");
  state = hcp2_engine_drive_status(&engine);
  assert(state->state == HCP2_DRIVE_CLOSED);
  assert(state->light_raw == 0x00);
  assert(state->light_on == 0);
}

static void test_button_press_release_sequence(void) {
  test_port_t test_port;
  hcp2_port_t port;
  hcp2_engine_config_t config;
  hcp2_engine_t engine;
  uint8_t request[HCP2_MAX_FRAME_LEN];
  uint8_t request_len;

  memset(&test_port, 0, sizeof(test_port));
  port = make_port(&test_port);
  hcp2_engine_config_default(&config);
  config.response_delay_us = 0;
  config.button_press_us = 100000;
  hcp2_engine_init(&engine, &port, &config);

  assert(hcp2_engine_press_button(&engine, HCP2_BUTTON_OPEN));

  request_len = build_status_poll(0x70, request);
  feed_bytes(&engine, request, request_len);
  hcp2_engine_poll(&engine);
  assert(test_port.tx_count == 1);
  expect_hex(test_port.tx[0], test_port.tx_len[0], "021710700003010210000000000000000000006C88");

  test_port.now_us = 100000;
  request_len = build_status_poll(0x78, request);
  feed_bytes(&engine, request, request_len);
  hcp2_engine_poll(&engine);
  assert(test_port.tx_count == 2);
  expect_hex(test_port.tx[1], test_port.tx_len[1], "021710780003010110000000000000000000006F4A");

  request_len = build_status_poll(0x79, request);
  feed_bytes(&engine, request, request_len);
  hcp2_engine_poll(&engine);
  assert(test_port.tx_count == 3);
  assert(test_port.tx[2][7] == 0x00);
  assert(test_port.tx[2][8] == 0x00);
  assert(test_port.tx[2][9] == 0x00);
}

static void test_bad_crc_no_response_and_recovery(void) {
  test_port_t test_port;
  hcp2_port_t port;
  hcp2_engine_config_t config;
  hcp2_engine_t engine;

  memset(&test_port, 0, sizeof(test_port));
  port = make_port(&test_port);
  hcp2_engine_config_default(&config);
  config.response_delay_us = 0;
  hcp2_engine_init(&engine, &port, &config);

  feed_hex(&engine, "02179CB900089C410002043E030000EB00");
  hcp2_engine_poll(&engine);
  assert(test_port.tx_count == 0);
  assert(engine.crc_errors > 0);

  feed_hex(&engine, "02179CB900089C410002043E030000EBCC");
  hcp2_engine_poll(&engine);
  assert(test_port.tx_count == 1);
  expect_hex(test_port.tx[0], test_port.tx_len[0], "0217103E000301000000000000000000000000741B");
}

static void test_rx_error_resets_partial_frame(void) {
  test_port_t test_port;
  hcp2_port_t port;
  hcp2_engine_t engine;

  memset(&test_port, 0, sizeof(test_port));
  port = make_port(&test_port);
  hcp2_engine_init(&engine, &port, NULL);

  hcp2_engine_rx_byte(&engine, 0x02, HCP2_RX_OK);
  hcp2_engine_rx_byte(&engine, 0x17, HCP2_RX_OK);
  hcp2_engine_rx_byte(&engine, 0x9C, HCP2_RX_PARITY_ERROR);
  feed_hex(&engine, "02179CB900089C410002043E030000EBCC");
  test_port.now_us = HCP2_DEFAULT_RESPONSE_DELAY_US;
  hcp2_engine_poll(&engine);
  assert(test_port.tx_count == 1);
}

static void test_mailbox_layout_and_reload_decision(void) {
  hcp2_lp_mailbox_t mailbox;
  hcp2_lp_health_sample_t before;
  hcp2_lp_health_sample_t after;

  hcp2_lp_mailbox_init(&mailbox);
  assert(HCP2_LP_MAILBOX_ADDR == 0x50002000u);
  assert(sizeof(mailbox) == 512u);
  assert(mailbox.magic == HCP2_LP_MAILBOX_MAGIC);
  assert(mailbox.abi_version == HCP2_LP_MAILBOX_ABI_VERSION);
  assert(mailbox.struct_size == HCP2_LP_MAILBOX_SIZE);
  assert(mailbox.firmware_version == HCP2_LP_FIRMWARE_VERSION);

  hcp2_lp_mailbox_sample_health(&mailbox, &before);
  after = before;
  assert(hcp2_lp_mailbox_reload_decision(&mailbox, HCP2_LP_FIRMWARE_VERSION, &before, &after) ==
         HCP2_LP_RELOAD_REQUIRED);

  mailbox.heartbeat = 8u;
  hcp2_lp_mailbox_sample_health(&mailbox, &after);
  assert(hcp2_lp_mailbox_reload_decision(&mailbox, HCP2_LP_FIRMWARE_VERSION, &before, &after) ==
         HCP2_LP_RELOAD_SKIP);
  assert(hcp2_lp_mailbox_reload_decision(&mailbox, HCP2_LP_FIRMWARE_VERSION + 1u, &before, &after) ==
         HCP2_LP_RELOAD_REQUIRED);

  mailbox.state = HCP2_DRIVE_OPENING;
  hcp2_lp_mailbox_sample_health(&mailbox, &after);
  assert(hcp2_lp_mailbox_reload_decision(&mailbox, HCP2_LP_FIRMWARE_VERSION + 1u, &before, &after) ==
         HCP2_LP_RELOAD_DEFER);

  mailbox.state = HCP2_DRIVE_STOPPED;
  before = after;
  mailbox.heartbeat = 9u;
  mailbox.polls_seen = 4u;
  mailbox.polls_answered = 4u;
  hcp2_lp_mailbox_sample_health(&mailbox, &before);
  mailbox.heartbeat = 10u;
  hcp2_lp_mailbox_sample_health(&mailbox, &after);
  assert(hcp2_lp_mailbox_reload_decision(&mailbox, HCP2_LP_FIRMWARE_VERSION, &before, &after) ==
         HCP2_LP_RELOAD_REQUIRED);

  mailbox.magic = 0u;
  assert(hcp2_lp_mailbox_reload_decision(&mailbox, HCP2_LP_FIRMWARE_VERSION, &before, &after) ==
         HCP2_LP_RELOAD_REQUIRED);
}

static void test_mailbox_state_seqlock(void) {
  hcp2_lp_mailbox_t mailbox;
  hcp2_drive_status_t state;
  hcp2_lp_state_snapshot_t snapshot;

  hcp2_lp_mailbox_init(&mailbox);
  memset(&state, 0, sizeof(state));
  state.target_position = 200u;
  state.current_position = 128u;
  state.state = HCP2_DRIVE_OPENING;
  state.light_on = 1u;
  hcp2_lp_mailbox_publish_state(&mailbox, &state, 123456u);
  hcp2_lp_mailbox_publish_counters(&mailbox, 123500u, 7u, 6u, 2u, 1u, 8000u);

  assert(hcp2_lp_mailbox_read_state(&mailbox, &snapshot));
  assert(snapshot.target_position == 200u);
  assert(snapshot.current_position == 128u);
  assert(snapshot.state == HCP2_DRIVE_OPENING);
  assert(snapshot.light_on == 1u);
  assert(snapshot.updated_us == 123456u);
  assert(mailbox.lp_time_us == 123500u);
  assert(mailbox.polls_seen == 7u);
  assert(mailbox.polls_answered == 6u);
  assert(mailbox.tx_abort_count == 2u);
  assert(mailbox.collision_count == 1u);
  assert(mailbox.max_de_hold_us == 8000u);

  mailbox.state_seq |= 1u;
  assert(!hcp2_lp_mailbox_read_state(&mailbox, &snapshot));
}

static void test_mailbox_command_epoch_and_ack(void) {
  hcp2_lp_mailbox_t mailbox;
  hcp2_lp_command_t command;
  uint32_t last_sequence = 0u;

  hcp2_lp_mailbox_init(&mailbox);
  hcp2_lp_mailbox_send_command(&mailbox, 0xA5A5u, 1u, HCP2_LP_COMMAND_OPEN, 0u, 1000u);
  assert(!hcp2_lp_mailbox_take_command(&mailbox, 0x1111u, &last_sequence, 0u, &command));
  assert(last_sequence == 0u);
  assert(hcp2_lp_mailbox_take_command(&mailbox, 0xA5A5u, &last_sequence, 999u, &command));
  assert(command.epoch == 0xA5A5u);
  assert(command.sequence == 1u);
  assert(command.command_id == HCP2_LP_COMMAND_OPEN);
  assert(command.deadline_us == 1000u);
  assert(mailbox.command_ack_sequence == 0u);
  hcp2_lp_mailbox_ack_command(&mailbox, command.sequence, HCP2_LP_COMMAND_RESULT_EXECUTED);
  assert(mailbox.command_ack_sequence == 1u);
  assert(mailbox.command_ack_result == HCP2_LP_COMMAND_RESULT_EXECUTED);
  assert(!hcp2_lp_mailbox_take_command(&mailbox, 0xA5A5u, &last_sequence, 999u, &command));

  hcp2_lp_mailbox_send_command(&mailbox, 0xBEEFu, 2u, HCP2_LP_COMMAND_LIGHT, 1u, 2000u);
  assert(!hcp2_lp_mailbox_take_command(&mailbox, 0xA5A5u, &last_sequence, 1000u, &command));
  assert(hcp2_lp_mailbox_take_command(&mailbox, 0xBEEFu, &last_sequence, 1000u, &command));
  assert(command.command_id == HCP2_LP_COMMAND_LIGHT);
  assert(command.argument == 1u);
  hcp2_lp_mailbox_ack_command(&mailbox, command.sequence, HCP2_LP_COMMAND_RESULT_EXECUTED);
  assert(mailbox.command_ack_sequence == 2u);

  hcp2_lp_mailbox_send_command(&mailbox, 0xBEEFu, 3u, HCP2_LP_COMMAND_OPEN, 0u, 3000u);
  assert(!hcp2_lp_mailbox_take_command(&mailbox, 0xBEEFu, &last_sequence, 3001u, &command));
  assert(mailbox.command_ack_sequence == 3u);
  assert(mailbox.command_ack_result == HCP2_LP_COMMAND_RESULT_EXPIRED);
}

int main(void) {
  test_vector_crc_file();
  test_crc_known_frame();
  test_builders();
  test_bus_scan_response();
  test_status_poll_idle_response();
  test_light_command_response();
  test_broadcast_decode();
  test_button_press_release_sequence();
  test_bad_crc_no_response_and_recovery();
  test_rx_error_resets_partial_frame();
  test_mailbox_layout_and_reload_decision();
  test_mailbox_state_seqlock();
  test_mailbox_command_epoch_and_ack();
  puts("hcp2 core tests ok");
  return 0;
}
