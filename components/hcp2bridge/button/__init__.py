import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import ICON_LIGHTBULB

from .. import CONF_HCP2BRIDGE_ID, HCP2Bridge, hcp2bridge_ns

DEPENDENCIES = ["hcp2bridge"]

HCP2BridgeOpenButton = hcp2bridge_ns.class_("HCP2BridgeOpenButton", button.Button, cg.Component)
HCP2BridgeCloseButton = hcp2bridge_ns.class_("HCP2BridgeCloseButton", button.Button, cg.Component)
HCP2BridgeStopButton = hcp2bridge_ns.class_("HCP2BridgeStopButton", button.Button, cg.Component)
HCP2BridgeHalfButton = hcp2bridge_ns.class_("HCP2BridgeHalfButton", button.Button, cg.Component)
HCP2BridgeVentButton = hcp2bridge_ns.class_("HCP2BridgeVentButton", button.Button, cg.Component)
HCP2BridgeLightButton = hcp2bridge_ns.class_("HCP2BridgeLightButton", button.Button, cg.Component)

CONF_OPEN = "open"
CONF_CLOSE = "close"
CONF_STOP = "stop"
CONF_HALF = "half"
CONF_VENT = "vent"
CONF_LIGHT = "light"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HCP2BRIDGE_ID): cv.use_id(HCP2Bridge),
        cv.Optional(CONF_OPEN): button.button_schema(HCP2BridgeOpenButton, icon="mdi:garage-open"),
        cv.Optional(CONF_CLOSE): button.button_schema(HCP2BridgeCloseButton, icon="mdi:garage"),
        cv.Optional(CONF_STOP): button.button_schema(HCP2BridgeStopButton, icon="mdi:stop"),
        cv.Optional(CONF_HALF): button.button_schema(HCP2BridgeHalfButton, icon="mdi:garage-alert"),
        cv.Optional(CONF_VENT): button.button_schema(HCP2BridgeVentButton, icon="mdi:fan"),
        cv.Optional(CONF_LIGHT): button.button_schema(HCP2BridgeLightButton, icon=ICON_LIGHTBULB),
    }
).extend(cv.COMPONENT_SCHEMA)


async def _new_button(config, key, parent):
    if conf := config.get(key):
        var = await button.new_button(conf)
        await cg.register_component(var, conf)
        cg.add(var.set_hcp2bridge_parent(parent))


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HCP2BRIDGE_ID])
    await _new_button(config, CONF_OPEN, parent)
    await _new_button(config, CONF_CLOSE, parent)
    await _new_button(config, CONF_STOP, parent)
    await _new_button(config, CONF_HALF, parent)
    await _new_button(config, CONF_VENT, parent)
    await _new_button(config, CONF_LIGHT, parent)
