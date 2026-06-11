from esphome.components import cover
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from .. import CONF_HCP2BRIDGE_ID, HCP2Bridge, hcp2bridge_ns

DEPENDENCIES = ["hcp2bridge"]

HCP2BridgeCover = hcp2bridge_ns.class_("HCP2BridgeCover", cover.Cover, cg.Component)

CONFIG_SCHEMA = cover.cover_schema(HCP2BridgeCover).extend(
    {
        cv.GenerateID(): cv.declare_id(HCP2BridgeCover),
        cv.GenerateID(CONF_HCP2BRIDGE_ID): cv.use_id(HCP2Bridge),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cover.register_cover(var, config)

    parent = await cg.get_variable(config[CONF_HCP2BRIDGE_ID])
    cg.add(var.set_hcp2bridge_parent(parent))
