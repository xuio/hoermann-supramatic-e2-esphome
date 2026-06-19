#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hcp2_engine.h"
#include "hcp2_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HCP2_LP_SRAM_BASE 0x50000000u
#define HCP2_LP_SRAM_SIZE 0x3FC0u
#define HCP2_LP_MAILBOX_OFFSET 0x2400u
#define HCP2_LP_MAILBOX_ADDR (HCP2_LP_SRAM_BASE + HCP2_LP_MAILBOX_OFFSET)

#define HCP2_LP_MAILBOX_MAGIC 0x32435048u
#define HCP2_LP_MAILBOX_ABI_VERSION 8u
#define HCP2_LP_FIRMWARE_VERSION 0x00000010u
#define HCP2_LP_PROTOCOL_EVENT_CAPACITY 16u
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
#define HCP2_LP_TRACE_WDT 11u
#define HCP2_LP_TRACE_STOP_TRIGGER 12u
#define HCP2_LP_TRACE_HEALTH 13u

#define HCP2_LP_HEALTH_FLAG_LOOP_OVERRUN 0x0001u
#define HCP2_LP_HEALTH_FLAG_RX_STARVATION 0x0002u
#define HCP2_LP_HEALTH_FLAG_STUCK_DE 0x0004u
#define HCP2_LP_HEALTH_FLAG_MAILBOX_REPAIR 0x0008u

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
  uint32_t sequence;
  uint32_t at_us;
  uint8_t event_type;
  uint8_t frame_type;
  uint8_t len;
  uint8_t reserved;
  uint8_t data[HCP2_MAX_FRAME_LEN];
} hcp2_lp_protocol_event_t;

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
  uint32_t epoch;
  uint32_t deadline_us;
  uint8_t target_position;
} hcp2_lp_stop_trigger_t;

typedef struct {
  uint32_t heartbeat;
  uint32_t polls_seen;
  uint32_t polls_answered;
  uint32_t last_poll_us;
  uint32_t command_sequence;
  uint32_t command_ack_sequence;
  uint8_t drive_state;
} hcp2_lp_health_sample_t;

typedef struct {
  uint32_t now_us;
  uint32_t polls_seen;
  uint32_t polls_answered;
  uint32_t tx_abort_count;
  uint32_t collision_count;
  uint32_t max_de_hold_us;
  uint32_t last_poll_us;
  uint32_t crc_error_count;
  uint32_t rx_error_count;
  uint32_t max_loop_us;
  uint32_t loop_overrun_count;
  uint32_t rx_starvation_count;
  uint32_t stuck_de_count;
  uint32_t mailbox_repair_count;
  uint16_t health_flags;
  uint16_t max_rx_fifo_count;
  uint32_t max_poll_rx_to_schedule_us;
  uint32_t max_response_schedule_to_tx_start_us;
  uint32_t max_response_tx_us;
} hcp2_lp_counters_t;

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
  volatile uint32_t last_poll_us;
  volatile uint32_t crc_error_count;
  volatile uint32_t rx_error_count;
  volatile uint32_t stop_trigger_epoch;
  volatile uint32_t stop_trigger_deadline_us;
  volatile uint8_t stop_trigger_target_position;
  volatile uint8_t stop_trigger_armed;
  volatile uint16_t stop_trigger_fire_count;
  volatile uint16_t health_flags;
  volatile uint16_t max_rx_fifo_count;
  volatile uint32_t max_loop_us;
  volatile uint32_t loop_overrun_count;
  volatile uint32_t rx_starvation_count;
  volatile uint32_t stuck_de_count;
  volatile uint32_t mailbox_repair_count;
  volatile uint32_t max_poll_rx_to_schedule_us;
  volatile uint32_t max_response_schedule_to_tx_start_us;
  volatile uint32_t max_response_tx_us;
  volatile uint32_t protocol_sequence;
  volatile uint32_t protocol_at_us;
  volatile uint8_t protocol_event_type;
  volatile uint8_t protocol_frame_type;
  volatile uint8_t protocol_len;
  volatile uint8_t protocol_reserved;
  volatile uint8_t protocol_data[HCP2_MAX_FRAME_LEN];
  volatile uint32_t protocol_head;
  volatile uint32_t protocol_tail;
  hcp2_lp_protocol_event_t protocol_events[HCP2_LP_PROTOCOL_EVENT_CAPACITY];
  volatile uint32_t trace_head;
  volatile uint32_t trace_tail;
  hcp2_lp_trace_entry_t trace[HCP2_LP_TRACE_CAPACITY];
  volatile uint32_t config_sequence;
  volatile uint8_t config_slave_id;
  volatile uint8_t config_signature[HCP2_SIGNATURE_LEN];
  volatile uint8_t config_reserved0;
  volatile uint32_t config_response_delay_us;
  volatile uint32_t config_button_press_us;
  uint8_t reserved1[100];
} hcp2_lp_mailbox_t;

#define HCP2_LP_MAILBOX_SIZE ((uint16_t) sizeof(hcp2_lp_mailbox_t))

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define HCP2_STATIC_ASSERT(cond, msg) _Static_assert(cond, #msg)
#elif defined(__cplusplus) && __cplusplus >= 201103L
#define HCP2_STATIC_ASSERT(cond, msg) static_assert(cond, #msg)
#else
#define HCP2_STATIC_ASSERT(cond, msg) typedef char hcp2_static_assert_##msg[(cond) ? 1 : -1]
#endif

HCP2_STATIC_ASSERT(sizeof(hcp2_lp_mailbox_t) == 1280u, hcp2_lp_mailbox_size_must_be_1280);
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
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, last_poll_us) == 76u, hcp2_lp_mailbox_last_poll_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, crc_error_count) == 80u, hcp2_lp_mailbox_crc_error_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, rx_error_count) == 84u, hcp2_lp_mailbox_rx_error_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, stop_trigger_epoch) == 88u,
                   hcp2_lp_mailbox_stop_trigger_epoch_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, stop_trigger_deadline_us) == 92u,
                   hcp2_lp_mailbox_stop_trigger_deadline_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, stop_trigger_target_position) == 96u,
                   hcp2_lp_mailbox_stop_trigger_target_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, stop_trigger_armed) == 97u,
                   hcp2_lp_mailbox_stop_trigger_armed_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, stop_trigger_fire_count) == 98u,
                   hcp2_lp_mailbox_stop_trigger_fire_count_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, health_flags) == 100u, hcp2_lp_mailbox_health_flags_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, max_rx_fifo_count) == 102u,
                   hcp2_lp_mailbox_max_rx_fifo_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, max_loop_us) == 104u, hcp2_lp_mailbox_max_loop_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, loop_overrun_count) == 108u,
                   hcp2_lp_mailbox_loop_overrun_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, rx_starvation_count) == 112u,
                   hcp2_lp_mailbox_rx_starvation_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, stuck_de_count) == 116u,
                   hcp2_lp_mailbox_stuck_de_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, mailbox_repair_count) == 120u,
                   hcp2_lp_mailbox_mailbox_repair_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, max_poll_rx_to_schedule_us) == 124u,
                   hcp2_lp_mailbox_max_poll_rx_to_schedule_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, max_response_schedule_to_tx_start_us) == 128u,
                   hcp2_lp_mailbox_max_response_schedule_to_tx_start_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, max_response_tx_us) == 132u,
                   hcp2_lp_mailbox_max_response_tx_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, protocol_sequence) == 136u,
                   hcp2_lp_mailbox_protocol_sequence_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, protocol_at_us) == 140u,
                   hcp2_lp_mailbox_protocol_at_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, protocol_event_type) == 144u,
                   hcp2_lp_mailbox_protocol_event_type_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, protocol_data) == 148u,
                   hcp2_lp_mailbox_protocol_data_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, protocol_head) == 180u,
                   hcp2_lp_mailbox_protocol_head_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, protocol_tail) == 184u,
                   hcp2_lp_mailbox_protocol_tail_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, protocol_events) == 188u,
                   hcp2_lp_mailbox_protocol_events_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, trace_head) == 892u,
                   hcp2_lp_mailbox_trace_head_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, trace) == 900u, hcp2_lp_mailbox_trace_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, config_sequence) == 1156u,
                   hcp2_lp_mailbox_config_sequence_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, config_slave_id) == 1160u,
                   hcp2_lp_mailbox_config_slave_id_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, config_signature) == 1161u,
                   hcp2_lp_mailbox_config_signature_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, config_response_delay_us) == 1172u,
                   hcp2_lp_mailbox_config_response_delay_offset);
HCP2_STATIC_ASSERT(offsetof(hcp2_lp_mailbox_t, config_button_press_us) == 1176u,
                   hcp2_lp_mailbox_config_button_press_offset);

void hcp2_lp_mailbox_init(volatile hcp2_lp_mailbox_t *mailbox);
void hcp2_lp_mailbox_repair_header(volatile hcp2_lp_mailbox_t *mailbox);
void hcp2_lp_mailbox_write_config(volatile hcp2_lp_mailbox_t *mailbox,
                                  const hcp2_engine_config_t *config);
uint8_t hcp2_lp_mailbox_read_config(const volatile hcp2_lp_mailbox_t *mailbox,
                                    hcp2_engine_config_t *out);
void hcp2_lp_mailbox_publish_state(volatile hcp2_lp_mailbox_t *mailbox, const hcp2_drive_status_t *state,
                                   uint32_t now_us);
void hcp2_lp_mailbox_publish_counters(volatile hcp2_lp_mailbox_t *mailbox,
                                      const hcp2_lp_counters_t *counters);
void hcp2_lp_mailbox_publish_protocol_event(volatile hcp2_lp_mailbox_t *mailbox,
                                            const hcp2_protocol_event_t *event);
uint8_t hcp2_lp_mailbox_read_protocol_event(const volatile hcp2_lp_mailbox_t *mailbox,
                                            uint32_t *last_sequence, hcp2_lp_protocol_event_t *out);
uint8_t hcp2_lp_mailbox_read_state(const volatile hcp2_lp_mailbox_t *mailbox, hcp2_lp_state_snapshot_t *out);
void hcp2_lp_mailbox_send_command(volatile hcp2_lp_mailbox_t *mailbox, uint32_t epoch, uint32_t sequence,
                                  hcp2_lp_command_id_t command_id, uint8_t argument, uint32_t deadline_us);
uint8_t hcp2_lp_mailbox_take_command(volatile hcp2_lp_mailbox_t *mailbox, uint32_t expected_epoch,
                                     uint32_t *last_sequence, uint32_t now_us, hcp2_lp_command_t *out);
void hcp2_lp_mailbox_ack_command(volatile hcp2_lp_mailbox_t *mailbox, uint32_t sequence,
                                 hcp2_lp_command_result_t result);
void hcp2_lp_mailbox_arm_stop_trigger(volatile hcp2_lp_mailbox_t *mailbox, uint32_t epoch,
                                      uint8_t target_position, uint32_t deadline_us);
void hcp2_lp_mailbox_disarm_stop_trigger(volatile hcp2_lp_mailbox_t *mailbox);
uint8_t hcp2_lp_mailbox_read_stop_trigger(const volatile hcp2_lp_mailbox_t *mailbox,
                                          hcp2_lp_stop_trigger_t *out);
void hcp2_lp_mailbox_mark_stop_trigger_fired(volatile hcp2_lp_mailbox_t *mailbox);
void hcp2_lp_mailbox_sample_health(const volatile hcp2_lp_mailbox_t *mailbox, hcp2_lp_health_sample_t *out);
hcp2_lp_reload_decision_t hcp2_lp_mailbox_reload_decision(const volatile hcp2_lp_mailbox_t *mailbox,
                                                          uint32_t expected_firmware_version,
                                                          const hcp2_lp_health_sample_t *before,
                                                          const hcp2_lp_health_sample_t *after);

#ifdef __cplusplus
}
#endif
