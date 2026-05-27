#include "uapbridge_esp.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "esphome/components/watchdog/watchdog.h"
#include "esp_spiffs.h"

namespace esphome {
namespace uapbridge_esp {
static const char *const TAG = "uapbridge_esp";
static constexpr uint32_t UAPBRIDGE_PERSISTENT_LOG_MAGIC = 0x5531504C;  // "U1PL"
static constexpr uint32_t UAPBRIDGE_PERSISTENT_LOG_VERSION = 0x00040001;
static constexpr const char *UAPBRIDGE_PERSISTENT_LOG_PARTITION = "hcp_logs";
static constexpr const char *UAPBRIDGE_PERSISTENT_LOG_MOUNT = "/hcp";
static constexpr const char *UAPBRIDGE_PERSISTENT_LOG_FILE = "/hcp/current.hcplog";
static constexpr uint8_t UAPBRIDGE_PLOG_TYPE_RX = 1;
static constexpr uint8_t UAPBRIDGE_PLOG_TYPE_TX = 2;
static constexpr uint8_t UAPBRIDGE_PLOG_TYPE_GAP = 3;
static constexpr uint8_t UAPBRIDGE_PLOG_TYPE_FRAME = 4;
static constexpr uint8_t UAPBRIDGE_PLOG_TYPE_CMD = 5;
static constexpr uint8_t UAPBRIDGE_PLOG_TYPE_STATUS = 6;
static constexpr uint8_t UAPBRIDGE_PLOG_TYPE_CONTROL = 7;
static constexpr uint8_t UAPBRIDGE_PLOG_FLAG_CRC_OK = 0x01;
static constexpr uint8_t UAPBRIDGE_PLOG_FIXED_PAYLOAD_LEN = 34;
static constexpr uint8_t UAPBRIDGE_PLOG_MAX_RECORD_TOTAL_LEN =
    1 + UAPBRIDGE_PLOG_FIXED_PAYLOAD_LEN + UAPBRIDGE_PERSISTENT_LOG_DATA_LEN;
static constexpr uint32_t UAPBRIDGE_PERSISTENT_LOG_SAVE_INTERVAL_MS = 10000;

static void plog_write_u16(uint8_t *dst, uint16_t value) {
  dst[0] = value & 0xFF;
  dst[1] = (value >> 8) & 0xFF;
}

static void plog_write_u32(uint8_t *dst, uint32_t value) {
  dst[0] = value & 0xFF;
  dst[1] = (value >> 8) & 0xFF;
  dst[2] = (value >> 16) & 0xFF;
  dst[3] = (value >> 24) & 0xFF;
}

static uint16_t plog_read_u16(const uint8_t *src) {
  return (uint16_t) src[0] | ((uint16_t) src[1] << 8);
}

static uint32_t plog_read_u32(const uint8_t *src) {
  return (uint32_t) src[0] | ((uint32_t) src[1] << 8) | ((uint32_t) src[2] << 16) | ((uint32_t) src[3] << 24);
}

static const char UAPBRIDGE_HTTP_INDEX_HTML[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>UAP1 HCP Monitor</title>"
    "<style>body{font-family:system-ui,sans-serif;margin:24px;background:#101418;color:#e8edf2}"
    "code,pre{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}"
    "pre{white-space:pre-wrap;background:#05070a;border:1px solid #2a3440;padding:12px;min-height:70vh}"
    "a{color:#8cc8ff}</style></head><body>"
    "<h1>UAP1 HCP Monitor</h1><p><a href=\"/stats\">stats</a> <a href=\"/recent\">recent</a> "
    "<a href=\"/broadcast_status\">broadcast status</a> <a href=\"/persistent_log\">persistent log</a> "
    "<a href=\"/stream\">plain stream</a></p><pre id=\"log\"></pre>"
    "<script>const log=document.getElementById('log');function add(x){log.textContent+=x+'\\n';"
    "log.scrollTop=log.scrollHeight;}const es=new EventSource('/events');"
    "for(const t of ['hello','rx','tx','gap','frame','cmd','status'])es.addEventListener(t,e=>add(t.toUpperCase()+' '+e.data));"
    "es.onerror=()=>add('stream error or reconnecting');</script></body></html>";

void UAPBridge_esp::setup() {
  esphome::uapbridge::UAPBridge::setup();
  this->setup_persistent_log_();
  if (this->http_debug_enabled_()) {
    this->setup_http_debug_server_();
  }
}

float UAPBridge_esp::get_setup_priority() const { return setup_priority::AFTER_WIFI; }

void UAPBridge_esp::dump_config() {
  esphome::uapbridge::UAPBridge::dump_config();
  if (this->http_debug_enabled_()) {
    ESP_LOGCONFIG(TAG, "  HTTP Debug Port: %u", this->http_debug_port_);
    ESP_LOGCONFIG(TAG, "  HTTP Debug Send Gaps: %s", this->http_debug_send_gaps_ ? "true" : "false");
    ESP_LOGCONFIG(TAG, "  HTTP Debug Gap Threshold: %uus", (unsigned int) this->http_debug_gap_threshold_us_);
    ESP_LOGCONFIG(TAG, "  HTTP Debug History Size: %u", this->http_debug_history_size_);
  }
  ESP_LOGCONFIG(TAG, "  Persistent Protocol Log: %s", this->persistent_log_enabled_ ? "enabled" : "disabled");
  ESP_LOGCONFIG(TAG, "  Persistent Protocol Log Storage: SPIFFS partition '%s' mounted=%s",
                UAPBRIDGE_PERSISTENT_LOG_PARTITION, this->persistent_log_fs_mounted_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Persistent Protocol Log RAM Staging: %u bytes",
                (unsigned int) UAPBRIDGE_PERSISTENT_LOG_RAM_CAPACITY);
  ESP_LOGCONFIG(TAG, "  Persistent Protocol Log Max File: %u bytes",
                (unsigned int) UAPBRIDGE_PERSISTENT_LOG_MAX_FILE_BYTES);
}

void UAPBridge_esp::loop() {
  this->loop_fast();
  this->loop_slow();
  this->http_debug_accept_client_();
  this->http_debug_service_pending_client_();
  this->http_debug_service_keepalive_();
  this->service_persistent_log_save_();

  if (this->data_has_changed) {
    ESP_LOGD(TAG, "UAPBridge_esp::loop() - received Data has changed.");
    this->clear_data_changed_flag();
    this->state_callback_.call();
  }
}

void UAPBridge_esp::loop_fast() {
  this->receive();
  this->expire_pending_command();
  this->expire_valid_broadcast();

  if (millis() - this->last_call < CYCLE_TIME) {
    // avoid unnecessary frequent calls 
    return;
  }
  this->last_call = millis();
  if(this->send_time!=0 && (millis() >= this->send_time)) {
    ESP_LOGVV(TAG, "loop: transmitting");
    this->transmit();
    this->send_time = 0;
  }
}
/**
 * this seems to be a function that only logs stati and makes errorcorrections
 */
void UAPBridge_esp::loop_slow() {
  if (millis() - this->last_call_slow < CYCLE_TIME_SLOW) {
    // avoid unnecessary frequent calls 
    return;
  }
  this->last_call_slow = millis();

  ESP_LOGV(TAG, "loop_slow called - status=0x%04X", this->broadcast_status);
  if (!this->valid_broadcast) {
    return;
  }
  const hoermann_state_t new_state = this->decode_status_state(this->broadcast_status);
  this->apply_broadcast_status(this->broadcast_status);

  // --- Auto Error Correction ---
  const bool error_active = (this->broadcast_status & hoermann_state_error) == hoermann_state_error;
  if(this->auto_correction && error_active && !this->last_error_bit) {
    // if error just came up
    // if an error is detected and door is open/closed then try to reset it by requesting opening/closing without movement
    ESP_LOGD(TAG, "autocorrection started");
    bool correction_queued = false;
    if (new_state == hoermann_state_open) {
      correction_queued = this->set_command(true, hoermann_action_open);
    } else if (new_state == hoermann_state_closed) {
      correction_queued = this->set_command(true, hoermann_action_close);
    } else if (new_state == hoermann_state_stopped) {
      // in this state it is not possible to clear the error. But the next open or close cycle will clear it
      this->auto_correction_in_progress = false;
    }
    this->auto_correction_in_progress = correction_queued;
  }
  if(this->auto_correction) {
    // HINT: i guess if light is on it is sufficent to toggle the light off to reset the error
    // HINT: propably not. this will disable the lamp after correcting the error
    // or both
    if (this->auto_correction_in_progress && (this->broadcast_status & hoermann_state_light_relay)) {
      this->set_command(true, hoermann_action_toggle_light);
      this->auto_correction_in_progress = false;
    }
  }
  if (!error_active) {
    this->auto_correction_in_progress = false;
  }
  this->last_error_bit = error_active;
  // --- Auto Error Correction ---
}

void UAPBridge_esp::receive() {
  uint8_t debug_batch[64];
  size_t debug_batch_len = 0;
  uint32_t debug_batch_timestamp = 0;

  auto flush_debug_batch = [&]() {
    if (debug_batch_len > 0) {
      this->append_persistent_log_raw_(UAPBRIDGE_PLOG_TYPE_RX, debug_batch, debug_batch_len, debug_batch_timestamp);
      this->http_debug_emit_rx_(debug_batch, debug_batch_len, debug_batch_timestamp);
      debug_batch_len = 0;
    }
  };

  while (this->available() > 0) {
#if ESPHOME_LOGLEVEL >= ESPHOME_LOG_LEVEL_VERY_VERBOSE
    if (this->byte_cnt > 5) {
    	// data have not been fetched and will be ignored --> log them at least for debugging purposes
    	char temp[4];
    	sprintf(temp, "%02X ", this->rx_data[0]);
    	ESP_LOGVV(TAG, "in receive while avaiable: %s", temp);
    }
#endif
    // shift old elements and read new; only the last 5 bytes are evaluated; if there are more in the buffer, the older ones are ignored
    for (uint8_t i = 0; i < 4; i++) {
      this->rx_data[i] = this->rx_data[i+1];
    }
    uint8_t byte = 0;
    if(this->read_byte(&byte)){
      if (this->http_debug_enabled_() || this->persistent_log_enabled_) {
        const uint32_t now = micros();
        if (this->http_debug_send_gaps_ && this->http_debug_last_rx_us_ != 0) {
          const uint32_t gap = now - this->http_debug_last_rx_us_;
          if (gap > this->http_debug_gap_threshold_us_) {
            flush_debug_batch();
            this->append_persistent_log_gap_(now, gap);
            this->http_debug_emit_gap_(now, gap);
          }
        }
        if (debug_batch_len == 0) {
          debug_batch_timestamp = now;
        }
        debug_batch[debug_batch_len++] = byte;
        this->http_debug_last_rx_us_ = now;
        if (debug_batch_len == sizeof(debug_batch)) {
          flush_debug_batch();
        }
      }

      this->rx_data[4] = byte;
      //if read was successful
      this->byte_cnt++;
      this->process_rx_window();
    }
  }

  if (this->http_debug_enabled_() || this->persistent_log_enabled_) {
    flush_debug_batch();
  }
}

void UAPBridge_esp::process_rx_window() {
    uint8_t   length  = 0;
    uint8_t   counter = 0;
    ESP_LOGVV(TAG, "new data received");
    // Slave scan
    // 28 82 01 80 06
    if (this->rx_data[0] == UAP1_ADDR) {
      length = this->rx_data[1] & 0x0F;
      bool crc_valid = false;
      if (length == 2) {
        crc_valid = calc_crc8(this->rx_data, length + 3) == 0x00;
      }
      if (this->rx_data[2] == CMD_SLAVE_SCAN && this->rx_data[3] == UAP1_ADDR_MASTER && length == 2 && crc_valid) {
        this->update_diagnostic_string("scan", this->rx_data, 0, 5, true);
        ESP_LOGVV(TAG, "SlaveScan: %s", print_data(this->rx_data, 0, 5));
        ESP_LOGV(TAG, "->      SlaveScan"); 
        if (this->listen_only) {
          ESP_LOGD(TAG, "Listen-only mode: not responding to slave scan");
          return;
        }
        counter = (this->rx_data[1] & 0xF0) + 0x10;
        this->tx_data[0] = UAP1_ADDR_MASTER;
        this->tx_data[1] = 0x02 | counter;
        this->tx_data[2] = UAP1_TYPE;
        this->tx_data[3] = UAP1_ADDR;
        this->tx_data[4] = calc_crc8(this->tx_data, 4);
        this->tx_length = 5;
        this->transmit_prepared_response();
      } else if (this->diagnostic_mode && this->rx_data[2] == CMD_SLAVE_SCAN) {
        this->update_diagnostic_string("scan-invalid", this->rx_data, 0, 5, crc_valid);
        ESP_LOGD(TAG, "Invalid slave scan candidate len=%u crc=%s data=%s", length, crc_valid ? "ok" : "bad", print_data(this->rx_data, 0, 5));
      }
    }
    // Broadcast status
    // 00 92 12 02 35
    if (this->rx_data[0] == BROADCAST_ADDR) {
      length = this->rx_data[1] & 0x0F;
      bool crc_valid = false;
      if (length == 2) {
        crc_valid = calc_crc8(this->rx_data, length + 3) == 0x00;
      }
      if (length == 2 && crc_valid) {
        this->update_diagnostic_string("broadcast", this->rx_data, 0, 5, true);
        ESP_LOGVV(TAG, "Broadcast: %s", print_data(this->rx_data, 0, 5));
        ESP_LOGV(TAG, "->      Broadcast");
        const uint16_t new_status = this->rx_data[2] | ((uint16_t)this->rx_data[3] << 8);
        this->broadcast_status = new_status;
        this->apply_broadcast_status(new_status, "broadcast");
        this->last_valid_broadcast_ms = millis();
        if (!this->valid_broadcast) {
          this->valid_broadcast = true;
          this->data_has_changed = true;
        }
        if (this->diagnostic_mode && new_status != this->last_logged_status) {
          this->log_decoded_status(new_status);
          this->last_logged_status = new_status;
        }
      } else if (this->diagnostic_mode && !(this->rx_data[1] == BROADCAST_ADDR && ((this->rx_data[2] & 0x0F) == 1))) {
        this->update_diagnostic_string("broadcast-invalid", this->rx_data, 0, 5, crc_valid);
        ESP_LOGD(TAG, "Invalid broadcast candidate len=%u crc=%s data=%s", length, crc_valid ? "ok" : "bad", print_data(this->rx_data, 0, 5));
      }
    }
    // SupraMatic E2 observed one-byte broadcast status with a visible sync byte:
    // 00 00 01 01 C3 -> sync, broadcast addr, length 1, d0, crc.
    if (this->rx_data[0] == BROADCAST_ADDR && this->rx_data[1] == BROADCAST_ADDR) {
      length = this->rx_data[2] & 0x0F;
      bool crc_valid = false;
      if (length == 1) {
        crc_valid = calc_crc8(&this->rx_data[1], length + 3) == 0x00;
      }
      if (length == 1 && crc_valid) {
        this->update_diagnostic_string("broadcast-len1", this->rx_data, 1, 5, true);
        ESP_LOGVV(TAG, "Broadcast len1: %s", print_data(this->rx_data, 1, 5));
        ESP_LOGV(TAG, "->      Broadcast len1");
        const uint16_t new_status = this->rx_data[3];
        this->broadcast_status = new_status;
        this->apply_broadcast_status(new_status, "broadcast-len1");
        this->last_valid_broadcast_ms = millis();
        if (!this->valid_broadcast) {
          this->valid_broadcast = true;
          this->data_has_changed = true;
        }
        if (this->diagnostic_mode && new_status != this->last_logged_status) {
          this->log_decoded_status(new_status);
          this->last_logged_status = new_status;
        }
      } else if (this->diagnostic_mode && length == 1) {
        this->update_diagnostic_string("broadcast-len1-invalid", this->rx_data, 1, 5, crc_valid);
        ESP_LOGD(TAG, "Invalid len1 broadcast candidate len=%u crc=%s data=%s", length, crc_valid ? "ok" : "bad", print_data(this->rx_data, 1, 5));
      }
    }
    // Slave status request (only 4 byte --> other indices of rx_data!)
    // 28 A1 20 2E
    if (this->rx_data[1] == UAP1_ADDR) {
      length = this->rx_data[2] & 0x0F;
      bool crc_valid = false;
      if (length == 1) {
        crc_valid = calc_crc8(&this->rx_data[1], length + 3) == 0x00;
      }
      if (this->rx_data[3] == CMD_SLAVE_STATUS_REQUEST && length == 1 && crc_valid) {
        this->update_diagnostic_string("status-request", this->rx_data, 1, 5, true);
        ESP_LOGVV(TAG, "Slave status request: %s", print_data(this->rx_data, 1, 5));
        ESP_LOGV(TAG, "->      Slave status request");
        if (this->listen_only) {
          ESP_LOGD(TAG, "Listen-only mode: not responding to status request");
          return;
        }
        counter = (this->rx_data[2] & 0xF0) + 0x10;
        this->tx_data[0] = UAP1_ADDR_MASTER;
        this->tx_data[1] = 0x03 | counter;
        this->tx_data[2] = CMD_SLAVE_STATUS_RESPONSE;
        this->tx_data[3] = (uint8_t)(this->next_action & 0xFF);
        this->tx_data[4] = (uint8_t)((this->next_action >> 8) & 0xFF);
        if (this->next_action != hoermann_action_none) {
          ESP_LOGI(TAG, "Sending one-shot HCP command: %s (0x%04X)", this->action_name(this->next_action), (unsigned int) this->next_action);
          this->http_debug_emit_command_("sent", this->next_action);
          if (this->next_action == hoermann_action_open || this->next_action == hoermann_action_close ||
              this->next_action == hoermann_action_venting || this->next_action == hoermann_action_impulse) {
            this->movement_command_callback_.call();
          }
          this->command_sequence++;
        }
        this->next_action = hoermann_action_none;
        this->command_set_at = 0;
        this->tx_data[5] = calc_crc8(this->tx_data, 5);
        this->tx_length = 6;
        this->transmit_prepared_response();
      } else if (this->diagnostic_mode && this->rx_data[3] == CMD_SLAVE_STATUS_REQUEST) {
        this->update_diagnostic_string("status-request-invalid", this->rx_data, 1, 5, crc_valid);
        ESP_LOGD(TAG, "Invalid status request candidate len=%u crc=%s data=%s", length, crc_valid ? "ok" : "bad", print_data(this->rx_data, 1, 5));
      }
    }
#if ESPHOME_LOGLEVEL >= ESPHOME_LOG_LEVEL_VERY_VERBOSE
    // just print the data
    if (this->byte_cnt >= 5) {
      ESP_LOGVV(TAG, "Just printed: %s", print_data(this->rx_data, 0, 5));
    }	
#endif
}

void UAPBridge_esp::transmit() {
  ESP_LOGVV(TAG, "Transmit: %s", print_data(this->tx_data, 0, this->tx_length));
  if (this->rts_pin_ != nullptr) {
    this->rts_pin_->digital_write(true);// LOW(false) = listen, HIGH(true) = transmit
  }
  // Generate Sync break
  this->parent_->set_baud_rate(9600);
  this->parent_->set_data_bits(7);
  this->parent_->set_parity(esphome::uart::UARTParityOptions::UART_CONFIG_PARITY_NONE);
  this->parent_->set_stop_bits(1);
  this->parent_->load_settings(false);
  this->write_byte(0x00);
  this->flush();

  // Transmit
  this->parent_->set_baud_rate(19200);
  this->parent_->set_data_bits(8);
  this->parent_->set_parity(esphome::uart::UARTParityOptions::UART_CONFIG_PARITY_NONE);
  this->parent_->set_stop_bits(1);
  this->parent_->load_settings(false);
  this->write_array(this->tx_data, this->tx_length);
  this->flush();
  this->append_persistent_log_raw_(UAPBRIDGE_PLOG_TYPE_TX, this->tx_data, this->tx_length, micros());
  this->http_debug_emit_tx_(this->tx_data, this->tx_length, micros());

  if (this->rts_pin_ != nullptr) {
    this->rts_pin_->digital_write(false);// LOW(false) = listen, HIGH(true) = transmit
  }

  ESP_LOGVV(TAG, "TX duration: %dms", millis() - this->send_time);
}

void UAPBridge_esp::transmit_prepared_response() {
  if (this->tx_length == 0) {
    return;
  }
  this->send_time = millis();
  this->transmit();
  this->send_time = 0;
  this->tx_length = 0;
}
/**
 * Helper to set next Command and *not* skip Current Command before end was sent
 */
bool UAPBridge_esp::set_command(bool cond, const hoermann_action_t command, bool bypass_impulse_interlock) {
  if (!this->command_allowed(command, bypass_impulse_interlock)) {
    return false;
  }
  if (cond) {
    if (this->next_action != hoermann_action_none) {
      ESP_LOGW(TAG, "Last command was not yet fetched by HCP master; keeping queued action %s (0x%04X), rejected %s",
               this->action_name(this->next_action), (unsigned int) this->next_action, this->action_name(command));
      this->http_debug_emit_command_("blocked", command, "previous_command_pending");
      return false;
    } else {
      this->next_action = command;
      this->command_set_at = millis();
      if (command == hoermann_action_open && this->obstruction_state) {
        ESP_LOGI(TAG, "Clearing obstruction/close failure on accepted open command");
        this->set_obstruction_state(false);
      }
      ESP_LOGI(TAG, "Queued one-shot HCP command: %s (0x%04X)", this->action_name(command), (unsigned int) command);
      this->http_debug_emit_command_("queued", command);
      return true;
    }
  } else {
    if (command == hoermann_action_open && this->obstruction_state && this->state == hoermann_state_open) {
      ESP_LOGI(TAG, "Clearing obstruction/close failure because an open command was requested while HCP already reports open");
      this->set_obstruction_state(false);
    }
    ESP_LOGD(TAG, "Skipped HCP command %s because requested state already matches decoded state", this->action_name(command));
    this->http_debug_emit_command_("skipped", command, "state_already_matches");
    return false;
  }
}

bool UAPBridge_esp::command_allowed(const hoermann_action_t command, bool bypass_impulse_interlock) {
  if (this->listen_only) {
    ESP_LOGW(TAG, "Blocked HCP command %s because listen_only is true", this->action_name(command));
    this->http_debug_emit_command_("blocked", command, "listen_only");
    return false;
  }

  if (command == hoermann_action_close && !this->allow_remote_close) {
    ESP_LOGW(TAG, "Blocked close command because allow_remote_close is false");
    this->http_debug_emit_command_("blocked", command, "close_disabled");
    return false;
  }
  if (command == hoermann_action_impulse && !this->allow_remote_impulse && !bypass_impulse_interlock) {
    ESP_LOGW(TAG, "Blocked impulse command because allow_remote_impulse is false");
    this->http_debug_emit_command_("blocked", command, "impulse_disabled");
    return false;
  }
  if (command == hoermann_action_stop && !this->use_unverified_stop_command) {
    ESP_LOGW(TAG, "Blocked raw stop command because use_unverified_stop_command is false");
    this->http_debug_emit_command_("blocked", command, "raw_stop_disabled");
    return false;
  }

  if (this->require_fresh_broadcast_for_commands && !this->bus_state_is_fresh()) {
    ESP_LOGW(TAG, "Blocked HCP command %s because no fresh valid HCP broadcast is available", this->action_name(command));
    this->http_debug_emit_command_("blocked", command, "stale_broadcast");
    return false;
  }

  const hoermann_state_t gate_state = this->decode_status_state(this->broadcast_status);
  const bool gate_error = (this->broadcast_status & hoermann_state_error) != 0;
  const bool gate_prewarn = (this->broadcast_status & hoermann_state_prewarn) != 0;
  const bool obstruction_recovery_open = this->obstruction_state && command == hoermann_action_open;

  if (command == hoermann_action_venting && !this->allow_remote_close && gate_state == hoermann_state_open) {
    ESP_LOGW(TAG, "Blocked venting command from open state because allow_remote_close is false");
    this->http_debug_emit_command_("blocked", command, "venting_from_open_close_disabled");
    return false;
  }

  if (this->obstruction_state &&
      (command == hoermann_action_close || command == hoermann_action_venting ||
       (command == hoermann_action_impulse && !bypass_impulse_interlock))) {
    ESP_LOGW(TAG, "Blocked HCP command %s because obstruction/close failure is active", this->action_name(command));
    this->http_debug_emit_command_("blocked", command, "obstruction_active");
    return false;
  }

  if (this->is_movement_command(command) && !obstruction_recovery_open &&
      (gate_state == hoermann_state_unknown || gate_state == hoermann_state_stopped)) {
    ESP_LOGW(TAG, "Blocked movement HCP command %s because decoded door state is %s",
             this->action_name(command), this->state_string.c_str());
    this->http_debug_emit_command_("blocked", command, "unsafe_state");
    return false;
  }

  if (this->is_movement_command(command) && !obstruction_recovery_open && (gate_error || gate_prewarn)) {
    ESP_LOGW(TAG, "Blocked movement HCP command %s because error/prewarn is active", this->action_name(command));
    this->http_debug_emit_command_("blocked", command, "error_or_prewarn");
    return false;
  }

  return true;
}

bool UAPBridge_esp::is_movement_command(const hoermann_action_t command) {
  switch (command) {
    case hoermann_action_open:
    case hoermann_action_close:
    case hoermann_action_impulse:
    case hoermann_action_venting:
    case hoermann_action_stop:
      return true;
    default:
      return false;
  }
}

bool UAPBridge_esp::bus_state_is_fresh() const {
  if (!this->valid_broadcast || this->last_valid_broadcast_ms == 0) {
    return false;
  }
  if (this->valid_broadcast_timeout_ms == 0) {
    return true;
  }
  return millis() - this->last_valid_broadcast_ms <= this->valid_broadcast_timeout_ms;
}

bool UAPBridge_esp::moving_state_is_fresh() const {
  if (!this->bus_state_is_fresh() || this->last_moving_broadcast_ms == 0) {
    return false;
  }
  return millis() - this->last_moving_broadcast_ms <= STOP_FALLBACK_MOVING_TIMEOUT_MS;
}

UAPBridge_esp::hoermann_state_t UAPBridge_esp::decode_status_state(uint16_t status) const {
  if (status & hoermann_state_open) {
    return hoermann_state_open;
  }
  if (status & hoermann_state_closed) {
    return hoermann_state_closed;
  }
  if ((status & (hoermann_state_direction | hoermann_state_moving)) == hoermann_state_opening) {
    return hoermann_state_opening;
  }
  if ((status & (hoermann_state_direction | hoermann_state_moving)) == hoermann_state_closing) {
    return hoermann_state_closing;
  }
  if (status & hoermann_state_venting) {
    return hoermann_state_venting;
  }
  if (status == 0) {
    return hoermann_state_stopped;
  }
  return hoermann_state_unknown;
}

void UAPBridge_esp::apply_broadcast_status(uint16_t status, const char *frame_type) {
  const hoermann_state_t new_state = this->decode_status_state(status);
  if (new_state == hoermann_state_opening || new_state == hoermann_state_closing) {
    this->last_moving_broadcast_ms = millis();
  }
  if (new_state == hoermann_state_closed) {
    this->set_obstruction_state(false);
  }
  if (new_state != this->state) {
    this->handle_state_change(new_state);
  }

  this->update_boolean_state("relay", this->relay_enabled, (status & hoermann_state_opt_relay) != 0);
  this->update_boolean_state("light", this->light_enabled, (status & hoermann_state_light_relay) != 0);
  this->update_boolean_state("vent", this->venting_enabled, (status & hoermann_state_venting) != 0);
  this->update_boolean_state("err", this->error_state, (status & hoermann_state_error) != 0);
  this->update_boolean_state("prewarn", this->prewarn_state, (status & hoermann_state_prewarn) != 0);
  if (frame_type != nullptr) {
    this->record_broadcast_status_(status, frame_type);
  }
}

void UAPBridge_esp::expire_pending_command() {
  if (this->next_action == hoermann_action_none || this->command_timeout_ms == 0 || this->command_set_at == 0) {
    return;
  }

  if (millis() - this->command_set_at > this->command_timeout_ms) {
    ESP_LOGW(TAG, "Expired queued HCP command %s (0x%04X) after %ums without a status request",
             this->action_name(this->next_action), (unsigned int) this->next_action, (unsigned int) this->command_timeout_ms);
    this->http_debug_emit_command_("expired", this->next_action, "status_request_timeout");
    this->next_action = hoermann_action_none;
    this->command_set_at = 0;
  }
}

void UAPBridge_esp::expire_valid_broadcast() {
  if (!this->valid_broadcast || this->valid_broadcast_timeout_ms == 0 || this->last_valid_broadcast_ms == 0) {
    return;
  }

  if (millis() - this->last_valid_broadcast_ms > this->valid_broadcast_timeout_ms) {
    ESP_LOGW(TAG, "No valid HCP broadcast for %ums; marking bus state stale", (unsigned int) this->valid_broadcast_timeout_ms);
    this->valid_broadcast = false;
    this->broadcast_status = 0;
    this->last_moving_broadcast_ms = 0;
    this->last_error_bit = false;
    this->auto_correction_in_progress = false;
    if (this->next_action != hoermann_action_none) {
      ESP_LOGW(TAG, "Dropping queued HCP command %s because bus state is stale", this->action_name(this->next_action));
      this->http_debug_emit_command_("dropped", this->next_action, "bus_state_stale");
      this->next_action = hoermann_action_none;
      this->command_set_at = 0;
    }
    this->update_boolean_state("relay", this->relay_enabled, false);
    this->update_boolean_state("light", this->light_enabled, false);
    this->update_boolean_state("vent", this->venting_enabled, false);
    this->update_boolean_state("err", this->error_state, false);
    this->update_boolean_state("prewarn", this->prewarn_state, false);
    this->handle_state_change(hoermann_state_unknown);
    this->data_has_changed = true;
    this->last_valid_broadcast_ms = 0;
  }
}

bool UAPBridge_esp::action_open() {
  ESP_LOGD(TAG, "Action: open called");
  return this->set_command(this->state != hoermann_state_open, hoermann_action_open);
}

bool UAPBridge_esp::action_close() {
  ESP_LOGD(TAG, "Action: close called");
  return this->set_command(this->state != hoermann_state_closed, hoermann_action_close);
}

bool UAPBridge_esp::action_stop() {
  ESP_LOGD(TAG, "Action: stop called");
  if (this->next_action != hoermann_action_none) {
    ESP_LOGI(TAG, "Cancelled queued HCP command before it was fetched: %s (0x%04X)",
             this->action_name(this->next_action), (unsigned int) this->next_action);
    this->http_debug_emit_command_("cancelled", this->next_action, "stop_before_fetch");
    this->next_action = hoermann_action_none;
    this->command_set_at = 0;
    return true;
  }
  if (this->use_unverified_stop_command) {
    return this->set_command(true, hoermann_action_stop);
  }
  if (!this->bus_state_is_fresh()) {
    ESP_LOGW(TAG, "Blocked stop fallback because no fresh valid HCP broadcast is available");
    return false;
  }
  if ((this->state == hoermann_state_opening || this->state == hoermann_state_closing) && this->moving_state_is_fresh()) {
    ESP_LOGI(TAG, "Using impulse as stop fallback while door is moving");
    return this->set_command(true, hoermann_action_impulse, true);
  } else if (this->state == hoermann_state_opening || this->state == hoermann_state_closing) {
    ESP_LOGW(TAG, "Blocked stop fallback because no recent moving HCP broadcast is available");
    return false;
  } else {
    ESP_LOGW(TAG, "Ignored stop fallback because decoded state is not moving: %s", this->state_string.c_str());
    return false;
  }
}

bool UAPBridge_esp::action_venting() {
  ESP_LOGD(TAG, "Action: venting called");
  return this->set_command(this->state != hoermann_state_venting, hoermann_action_venting);
}

bool UAPBridge_esp::action_toggle_light() {
  ESP_LOGD(TAG, "Action: toggle light called");
  return this->set_command(true, hoermann_action_toggle_light);
}

bool UAPBridge_esp::action_impulse() {
  ESP_LOGD(TAG, "Action: impulse called");
  return this->set_command(true, hoermann_action_impulse);
}

UAPBridge_esp::hoermann_state_t UAPBridge_esp::get_state() {
  return this->state;
}

std::string UAPBridge_esp::get_state_string() {
  return this->state_string;
}

std::string UAPBridge_esp::get_diagnostic_string() {
  return this->diagnostic_string;
}

uint16_t UAPBridge_esp::get_raw_status() {
  return this->broadcast_status;
}

void UAPBridge_esp::set_venting(bool state) {
  if (state) {
    this->action_venting();
  } else {
    this->action_close();
  }
  ESP_LOGD(TAG, "Venting state set to %s", state ? "ON" : "OFF");
}

void UAPBridge_esp::set_light(bool state) {
  this->set_command((this->light_enabled != state), hoermann_action_toggle_light);
  ESP_LOGD(TAG, "Light state set to %s", state ? "ON" : "OFF");
}

uint8_t UAPBridge_esp::calc_crc8(uint8_t *p_data, uint8_t length) {
  uint8_t i;
  uint8_t data;
  uint8_t crc = 0xF3;
  
  for(i = 0; i < length; i++) {
    /* XOR-in next input byte */
    data = *p_data ^ crc;
    p_data++;
    /* get current CRC value = remainder */
    crc = crc_table[data];
  }
  
  return crc;
}

char* UAPBridge_esp::print_data(uint8_t *p_data, uint8_t from, uint8_t to) {
	char temp[4];
	static char output[30];

  sprintf(output, "%5lu: ", millis() & 0xFFFFul);
	for (uint8_t i = from; i < to; i++) {
		sprintf(temp, "%02X ", p_data[i]);
		strcat(output, temp);
	}
	this->byte_cnt = 0;
  return &output[0];
}

void UAPBridge_esp::handle_state_change(hoermann_state_t new_state) {
  this->state = new_state;
  ESP_LOGV(TAG, "State changed from %s to %d", this->state_string.c_str(), new_state);
  switch (new_state) {
    case hoermann_state_open:
      this->state_string = "Open";
      break;
    case hoermann_state_closed:
      this->state_string = "Closed";
      break;
    case hoermann_state_opening:
      this->state_string = "Opening";
      break;
    case hoermann_state_closing:
      this->state_string = "Closing";
      break;
    case hoermann_state_venting:
      this->state_string = "Venting";
      break;
    case hoermann_state_stopped:
      this->state_string = "Stopped";
      break;
    case hoermann_state_unknown:
      this->state_string = "Unknown";
      break;
    default:
      this->state_string = "Error";
      break;
  }

  this->data_has_changed = true;
}

void UAPBridge_esp::update_boolean_state(const char * name, bool &current_state, bool new_state) {
  ESP_LOGV(TAG, "update_boolean_state: %s from %s to %s", name, current_state ? "true" : "false", new_state ? "true" : "false");
  if (current_state != new_state) {
    current_state = new_state;
    this->data_has_changed = true;
  }
}

const char *UAPBridge_esp::action_name(hoermann_action_t command) {
  switch (command) {
    case hoermann_action_stop:
      return "stop";
    case hoermann_action_open:
      return "open";
    case hoermann_action_close:
      return "close";
    case hoermann_action_impulse:
      return "impulse";
    case hoermann_action_toggle_light:
      return "toggle_light";
    case hoermann_action_venting:
      return "venting";
    case hoermann_action_none:
      return "none";
    default:
      return "unknown";
  }
}

void UAPBridge_esp::update_diagnostic_string(const char *frame_type, uint8_t *p_data, uint8_t from, uint8_t to, bool crc_valid) {
  char output[96];
  int written = snprintf(output, sizeof(output), "%lu %s crc=%s data=", millis(), frame_type, crc_valid ? "ok" : "bad");
  for (uint8_t i = from; i < to && written > 0 && written < (int) sizeof(output); i++) {
    written += snprintf(output + written, sizeof(output) - written, "%02X ", p_data[i]);
  }
  this->diagnostic_string = output;
  this->http_debug_emit_frame_(frame_type, p_data, from, to, crc_valid);
  if (this->diagnostic_mode) {
    ESP_LOGD(TAG, "%s", this->diagnostic_string.c_str());
  }
}

void UAPBridge_esp::log_decoded_status(uint16_t status) {
  ESP_LOGD(TAG,
           "Decoded status 0x%04X: open=%u closed=%u relay=%u light=%u error=%u direction=%u moving=%u venting=%u prewarn=%u",
           status,
           (status & hoermann_state_open) != 0,
           (status & hoermann_state_closed) != 0,
           (status & hoermann_state_opt_relay) != 0,
           (status & hoermann_state_light_relay) != 0,
           (status & hoermann_state_error) != 0,
           (status & hoermann_state_direction) != 0,
           (status & hoermann_state_moving) != 0,
           (status & hoermann_state_venting) != 0,
           (status & hoermann_state_prewarn) != 0);
}

void UAPBridge_esp::setup_http_debug_server_() {
  this->http_debug_server_ = socket::socket_ip_loop_monitored(SOCK_STREAM, 0);
  if (this->http_debug_server_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create UAP1 HTTP debug socket");
    return;
  }

  int enable = 1;
  int err = this->http_debug_server_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  if (err != 0) {
    ESP_LOGW(TAG, "SO_REUSEADDR failed: errno=%d", errno);
  }

  err = this->http_debug_server_->setblocking(false);
  if (err != 0) {
    ESP_LOGE(TAG, "Failed to set UAP1 HTTP debug server nonblocking: errno=%d", errno);
    this->http_debug_server_.reset();
    return;
  }

  struct sockaddr_storage server_addr;
  socklen_t server_addr_len =
      socket::set_sockaddr_any((struct sockaddr *) &server_addr, sizeof(server_addr), this->http_debug_port_);
  if (server_addr_len == 0) {
    ESP_LOGE(TAG, "Failed to build UAP1 HTTP debug bind address");
    this->http_debug_server_.reset();
    return;
  }

  err = this->http_debug_server_->bind((struct sockaddr *) &server_addr, server_addr_len);
  if (err != 0) {
    ESP_LOGE(TAG, "Failed to bind UAP1 HTTP debug port %u: errno=%d", this->http_debug_port_, errno);
    this->http_debug_server_.reset();
    return;
  }

  err = this->http_debug_server_->listen(2);
  if (err != 0) {
    ESP_LOGE(TAG, "Failed to listen on UAP1 HTTP debug port %u: errno=%d", this->http_debug_port_, errno);
    this->http_debug_server_.reset();
    return;
  }

  ESP_LOGI(TAG, "UAP1 HTTP debug monitor listening on port %u", this->http_debug_port_);
}

void UAPBridge_esp::http_debug_accept_client_() {
  if (!this->http_debug_enabled_() || this->http_debug_server_ == nullptr || !this->http_debug_server_->ready()) {
    return;
  }

  while (true) {
    struct sockaddr_storage source_addr;
    socklen_t source_addr_len = sizeof(source_addr);
    auto new_client = this->http_debug_server_->accept_loop_monitored((struct sockaddr *) &source_addr,
                                                                      &source_addr_len);
    if (!new_client) {
      break;
    }

    char peername[socket::SOCKADDR_STR_LEN];
    new_client->getpeername_to(peername);
    int enable = 1;
    new_client->setsockopt(IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
    int err = new_client->setblocking(false);
    if (err != 0) {
      ESP_LOGW(TAG, "Rejecting UAP1 HTTP debug client from %s, nonblocking failed: errno=%d", peername, errno);
      new_client.reset();
      continue;
    }

    if (this->http_debug_pending_client_ != nullptr) {
      this->http_debug_send_response_(std::move(new_client), "503 Service Unavailable",
                                      "text/plain; charset=utf-8", "busy\n");
      continue;
    }

    ESP_LOGD(TAG, "Accepted UAP1 HTTP debug client from %s", peername);
    this->http_debug_pending_client_ = std::move(new_client);
    this->http_debug_request_buffer_len_ = 0;
    this->http_debug_pending_client_started_ms_ = millis();
  }
}

void UAPBridge_esp::http_debug_service_pending_client_() {
  if (this->http_debug_pending_client_ == nullptr) {
    return;
  }
  if (millis() - this->http_debug_pending_client_started_ms_ > UAPBRIDGE_HTTP_PENDING_TIMEOUT_MS) {
    this->http_debug_pending_client_.reset();
    this->http_debug_request_buffer_len_ = 0;
    return;
  }
  if (!this->http_debug_pending_client_->ready()) {
    return;
  }

  while (true) {
    char buffer[96];
    ssize_t read_len = this->http_debug_pending_client_->read(buffer, sizeof(buffer));
    if (read_len > 0) {
      for (ssize_t i = 0; i < read_len; i++) {
        char c = buffer[i];
        if (c == '\r') {
          continue;
        }
        if (c == '\n') {
          this->http_debug_request_buffer_[this->http_debug_request_buffer_len_] = '\0';
          std::string request_line(this->http_debug_request_buffer_, this->http_debug_request_buffer_len_);
          auto client = std::move(this->http_debug_pending_client_);
          this->http_debug_request_buffer_len_ = 0;
          this->http_debug_handle_request_(std::move(client), request_line);
          return;
        }
        if (this->http_debug_request_buffer_len_ < sizeof(this->http_debug_request_buffer_) - 1) {
          this->http_debug_request_buffer_[this->http_debug_request_buffer_len_++] = c;
        } else {
          this->http_debug_send_response_(std::move(this->http_debug_pending_client_), "414 URI Too Long",
                                          "text/plain; charset=utf-8", "request line too long\n");
          this->http_debug_request_buffer_len_ = 0;
          return;
        }
      }
      continue;
    }

    if (read_len == 0) {
      this->http_debug_pending_client_.reset();
      this->http_debug_request_buffer_len_ = 0;
      return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }

    ESP_LOGW(TAG, "UAP1 HTTP debug read failed: errno=%d", errno);
    this->http_debug_pending_client_.reset();
    this->http_debug_request_buffer_len_ = 0;
    return;
  }
}

void UAPBridge_esp::http_debug_handle_request_(std::unique_ptr<socket::Socket> client,
                                               const std::string &request_line) {
  const std::string path = this->http_debug_path_from_request_line_(request_line);
  if (path.empty()) {
    this->http_debug_send_response_(std::move(client), "400 Bad Request", "text/plain; charset=utf-8",
                                    "bad request\n");
    return;
  }

  if (path == "/" || path == "/index.html") {
    this->http_debug_send_response_(std::move(client), "200 OK", "text/html; charset=utf-8",
                                    UAPBRIDGE_HTTP_INDEX_HTML);
    return;
  }
  if (path == "/stats") {
    this->http_debug_send_response_(std::move(client), "200 OK", "application/json",
                                    this->http_debug_stats_json_());
    return;
  }
  if (path == "/broadcast_status") {
    this->http_debug_send_response_(std::move(client), "200 OK", "application/json",
                                    this->http_debug_broadcast_status_json_());
    return;
  }
  if (path == "/persistent_log") {
    this->save_persistent_log_(true);
    this->http_debug_send_persistent_log_response_(std::move(client));
    return;
  }
  if (path == "/persistent_log.bin") {
    this->save_persistent_log_(true);
    this->http_debug_send_persistent_log_binary_response_(std::move(client));
    return;
  }
  if (path == "/persistent_log/start") {
    if (!this->persistent_log_fs_mounted_) {
      this->setup_persistent_log_(false);
    }
    this->persistent_log_enabled_ = true;
    this->append_persistent_log_record_(UAPBRIDGE_PLOG_TYPE_CONTROL, 0, 1, 0, 0, this->broadcast_status,
                                        nullptr, 0, 0, micros());
    this->save_persistent_log_(true);
    this->http_debug_send_response_(std::move(client), "200 OK", "application/json",
                                    this->http_debug_persistent_log_summary_json_());
    return;
  }
  if (path == "/persistent_log/stop") {
    this->append_persistent_log_record_(UAPBRIDGE_PLOG_TYPE_CONTROL, 0, 2, 0, 0, this->broadcast_status,
                                        nullptr, 0, 0, micros());
    this->persistent_log_enabled_ = false;
    this->save_persistent_log_(true);
    this->http_debug_send_response_(std::move(client), "200 OK", "application/json",
                                    this->http_debug_persistent_log_summary_json_());
    return;
  }
  if (path == "/persistent_log/clear") {
    if (!this->persistent_log_fs_mounted_) {
      this->setup_persistent_log_(false);
    }
    this->clear_persistent_log_();
    this->http_debug_send_response_(std::move(client), "200 OK", "application/json",
                                    this->http_debug_persistent_log_summary_json_());
    return;
  }
  if (path == "/persistent_log/format") {
    this->format_persistent_log_();
    this->http_debug_send_response_(std::move(client), "200 OK", "application/json",
                                    this->http_debug_persistent_log_summary_json_());
    return;
  }
  if (path == "/recent") {
    this->http_debug_send_response_(std::move(client), "200 OK", "text/plain; charset=utf-8",
                                    this->http_debug_recent_body_());
    return;
  }
  if (path == "/events") {
    this->http_debug_start_stream_(std::move(client), true);
    return;
  }
  if (path == "/stream") {
    this->http_debug_start_stream_(std::move(client), false);
    return;
  }

  this->http_debug_send_response_(std::move(client), "404 Not Found", "text/plain; charset=utf-8", "not found\n");
}

void UAPBridge_esp::http_debug_start_stream_(std::unique_ptr<socket::Socket> client, bool sse) {
  if (this->http_debug_stream_client_ != nullptr) {
    this->http_debug_send_response_(std::move(client), "409 Conflict", "text/plain; charset=utf-8",
                                    "stream already connected\n");
    return;
  }

  const char *content_type = sse ? "text/event-stream" : "text/plain; charset=utf-8";
  std::string header = "HTTP/1.1 200 OK\r\nContent-Type: ";
  header += content_type;
  header += "\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
  if (!this->http_debug_write_all_(client.get(), header, 1000)) {
    ESP_LOGW(TAG, "Failed to write UAP1 HTTP debug stream header: errno=%d", errno);
    return;
  }

  this->http_debug_stream_client_ = std::move(client);
  this->http_debug_stream_is_sse_ = sse;
  this->http_debug_last_keepalive_ms_ = millis();
  this->http_debug_send_stream_line_("hello", this->http_debug_stats_json_(),
                                     "HELLO uapbridge_esp_http_debug v1 baud=19200 mode=uap1-emulation");
}

void UAPBridge_esp::http_debug_send_response_(std::unique_ptr<socket::Socket> client, const char *status,
                                              const char *content_type, const std::string &body) {
  std::string response = "HTTP/1.1 ";
  response += status;
  response += "\r\nContent-Type: ";
  response += content_type;
  response += "\r\nContent-Length: ";
  response += std::to_string(body.size());
  response += "\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
  response += body;
  if (!this->http_debug_write_all_(client.get(), response, 1000)) {
    ESP_LOGW(TAG, "Failed to write complete UAP1 HTTP debug response: errno=%d", errno);
  }
}

bool UAPBridge_esp::http_debug_write_all_(socket::Socket *client, const std::string &payload, uint32_t timeout_ms) {
  size_t offset = 0;
  const uint32_t started = millis();
  while (offset < payload.size()) {
    ssize_t written = client->write(payload.data() + offset, payload.size() - offset);
    if (written > 0) {
      offset += written;
      continue;
    }
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      return false;
    }
    if (millis() - started > timeout_ms) {
      return false;
    }
    delay(1);
  }
  return true;
}

void UAPBridge_esp::http_debug_send_stream_line_(const std::string &event_type, const std::string &json_data,
                                                 const std::string &plain_line) {
  if (this->http_debug_stream_client_ == nullptr) {
    return;
  }

  std::string out;
  if (this->http_debug_stream_is_sse_) {
    out = "event: ";
    out += event_type;
    out += "\ndata: ";
    out += json_data;
    out += "\n\n";
  } else {
    out = plain_line;
    out += "\n";
  }

  ssize_t written = this->http_debug_stream_client_->write(out.data(), out.size());
  if (written < 0 || (size_t) written != out.size()) {
    this->http_debug_disconnect_stream_client_();
  }
}

void UAPBridge_esp::http_debug_emit_rx_(const uint8_t *data, size_t len, uint32_t timestamp_us) {
  if (!this->http_debug_enabled_() || len == 0) {
    return;
  }
  const std::string hex = this->http_debug_hex_encode_(data, len);
  const uint32_t seq = ++this->http_debug_rx_sequence_;
  this->http_debug_rx_bytes_ += len;

  std::string plain = "RX ";
  plain += std::to_string(seq);
  plain += " ";
  plain += std::to_string(timestamp_us);
  plain += " ";
  plain += hex;
  this->http_debug_append_recent_(plain);

  std::string json = "{\"seq\":";
  json += std::to_string(seq);
  json += ",\"micros\":";
  json += std::to_string(timestamp_us);
  json += ",\"hex\":\"";
  json += hex;
  json += "\"}";
  this->http_debug_send_stream_line_("rx", json, plain);
}

void UAPBridge_esp::http_debug_emit_tx_(const uint8_t *data, size_t len, uint32_t timestamp_us) {
  if (!this->http_debug_enabled_() || len == 0) {
    return;
  }
  const std::string hex = this->http_debug_hex_encode_(data, len);
  const uint32_t seq = ++this->http_debug_tx_sequence_;
  this->http_debug_tx_bytes_ += len;

  std::string plain = "TX ";
  plain += std::to_string(seq);
  plain += " ";
  plain += std::to_string(timestamp_us);
  plain += " ";
  plain += hex;
  this->http_debug_append_recent_(plain);

  std::string json = "{\"seq\":";
  json += std::to_string(seq);
  json += ",\"micros\":";
  json += std::to_string(timestamp_us);
  json += ",\"hex\":\"";
  json += hex;
  json += "\"}";
  this->http_debug_send_stream_line_("tx", json, plain);
}

void UAPBridge_esp::http_debug_emit_gap_(uint32_t timestamp_us, uint32_t gap_us) {
  if (!this->http_debug_enabled_()) {
    return;
  }
  const uint32_t seq = ++this->http_debug_gap_sequence_;
  char plain[96];
  snprintf(plain, sizeof(plain), "GAP %lu %lu %lu", (unsigned long) seq, (unsigned long) timestamp_us,
           (unsigned long) gap_us);
  this->http_debug_append_recent_(plain);

  std::string json = "{\"seq\":";
  json += std::to_string(seq);
  json += ",\"micros\":";
  json += std::to_string(timestamp_us);
  json += ",\"gap_us\":";
  json += std::to_string(gap_us);
  json += "}";
  this->http_debug_send_stream_line_("gap", json, plain);
}

void UAPBridge_esp::http_debug_emit_frame_(const char *frame_type, uint8_t *p_data, uint8_t from, uint8_t to,
                                           bool crc_valid) {
  this->append_persistent_log_frame_(frame_type, p_data, from, to, crc_valid);
  if (!this->http_debug_enabled_()) {
    return;
  }
  const uint32_t seq = ++this->http_debug_frame_sequence_;
  std::string hex = this->http_debug_hex_encode_(p_data + from, to > from ? to - from : 0);

  std::string plain = "FRAME ";
  plain += std::to_string(seq);
  plain += " ";
  plain += std::to_string(millis());
  plain += " ";
  plain += frame_type;
  plain += " crc=";
  plain += crc_valid ? "ok" : "bad";
  plain += " ";
  plain += hex;
  this->http_debug_append_recent_(plain);

  std::string json = "{\"seq\":";
  json += std::to_string(seq);
  json += ",\"millis\":";
  json += std::to_string(millis());
  json += ",\"type\":\"";
  json += frame_type;
  json += "\",\"crc\":\"";
  json += crc_valid ? "ok" : "bad";
  json += "\",\"hex\":\"";
  json += hex;
  json += "\"}";
  this->http_debug_send_stream_line_("frame", json, plain);
}

void UAPBridge_esp::http_debug_emit_command_(const char *phase, hoermann_action_t command, const char *reason) {
  this->append_persistent_log_command_(phase, command, reason);
  if (!this->http_debug_enabled_()) {
    return;
  }

  const uint32_t seq = ++this->http_debug_cmd_sequence_;
  char word[8];
  snprintf(word, sizeof(word), "0x%04X", (unsigned int) command);

  std::string plain = "CMD ";
  plain += std::to_string(seq);
  plain += " ";
  plain += std::to_string(millis());
  plain += " ";
  plain += phase;
  plain += " ";
  plain += this->action_name(command);
  plain += " ";
  plain += word;
  if (reason != nullptr) {
    plain += " reason=";
    plain += reason;
  }
  this->http_debug_append_recent_(plain);

  std::string json = "{\"seq\":";
  json += std::to_string(seq);
  json += ",\"millis\":";
  json += std::to_string(millis());
  json += ",\"phase\":\"";
  json += phase;
  json += "\",\"action\":\"";
  json += this->action_name(command);
  json += "\",\"word\":\"";
  json += word;
  json += "\",\"raw_status\":";
  json += std::to_string(this->broadcast_status);
  json += ",\"state\":\"";
  json += this->state_string;
  json += "\",\"valid_broadcast\":";
  json += this->valid_broadcast ? "true" : "false";
  if (reason != nullptr) {
    json += ",\"reason\":\"";
    json += reason;
    json += "\"";
  }
  json += "}";
  this->http_debug_send_stream_line_("cmd", json, plain);
}

void UAPBridge_esp::http_debug_emit_status_transition_(uint16_t status, const char *frame_type) {
  this->append_persistent_log_status_(status, frame_type);
  if (!this->http_debug_enabled_()) {
    return;
  }

  const uint32_t seq = ++this->broadcast_status_transition_sequence_;
  char status_word[8];
  snprintf(status_word, sizeof(status_word), "0x%04X", (unsigned int) status);

  std::string plain = "STATUS ";
  plain += std::to_string(seq);
  plain += " ";
  plain += std::to_string(millis());
  plain += " ";
  plain += frame_type != nullptr ? frame_type : "broadcast";
  plain += " ";
  plain += status_word;
  plain += " state=";
  plain += this->state_string;
  this->http_debug_append_recent_(plain);

  std::string json = "{\"seq\":";
  json += std::to_string(seq);
  json += ",\"millis\":";
  json += std::to_string(millis());
  json += ",\"frame_type\":\"";
  json += frame_type != nullptr ? frame_type : "broadcast";
  json += "\",\"status\":";
  json += std::to_string(status);
  json += ",\"hex\":\"";
  json += status_word;
  json += "\",\"state\":\"";
  json += this->state_string;
  json += "\",\"bits\":";
  json += this->http_debug_status_bits_json_(status);
  json += "}";
  this->http_debug_send_stream_line_("status", json, plain);
}

void UAPBridge_esp::http_debug_service_keepalive_() {
  if (this->http_debug_stream_client_ == nullptr ||
      millis() - this->http_debug_last_keepalive_ms_ < UAPBRIDGE_HTTP_KEEPALIVE_MS) {
    return;
  }
  this->http_debug_last_keepalive_ms_ = millis();
  if (this->http_debug_stream_is_sse_) {
    std::string out = ": keepalive\n\n";
    ssize_t written = this->http_debug_stream_client_->write(out.data(), out.size());
    if (written < 0 || (size_t) written != out.size()) {
      this->http_debug_disconnect_stream_client_();
    }
  } else {
    this->http_debug_send_stream_line_("keepalive", "{}", "# keepalive");
  }
}

void UAPBridge_esp::http_debug_disconnect_stream_client_() { this->http_debug_stream_client_.reset(); }

void UAPBridge_esp::http_debug_append_recent_(const std::string &line) {
  if (this->http_debug_history_size_ == 0) {
    return;
  }
  this->http_debug_recent_lines_.push_back(line);
  while (this->http_debug_recent_lines_.size() > this->http_debug_history_size_) {
    this->http_debug_recent_lines_.pop_front();
  }
}

std::string UAPBridge_esp::http_debug_recent_body_() {
  std::string out;
  for (const auto &line : this->http_debug_recent_lines_) {
    out += line;
    out += "\n";
  }
  return out;
}

std::string UAPBridge_esp::http_debug_stats_json_() {
  std::string json = "{\"component\":\"uapbridge_esp\",\"baud\":19200,\"mode\":\"uap1-emulation\",\"rx_sequence\":";
  json += std::to_string(this->http_debug_rx_sequence_);
  json += ",\"rx_bytes\":";
  json += std::to_string(this->http_debug_rx_bytes_);
  json += ",\"tx_sequence\":";
  json += std::to_string(this->http_debug_tx_sequence_);
  json += ",\"tx_bytes\":";
  json += std::to_string(this->http_debug_tx_bytes_);
  json += ",\"frame_sequence\":";
  json += std::to_string(this->http_debug_frame_sequence_);
  json += ",\"gap_sequence\":";
  json += std::to_string(this->http_debug_gap_sequence_);
  json += ",\"cmd_sequence\":";
  json += std::to_string(this->http_debug_cmd_sequence_);
  json += ",\"last_rx_us\":";
  json += std::to_string(this->http_debug_last_rx_us_);
  json += ",\"valid_broadcast\":";
  json += this->valid_broadcast ? "true" : "false";
  json += ",\"obstruction\":";
  json += this->obstruction_state ? "true" : "false";
  json += ",\"raw_status\":";
  json += std::to_string(this->broadcast_status);
  json += ",\"command_sequence\":";
  json += std::to_string(this->command_sequence);
  json += ",\"queued_action\":\"";
  json += this->action_name(this->next_action);
  json += "\",\"state\":\"";
  json += this->state_string;
  json += "\"";
  json += ",\"stream_connected\":";
  json += this->http_debug_stream_client_ == nullptr ? "false" : "true";
  json += ",\"history_size\":";
  json += std::to_string(this->http_debug_history_size_);
  json += ",\"broadcast_status_record_count\":";
  json += std::to_string(this->broadcast_status_record_count_());
  json += ",\"broadcast_status_overflow_count\":";
  json += std::to_string(this->broadcast_status_overflow_count_);
  json += ",\"status_transition_sequence\":";
  json += std::to_string(this->broadcast_status_transition_sequence_);
  json += "}\n";
  return json;
}

std::string UAPBridge_esp::http_debug_broadcast_status_json_() {
  std::string json = "{\"raw_status\":";
  json += std::to_string(this->broadcast_status);
  json += ",\"state\":\"";
  json += this->state_string;
  json += "\",\"valid_broadcast\":";
  json += this->valid_broadcast ? "true" : "false";
  json += ",\"transition_sequence\":";
  json += std::to_string(this->broadcast_status_transition_sequence_);
  json += ",\"overflow_count\":";
  json += std::to_string(this->broadcast_status_overflow_count_);
  json += ",\"records\":[";
  bool first = true;
  for (const auto &record : this->broadcast_status_records_) {
    if (!record.used) {
      continue;
    }
    if (!first) {
      json += ",";
    }
    first = false;
    char status_word[8];
    snprintf(status_word, sizeof(status_word), "0x%04X", (unsigned int) record.status);
    json += "{\"status\":";
    json += std::to_string(record.status);
    json += ",\"hex\":\"";
    json += status_word;
    json += "\",\"count\":";
    json += std::to_string(record.count);
    json += ",\"first_ms\":";
    json += std::to_string(record.first_ms);
    json += ",\"last_ms\":";
    json += std::to_string(record.last_ms);
    json += ",\"bits\":";
    json += this->http_debug_status_bits_json_(record.status);
    json += "}";
  }
  json += "]}\n";
  return json;
}

std::string UAPBridge_esp::http_debug_persistent_log_summary_json_() {
  this->persistent_log_refresh_fs_info_();
  std::string json = "{\"enabled\":";
  json += this->persistent_log_enabled_ ? "true" : "false";
  json += ",\"ready\":";
  json += this->persistent_log_ready_ ? "true" : "false";
  json += ",\"format_required\":";
  json += this->persistent_log_format_required_ ? "true" : "false";
  json += ",\"storage\":\"spiffs\"";
  json += ",\"mount\":\"";
  json += UAPBRIDGE_PERSISTENT_LOG_MOUNT;
  json += "\",\"file\":\"";
  json += UAPBRIDGE_PERSISTENT_LOG_FILE;
  json += "\",\"compression\":\"binary_records_ram_staged\"";
  json += ",\"format_version\":";
  json += std::to_string(UAPBRIDGE_PERSISTENT_LOG_VERSION);
  json += ",\"filesystem_total\":";
  json += std::to_string(this->persistent_log_fs_total_);
  json += ",\"filesystem_used\":";
  json += std::to_string(this->persistent_log_fs_used_);
  json += ",\"max_file_bytes\":";
  json += std::to_string(UAPBRIDGE_PERSISTENT_LOG_MAX_FILE_BYTES);
  json += ",\"file_bytes\":";
  json += std::to_string(this->persistent_log_file_bytes_);
  json += ",\"ram_used\":";
  json += std::to_string(this->persistent_log_ram_used_);
  json += ",\"ram_capacity\":";
  json += std::to_string(UAPBRIDGE_PERSISTENT_LOG_RAM_CAPACITY);
  json += ",\"dropped_records\":";
  json += std::to_string(this->persistent_log_dropped_records_);
  json += ",\"dropped_bytes\":";
  json += std::to_string(this->persistent_log_dropped_bytes_);
  json += ",\"next_seq\":";
  json += std::to_string(this->persistent_log_next_seq_);
  json += "}\n";
  return json;
}

std::string UAPBridge_esp::http_debug_persistent_log_record_json_(const uint8_t *record, size_t total_len) {
  if (record == nullptr || total_len < 1 + UAPBRIDGE_PLOG_FIXED_PAYLOAD_LEN ||
      total_len > UAPBRIDGE_PLOG_MAX_RECORD_TOTAL_LEN || record[0] != total_len) {
    return "{\"decode_error\":\"invalid persistent log record\"}";
  }

  const uint8_t *payload = record + 1;
  const uint8_t type = payload[0];
  const uint8_t flags = payload[1];
  const uint8_t source = payload[2];
  const uint8_t phase = payload[3];
  const uint8_t reason = payload[4];
  const uint8_t data_len = payload[5];
  if (data_len + 1 + UAPBRIDGE_PLOG_FIXED_PAYLOAD_LEN != total_len) {
    return "{\"decode_error\":\"invalid persistent log data length\"}";
  }
  const uint32_t seq = plog_read_u32(payload + 6);
  const uint32_t first_ms = plog_read_u32(payload + 10);
  const uint32_t last_ms = plog_read_u32(payload + 14);
  const uint32_t first_us = plog_read_u32(payload + 18);
  const uint32_t last_us = plog_read_u32(payload + 22);
  const uint16_t status = plog_read_u16(payload + 26);
  const uint16_t action = plog_read_u16(payload + 28);
  const uint16_t state = plog_read_u16(payload + 30);
  const uint16_t repeat = plog_read_u16(payload + 32);

  char status_word[8];
  char action_word[8];
  snprintf(status_word, sizeof(status_word), "0x%04X", (unsigned int) status);
  snprintf(action_word, sizeof(action_word), "0x%04X", (unsigned int) action);
  const char *action_name = action == 0 ? "none" : this->action_name((hoermann_action_t) action);

  std::string json = "{\"seq\":";
  json += std::to_string(seq);
  json += ",\"type\":\"";
  json += this->persistent_log_type_name_(type);
  json += "\",\"source\":\"";
  json += this->persistent_log_source_name_(source);
  json += "\",\"phase\":\"";
  json += this->persistent_log_phase_name_(phase);
  json += "\",\"reason\":\"";
  json += this->persistent_log_reason_name_(reason);
  json += "\",\"flags\":";
  json += std::to_string(flags);
  json += ",\"crc\":\"";
  json += (flags & UAPBRIDGE_PLOG_FLAG_CRC_OK) ? "ok" : "unknown_or_bad";
  json += "\",\"first_ms\":";
  json += std::to_string(first_ms);
  json += ",\"last_ms\":";
  json += std::to_string(last_ms);
  json += ",\"first_micros\":";
  json += std::to_string(first_us);
  json += ",\"last_micros\":";
  json += std::to_string(last_us);
  json += ",\"repeat\":";
  json += std::to_string(repeat == 0 ? 1 : repeat);
  json += ",\"status\":";
  json += std::to_string(status);
  json += ",\"status_hex\":\"";
  json += status_word;
  json += "\",\"action\":\"";
  json += action_name;
  json += "\",\"action_hex\":\"";
  json += action_word;
  json += "\",\"state\":";
  json += std::to_string(state);
  json += ",\"bits\":";
  json += this->http_debug_status_bits_json_(status);
  json += ",\"hex\":\"";
  json += this->http_debug_hex_encode_(payload + UAPBRIDGE_PLOG_FIXED_PAYLOAD_LEN, data_len);
  json += "\"}";
  return json;
}

void UAPBridge_esp::http_debug_send_persistent_log_response_(std::unique_ptr<socket::Socket> client) {
  watchdog::WatchdogManager watchdog(60000);
  std::string header =
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-cache\r\nConnection: close\r\n"
      "Access-Control-Allow-Origin: *\r\n\r\n";
  if (!this->http_debug_write_all_(client.get(), header, 1000)) {
    return;
  }

  std::string prefix = this->http_debug_persistent_log_summary_json_();
  if (!prefix.empty() && prefix.back() == '\n') {
    prefix.pop_back();
  }
  if (!prefix.empty() && prefix.back() == '}') {
    prefix.pop_back();
  }
  prefix += ",\"records\":[";
  if (!this->http_debug_write_all_(client.get(), prefix, 1000)) {
    return;
  }

  bool first = true;
  std::string chunk;
  chunk.reserve(2048);
  auto flush_chunk = [&]() -> bool {
    if (chunk.empty()) {
      return true;
    }
    const bool ok = this->http_debug_write_all_(client.get(), chunk, 1000);
    chunk.clear();
    return ok;
  };
  FILE *file = fopen(UAPBRIDGE_PERSISTENT_LOG_FILE, "rb");
  if (file != nullptr) {
    while (true) {
      uint8_t total_len = 0;
      if (fread(&total_len, 1, 1, file) != 1) {
        break;
      }
      uint8_t record[UAPBRIDGE_PLOG_MAX_RECORD_TOTAL_LEN]{};
      record[0] = total_len;
      if (total_len == 0 || total_len > UAPBRIDGE_PLOG_MAX_RECORD_TOTAL_LEN ||
          fread(record + 1, 1, total_len - 1, file) != total_len - 1) {
        if (!flush_chunk()) {
          fclose(file);
          return;
        }
        std::string error = first ? "" : ",";
        error += "{\"decode_error\":\"truncated persistent log file\"}";
        this->http_debug_write_all_(client.get(), error, 1000);
        break;
      }
      std::string item = first ? "" : ",";
      item += this->http_debug_persistent_log_record_json_(record, total_len);
      if (chunk.size() + item.size() > 2048 && !flush_chunk()) {
        fclose(file);
        return;
      }
      chunk += item;
      first = false;
    }
    fclose(file);
  }
  if (!flush_chunk()) {
    return;
  }

  std::string suffix = "]}\n";
  this->http_debug_write_all_(client.get(), suffix, 1000);
}

void UAPBridge_esp::http_debug_send_persistent_log_binary_response_(std::unique_ptr<socket::Socket> client) {
  watchdog::WatchdogManager watchdog(60000);
  this->persistent_log_refresh_fs_info_();
  std::string header =
      "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n"
      "Access-Control-Allow-Origin: *\r\nContent-Length: ";
  header += std::to_string(this->persistent_log_file_bytes_);
  header += "\r\n\r\n";
  if (!this->http_debug_write_all_(client.get(), header, 1000)) {
    return;
  }

  FILE *file = fopen(UAPBRIDGE_PERSISTENT_LOG_FILE, "rb");
  if (file == nullptr) {
    return;
  }
  char buffer[1024];
  while (true) {
    const size_t len = fread(buffer, 1, sizeof(buffer), file);
    if (len == 0) {
      break;
    }
    if (!this->http_debug_write_all_(client.get(), std::string(buffer, len), 1000)) {
      fclose(file);
      return;
    }
  }
  fclose(file);
}

std::string UAPBridge_esp::http_debug_status_bits_json_(uint16_t status) {
  std::string json = "{\"open\":";
  json += (status & hoermann_state_open) != 0 ? "true" : "false";
  json += ",\"closed\":";
  json += (status & hoermann_state_closed) != 0 ? "true" : "false";
  json += ",\"relay\":";
  json += (status & hoermann_state_opt_relay) != 0 ? "true" : "false";
  json += ",\"light\":";
  json += (status & hoermann_state_light_relay) != 0 ? "true" : "false";
  json += ",\"error\":";
  json += (status & hoermann_state_error) != 0 ? "true" : "false";
  json += ",\"direction_closing\":";
  json += (status & hoermann_state_direction) != 0 ? "true" : "false";
  json += ",\"moving\":";
  json += (status & hoermann_state_moving) != 0 ? "true" : "false";
  json += ",\"venting\":";
  json += (status & hoermann_state_venting) != 0 ? "true" : "false";
  json += ",\"prewarn\":";
  json += (status & hoermann_state_prewarn) != 0 ? "true" : "false";
  json += "}";
  return json;
}

std::string UAPBridge_esp::http_debug_hex_encode_(const uint8_t *data, size_t len) {
  static const char *const hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out.push_back(hex[(data[i] >> 4) & 0x0F]);
    out.push_back(hex[data[i] & 0x0F]);
  }
  return out;
}

std::string UAPBridge_esp::http_debug_path_from_request_line_(const std::string &request_line) {
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

void UAPBridge_esp::record_broadcast_status_(uint16_t status, const char *frame_type) {
  const uint32_t now = millis();
  for (auto &record : this->broadcast_status_records_) {
    if (record.used && record.status == status) {
      record.count++;
      record.last_ms = now;
      if (status != this->previous_recorded_broadcast_status_) {
        this->http_debug_emit_status_transition_(status, frame_type);
        this->previous_recorded_broadcast_status_ = status;
      }
      return;
    }
  }

  for (auto &record : this->broadcast_status_records_) {
    if (!record.used) {
      record.used = true;
      record.status = status;
      record.count = 1;
      record.first_ms = now;
      record.last_ms = now;
      if (status != this->previous_recorded_broadcast_status_) {
        this->http_debug_emit_status_transition_(status, frame_type);
        this->previous_recorded_broadcast_status_ = status;
      }
      return;
    }
  }

  this->broadcast_status_overflow_count_++;
  if (status != this->previous_recorded_broadcast_status_) {
    this->http_debug_emit_status_transition_(status, frame_type);
    this->previous_recorded_broadcast_status_ = status;
  }
}

uint8_t UAPBridge_esp::broadcast_status_record_count_() const {
  uint8_t count = 0;
  for (const auto &record : this->broadcast_status_records_) {
    if (record.used) {
      count++;
    }
  }
  return count;
}

bool UAPBridge_esp::setup_persistent_log_(bool format_if_mount_failed) {
  if (this->persistent_log_fs_mounted_) {
    this->persistent_log_refresh_fs_info_();
    return true;
  }

  esp_vfs_spiffs_conf_t conf{};
  conf.base_path = UAPBRIDGE_PERSISTENT_LOG_MOUNT;
  conf.partition_label = UAPBRIDGE_PERSISTENT_LOG_PARTITION;
  conf.max_files = 2;
  conf.format_if_mount_failed = format_if_mount_failed;

  esp_err_t err = esp_vfs_spiffs_register(&conf);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "Persistent protocol log filesystem mount failed: %s", esp_err_to_name(err));
    this->persistent_log_ready_ = false;
    this->persistent_log_fs_mounted_ = false;
    this->persistent_log_format_required_ = true;
    this->reset_persistent_log_store_();
    return false;
  }

  this->persistent_log_ready_ = true;
  this->persistent_log_fs_mounted_ = true;
  this->persistent_log_format_required_ = false;
  this->reset_persistent_log_store_();
  this->persistent_log_file_bytes_ = this->persistent_log_file_size_();
  this->persistent_log_refresh_fs_info_();
  ESP_LOGI(TAG, "Persistent protocol log filesystem mounted: total=%u used=%u file=%u",
           (unsigned int) this->persistent_log_fs_total_, (unsigned int) this->persistent_log_fs_used_,
           (unsigned int) this->persistent_log_file_bytes_);
  return true;
}

bool UAPBridge_esp::format_persistent_log_() {
  this->persistent_log_enabled_ = false;
  this->reset_persistent_log_store_();
  if (this->persistent_log_fs_mounted_) {
    esp_vfs_spiffs_unregister(UAPBRIDGE_PERSISTENT_LOG_PARTITION);
    this->persistent_log_fs_mounted_ = false;
    this->persistent_log_ready_ = false;
  }

  ESP_LOGW(TAG, "Formatting persistent protocol log filesystem; do not remove power");
  watchdog::WatchdogManager watchdog(60000);
  const bool mounted = this->setup_persistent_log_(true);
  if (mounted) {
    this->clear_persistent_log_();
  }
  return mounted;
}

void UAPBridge_esp::reset_persistent_log_store_() {
  this->persistent_log_next_seq_ = 1;
  this->persistent_log_ram_used_ = 0;
  this->persistent_log_file_bytes_ = 0;
  this->persistent_log_dropped_records_ = 0;
  this->persistent_log_dropped_bytes_ = 0;
  this->persistent_log_dirty_ = false;
  this->persistent_log_unsaved_records_ = 0;
  this->persistent_log_last_record_pos_ = 0xFFFF;
  this->persistent_log_last_record_len_ = 0;
}

void UAPBridge_esp::clear_persistent_log_() {
  this->reset_persistent_log_store_();
  if (this->persistent_log_fs_mounted_) {
    FILE *file = fopen(UAPBRIDGE_PERSISTENT_LOG_FILE, "wb");
    if (file != nullptr) {
      fclose(file);
    } else {
      remove(UAPBRIDGE_PERSISTENT_LOG_FILE);
    }
    this->persistent_log_refresh_fs_info_();
  }
}

void UAPBridge_esp::append_persistent_log_status_(uint16_t status, const char *frame_type) {
  this->append_persistent_log_record_(UAPBRIDGE_PLOG_TYPE_STATUS, this->persistent_log_source_code_(frame_type),
                                      0, 0, 0, status, nullptr, 0, 0, micros());
}

void UAPBridge_esp::append_persistent_log_command_(const char *phase, hoermann_action_t command, const char *reason) {
  this->append_persistent_log_record_(UAPBRIDGE_PLOG_TYPE_CMD, 0, this->persistent_log_phase_code_(phase),
                                      this->persistent_log_reason_code_(reason), (uint16_t) command,
                                      this->broadcast_status, nullptr, 0, 0, micros());
}

void UAPBridge_esp::append_persistent_log_frame_(const char *frame_type, uint8_t *p_data, uint8_t from, uint8_t to,
                                                 bool crc_valid) {
  const uint8_t len = to > from ? to - from : 0;
  const uint8_t stored_len = len > UAPBRIDGE_PERSISTENT_LOG_DATA_LEN ? UAPBRIDGE_PERSISTENT_LOG_DATA_LEN : len;
  this->append_persistent_log_record_(UAPBRIDGE_PLOG_TYPE_FRAME, this->persistent_log_source_code_(frame_type),
                                      0, 0, 0, this->broadcast_status, p_data + from, stored_len,
                                      crc_valid ? UAPBRIDGE_PLOG_FLAG_CRC_OK : 0, micros());
}

void UAPBridge_esp::append_persistent_log_raw_(uint8_t type, const uint8_t *data, size_t len, uint32_t timestamp_us,
                                               uint8_t source, uint8_t flags) {
  while (len > 0) {
    const uint8_t chunk = len > UAPBRIDGE_PERSISTENT_LOG_DATA_LEN ? UAPBRIDGE_PERSISTENT_LOG_DATA_LEN : len;
    this->append_persistent_log_record_(type, source, 0, 0, 0, this->broadcast_status, data, chunk, flags, timestamp_us);
    data += chunk;
    len -= chunk;
  }
}

void UAPBridge_esp::append_persistent_log_gap_(uint32_t timestamp_us, uint32_t gap_us) {
  uint8_t data[4];
  plog_write_u32(data, gap_us);
  this->append_persistent_log_record_(UAPBRIDGE_PLOG_TYPE_GAP, 0, 0, 0, 0, this->broadcast_status, data,
                                      sizeof(data), 0, timestamp_us);
}

void UAPBridge_esp::append_persistent_log_record_(uint8_t type, uint8_t source, uint8_t phase, uint8_t reason,
                                                  uint16_t action, uint16_t status, const uint8_t *data,
                                                  uint8_t len, uint8_t flags, uint32_t timestamp_us) {
  if (!this->persistent_log_enabled_ && type != UAPBRIDGE_PLOG_TYPE_CONTROL) {
    return;
  }
  if (!this->persistent_log_ready_ || !this->persistent_log_fs_mounted_) {
    this->persistent_log_dropped_records_++;
    this->persistent_log_dropped_bytes_ += len;
    return;
  }

  if (len > UAPBRIDGE_PERSISTENT_LOG_DATA_LEN) {
    len = UAPBRIDGE_PERSISTENT_LOG_DATA_LEN;
  }
  const uint8_t payload_len = UAPBRIDGE_PLOG_FIXED_PAYLOAD_LEN + len;
  const uint8_t total_len = 1 + payload_len;
  const uint32_t now_ms = millis();
  const uint32_t event_us = timestamp_us == 0 ? micros() : timestamp_us;
  const uint16_t state_value = (uint16_t) this->state;

  if (this->persistent_log_last_record_pos_ != 0xFFFF &&
      this->persistent_log_last_record_len_ == total_len &&
      this->persistent_log_last_record_pos_ + total_len <= this->persistent_log_ram_used_ &&
      this->persistent_log_ram_[this->persistent_log_last_record_pos_] == total_len) {
    uint8_t *previous = this->persistent_log_ram_ + this->persistent_log_last_record_pos_;
    uint8_t *payload = previous + 1;
    bool same = payload[0] == type && payload[1] == flags && payload[2] == source && payload[3] == phase &&
                payload[4] == reason && payload[5] == len && plog_read_u16(payload + 26) == status &&
                plog_read_u16(payload + 28) == action && plog_read_u16(payload + 30) == state_value;
    if (same && len > 0 && data != nullptr) {
      same = memcmp(payload + UAPBRIDGE_PLOG_FIXED_PAYLOAD_LEN, data, len) == 0;
    }
    if (same) {
      const uint16_t repeat = plog_read_u16(payload + 32);
      if (repeat < 0xFFFF) {
        plog_write_u32(payload + 14, now_ms);
        plog_write_u32(payload + 22, event_us);
        plog_write_u16(payload + 32, repeat + 1);
        this->mark_persistent_log_dirty_();
        return;
      }
    }
  }

  if (this->persistent_log_ram_used_ + total_len > UAPBRIDGE_PERSISTENT_LOG_RAM_CAPACITY) {
    this->save_persistent_log_(true);
  }
  if (this->persistent_log_ram_used_ + total_len > UAPBRIDGE_PERSISTENT_LOG_RAM_CAPACITY ||
      this->persistent_log_file_bytes_ + this->persistent_log_ram_used_ + total_len >
          UAPBRIDGE_PERSISTENT_LOG_MAX_FILE_BYTES) {
    this->persistent_log_dropped_records_++;
    this->persistent_log_dropped_bytes_ += total_len;
    return;
  }

  uint8_t record[UAPBRIDGE_PLOG_MAX_RECORD_TOTAL_LEN]{};
  record[0] = total_len;
  uint8_t *payload = record + 1;
  payload[0] = type;
  payload[1] = flags;
  payload[2] = source;
  payload[3] = phase;
  payload[4] = reason;
  payload[5] = len;
  plog_write_u32(payload + 6, this->persistent_log_next_seq_++);
  plog_write_u32(payload + 10, now_ms);
  plog_write_u32(payload + 14, now_ms);
  plog_write_u32(payload + 18, event_us);
  plog_write_u32(payload + 22, event_us);
  plog_write_u16(payload + 26, status);
  plog_write_u16(payload + 28, action);
  plog_write_u16(payload + 30, state_value);
  plog_write_u16(payload + 32, 1);
  if (len > 0 && data != nullptr) {
    memcpy(payload + UAPBRIDGE_PLOG_FIXED_PAYLOAD_LEN, data, len);
  }

  const uint16_t write_pos = this->persistent_log_ram_used_;
  memcpy(this->persistent_log_ram_ + write_pos, record, total_len);
  this->persistent_log_ram_used_ += total_len;
  this->persistent_log_last_record_pos_ = write_pos;
  this->persistent_log_last_record_len_ = total_len;
  this->mark_persistent_log_dirty_();
}

void UAPBridge_esp::service_persistent_log_save_() {
  if (!this->persistent_log_dirty_) {
    return;
  }
  const uint32_t now = millis();
  if (now - this->persistent_log_last_save_ms_ < UAPBRIDGE_PERSISTENT_LOG_SAVE_INTERVAL_MS) {
    return;
  }
  this->save_persistent_log_(true);
}

bool UAPBridge_esp::save_persistent_log_(bool force) {
  if (!force && !this->persistent_log_dirty_) {
    return true;
  }
  if (!this->persistent_log_fs_mounted_ || this->persistent_log_ram_used_ == 0) {
    this->persistent_log_dirty_ = false;
    this->persistent_log_unsaved_records_ = 0;
    this->persistent_log_last_save_ms_ = millis();
    return this->persistent_log_fs_mounted_;
  }
  if (!this->persistent_log_write_bytes_(this->persistent_log_ram_, this->persistent_log_ram_used_)) {
    return false;
  }
  this->persistent_log_ram_used_ = 0;
  this->persistent_log_last_record_pos_ = 0xFFFF;
  this->persistent_log_last_record_len_ = 0;
  this->persistent_log_dirty_ = false;
  this->persistent_log_unsaved_records_ = 0;
  this->persistent_log_last_save_ms_ = millis();
  this->persistent_log_refresh_fs_info_();
  return true;
}

bool UAPBridge_esp::persistent_log_write_bytes_(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0) {
    return true;
  }
  if (!this->persistent_log_fs_mounted_) {
    return false;
  }
  if (this->persistent_log_file_bytes_ + len > UAPBRIDGE_PERSISTENT_LOG_MAX_FILE_BYTES) {
    this->persistent_log_dropped_records_++;
    this->persistent_log_dropped_bytes_ += len;
    return false;
  }

  FILE *file = fopen(UAPBRIDGE_PERSISTENT_LOG_FILE, "ab");
  if (file == nullptr) {
    ESP_LOGW(TAG, "Failed to open persistent protocol log for append");
    return false;
  }
  const size_t written = fwrite(data, 1, len, file);
  fclose(file);
  if (written != len) {
    ESP_LOGW(TAG, "Short persistent protocol log write: %u/%u", (unsigned int) written, (unsigned int) len);
    this->persistent_log_dropped_bytes_ += len - written;
    return false;
  }
  this->persistent_log_file_bytes_ += len;
  return true;
}

bool UAPBridge_esp::persistent_log_refresh_fs_info_() {
  if (!this->persistent_log_fs_mounted_) {
    return false;
  }
  size_t total = 0;
  size_t used = 0;
  esp_err_t err = esp_spiffs_info(UAPBRIDGE_PERSISTENT_LOG_PARTITION, &total, &used);
  if (err != ESP_OK) {
    return false;
  }
  this->persistent_log_fs_total_ = total;
  this->persistent_log_fs_used_ = used;
  this->persistent_log_file_bytes_ = this->persistent_log_file_size_();
  return true;
}

uint32_t UAPBridge_esp::persistent_log_file_size_() {
  FILE *file = fopen(UAPBRIDGE_PERSISTENT_LOG_FILE, "rb");
  if (file == nullptr) {
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return this->persistent_log_file_bytes_;
  }
  const long size = ftell(file);
  fclose(file);
  return size > 0 ? (uint32_t) size : 0;
}

void UAPBridge_esp::mark_persistent_log_dirty_() {
  this->persistent_log_dirty_ = true;
  if (this->persistent_log_unsaved_records_ < 0xFF) {
    this->persistent_log_unsaved_records_++;
  }
}

uint8_t UAPBridge_esp::persistent_log_phase_code_(const char *phase) const {
  if (phase == nullptr) return 0;
  if (strcmp(phase, "start") == 0) return 1;
  if (strcmp(phase, "stop") == 0) return 2;
  if (strcmp(phase, "clear") == 0) return 3;
  if (strcmp(phase, "queued") == 0) return 10;
  if (strcmp(phase, "sent") == 0) return 11;
  if (strcmp(phase, "blocked") == 0) return 12;
  if (strcmp(phase, "expired") == 0) return 13;
  if (strcmp(phase, "dropped") == 0) return 14;
  if (strcmp(phase, "cancelled") == 0) return 15;
  if (strcmp(phase, "skipped") == 0) return 16;
  return 255;
}

uint8_t UAPBridge_esp::persistent_log_reason_code_(const char *reason) const {
  if (reason == nullptr) return 0;
  if (strcmp(reason, "listen_only") == 0) return 1;
  if (strcmp(reason, "close_disabled") == 0) return 2;
  if (strcmp(reason, "impulse_disabled") == 0) return 3;
  if (strcmp(reason, "raw_stop_disabled") == 0) return 4;
  if (strcmp(reason, "stale_broadcast") == 0) return 5;
  if (strcmp(reason, "unsafe_state") == 0) return 6;
  if (strcmp(reason, "error_or_prewarn") == 0) return 7;
  if (strcmp(reason, "previous_command_pending") == 0) return 8;
  if (strcmp(reason, "state_already_matches") == 0) return 9;
  if (strcmp(reason, "status_request_timeout") == 0) return 10;
  if (strcmp(reason, "bus_state_stale") == 0) return 11;
  if (strcmp(reason, "stop_before_fetch") == 0) return 12;
  if (strcmp(reason, "venting_from_open_close_disabled") == 0) return 13;
  if (strcmp(reason, "obstruction_active") == 0) return 14;
  return 255;
}

uint8_t UAPBridge_esp::persistent_log_source_code_(const char *source) const {
  if (source == nullptr) return 0;
  if (strcmp(source, "broadcast") == 0) return 10;
  if (strcmp(source, "broadcast-len1") == 0) return 11;
  if (strcmp(source, "scan") == 0) return 12;
  if (strcmp(source, "status-request") == 0) return 13;
  if (strcmp(source, "scan-invalid") == 0) return 20;
  if (strcmp(source, "broadcast-invalid") == 0) return 21;
  if (strcmp(source, "broadcast-len1-invalid") == 0) return 22;
  if (strcmp(source, "status-request-invalid") == 0) return 23;
  return 255;
}

const char *UAPBridge_esp::persistent_log_type_name_(uint8_t type) const {
  switch (type) {
    case UAPBRIDGE_PLOG_TYPE_RX: return "rx";
    case UAPBRIDGE_PLOG_TYPE_TX: return "tx";
    case UAPBRIDGE_PLOG_TYPE_GAP: return "gap";
    case UAPBRIDGE_PLOG_TYPE_FRAME: return "frame";
    case UAPBRIDGE_PLOG_TYPE_CMD: return "command";
    case UAPBRIDGE_PLOG_TYPE_STATUS: return "status";
    case UAPBRIDGE_PLOG_TYPE_CONTROL: return "control";
    default: return "unknown";
  }
}

const char *UAPBridge_esp::persistent_log_phase_name_(uint8_t phase) const {
  switch (phase) {
    case 0: return "none";
    case 1: return "start";
    case 2: return "stop";
    case 3: return "clear";
    case 10: return "queued";
    case 11: return "sent";
    case 12: return "blocked";
    case 13: return "expired";
    case 14: return "dropped";
    case 15: return "cancelled";
    case 16: return "skipped";
    case 255: return "other";
    default: return "unknown";
  }
}

const char *UAPBridge_esp::persistent_log_reason_name_(uint8_t reason) const {
  switch (reason) {
    case 0: return "none";
    case 1: return "listen_only";
    case 2: return "close_disabled";
    case 3: return "impulse_disabled";
    case 4: return "raw_stop_disabled";
    case 5: return "stale_broadcast";
    case 6: return "unsafe_state";
    case 7: return "error_or_prewarn";
    case 8: return "previous_command_pending";
    case 9: return "state_already_matches";
    case 10: return "status_request_timeout";
    case 11: return "bus_state_stale";
    case 12: return "stop_before_fetch";
    case 13: return "venting_from_open_close_disabled";
    case 14: return "obstruction_active";
    case 255: return "other";
    default: return "unknown";
  }
}

const char *UAPBridge_esp::persistent_log_source_name_(uint8_t source) const {
  switch (source) {
    case 0: return "raw";
    case 10: return "broadcast";
    case 11: return "broadcast-len1";
    case 12: return "scan";
    case 13: return "status-request";
    case 20: return "scan-invalid";
    case 21: return "broadcast-invalid";
    case 22: return "broadcast-len1-invalid";
    case 23: return "status-request-invalid";
    case 255: return "other";
    default: return "unknown";
  }
}

}  // namespace uapbridge_esp
}  // namespace esphome
