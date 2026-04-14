# TV-B-Gone Core for ESP-IDF

`TV-B-Gone Core for ESP-IDF` packages the current TV-B-Gone ESP-IDF firmware as a reusable component for ESP-IDF projects.

## What It Includes

- The current TV-B-Gone transmit/runtime logic
- The bundled TV-B-Gone world power-code database
- GitHub Actions for example validation and registry publication

## Default Hardware Configuration

The default configuration matches the current TV-B-Gone firmware wiring:

- IR LED: `GPIO2`
- NA button: `GPIO10`
- EU button: `GPIO9`
- Visible LED: `GPIO8` active-low

## Public API

```c
tvbgone_core_config_t config;
tvbgone_core_get_default_config(&config);
ESP_ERROR_CHECK(tvbgone_core_start(&config));
```

Use `tvbgone_core_get_default_config()` to start from the standard TV-B-Gone
board wiring, then override fields if your board differs.

## Example

The example in [`examples/tvbgone_esp32c3_supermini`](examples/tvbgone_esp32c3_supermini)
is the reference application for this component. It preserves the current
operational mode:

- press NA to transmit the North America database
- press EU to transmit the Europe database
- press either button during transmission to restart from the beginning
- blink the visible LED before each code and at region start/end

## Build The Example

```bash
cd examples/tvbgone_esp32c3_supermini
idf.py set-target esp32c3
idf.py build
```

## Registry Publishing

This component is prepared for the Espressif Component Registry. The publish workflow is tag-driven and expects a GitHub Actions secret named `IDF_COMPONENT_API_TOKEN`.
