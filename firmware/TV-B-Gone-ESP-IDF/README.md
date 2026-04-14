# TV-B-Gone Kit V2 — ESP-IDF Firmware

ESP-IDF firmware for the TV-B-Gone Kit V2, targeting the **ESP32-C3 Super Mini** board. Written in plain C, using FreeRTOS and the ESP-IDF RMT driver for precise IR signal generation.

## Overview

This application transmits IR power-off codes for a wide range of television brands. It supports two regional databases:

- **NA** — North America, Asia, and the rest of the world not covered by EU (~130 codes)
- **EU** — Europe, Middle East, Australia, New Zealand, and parts of Africa and South America (~230 codes)

Press the NA or EU pushbutton to transmit the corresponding code database. All transmission logic runs inside a FreeRTOS task. Microsecond-accurate IR pulses are generated using the RMT peripheral; inter-code gaps use `vTaskDelay()` to yield to the scheduler.

## Requirements

- **ESP-IDF v5.5** or later
- **Target:** `esp32c3`

## Hardware

| Signal | Default GPIO | Description |
|--------|-------------|-------------|
| Visible LED | GPIO 8 | Built-in LED on the ESP32-C3 Super Mini (active-LOW) |
| IR LED | GPIO 2 | IR LED via IRLU024NPBF MOSFET amplifier |
| NA Button | GPIO 10 | Pushbutton to GND (internal pull-up) |
| EU Button | GPIO 9 | Pushbutton to GND (internal pull-up) |

> GPIO assignments are configurable via `idf.py menuconfig` — see [Configuration](#configuration).

**Power:** 3×AA battery pack connected to the 5V and GND pins (Vcc ≈ 4.5V).

**IR LED circuit:**
- MOSFET Source → GND
- MOSFET Gate → 1 kΩ resistor → IR LED GPIO (with 47 kΩ pull-down to GND)
- MOSFET Drain → 4.7 Ω (½W) resistor → IR333 LED → +4.5V (anode to +4.5V)
- Multiple IR LED/resistor pairs can be connected in parallel for longer range

## Build and Flash

```bash
# Set the target
idf.py set-target esp32c3

# Build
idf.py build

# Flash and monitor
idf.py -p <PORT> flash monitor
```

Default monitor baud rate: **115200**.

## Configuration

GPIO pins can be changed without editing source code:

```bash
idf.py menuconfig
```

Navigate to **TV-B-Gone Configuration**:

| Option | Default | Description |
|--------|---------|-------------|
| `TVBGONE_VISLED_GPIO` | 8 | Visible status LED GPIO |
| `TVBGONE_IRLED_GPIO` | 2 | IR LED output GPIO |
| `TVBGONE_BUTTON_NA_GPIO` | 10 | NA region select button GPIO |
| `TVBGONE_BUTTON_EU_GPIO` | 9 | EU region select button GPIO |

## Files

| File | Description |
|------|-------------|
| `main/main.c` | Application entry point, hardware init, FreeRTOS task, RMT IR transmission |
| `main/main.h` | Pin definitions, region constants, `IrCode` struct, macro definitions |
| `main/WORLDcodes.cpp` | NA and EU IR power-code databases |
| `main/Kconfig.projbuild` | Menuconfig options for GPIO assignments |
| `main/CMakeLists.txt` | Component registration |
| `CMakeLists.txt` | Project CMake file |

## Usage

1. Press the **NA** button to transmit the North America/Asia code database.
2. Press the **EU** button to transmit the Europe/Africa/Oceania code database.
3. The visible LED blinks **3 times** for NA or **6 times** for EU at the start and end of each run, and blips briefly before each code is sent.
4. Press either button again during transmission to restart from the beginning.

## License

Creative Commons **CC BY-SA 4.0** — you are free to distribute, remix, adapt, and build upon this work, including for commercial use, as long as attribution is given and adaptations are shared under the same terms.

Original firmware by Mitch Altman and Limor Fried (2009). Ported to Arduino by Ken Shirriff (2009). Rewritten for ESP-IDF by Mitch Altman (2026).
