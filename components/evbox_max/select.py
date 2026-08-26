import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import CONF_ID, CONF_TYPE
from . import evbox_max_ns, EvboxMaxComponent

CONF_EVBOX_MAX_ID = "evbox_max_id"

EvboxModeSelect = evbox_max_ns.class_("EvboxModeSelect", select.Select, cg.Component)
EvboxSelectType = evbox_max_ns.enum("EvboxSelectType")

SELECT_TYPES = {
    "MODE": EvboxSelectType.EVBOX_SELECT_MODE,
    "FAILSAFE_MODE": EvboxSelectType.EVBOX_SELECT_FAILSAFE_MODE,
    "EVBOX_L1_GRID_PHASE": EvboxSelectType.EVBOX_SELECT_EVBOX_L1_GRID_PHASE,
    "EVBOX_L2_GRID_PHASE": EvboxSelectType.EVBOX_SELECT_EVBOX_L2_GRID_PHASE,
    "EVBOX_L3_GRID_PHASE": EvboxSelectType.EVBOX_SELECT_EVBOX_L3_GRID_PHASE,
}

CONFIG_SCHEMA = select.select_schema(EvboxModeSelect).extend(
    {
        cv.GenerateID(CONF_EVBOX_MAX_ID): cv.use_id(EvboxMaxComponent),
        cv.Required(CONF_TYPE): cv.one_of(*SELECT_TYPES.keys(), upper=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    select_type = config[CONF_TYPE]
    if select_type == "MODE":
        options = ["MANUAL", "LOAD_BALANCING", "PV_SURPLUS", "DISABLED"]
    elif select_type == "FAILSAFE_MODE":
        options = ["STOP", "LIMIT_6A"]
    else:
        options = ["L1", "L2", "L3"]
    var = await select.new_select(config, options=options)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_EVBOX_MAX_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_type(SELECT_TYPES[select_type]))
