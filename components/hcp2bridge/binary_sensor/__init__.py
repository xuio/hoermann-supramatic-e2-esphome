from esphome.components import binary_sensor
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_LIGHT,
    DEVICE_CLASS_MOVING,
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

CONF_CLOSED = "closed"
CONF_GOT_VALID_BROADCAST = "got_valid_broadcast"
CONF_LIGHT = "light"
CONF_MOVING = "moving"
CONF_OPEN = "open"

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
