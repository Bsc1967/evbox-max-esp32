import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import uart, sensor, text_sensor
from esphome.const import (
    CONF_ID,
    CONF_MODE,
    CONF_TEMPERATURE,
    CONF_MAX_CURRENT,
)

CODEOWNERS = ["@Bsc1967"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "text_sensor", "number", "select", "switch"]

CONF_FAILSAFE_CURRENT = "failsafe_current"
CONF_FAILSAFE_MODE = "failsafe_mode"
CONF_HEARTBEAT_INTERVAL = "heartbeat_interval"
CONF_WATCHDOG_TIMEOUT = "watchdog_timeout"
CONF_MANUAL_CURRENT = "manual_current"
CONF_ENABLE_PV_MODE = "enable_pv_mode"
CONF_STATUS = "status"
CONF_STATE = "state"
CONF_EV_CURRENT = "ev_current"
CONF_CURRENT_LIMIT = "current_limit"
CONF_DESIRED_CURRENT = "desired_current"
CONF_COMMANDED_CURRENT = "commanded_current"
CONF_RETURNED_CURRENT_LIMIT = "returned_current_limit"
CONF_L1_CURRENT = "l1_current"
CONF_L2_CURRENT = "l2_current"
CONF_L3_CURRENT = "l3_current"
CONF_L1_VOLTAGE = "l1_voltage"
CONF_L2_VOLTAGE = "l2_voltage"
CONF_L3_VOLTAGE = "l3_voltage"
CONF_L1_POWER_FACTOR = "l1_power_factor"
CONF_L2_POWER_FACTOR = "l2_power_factor"
CONF_L3_POWER_FACTOR = "l3_power_factor"
CONF_DETECTED_CHARGE_PHASES = "detected_charge_phases"
CONF_ACTIVE_PHASE_MASK = "active_phase_mask"
CONF_POWER = "power"
CONF_SESSION_ENERGY = "session_energy"
CONF_METER_VALUE = "meter_value"
CONF_COMMUNICATION_STATUS = "communication_status"
CONF_PROTOCOL_PROFILE = "protocol_profile"
CONF_CB_SERIAL = "cb_serial"
CONF_CB_STATUS_DETAIL = "cb_status_detail"
CONF_CURRENT_REQUEST_STATE = "current_request_state"
CONF_CB_FIRMWARE = "cb_firmware"
CONF_CB_HARDWARE_GENERATION = "cb_hardware_generation"
CONF_COMMISSIONING_MODE = "commissioning_mode"
CONF_RS485_DE_PIN = "rs485_de_pin"
CONF_CHARGE_PHASES = "charge_phases"
CONF_CHARGER_BREAKER_CURRENT = "charger_breaker_current"
CONF_MAIN_FUSE_CURRENT = "main_fuse_current"
CONF_RELAY_EVBOX_KNOWN_PIN = "relay_evbox_known_pin"
CONF_RELAY_JANITZA_OK_PIN = "relay_janitza_ok_pin"
CONF_RELAY_CHARGING_ACTIVE_PIN = "relay_charging_active_pin"
CONF_RELAY_FAILSAFE_PIN = "relay_failsafe_pin"

evbox_max_ns = cg.esphome_ns.namespace("evbox_max")

EvboxMaxComponent = evbox_max_ns.class_(
    "EvboxMaxComponent",
    cg.Component,
    uart.UARTDevice,
)

ChargingMode = evbox_max_ns.enum("ChargingMode")
CHARGING_MODES = {
    "MANUAL": ChargingMode.CHARGING_MODE_MANUAL,
    "LOAD_BALANCING": ChargingMode.CHARGING_MODE_LOAD_BALANCING,
    "PV_SURPLUS": ChargingMode.CHARGING_MODE_PV_SURPLUS,
    "DISABLED": ChargingMode.CHARGING_MODE_DISABLED,
}

FailsafeMode = evbox_max_ns.enum("FailsafeMode")
FAILSAFE_MODES = {
    "STOP": FailsafeMode.FAILSAFE_MODE_STOP,
    "LIMIT_6A": FailsafeMode.FAILSAFE_MODE_LIMIT_6A,
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(EvboxMaxComponent),
        cv.Optional(CONF_MODE, default="MANUAL"): cv.enum(CHARGING_MODES, upper=True),
        cv.Optional(CONF_MAX_CURRENT, default=16): cv.float_range(min=0, max=32),
        cv.Optional(CONF_CHARGER_BREAKER_CURRENT, default=16): cv.float_range(min=0, max=32),
        cv.Optional(CONF_MAIN_FUSE_CURRENT, default=25): cv.float_range(min=0, max=80),
        cv.Optional(CONF_MANUAL_CURRENT, default=6): cv.float_range(min=0, max=32),
        cv.Optional(CONF_CHARGE_PHASES, default=1): cv.one_of(1, 2, 3, int=True),
        cv.Optional(CONF_FAILSAFE_CURRENT, default=6): cv.float_range(min=0, max=16),
        cv.Optional(CONF_FAILSAFE_MODE, default="LIMIT_6A"): cv.enum(
            FAILSAFE_MODES, upper=True
        ),
        cv.Optional(CONF_HEARTBEAT_INTERVAL, default="5s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_WATCHDOG_TIMEOUT, default="30s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_COMMISSIONING_MODE, default=True): cv.boolean,
        cv.Optional(CONF_RS485_DE_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_RELAY_EVBOX_KNOWN_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_RELAY_JANITZA_OK_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_RELAY_CHARGING_ACTIVE_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_RELAY_FAILSAFE_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_STATE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_COMMUNICATION_STATUS): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_PROTOCOL_PROFILE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_CB_SERIAL): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_CB_STATUS_DETAIL): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_CURRENT_REQUEST_STATE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_CB_FIRMWARE): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class="measurement",
        ),
        cv.Optional(CONF_CB_HARDWARE_GENERATION): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class="measurement",
        ),
        cv.Optional(CONF_EV_CURRENT): sensor.sensor_schema(
            unit_of_measurement="A",
            accuracy_decimals=1,
            device_class="current",
            state_class="measurement",
        ),
        cv.Optional(CONF_CURRENT_LIMIT): sensor.sensor_schema(
            unit_of_measurement="A",
            accuracy_decimals=1,
            device_class="current",
            state_class="measurement",
        ),
        cv.Optional(CONF_DESIRED_CURRENT): sensor.sensor_schema(
            unit_of_measurement="A",
            accuracy_decimals=1,
            device_class="current",
            state_class="measurement",
        ),
        cv.Optional(CONF_COMMANDED_CURRENT): sensor.sensor_schema(
            unit_of_measurement="A",
            accuracy_decimals=1,
            device_class="current",
            state_class="measurement",
        ),
        cv.Optional(CONF_RETURNED_CURRENT_LIMIT): sensor.sensor_schema(
            unit_of_measurement="A",
            accuracy_decimals=1,
            device_class="current",
            state_class="measurement",
        ),
        cv.Optional(CONF_L1_CURRENT): sensor.sensor_schema(
            unit_of_measurement="A",
            accuracy_decimals=1,
            device_class="current",
            state_class="measurement",
        ),
        cv.Optional(CONF_L2_CURRENT): sensor.sensor_schema(
            unit_of_measurement="A",
            accuracy_decimals=1,
            device_class="current",
            state_class="measurement",
        ),
        cv.Optional(CONF_L3_CURRENT): sensor.sensor_schema(
            unit_of_measurement="A",
            accuracy_decimals=1,
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
        cv.Optional(CONF_L1_POWER_FACTOR): sensor.sensor_schema(
            accuracy_decimals=3,
            state_class="measurement",
        ),
        cv.Optional(CONF_L2_POWER_FACTOR): sensor.sensor_schema(
            accuracy_decimals=3,
            state_class="measurement",
        ),
        cv.Optional(CONF_L3_POWER_FACTOR): sensor.sensor_schema(
            accuracy_decimals=3,
            state_class="measurement",
        ),
        cv.Optional(CONF_DETECTED_CHARGE_PHASES): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class="measurement",
        ),
        cv.Optional(CONF_ACTIVE_PHASE_MASK): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class="measurement",
        ),
        cv.Optional(CONF_POWER): sensor.sensor_schema(
            unit_of_measurement="W",
            accuracy_decimals=0,
            device_class="power",
            state_class="measurement",
        ),
        cv.Optional(CONF_SESSION_ENERGY): sensor.sensor_schema(
            unit_of_measurement="kWh",
            accuracy_decimals=3,
            device_class="energy",
            state_class="total_increasing",
        ),
        cv.Optional(CONF_METER_VALUE): sensor.sensor_schema(
            unit_of_measurement="kWh",
            accuracy_decimals=3,
            device_class="energy",
            state_class="total_increasing",
        ),
        cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement="Â°C",
            accuracy_decimals=1,
            device_class="temperature",
            state_class="measurement",
        ),
    }
).extend(cv.COMPONENT_SCHEMA).extend(uart.UART_DEVICE_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_mode(config[CONF_MODE]))
    cg.add(var.set_max_current(config[CONF_MAX_CURRENT]))
    cg.add(var.set_charger_breaker_current(config[CONF_CHARGER_BREAKER_CURRENT]))
    cg.add(var.set_main_fuse_current(config[CONF_MAIN_FUSE_CURRENT]))
    cg.add(var.set_manual_current(config[CONF_MANUAL_CURRENT]))
    cg.add(var.set_charge_phases(config[CONF_CHARGE_PHASES]))
    cg.add(var.set_failsafe_current(config[CONF_FAILSAFE_CURRENT]))
    cg.add(var.set_failsafe_mode(config[CONF_FAILSAFE_MODE]))
    cg.add(var.set_heartbeat_interval(config[CONF_HEARTBEAT_INTERVAL]))
    cg.add(var.set_watchdog_timeout(config[CONF_WATCHDOG_TIMEOUT]))
    cg.add(var.set_commissioning_mode(config[CONF_COMMISSIONING_MODE]))

    if CONF_RS485_DE_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_RS485_DE_PIN])
        cg.add(var.set_rs485_de_pin(pin))
    if CONF_RELAY_EVBOX_KNOWN_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_RELAY_EVBOX_KNOWN_PIN])
        cg.add(var.set_relay_evbox_known_pin(pin))
    if CONF_RELAY_JANITZA_OK_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_RELAY_JANITZA_OK_PIN])
        cg.add(var.set_relay_janitza_ok_pin(pin))
    if CONF_RELAY_CHARGING_ACTIVE_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_RELAY_CHARGING_ACTIVE_PIN])
        cg.add(var.set_relay_charging_active_pin(pin))
    if CONF_RELAY_FAILSAFE_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_RELAY_FAILSAFE_PIN])
        cg.add(var.set_relay_failsafe_pin(pin))

    if CONF_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_STATUS])
        cg.add(var.set_status_text_sensor(sens))
    if CONF_STATE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_STATE])
        cg.add(var.set_state_text_sensor(sens))
    if CONF_COMMUNICATION_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_COMMUNICATION_STATUS])
        cg.add(var.set_communication_text_sensor(sens))
    if CONF_PROTOCOL_PROFILE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_PROTOCOL_PROFILE])
        cg.add(var.set_protocol_profile_text_sensor(sens))
    if CONF_CB_SERIAL in config:
        sens = await text_sensor.new_text_sensor(config[CONF_CB_SERIAL])
        cg.add(var.set_cb_serial_text_sensor(sens))
    if CONF_CB_STATUS_DETAIL in config:
        sens = await text_sensor.new_text_sensor(config[CONF_CB_STATUS_DETAIL])
        cg.add(var.set_cb_status_detail_text_sensor(sens))
    if CONF_CURRENT_REQUEST_STATE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_CURRENT_REQUEST_STATE])
        cg.add(var.set_current_request_state_text_sensor(sens))
    if CONF_CB_FIRMWARE in config:
        sens = await sensor.new_sensor(config[CONF_CB_FIRMWARE])
        cg.add(var.set_cb_firmware_sensor(sens))
    if CONF_CB_HARDWARE_GENERATION in config:
        sens = await sensor.new_sensor(config[CONF_CB_HARDWARE_GENERATION])
        cg.add(var.set_cb_hardware_generation_sensor(sens))
    if CONF_EV_CURRENT in config:
        sens = await sensor.new_sensor(config[CONF_EV_CURRENT])
        cg.add(var.set_ev_current_sensor(sens))
    if CONF_CURRENT_LIMIT in config:
        sens = await sensor.new_sensor(config[CONF_CURRENT_LIMIT])
        cg.add(var.set_current_limit_sensor(sens))
    if CONF_DESIRED_CURRENT in config:
        sens = await sensor.new_sensor(config[CONF_DESIRED_CURRENT])
        cg.add(var.set_desired_current_sensor(sens))
    if CONF_COMMANDED_CURRENT in config:
        sens = await sensor.new_sensor(config[CONF_COMMANDED_CURRENT])
        cg.add(var.set_commanded_current_sensor(sens))
    if CONF_RETURNED_CURRENT_LIMIT in config:
        sens = await sensor.new_sensor(config[CONF_RETURNED_CURRENT_LIMIT])
        cg.add(var.set_returned_current_limit_sensor(sens))
    if CONF_L1_CURRENT in config:
        sens = await sensor.new_sensor(config[CONF_L1_CURRENT])
        cg.add(var.set_l1_current_sensor(sens))
    if CONF_L2_CURRENT in config:
        sens = await sensor.new_sensor(config[CONF_L2_CURRENT])
        cg.add(var.set_l2_current_sensor(sens))
    if CONF_L3_CURRENT in config:
        sens = await sensor.new_sensor(config[CONF_L3_CURRENT])
        cg.add(var.set_l3_current_sensor(sens))
    if CONF_L1_VOLTAGE in config:
        sens = await sensor.new_sensor(config[CONF_L1_VOLTAGE])
        cg.add(var.set_l1_voltage_sensor(sens))
    if CONF_L2_VOLTAGE in config:
        sens = await sensor.new_sensor(config[CONF_L2_VOLTAGE])
        cg.add(var.set_l2_voltage_sensor(sens))
    if CONF_L3_VOLTAGE in config:
        sens = await sensor.new_sensor(config[CONF_L3_VOLTAGE])
        cg.add(var.set_l3_voltage_sensor(sens))
    if CONF_L1_POWER_FACTOR in config:
        sens = await sensor.new_sensor(config[CONF_L1_POWER_FACTOR])
        cg.add(var.set_l1_power_factor_sensor(sens))
    if CONF_L2_POWER_FACTOR in config:
        sens = await sensor.new_sensor(config[CONF_L2_POWER_FACTOR])
        cg.add(var.set_l2_power_factor_sensor(sens))
    if CONF_L3_POWER_FACTOR in config:
        sens = await sensor.new_sensor(config[CONF_L3_POWER_FACTOR])
        cg.add(var.set_l3_power_factor_sensor(sens))
    if CONF_DETECTED_CHARGE_PHASES in config:
        sens = await sensor.new_sensor(config[CONF_DETECTED_CHARGE_PHASES])
        cg.add(var.set_detected_charge_phases_sensor(sens))
    if CONF_ACTIVE_PHASE_MASK in config:
        sens = await sensor.new_sensor(config[CONF_ACTIVE_PHASE_MASK])
        cg.add(var.set_active_phase_mask_sensor(sens))
    if CONF_POWER in config:
        sens = await sensor.new_sensor(config[CONF_POWER])
        cg.add(var.set_power_sensor(sens))
    if CONF_SESSION_ENERGY in config:
        sens = await sensor.new_sensor(config[CONF_SESSION_ENERGY])
        cg.add(var.set_session_energy_sensor(sens))
    if CONF_METER_VALUE in config:
        sens = await sensor.new_sensor(config[CONF_METER_VALUE])
        cg.add(var.set_meter_value_sensor(sens))
    if CONF_TEMPERATURE in config:
        sens = await sensor.new_sensor(config[CONF_TEMPERATURE])
        cg.add(var.set_temperature_sensor(sens))
