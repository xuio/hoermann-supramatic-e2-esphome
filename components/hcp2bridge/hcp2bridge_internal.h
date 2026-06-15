#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

extern "C" {
#include "hcp2_mailbox.h"
}

namespace esphome {
namespace hcp2bridge {

static constexpr uint32_t HCP2BRIDGE_BAUD_RATE = 57600;
static constexpr size_t HCP2BRIDGE_RX_BUFFER_SIZE = 256;
static constexpr size_t HCP2BRIDGE_TX_BUFFER_SIZE = 256;
static constexpr size_t HCP2BRIDGE_UART_EVENT_QUEUE_LEN = 32;
static constexpr size_t HCP2BRIDGE_COMMAND_QUEUE_LEN = 8;
static constexpr uint32_t HCP2BRIDGE_BUS_TASK_STACK_BYTES = 6144;
static constexpr uint32_t HCP2BRIDGE_LP_TASK_STACK_BYTES = 4096;
static constexpr uint32_t HCP2BRIDGE_HTTP_TASK_STACK_BYTES = 8192;
static constexpr uint32_t HCP2BRIDGE_LP_HEARTBEAT_PROBE_MS = 20;
static constexpr uint32_t HCP2BRIDGE_LP_MAILBOX_TIMEOUT_MS = 250;
static constexpr uint32_t HCP2BRIDGE_LP_HEALTH_LOG_INTERVAL_MS = 5000;
static constexpr uint32_t HCP2BRIDGE_BUS_ONLINE_TIMEOUT_US = 1000000;
static constexpr uint32_t HCP2BRIDGE_OBSTRUCTION_COMMAND_GRACE_MS = 2000;
static constexpr uint32_t HCP2BRIDGE_OBSTRUCTION_LATCH_MS = 10000;
static constexpr uint32_t HCP2BRIDGE_HTTP_SETUP_DELAY_MS = 5000;
static constexpr uint32_t HCP2BRIDGE_HTTP_SETUP_RETRY_MS = 5000;
static constexpr uint32_t HCP2BRIDGE_HTTP_PENDING_TIMEOUT_MS = 3000;
static constexpr uint32_t HCP2BRIDGE_HTTP_RESPONSE_TIMEOUT_MS = 500;
static constexpr uint32_t HCP2BRIDGE_HTTP_LARGE_RESPONSE_TIMEOUT_MS = 1500;
static constexpr uint32_t HCP2BRIDGE_HTTP_WS_HANDSHAKE_TIMEOUT_MS = 500;
static constexpr uint32_t HCP2BRIDGE_HTTP_WS_WRITE_TIMEOUT_MS = 150;
static constexpr uint32_t HCP2BRIDGE_HTTP_WS_SEND_INTERVAL_MS = 250;
static constexpr uint32_t HCP2BRIDGE_HTTP_WS_HEALTH_INTERVAL_MS = 500;
static constexpr uint32_t HCP2BRIDGE_HTTP_TASK_IDLE_MS = 10;
static constexpr size_t HCP2BRIDGE_HTTP_REQUEST_READ_BUDGET_BYTES = 256;
static constexpr size_t HCP2BRIDGE_HTTP_WS_DRAIN_BUDGET_BYTES = 128;
static constexpr size_t HCP2BRIDGE_HTTP_LARGE_RESPONSE_BYTES = 4096;
static constexpr size_t HCP2BRIDGE_HTTP_WS_MAX_CHUNK_BYTES = 2048;
static constexpr uint32_t HCP2BRIDGE_LP_TRACE_DRAIN_MAX_PER_TICK = HCP2_LP_TRACE_CAPACITY;
static constexpr uint32_t HCP2BRIDGE_MAX_DE_HIGH_US = 9000;
static constexpr uint32_t HCP2BRIDGE_PENDING_REPLY_GRACE_MS = 20;
static constexpr const char *HCP2BRIDGE_WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

inline bool hcp2_ascii_iequals(const std::string &left, const char *right) {
  if (right == nullptr) {
    return left.empty();
  }
  const size_t right_len = std::strlen(right);
  if (left.size() != right_len) {
    return false;
  }
  for (size_t i = 0; i < left.size(); i++) {
    if (std::tolower((unsigned char) left[i]) != std::tolower((unsigned char) right[i])) {
      return false;
    }
  }
  return true;
}

inline uint32_t hcp2_raw_missed_polls(uint32_t polls_seen, uint32_t polls_answered) {
  return polls_seen >= polls_answered ? polls_seen - polls_answered : 0u;
}

inline bool hcp2_pending_reply(uint32_t polls_seen, uint32_t polls_answered, uint32_t last_poll_age_ms) {
  return hcp2_raw_missed_polls(polls_seen, polls_answered) == 1u &&
         last_poll_age_ms <= HCP2BRIDGE_PENDING_REPLY_GRACE_MS;
}

inline uint32_t hcp2_effective_missed_polls(uint32_t polls_seen, uint32_t polls_answered,
                                            uint32_t last_poll_age_ms, bool *pending_response) {
  const bool pending = hcp2_pending_reply(polls_seen, polls_answered, last_poll_age_ms);
  if (pending_response != nullptr) {
    *pending_response = pending;
  }
  return pending ? 0u : hcp2_raw_missed_polls(polls_seen, polls_answered);
}

}  // namespace hcp2bridge
}  // namespace esphome
