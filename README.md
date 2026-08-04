# EVBox MAX ESP32 Controller

ESPHome external components for replacing the original EVBox ChargePoint
controller with an ESP32 while keeping the EVBox ChargeBox hardware.

Target hardware:

- EVBox BusinessLine G2, circa 2013
- Olimex ESP32-POE WROOM with native Ethernet
- RS485 through a 3.3 V MAX3485 transceiver
- Janitza UMG604 Pro over Modbus TCP
- Local-only control; Home Assistant is used for visualization, logging, and
  operator controls

## Components

This repository is structured as an ESPHome external component source:

```yaml
external_components:
  - source: github://Bsc1967/evbox-max-esp32@main
    components:
      - evbox_max
      - janitza_umg604
```

The source components live under `components/`, which is the layout ESPHome
expects for git-based external components.

## Current State

The project contains a maintainable implementation scaffold:

- `components/evbox_max`: EVBox MAX UART/RS485 controller, frame parser,
  checksum, G2 autodetect hooks, address assignment hooks, heartbeat,
  state machine, session tracking, watchdog, charge-current control, and
  separated charge-control algorithms.
- `components/janitza_umg604`: Modbus TCP polling component with configurable
  IP/registers and sensors for phase current, phase voltage, total power, and
  import/export power.
- `examples/evbox_max_olimex_poe.yaml`: example ESPHome configuration for an
  Olimex ESP32-POE WROOM installation.
- `examples/evbox_max_esp32_wroom_wifi.yaml`: test configuration for an
  ESP32-WROOM-32 over WiFi. Use this for bench testing; the ESP32-POE remains
  the better production target because it has native Ethernet.

The EVBox MAX protocol implementation intentionally keeps the frame format in
one place (`protocol.*`). Before connecting to live charging hardware, validate
the command identifiers and payload fields against captured EVBox traffic or
vendor documentation.

## Safety

Charging control is safety-critical. Treat this firmware as an integration
starting point until it has been validated on a bench setup:

- Start with the contactor disabled.
- Verify RS485 receive-only parsing first.
- Confirm address assignment and heartbeat behavior.
- Confirm fail-safe behavior on Janitza and EVBox communication loss.
- Only then allow non-zero pilot/current control.

## Documentation

- `docs/architecture_nl.md`: Dutch explanation of the communication layers,
  EVBox state machine, Janitza polling, control modes, and fail-safe behavior.

## License

MIT. See `LICENSE`.
