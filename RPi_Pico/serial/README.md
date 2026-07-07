# RPi Pico Serial Demo

This directory contains the Raspberry Pi Pico serial (USB CDC) demo for OpenLibCLI.

## Table of Contents

- [Target](#target)
- [Development Environment](#development-environment)
- [Usage](#usage)
- [Notes](#notes)

## Target

- Board: Raspberry Pi Pico / Pico W / Pico 2 / Pico 2 W with RAM=264KB, ROM(Flash)=2MB
- Transport: Serial USB CDC (via `stdio`)

## Development Environment

- SDK: Raspberry Pi Pico SDK 2.2.0
- Toolchain: ARM GCC 14.2 Rel1
- IDE: VS Code with Raspberry Pi Pico extension
- Flashing: picotool 2.2.0-a4 (UF2 / `picotool load`)

## Usage

Select your board in `CMakeLists.txt`:

```
# Pico       → pico     (LED on GPIO 25)
# Pico W     → pico_w   (LED on CYW43 WL GPIO)
# Pico 2     → pico2    (LED on GPIO 25)
# Pico 2 W   → pico2_w  (LED on CYW43 WL GPIO)
set(PICO_BOARD pico CACHE STRING "Board type")
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

Connect to the USB serial port (115200 baud) and start the CLI.

## Notes

The project is built with the standard Pico SDK build system and VS Code extension. LED control auto-detects the board type at compile time: CYW43 GPIO for W variants, regular GPIO for non-W variants. USB CDC serial is used for the CLI transport. Hardware UART (GP0/GP1) is disabled to avoid conflicts.
