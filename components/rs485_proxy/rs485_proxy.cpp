#include "rs485_proxy.h"

#include <cerrno>
#include <cstring>

namespace esphome {
namespace rs485_proxy {

static const char *const TAG = "rs485_proxy";

void RS485Proxy::setup() {
  if (this->rts_pin_ != nullptr) {
    this->rts_pin_->setup();
    this->rts_pin_->pin_mode(gpio::Flags::FLAG_OUTPUT);
    this->rts_pin_->digital_write(false);
  }
  this->setup_server_();
}

float RS485Proxy::get_setup_priority() const { return setup_priority::AFTER_WIFI; }

void RS485Proxy::dump_config() {
  ESP_LOGCONFIG(TAG, "RS485 TCP Proxy");
  ESP_LOGCONFIG(TAG, "  Port: %u", this->port_);
  ESP_LOGCONFIG(TAG, "  TX Enabled: %s", this->allow_tx_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Auth Required: %s", this->auth_token_.empty() ? "false" : "true");
  ESP_LOGCONFIG(TAG, "  Send Gaps: %s", this->send_gaps_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Gap Threshold: %uus", (unsigned int) this->gap_threshold_us_);
  if (this->rts_pin_ != nullptr) {
    char rts_pin_buf[GPIO_SUMMARY_MAX_LEN];
    this->rts_pin_->dump_summary(rts_pin_buf, sizeof(rts_pin_buf));
    ESP_LOGCONFIG(TAG, "  RTS Pin: %s", rts_pin_buf);
  } else {
    ESP_LOGCONFIG(TAG, "  RTS Pin: none, assuming hardware auto-direction");
  }
}

void RS485Proxy::loop() {
  this->accept_client_();
  this->service_uart_();
  this->service_client_();
}

void RS485Proxy::setup_server_() {
  this->server_ = socket::socket_ip_loop_monitored(SOCK_STREAM, 0);
  if (this->server_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create TCP server socket");
    this->mark_failed();
    return;
  }

  int enable = 1;
  int err = this->server_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  if (err != 0) {
    ESP_LOGW(TAG, "SO_REUSEADDR failed: errno=%d", errno);
  }

  err = this->server_->setblocking(false);
  if (err != 0) {
    ESP_LOGE(TAG, "Failed to set TCP server nonblocking: errno=%d", errno);
    this->mark_failed();
    return;
  }

  struct sockaddr_storage server_addr;
  socklen_t server_addr_len = socket::set_sockaddr_any((struct sockaddr *) &server_addr, sizeof(server_addr), this->port_);
  if (server_addr_len == 0) {
    ESP_LOGE(TAG, "Failed to build TCP bind address");
    this->mark_failed();
    return;
  }

  err = this->server_->bind((struct sockaddr *) &server_addr, server_addr_len);
  if (err != 0) {
    ESP_LOGE(TAG, "Failed to bind TCP port %u: errno=%d", this->port_, errno);
    this->mark_failed();
    return;
  }

  err = this->server_->listen(1);
  if (err != 0) {
    ESP_LOGE(TAG, "Failed to listen on TCP port %u: errno=%d", this->port_, errno);
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "RS485 proxy listening on TCP port %u", this->port_);
}

void RS485Proxy::accept_client_() {
  if (this->server_ == nullptr || !this->server_->ready()) {
    return;
  }

  while (true) {
    struct sockaddr_storage source_addr;
    socklen_t source_addr_len = sizeof(source_addr);
    auto new_client = this->server_->accept_loop_monitored((struct sockaddr *) &source_addr, &source_addr_len);
    if (!new_client) {
      break;
    }

    char peername[socket::SOCKADDR_STR_LEN];
    new_client->getpeername_to(peername);
    if (this->client_ != nullptr) {
      ESP_LOGW(TAG, "Rejecting second proxy client from %s", peername);
      new_client.reset();
      continue;
    }

    int enable = 1;
    new_client->setsockopt(IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
    int err = new_client->setblocking(false);
    if (err != 0) {
      ESP_LOGW(TAG, "Rejecting proxy client from %s, nonblocking failed: errno=%d", peername, errno);
      new_client.reset();
      continue;
    }

    ESP_LOGI(TAG, "Accepted proxy client from %s", peername);
    this->client_ = std::move(new_client);
    this->authenticated_ = this->auth_token_.empty();
    this->command_buffer_len_ = 0;
    this->client_connected_ms_ = millis();
    this->last_rx_us_ = 0;
    this->rx_sequence_ = 0;
    this->tx_sequence_ = 0;
    this->send_hello_();
  }
}

void RS485Proxy::disconnect_client_() {
  if (this->client_ != nullptr) {
    ESP_LOGI(TAG, "Proxy client disconnected");
  }
  this->client_.reset();
  this->authenticated_ = false;
  this->command_buffer_len_ = 0;
}

void RS485Proxy::send_hello_() {
  std::string line = "HELLO rs485_proxy v1 baud=19200 mode=";
  line += this->allow_tx_ ? "tx-capable" : "rx-only";
  line += " auth=";
  line += this->auth_token_.empty() ? "none" : "required";
  this->send_line_(line);
}

void RS485Proxy::send_line_(const std::string &line) {
  if (this->client_ == nullptr) {
    return;
  }

  std::string out = line;
  out += "\n";
  ssize_t written = this->client_->write(out.data(), out.size());
  if (written < 0 || (size_t) written != out.size()) {
    ESP_LOGW(TAG, "TCP write failed or partial: written=%d expected=%u errno=%d",
             (int) written, (unsigned int) out.size(), errno);
    this->disconnect_client_();
  }
}

void RS485Proxy::service_uart_() {
  if (this->client_ == nullptr || !this->authenticated_) {
    return;
  }

  uint8_t batch[64];
  size_t batch_len = 0;
  uint32_t batch_timestamp = 0;

  while (this->available() > 0) {
    uint8_t byte;
    if (!this->read_byte(&byte)) {
      break;
    }

    const uint32_t now = micros();
    if (this->send_gaps_ && this->last_rx_us_ != 0) {
      const uint32_t gap = now - this->last_rx_us_;
      if (gap > this->gap_threshold_us_) {
        if (batch_len > 0) {
          this->flush_rx_batch_(batch, batch_len, batch_timestamp);
          batch_len = 0;
        }
        char gap_line[64];
        snprintf(gap_line, sizeof(gap_line), "GAP %lu %lu", (unsigned long) now, (unsigned long) gap);
        this->send_line_(gap_line);
      }
    }

    if (batch_len == 0) {
      batch_timestamp = now;
    }
    batch[batch_len++] = byte;
    this->last_rx_us_ = now;

    if (batch_len == sizeof(batch)) {
      this->flush_rx_batch_(batch, batch_len, batch_timestamp);
      batch_len = 0;
    }
  }

  if (batch_len > 0) {
    this->flush_rx_batch_(batch, batch_len, batch_timestamp);
  }
}

void RS485Proxy::flush_rx_batch_(uint8_t *data, size_t len, uint32_t timestamp_us) {
  const std::string hex = this->hex_encode_(data, len);
  char prefix[64];
  snprintf(prefix, sizeof(prefix), "RX %lu %lu ", (unsigned long) ++this->rx_sequence_, (unsigned long) timestamp_us);
  this->send_line_(std::string(prefix) + hex);
}

void RS485Proxy::service_client_() {
  if (this->client_ == nullptr) {
    return;
  }

  if (!this->authenticated_ && !this->auth_token_.empty() &&
      millis() - this->client_connected_ms_ > PROXY_AUTH_TIMEOUT_MS) {
    ESP_LOGW(TAG, "Disconnecting unauthenticated proxy client after auth timeout");
    this->disconnect_client_();
    return;
  }

  if (!this->client_->ready()) {
    return;
  }

  while (true) {
    char buffer[96];
    ssize_t read_len = this->client_->read(buffer, sizeof(buffer));
    if (read_len > 0) {
      for (ssize_t i = 0; i < read_len; i++) {
        char c = buffer[i];
        if (c == '\r') {
          continue;
        }
        if (c == '\n') {
          this->command_buffer_[this->command_buffer_len_] = '\0';
          this->handle_command_(std::string(this->command_buffer_, this->command_buffer_len_));
          this->command_buffer_len_ = 0;
        } else if (this->command_buffer_len_ < sizeof(this->command_buffer_) - 1) {
          this->command_buffer_[this->command_buffer_len_++] = c;
        } else {
          this->send_line_("ERR command-too-long");
          this->command_buffer_len_ = 0;
        }
      }
      continue;
    }

    if (read_len == 0) {
      this->disconnect_client_();
      return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }

    ESP_LOGW(TAG, "TCP read failed: errno=%d", errno);
    this->disconnect_client_();
    return;
  }
}

void RS485Proxy::handle_command_(const std::string &line) {
  if (line.empty()) {
    return;
  }

  if (line == "PING") {
    this->send_line_("PONG");
    return;
  }
  if (line == "INFO") {
    this->send_hello_();
    return;
  }
  if (line == "CLOSE") {
    this->disconnect_client_();
    return;
  }

  if (line.rfind("AUTH ", 0) == 0) {
    const std::string token = line.substr(5);
    if (!this->auth_token_.empty() && token == this->auth_token_) {
      this->authenticated_ = true;
      this->send_line_("OK AUTH");
    } else {
      this->send_line_("ERR auth-failed");
    }
    return;
  }

  if (!this->authenticated_) {
    this->send_line_("ERR auth-required");
    return;
  }

  if (line.rfind("TXB ", 0) == 0 || line.rfind("TX ", 0) == 0) {
    const bool with_break = line[2] == 'B';
    const size_t hex_start = with_break ? 4 : 3;
    std::vector<uint8_t> bytes;
    if (!this->parse_hex_(line.substr(hex_start), &bytes)) {
      this->send_line_("ERR bad-hex");
      return;
    }
    if (bytes.empty()) {
      this->send_line_("ERR empty-tx");
      return;
    }
    if (!this->tx_is_authorized_(with_break ? "TXB" : "TX")) {
      return;
    }
    if (!this->bus_is_idle_for_tx_()) {
      this->send_line_("ERR bus-not-idle");
      ESP_LOGW(TAG, "Rejected proxy TX because the RS-485 bus is not idle");
      return;
    }
    this->send_uart_bytes_(bytes, with_break);
    return;
  }

  this->send_line_("ERR unknown-command");
}

bool RS485Proxy::tx_is_authorized_(const char *command_name) {
  if (!this->allow_tx_) {
    this->send_line_(std::string("ERR tx-disabled ") + command_name);
    ESP_LOGW(TAG, "Rejected proxy %s because allow_tx is false", command_name);
    return false;
  }
  if (!this->authenticated_) {
    this->send_line_("ERR auth-required");
    return false;
  }
  return true;
}

bool RS485Proxy::bus_is_idle_for_tx_() {
  if (this->available() > 0) {
    return false;
  }
  if (this->last_rx_us_ == 0) {
    return true;
  }
  return micros() - this->last_rx_us_ > this->gap_threshold_us_;
}

bool RS485Proxy::parse_hex_(const std::string &hex, std::vector<uint8_t> *out) {
  out->clear();
  int high_nibble = -1;

  for (char c : hex) {
    if (c == ' ' || c == ':' || c == '-' || c == '\t') {
      continue;
    }
    int value = this->hex_value_(c);
    if (value < 0) {
      return false;
    }
    if (high_nibble < 0) {
      high_nibble = value;
    } else {
      out->push_back((uint8_t) ((high_nibble << 4) | value));
      high_nibble = -1;
    }
    if (out->size() > 128) {
      return false;
    }
  }

  return high_nibble < 0;
}

int RS485Proxy::hex_value_(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

void RS485Proxy::send_uart_bytes_(const std::vector<uint8_t> &data, bool with_break) {
  if (this->rts_pin_ != nullptr) {
    this->rts_pin_->digital_write(true);
  }

  if (with_break) {
    this->parent_->set_baud_rate(9600);
    this->parent_->set_data_bits(7);
    this->parent_->set_parity(esphome::uart::UARTParityOptions::UART_CONFIG_PARITY_NONE);
    this->parent_->set_stop_bits(1);
    this->parent_->load_settings(false);
    this->write_byte(0x00);
    this->flush();
  }

  this->parent_->set_baud_rate(19200);
  this->parent_->set_data_bits(8);
  this->parent_->set_parity(esphome::uart::UARTParityOptions::UART_CONFIG_PARITY_NONE);
  this->parent_->set_stop_bits(1);
  this->parent_->load_settings(false);
  this->write_array(data.data(), data.size());
  this->flush();

  if (this->rts_pin_ != nullptr) {
    this->rts_pin_->digital_write(false);
  }

  char line[96];
  snprintf(line, sizeof(line), "OK TX %lu bytes=%u break=%u", (unsigned long) ++this->tx_sequence_,
           (unsigned int) data.size(), with_break ? 1 : 0);
  this->send_line_(line);
  ESP_LOGI(TAG, "Proxy transmitted %u byte(s), break=%u", (unsigned int) data.size(), with_break ? 1 : 0);
}

std::string RS485Proxy::hex_encode_(const uint8_t *data, size_t len) {
  static const char *const hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out.push_back(hex[(data[i] >> 4) & 0x0F]);
    out.push_back(hex[data[i] & 0x0F]);
  }
  return out;
}

}  // namespace rs485_proxy
}  // namespace esphome
