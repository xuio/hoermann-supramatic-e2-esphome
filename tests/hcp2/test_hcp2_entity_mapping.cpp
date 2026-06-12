#include <cassert>
#include <cstring>

#include "hcp2_entity_mapping.h"

using esphome::hcp2bridge::HCP2CoverOperation;
using esphome::hcp2bridge::hcp2_cover_button_for_control;
using esphome::hcp2bridge::hcp2_cover_operation;
using esphome::hcp2bridge::hcp2_state_is_closed;
using esphome::hcp2bridge::hcp2_state_is_light_on;
using esphome::hcp2bridge::hcp2_state_is_moving;
using esphome::hcp2bridge::hcp2_state_is_open;
using esphome::hcp2bridge::hcp2_state_name;
using esphome::hcp2bridge::hcp2_state_position;

static hcp2_drive_status_t status_with(hcp2_drive_state_code_t state, uint8_t position, uint8_t light) {
  hcp2_drive_status_t status{};
  status.state = state;
  status.current_position = position;
  status.light_on = light;
  return status;
}

static void test_binary_sensor_and_position_mapping() {
  hcp2_drive_status_t status = status_with(HCP2_DRIVE_CLOSED, 0, 0);
  assert(hcp2_state_is_closed(status));
  assert(!hcp2_state_is_open(status));
  assert(!hcp2_state_is_moving(status));
  assert(!hcp2_state_is_light_on(status));
  assert(hcp2_state_position(status) == 0.0f);

  status = status_with(HCP2_DRIVE_OPEN, 200, 1);
  assert(!hcp2_state_is_closed(status));
  assert(hcp2_state_is_open(status));
  assert(!hcp2_state_is_moving(status));
  assert(hcp2_state_is_light_on(status));
  assert(hcp2_state_position(status) == 1.0f);

  status = status_with(HCP2_DRIVE_OPENING, 100, 0);
  assert(hcp2_state_is_moving(status));
  assert(hcp2_state_position(status) == 0.5f);

  status = status_with(HCP2_DRIVE_OPEN, 250, 0);
  assert(hcp2_state_position(status) == 1.0f);
}

static void test_cover_operation_mapping() {
  assert(hcp2_cover_operation(HCP2_DRIVE_OPENING) == HCP2CoverOperation::OPENING);
  assert(hcp2_cover_operation(HCP2_DRIVE_HALF_OPENING) == HCP2CoverOperation::OPENING);
  assert(hcp2_cover_operation(HCP2_DRIVE_CLOSING) == HCP2CoverOperation::CLOSING);
  assert(hcp2_cover_operation(HCP2_DRIVE_OPEN) == HCP2CoverOperation::IDLE);
  assert(hcp2_cover_operation(HCP2_DRIVE_CLOSED) == HCP2CoverOperation::IDLE);
  assert(hcp2_cover_operation(HCP2_DRIVE_STOPPED) == HCP2CoverOperation::IDLE);
}

static void test_cover_control_mapping() {
  assert(hcp2_cover_button_for_control(true, false, 0.0f) == HCP2_BUTTON_STOP);
  assert(hcp2_cover_button_for_control(false, false, 0.0f) == HCP2_BUTTON_NONE);
  assert(hcp2_cover_button_for_control(false, true, 0.0f) == HCP2_BUTTON_CLOSE);
  assert(hcp2_cover_button_for_control(false, true, 0.01f) == HCP2_BUTTON_CLOSE);
  assert(hcp2_cover_button_for_control(false, true, 0.5f) == HCP2_BUTTON_HALF);
  assert(hcp2_cover_button_for_control(false, true, 0.99f) == HCP2_BUTTON_OPEN);
  assert(hcp2_cover_button_for_control(false, true, 1.0f) == HCP2_BUTTON_OPEN);
}

static void test_state_names() {
  assert(std::strcmp(hcp2_state_name(HCP2_DRIVE_STOPPED), "stopped") == 0);
  assert(std::strcmp(hcp2_state_name(HCP2_DRIVE_OPENING), "opening") == 0);
  assert(std::strcmp(hcp2_state_name(HCP2_DRIVE_CLOSING), "closing") == 0);
  assert(std::strcmp(hcp2_state_name(HCP2_DRIVE_HALF_OPENING), "half_opening") == 0);
  assert(std::strcmp(hcp2_state_name(HCP2_DRIVE_OPEN), "open") == 0);
  assert(std::strcmp(hcp2_state_name(HCP2_DRIVE_CLOSED), "closed") == 0);
  assert(std::strcmp(hcp2_state_name(HCP2_DRIVE_PART_OPEN), "part_open") == 0);
  assert(std::strcmp(hcp2_state_name((hcp2_drive_state_code_t) 0xAA), "unknown") == 0);
}

int main() {
  test_binary_sensor_and_position_mapping();
  test_cover_operation_mapping();
  test_cover_control_mapping();
  test_state_names();
  return 0;
}
