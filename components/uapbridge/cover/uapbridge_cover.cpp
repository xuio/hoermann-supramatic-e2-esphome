#include "uapbridge_cover.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>

namespace esphome {
namespace uapbridge {

static const char* const TAG = "uapbridge.cover";

void UAPBridgeCover::setup() {
  if (this->time_based_position_) {
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
  }
  this->parent_->add_on_state_callback([this]() { this->on_event_triggered(); });
}

void UAPBridgeCover::dump_config() {
  LOG_COVER("", "UAPBridge Cover", this);
  ESP_LOGCONFIG(TAG, "  Time Based Position: %s", this->time_based_position_ ? "true" : "false");
  if (this->time_based_position_) {
    ESP_LOGCONFIG(TAG, "  Open Duration: %.1fs", this->open_duration_ms_ / 1000.0f);
    ESP_LOGCONFIG(TAG, "  Close Duration: %.1fs", this->close_duration_ms_ / 1000.0f);
    ESP_LOGCONFIG(TAG, "  Position Publish Interval: %ums", (unsigned int) this->position_publish_interval_ms_);
    ESP_LOGCONFIG(TAG, "  Position Deadband: %.1f%%", this->position_deadband_ * 100.0f);
    ESP_LOGCONFIG(TAG, "  Venting Position: %.1f%%", this->venting_position_ * 100.0f);
    ESP_LOGCONFIG(TAG, "  Learn Travel Durations: %s", this->learn_travel_durations_ ? "true" : "false");
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

  this->arm_pending_movement_(operation, target, "Home Assistant position request");
}

void UAPBridgeCover::handle_time_based_event_() {
  const auto state = this->parent_->get_state();

  switch (state) {
    case UAPBridge::hoermann_state_t::hoermann_state_open:
      this->finish_travel_measurement_(state);
      this->sync_known_position_(cover::COVER_OPEN, "HCP open end state");
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_closed:
      this->finish_travel_measurement_(state);
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
        this->start_estimated_movement_(cover::COVER_OPERATION_OPENING, target, "HCP opening state");
      }
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_closing:
      if (this->current_operation != cover::COVER_OPERATION_CLOSING) {
        const float target = this->pending_movement_ && this->pending_operation_ == cover::COVER_OPERATION_CLOSING
                                 ? this->pending_target_position_
                                 : cover::COVER_CLOSED;
        this->clear_pending_movement_("HCP closing state");
        this->start_estimated_movement_(cover::COVER_OPERATION_CLOSING, target, "HCP closing state");
      }
      break;
    case UAPBridge::hoermann_state_t::hoermann_state_stopped:
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

void UAPBridgeCover::start_estimated_movement_(cover::CoverOperation operation, float target, const char *reason) {
  if (this->current_operation != cover::COVER_OPERATION_IDLE) {
    this->recompute_position_();
  }

  this->target_position_ = clamp(target, 0.0f, 1.0f);
  this->current_operation = operation;
  this->waiting_for_end_state_ = false;
  const uint32_t now = millis();
  this->last_recompute_time_ = now;
  this->last_publish_time_ = 0;
  this->begin_travel_measurement_(operation);
  ESP_LOGI(TAG, "Started estimated %s toward %.0f%%: %s",
           operation == cover::COVER_OPERATION_OPENING ? "opening" : "closing",
           this->target_position_ * 100.0f, reason);
  this->publish_if_changed_(true, false);
}

void UAPBridgeCover::recompute_position_() {
  if (this->current_operation == cover::COVER_OPERATION_IDLE) {
    return;
  }

  const uint32_t now = millis();
  if (this->last_recompute_time_ == 0) {
    this->last_recompute_time_ = now;
    return;
  }

  const uint32_t elapsed = now - this->last_recompute_time_;
  const uint32_t duration = this->current_operation == cover::COVER_OPERATION_OPENING ? this->open_duration_ms_ : this->close_duration_ms_;
  if (duration == 0) {
    return;
  }

  const float direction = this->current_operation == cover::COVER_OPERATION_OPENING ? 1.0f : -1.0f;
  this->position += direction * (float) elapsed / (float) duration;
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

void UAPBridgeCover::complete_estimated_target_() {
  const auto operation = this->current_operation;
  const float target = this->target_position_;

  if (!this->is_open_target_(target) && !this->is_closed_target_(target)) {
    if (!this->parent_->action_stop()) {
      ESP_LOGW(TAG, "Could not stop at estimated %.0f%% target; continuing estimate toward end stop", target * 100.0f);
      this->target_position_ = operation == cover::COVER_OPERATION_OPENING ? cover::COVER_OPEN : cover::COVER_CLOSED;
      return;
    }
    this->position = target;
    this->current_operation = cover::COVER_OPERATION_IDLE;
    this->waiting_for_end_state_ = false;
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
  this->last_publish_time_ = millis();
  this->publish_if_changed_(true, false);
}

void UAPBridgeCover::sync_known_position_(float position, const char *reason) {
  this->clear_pending_movement_(reason);
  this->position = clamp(position, 0.0f, 1.0f);
  this->target_position_ = this->position;
  this->current_operation = cover::COVER_OPERATION_IDLE;
  this->waiting_for_end_state_ = false;
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
  this->travel_measurement_active_ = false;
  ESP_LOGI(TAG, "Stopped estimated movement at %.0f%%: %s", this->position * 100.0f, reason);
  this->publish_if_changed_(true);
}

void UAPBridgeCover::force_non_closed_estimate_(const char *reason) {
  this->clear_pending_movement_(reason);
  this->recompute_position_();
  this->current_operation = cover::COVER_OPERATION_IDLE;
  this->waiting_for_end_state_ = false;
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
  this->travel_measurement_start_position_ = this->position;
}

void UAPBridgeCover::finish_travel_measurement_(UAPBridge::hoermann_state_t end_state) {
  if (!this->learn_travel_durations_ || !this->travel_measurement_active_) {
    return;
  }

  const uint32_t elapsed = millis() - this->travel_measurement_started_ms_;
  if (elapsed < 3000 || elapsed > 120000) {
    this->travel_measurement_active_ = false;
    return;
  }

  if (this->travel_measurement_operation_ == cover::COVER_OPERATION_OPENING &&
      end_state == UAPBridge::hoermann_state_t::hoermann_state_open &&
      this->travel_measurement_start_position_ <= this->position_deadband_) {
    this->open_duration_ms_ = elapsed;
    ESP_LOGI(TAG, "Learned full open travel duration: %.1fs. Copy this to open_duration if stable.",
             elapsed / 1000.0f);
  } else if (this->travel_measurement_operation_ == cover::COVER_OPERATION_CLOSING &&
             end_state == UAPBridge::hoermann_state_t::hoermann_state_closed &&
             this->travel_measurement_start_position_ >= 1.0f - this->position_deadband_) {
    this->close_duration_ms_ = elapsed;
    ESP_LOGI(TAG, "Learned full close travel duration: %.1fs. Copy this to close_duration if stable.",
             elapsed / 1000.0f);
  }

  this->travel_measurement_active_ = false;
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
