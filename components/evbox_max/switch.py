import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID, CONF_TYPE
from . import evbox_max_ns, EvboxMaxComponent

CONF_EVBOX_MAX_ID = "evbox_max_id"

EvboxCommandSwitch = evbox_max_ns.class_("EvboxCommandSwitch", switch.Switch, cg.Component)
EvboxSwitchType = evbox_max_ns.enum("EvboxSwitchType")

SWITCH_TYPES = {
    "START": EvboxSwitchType.EVBOX_SWITCH_START,
    "STOP": EvboxSwitchType.EVBOX_SWITCH_STOP,
    "ENABLE_PV_MODE": EvboxSwitchType.EVBOX_SWITCH_ENABLE_PV_MODE,
    "PAUSE": EvboxSwitchType.EVBOX_SWITCH_PAUSE,
}

CONFIG_SCHEMA = switch.switch_schema(EvboxCommandSwitch).extend(
    {
        cv.GenerateID(CONF_EVBOX_MAX_ID): cv.use_id(EvboxMaxComponent),
        cv.Required(CONF_TYPE): cv.enum(SWITCH_TYPES, upper=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_EVBOX_MAX_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_type(config[CONF_TYPE]))
