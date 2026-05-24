#include "uapbridge_esp.h"

namespace esphome {
namespace uapbridge_esp {
static const char *const TAG = "uapbridge_esp";

void UAPBridge_esp::loop() {
  this->loop_fast();
  this->loop_slow();

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

    if (this->ignore_next_event) {
      this->ignore_next_event = false;
    } else {
      ESP_LOGV(TAG, "loop_slow called - status=0x%04X", this->broadcast_status);
      if (this->diagnostic_mode) {
        this->log_decoded_status(this->broadcast_status);
      }
      hoermann_state_t new_state = hoermann_state_unknown;

      if (this->broadcast_status & hoermann_state_open) {
        new_state = hoermann_state_open;
      } else if (this->broadcast_status & hoermann_state_closed) {
        new_state = hoermann_state_closed;
      } else if ((this->broadcast_status & (hoermann_state_direction | hoermann_state_moving)) == hoermann_state_opening) {
        new_state = hoermann_state_opening;
      } else if ((this->broadcast_status & (hoermann_state_direction | hoermann_state_moving)) == hoermann_state_closing) {
        new_state = hoermann_state_closing;
      } else if (this->broadcast_status & hoermann_state_venting) {
        new_state = hoermann_state_venting;
      } else if (this->broadcast_status != 0) {
        new_state = hoermann_state_stopped;
      }

      if (new_state != this->state) {
        this->handle_state_change(new_state);
      }

      this->update_boolean_state("relay", this->relay_enabled, (this->broadcast_status & hoermann_state_opt_relay));
      this->update_boolean_state("light", this->light_enabled, (this->broadcast_status & hoermann_state_light_relay));
      this->update_boolean_state("vent", this->venting_enabled, (this->broadcast_status & hoermann_state_venting));
      this->update_boolean_state("err", this->error_state, (this->broadcast_status & hoermann_state_error));
      this->update_boolean_state("prewarn", this->prewarn_state, (this->broadcast_status & hoermann_state_prewarn));

      // --- Auto Error Correction ---
      const bool error_active = (this->broadcast_status & hoermann_state_error) == hoermann_state_error;
      if(this->auto_correction && error_active && !this->last_error_bit) {
        // if error just came up
        // if an error is detected and door is open/closed then try to reset it by requesting opening/closing without movement
        ESP_LOGD(TAG, "autocorrection started");
        if (new_state == hoermann_state_open) {
          this->set_command(true, hoermann_action_open);
        } else if (new_state == hoermann_state_closed) {
          this->set_command(true, hoermann_action_close);
        } else if (new_state == hoermann_state_stopped) {
          // in this state it is not possible to clear the error. But the next open or close cycle will clear it
          this->auto_correction_in_progress = false;
        }
        this->auto_correction_in_progress = true;
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
}

void UAPBridge_esp::receive() {
  uint8_t   length  = 0;
  uint8_t   counter = 0;
  bool   newData = false;
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
    if(this->read_byte(&this->rx_data[4])){
      //if read was successful
      this->byte_cnt++;
    }
    newData = true;
  }
  if (newData) {
    ESP_LOGVV(TAG, "new data received");
    newData = false;
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
        this->send_time = millis();
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
        this->broadcast_status = this->rx_data[2];
        this->broadcast_status |= (uint16_t)this->rx_data[3] << 8;
        this->last_valid_broadcast_ms = millis();
        if (!this->valid_broadcast) {
          this->valid_broadcast = true;
          this->data_has_changed = true;
        }
        if (this->diagnostic_mode) {
          this->log_decoded_status(this->broadcast_status);
        }
      } else if (this->diagnostic_mode) {
        this->update_diagnostic_string("broadcast-invalid", this->rx_data, 0, 5, crc_valid);
        ESP_LOGD(TAG, "Invalid broadcast candidate len=%u crc=%s data=%s", length, crc_valid ? "ok" : "bad", print_data(this->rx_data, 0, 5));
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
        }
        this->next_action = hoermann_action_none;
        this->command_set_at = 0;
        this->tx_data[5] = calc_crc8(this->tx_data, 5);
        this->tx_length = 6;
        this->send_time = millis();
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

  if (this->rts_pin_ != nullptr) {
    this->rts_pin_->digital_write(false);// LOW(false) = listen, HIGH(true) = transmit
  }

  ESP_LOGVV(TAG, "TX duration: %dms", millis() - this->send_time);
}
/**
 * Helper to set next Command and *not* skip Current Command before end was sent
 */
void UAPBridge_esp::set_command(bool cond, const hoermann_action_t command) {
  if (command == hoermann_action_close && !this->allow_remote_close) {
    ESP_LOGW(TAG, "Blocked close command because allow_remote_close is false");
    return;
  }
  if (cond) {
    if (this->next_action != hoermann_action_none) {
      ESP_LOGW(TAG, "Last command was not yet fetched by HCP master; keeping queued action %s (0x%04X), rejected %s",
               this->action_name(this->next_action), (unsigned int) this->next_action, this->action_name(command));
    } else {
      this->next_action = command;
      this->command_set_at = millis();
      this->ignore_next_event = true;
      ESP_LOGI(TAG, "Queued one-shot HCP command: %s (0x%04X)", this->action_name(command), (unsigned int) command);
    }
  } else {
    ESP_LOGD(TAG, "Skipped HCP command %s because requested state already matches decoded state", this->action_name(command));
  }
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
    this->handle_state_change(hoermann_state_unknown);
    this->data_has_changed = true;
    this->last_valid_broadcast_ms = 0;
  }
}

void UAPBridge_esp::action_open() {
  ESP_LOGD(TAG, "Action: open called");
  this->set_command(this->state != hoermann_state_open, hoermann_action_open);
}

void UAPBridge_esp::action_close() {
  ESP_LOGD(TAG, "Action: close called");
  this->set_command(this->state != hoermann_state_closed, hoermann_action_close);
}

void UAPBridge_esp::action_stop() {
  ESP_LOGD(TAG, "Action: stop called");
  this->set_command(true, hoermann_action_stop);
}

void UAPBridge_esp::action_venting() {
  ESP_LOGD(TAG, "Action: venting called");
  this->set_command(this->state != hoermann_state_venting, hoermann_action_venting);
}

void UAPBridge_esp::action_toggle_light() {
  ESP_LOGD(TAG, "Action: toggle light called");
  this->set_command(true, hoermann_action_toggle_light);
}

void UAPBridge_esp::action_impulse() {
  ESP_LOGD(TAG, "Action: impulse called");
  this->set_command(true, hoermann_action_impulse);
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

}  // namespace uapbridge_esp
}  // namespace esphome
