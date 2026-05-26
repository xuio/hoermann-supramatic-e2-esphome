#include "uapbridge_esp.h"

#include <cerrno>
#include <cstring>

namespace esphome {
namespace uapbridge_esp {
static const char *const TAG = "uapbridge_esp";

static const char UAPBRIDGE_HTTP_INDEX_HTML[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>UAP1 HCP Monitor</title>"
    "<style>body{font-family:system-ui,sans-serif;margin:24px;background:#101418;color:#e8edf2}"
    "code,pre{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}"
    "pre{white-space:pre-wrap;background:#05070a;border:1px solid #2a3440;padding:12px;min-height:70vh}"
    "a{color:#8cc8ff}</style></head><body>"
    "<h1>UAP1 HCP Monitor</h1><p><a href=\"/stats\">stats</a> <a href=\"/recent\">recent</a> "
    "<a href=\"/stream\">plain stream</a></p><pre id=\"log\"></pre>"
    "<script>const log=document.getElementById('log');function add(x){log.textContent+=x+'\\n';"
    "log.scrollTop=log.scrollHeight;}const es=new EventSource('/events');"
    "for(const t of ['hello','rx','tx','gap','frame'])es.addEventListener(t,e=>add(t.toUpperCase()+' '+e.data));"
    "es.onerror=()=>add('stream error or reconnecting');</script></body></html>";

void UAPBridge_esp::setup() {
  esphome::uapbridge::UAPBridge::setup();
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
}

void UAPBridge_esp::loop() {
  this->loop_fast();
  this->loop_slow();
  this->http_debug_accept_client_();
  this->http_debug_service_pending_client_();
  this->http_debug_service_keepalive_();

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
      if (this->http_debug_enabled_()) {
        const uint32_t now = micros();
        if (this->http_debug_send_gaps_ && this->http_debug_last_rx_us_ != 0) {
          const uint32_t gap = now - this->http_debug_last_rx_us_;
          if (gap > this->http_debug_gap_threshold_us_) {
            flush_debug_batch();
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

  if (this->http_debug_enabled_()) {
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
        this->apply_broadcast_status(new_status);
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
        this->apply_broadcast_status(new_status);
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
      return false;
    } else {
      this->next_action = command;
      this->command_set_at = millis();
      ESP_LOGI(TAG, "Queued one-shot HCP command: %s (0x%04X)", this->action_name(command), (unsigned int) command);
      return true;
    }
  } else {
    ESP_LOGD(TAG, "Skipped HCP command %s because requested state already matches decoded state", this->action_name(command));
    return false;
  }
}

bool UAPBridge_esp::command_allowed(const hoermann_action_t command, bool bypass_impulse_interlock) {
  if (this->listen_only) {
    ESP_LOGW(TAG, "Blocked HCP command %s because listen_only is true", this->action_name(command));
    return false;
  }

  if (command == hoermann_action_close && !this->allow_remote_close) {
    ESP_LOGW(TAG, "Blocked close command because allow_remote_close is false");
    return false;
  }
  if (command == hoermann_action_impulse && !this->allow_remote_impulse && !bypass_impulse_interlock) {
    ESP_LOGW(TAG, "Blocked impulse command because allow_remote_impulse is false");
    return false;
  }
  if (command == hoermann_action_stop && !this->use_unverified_stop_command) {
    ESP_LOGW(TAG, "Blocked raw stop command because use_unverified_stop_command is false");
    return false;
  }

  if (this->require_fresh_broadcast_for_commands && !this->bus_state_is_fresh()) {
    ESP_LOGW(TAG, "Blocked HCP command %s because no fresh valid HCP broadcast is available", this->action_name(command));
    return false;
  }

  const hoermann_state_t gate_state = this->decode_status_state(this->broadcast_status);
  const bool gate_error = (this->broadcast_status & hoermann_state_error) != 0;
  const bool gate_prewarn = (this->broadcast_status & hoermann_state_prewarn) != 0;

  if (command == hoermann_action_venting && !this->allow_remote_close && gate_state == hoermann_state_open) {
    ESP_LOGW(TAG, "Blocked venting command from open state because allow_remote_close is false");
    return false;
  }

  if (this->is_movement_command(command) && (gate_state == hoermann_state_unknown || gate_state == hoermann_state_stopped)) {
    ESP_LOGW(TAG, "Blocked movement HCP command %s because decoded door state is %s",
             this->action_name(command), this->state_string.c_str());
    return false;
  }

  if (this->is_movement_command(command) && (gate_error || gate_prewarn)) {
    ESP_LOGW(TAG, "Blocked movement HCP command %s because error/prewarn is active", this->action_name(command));
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

void UAPBridge_esp::apply_broadcast_status(uint16_t status) {
  const hoermann_state_t new_state = this->decode_status_state(status);
  if (new_state == hoermann_state_opening || new_state == hoermann_state_closing) {
    this->last_moving_broadcast_ms = millis();
  }
  if (new_state != this->state) {
    this->handle_state_change(new_state);
  }

  this->update_boolean_state("relay", this->relay_enabled, (status & hoermann_state_opt_relay) != 0);
  this->update_boolean_state("light", this->light_enabled, (status & hoermann_state_light_relay) != 0);
  this->update_boolean_state("vent", this->venting_enabled, (status & hoermann_state_venting) != 0);
  this->update_boolean_state("err", this->error_state, (status & hoermann_state_error) != 0);
  this->update_boolean_state("prewarn", this->prewarn_state, (status & hoermann_state_prewarn) != 0);
}

void UAPBridge_esp::expire_pending_command() {
  if (this->next_action == hoermann_action_none || this->command_timeout_ms == 0 || this->command_set_at == 0) {
    return;
  }

  if (millis() - this->command_set_at > this->command_timeout_ms) {
    ESP_LOGW(TAG, "Expired queued HCP command %s (0x%04X) after %ums without a status request",
             this->action_name(this->next_action), (unsigned int) this->next_action, (unsigned int) this->command_timeout_ms);
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
  json += ",\"last_rx_us\":";
  json += std::to_string(this->http_debug_last_rx_us_);
  json += ",\"valid_broadcast\":";
  json += this->valid_broadcast ? "true" : "false";
  json += ",\"raw_status\":";
  json += std::to_string(this->broadcast_status);
  json += ",\"command_sequence\":";
  json += std::to_string(this->command_sequence);
  json += ",\"stream_connected\":";
  json += this->http_debug_stream_client_ == nullptr ? "false" : "true";
  json += ",\"history_size\":";
  json += std::to_string(this->http_debug_history_size_);
  json += "}\n";
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

}  // namespace uapbridge_esp
}  // namespace esphome
