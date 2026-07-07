# ESP32 Telnet Demo

This directory contains the ESP32 telnet (WiFi) demo for OpenLibCLI.

## Table of Contents

- [Target](#target)
- [Development Environment](#development-environment)
- [Usage](#usage)
- [Notes](#notes)

## Target

- Board: ESP32 / ESP32-S3 / ESP32-C3 / ESP32-C6 / ESP32-H2 with >=4MB flash
- Transport: Telnet over WiFi (BSD sockets via lwIP)

## Development Environment

- SDK: ESP-IDF 5.5.1
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

Built-in LED GPIO auto-detects per target chip (override via menuconfig). The server uses BSD sockets (lwIP via ESP-IDF) and accepts a single telnet client — the listening socket is closed once a connection is established. IAC negotiation and 0xFF escaping are handled identically to the Pico W telnet demo.
