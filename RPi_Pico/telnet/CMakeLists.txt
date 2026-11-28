# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# Board type — the Telnet demo needs WiFi, so a W board:
#   Pico W     → pico_w   (LED on CYW43 WL GPIO)  <-- default
#   Pico 2 W   → pico2_w  (LED on CYW43 WL GPIO)
set(PICO_BOARD pico_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(OpenLibCLI C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(OpenLibCLI
        main.c
        cli_demo_setup.c
        cli_transport_telnet.c
        OpenLibCLI/cli.c
        OpenLibCLI/utils/cli_args_parse.c
        )

target_include_directories(OpenLibCLI PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/OpenLibCLI
        ${CMAKE_CURRENT_SOURCE_DIR}/OpenLibCLI/config
        ${CMAKE_CURRENT_SOURCE_DIR}/OpenLibCLI/utils
        )

target_link_libraries(OpenLibCLI
        pico_stdlib
        pico_cyw43_arch_lwip_threadsafe_background
        hardware_watchdog
        )

# Nothing in this demo shows the engine's tick timing (cli_tick_stats_get()): left out.
target_compile_definitions(OpenLibCLI PRIVATE CLI_ENABLE_TIMMING_STATS=0)

# The CLI runs on core 0's stack. The SDK reserves 2 KB for it, less than a command that
# prints needs with the desktop cli_config.h (a 1 KB output line, printf, lwIP and the USB
# interrupt): reserve the whole 4 KB scratch bank the stack lives in.
target_compile_definitions(OpenLibCLI PRIVATE PICO_STACK_SIZE=0x1000)

# Enable USB CDC serial (required for stdio over USB)
pico_enable_stdio_usb(OpenLibCLI 1)
pico_enable_stdio_uart(OpenLibCLI 0)

# CYW43 support is provided by pico_cyw43_arch_lwip_threadsafe_background above.
if (PICO_CYW43_SUPPORTED)
    target_compile_definitions(OpenLibCLI PRIVATE CLI_HAS_CYW43=1)
endif()

# Generate .map file for memory analysis
target_link_options(OpenLibCLI PRIVATE "-Wl,-Map=${CMAKE_BINARY_DIR}/OpenLibCLI.map")

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(OpenLibCLI)

# add url via pico_set_program_url

