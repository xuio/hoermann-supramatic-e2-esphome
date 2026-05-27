#include "uapbridge_cover.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>

namespace esphome {
namespace uapbridge {

static const char* const TAG = "uapbridge.cover";
static constexpr uint32_t UAPBRIDGE_TRAVEL_DURATION_PREF_VERSION = 0xA3D21002;
static constexpr uint32_t UAPBRIDGE_TRAVEL_DURATION_MAGIC = 0x55415031;  // "UAP1"
static constexpr uint32_t UAPBRIDGE_MIN_TRAVEL_DURATION_MS = 3000;
static constexpr uint32_t UAPBRIDGE_MAX_TRAVEL_DURATION_MS = 120000;
static constexpr uint32_t UAPBRIDGE_MAX_TIMING_DELAY_MS = 30000;
static constexpr uint8_t UAPBRIDGE_CURVE_POINTS = 21;
static constexpr float UAPBRIDGE_OPEN_CURVE_POSITIONS[UAPBRIDGE_CURVE_POINTS] = {
    0.000f, 0.050f, 0.100f, 0.150f, 0.200f, 0.250f, 0.300f, 0.350f, 0.400f, 0.450f, 0.500f,
    0.550f, 0.600f, 0.650f, 0.700f, 0.750f, 0.800f, 0.850f, 0.900f, 0.950f, 1.000f};
static constexpr float UAPBRIDGE_OPEN_CURVE_TIMES[UAPBRIDGE_CURVE_POINTS] = {
    0.000000f, 0.067515f, 0.127617f, 0.182429f, 0.239383f, 0.293670f, 0.340556f,
    0.385968f, 0.433136f, 0.482617f, 0.529867f, 0.571687f, 0.611942f, 0.656035f,
    0.701833f, 0.750876f, 0.796362f, 0.832549f, 0.876414f, 0.932986f, 1.000000f};
static constexpr float UAPBRIDGE_CLOSE_CURVE_POSITIONS[UAPBRIDGE_CURVE_POINTS] = {
    1.000f, 0.950f, 0.900f, 0.850f, 0.800f, 0.750f, 0.700f, 0.650f, 0.600f, 0.550f, 0.500f,
    0.450f, 0.400f, 0.350f, 0.300f, 0.250f, 0.200f, 0.150f, 0.100f, 0.050f, 0.000f};
static constexpr float UAPBRIDGE_CLOSE_CURVE_TIMES[UAPBRIDGE_CURVE_POINTS] = {
    0.000000f, 0.036259f, 0.080759f, 0.141418f, 0.192207f, 0.242774f, 0.289641f,
    0.334118f, 0.375904f, 0.420744f, 0.467326f, 0.515136f, 0.563411f, 0.605913f,
    0.654759f, 0.705698f, 0.759795f, 0.813757f, 0.868730f, 0.930084f, 1.000000f};

struct UAPBridgeTravelDurationStore {
  uint32_t magic;
  uint32_t open_duration_ms;
  uint32_t close_duration_ms;
  uint32_t open_start_delay_ms;
  uint32_t close_start_delay_ms;
  uint32_t open_report_delay_ms;
  uint32_t close_report_delay_ms;
};

void UAPBridgeCover::setup() {
  if (this->time_based_position_) {
    this->travel_duration_pref_ =
        this->make_entity_preference<UAPBridgeTravelDurationStore>(UAPBRIDGE_TRAVEL_DURATION_PREF_VERSION);
    this->load_travel_durations_();

    auto restore = this->restore_state_();
    if (restore.has_value()) {
      restore->apply(this);
      this->position = clamp(this->position, 0.0f, 1.0f);
      if (this->is_closed_target_(this->position)) {
        this->position = std::max(this->position_deadband_, 0.01f);
        ESP_LOGI(TAG, "Restored closed estimate conservatively as %.0f%% until HCP confirms closed",
                 this->position * 100.0f);
      }
      ESP_LOGI(TAG, "Restored estimated cover position %.0f%%", this->position * 100.0f);
    } else {
      this->position = 0.5f;
      ESP_LOGI(TAG, "No restored cover position; starting estimate at 50%%");
    }
    this->target_position_ = this->position;
    this->setup_complete_ = true;
  }
  this->parent_->add_on_state_callback([this]() { this->on_event_triggered(); });
}

void UAPBridgeCover::set_open_duration(uint32_t value) {
  if (!this->is_valid_travel_duration_(value)) {
    ESP_LOGW(TAG, "Ignoring invalid open travel duration: %.1fs", value / 1000.0f);
    return;
  }
  if (this->open_duration_ms_ == value) {
    return;
  }
  this->open_duration_ms_ = value;
  ESP_LOGI(TAG, "Open travel duration set to %.1fs", this->open_duration_ms_ / 1000.0f);
  if (this->setup_complete_) {
    this->save_travel_durations_();
  }
}

void UAPBridgeCover::set_close_duration(uint32_t value) {
  if (!this->is_valid_travel_duration_(value)) {
    ESP_LOGW(TAG, "Ignoring invalid close travel duration: %.1fs", value / 1000.0f);
    return;
  }
  if (this->close_duration_ms_ == value) {
    return;
  }
  this->close_duration_ms_ = value;
  ESP_LOGI(TAG, "Close travel duration set to %.1fs", this->close_duration_ms_ / 1000.0f);
  if (this->setup_complete_) {
    this->save_travel_durations_();
  }
}

void UAPBridgeCover::set_open_start_delay(uint32_t value) {
  if (!this->is_valid_delay_(value)) {
    ESP_LOGW(TAG, "Ignoring invalid open start delay: %.1fs", value / 1000.0f);
    return;
  }
  this->open_start_delay_ms_ = value;
  if (this->setup_complete_) {
    this->save_travel_durations_();
  }
}

void UAPBridgeCover::set_close_start_delay(uint32_t value) {
  if (!this->is_valid_delay_(value)) {
    ESP_LOGW(TAG, "Ignoring invalid close start delay: %.1fs", value / 1000.0f);
    return;
  }
  this->close_start_delay_ms_ = value;
  if (this->setup_complete_) {
    this->save_travel_durations_();
  }
}

void UAPBridgeCover::set_open_report_delay(uint32_t value) {
  if (!this->is_valid_delay_(value)) {
    ESP_LOGW(TAG, "Ignoring invalid open report delay: %.1fs", value / 1000.0f);
    return;
  }
  this->open_report_delay_ms_ = value;
  if (this->setup_complete_) {
    this->save_travel_durations_();
  }
}

void UAPBridgeCover::set_close_report_delay(uint32_t value) {
  if (!this->is_valid_delay_(value)) {
    ESP_LOGW(TAG, "Ignoring invalid close report delay: %.1fs", value / 1000.0f);
    return;
  }
  this->close_report_delay_ms_ = value;
  if (this->setup_complete_) {
    this->save_travel_durations_();
  }
}

void UAPBridgeCover::dump_config() {
  LOG_COVER("", "UAPBridge Cover", this);
  ESP_LOGCONFIG(TAG, "  Time Based Position: %s", this->time_based_position_ ? "true" : "false");
  if (this->time_based_position_) {
    ESP_LOGCONFIG(TAG, "  Open Motion Duration: %.3fs", this->open_duration_ms_ / 1000.0f);
    ESP_LOGCONFIG(TAG, "  Close Motion Duration: %.3fs", this->close_duration_ms_ / 1000.0f);
    ESP_LOGCONFIG(TAG, "  Open Start Delay: %.3fs", this->open_start_delay_ms_ / 1000.0f);
    ESP_LOGCONFIG(TAG, "  Close Start Delay: %.3fs", this->close_start_delay_ms_ / 1000.0f);
    ESP_LOGCONFIG(TAG, "  Open Report Delay: %.3fs", this->open_report_delay_ms_ / 1000.0f);
    ESP_LOGCONFIG(TAG, "  Close Report Delay: %.3fs", this->close_report_delay_ms_ / 1000.0f);
    ESP_LOGCONFIG(TAG, "  Close Obstruction Grace: %.1fs", this->close_obstruction_grace_ms_ / 1000.0f);
    ESP_LOGCONFIG(TAG, "  Position Publish Interval: %ums", (unsigned int) this->position_publish_interval_ms_);
    ESP_LOGCONFIG(TAG, "  Position Deadband: %.1f%%", this->position_deadband_ * 100.0f);
    ESP_LOGCONFIG(TAG, "  Venting Position: %.1f%%", this->venting_position_ * 100.0f);
    ESP_LOGCONFIG(TAG, "  Learn Travel Durations: %s", this->learn_travel_durations_ ? "true" : "false");
    ESP_LOGCONFIG(TAG, "  Empirical Motion Curve: %s", this->use_motion_curve_ ? "true" : "false");
  }
}

void UAPBridgeCover::loop() {
  if (!this->time_based_position_) {
    return;
  }

  if (this->pending_movement_) {
    this->service_pending_movement_();
  }

  if (this->current_operation == cover::COVER_OPERATION_IDLE) {
    return;
  }

  if (this->waiting_for_end_state_) {
    this->service_end_state_wait_();
    if (!this->waiting_for_end_state_ || this->current_operation == cover::COVER_OPERATION_IDLE) {
      return;
    }
    if (millis() - this->last_publish_time_ >= this->position_publish_interval_ms_) {
      this->last_publish_time_ = millis();
      this->publish_if_changed_(true, false);
    }
    return;
  }

  this->recompute_position_();
  if (this->is_at_target_()) {
    this->complete_estimated_target_();
    return;
  }

  if (millis() - this->last_publish_time_ >= this->position_publish_interval_ms_) {
    this->last_publish_time_ = millis();
    this->publish_if_changed_(true, false);
  }
}

cover::CoverTraits UAPBridgeCover::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_is_assumed_state(true);
  traits.set_supports_position(this->time_based_position_);
  traits.set_supports_stop(true);
  traits.set_supports_tilt(false);
  traits.set_supports_toggle(false);
  return traits;
}

void UAPBridgeCover::control(const cover::CoverCall& call) {
  if (call.get_stop()) {
    if (this->time_based_position_) {
      this->recompute_position_();
    }
    if (parent_->action_stop()) {
      if (this->time_based_position_ && this->pending_movement_) {
        this->clear_pending_movement_("explicit stop command");
        this->publish_if_changed_(true, false);
      } else if (this->time_based_position_) {
        this->stop_estimated_movement_("explicit stop command");
      }
      ESP_LOGI(TAG, "Stopping the cover");
    } else {
      ESP_LOGW(TAG, "Stop command was rejected by UAP bridge safety gates");
    }
    return;
  }

  if (call.get_position().has_value()) {
    float position = clamp(*call.get_position(), 0.0f, 1.0f);
    if (this->time_based_position_) {
      this->control_time_based_position_(position);
    } else if (position == 0) {
      if (parent_->action_close()) {
        ESP_LOGI(TAG, "Closing the cover");
      } else {
        ESP_LOGW(TAG, "Close command was rejected by UAP bridge safety gates");
      }
    } else if (position == 1) {
      if (parent_->action_open()) {
        ESP_LOGI(TAG, "Opening the cover");
      } else {
        ESP_LOGW(TAG, "Open command was rejected by UAP bridge safety gates");
      }
    } else {
      ESP_LOGW(TAG, "Ignoring intermediate cover position %.0f%% because time_based_position is disabled",
               position * 100.0f);
    }
  }
  if (call.get_toggle()) {
    ESP_LOGW(TAG, "Ignoring cover toggle; use the explicit impulse button if needed");
  }
}

void UAPBridgeCover::on_event_triggered() {
  if (this->time_based_position_) {
    this->handle_time_based_event_();
    return;
  }

  switch (this->parent_->get_state()) {
    case UAPBridge::hoermann_state_t::hoermann_state_opening:
      this->current_operation = cover::COVER_OPERATION_OPENING;
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_open:
      this->current_operation = cover::COVER_OPERATION_IDLE;
      this->position = 1;
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_closed:
      this->current_operation = cover::COVER_OPERATION_IDLE;
      this->position = 0;
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_closing:
      this->current_operation = cover::COVER_OPERATION_CLOSING;
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_error:
    case UAPBridge::hoermann_state_t::hoermann_state_venting:
      this->current_operation = cover::COVER_OPERATION_IDLE;
      this->position = 0.2;
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_stopped:
      this->current_operation = cover::COVER_OPERATION_IDLE;
      this->position = 0.5;
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_unknown:
    default:
      this->current_operation = cover::COVER_OPERATION_IDLE;
      this->position = 1.0;
      break;
  }
  if (this->previousState_ != this->parent_->get_state() || this->previousOperation_ != this->current_operation) {
    ESP_LOGV(TAG, "UAPBridgeCover::update() - position is %f", this->position);
    ESP_LOGV(TAG, "UAPBridgeCover::update() - operation is %d", this->current_operation);
    ESP_LOGD(TAG, "UAPBridgeCover::update() - state changed");
    this->publish_state();
    this->previousState_ = this->parent_->get_state();
    this->previousOperation_ = this->current_operation;
  }
}

void UAPBridgeCover::control_time_based_position_(float target) {
  if (this->pending_movement_) {
    this->service_pending_movement_();
  }
  if (this->current_operation != cover::COVER_OPERATION_IDLE) {
    this->recompute_position_();
  }

  if (this->is_closed_target_(target)) {
    target = cover::COVER_CLOSED;
  } else if (this->is_open_target_(target)) {
    target = cover::COVER_OPEN;
  }

  const auto operation = target < this->position ? cover::COVER_OPERATION_CLOSING : cover::COVER_OPERATION_OPENING;

  if (this->pending_movement_) {
    if (std::fabs(target - this->position) <= this->position_deadband_) {
      if (this->parent_->action_stop()) {
        this->clear_pending_movement_("Home Assistant target reached before queued command was fetched");
        this->target_position_ = this->position;
        this->publish_if_changed_(true, false);
      } else {
        ESP_LOGW(TAG, "Could not cancel queued command for requested %.0f%%", target * 100.0f);
        this->publish_if_changed_(true, false);
      }
      return;
    }

    if (operation == this->pending_operation_) {
      this->pending_target_position_ = target;
      this->target_position_ = target;
      ESP_LOGI(TAG, "Retargeted pending estimated movement to %.0f%%", target * 100.0f);
      this->publish_if_changed_(true, false);
      return;
    }

    if (this->parent_->action_stop()) {
      this->clear_pending_movement_("opposite direction request before queued command was fetched");
    } else {
      ESP_LOGW(TAG, "Rejected reverse position request %.0f%% because queued command could not be cancelled",
               target * 100.0f);
      this->publish_if_changed_(true, false);
      return;
    }
  }

  if (this->current_operation == cover::COVER_OPERATION_IDLE && std::fabs(target - this->position) <= this->position_deadband_) {
    ESP_LOGI(TAG, "Cover already near requested position %.0f%%; estimate is %.0f%%",
             target * 100.0f, this->position * 100.0f);
    this->target_position_ = target;
    this->publish_if_changed_(true);
    return;
  }

  if (this->current_operation != cover::COVER_OPERATION_IDLE) {
    if (std::fabs(target - this->position) <= this->position_deadband_) {
      if (this->parent_->action_stop()) {
        this->position = target;
        this->stop_estimated_movement_("Home Assistant target reached while already moving");
      } else {
        ESP_LOGW(TAG, "Could not stop at requested %.0f%% while already moving", target * 100.0f);
        this->publish_if_changed_(true, false);
      }
      return;
    }

    if (operation == this->current_operation) {
      this->target_position_ = target;
      ESP_LOGI(TAG, "Retargeted active estimated movement to %.0f%%", this->target_position_ * 100.0f);
      this->publish_if_changed_(true, false);
      return;
    }

    if (this->parent_->action_stop()) {
      this->stop_estimated_movement_("opposite direction Home Assistant position request");
      ESP_LOGW(TAG, "Stopped active movement before reversing direction; send the position request again after the door stops");
    } else {
      ESP_LOGW(TAG, "Rejected reverse position request %.0f%% because stop command was not accepted", target * 100.0f);
      this->publish_if_changed_(true, false);
    }
    return;
  }

  const bool accepted = operation == cover::COVER_OPERATION_OPENING ? this->parent_->action_open() : this->parent_->action_close();
  if (!accepted) {
    ESP_LOGW(TAG, "Position request %.0f%% was rejected by UAP bridge safety gates", target * 100.0f);
    this->publish_if_changed_(true, false);
    return;
  }

  if (operation == cover::COVER_OPERATION_OPENING) {
    this->parent_->set_obstruction_state(false);
  }
  this->arm_pending_movement_(operation, target, "Home Assistant position request");
}

void UAPBridgeCover::handle_time_based_event_() {
  const auto state = this->parent_->get_state();

  switch (state) {
    case UAPBridge::hoermann_state_t::hoermann_state_open:
      if (this->should_ignore_old_end_state_(state)) {
        ESP_LOGD(TAG, "Ignoring old HCP open state while expected closing movement is starting");
        break;
      }
      if ((this->pending_movement_ && this->pending_operation_ == cover::COVER_OPERATION_CLOSING) ||
          this->current_operation == cover::COVER_OPERATION_CLOSING) {
        this->latch_close_obstruction_("HCP returned open during close attempt");
        this->force_non_closed_estimate_("HCP returned open during close attempt");
        break;
      }
      this->finish_travel_measurement_(state);
      this->sync_known_position_(cover::COVER_OPEN, "HCP open end state");
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_closed:
      if (this->should_ignore_old_end_state_(state)) {
        ESP_LOGD(TAG, "Ignoring old HCP closed state while expected opening movement is starting");
        break;
      }
      this->finish_travel_measurement_(state);
      this->parent_->set_obstruction_state(false);
      this->sync_known_position_(cover::COVER_CLOSED, "HCP closed end state");
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_venting:
      this->sync_known_position_(this->venting_position_, "HCP venting state");
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_opening:
      if (this->current_operation != cover::COVER_OPERATION_OPENING) {
        const float target = this->pending_movement_ && this->pending_operation_ == cover::COVER_OPERATION_OPENING
                                 ? this->pending_target_position_
                                 : cover::COVER_OPEN;
        this->clear_pending_movement_("HCP opening state");
        this->start_estimated_movement_(cover::COVER_OPERATION_OPENING, target, "HCP opening state", true);
      } else {
        this->movement_start_grace_until_ms_ = 0;
      }
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_closing:
      if (this->current_operation != cover::COVER_OPERATION_CLOSING) {
        const float target = this->pending_movement_ && this->pending_operation_ == cover::COVER_OPERATION_CLOSING
                                 ? this->pending_target_position_
                                 : cover::COVER_CLOSED;
        this->clear_pending_movement_("HCP closing state");
        this->start_estimated_movement_(cover::COVER_OPERATION_CLOSING, target, "HCP closing state", true);
      } else {
        this->movement_start_grace_until_ms_ = 0;
      }
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_stopped:
      if (this->pending_movement_ || this->current_operation != cover::COVER_OPERATION_IDLE) {
        ESP_LOGD(TAG, "Ignoring ambiguous HCP stopped state while estimated movement is active");
        break;
      }
      this->stop_estimated_movement_("HCP stopped state");
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_unknown:
    default:
      this->force_non_closed_estimate_("HCP state became unknown");
      break;
  }

  this->previousState_ = state;
}

void UAPBridgeCover::arm_pending_movement_(cover::CoverOperation operation, float target, const char *reason) {
  this->pending_movement_ = true;
  this->pending_operation_ = operation;
  this->pending_target_position_ = clamp(target, 0.0f, 1.0f);
  this->pending_command_sequence_ = this->parent_->get_command_sequence();
  this->pending_started_ms_ = millis();
  this->target_position_ = this->pending_target_position_;
  this->end_state_wait_started_ms_ = 0;
  ESP_LOGI(TAG, "Queued estimated %s toward %.0f%%; waiting for HCP command transmission: %s",
           operation == cover::COVER_OPERATION_OPENING ? "opening" : "closing",
           this->pending_target_position_ * 100.0f, reason);
  this->publish_if_changed_(true, false);
}

void UAPBridgeCover::service_pending_movement_() {
  if (!this->pending_movement_) {
    return;
  }

  if (this->parent_->get_command_sequence() != this->pending_command_sequence_) {
    const auto operation = this->pending_operation_;
    const float target = this->pending_target_position_;
    this->clear_pending_movement_("HCP command was transmitted");
    this->start_estimated_movement_(operation, target, "HCP command was transmitted");
    return;
  }

  const uint32_t timeout_ms = this->parent_->get_command_timeout();
  if (timeout_ms != 0 && millis() - this->pending_started_ms_ > timeout_ms + 100) {
    ESP_LOGW(TAG, "Queued HCP command was not fetched within %ums; cancelling pending estimate",
             (unsigned int) timeout_ms);
    this->clear_pending_movement_("queued command timed out");
    this->publish_if_changed_(true, false);
  }
}

void UAPBridgeCover::clear_pending_movement_(const char *reason) {
  if (!this->pending_movement_) {
    return;
  }
  ESP_LOGD(TAG, "Cleared pending estimated movement: %s", reason);
  this->pending_movement_ = false;
  this->pending_operation_ = cover::COVER_OPERATION_IDLE;
}

void UAPBridgeCover::start_estimated_movement_(cover::CoverOperation operation, float target, const char *reason,
                                               bool movement_confirmed) {
  if (this->current_operation != cover::COVER_OPERATION_IDLE) {
    this->recompute_position_();
  }

  this->target_position_ = clamp(target, 0.0f, 1.0f);
  this->current_operation = operation;
  this->waiting_for_end_state_ = false;
  this->end_state_wait_started_ms_ = 0;
  const uint32_t now = millis();
  this->last_recompute_time_ = now;
  this->last_publish_time_ = 0;
  this->movement_started_ms_ = now;
  this->movement_start_position_ = this->position;
  this->movement_start_grace_until_ms_ = movement_confirmed ? 0 : now + 5000;
  this->begin_travel_measurement_(operation);
  ESP_LOGI(TAG, "Started estimated %s toward %.0f%% from %.0f%%: %s",
           operation == cover::COVER_OPERATION_OPENING ? "opening" : "closing",
           this->target_position_ * 100.0f, this->movement_start_position_ * 100.0f, reason);
  this->publish_if_changed_(true, false);
}

bool UAPBridgeCover::should_ignore_old_end_state_(UAPBridge::hoermann_state_t state) const {
  if (this->pending_movement_) {
    return (state == UAPBridge::hoermann_state_t::hoermann_state_open &&
            this->pending_operation_ == cover::COVER_OPERATION_CLOSING) ||
           (state == UAPBridge::hoermann_state_t::hoermann_state_closed &&
            this->pending_operation_ == cover::COVER_OPERATION_OPENING);
  }
  if (this->current_operation == cover::COVER_OPERATION_IDLE || this->movement_start_grace_until_ms_ == 0 ||
      millis() > this->movement_start_grace_until_ms_) {
    return false;
  }
  return (state == UAPBridge::hoermann_state_t::hoermann_state_open &&
          this->current_operation == cover::COVER_OPERATION_CLOSING) ||
         (state == UAPBridge::hoermann_state_t::hoermann_state_closed &&
          this->current_operation == cover::COVER_OPERATION_OPENING);
}

void UAPBridgeCover::recompute_position_() {
  if (this->current_operation == cover::COVER_OPERATION_IDLE) {
    return;
  }

  const uint32_t now = millis();
  if (this->movement_started_ms_ == 0) {
    this->movement_started_ms_ = now;
    this->movement_start_position_ = this->position;
  }
  if (this->last_recompute_time_ == 0) {
    this->last_recompute_time_ = now;
    return;
  }

  const uint32_t elapsed_since_command = now - this->movement_started_ms_;
  const uint32_t start_delay = this->start_delay_for_operation_(this->current_operation);
  if (elapsed_since_command <= start_delay) {
    this->position = this->movement_start_position_;
    this->last_recompute_time_ = now;
    return;
  }

  const uint32_t elapsed = elapsed_since_command - start_delay;
  const uint32_t duration = this->current_operation == cover::COVER_OPERATION_OPENING ? this->open_duration_ms_ : this->close_duration_ms_;
  if (duration == 0) {
    return;
  }

  if (this->use_motion_curve_) {
    const float start_curve_time = this->curve_time_for_position_(this->current_operation, this->movement_start_position_);
    const float curve_time = clamp(start_curve_time + (float) elapsed / (float) duration, 0.0f, 1.0f);
    this->position = this->curve_position_for_time_(this->current_operation, curve_time);
  } else {
    const float direction = this->current_operation == cover::COVER_OPERATION_OPENING ? 1.0f : -1.0f;
    this->position = this->movement_start_position_ + direction * (float) elapsed / (float) duration;
  }
  this->position = clamp(this->position, 0.0f, 1.0f);
  this->last_recompute_time_ = now;
}

bool UAPBridgeCover::is_at_target_() const {
  switch (this->current_operation) {
    case cover::COVER_OPERATION_OPENING:
      return this->position >= this->target_position_;
    case cover::COVER_OPERATION_CLOSING:
      return this->position <= this->target_position_;
    case cover::COVER_OPERATION_IDLE:
    default:
      return true;
  }
}

bool UAPBridgeCover::target_is_end_state_() const {
  return this->is_open_target_(this->target_position_) || this->is_closed_target_(this->target_position_);
}

void UAPBridgeCover::latch_close_obstruction_(const char *reason) {
  if (!this->parent_->get_obstruction_state()) {
    ESP_LOGW(TAG, "Latching obstruction/close failure: %s", reason);
  } else {
    ESP_LOGW(TAG, "Obstruction/close failure remains active: %s", reason);
  }
  this->parent_->set_obstruction_state(true);
}

void UAPBridgeCover::service_end_state_wait_() {
  if (!this->waiting_for_end_state_ || this->current_operation != cover::COVER_OPERATION_CLOSING ||
      this->end_state_wait_started_ms_ == 0 || this->close_obstruction_grace_ms_ == 0) {
    return;
  }

  const uint32_t expected_report_window = this->close_report_delay_ms_ + this->close_obstruction_grace_ms_;
  if (millis() - this->end_state_wait_started_ms_ < expected_report_window) {
    return;
  }
  if (this->parent_->get_state() == UAPBridge::hoermann_state_t::hoermann_state_closed) {
    return;
  }

  this->latch_close_obstruction_("close travel elapsed without HCP closed bit");
  this->force_non_closed_estimate_("close travel elapsed without HCP closed bit");
}

void UAPBridgeCover::complete_estimated_target_() {
  const auto operation = this->current_operation;
  const float target = this->target_position_;

  if (!this->target_is_end_state_()) {
    if (!this->parent_->action_stop()) {
      ESP_LOGW(TAG, "Could not stop at estimated %.0f%% target; continuing estimate toward end stop", target * 100.0f);
      this->target_position_ = operation == cover::COVER_OPERATION_OPENING ? cover::COVER_OPEN : cover::COVER_CLOSED;
      return;
    }
    this->position = target;
    this->current_operation = cover::COVER_OPERATION_IDLE;
    this->waiting_for_end_state_ = false;
    this->end_state_wait_started_ms_ = 0;
    this->movement_start_grace_until_ms_ = 0;
    this->movement_started_ms_ = 0;
    this->travel_measurement_active_ = false;
    ESP_LOGI(TAG, "Stopped at estimated intermediate position %.0f%%", target * 100.0f);
    this->publish_if_changed_(true);
    return;
  }

  if (this->is_closed_target_(target)) {
    this->position = std::max(this->position_deadband_, 0.01f);
    ESP_LOGW(TAG, "Estimated close travel time elapsed; waiting for HCP closed bit before publishing 0%%");
  } else {
    this->position = cover::COVER_OPEN;
    ESP_LOGI(TAG, "Estimated open travel time elapsed; waiting for HCP open bit");
  }
  this->waiting_for_end_state_ = true;
  this->end_state_wait_started_ms_ = millis();
  this->last_publish_time_ = millis();
  this->publish_if_changed_(true, false);
}

void UAPBridgeCover::sync_known_position_(float position, const char *reason) {
  this->clear_pending_movement_(reason);
  this->position = clamp(position, 0.0f, 1.0f);
  this->target_position_ = this->position;
  this->current_operation = cover::COVER_OPERATION_IDLE;
  this->waiting_for_end_state_ = false;
  this->end_state_wait_started_ms_ = 0;
  this->movement_start_grace_until_ms_ = 0;
  this->movement_started_ms_ = 0;
  this->travel_measurement_active_ = false;
  ESP_LOGI(TAG, "Synced cover position to %.0f%% from %s", this->position * 100.0f, reason);
  this->publish_if_changed_(true);
}

void UAPBridgeCover::stop_estimated_movement_(const char *reason) {
  this->clear_pending_movement_(reason);
  this->recompute_position_();
  this->current_operation = cover::COVER_OPERATION_IDLE;
  this->target_position_ = this->position;
  this->waiting_for_end_state_ = false;
  this->end_state_wait_started_ms_ = 0;
  this->movement_start_grace_until_ms_ = 0;
  this->movement_started_ms_ = 0;
  this->travel_measurement_active_ = false;
  ESP_LOGI(TAG, "Stopped estimated movement at %.0f%%: %s", this->position * 100.0f, reason);
  this->publish_if_changed_(true);
}

void UAPBridgeCover::force_non_closed_estimate_(const char *reason) {
  this->clear_pending_movement_(reason);
  this->recompute_position_();
  this->current_operation = cover::COVER_OPERATION_IDLE;
  this->waiting_for_end_state_ = false;
  this->end_state_wait_started_ms_ = 0;
  this->movement_start_grace_until_ms_ = 0;
  this->movement_started_ms_ = 0;
  this->travel_measurement_active_ = false;
  if (this->is_closed_target_(this->position)) {
    this->position = std::max(this->position_deadband_, 0.01f);
  }
  this->target_position_ = this->position;
  ESP_LOGW(TAG, "Publishing conservative non-closed estimate %.0f%%: %s",
           this->position * 100.0f, reason);
  this->publish_if_changed_(true, false);
}

void UAPBridgeCover::begin_travel_measurement_(cover::CoverOperation operation) {
  if (!this->learn_travel_durations_) {
    return;
  }
  this->travel_measurement_active_ = true;
  this->travel_measurement_operation_ = operation;
  this->travel_measurement_started_ms_ = millis();
  this->travel_measurement_start_position_ = this->movement_start_position_;
}

void UAPBridgeCover::finish_travel_measurement_(UAPBridge::hoermann_state_t end_state) {
  if (!this->learn_travel_durations_ || !this->travel_measurement_active_) {
    return;
  }

  const uint32_t elapsed = millis() - this->travel_measurement_started_ms_;
  const uint32_t configured_delays =
      this->start_delay_for_operation_(this->travel_measurement_operation_) +
      this->report_delay_for_operation_(this->travel_measurement_operation_);
  if (elapsed <= configured_delays) {
    this->travel_measurement_active_ = false;
    return;
  }
  const uint32_t visible_elapsed = elapsed - configured_delays;
  if (visible_elapsed < 3000 || visible_elapsed > 120000) {
    this->travel_measurement_active_ = false;
    return;
  }

  if (this->travel_measurement_operation_ == cover::COVER_OPERATION_OPENING &&
      end_state == UAPBridge::hoermann_state_t::hoermann_state_open &&
      this->travel_measurement_start_position_ <= this->position_deadband_) {
    this->set_open_duration(visible_elapsed);
    ESP_LOGI(TAG, "Automatically learned full open visible travel duration: %.3fs (elapsed %.3fs minus %.3fs delay)",
             visible_elapsed / 1000.0f, elapsed / 1000.0f, configured_delays / 1000.0f);
  } else if (this->travel_measurement_operation_ == cover::COVER_OPERATION_CLOSING &&
             end_state == UAPBridge::hoermann_state_t::hoermann_state_closed &&
             this->travel_measurement_start_position_ >= 1.0f - this->position_deadband_) {
    this->set_close_duration(visible_elapsed);
    ESP_LOGI(TAG, "Automatically learned full close visible travel duration: %.3fs (elapsed %.3fs minus %.3fs delay)",
             visible_elapsed / 1000.0f, elapsed / 1000.0f, configured_delays / 1000.0f);
  }

  this->travel_measurement_active_ = false;
}

void UAPBridgeCover::load_travel_durations_() {
  UAPBridgeTravelDurationStore store{};
  if (!this->travel_duration_pref_.load(&store)) {
    ESP_LOGD(TAG, "No stored travel durations; using YAML defaults");
    return;
  }
  if (store.magic != UAPBRIDGE_TRAVEL_DURATION_MAGIC ||
      !this->is_valid_travel_duration_(store.open_duration_ms) ||
      !this->is_valid_travel_duration_(store.close_duration_ms) ||
      !this->is_valid_delay_(store.open_start_delay_ms) ||
      !this->is_valid_delay_(store.close_start_delay_ms) ||
      !this->is_valid_delay_(store.open_report_delay_ms) ||
      !this->is_valid_delay_(store.close_report_delay_ms)) {
    ESP_LOGW(TAG, "Ignoring invalid stored travel durations");
    return;
  }

  this->open_duration_ms_ = store.open_duration_ms;
  this->close_duration_ms_ = store.close_duration_ms;
  this->open_start_delay_ms_ = store.open_start_delay_ms;
  this->close_start_delay_ms_ = store.close_start_delay_ms;
  this->open_report_delay_ms_ = store.open_report_delay_ms;
  this->close_report_delay_ms_ = store.close_report_delay_ms;
  ESP_LOGI(TAG,
           "Loaded stored travel timing: open=%.3fs close=%.3fs open_start=%.3fs close_start=%.3fs "
           "open_report=%.3fs close_report=%.3fs",
           this->open_duration_ms_ / 1000.0f, this->close_duration_ms_ / 1000.0f,
           this->open_start_delay_ms_ / 1000.0f, this->close_start_delay_ms_ / 1000.0f,
           this->open_report_delay_ms_ / 1000.0f, this->close_report_delay_ms_ / 1000.0f);
}

void UAPBridgeCover::save_travel_durations_() {
  if (!this->time_based_position_) {
    return;
  }

  UAPBridgeTravelDurationStore store{};
  store.magic = UAPBRIDGE_TRAVEL_DURATION_MAGIC;
  store.open_duration_ms = this->open_duration_ms_;
  store.close_duration_ms = this->close_duration_ms_;
  store.open_start_delay_ms = this->open_start_delay_ms_;
  store.close_start_delay_ms = this->close_start_delay_ms_;
  store.open_report_delay_ms = this->open_report_delay_ms_;
  store.close_report_delay_ms = this->close_report_delay_ms_;
  if (!this->travel_duration_pref_.save(&store)) {
    ESP_LOGW(TAG, "Failed to save learned travel durations");
    return;
  }
  if (global_preferences != nullptr) {
    global_preferences->sync();
  }
  ESP_LOGD(TAG, "Saved travel durations: open=%.1fs close=%.1fs",
           this->open_duration_ms_ / 1000.0f, this->close_duration_ms_ / 1000.0f);
}

bool UAPBridgeCover::is_valid_travel_duration_(uint32_t duration_ms) const {
  return duration_ms >= UAPBRIDGE_MIN_TRAVEL_DURATION_MS && duration_ms <= UAPBRIDGE_MAX_TRAVEL_DURATION_MS;
}

bool UAPBridgeCover::is_valid_delay_(uint32_t delay_ms) const {
  return delay_ms <= UAPBRIDGE_MAX_TIMING_DELAY_MS;
}

uint32_t UAPBridgeCover::start_delay_for_operation_(cover::CoverOperation operation) const {
  return operation == cover::COVER_OPERATION_OPENING ? this->open_start_delay_ms_ : this->close_start_delay_ms_;
}

uint32_t UAPBridgeCover::report_delay_for_operation_(cover::CoverOperation operation) const {
  return operation == cover::COVER_OPERATION_OPENING ? this->open_report_delay_ms_ : this->close_report_delay_ms_;
}

float UAPBridgeCover::curve_time_for_position_(cover::CoverOperation operation, float position) const {
  position = clamp(position, 0.0f, 1.0f);
  const float *positions =
      operation == cover::COVER_OPERATION_OPENING ? UAPBRIDGE_OPEN_CURVE_POSITIONS : UAPBRIDGE_CLOSE_CURVE_POSITIONS;
  const float *times =
      operation == cover::COVER_OPERATION_OPENING ? UAPBRIDGE_OPEN_CURVE_TIMES : UAPBRIDGE_CLOSE_CURVE_TIMES;

  if (operation == cover::COVER_OPERATION_OPENING) {
    if (position <= positions[0]) {
      return times[0];
    }
    for (uint8_t i = 0; i < UAPBRIDGE_CURVE_POINTS - 1; i++) {
      if (position <= positions[i + 1]) {
        const float span = positions[i + 1] - positions[i];
        const float ratio = span > 0.0f ? (position - positions[i]) / span : 0.0f;
        return times[i] + ratio * (times[i + 1] - times[i]);
      }
    }
    return times[UAPBRIDGE_CURVE_POINTS - 1];
  }

  if (position >= positions[0]) {
    return times[0];
  }
  for (uint8_t i = 0; i < UAPBRIDGE_CURVE_POINTS - 1; i++) {
    if (position >= positions[i + 1]) {
      const float span = positions[i] - positions[i + 1];
      const float ratio = span > 0.0f ? (positions[i] - position) / span : 0.0f;
      return times[i] + ratio * (times[i + 1] - times[i]);
    }
  }
  return times[UAPBRIDGE_CURVE_POINTS - 1];
}

float UAPBridgeCover::curve_position_for_time_(cover::CoverOperation operation, float normalized_time) const {
  normalized_time = clamp(normalized_time, 0.0f, 1.0f);
  const float *positions =
      operation == cover::COVER_OPERATION_OPENING ? UAPBRIDGE_OPEN_CURVE_POSITIONS : UAPBRIDGE_CLOSE_CURVE_POSITIONS;
  const float *times =
      operation == cover::COVER_OPERATION_OPENING ? UAPBRIDGE_OPEN_CURVE_TIMES : UAPBRIDGE_CLOSE_CURVE_TIMES;

  if (normalized_time <= times[0]) {
    return positions[0];
  }
  for (uint8_t i = 0; i < UAPBRIDGE_CURVE_POINTS - 1; i++) {
    if (normalized_time <= times[i + 1]) {
      const float span = times[i + 1] - times[i];
      const float ratio = span > 0.0f ? (normalized_time - times[i]) / span : 0.0f;
      return clamp(positions[i] + ratio * (positions[i + 1] - positions[i]), 0.0f, 1.0f);
    }
  }
  return positions[UAPBRIDGE_CURVE_POINTS - 1];
}

void UAPBridgeCover::publish_if_changed_(bool force, bool save) {
  if (force || this->previousState_ != this->parent_->get_state() ||
      this->previousOperation_ != this->current_operation ||
      !this->almost_equal_(this->previousPosition_, this->position)) {
    ESP_LOGD(TAG, "Publishing cover estimate: position=%.0f%% operation=%d hcp_state=%d",
             this->position * 100.0f, this->current_operation, this->parent_->get_state());
    this->publish_state(save);
    this->previousState_ = this->parent_->get_state();
    this->previousOperation_ = this->current_operation;
    this->previousPosition_ = this->position;
  }
}

bool UAPBridgeCover::almost_equal_(float a, float b) const {
  return std::fabs(a - b) <= 0.001f;
}

bool UAPBridgeCover::is_closed_target_(float target) const {
  return target <= this->position_deadband_;
}

bool UAPBridgeCover::is_open_target_(float target) const {
  return target >= 1.0f - this->position_deadband_;
}
}  // namespace uapbridge
}  // namespace esphome
