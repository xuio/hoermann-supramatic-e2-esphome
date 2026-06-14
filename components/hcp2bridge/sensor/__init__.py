from esphome.components import sensor
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    ENTITY_CATEGORY_DIAGNOSTIC,
    ICON_CHIP,
    ICON_COUNTER,
    ICON_TIMER,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_EMPTY,
)

from .. import CONF_HCP2BRIDGE_ID, HCP2Bridge, hcp2bridge_ns

DEPENDENCIES = ["hcp2bridge"]

HCP2BridgeLPHeartbeatSensor = hcp2bridge_ns.class_(
    "HCP2BridgeLPHeartbeatSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgeLPResetCountSensor = hcp2bridge_ns.class_(
    "HCP2BridgeLPResetCountSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgePollsSeenSensor = hcp2bridge_ns.class_(
    "HCP2BridgePollsSeenSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgePollsAnsweredSensor = hcp2bridge_ns.class_(
    "HCP2BridgePollsAnsweredSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgeMissedPollsSensor = hcp2bridge_ns.class_(
    "HCP2BridgeMissedPollsSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgeTxAbortSensor = hcp2bridge_ns.class_("HCP2BridgeTxAbortSensor", sensor.Sensor, cg.PollingComponent)
HCP2BridgeCollisionSensor = hcp2bridge_ns.class_("HCP2BridgeCollisionSensor", sensor.Sensor, cg.PollingComponent)
HCP2BridgeMaxDEHoldSensor = hcp2bridge_ns.class_("HCP2BridgeMaxDEHoldSensor", sensor.Sensor, cg.PollingComponent)
HCP2BridgeLastPollAgeSensor = hcp2bridge_ns.class_(
    "HCP2BridgeLastPollAgeSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgeCRCErrorSensor = hcp2bridge_ns.class_("HCP2BridgeCRCErrorSensor", sensor.Sensor, cg.PollingComponent)
HCP2BridgeRXErrorSensor = hcp2bridge_ns.class_("HCP2BridgeRXErrorSensor", sensor.Sensor, cg.PollingComponent)
HCP2BridgeStopTriggerFireSensor = hcp2bridge_ns.class_(
    "HCP2BridgeStopTriggerFireSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgeHealthFlagsSensor = hcp2bridge_ns.class_(
    "HCP2BridgeHealthFlagsSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgeMaxRXFifoSensor = hcp2bridge_ns.class_("HCP2BridgeMaxRXFifoSensor", sensor.Sensor, cg.PollingComponent)
HCP2BridgeMaxLoopSensor = hcp2bridge_ns.class_("HCP2BridgeMaxLoopSensor", sensor.Sensor, cg.PollingComponent)
HCP2BridgeMaxPollRXToScheduleSensor = hcp2bridge_ns.class_(
    "HCP2BridgeMaxPollRXToScheduleSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgeMaxResponseScheduleToTXStartSensor = hcp2bridge_ns.class_(
    "HCP2BridgeMaxResponseScheduleToTXStartSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgeMaxResponseTXSensor = hcp2bridge_ns.class_(
    "HCP2BridgeMaxResponseTXSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgeLoopOverrunSensor = hcp2bridge_ns.class_(
    "HCP2BridgeLoopOverrunSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgeRXStarvationSensor = hcp2bridge_ns.class_(
    "HCP2BridgeRXStarvationSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgeStuckDESensor = hcp2bridge_ns.class_("HCP2BridgeStuckDESensor", sensor.Sensor, cg.PollingComponent)
HCP2BridgeMailboxRepairSensor = hcp2bridge_ns.class_(
    "HCP2BridgeMailboxRepairSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgeHPResetCountSensor = hcp2bridge_ns.class_(
    "HCP2BridgeHPResetCountSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgeHPPanicResetSensor = hcp2bridge_ns.class_(
    "HCP2BridgeHPPanicResetSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgeHPWDTResetSensor = hcp2bridge_ns.class_(
    "HCP2BridgeHPWDTResetSensor", sensor.Sensor, cg.PollingComponent
)
HCP2BridgeHPBrownoutResetSensor = hcp2bridge_ns.class_(
    "HCP2BridgeHPBrownoutResetSensor", sensor.Sensor, cg.PollingComponent
)

CONF_COLLISIONS = "collisions"
CONF_CRC_ERRORS = "crc_errors"
CONF_HP_BROWNOUT_RESETS = "hp_brownout_resets"
CONF_HP_PANIC_RESETS = "hp_panic_resets"
CONF_HP_RESETS = "hp_resets"
CONF_HP_WDT_RESETS = "hp_wdt_resets"
CONF_LAST_POLL_AGE = "last_poll_age"
CONF_LOOP_OVERRUNS = "loop_overruns"
CONF_LP_HEARTBEAT = "lp_heartbeat"
CONF_LP_HEALTH_FLAGS = "lp_health_flags"
CONF_LP_RESETS = "lp_resets"
CONF_MAILBOX_REPAIRS = "mailbox_repairs"
CONF_MAX_DE_HOLD = "max_de_hold"
CONF_MAX_LOOP = "max_loop"
CONF_MAX_POLL_RX_TO_SCHEDULE = "max_poll_rx_to_schedule"
CONF_MAX_RESPONSE_SCHEDULE_TO_TX_START = "max_response_schedule_to_tx_start"
CONF_MAX_RESPONSE_TX = "max_response_tx"
CONF_MAX_RX_FIFO = "max_rx_fifo"
CONF_MISSED_POLLS = "missed_polls"
CONF_POLLS_ANSWERED = "polls_answered"
CONF_POLLS_SEEN = "polls_seen"
CONF_RX_ERRORS = "rx_errors"
CONF_RX_STARVATIONS = "rx_starvations"
CONF_STOP_TRIGGER_FIRES = "stop_trigger_fires"
CONF_STUCK_DE_RECOVERIES = "stuck_de_recoveries"
CONF_TX_ABORTS = "tx_aborts"


def counter_schema(sensor_class, icon=ICON_COUNTER):
    return sensor.sensor_schema(
        sensor_class,
        unit_of_measurement=UNIT_EMPTY,
        accuracy_decimals=0,
        state_class=STATE_CLASS_TOTAL_INCREASING,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        icon=icon,
    ).extend(cv.polling_component_schema("10s"))


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HCP2BRIDGE_ID): cv.use_id(HCP2Bridge),
        cv.Optional(CONF_LP_HEARTBEAT): counter_schema(HCP2BridgeLPHeartbeatSensor, ICON_CHIP),
        cv.Optional(CONF_LP_RESETS): counter_schema(HCP2BridgeLPResetCountSensor, ICON_CHIP),
        cv.Optional(CONF_POLLS_SEEN): counter_schema(HCP2BridgePollsSeenSensor),
        cv.Optional(CONF_POLLS_ANSWERED): counter_schema(HCP2BridgePollsAnsweredSensor),
        cv.Optional(CONF_MISSED_POLLS): counter_schema(HCP2BridgeMissedPollsSensor),
        cv.Optional(CONF_TX_ABORTS): counter_schema(HCP2BridgeTxAbortSensor),
        cv.Optional(CONF_COLLISIONS): counter_schema(HCP2BridgeCollisionSensor),
        cv.Optional(CONF_CRC_ERRORS): counter_schema(HCP2BridgeCRCErrorSensor),
        cv.Optional(CONF_RX_ERRORS): counter_schema(HCP2BridgeRXErrorSensor),
        cv.Optional(CONF_STOP_TRIGGER_FIRES): counter_schema(HCP2BridgeStopTriggerFireSensor),
        cv.Optional(CONF_LP_HEALTH_FLAGS): sensor.sensor_schema(
            HCP2BridgeHealthFlagsSensor,
            unit_of_measurement=UNIT_EMPTY,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:alert-outline",
        ).extend(cv.polling_component_schema("10s")),
        cv.Optional(CONF_MAX_RX_FIFO): sensor.sensor_schema(
            HCP2BridgeMaxRXFifoSensor,
            unit_of_measurement=UNIT_EMPTY,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon=ICON_COUNTER,
        ).extend(cv.polling_component_schema("10s")),
        cv.Optional(CONF_MAX_LOOP): sensor.sensor_schema(
            HCP2BridgeMaxLoopSensor,
            unit_of_measurement="us",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon=ICON_TIMER,
        ).extend(cv.polling_component_schema("10s")),
        cv.Optional(CONF_MAX_POLL_RX_TO_SCHEDULE): sensor.sensor_schema(
            HCP2BridgeMaxPollRXToScheduleSensor,
            unit_of_measurement="us",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon=ICON_TIMER,
        ).extend(cv.polling_component_schema("10s")),
        cv.Optional(CONF_MAX_RESPONSE_SCHEDULE_TO_TX_START): sensor.sensor_schema(
            HCP2BridgeMaxResponseScheduleToTXStartSensor,
            unit_of_measurement="us",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon=ICON_TIMER,
        ).extend(cv.polling_component_schema("10s")),
        cv.Optional(CONF_MAX_RESPONSE_TX): sensor.sensor_schema(
            HCP2BridgeMaxResponseTXSensor,
            unit_of_measurement="us",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon=ICON_TIMER,
        ).extend(cv.polling_component_schema("10s")),
        cv.Optional(CONF_LOOP_OVERRUNS): counter_schema(HCP2BridgeLoopOverrunSensor),
        cv.Optional(CONF_RX_STARVATIONS): counter_schema(HCP2BridgeRXStarvationSensor),
        cv.Optional(CONF_STUCK_DE_RECOVERIES): counter_schema(HCP2BridgeStuckDESensor),
        cv.Optional(CONF_MAILBOX_REPAIRS): counter_schema(HCP2BridgeMailboxRepairSensor, ICON_CHIP),
        cv.Optional(CONF_HP_RESETS): counter_schema(HCP2BridgeHPResetCountSensor, ICON_CHIP),
        cv.Optional(CONF_HP_PANIC_RESETS): counter_schema(HCP2BridgeHPPanicResetSensor, ICON_CHIP),
        cv.Optional(CONF_HP_WDT_RESETS): counter_schema(HCP2BridgeHPWDTResetSensor, ICON_CHIP),
        cv.Optional(CONF_HP_BROWNOUT_RESETS): counter_schema(HCP2BridgeHPBrownoutResetSensor, ICON_CHIP),
        cv.Optional(CONF_MAX_DE_HOLD): sensor.sensor_schema(
            HCP2BridgeMaxDEHoldSensor,
            unit_of_measurement="us",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon=ICON_TIMER,
        ).extend(cv.polling_component_schema("10s")),
        cv.Optional(CONF_LAST_POLL_AGE): sensor.sensor_schema(
            HCP2BridgeLastPollAgeSensor,
            unit_of_measurement="ms",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon=ICON_TIMER,
        ).extend(cv.polling_component_schema("10s")),
    }
)


async def _new_sensor(config, key, parent):
    if conf := config.get(key):
        var = await sensor.new_sensor(conf)
        await cg.register_component(var, conf)
        cg.add(var.set_hcp2bridge_parent(parent))


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HCP2BRIDGE_ID])
    await _new_sensor(config, CONF_LP_HEARTBEAT, parent)
    await _new_sensor(config, CONF_LP_RESETS, parent)
    await _new_sensor(config, CONF_POLLS_SEEN, parent)
    await _new_sensor(config, CONF_POLLS_ANSWERED, parent)
    await _new_sensor(config, CONF_MISSED_POLLS, parent)
    await _new_sensor(config, CONF_TX_ABORTS, parent)
    await _new_sensor(config, CONF_COLLISIONS, parent)
    await _new_sensor(config, CONF_CRC_ERRORS, parent)
    await _new_sensor(config, CONF_RX_ERRORS, parent)
    await _new_sensor(config, CONF_STOP_TRIGGER_FIRES, parent)
    await _new_sensor(config, CONF_LP_HEALTH_FLAGS, parent)
    await _new_sensor(config, CONF_MAX_RX_FIFO, parent)
    await _new_sensor(config, CONF_MAX_LOOP, parent)
    await _new_sensor(config, CONF_MAX_POLL_RX_TO_SCHEDULE, parent)
    await _new_sensor(config, CONF_MAX_RESPONSE_SCHEDULE_TO_TX_START, parent)
    await _new_sensor(config, CONF_MAX_RESPONSE_TX, parent)
    await _new_sensor(config, CONF_LOOP_OVERRUNS, parent)
    await _new_sensor(config, CONF_RX_STARVATIONS, parent)
    await _new_sensor(config, CONF_STUCK_DE_RECOVERIES, parent)
    await _new_sensor(config, CONF_MAILBOX_REPAIRS, parent)
    await _new_sensor(config, CONF_HP_RESETS, parent)
    await _new_sensor(config, CONF_HP_PANIC_RESETS, parent)
    await _new_sensor(config, CONF_HP_WDT_RESETS, parent)
    await _new_sensor(config, CONF_HP_BROWNOUT_RESETS, parent)
    await _new_sensor(config, CONF_MAX_DE_HOLD, parent)
    await _new_sensor(config, CONF_LAST_POLL_AGE, parent)
