#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"


namespace esphome {
namespace uapbridge {

class UAPBridge : public uart::UARTDevice, public Component {
  public:
    // Enumeration for states
    enum hoermann_state_t {
      hoermann_state_stopped      = 0x0000,
      hoermann_state_open         = 0x0001,
      hoermann_state_closed       = 0x0002,
      hoermann_state_opt_relay    = 0x0004,
      hoermann_state_light_relay  = 0x0008,
      hoermann_state_error        = 0x0010,
      hoermann_state_direction    = 0x0020,
      hoermann_state_moving       = 0x0040,
      hoermann_state_opening      = 0x0040,
      hoermann_state_closing      = 0x0060,
      hoermann_state_venting      = 0x0080,
      hoermann_state_prewarn      = 0x0100,
      hoermann_state_unknown      = 0xFFFE,
      hoermann_state_unkown       = hoermann_state_unknown
    };

    void setup() override;
    void dump_config() override;
    void add_on_state_callback(std::function<void()> &&callback);
    void add_on_movement_command_callback(std::function<void()> &&callback);
    void set_rts_pin(InternalGPIOPin *rts_pin) { this->rts_pin_ = rts_pin; }
    void set_auto_correction(bool value) { this->auto_correction = value; }
    void set_allow_remote_close(bool value) { this->allow_remote_close = value; }
    bool get_allow_remote_close() const { return this->allow_remote_close; }
    void set_allow_remote_impulse(bool value) { this->allow_remote_impulse = value; }
    bool get_allow_remote_impulse() const { return this->allow_remote_impulse; }
    void set_use_unverified_stop_command(bool value) { this->use_unverified_stop_command = value; }
    bool get_use_unverified_stop_command() const { return this->use_unverified_stop_command; }
    void set_require_fresh_broadcast_for_commands(bool value) { this->require_fresh_broadcast_for_commands = value; }
    bool get_require_fresh_broadcast_for_commands() const { return this->require_fresh_broadcast_for_commands; }
    void set_command_timeout(uint32_t value) { this->command_timeout_ms = value; }
    uint32_t get_command_timeout() const { return this->command_timeout_ms; }
    void set_diagnostic_mode(bool value) { this->diagnostic_mode = value; }
    bool get_diagnostic_mode() const { return this->diagnostic_mode; }
    void set_listen_only(bool value) { this->listen_only = value; }
    bool get_listen_only() const { return this->listen_only; }
    void set_valid_broadcast_timeout(uint32_t value) { this->valid_broadcast_timeout_ms = value; }
    uint32_t get_valid_broadcast_timeout() const { return this->valid_broadcast_timeout_ms; }
    void set_trust_light_feedback(bool value) { this->trust_light_feedback = value; }
    bool get_trust_light_feedback() const { return this->trust_light_feedback; }

    virtual bool action_open() = 0;
    virtual bool action_close() = 0;
    virtual bool action_stop() = 0;
    virtual bool action_venting() = 0;
    virtual bool action_toggle_light() = 0;
    virtual bool action_impulse() = 0;
    virtual uint32_t get_command_sequence() const { return 0; }

    virtual hoermann_state_t get_state() = 0;
    virtual std::string get_state_string() = 0;
    virtual std::string get_diagnostic_string() { return ""; }
    virtual uint16_t get_raw_status() { return 0; }
    virtual void set_venting(bool state) = 0;
    bool get_venting_enabled() const { return this->venting_enabled; }
    virtual void set_light(bool state) = 0;
    bool get_light_enabled() const { return this->light_enabled; }
    bool get_relay_enabled() const { return this->relay_enabled; }
    void set_relay_enabled(bool value) { this->relay_enabled = value; }
    bool get_error_state() const { return this->error_state; }
    void set_error_state(bool value) { this->error_state = value; }
    bool get_prewarn_state() const { return this->prewarn_state; }
    void set_prewarn_state(bool value) { this->prewarn_state = value; }
    bool get_obstruction_state() const { return this->obstruction_state; }
    void set_obstruction_state(bool value) {
      if (this->obstruction_state == value) {
        return;
      }
      this->obstruction_state = value;
      this->data_has_changed = true;
    }
    bool get_pic16_com() const { return this->pic16_com; }
    void set_pic16_com(bool value) { this->pic16_com = value; }
    bool get_valid_broadcast() const { return this->valid_broadcast; }
    void set_valid_broadcast(bool value) { this->valid_broadcast = value; }
    bool has_data_changed() const { return this->data_has_changed; }
    void clear_data_changed_flag() { this->data_has_changed = false; }

  protected:
    // yaml parameters
    InternalGPIOPin *rts_pin_ = nullptr;
    bool auto_correction = false;
    bool allow_remote_close = false;
    bool allow_remote_impulse = false;
    bool use_unverified_stop_command = false;
    bool require_fresh_broadcast_for_commands = true;
    uint32_t command_timeout_ms = 1200;
    bool diagnostic_mode = false;
    bool listen_only = false;
    uint32_t valid_broadcast_timeout_ms = 10000;
    bool trust_light_feedback = true;
    // \yaml parameters
    CallbackManager<void()> state_callback_;
    CallbackManager<void()> movement_command_callback_;
    // state variables
    bool venting_enabled = false;
    bool light_enabled = false;
    bool relay_enabled = false;
    bool error_state = false;
    bool prewarn_state = false;
    bool obstruction_state = false;
    bool pic16_com = false;
    bool valid_broadcast = false;
    bool data_has_changed = false;
    // \state variables
};

}  // namespace uapbridge
}  // namespace esphome
