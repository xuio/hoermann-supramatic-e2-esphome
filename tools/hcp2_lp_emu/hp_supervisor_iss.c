#include <stddef.h>
#include <stdint.h>

#include "hcp2_supervisor.h"

#define HCP2_HP_ISS_CONTROL_ADDR 0x40800000u

typedef enum {
  HCP2_HP_ISS_REQ_NONE = 0,
  HCP2_HP_ISS_REQ_BEGIN_SESSION = 1,
  HCP2_HP_ISS_REQ_PROBE_RELOAD = 2,
  HCP2_HP_ISS_REQ_SEND_COMMAND = 3,
  HCP2_HP_ISS_REQ_ACK_RECEIVED = 4,
  HCP2_HP_ISS_REQ_READ_STATE = 5,
} hcp2_hp_iss_request_t;

typedef struct {
  volatile uint32_t request;
  volatile uint32_t response;
  volatile uint32_t arg0;
  volatile uint32_t arg1;
  volatile uint32_t arg2;
  volatile uint32_t result0;
  volatile uint32_t result1;
  volatile uint32_t result2;
  volatile uint32_t result3;
} hcp2_hp_iss_control_t;

void *memset(void *dest, int value, size_t len) {
  unsigned char *out = (unsigned char *) dest;
  while (len-- > 0u) {
    *out++ = (unsigned char) value;
  }
  return dest;
}

void *memcpy(void *dest, const void *src, size_t len) {
  unsigned char *out = (unsigned char *) dest;
  const unsigned char *in = (const unsigned char *) src;
  while (len-- > 0u) {
    *out++ = *in++;
  }
  return dest;
}

static void barrier_(void) {
#if defined(__GNUC__) || defined(__clang__)
  __sync_synchronize();
#endif
}

__attribute__((section(".text.start"))) void _start(void) {
  volatile hcp2_hp_iss_control_t *control = (volatile hcp2_hp_iss_control_t *) HCP2_HP_ISS_CONTROL_ADDR;
  volatile hcp2_lp_mailbox_t *mailbox = (volatile hcp2_lp_mailbox_t *) HCP2_LP_MAILBOX_ADDR;
  hcp2_hp_supervisor_t supervisor;

  hcp2_hp_supervisor_init(&supervisor, mailbox, HCP2_LP_FIRMWARE_VERSION);
  hcp2_hp_supervisor_begin_session(&supervisor, 1u);
  control->response = 0u;
  control->request = HCP2_HP_ISS_REQ_NONE;

  for (;;) {
    const uint32_t request = control->request;
    if (request == HCP2_HP_ISS_REQ_NONE) {
      continue;
    }

    control->result0 = 0u;
    control->result1 = 0u;
    control->result2 = 0u;
    control->result3 = 0u;
    barrier_();

    switch ((hcp2_hp_iss_request_t) request) {
      case HCP2_HP_ISS_REQ_BEGIN_SESSION:
        hcp2_hp_supervisor_begin_session(&supervisor, control->arg0);
        control->result0 = supervisor.epoch;
        break;

      case HCP2_HP_ISS_REQ_PROBE_RELOAD: {
        hcp2_lp_health_sample_t before;
        hcp2_lp_health_sample_t after;
        hcp2_hp_supervisor_sample_health(&supervisor, &after);
        before = after;
        before.heartbeat = control->arg0;
        after.heartbeat = control->arg1;
        if (control->arg2 != 0u) {
          before.polls_seen = control->arg2;
          before.polls_answered = control->arg2;
        }
        const hcp2_lp_reload_decision_t decision =
            hcp2_hp_supervisor_reload_decision(&supervisor, &before, &after);
        control->result0 = (uint32_t) decision;
        control->result1 = decision == HCP2_LP_RELOAD_SKIP ? 1u : 0u;
        break;
      }

      case HCP2_HP_ISS_REQ_SEND_COMMAND:
        control->result0 = hcp2_hp_supervisor_send_command(
            &supervisor, (hcp2_lp_command_id_t) control->arg0, (uint8_t) control->arg1);
        break;

      case HCP2_HP_ISS_REQ_ACK_RECEIVED:
        control->result0 = hcp2_hp_supervisor_ack_received(&supervisor, control->arg0);
        control->result1 = (uint32_t) hcp2_hp_supervisor_ack_result(&supervisor, control->arg0);
        break;

      case HCP2_HP_ISS_REQ_READ_STATE: {
        hcp2_lp_state_snapshot_t snapshot;
        memset(&snapshot, 0, sizeof(snapshot));
        const uint8_t ok = hcp2_hp_supervisor_read_state(&supervisor, &snapshot);
        control->result0 = ok;
        control->result1 = (uint32_t) snapshot.target_position | ((uint32_t) snapshot.current_position << 8) |
                           ((uint32_t) snapshot.state << 16) | ((uint32_t) snapshot.light_on << 24);
        control->result2 = snapshot.updated_us;
        break;
      }

      case HCP2_HP_ISS_REQ_NONE:
      default:
        control->result0 = 0xFFFFFFFFu;
        break;
    }

    control->request = HCP2_HP_ISS_REQ_NONE;
    barrier_();
    control->response = request;
  }
}
