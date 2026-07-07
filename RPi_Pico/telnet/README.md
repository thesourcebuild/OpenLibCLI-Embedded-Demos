# RPi Pico Telnet Demo

This directory contains the Raspberry Pi Pico W / Pico 2 W telnet demo for OpenLibCLI.

## Table of Contents

- [Target](#target)
- [Development Environment](#development-environment)
- [Usage](#usage)
- [Notes](#notes)

## Target

- Board: Raspberry Pi Pico W / Pico 2 W with RAM=264KB, ROM(Flash)=2MB
- Transport: Telnet over WiFi (lwIP raw TCP API)

## Development Environment

- SDK: Raspberry Pi Pico SDK 2.2.0
- Toolchain: ARM GCC 14.2 Rel1
- IDE: VS Code with Raspberry Pi Pico extension
- Flashing: picotool 2.2.0-a4 (UF2 / `picotool load`)

## Usage

The board is set to `pico_w` in `CMakeLists.txt`. For Pico 2 W, change to `pico2_w`.

Edit `wifi_credentials.h` with your WiFi SSID and password:

```c
#define CLI_WIFI_SSID     "your-ssid"
#define CLI_WIFI_PASSWORD "your-password"
```

Configure and build:

```
mkdir build && cd build
cmake .. -G "NMake Makefiles"
nmake
```

Flash (hold BOOTSEL or use picotool):

```
picotool load build\OpenLibCLI.uf2 -fx
```

Open a serial monitor (115200 baud) to see the assigned IP address, then connect via telnet:

```
telnet 192.168.100.24 2323
```

The CLI will negotiate telnet options (ECHO, SGA, NAWS) automatically.

## Notes

The project uses the lwIP **raw TCP callback API** (not BSD sockets) because the Pico SDK's `pico_lwip_nosys` is compiled with `NO_SYS=1`, which is incompatible with the socket/sequential API. The telnet IAC state machine (ECHO, SGA, NAWS, LINEMODE) runs inside the `tcp_recv` callback. Single-client architecture: the server stops listening after one client connects.
