# OpenLibCLI Embedded Demos

Embedded platform demos for [OpenLibCLI](https://github.com/thesourcebuild/OpenLibCLI) — a modern, pure-C99, 100% static-allocation CLI engine for desktop and embedded systems.

## Table of Contents

- [Examples](#examples)
- [Demos](#demos)
- [License](#license)

## Examples

| Platform | Status | Transports | IDE/Framework |
|---|---|---|---|
| **Arduino** | Complete | Serial (UART), Telnet (WiFi) | Arduino IDE, PlatformIO |
| **STM32** (STM32F103RB) | Complete | Serial (UART) | STM32CubeIDE, Make, CMake, Keil, IAR |
| **ESP32** | Complete | Serial, Telnet (native ESP-IDF) | ESP-IDF |
| **RPi Pico** | Complete | Serial, Telnet (native Pico SDK) | Pico SDK |

## Demos

### Arduino (`Arduino/OpenLibCLI/`)

Two example sketches packaged as a proper Arduino library:

- **`examples/serial/`** — CLI over Arduino Serial (UART) at 115200 baud
- **`examples/telnet/`** — CLI over WiFi Telnet (port 2323) with IAC negotiation

Commands: `exit`, `help`, `history`, `clear`, `show version`, `led <on|off>`, `reboot`, `calc` submenu, `arith` sub-submenu, `add <a> <b>`, `sub <a> <b>`. Features: banner/MOTD, auth (admin/admin, enable secret), idle timeout, tab-completion, ANSI support toggle.

Install via Arduino IDE (ZIP import) or PlatformIO (`library.json` / `platformio.ini`).

### STM32 (`STM32/serial/`)

STM32CubeMX project targeting the **Nucleo-F103RB** board with RAM=20KB, ROM(Flash)=64KB/128KB, LED on PA5 (built-in). Supports multiple build systems:

- **Make** (`arm-none-eabi-gcc`, `-specs=nano.specs`)
- **CMake** (GCC ARM / Clang ARM toolchains)
- **Keil MDK-ARM** (`MDK-ARM/`)
- **IAR Embedded Workbench** (`EWARM/`)
- **STM32CubeIDE** (`.project`, `.cproject`)

### ESP32 & RPi Pico

Placeholder directories for future native (non-Arduino) demos using ESP-IDF and Pico SDK respectively.

## Contributions

Contributions of all sizes are warmly welcome!. Please feel free to:

- Report issues using [the issue guide](docs/create_a_issue.md)
- Submit pull requests
- Improve documentation
- Suggest new features
- Start a discussion

Let's make the library better for everyone.

---

## License

MIT License — see ['LICENSE'](LICENSE) file and source file headers.

---

## Author

Muhammad Hassaan Shah

- GitHub: [@thesourcebuild](https://github.com/thesourcebuild)
- Project: [github.com/thesourcebuild/OpenLibCLI](https://github.com/thesourcebuild/OpenLibCLI)
