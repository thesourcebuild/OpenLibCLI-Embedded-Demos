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
- Transport: the ESP-IDF console port — a UART (default: UART0, e.g. TX=GPIO1 RX=GPIO3 on ESP32) or the USB-Serial/JTAG port

## Development Environment

- SDK: ESP-IDF 6.1
- IDE: VS Code with ESP-IDF extension, or command-line with `idf.py`
- Flashing: `idf.py flash` (via UART or JTAG)

## Usage

Configure the built-in LED for your board in `menuconfig` (under "OpenLibCLI Serial Demo Configuration"): its type (plain LED on a GPIO, or addressable WS2812 RGB LED) and GPIO. The defaults follow the target's common dev board:

```
// ESP32 DevKitC         → plain LED, GPIO2
// ESP32-C3/C6/H2 DevKitM → WS2812 RGB LED, GPIO8
// ESP32-S3 DevKitC-1    → WS2812 RGB LED, GPIO48 (GPIO38 on v1.1 boards: set it)
```

Build and flash:

```
idf.py set-target esp32       # or esp32c3, esp32s3, etc.
idf.py menuconfig             # optional: set the LED, the console port
idf.py build
idf.py -p PORT flash monitor
```

Connect to the serial console (115200 baud by default) and start the CLI. Press `Ctrl+]` to exit the monitor.

## Notes

The CLI runs in the main task. `sdkconfig.defaults` sets its stack to 6144 bytes (the ESP-IDF default, 3584, is too small for a command that prints with this demo's buffers). The defaults apply when `sdkconfig` is created: after updating from an older version, delete `sdkconfig`, or set "Main task stack size" in `menuconfig`.

The CLI runs on the ESP-IDF console port, as selected in `menuconfig` under "ESP-STDIO" → "Channel for console output":

- **UART** (default or custom): the CLI uses that UART at the configured "UART console baud rate" (115200 by default) and installs its driver.
- **USB Serial/JTAG Controller** (ESP32-C3/C6/H2/S3 and others): the CLI installs the USB-Serial/JTAG driver and routes the console's `printf()` / log output through it too. Output that no one reads (no terminal, cable unplugged) is dropped after a short wait instead of blocking the CLI.
- **USB CDC** and **None** are not supported: the build stops with an error.

With a UART console, the chips that have a USB-Serial/JTAG port also copy the console's log output there by default ("Channel for console secondary output"); the CLI itself stays on the UART.

`led on` lights the built-in LED (a dim white on an RGB LED), `led off` switches it off. The RGB LED is driven through the RMT peripheral with the `espressif/led_strip` component, which the ESP-IDF Component Manager downloads on the first build (declared in `main/idf_component.yml`). If the LED cannot be set up (e.g. a wrong GPIO), `led` reports that it is not available.
