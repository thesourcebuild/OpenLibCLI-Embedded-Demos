# ESP32 Telnet Demo

This directory contains the ESP32 telnet (WiFi) demo for OpenLibCLI.

## Table of Contents

- [Target](#target)
- [Development Environment](#development-environment)
- [Usage](#usage)
- [Notes](#notes)

## Target

- Board: ESP32 / ESP32-S3 / ESP32-C3 / ESP32-C6 with >=4MB flash (a target with WiFi: ESP32-H2 has none, and the build stops with an error on such a target)
- Transport: Telnet over WiFi (BSD sockets via lwIP)

## Development Environment

- SDK: ESP-IDF 6.1
- Framework: ESP-IDF CMake build system
- Flashing: `idf.py flash` or `esptool.py`

## Usage

Select your target and edit `main/wifi_credentials.h` with your WiFi SSID and password:

```c
#define CLI_WIFI_SSID     "your-ssid"
#define CLI_WIFI_PASSWORD "your-password"
```

Build and flash:

```
idf.py build
idf.py flash monitor
```

Watch the serial output for the assigned IP, then connect via telnet:

```
telnet 192.168.x.x 2323
```

The CLI will negotiate telnet options (ECHO, SGA, NAWS) automatically.

## Notes

The CLI runs in the main task. `sdkconfig.defaults` sets its stack to 6144 bytes (the ESP-IDF default, 3584, is too small for a command that prints with this demo's buffers). The defaults apply when `sdkconfig` is created: after updating from an older version, delete `sdkconfig`, or set "Main task stack size" in `menuconfig`.

The built-in LED type and GPIO default to the target chip's common dev board (override via menuconfig, "OpenLibCLI Telnet Demo Configuration"): a plain LED on GPIO2 on ESP32, a WS2812 RGB LED on GPIO8 on ESP32-C3/C6 and on GPIO48 on ESP32-S3 (GPIO38 on ESP32-S3-DevKitC-1 v1.1 boards: set it). `led on` lights it (a dim white on an RGB LED) and `led off` switches it off; it keeps its state from one session to the next. The RGB LED is driven through the RMT peripheral with the `espressif/led_strip` component, which the ESP-IDF Component Manager downloads on the first build (declared in `main/idf_component.yml`). If the LED cannot be set up, `led` reports that it is not available. The server uses BSD sockets (lwIP via ESP-IDF) and serves one telnet client at a time: a client that connects while a session runs is disconnected at once, as in the Pico W and Arduino demos. `exit` / `quit` and 60 s of inactivity restart the login on the same connection. IAC negotiation and 0xFF escaping are handled identically to the Pico W telnet demo.
