# evbox_max

ESPHome external component for controlling an EVBox ChargeBox over the EVBox
MAX RS485 protocol.

## Responsibilities

- UART transport at 38400 8N1
- Optional MAX3485 driver-enable pin for half-duplex RS485
- MAX frame encoding and parsing
- Additive two's-complement checksum
- ChargeBox G2 registration/autodetect hook
- Address assignment hook
- Information/configuration read flow
- Heartbeat
- Local state machine
- Charge-current setpoint generation
- Session start/stop hooks
- Communication watchdog and fault transition
- Manual, load-balancing, PV-surplus, and disabled control modes
- Configurable charge phase count for PV surplus and load-balancing current
  calculations

## State Machine

```text
BOOT
WAIT_REGISTRATION
ASSIGN_ADDRESS
READ_INFO
READ_CONFIG
IDLE
AUTHORIZED
STARTING
CHARGING
PAUSED
FINISHING
FAULT
```

## Protocol Boundary

The protocol code is isolated in:

- `protocol.h`
- `protocol.cpp`

Charge-current decisions are isolated in:

- `controller.h`
- `controller.cpp`

The ESPHome component lifecycle and Home Assistant sensors are isolated in:

- `evbox_max.h`
- `evbox_max.cpp`

Home Assistant controls are isolated in:

- `number.py`, `select.py`, `switch.py`
- `entities.h`, `entities.cpp`

## Validation Work Still Required

The current frame identifiers and payload fields are a structured starting
point. Validate them against a logic-analyzer capture from the original EVBox
controller before enabling contactor control.
