#pragma once

#include <cstdio>
#include <deque>
#include <memory>
#include <string>

#include "esphome/components/socket/socket.h"
#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/uapbridge/uapbridge.h"

#define CYCLE_TIME                1   // ms
#define CYCLE_TIME_SLOW         100   // ms
#define UAPBRIDGE_HTTP_KEEPALIVE_MS 5000
#define UAPBRIDGE_HTTP_PENDING_TIMEOUT_MS 3000

#define BROADCAST_ADDR            0x00
#define UAP1_ADDR                 0x28
#define UAP1_ADDR_MASTER          128

#define UAP1_TYPE                 0x14

#define CMD_SLAVE_SCAN            0x01
#define CMD_SLAVE_STATUS_REQUEST  0x20
#define CMD_SLAVE_STATUS_RESPONSE 0x29

#define STOP_FALLBACK_MOVING_TIMEOUT_MS 1000
#define UAPBRIDGE_STATUS_RECORD_COUNT 32
#define UAPBRIDGE_PERSISTENT_LOG_RAM_CAPACITY 16384
#define UAPBRIDGE_PERSISTENT_LOG_MAX_FILE_BYTES (3 * 1024 * 1024)
#define UAPBRIDGE_PERSISTENT_LOG_DATA_LEN 32
#define UAPBRIDGE_PERSISTENT_LOG_WRITE_CHUNK_BYTES 128
#define UAPBRIDGE_FRAME_SCAN_BUFFER_LEN 24

namespace esphome {
namespace uapbridge_esp {

class UAPBridge_esp : public esphome::uapbridge::UAPBridge {
  public:
    // Enumeration for actions
    enum hoermann_action_t {
      hoermann_action_stop          = 0x10FF, // thanks to https://github.com/avshrs/ESP32_Hormann_Supramatic_e3/blob/main/src/hoermann.h#L20 !
      hoermann_action_open          = 0x1001,
      hoermann_action_close         = 0x1002,
      hoermann_action_impulse       = 0x1004,
      hoermann_action_toggle_light  = 0x1008,
      hoermann_action_venting       = 0x1010,
    /*hoermann_action_test1         = 0x1020, //no reaction on my E3
      hoermann_action_test2         = 0x1040,
      hoermann_action_test3         = 0x1080,*/
      hoermann_action_none          = 0x1000
    };

    void setup() override;
    void loop() override;
    void dump_config() override;
    float get_setup_priority() const override;

    bool action_open() override;
    bool action_close() override;
    bool action_open_from_estimated_position() override;
    bool action_close_from_estimated_position() override;
    bool action_stop() override;
    bool action_venting() override;
    bool action_toggle_light() override;
    bool action_impulse() override;
    uint32_t get_command_sequence() const override { return this->command_sequence; }
    void set_http_debug_port(uint16_t port) { this->http_debug_port_ = port; }
    void set_http_debug_send_gaps(bool send_gaps) { this->http_debug_send_gaps_ = send_gaps; }
    void set_http_debug_gap_threshold_us(uint32_t gap_threshold_us) { this->http_debug_gap_threshold_us_ = gap_threshold_us; }
    void set_http_debug_history_size(uint16_t history_size) { this->http_debug_history_size_ = history_size; }
    void set_persistent_log_enabled(bool enabled) { this->persistent_log_enabled_ = enabled; }

    hoermann_state_t get_state();
    std::string get_state_string();
    void set_venting(bool state);
    void set_light(bool state);
    std::string get_diagnostic_string() override;
    uint16_t get_raw_status() override;
    uint32_t get_status_transition_count() const { return this->broadcast_status_transition_sequence_; }
    uint32_t get_unknown_valid_frame_count() const { return this->unknown_valid_frame_count_; }
    std::string get_last_valid_frame_hex() const { return this->last_valid_frame_hex_; }

  protected:
    hoermann_state_t state = hoermann_state_stopped;
    hoermann_action_t next_action = hoermann_action_none;
    // state variables
    std::string state_string = "unknown";
    // \state variables
    // Internal methods
    void loop_fast();
    void loop_slow();
    void receive();
    void process_rx_window();
    void reset_hcp_frame_scanner_();
    void observe_hcp_frame_candidate_(uint8_t byte, uint32_t timestamp_us);
    void record_hcp_frame_candidate_(const uint8_t *frame, uint8_t len);
    const char *classify_hcp_frame_(const uint8_t *frame, uint8_t len) const;
    bool is_hcp_rx_destination_(uint8_t address) const;
    void transmit();
    void transmit_prepared_response();
    bool set_command(bool cond, const hoermann_action_t command, bool bypass_impulse_interlock = false,
                     bool bypass_unsafe_state = false);
    bool command_allowed(const hoermann_action_t command, bool bypass_impulse_interlock = false,
                         bool bypass_unsafe_state = false);
    bool is_movement_command(const hoermann_action_t command);
    bool bus_state_is_fresh() const;
    bool moving_state_is_fresh() const;
    hoermann_state_t decode_status_state(uint16_t status) const;
    void apply_broadcast_status(uint16_t status, const char *frame_type = nullptr);
    void expire_pending_command();
    void expire_valid_broadcast();
    uint8_t calc_crc8(const uint8_t *p_data, uint8_t length);
    void handle_state_change(hoermann_state_t new_state);
    char* print_data(uint8_t *p_data, uint8_t from, uint8_t to);
    void update_boolean_state(const char * name, bool &current_state, bool new_state);
    const char *action_name(hoermann_action_t command);
    void update_diagnostic_string(const char *frame_type, uint8_t *p_data, uint8_t from, uint8_t to, bool crc_valid);
    void log_decoded_status(uint16_t status);
    bool http_debug_enabled_() const { return this->http_debug_port_ != 0; }
    void setup_http_debug_server_();
    void http_debug_accept_client_();
    void http_debug_service_pending_client_();
    void http_debug_service_keepalive_();
    void http_debug_disconnect_stream_client_();
    void http_debug_handle_request_(std::unique_ptr<socket::Socket> client, const std::string &request_line);
    void http_debug_start_stream_(std::unique_ptr<socket::Socket> client, bool sse);
    void http_debug_send_response_(std::unique_ptr<socket::Socket> client, const char *status, const char *content_type,
                                   const std::string &body);
    bool http_debug_write_all_(socket::Socket *client, const std::string &payload, uint32_t timeout_ms);
    void http_debug_send_stream_line_(const std::string &event_type, const std::string &json_data,
                                      const std::string &plain_line);
    void http_debug_emit_rx_(const uint8_t *data, size_t len, uint32_t timestamp_us);
    void http_debug_emit_tx_(const uint8_t *data, size_t len, uint32_t timestamp_us);
    void http_debug_emit_gap_(uint32_t timestamp_us, uint32_t gap_us);
    void http_debug_emit_frame_(const char *frame_type, uint8_t *p_data, uint8_t from, uint8_t to, bool crc_valid);
    void http_debug_emit_command_(const char *phase, hoermann_action_t command, const char *reason = nullptr);
    void http_debug_emit_status_transition_(uint16_t status, const char *frame_type);
    void http_debug_append_recent_(const std::string &line);
    std::string http_debug_recent_body_();
    std::string http_debug_stats_json_();
    std::string http_debug_broadcast_status_json_();
    std::string http_debug_persistent_log_summary_json_();
    void http_debug_send_persistent_log_response_(std::unique_ptr<socket::Socket> client);
    void http_debug_send_persistent_log_binary_response_(std::unique_ptr<socket::Socket> client);
    void service_bus_during_http_transfer_();
    std::string http_debug_status_bits_json_(uint16_t status);
    std::string http_debug_hex_encode_(const uint8_t *data, size_t len);
    std::string http_debug_path_from_request_line_(const std::string &request_line);
    void record_broadcast_status_(uint16_t status, const char *frame_type);
    uint8_t broadcast_status_record_count_() const;
    bool setup_persistent_log_(bool format_if_mount_failed = false);
    bool format_persistent_log_();
    void reset_persistent_log_store_();
    void clear_persistent_log_();
    void append_persistent_log_status_(uint16_t status, const char *frame_type);
    void append_persistent_log_command_(const char *phase, hoermann_action_t command, const char *reason);
    void append_persistent_log_frame_(const char *frame_type, uint8_t *p_data, uint8_t from, uint8_t to,
                                      bool crc_valid);
    void append_persistent_log_raw_(uint8_t type, const uint8_t *data, size_t len, uint32_t timestamp_us,
                                    uint8_t source = 0, uint8_t flags = 0);
    void append_persistent_log_gap_(uint32_t timestamp_us, uint32_t gap_us);
    void append_persistent_log_record_(uint8_t type, uint8_t source, uint8_t phase, uint8_t reason,
                                       uint16_t action, uint16_t status, const uint8_t *data = nullptr,
                                       uint8_t len = 0, uint8_t flags = 0, uint32_t timestamp_us = 0);
    void service_persistent_log_save_();
    bool save_persistent_log_(bool force = false);
    bool queue_persistent_log_flush_();
    bool write_persistent_log_chunk_();
    bool drain_persistent_log_flush_(uint32_t timeout_ms);
    bool open_persistent_log_file_();
    void close_persistent_log_file_();
    void mark_persistent_log_dirty_();
    bool persistent_log_write_bytes_(const uint8_t *data, size_t len);
    bool persistent_log_refresh_fs_info_();
    uint32_t persistent_log_file_size_();
    std::string http_debug_persistent_log_record_json_(const uint8_t *record, size_t total_len);
    uint8_t persistent_log_phase_code_(const char *phase) const;
    uint8_t persistent_log_reason_code_(const char *reason) const;
    uint8_t persistent_log_source_code_(const char *source) const;
    const char *persistent_log_type_name_(uint8_t type) const;
    const char *persistent_log_phase_name_(uint8_t phase) const;
    const char *persistent_log_reason_name_(uint8_t reason) const;
    const char *persistent_log_source_name_(uint8_t source) const;
    struct BroadcastStatusRecord {
      bool used{false};
      uint16_t status{0};
      uint32_t count{0};
      uint32_t first_ms{0};
      uint32_t last_ms{0};
    };
    // internal function variables
    uint32_t last_call       = 0;
    uint32_t last_call_slow   = 0;
    uint16_t broadcast_status = 0;
    uint16_t last_logged_status = 0xFFFF;
    uint16_t previous_recorded_broadcast_status_ = 0xFFFF;
    bool auto_correction_in_progress = false;
    bool last_error_bit = false;
    uint32_t command_set_at = 0;
    uint32_t command_sequence = 0;
    uint32_t last_valid_broadcast_ms = 0;
    uint32_t last_moving_broadcast_ms = 0;
    std::string diagnostic_string = "no HCP frame received";
    uint8_t    rx_data[5]       = {0, 0, 0, 0, 0};
    uint8_t    tx_data[6]       = {0, 0, 0, 0, 0, 0};
    uint8_t    tx_length        = 0;
    uint8_t    byte_cnt         = 0;
    uint32_t   send_time        = 0;
    uint16_t http_debug_port_{0};
    bool http_debug_send_gaps_{true};
    uint32_t http_debug_gap_threshold_us_{3000};
    uint16_t http_debug_history_size_{200};
    bool persistent_log_enabled_{false};
    bool persistent_log_ready_{false};
    bool persistent_log_fs_mounted_{false};
    bool persistent_log_format_required_{false};
    bool persistent_log_dirty_{false};
    uint8_t persistent_log_unsaved_records_{0};
    uint32_t persistent_log_last_save_ms_{0};
    uint16_t persistent_log_last_record_pos_{0xFFFF};
    uint8_t persistent_log_last_record_len_{0};
    uint32_t persistent_log_next_seq_{1};
    uint32_t persistent_log_file_bytes_{0};
    uint32_t persistent_log_dropped_records_{0};
    uint32_t persistent_log_dropped_bytes_{0};
    size_t persistent_log_fs_total_{0};
    size_t persistent_log_fs_used_{0};
    uint16_t persistent_log_ram_used_{0};
    uint16_t persistent_log_flush_len_{0};
    uint16_t persistent_log_flush_offset_{0};
    uint8_t persistent_log_ram_[UAPBRIDGE_PERSISTENT_LOG_RAM_CAPACITY]{};
    uint8_t persistent_log_flush_[UAPBRIDGE_PERSISTENT_LOG_RAM_CAPACITY]{};
    FILE *persistent_log_file_{nullptr};
    std::unique_ptr<socket::ListenSocket> http_debug_server_;
    std::unique_ptr<socket::Socket> http_debug_pending_client_;
    std::unique_ptr<socket::Socket> http_debug_stream_client_;
    bool http_debug_stream_is_sse_{true};
    char http_debug_request_buffer_[256]{};
    size_t http_debug_request_buffer_len_{0};
    uint32_t http_debug_pending_client_started_ms_{0};
    uint32_t http_debug_last_rx_us_{0};
    uint32_t http_debug_rx_sequence_{0};
    uint32_t http_debug_tx_sequence_{0};
    uint32_t http_debug_frame_sequence_{0};
    uint32_t http_debug_gap_sequence_{0};
    uint32_t http_debug_cmd_sequence_{0};
    uint32_t http_debug_rx_bytes_{0};
    uint32_t http_debug_tx_bytes_{0};
    uint32_t http_debug_last_keepalive_ms_{0};
    uint32_t broadcast_status_transition_sequence_{0};
    uint32_t broadcast_status_overflow_count_{0};
    uint32_t valid_frame_scan_sequence_{0};
    uint32_t unknown_valid_frame_count_{0};
    std::string last_valid_frame_hex_{"none"};
    uint8_t frame_scan_buffer_[UAPBRIDGE_FRAME_SCAN_BUFFER_LEN]{};
    uint8_t frame_scan_len_{0};
    uint32_t frame_scan_last_rx_us_{0};
    BroadcastStatusRecord broadcast_status_records_[UAPBRIDGE_STATUS_RECORD_COUNT]{};
    std::deque<std::string> http_debug_recent_lines_;
    const uint8_t crc_table[256] = {
      0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15, 0x38, 0x3F, 0x36, 0x31, 0x24, 0x23, 0x2A, 0x2D,
      0x70, 0x77, 0x7E, 0x79, 0x6C, 0x6B, 0x62, 0x65, 0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D,
      0xE0, 0xE7, 0xEE, 0xE9, 0xFC, 0xFB, 0xF2, 0xF5, 0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD,
      0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85, 0xA8, 0xAF, 0xA6, 0xA1, 0xB4, 0xB3, 0xBA, 0xBD,
      0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2, 0xFF, 0xF8, 0xF1, 0xF6, 0xE3, 0xE4, 0xED, 0xEA,
      0xB7, 0xB0, 0xB9, 0xBE, 0xAB, 0xAC, 0xA5, 0xA2, 0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D, 0x9A,
      0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32, 0x1F, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0D, 0x0A,
      0x57, 0x50, 0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42, 0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A,
      0x89, 0x8E, 0x87, 0x80, 0x95, 0x92, 0x9B, 0x9C, 0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4,
      0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC, 0xC1, 0xC6, 0xCF, 0xC8, 0xDD, 0xDA, 0xD3, 0xD4,
      0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C, 0x51, 0x56, 0x5F, 0x58, 0x4D, 0x4A, 0x43, 0x44,
      0x19, 0x1E, 0x17, 0x10, 0x05, 0x02, 0x0B, 0x0C, 0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A, 0x33, 0x34,
      0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B, 0x76, 0x71, 0x78, 0x7F, 0x6A, 0x6D, 0x64, 0x63,
      0x3E, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B, 0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13,
      0xAE, 0xA9, 0xA0, 0xA7, 0xB2, 0xB5, 0xBC, 0xBB, 0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83,
      0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB, 0xE6, 0xE1, 0xE8, 0xEF, 0xFA, 0xFD, 0xF4, 0xF3
    };
};

}  // namespace uapbridge_esp
}  // namespace esphome
