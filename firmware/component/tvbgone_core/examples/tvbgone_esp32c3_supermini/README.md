# TV-B-Gone ESP32-C3 Super Mini Example

This example reproduces the current `firmware/TV-B-Gone-ESP-IDF` operational
mode using the reusable `tvbgone_core` component.

## Wiring

- IR LED amplifier gate: `GPIO2`
- NA button: `GPIO10`
- EU button: `GPIO9`
- Built-in visible LED: `GPIO8` active-low

## Behavior

- Press the NA button to transmit the North America database.
- Press the EU button to transmit the Europe database.
- Press either button during an active transmission to restart from the first
  code of the newly selected region.

## Build

```bash
idf.py set-target esp32c3
idf.py build
```
