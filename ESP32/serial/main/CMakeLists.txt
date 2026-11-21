idf_component_register(SRCS "main.c"
                            "cli_demo_setup.c"
                            "cli_transport_serial.c"
                            "OpenLibCLI/cli.c"
                            "OpenLibCLI/utils/cli_args_parse.c"
                    INCLUDE_DIRS "."
                            "OpenLibCLI"
                            "OpenLibCLI/config"
                            "OpenLibCLI/utils"
                    REQUIRES esp_driver_uart esp_driver_usb_serial_jtag vfs esp_driver_gpio esp_timer)

# Nothing in this demo shows the engine's tick timing (cli_tick_stats_get()): left out.
target_compile_definitions(${COMPONENT_LIB} PRIVATE CLI_ENABLE_TIMMING_STATS=0)
