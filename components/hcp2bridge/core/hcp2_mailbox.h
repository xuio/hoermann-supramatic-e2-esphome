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
#define HCP2_LP_MAILBOX_ABI_VERSION 2u
#define HCP2_LP_FIRMWARE_VERSION 0x00000002u
#define HCP2_LP_TRACE_CAPACITY 32u

#define HCP2_LP_TRACE_BOOT 1u
#define HCP2_LP_TRACE_TX 2u
#define HCP2_LP_TRACE_COMMAND 3u
#define HCP2_LP_TRACE_RX 4u
#define HCP2_LP_TRACE_RX_ERROR 5u
#define HCP2_LP_TRACE_DE 6u
#define HCP2_LP_TRACE_GPIO_RX 7u
#define HCP2_LP_TRACE_RX_ECHO 8u
#define HCP2_LP_TRACE_TX_ABORT 9u
#define HCP2_LP_TRACE_COLLISION 10u

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
  HCP2_LP_RELOAD_DEFER = 2,
} hcp2_lp_reload_decision_t;

typedef enum {
  HCP2_LP_COMMAND_RESULT_NONE = 0,
  HCP2_LP_COMMAND_RESULT_EXECUTED = 1,
  HCP2_LP_COMMAND_RESULT_EXPIRED = 2,
  HCP2_LP_COMMAND_RESULT_UNKNOWN = 3,
  HCP2_LP_COMMAND_RESULT_BUSY = 4,
} hcp2_lp_command_result_t;

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
  uint32_t deadline_us;
  uint8_t command_id;
  uint8_t argument;
} hcp2_lp_command_t;

typedef struct {
  uint32_t heartbeat;
  uint32_t polls_seen;
  uint32_t polls_answered;
  uint32_t command_sequence;
  uint32_t command_ack_sequence;
  uint8_t drive_state;
} hcp2_lp_health_sample_t;

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
  volatile uint8_t command_ack_result;
  uint8_t reserved0;
  volatile uint32_t command_deadline_us;
  volatile uint32_t lp_time_us;
  volatile uint32_t polls_seen;
  volatile uint32_t polls_answered;
  volatile uint32_t tx_abort_count;
  volatile uint32_t collision_count;
  volatile uint32_t max_de_hold_us;
  volatile uint32_t lp_reset_count;
  volatile uint32_t trace_head;
  volatile uint32_t trace_tail;
  hcp2_lp_trace_entry_t trace[HCP2_LP_TRACE_CAPACITY];
  uint8_t reserved1[172];
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
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, command_ack_result) == 42u,
                   hcp2_lp_mailbox_command_ack_result_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, command_deadline_us) == 44u,
                   hcp2_lp_mailbox_command_deadline_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, lp_time_us) == 48u, hcp2_lp_mailbox_lp_time_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, polls_seen) == 52u, hcp2_lp_mailbox_polls_seen_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, polls_answered) == 56u, hcp2_lp_mailbox_polls_answered_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, tx_abort_count) == 60u, hcp2_lp_mailbox_tx_abort_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, collision_count) == 64u, hcp2_lp_mailbox_collision_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, max_de_hold_us) == 68u, hcp2_lp_mailbox_max_de_hold_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, lp_reset_count) == 72u, hcp2_lp_mailbox_lp_reset_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, trace) == 84u, hcp2_lp_mailbox_trace_offset);

void hcp2_lp_mailbox_init(volatile hcp2_lp_mailbox_t *mailbox);
void hcp2_lp_mailbox_publish_state(volatile hcp2_lp_mailbox_t *mailbox, const hcp2_drive_status_t *state,
                                   uint32_t now_us);
void hcp2_lp_mailbox_publish_counters(volatile hcp2_lp_mailbox_t *mailbox, uint32_t now_us, uint32_t polls_seen,
                                      uint32_t polls_answered, uint32_t tx_abort_count,
                                      uint32_t collision_count, uint32_t max_de_hold_us);
uint8_t hcp2_lp_mailbox_read_state(const volatile hcp2_lp_mailbox_t *mailbox, hcp2_lp_state_snapshot_t *out);
void hcp2_lp_mailbox_send_command(volatile hcp2_lp_mailbox_t *mailbox, uint32_t epoch, uint32_t sequence,
                                  hcp2_lp_command_id_t command_id, uint8_t argument, uint32_t deadline_us);
uint8_t hcp2_lp_mailbox_take_command(volatile hcp2_lp_mailbox_t *mailbox, uint32_t expected_epoch,
                                     uint32_t *last_sequence, uint32_t now_us, hcp2_lp_command_t *out);
void hcp2_lp_mailbox_ack_command(volatile hcp2_lp_mailbox_t *mailbox, uint32_t sequence,
                                 hcp2_lp_command_result_t result);
void hcp2_lp_mailbox_sample_health(const volatile hcp2_lp_mailbox_t *mailbox, hcp2_lp_health_sample_t *out);
hcp2_lp_reload_decision_t hcp2_lp_mailbox_reload_decision(const volatile hcp2_lp_mailbox_t *mailbox,
                                                          uint32_t expected_firmware_version,
                                                          const hcp2_lp_health_sample_t *before,
                                                          const hcp2_lp_health_sample_t *after);

#ifdef __cplusplus
}
#endif
