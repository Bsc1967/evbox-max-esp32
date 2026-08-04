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

The default register addresses are placeholders for a compact contiguous map.
Set the `registers:` block in YAML to match the actual UMG604 Pro Modbus
profile and configured data format.

The implementation currently expects 32-bit IEEE-754 float values spanning two
holding registers in big-endian register order.
