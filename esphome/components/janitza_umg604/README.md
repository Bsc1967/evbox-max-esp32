# janitza_umg604

ESPHome external component for polling a Janitza UMG604 Pro over Modbus TCP.

## Responsibilities

- Modbus TCP client
- Configurable IP address, port, unit id, and register map
- Per-phase current sensors
- Per-phase voltage sensors
- Total power sensor
- Import and export power sensors
- Automatic 1/3-phase charge detection from measured phase currents
- Communication status text sensor
- Single-request live polling for the proven UMG604 live register block, so
  overload detection is not delayed by multiple TCP round-trips.

## Register Map

The default register addresses match the working `Janitza UMG604-Pro HA`
project:

- L-N voltage: 1317, 1319, 1321
- Phase current: 1325, 1327, 1329
- Signed total active power: 1369

Import/export power sensors are derived from signed total active power:

- positive total power -> import
- negative total power -> export

For overload response, set `evbox_max_id` so this component pushes import/export
values directly into the EVBox controller. The EVBox component immediately sends
a lower current setpoint when the calculated current drops.

When `evbox_max_id` is configured, the component also detects the active measured
charge phases. Each phase above `phase_detect_current` is added to an active
phase mask:

- L1 active -> bit 0
- L2 active -> bit 1
- L3 active -> bit 2

The EVBox controller uses that mask for per-phase main-fuse protection, so
1-phase, 2-phase, and 3-phase charging can be limited against the correct
measured phases. If no phase is above the threshold, the last detected phase
set is kept.

The implementation currently expects 32-bit IEEE-754 float values spanning two
holding registers in big-endian Modbus register/byte order. If values are
wildly large, negative, or near zero while the meter display is normal, check
the UMG604 register map and byte/word order.
