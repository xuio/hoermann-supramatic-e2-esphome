#pragma once

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "../hcp2bridge.h"

namespace esphome {
namespace hcp2bridge {

class HCP2BridgeDiagnosticSensorBase : public sensor::Sensor, public PollingComponent {
 public:
  void set_hcp2bridge_parent(HCP2Bridge *parent) { this->parent_ = parent; }
  void update() override;
  void dump_config() override;

 protected:
  virtual uint32_t read_value_() const = 0;
  virtual const char *sensor_name_() const = 0;
  HCP2Bridge *parent_{nullptr};
};

class HCP2BridgeLPHeartbeatSensor : public HCP2BridgeDiagnosticSensorBase {
 protected:
  uint32_t read_value_() const override { return this->parent_->get_lp_heartbeat(); }
  const char *sensor_name_() const override { return "LP Heartbeat"; }
};

class HCP2BridgeLPResetCountSensor : public HCP2BridgeDiagnosticSensorBase {
 protected:
  uint32_t read_value_() const override { return this->parent_->get_lp_reset_count(); }
  const char *sensor_name_() const override { return "LP Reset Count"; }
};

class HCP2BridgePollsSeenSensor : public HCP2BridgeDiagnosticSensorBase {
 protected:
  uint32_t read_value_() const override { return this->parent_->get_lp_poll_count(); }
  const char *sensor_name_() const override { return "Polls Seen"; }
};

class HCP2BridgePollsAnsweredSensor : public HCP2BridgeDiagnosticSensorBase {
 protected:
  uint32_t read_value_() const override { return this->parent_->get_lp_response_count(); }
  const char *sensor_name_() const override { return "Polls Answered"; }
};

class HCP2BridgeMissedPollsSensor : public HCP2BridgeDiagnosticSensorBase {
 protected:
  uint32_t read_value_() const override { return this->parent_->get_lp_missed_poll_count(); }
  const char *sensor_name_() const override { return "Missed Polls"; }
};

class HCP2BridgeTxAbortSensor : public HCP2BridgeDiagnosticSensorBase {
 protected:
  uint32_t read_value_() const override { return this->parent_->get_lp_tx_abort_count(); }
  const char *sensor_name_() const override { return "TX Aborts"; }
};

class HCP2BridgeCollisionSensor : public HCP2BridgeDiagnosticSensorBase {
 protected:
  uint32_t read_value_() const override { return this->parent_->get_lp_collision_count(); }
  const char *sensor_name_() const override { return "Collisions"; }
};

class HCP2BridgeMaxDEHoldSensor : public HCP2BridgeDiagnosticSensorBase {
 protected:
  uint32_t read_value_() const override { return this->parent_->get_lp_max_de_hold_us(); }
  const char *sensor_name_() const override { return "Max DE Hold"; }
};

class HCP2BridgeLastPollAgeSensor : public HCP2BridgeDiagnosticSensorBase {
 protected:
  uint32_t read_value_() const override { return this->parent_->get_lp_last_poll_age_ms(); }
  const char *sensor_name_() const override { return "Last Poll Age"; }
};

class HCP2BridgeCRCErrorSensor : public HCP2BridgeDiagnosticSensorBase {
 protected:
  uint32_t read_value_() const override { return this->parent_->get_lp_crc_error_count(); }
  const char *sensor_name_() const override { return "CRC Errors"; }
};

class HCP2BridgeRXErrorSensor : public HCP2BridgeDiagnosticSensorBase {
 protected:
  uint32_t read_value_() const override { return this->parent_->get_lp_rx_error_count(); }
  const char *sensor_name_() const override { return "RX Errors"; }
};

class HCP2BridgeStopTriggerFireSensor : public HCP2BridgeDiagnosticSensorBase {
 protected:
  uint32_t read_value_() const override { return this->parent_->get_lp_stop_trigger_fire_count(); }
  const char *sensor_name_() const override { return "Stop Trigger Fires"; }
};

class HCP2BridgeHPResetCountSensor : public HCP2BridgeDiagnosticSensorBase {
 protected:
  uint32_t read_value_() const override { return this->parent_->get_hp_reset_count(); }
  const char *sensor_name_() const override { return "HP Reset Count"; }
};

class HCP2BridgeHPPanicResetSensor : public HCP2BridgeDiagnosticSensorBase {
 protected:
  uint32_t read_value_() const override { return this->parent_->get_hp_panic_reset_count(); }
  const char *sensor_name_() const override { return "HP Panic Resets"; }
};

class HCP2BridgeHPWDTResetSensor : public HCP2BridgeDiagnosticSensorBase {
 protected:
  uint32_t read_value_() const override { return this->parent_->get_hp_wdt_reset_count(); }
  const char *sensor_name_() const override { return "HP WDT Resets"; }
};

class HCP2BridgeHPBrownoutResetSensor : public HCP2BridgeDiagnosticSensorBase {
 protected:
  uint32_t read_value_() const override { return this->parent_->get_hp_brownout_reset_count(); }
  const char *sensor_name_() const override { return "HP Brownout Resets"; }
};

}  // namespace hcp2bridge
}  // namespace esphome
