from esphome.components import cover
import esphome.config_validation as cv
import esphome.codegen as cg
from .. import uapbridge_ns, CONF_UAPBRIDGE_ID, UAPBridge
from esphome.const import (
  CONF_ID
)

DEPENDENCIES = ["uapbridge"]

CONF_CLOSE_DURATION = "close_duration"
CONF_LEARN_TRAVEL_DURATIONS = "learn_travel_durations"
CONF_OPEN_DURATION = "open_duration"
CONF_POSITION_DEADBAND = "position_deadband"
CONF_POSITION_PUBLISH_INTERVAL = "position_publish_interval"
CONF_TIME_BASED_POSITION = "time_based_position"
CONF_VENTING_POSITION = "venting_position"

UAPBridgeCover = uapbridge_ns.class_("UAPBridgeCover", cover.Cover, cg.Component)

def position_deadband(value):
    value = cv.percentage(value)
    if value > 0.2:
        raise cv.Invalid("position_deadband must be 20% or less")
    return value

CONFIG_SCHEMA = cv.All(
  cover.cover_schema(UAPBridgeCover).extend({
    cv.GenerateID(): cv.declare_id(UAPBridgeCover),
    cv.GenerateID(CONF_UAPBRIDGE_ID): cv.use_id(UAPBridge),
    cv.Optional(CONF_TIME_BASED_POSITION, default=False): cv.boolean,
    cv.Optional(CONF_OPEN_DURATION, default="18s"): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_CLOSE_DURATION, default="18s"): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_POSITION_PUBLISH_INTERVAL, default="1s"): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_POSITION_DEADBAND, default="2%"): position_deadband,
    cv.Optional(CONF_VENTING_POSITION, default="20%"): cv.percentage,
    cv.Optional(CONF_LEARN_TRAVEL_DURATIONS, default=True): cv.boolean,
  }),
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cover.register_cover(var, config)
    parent = await cg.get_variable(config[CONF_UAPBRIDGE_ID])
    cg.add(var.set_uapbridge_parent(parent))
    cg.add(var.set_time_based_position(config[CONF_TIME_BASED_POSITION]))
    cg.add(var.set_open_duration(config[CONF_OPEN_DURATION].total_milliseconds))
    cg.add(var.set_close_duration(config[CONF_CLOSE_DURATION].total_milliseconds))
    cg.add(var.set_position_publish_interval(config[CONF_POSITION_PUBLISH_INTERVAL].total_milliseconds))
    cg.add(var.set_position_deadband(config[CONF_POSITION_DEADBAND]))
    cg.add(var.set_venting_position(config[CONF_VENTING_POSITION]))
    cg.add(var.set_learn_travel_durations(config[CONF_LEARN_TRAVEL_DURATIONS]))
