import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_ID, CONF_TYPE
from . import evbox_max_ns, EvboxMaxComponent

CONF_EVBOX_MAX_ID = "evbox_max_id"

EvboxCurrentNumber = evbox_max_ns.class_("EvboxCurrentNumber", number.Number, cg.Component)
EvboxNumberType = evbox_max_ns.enum("EvboxNumberType")

NUMBER_TYPES = {
    "MANUAL_CURRENT": EvboxNumberType.EVBOX_NUMBER_MANUAL_CURRENT,
    "MAX_CURRENT": EvboxNumberType.EVBOX_NUMBER_MAX_CURRENT,
    "FAILSAFE_CURRENT": EvboxNumberType.EVBOX_NUMBER_FAILSAFE_CURRENT,
    "CHARGER_BREAKER_CURRENT": EvboxNumberType.EVBOX_NUMBER_CHARGER_BREAKER_CURRENT,
    "MAIN_FUSE_CURRENT": EvboxNumberType.EVBOX_NUMBER_MAIN_FUSE_CURRENT,
}

CONFIG_SCHEMA = number.number_schema(
    EvboxCurrentNumber,
    unit_of_measurement="A",
).extend(
    {
        cv.GenerateID(CONF_EVBOX_MAX_ID): cv.use_id(EvboxMaxComponent),
        cv.Required(CONF_TYPE): cv.enum(NUMBER_TYPES, upper=True),
        cv.Optional("min_value", default=0): cv.float_,
        cv.Optional("max_value", default=32): cv.float_,
        cv.Optional("step", default=1): cv.float_,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = await number.new_number(
        config,
        min_value=config["min_value"],
        max_value=config["max_value"],
        step=config["step"],
    )
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_EVBOX_MAX_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_type(config[CONF_TYPE]))
