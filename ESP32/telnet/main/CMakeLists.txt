idf_component_register(SRCS "main.c"
                            "cli_demo_setup.c"
                            "cli_transport_telnet.c"
                            "OpenLibCLI/cli.c"
                            "OpenLibCLI/utils/cli_args_parse.c"
                    INCLUDE_DIRS "."
                            "OpenLibCLI"
                            "OpenLibCLI/config"
                            "OpenLibCLI/utils"
                    REQUIRES esp_wifi esp_netif nvs_flash esp_timer esp_driver_gpio lwip)

# Nothing in this demo shows the engine's tick timing (cli_tick_stats_get()): left out.
target_compile_definitions(${COMPONENT_LIB} PRIVATE CLI_ENABLE_TIMMING_STATS=0)
