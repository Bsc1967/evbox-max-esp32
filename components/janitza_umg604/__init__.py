import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, text_sensor
from esphome.const import CONF_ID, CONF_IP_ADDRESS, CONF_PORT

CODEOWNERS = ["@Bsc1967"]
AUTO_LOAD = ["sensor", "text_sensor"]

CONF_UNIT_ID = "unit_id"
CONF_POLL_INTERVAL = "poll_interval"
CONF_REGISTERS = "registers"
CONF_L1_CURRENT = "l1_current"
CONF_L2_CURRENT = "l2_current"
CONF_L3_CURRENT = "l3_current"
CONF_L1_VOLTAGE = "l1_voltage"
CONF_L2_VOLTAGE = "l2_voltage"
CONF_L3_VOLTAGE = "l3_voltage"
CONF_TOTAL_POWER = "total_power"
CONF_IMPORT_POWER = "import_power"
CONF_EXPORT_POWER = "export_power"
CONF_COMMUNICATION_STATUS = "communication_status"

janitza_ns = cg.esphome_ns.namespace("janitza_umg604")
JanitzaUmg604Component = janitza_ns.class_("JanitzaUmg604Component", cg.PollingComponent)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(JanitzaUmg604Component),
        cv.Required(CONF_IP_ADDRESS): cv.ipv4address,
        cv.Optional(CONF_PORT, default=502): cv.port,
        cv.Optional(CONF_UNIT_ID, default=1): cv.int_range(min=1, max=247),
        cv.Optional(CONF_POLL_INTERVAL, default="5s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_REGISTERS, default={}): cv.Schema(
            {
                cv.Optional(CONF_L1_CURRENT, default=1325): cv.positive_int,
                cv.Optional(CONF_L2_CURRENT, default=1327): cv.positive_int,
                cv.Optional(CONF_L3_CURRENT, default=1329): cv.positive_int,
                cv.Optional(CONF_L1_VOLTAGE, default=1317): cv.positive_int,
                cv.Optional(CONF_L2_VOLTAGE, default=1319): cv.positive_int,
                cv.Optional(CONF_L3_VOLTAGE, default=1321): cv.positive_int,
                cv.Optional(CONF_TOTAL_POWER, default=1369): cv.positive_int,
                cv.Optional(CONF_IMPORT_POWER, default=1369): cv.positive_int,
                cv.Optional(CONF_EXPORT_POWER, default=1369): cv.positive_int,
            }
        ),
        cv.Optional(CONF_L1_CURRENT): sensor.sensor_schema(
            unit_of_measurement="A",
            accuracy_decimals=2,
            device_class="current",
            state_class="measurement",
        ),
        cv.Optional(CONF_L2_CURRENT): sensor.sensor_schema(
            unit_of_measurement="A",
            accuracy_decimals=2,
            device_class="current",
            state_class="measurement",
        ),
        cv.Optional(CONF_L3_CURRENT): sensor.sensor_schema(
            unit_of_measurement="A",
            accuracy_decimals=2,
            device_class="current",
            state_class="measurement",
        ),
        cv.Optional(CONF_L1_VOLTAGE): sensor.sensor_schema(
            unit_of_measurement="V",
            accuracy_decimals=1,
            device_class="voltage",
            state_class="measurement",
        ),
        cv.Optional(CONF_L2_VOLTAGE): sensor.sensor_schema(
            unit_of_measurement="V",
            accuracy_decimals=1,
            device_class="voltage",
            state_class="measurement",
        ),
        cv.Optional(CONF_L3_VOLTAGE): sensor.sensor_schema(
            unit_of_measurement="V",
            accuracy_decimals=1,
            device_class="voltage",
            state_class="measurement",
        ),
        cv.Optional(CONF_TOTAL_POWER): sensor.sensor_schema(
            unit_of_measurement="W",
            accuracy_decimals=0,
            device_class="power",
            state_class="measurement",
        ),
        cv.Optional(CONF_IMPORT_POWER): sensor.sensor_schema(
            unit_of_measurement="W",
            accuracy_decimals=0,
            device_class="power",
            state_class="measurement",
        ),
        cv.Optional(CONF_EXPORT_POWER): sensor.sensor_schema(
            unit_of_measurement="W",
            accuracy_decimals=0,
            device_class="power",
            state_class="measurement",
        ),
        cv.Optional(CONF_COMMUNICATION_STATUS): text_sensor.text_sensor_schema(),
    }
).extend(cv.polling_component_schema("5s"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_host(str(config[CONF_IP_ADDRESS])))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_unit_id(config[CONF_UNIT_ID]))
    cg.add(var.set_poll_interval_ms(config[CONF_POLL_INTERVAL]))

    registers = config[CONF_REGISTERS]
    cg.add(var.set_register_l1_current(registers[CONF_L1_CURRENT]))
    cg.add(var.set_register_l2_current(registers[CONF_L2_CURRENT]))
    cg.add(var.set_register_l3_current(registers[CONF_L3_CURRENT]))
    cg.add(var.set_register_l1_voltage(registers[CONF_L1_VOLTAGE]))
    cg.add(var.set_register_l2_voltage(registers[CONF_L2_VOLTAGE]))
    cg.add(var.set_register_l3_voltage(registers[CONF_L3_VOLTAGE]))
    cg.add(var.set_register_total_power(registers[CONF_TOTAL_POWER]))
    cg.add(var.set_register_import_power(registers[CONF_IMPORT_POWER]))
    cg.add(var.set_register_export_power(registers[CONF_EXPORT_POWER]))

    for key, setter in [
        (CONF_L1_CURRENT, var.set_l1_current_sensor),
        (CONF_L2_CURRENT, var.set_l2_current_sensor),
        (CONF_L3_CURRENT, var.set_l3_current_sensor),
        (CONF_L1_VOLTAGE, var.set_l1_voltage_sensor),
        (CONF_L2_VOLTAGE, var.set_l2_voltage_sensor),
        (CONF_L3_VOLTAGE, var.set_l3_voltage_sensor),
        (CONF_TOTAL_POWER, var.set_total_power_sensor),
        (CONF_IMPORT_POWER, var.set_import_power_sensor),
        (CONF_EXPORT_POWER, var.set_export_power_sensor),
    ]:
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(setter(sens))

    if CONF_COMMUNICATION_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_COMMUNICATION_STATUS])
        cg.add(var.set_communication_text_sensor(sens))
