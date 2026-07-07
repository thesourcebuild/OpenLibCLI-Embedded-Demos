# ESP32 Serial Demo

This directory contains the ESP32 serial (UART) demo for OpenLibCLI.

## Table of Contents

- [Target](#target)
- [Development Environment](#development-environment)
- [Usage](#usage)
- [Notes](#notes)

## Target

- SoC: ESP32 / ESP32-C3 / ESP32-S3 / ESP32-C6 (any ESP-IDF supported target)
- RAM: 512 KB+ (depending on variant)
- ROM(Flash): 4 MB+
- Transport: Serial UART (console UART, TX=GPIO1 RX=GPIO3 by default)

## Development Environment

- SDK: ESP-IDF 6.0
- IDE: VS Code with ESP-IDF extension, or command-line with `idf.py`
- Flashing: `idf.py flash` (via UART or JTAG)

## Usage

Configure the LED GPIO for your board in `menuconfig` (under "OpenLibCLI Serial Demo Configuration") or edit `cli_demo_setup.c`:

```
// Common values:
//   ESP32 DevKitC     → GPIO2  (default)
//   ESP32-C3-DevKitM  → GPIO8
//   ESP32-S3-DevKitC  → GPIO48
```

Build and flash:

```
idf.py set-target esp32       # or esp32c3, esp32s3, etc.
idf.py menuconfig             # optional: set LED GPIO
idf.py build
idf.py -p PORT flash monitor
```

Connect to the serial console (115200 baud) and start the CLI. Press `Ctrl+]` to exit the monitor.

## Notes

The project uses the console UART (already initialised by ESP-IDF) for the CLI transport — no separate UART configuration is needed. LED control uses the GPIO number configured in menuconfig. The `CLI_LED_GPIO` Kconfig option defaults to GPIO2 (common on ESP32 DevKitC).
