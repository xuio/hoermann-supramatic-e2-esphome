#include "hcp2_frame.h"

#include <string.h>

#include "hcp2_crc.h"

static HCP2_HOT_TEXT uint16_t read_be16_(const uint8_t *data) {
  return (uint16_t) (((uint16_t) data[0] << 8) | data[1]);
}

static HCP2_HOT_TEXT void button_encoding_(hcp2_button_t button, uint8_t release_phase, uint8_t *phase,
                                           uint8_t *mask_lo, uint8_t *mask_hi) {
  *phase = 0u;
  *mask_lo = 0u;
  *mask_hi = 0u;

  switch (button) {
    case HCP2_BUTTON_OPEN:
      *phase = release_phase ? 0x01u : 0x02u;
      *mask_lo = 0x10u;
      break;
    case HCP2_BUTTON_CLOSE:
      *phase = release_phase ? 0x01u : 0x02u;
      *mask_lo = 0x20u;
      break;
    case HCP2_BUTTON_STOP:
      *phase = release_phase ? 0x01u : 0x02u;
      *mask_lo = 0x40u;
      break;
    case HCP2_BUTTON_VENT:
      *phase = release_phase ? 0x01u : 0x02u;
      *mask_hi = 0x40u;
      break;
    case HCP2_BUTTON_HALF:
      *phase = release_phase ? 0x01u : 0x02u;
      *mask_hi = 0x04u;
      break;
    case HCP2_BUTTON_LIGHT:
      *phase = release_phase ? 0x08u : 0x10u;
      *mask_hi = 0x02u;
      break;
    case HCP2_BUTTON_NONE:
    default:
      break;
  }
}

void hcp2_default_signature(uint8_t signature[HCP2_SIGNATURE_LEN]) {
  static const uint8_t default_signature[HCP2_SIGNATURE_LEN] = {
      0x00u, 0x00u, 0x02u, 0x05u, 0x04u, 0x30u, 0x10u, 0xFFu, 0xA8u, 0x45u};
  memcpy(signature, default_signature, HCP2_SIGNATURE_LEN);
}

HCP2_HOT_TEXT uint8_t hcp2_frame_master_expected_len(const uint8_t *data, uint8_t available,
                                                     uint8_t *expected_len) {
  uint8_t byte_count;

  if (data == 0 || expected_len == 0) {
    return 0u;
  }
  *expected_len = 0u;
  if (available < 2u) {
    return 1u;
  }

  switch (data[1]) {
    case HCP2_FC_WRITE_MULTIPLE_REGISTERS:
      if (available < 7u) {
        return 1u;
      }
      byte_count = data[6];
      if (byte_count > 24u) {
        return 0u;
      }
      *expected_len = (uint8_t) (9u + byte_count);
      return *expected_len <= HCP2_MAX_FRAME_LEN;

    case HCP2_FC_READ_WRITE_MULTIPLE_REGISTERS:
      if (available < 11u) {
        return 1u;
      }
      byte_count = data[10];
      if (byte_count > 18u) {
        return 0u;
      }
      *expected_len = (uint8_t) (13u + byte_count);
      return *expected_len <= HCP2_MAX_FRAME_LEN;

    default:
      return 0u;
  }
}

HCP2_HOT_TEXT hcp2_parse_result_t hcp2_frame_parse_master(const uint8_t *data, uint8_t len,
                                                          uint8_t configured_slave_id,
                                                          hcp2_decoded_frame_t *out) {
  uint8_t expected_len = 0u;
  const uint8_t expected_ok = hcp2_frame_master_expected_len(data, len, &expected_len);
  uint16_t read_addr;
  uint16_t read_qty;
  uint16_t write_addr;
  uint16_t write_qty;

  if (out != 0) {
    memset(out, 0, sizeof(*out));
  }
  if (!expected_ok) {
    return HCP2_PARSE_INVALID;
  }
  if (expected_len == 0u || len < expected_len) {
    return HCP2_PARSE_INCOMPLETE;
  }
  if (len != expected_len) {
    return HCP2_PARSE_INVALID;
  }
  if (!hcp2_crc16_check(data, len)) {
    return HCP2_PARSE_BAD_CRC;
  }
  if (out == 0) {
    return HCP2_PARSE_OK;
  }

  out->slave_id = data[0];
  out->type = HCP2_FRAME_OTHER_VALID;

  if (data[1] == HCP2_FC_WRITE_MULTIPLE_REGISTERS) {
    const uint16_t start_addr = read_be16_(&data[2]);
    const uint16_t quantity = read_be16_(&data[4]);
    const uint8_t byte_count = data[6];
    if (data[0] == 0u && start_addr == HCP2_REG_BROADCAST_STATUS && quantity == 9u && byte_count == 18u) {
      const uint8_t *payload = &data[7];
      const uint8_t state = payload[4];
      const uint8_t state_detail = payload[5];
      out->type = HCP2_FRAME_BROADCAST_STATUS;
      out->drive_status.target_position = payload[2];
      out->drive_status.current_position = payload[3];
      out->drive_status.state = (state == 0x00u && state_detail == 0x61u) ? (uint8_t) HCP2_DRIVE_VENT : state;
      out->drive_status.light_raw = payload[13];
      out->drive_status.light_on = (payload[13] == 0x10u || payload[13] == 0x14u) ? 1u : 0u;
    }
    return HCP2_PARSE_OK;
  }

  read_addr = read_be16_(&data[2]);
  read_qty = read_be16_(&data[4]);
  write_addr = read_be16_(&data[6]);
  write_qty = read_be16_(&data[8]);

  if (read_addr != HCP2_REG_STATUS_READ || write_addr != HCP2_REG_COMMAND_WRITE) {
    return HCP2_PARSE_OK;
  }
  if (read_qty == 8u && write_qty == 2u && data[10] == 4u) {
    if (data[0] == configured_slave_id) {
      out->type = HCP2_FRAME_STATUS_POLL;
      out->counter = data[11];
      out->command = data[12];
      out->argument = (uint16_t) (((uint16_t) data[14] << 8) | data[13]);
    }
  } else if (read_qty == 5u && write_qty == 3u && data[10] == 6u && data[11] == 0x00u && data[12] == 0x02u &&
             data[15] == 0x01u && data[16] == configured_slave_id &&
             (data[0] == configured_slave_id || data[0] == 0u)) {
    out->type = HCP2_FRAME_BUS_SCAN;
  } else if (read_qty == 2u && write_qty == 2u && data[10] == 4u) {
    if (data[0] == configured_slave_id) {
      out->type = HCP2_FRAME_COMMAND_ARG;
      out->counter = data[11];
      out->command = data[12];
      out->argument = (uint16_t) (((uint16_t) data[14] << 8) | data[13]);
    }
  }

  return HCP2_PARSE_OK;
}

HCP2_HOT_TEXT uint8_t hcp2_frame_build_scan_response(uint8_t slave_id,
                                                     const uint8_t signature[HCP2_SIGNATURE_LEN], uint8_t *out) {
  out[0] = slave_id;
  out[1] = HCP2_FC_READ_WRITE_MULTIPLE_REGISTERS;
  out[2] = HCP2_SIGNATURE_LEN;
  memcpy(&out[3], signature, HCP2_SIGNATURE_LEN);
  hcp2_crc16_append(out, HCP2_SCAN_RESPONSE_LEN - 2u);
  return HCP2_SCAN_RESPONSE_LEN;
}

HCP2_HOT_TEXT uint8_t hcp2_frame_build_command_response(uint8_t slave_id, uint8_t counter, uint8_t command,
                                                        uint8_t *out) {
  out[0] = slave_id;
  out[1] = HCP2_FC_READ_WRITE_MULTIPLE_REGISTERS;
  out[2] = 0x04u;
  out[3] = counter;
  out[4] = 0x00u;
  out[5] = command;
  out[6] = 0xFDu;
  hcp2_crc16_append(out, HCP2_COMMAND_RESPONSE_LEN - 2u);
  return HCP2_COMMAND_RESPONSE_LEN;
}

HCP2_HOT_TEXT uint8_t hcp2_frame_build_status_response(uint8_t slave_id, uint8_t counter, uint8_t command,
                                                       hcp2_button_t button, uint8_t release_phase, uint8_t *out) {
  uint8_t phase;
  uint8_t mask_lo;
  uint8_t mask_hi;

  out[0] = slave_id;
  out[1] = HCP2_FC_READ_WRITE_MULTIPLE_REGISTERS;
  out[2] = HCP2_STATUS_RESPONSE_DATA_LEN;
  memset(&out[3], 0, HCP2_STATUS_RESPONSE_DATA_LEN);
  out[3] = counter;
  out[4] = 0x00u;
  out[5] = command;
  out[6] = 0x01u;

  button_encoding_(button, release_phase, &phase, &mask_lo, &mask_hi);
  out[7] = phase;
  out[8] = mask_lo;
  out[9] = mask_hi;

  hcp2_crc16_append(out, HCP2_STATUS_RESPONSE_LEN - 2u);
  return HCP2_STATUS_RESPONSE_LEN;
}
