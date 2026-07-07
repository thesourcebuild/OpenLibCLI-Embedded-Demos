# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
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
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# Board type — change this for your hardware:
#   Pico       → pico     (LED on GPIO 25)  <-- default
#   Pico W     → pico_w   (LED on CYW43 WL GPIO)
#   Pico 2     → pico2    (LED on GPIO 25)
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
        cli_transport_serial.c
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
        hardware_watchdog
        )

# Enable USB CDC serial (required for stdio over USB)
pico_enable_stdio_usb(OpenLibCLI 1)
pico_enable_stdio_uart(OpenLibCLI 0)

# Force USB disconnect/reconnect on reset so Windows re-enumerates reliably
target_compile_definitions(OpenLibCLI PRIVATE
    PICO_STDIO_USB_CONNECT_WITH_CLASSIC_DISABLE=1
)

# CYW43 support (Pico W / Pico 2 W) is auto-detected by the SDK.
if (PICO_CYW43_SUPPORTED)
    target_link_libraries(OpenLibCLI pico_cyw43_arch_none)
    target_compile_definitions(OpenLibCLI PRIVATE CLI_HAS_CYW43=1)
endif()

# Generate .map file for memory analysis
target_link_options(OpenLibCLI PRIVATE "-Wl,-Map=${CMAKE_BINARY_DIR}/OpenLibCLI.map")

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(OpenLibCLI)

# add url via pico_set_program_url
