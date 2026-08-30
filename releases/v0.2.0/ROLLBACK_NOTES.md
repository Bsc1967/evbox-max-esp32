# EVBox MAX ESP32 v0.2.0 rollback notes

Date: 2026-08-30

Purpose: first usable PV surplus and per-phase load-balancing version after the first proven charging release.

Rollback component commit:

```text
2a9953b23c029b84311824eca7eb86b0052551a3
```

Use this ESPHome external component pin:

```yaml
external_components:
  - source: github://Bsc1967/evbox-max-esp32@2a9953b23c029b84311824eca7eb86b0052551a3
    refresh: 0s
    components:
      - evbox_max
      - janitza_umg604
```

GitHub tag intended for this frozen version:

```text
v0.2.0
```
