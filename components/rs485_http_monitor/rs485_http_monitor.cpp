#include "rs485_http_monitor.h"

#include <cerrno>
#include <cstring>

namespace esphome {
namespace rs485_http_monitor {

static const char *const TAG = "rs485_http_monitor";

static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>RS485 Monitor</title>"
    "<style>body{font-family:system-ui,sans-serif;margin:24px;background:#101418;color:#e8edf2}"
    "code,pre{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}"
    "pre{white-space:pre-wrap;background:#05070a;border:1px solid #2a3440;padding:12px;min-height:70vh}"
    "a{color:#8cc8ff}</style></head><body>"
    "<h1>RS485 Monitor</h1><p><a href=\"/stats\">stats</a> <a href=\"/recent\">recent</a> "
    "<a href=\"/stream\">plain stream</a></p><pre id=\"log\"></pre>"
    "<script>const log=document.getElementById('log');function add(x){log.textContent+=x+'\\n';"
    "log.scrollTop=log.scrollHeight;}const es=new EventSource('/events');"
    "es.addEventListener('hello',e=>add('HELLO '+e.data));"
    "es.addEventListener('rx',e=>add('RX '+e.data));"
    "es.addEventListener('gap',e=>add('GAP '+e.data));"
    "es.onerror=()=>add('stream error or reconnecting');</script></body></html>";

void RS485HTTPMonitor::setup() { this->setup_server_(); }

float RS485HTTPMonitor::get_setup_priority() const { return setup_priority::AFTER_WIFI; }

void RS485HTTPMonitor::dump_config() {
  ESP_LOGCONFIG(TAG, "RS485 HTTP Monitor");
  ESP_LOGCONFIG(TAG, "  Port: %u", this->port_);
  ESP_LOGCONFIG(TAG, "  Send Gaps: %s", this->send_gaps_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Gap Threshold: %uus", (unsigned int) this->gap_threshold_us_);
  ESP_LOGCONFIG(TAG, "  History Size: %u", this->history_size_);
}

void RS485HTTPMonitor::loop() {
  this->accept_client_();
  this->service_pending_client_();
  this->service_uart_();
  this->service_keepalive_();
}

void RS485HTTPMonitor::setup_server_() {
  this->server_ = socket::socket_ip_loop_monitored(SOCK_STREAM, 0);
  if (this->server_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create HTTP monitor socket");
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
    ESP_LOGE(TAG, "Failed to set HTTP monitor server nonblocking: errno=%d", errno);
    this->mark_failed();
    return;
  }

  struct sockaddr_storage server_addr;
  socklen_t server_addr_len = socket::set_sockaddr_any((struct sockaddr *) &server_addr, sizeof(server_addr), this->port_);
  if (server_addr_len == 0) {
    ESP_LOGE(TAG, "Failed to build HTTP monitor bind address");
    this->mark_failed();
    return;
  }

  err = this->server_->bind((struct sockaddr *) &server_addr, server_addr_len);
  if (err != 0) {
    ESP_LOGE(TAG, "Failed to bind HTTP monitor port %u: errno=%d", this->port_, errno);
    this->mark_failed();
    return;
  }

  err = this->server_->listen(2);
  if (err != 0) {
    ESP_LOGE(TAG, "Failed to listen on HTTP monitor port %u: errno=%d", this->port_, errno);
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "RS485 HTTP monitor listening on port %u", this->port_);
}

void RS485HTTPMonitor::accept_client_() {
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
    int enable = 1;
    new_client->setsockopt(IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
    int err = new_client->setblocking(false);
    if (err != 0) {
      ESP_LOGW(TAG, "Rejecting HTTP monitor client from %s, nonblocking failed: errno=%d", peername, errno);
      new_client.reset();
      continue;
    }

    if (this->pending_client_ != nullptr) {
      ESP_LOGW(TAG, "Rejecting HTTP monitor client from %s because another request is pending", peername);
      this->send_http_response_(std::move(new_client), "503 Service Unavailable", "text/plain; charset=utf-8",
                                "busy\n");
      continue;
    }

    ESP_LOGD(TAG, "Accepted HTTP monitor client from %s", peername);
    this->pending_client_ = std::move(new_client);
    this->request_buffer_len_ = 0;
  }
}

void RS485HTTPMonitor::service_pending_client_() {
  if (this->pending_client_ == nullptr || !this->pending_client_->ready()) {
    return;
  }

  while (true) {
    char buffer[96];
    ssize_t read_len = this->pending_client_->read(buffer, sizeof(buffer));
    if (read_len > 0) {
      for (ssize_t i = 0; i < read_len; i++) {
        char c = buffer[i];
        if (c == '\r') {
          continue;
        }
        if (c == '\n') {
          this->request_buffer_[this->request_buffer_len_] = '\0';
          std::string request_line(this->request_buffer_, this->request_buffer_len_);
          auto client = std::move(this->pending_client_);
          this->request_buffer_len_ = 0;
          this->handle_http_request_(std::move(client), request_line);
          return;
        }
        if (this->request_buffer_len_ < sizeof(this->request_buffer_) - 1) {
          this->request_buffer_[this->request_buffer_len_++] = c;
        } else {
          this->send_http_response_(std::move(this->pending_client_), "414 URI Too Long", "text/plain; charset=utf-8",
                                    "request line too long\n");
          this->request_buffer_len_ = 0;
          return;
        }
      }
      continue;
    }

    if (read_len == 0) {
      this->pending_client_.reset();
      this->request_buffer_len_ = 0;
      return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }

    ESP_LOGW(TAG, "HTTP monitor read failed: errno=%d", errno);
    this->pending_client_.reset();
    this->request_buffer_len_ = 0;
    return;
  }
}

void RS485HTTPMonitor::handle_http_request_(std::unique_ptr<socket::Socket> client, const std::string &request_line) {
  const std::string path = this->path_from_request_line_(request_line);
  if (path.empty()) {
    this->send_http_response_(std::move(client), "400 Bad Request", "text/plain; charset=utf-8", "bad request\n");
    return;
  }

  if (path == "/" || path == "/index.html") {
    this->send_http_response_(std::move(client), "200 OK", "text/html; charset=utf-8", INDEX_HTML);
    return;
  }
  if (path == "/stats") {
    this->send_http_response_(std::move(client), "200 OK", "application/json", this->stats_json_());
    return;
  }
  if (path == "/recent") {
    this->send_http_response_(std::move(client), "200 OK", "text/plain; charset=utf-8", this->recent_body_());
    return;
  }
  if (path == "/events") {
    this->start_stream_(std::move(client), true);
    return;
  }
  if (path == "/stream") {
    this->start_stream_(std::move(client), false);
    return;
  }

  this->send_http_response_(std::move(client), "404 Not Found", "text/plain; charset=utf-8", "not found\n");
}

void RS485HTTPMonitor::start_stream_(std::unique_ptr<socket::Socket> client, bool sse) {
  if (this->stream_client_ != nullptr) {
    this->send_http_response_(std::move(client), "409 Conflict", "text/plain; charset=utf-8",
                              "stream already connected\n");
    return;
  }

  const char *content_type = sse ? "text/event-stream" : "text/plain; charset=utf-8";
  std::string header = "HTTP/1.1 200 OK\r\nContent-Type: ";
  header += content_type;
  header += "\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
  ssize_t written = client->write(header.data(), header.size());
  if (written < 0 || (size_t) written != header.size()) {
    ESP_LOGW(TAG, "Failed to write HTTP stream header: errno=%d", errno);
    return;
  }

  this->stream_client_ = std::move(client);
  this->stream_is_sse_ = sse;
  this->last_keepalive_ms_ = millis();
  this->send_stream_line_("hello", this->stats_json_(), "HELLO rs485_http_monitor v1 baud=19200 mode=rx-only");
  ESP_LOGI(TAG, "HTTP monitor %s stream connected", sse ? "SSE" : "plain-text");
}

void RS485HTTPMonitor::send_http_response_(std::unique_ptr<socket::Socket> client, const char *status,
                                           const char *content_type, const std::string &body) {
  std::string response = "HTTP/1.1 ";
  response += status;
  response += "\r\nContent-Type: ";
  response += content_type;
  response += "\r\nContent-Length: ";
  response += std::to_string(body.size());
  response += "\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
  response += body;
  client->write(response.data(), response.size());
}

void RS485HTTPMonitor::service_uart_() {
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
          this->emit_rx_(batch, batch_len, batch_timestamp);
          batch_len = 0;
        }
        this->emit_gap_(now, gap);
      }
    }

    if (batch_len == 0) {
      batch_timestamp = now;
    }
    batch[batch_len++] = byte;
    this->last_rx_us_ = now;

    if (batch_len == sizeof(batch)) {
      this->emit_rx_(batch, batch_len, batch_timestamp);
      batch_len = 0;
    }
  }

  if (batch_len > 0) {
    this->emit_rx_(batch, batch_len, batch_timestamp);
  }
}

void RS485HTTPMonitor::emit_rx_(uint8_t *data, size_t len, uint32_t timestamp_us) {
  const std::string hex = this->hex_encode_(data, len);
  const uint32_t seq = ++this->rx_sequence_;
  this->rx_bytes_ += len;

  std::string plain_line = "RX ";
  plain_line += std::to_string(seq);
  plain_line += " ";
  plain_line += std::to_string(timestamp_us);
  plain_line += " ";
  plain_line += hex;
  this->append_recent_(plain_line);

  std::string json = "{\"seq\":";
  json += std::to_string(seq);
  json += ",\"micros\":";
  json += std::to_string(timestamp_us);
  json += ",\"hex\":\"";
  json += hex;
  json += "\"}";
  this->send_stream_line_("rx", json, plain_line);
}

void RS485HTTPMonitor::emit_gap_(uint32_t timestamp_us, uint32_t gap_us) {
  const uint32_t seq = ++this->gap_sequence_;
  char plain[96];
  snprintf(plain, sizeof(plain), "GAP %lu %lu %lu", (unsigned long) seq, (unsigned long) timestamp_us,
           (unsigned long) gap_us);
  std::string plain_line = plain;
  this->append_recent_(plain_line);

  std::string json = "{\"seq\":";
  json += std::to_string(seq);
  json += ",\"micros\":";
  json += std::to_string(timestamp_us);
  json += ",\"gap_us\":";
  json += std::to_string(gap_us);
  json += "}";
  this->send_stream_line_("gap", json, plain_line);
}

void RS485HTTPMonitor::send_stream_line_(const std::string &event_type, const std::string &json_data,
                                         const std::string &plain_line) {
  if (this->stream_client_ == nullptr) {
    return;
  }

  std::string out;
  if (this->stream_is_sse_) {
    out = "event: ";
    out += event_type;
    out += "\ndata: ";
    out += json_data;
    out += "\n\n";
  } else {
    out = plain_line;
    out += "\n";
  }

  ssize_t written = this->stream_client_->write(out.data(), out.size());
  if (written < 0 || (size_t) written != out.size()) {
    ESP_LOGW(TAG, "HTTP monitor stream write failed or partial: errno=%d", errno);
    this->disconnect_stream_client_();
  }
}

void RS485HTTPMonitor::service_keepalive_() {
  if (this->stream_client_ == nullptr || millis() - this->last_keepalive_ms_ < 15000) {
    return;
  }
  this->last_keepalive_ms_ = millis();
  if (this->stream_is_sse_) {
    std::string out = ": keepalive\n\n";
    ssize_t written = this->stream_client_->write(out.data(), out.size());
    if (written < 0 || (size_t) written != out.size()) {
      this->disconnect_stream_client_();
    }
  } else {
    this->send_stream_line_("keepalive", "{}", "# keepalive");
  }
}

void RS485HTTPMonitor::disconnect_stream_client_() {
  if (this->stream_client_ != nullptr) {
    ESP_LOGI(TAG, "HTTP monitor stream disconnected");
  }
  this->stream_client_.reset();
}

void RS485HTTPMonitor::append_recent_(const std::string &line) {
  if (this->history_size_ == 0) {
    return;
  }
  this->recent_lines_.push_back(line);
  while (this->recent_lines_.size() > this->history_size_) {
    this->recent_lines_.pop_front();
  }
}

std::string RS485HTTPMonitor::recent_body_() {
  std::string out;
  for (const auto &line : this->recent_lines_) {
    out += line;
    out += "\n";
  }
  return out;
}

std::string RS485HTTPMonitor::stats_json_() {
  std::string json = "{\"component\":\"rs485_http_monitor\",\"baud\":19200,\"mode\":\"rx-only\",\"rx_sequence\":";
  json += std::to_string(this->rx_sequence_);
  json += ",\"rx_bytes\":";
  json += std::to_string(this->rx_bytes_);
  json += ",\"gap_sequence\":";
  json += std::to_string(this->gap_sequence_);
  json += ",\"last_rx_us\":";
  json += std::to_string(this->last_rx_us_);
  json += ",\"stream_connected\":";
  json += this->stream_client_ == nullptr ? "false" : "true";
  json += ",\"history_size\":";
  json += std::to_string(this->history_size_);
  json += "}\n";
  return json;
}

std::string RS485HTTPMonitor::hex_encode_(const uint8_t *data, size_t len) {
  static const char *const hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out.push_back(hex[(data[i] >> 4) & 0x0F]);
    out.push_back(hex[data[i] & 0x0F]);
  }
  return out;
}

std::string RS485HTTPMonitor::path_from_request_line_(const std::string &request_line) {
  if (request_line.rfind("GET ", 0) != 0) {
    return "";
  }
  size_t start = 4;
  size_t end = request_line.find(' ', start);
  if (end == std::string::npos || end <= start) {
    return "";
  }
  std::string path = request_line.substr(start, end - start);
  size_t query_start = path.find('?');
  if (query_start != std::string::npos) {
    path.resize(query_start);
  }
  return path;
}

}  // namespace rs485_http_monitor
}  // namespace esphome
