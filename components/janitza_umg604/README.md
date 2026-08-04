# janitza_umg604

ESPHome external component for polling a Janitza UMG604 Pro over Modbus TCP.

## Responsibilities

- Modbus TCP client
- Configurable IP address, port, unit id, and register map
- Per-phase current sensors
- Per-phase voltage sensors
- Total power sensor
- Import and export power sensors
- Communication status text sensor

## Register Map

The default register addresses match the working `Janitza UMG604-Pro HA`
project:

- L-N voltage: 1317, 1319, 1321
- Phase current: 1325, 1327, 1329
- Signed total active power: 1369

Import/export power sensors are derived from signed total active power:

- positive total power -> import
- negative total power -> export

The implementation currently expects 32-bit IEEE-754 float values spanning two
holding registers in big-endian Modbus register/byte order. If values are
wildly large, negative, or near zero while the meter display is normal, check
the UMG604 register map and byte/word order.
