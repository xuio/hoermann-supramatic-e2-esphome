#pragma once

#include <memory>
#include <string>
#include <vector>

#include "esphome/components/socket/socket.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace rs485_proxy {

#define PROXY_AUTH_TIMEOUT_MS 5000

class RS485Proxy : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void set_port(uint16_t port) { this->port_ = port; }
  void set_allow_tx(bool allow_tx) { this->allow_tx_ = allow_tx; }
  void set_auth_token(const std::string &auth_token) { this->auth_token_ = auth_token; }
  void set_rts_pin(InternalGPIOPin *rts_pin) { this->rts_pin_ = rts_pin; }
  void set_send_gaps(bool send_gaps) { this->send_gaps_ = send_gaps; }
  void set_gap_threshold_us(uint32_t gap_threshold_us) { this->gap_threshold_us_ = gap_threshold_us; }

 protected:
  void setup_server_();
  void accept_client_();
  void disconnect_client_();
  void send_hello_();
  void send_line_(const std::string &line);
  void service_uart_();
  void service_client_();
  void handle_command_(const std::string &line);
  bool tx_is_authorized_(const char *command_name);
  bool bus_is_idle_for_tx_();
  bool parse_hex_(const std::string &hex, std::vector<uint8_t> *out);
  int hex_value_(char c);
  void send_uart_bytes_(const std::vector<uint8_t> &data, bool with_break);
  void flush_rx_batch_(uint8_t *data, size_t len, uint32_t timestamp_us);
  std::string hex_encode_(const uint8_t *data, size_t len);

  uint16_t port_{6638};
  bool allow_tx_{false};
  std::string auth_token_;
  InternalGPIOPin *rts_pin_{nullptr};
  bool send_gaps_{true};
  uint32_t gap_threshold_us_{3000};

  std::unique_ptr<socket::ListenSocket> server_;
  std::unique_ptr<socket::Socket> client_;
  bool authenticated_{false};
  char command_buffer_[256]{};
  size_t command_buffer_len_{0};
  uint32_t client_connected_ms_{0};
  uint32_t last_rx_us_{0};
  uint32_t rx_sequence_{0};
  uint32_t tx_sequence_{0};
};

}  // namespace rs485_proxy
}  // namespace esphome
