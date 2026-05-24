#pragma once

#include <deque>
#include <memory>
#include <string>

#include "esphome/components/socket/socket.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

namespace esphome {
namespace rs485_http_monitor {

#define HTTP_MONITOR_KEEPALIVE_MS 5000
#define HTTP_MONITOR_PENDING_TIMEOUT_MS 3000

class RS485HTTPMonitor : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void set_port(uint16_t port) { this->port_ = port; }
  void set_send_gaps(bool send_gaps) { this->send_gaps_ = send_gaps; }
  void set_gap_threshold_us(uint32_t gap_threshold_us) { this->gap_threshold_us_ = gap_threshold_us; }
  void set_history_size(uint16_t history_size) { this->history_size_ = history_size; }

 protected:
  void setup_server_();
  void accept_client_();
  void service_pending_client_();
  void service_uart_();
  void service_keepalive_();
  void disconnect_stream_client_();
  void handle_http_request_(std::unique_ptr<socket::Socket> client, const std::string &request_line);
  void start_stream_(std::unique_ptr<socket::Socket> client, bool sse);
  void send_http_response_(std::unique_ptr<socket::Socket> client, const char *status, const char *content_type,
                           const std::string &body);
  bool write_all_(socket::Socket *client, const std::string &payload, uint32_t timeout_ms);
  void send_stream_line_(const std::string &event_type, const std::string &json_data, const std::string &plain_line);
  void emit_rx_(uint8_t *data, size_t len, uint32_t timestamp_us);
  void emit_gap_(uint32_t timestamp_us, uint32_t gap_us);
  void append_recent_(const std::string &line);
  std::string recent_body_();
  std::string stats_json_();
  std::string hex_encode_(const uint8_t *data, size_t len);
  std::string path_from_request_line_(const std::string &request_line);

  uint16_t port_{8080};
  bool send_gaps_{true};
  uint32_t gap_threshold_us_{3000};
  uint16_t history_size_{120};

  std::unique_ptr<socket::ListenSocket> server_;
  std::unique_ptr<socket::Socket> pending_client_;
  std::unique_ptr<socket::Socket> stream_client_;
  bool stream_is_sse_{true};
  char request_buffer_[256]{};
  size_t request_buffer_len_{0};
  uint32_t pending_client_started_ms_{0};

  uint32_t last_rx_us_{0};
  uint32_t rx_sequence_{0};
  uint32_t gap_sequence_{0};
  uint32_t rx_bytes_{0};
  uint32_t last_keepalive_ms_{0};
  std::deque<std::string> recent_lines_;
};

}  // namespace rs485_http_monitor
}  // namespace esphome
