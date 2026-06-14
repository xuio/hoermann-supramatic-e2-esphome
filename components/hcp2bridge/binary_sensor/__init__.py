from esphome.components import binary_sensor
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_LIGHT,
    DEVICE_CLASS_MOVING,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from .. import CONF_HCP2BRIDGE_ID, HCP2Bridge, hcp2bridge_ns

DEPENDENCIES = ["hcp2bridge"]

HCP2BridgeValidBroadcastSensor = hcp2bridge_ns.class_(
    "HCP2BridgeValidBroadcastSensor", binary_sensor.BinarySensor, cg.Component
)
HCP2BridgeMovingSensor = hcp2bridge_ns.class_("HCP2BridgeMovingSensor", binary_sensor.BinarySensor, cg.Component)
HCP2BridgeOpenSensor = hcp2bridge_ns.class_("HCP2BridgeOpenSensor", binary_sensor.BinarySensor, cg.Component)
HCP2BridgeClosedSensor = hcp2bridge_ns.class_("HCP2BridgeClosedSensor", binary_sensor.BinarySensor, cg.Component)
HCP2BridgeLightSensor = hcp2bridge_ns.class_("HCP2BridgeLightSensor", binary_sensor.BinarySensor, cg.Component)
HCP2BridgeObstructionSensor = hcp2bridge_ns.class_(
    "HCP2BridgeObstructionSensor", binary_sensor.BinarySensor, cg.Component
)
HCP2BridgeBusOnlineSensor = hcp2bridge_ns.class_(
    "HCP2BridgeBusOnlineSensor", binary_sensor.BinarySensor, cg.Component
)
HCP2BridgeContinuityHealthySensor = hcp2bridge_ns.class_(
    "HCP2BridgeContinuityHealthySensor", binary_sensor.BinarySensor, cg.Component
)
HCP2BridgeSafeForOTARestartSensor = hcp2bridge_ns.class_(
    "HCP2BridgeSafeForOTARestartSensor", binary_sensor.BinarySensor, cg.Component
)
HCP2BridgeContinuityProblemSensor = hcp2bridge_ns.class_(
    "HCP2BridgeContinuityProblemSensor", binary_sensor.BinarySensor, cg.Component
)

CONF_BUS_ONLINE = "bus_online"
CONF_CLOSED = "closed"
CONF_CONTINUITY_HEALTHY = "continuity_healthy"
CONF_CONTINUITY_PROBLEM = "continuity_problem"
CONF_GOT_VALID_BROADCAST = "got_valid_broadcast"
CONF_LIGHT = "light"
CONF_MOVING = "moving"
CONF_OBSTRUCTION = "obstruction"
CONF_OPEN = "open"
CONF_SAFE_FOR_OTA_RESTART = "safe_for_ota_restart"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HCP2BRIDGE_ID): cv.use_id(HCP2Bridge),
        cv.Optional(CONF_GOT_VALID_BROADCAST): binary_sensor.binary_sensor_schema(
            HCP2BridgeValidBroadcastSensor,
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_MOVING): binary_sensor.binary_sensor_schema(
            HCP2BridgeMovingSensor,
            device_class=DEVICE_CLASS_MOVING,
        ),
        cv.Optional(CONF_OPEN): binary_sensor.binary_sensor_schema(HCP2BridgeOpenSensor),
        cv.Optional(CONF_CLOSED): binary_sensor.binary_sensor_schema(HCP2BridgeClosedSensor),
        cv.Optional(CONF_LIGHT): binary_sensor.binary_sensor_schema(
            HCP2BridgeLightSensor,
            device_class=DEVICE_CLASS_LIGHT,
        ),
        cv.Optional(CONF_OBSTRUCTION): binary_sensor.binary_sensor_schema(
            HCP2BridgeObstructionSensor,
            device_class=DEVICE_CLASS_PROBLEM,
        ),
        cv.Optional(CONF_BUS_ONLINE): binary_sensor.binary_sensor_schema(
            HCP2BridgeBusOnlineSensor,
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_CONTINUITY_HEALTHY): binary_sensor.binary_sensor_schema(
            HCP2BridgeContinuityHealthySensor,
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_SAFE_FOR_OTA_RESTART): binary_sensor.binary_sensor_schema(
            HCP2BridgeSafeForOTARestartSensor,
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_CONTINUITY_PROBLEM): binary_sensor.binary_sensor_schema(
            HCP2BridgeContinuityProblemSensor,
            device_class=DEVICE_CLASS_PROBLEM,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def _new_sensor(config, key, parent):
    if conf := config.get(key):
        var = await binary_sensor.new_binary_sensor(conf)
        await cg.register_component(var, conf)
        cg.add(var.set_hcp2bridge_parent(parent))


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HCP2BRIDGE_ID])
    await _new_sensor(config, CONF_GOT_VALID_BROADCAST, parent)
    await _new_sensor(config, CONF_MOVING, parent)
    await _new_sensor(config, CONF_OPEN, parent)
    await _new_sensor(config, CONF_CLOSED, parent)
    await _new_sensor(config, CONF_LIGHT, parent)
    await _new_sensor(config, CONF_OBSTRUCTION, parent)
    await _new_sensor(config, CONF_BUS_ONLINE, parent)
    await _new_sensor(config, CONF_CONTINUITY_HEALTHY, parent)
    await _new_sensor(config, CONF_SAFE_FOR_OTA_RESTART, parent)
    await _new_sensor(config, CONF_CONTINUITY_PROBLEM, parent)
