#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HCP2_DEFAULT_SLAVE_ID 2u
#define HCP2_MAX_FRAME_LEN 32u
#define HCP2_SIGNATURE_LEN 10u
#define HCP2_STATUS_RESPONSE_DATA_LEN 16u
#define HCP2_STATUS_RESPONSE_LEN 21u
#define HCP2_SCAN_RESPONSE_LEN 15u
#define HCP2_COMMAND_RESPONSE_LEN 9u

#define HCP2_FC_WRITE_MULTIPLE_REGISTERS 0x10u
#define HCP2_FC_READ_WRITE_MULTIPLE_REGISTERS 0x17u

#define HCP2_REG_STATUS_READ 0x9CB9u
#define HCP2_REG_COMMAND_WRITE 0x9C41u
#define HCP2_REG_BROADCAST_STATUS 0x9D31u

typedef enum {
  HCP2_FRAME_NONE = 0,
  HCP2_FRAME_STATUS_POLL,
  HCP2_FRAME_BUS_SCAN,
  HCP2_FRAME_COMMAND_ARG,
  HCP2_FRAME_BROADCAST_STATUS,
  HCP2_FRAME_OTHER_VALID,
} hcp2_frame_type_t;

typedef enum {
  HCP2_PARSE_OK = 0,
  HCP2_PARSE_INCOMPLETE,
  HCP2_PARSE_INVALID,
  HCP2_PARSE_BAD_CRC,
} hcp2_parse_result_t;

typedef enum {
  HCP2_DRIVE_STOPPED = 0x00,
  HCP2_DRIVE_OPENING = 0x01,
  HCP2_DRIVE_CLOSING = 0x02,
  HCP2_DRIVE_HALF_OPENING = 0x05,
  HCP2_DRIVE_VENT_MOVING = 0x09,
  HCP2_DRIVE_VENT = 0x0A,
  HCP2_DRIVE_OPEN = 0x20,
  HCP2_DRIVE_CLOSED = 0x40,
  HCP2_DRIVE_PART_OPEN = 0x80,
} hcp2_drive_state_code_t;

typedef enum {
  HCP2_BUTTON_NONE = 0,
  HCP2_BUTTON_OPEN,
  HCP2_BUTTON_CLOSE,
  HCP2_BUTTON_STOP,
  HCP2_BUTTON_VENT,
  HCP2_BUTTON_HALF,
  HCP2_BUTTON_LIGHT,
} hcp2_button_t;

typedef struct {
  uint8_t target_position;
  uint8_t current_position;
  uint8_t state;
  uint8_t light_raw;
  uint8_t light_on;
} hcp2_drive_status_t;

typedef struct {
  hcp2_frame_type_t type;
  uint8_t slave_id;
  uint8_t counter;
  uint8_t command;
  uint16_t argument;
  hcp2_drive_status_t drive_status;
} hcp2_decoded_frame_t;

uint8_t hcp2_frame_master_expected_len(const uint8_t *data, uint8_t available, uint8_t *expected_len);
hcp2_parse_result_t hcp2_frame_parse_master(const uint8_t *data, uint8_t len, uint8_t configured_slave_id,
                                            hcp2_decoded_frame_t *out);

uint8_t hcp2_frame_build_scan_response(uint8_t slave_id, const uint8_t signature[HCP2_SIGNATURE_LEN],
                                       uint8_t *out);
uint8_t hcp2_frame_build_command_response(uint8_t slave_id, uint8_t counter, uint8_t command, uint8_t *out);
uint8_t hcp2_frame_build_status_response(uint8_t slave_id, uint8_t counter, uint8_t command, hcp2_button_t button,
                                         uint8_t release_phase, uint8_t *out);
void hcp2_default_signature(uint8_t signature[HCP2_SIGNATURE_LEN]);

#ifdef __cplusplus
}
#endif
