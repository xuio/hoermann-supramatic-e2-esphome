#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hcp2_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HCP2_LP_SRAM_BASE 0x50000000u
#define HCP2_LP_SRAM_SIZE 0x3FC0u
#define HCP2_LP_MAILBOX_OFFSET 0x2000u
#define HCP2_LP_MAILBOX_ADDR (HCP2_LP_SRAM_BASE + HCP2_LP_MAILBOX_OFFSET)

#define HCP2_LP_MAILBOX_MAGIC 0x32435048u
#define HCP2_LP_MAILBOX_ABI_VERSION 1u
#define HCP2_LP_FIRMWARE_VERSION 0x00000001u
#define HCP2_LP_TRACE_CAPACITY 32u

typedef enum {
  HCP2_LP_COMMAND_NONE = 0,
  HCP2_LP_COMMAND_OPEN = 1,
  HCP2_LP_COMMAND_CLOSE = 2,
  HCP2_LP_COMMAND_STOP = 3,
  HCP2_LP_COMMAND_VENT = 4,
  HCP2_LP_COMMAND_HALF = 5,
  HCP2_LP_COMMAND_LIGHT = 6,
} hcp2_lp_command_id_t;

typedef enum {
  HCP2_LP_RELOAD_REQUIRED = 0,
  HCP2_LP_RELOAD_SKIP = 1,
} hcp2_lp_reload_decision_t;

typedef struct {
  uint32_t at_us;
  uint16_t event;
  uint16_t value;
} hcp2_lp_trace_entry_t;

typedef struct {
  uint8_t target_position;
  uint8_t current_position;
  uint8_t state;
  uint8_t light_on;
  uint32_t updated_us;
} hcp2_lp_state_snapshot_t;

typedef struct {
  uint32_t epoch;
  uint32_t sequence;
  uint8_t command_id;
  uint8_t argument;
} hcp2_lp_command_t;

typedef struct {
  uint32_t magic;
  uint16_t abi_version;
  uint16_t struct_size;
  uint32_t firmware_version;
  volatile uint32_t heartbeat;
  volatile uint32_t state_seq;
  volatile uint8_t target_position;
  volatile uint8_t current_position;
  volatile uint8_t state;
  volatile uint8_t light_on;
  volatile uint32_t state_updated_us;
  volatile uint32_t command_epoch;
  volatile uint32_t command_sequence;
  volatile uint32_t command_ack_sequence;
  volatile uint8_t command_id;
  volatile uint8_t command_argument;
  uint8_t reserved0[2];
  volatile uint32_t trace_head;
  volatile uint32_t trace_tail;
  hcp2_lp_trace_entry_t trace[HCP2_LP_TRACE_CAPACITY];
  uint8_t reserved1[204];
} hcp2_lp_mailbox_t;

#define HCP2_LP_MAILBOX_SIZE ((uint16_t) sizeof(hcp2_lp_mailbox_t))

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define HCP2_STATIC_ASSERT(cond, msg) _Static_assert(cond, #msg)
#elif defined(__cplusplus) && __cplusplus >= 201103L
#define HCP2_STATIC_ASSERT(cond, msg) static_assert(cond, #msg)
#else
#define HCP2_STATIC_ASSERT(cond, msg) typedef char hcp2_static_assert_##msg[(cond) ? 1 : -1]
#endif

HCP2_STATIC_ASSERT(sizeof(hcp2_lp_mailbox_t) == 512u, hcp2_lp_mailbox_size_must_be_512);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, magic) == 0u, hcp2_lp_mailbox_magic_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, heartbeat) == 12u, hcp2_lp_mailbox_heartbeat_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, state_seq) == 16u, hcp2_lp_mailbox_state_seq_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, command_epoch) == 28u, hcp2_lp_mailbox_command_epoch_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, trace) == 52u, hcp2_lp_mailbox_trace_offset);

void hcp2_lp_mailbox_init(volatile hcp2_lp_mailbox_t *mailbox);
void hcp2_lp_mailbox_publish_state(volatile hcp2_lp_mailbox_t *mailbox, const hcp2_drive_status_t *state,
                                   uint32_t now_us);
uint8_t hcp2_lp_mailbox_read_state(const volatile hcp2_lp_mailbox_t *mailbox, hcp2_lp_state_snapshot_t *out);
void hcp2_lp_mailbox_send_command(volatile hcp2_lp_mailbox_t *mailbox, uint32_t epoch, uint32_t sequence,
                                  hcp2_lp_command_id_t command_id, uint8_t argument);
uint8_t hcp2_lp_mailbox_take_command(volatile hcp2_lp_mailbox_t *mailbox, uint32_t expected_epoch,
                                     uint32_t *last_sequence, hcp2_lp_command_t *out);
hcp2_lp_reload_decision_t hcp2_lp_mailbox_reload_decision(const volatile hcp2_lp_mailbox_t *mailbox,
                                                          uint32_t expected_firmware_version,
                                                          uint32_t heartbeat_before,
                                                          uint32_t heartbeat_after);

#ifdef __cplusplus
}
#endif
