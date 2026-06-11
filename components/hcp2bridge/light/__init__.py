from esphome.components import light
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_OUTPUT_ID
from .. import CONF_HCP2BRIDGE_ID, HCP2Bridge, hcp2bridge_ns

DEPENDENCIES = ["hcp2bridge"]

HCP2BridgeLight = hcp2bridge_ns.class_("HCP2BridgeLight", light.LightOutput, cg.Component)

CONFIG_SCHEMA = (
    light.BINARY_LIGHT_SCHEMA.extend(
        {
            cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(HCP2BridgeLight),
            cv.GenerateID(CONF_HCP2BRIDGE_ID): cv.use_id(HCP2Bridge),
        }
    )
    .extend(cv.ENTITY_BASE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await cg.register_component(var, config)
    await light.register_light(var, config)

    parent = await cg.get_variable(config[CONF_HCP2BRIDGE_ID])
    cg.add(var.set_hcp2bridge_parent(parent))
