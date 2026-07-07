/**
 * @file cli_demo_setup.c
 * @brief OpenLibCLI serial demo lifecycle for STM32 HAL.
 */

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#include <string.h>

#include "cli_transport_serial.h"
#include "cli_demo_setup.h"
#include "OpenLibCLI/cli.h"
#include "OpenLibCLI/cli_version.h"
#include "OpenLibCLI/utils/cli_args_parse.h"

/*=======================================================================================
 * Private Defines
 *=======================================================================================*/
#ifndef CLI_MAX_COMMANDS
#define CLI_MAX_COMMANDS 20
#endif

/** Uncomment exactly one: */
#define CLI_PARSE_BACKEND_HANDROLLED  1
#define CLI_PARSE_BACKEND_STRTOL      2
#define CLI_PARSE_BACKEND_SSCANF      3

/** Select active backend here: */
#define CLI_PARSE_BACKEND  CLI_PARSE_BACKEND_HANDROLLED

#define CLI_DEMO_BANNER_SERIAL                                                                     \
  "************************************************************\r\n"                               \
  "*             OpenLibCLI  --  Serial (UART)                *\r\n"                               \
  "*         Pure-C, 100%% Static Allocation CLI Engine       *\r\n"                              \
  "************************************************************"

/** @brief Calculator sub-menu mode. */
#define APP_MODE_CALCULATOR (CLI_MODE_USER_BASE)

/** @brief Arithmetic sub-menu mode. */
#define APP_MODE_ARITHMETIC (CLI_MODE_USER_BASE + 1)

/** @brief LED pin — adjust for your board. */
#define LED_PORT GPIOA
#define LED_PIN  GPIO_PIN_5

/*=======================================================================================
 * Private Macros
 *=======================================================================================*/

/* No private function-like macros. */

/*=======================================================================================
 * Private Types
 *=======================================================================================*/

typedef struct {
  uint32_t session_id;
  uint32_t rx_packets;
  uint32_t tx_packets;
} cli_serial_app_data_t;

/*=======================================================================================
 * External Data Variables
 *=======================================================================================*/

/* No externally visible data variables. */

/*=======================================================================================
 * Private Variables
 *=======================================================================================*/

static cli_serial_app_data_t s_app;
static cli_struct_t s_cli;
static cli_cmd_struct_t s_cmd_pool[CLI_MAX_COMMANDS];
static uint32_t s_session_id = 0;

/*=======================================================================================
 * Private Function Prototypes
 *=======================================================================================*/

static int8_t cmd_show_version(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_led(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_reboot(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_calculator(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_arithmetic(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_add(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_sub(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);

static void cli_register_commands(cli_struct_t *cli);

static uint32_t stm32_now_sec(void *ctx);

/*=======================================================================================
 * Public Functions
 *=======================================================================================*/

/**
 * @brief Initialize and start the CLI demo session on the given UART.
 *
 * @param[in] huart STM32 HAL UART handle used by the transport.
 */

void cli_demo_setup(UART_HandleTypeDef *huart) {
  cli_platform_struct_t platform;
  cli_transport_struct_t transport;
  GPIO_InitTypeDef gpio_led = {0};
  uint8_t can_setup = 1U;

  if (huart == NULL) {
    can_setup = 0U;
  }

  if (can_setup == 1U) {
    cli_demo_shutdown();

    /* Init on-board LED pin */
    __HAL_RCC_GPIOC_CLK_ENABLE();
    gpio_led.Pin = LED_PIN;
    gpio_led.Mode = GPIO_MODE_OUTPUT_PP;
    gpio_led.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_PORT, &gpio_led);
		
    memset(&platform, 0, sizeof(platform));
    platform.now_sec = stm32_now_sec;
    platform.now_sec_ctx = NULL;

    cli_transport_serial_init(&transport, huart);

    cli_init(&s_cli, "MCU", &transport, &platform, s_cmd_pool, sizeof(s_cmd_pool) / sizeof(s_cmd_pool[0]));
    cli_register_commands(&s_cli);
		
		//>! set to false for non ANSI/VT100 terminal
		//  cli_set_ansi_supported(&s_cli, false);
		
		//>! Normally for RTOS/Baremetal MCUs environment
		cli_set_exit_root_policy(&s_cli, CLI_EXIT_ROOT_POLICY_RESET_SESSION);
		cli_set_quit_policy(&s_cli, CLI_QUIT_POLICY_RESET_SESSION);
		cli_set_auth_failure_mode(&s_cli, CLI_AUTH_FAILURE_MODE_LOCKOUT);

    s_session_id++;
    memset(&s_app, 0, sizeof(s_app));
    s_app.session_id = s_session_id;
    s_app.rx_packets = 12345U;
    s_app.tx_packets = 11000U;
    cli_set_userdata(&s_cli, &s_app);
		
#if CLI_ENABLE_BANNER
  cli_set_banner(&s_cli, CLI_DEMO_BANNER_SERIAL);
#endif

#if CLI_ENABLE_AUTH
    cli_set_enable_secret(&s_cli, "cisco");
    cli_add_user(&s_cli, "admin", "admin", CLI_PRIV_PRIVILEGED);
    cli_require_authorization(&s_cli, true);
    cli_set_idle_timeout(&s_cli, 60);
#endif

#if CLI_ENABLE_MODE_NAMES
    cli_set_mode_name(&s_cli, CLI_MODE_CONFIG, "config");
#endif

    cli_start(&s_cli);
  }
}

/**
 * @brief Run one non-blocking service tick of the active CLI session.
 */

void cli_demo_poll(void) {
  if (cli_session_engine(&s_cli) != CLI_OK) {
    cli_demo_shutdown();
  }
}

/**
 * @brief Shut down the active CLI session if running.
 */

void cli_demo_shutdown(void) {
  cli_done(&s_cli);
}

/**
 * @brief Report whether a CLI session is currently active.
 *
 * @return 1 when a session is active, otherwise 0.
 */

uint8_t cli_demo_is_running(void) {
  return (s_cli.hostname[0] != '\0') ? 1U : 0U;
}

/*=======================================================================================
 * Private Functions
 *=======================================================================================*/

/**
 * @brief Demo command handler that prints library and platform info.
 */

static int8_t cmd_show_version(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]) {
  (void)cmd;
  (void)argc;
  (void)argv;

  cli_println(cli, "OpenLibCLI Library version: %s", CLI_VERSION_STRING);
  cli_println(cli, "Platforms   : Linux / macOS / Windows / Embedded");
  cli_println(cli, "Transports  : Telnet, TCP, UDP, Serial (UART, stdin/stdout), pipe");
  return CLI_OK;
}

static int8_t cmd_led(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]) {
  (void)cmd;
  int8_t rc = CLI_OK;

  if (argc < 2) {
    cli_error(cli, "Usage: led <on|off>");
    rc = CLI_ERR;
  } else if (strcmp(argv[1], "on") == 0) {
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);
    cli_println(cli, "LED turned on");
  } else if (strcmp(argv[1], "off") == 0) {
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
    cli_println(cli, "LED turned off");
  } else {
    cli_error(cli, "Invalid state. Use 'on' or 'off'");
    rc = CLI_ERR;
  }

  return rc;
}

static int8_t cmd_reboot(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]) {
  (void)cmd;
  (void)argc;
  (void)argv;

//  int8_t rc = CLI_OK;
  
  cli_println(cli, "Resetting the system...");
  HAL_Delay(100);
  NVIC_SystemReset();
  /* NVIC_SystemReset does not return */
  while (1) {}
  
//  return rc;
}

static int8_t cmd_calculator(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]) {
  (void)cmd;
  (void)argc;
  (void)argv;
#if CLI_ENABLE_MODE_NAMES
  cli_set_mode_name(cli, APP_MODE_CALCULATOR, "");
  cli_set_mode_name(cli, APP_MODE_ARITHMETIC, "");
#endif
  cli_set_mode(cli, APP_MODE_CALCULATOR);
  return CLI_OK;
}

static int8_t cmd_arithmetic(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]) {
  (void)cmd;
  (void)argc;
  (void)argv;
  cli_set_mode(cli, APP_MODE_ARITHMETIC);
  return CLI_OK;
}

static int8_t cmd_add(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]) {
  (void)cmd;
  int8_t rc = CLI_ERR;

  if (argc < 3 || argc > 4) {
    cli_error(cli, "Usage: add <a> <b>  or  add <a> + <b>");
  } else if (argc == 4 && strcmp(argv[2], "+") != 0) {
    cli_error(cli, "Usage: add <a> + <b>");
  } else {
    int32_t a;
    int32_t b;
		
#if CLI_PARSE_BACKEND == CLI_PARSE_BACKEND_HANDROLLED
    parse_i32(argv[1], &a);
    parse_i32((argc == 4) ? argv[3] : argv[2], &b);
#elif CLI_PARSE_BACKEND == CLI_PARSE_BACKEND_STRTOL
    strtol_parse_i32(argv[1], &a);
    strtol_parse_i32((argc == 4) ? argv[3] : argv[2], &b);
#else
    cli_sscanf(argv[1], "%d", &a);
    cli_sscanf((argc == 4) ? argv[3] : argv[2], "%d", &b);
#endif
		
    cli_println(cli, "%d", a + b);
    rc = CLI_OK;
  }

  return rc;
}

static int8_t cmd_sub(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]) {
  (void)cmd;
  int8_t rc = CLI_ERR;

  if (argc < 3 || argc > 4) {
    cli_error(cli, "Usage: sub <a> <b>");
  } else if (argc == 4 && strcmp(argv[2], "-") != 0) {
    cli_error(cli, "Usage: sub <a> - <b>");
  } else {
    int32_t a;
    int32_t b;
		
#if CLI_PARSE_BACKEND == CLI_PARSE_BACKEND_HANDROLLED
    parse_i32(argv[1], &a);
    parse_i32((argc == 4) ? argv[3] : argv[2], &b);
#elif CLI_PARSE_BACKEND == CLI_PARSE_BACKEND_STRTOL
    strtol_parse_i32(argv[1], &a);
    strtol_parse_i32((argc == 4) ? argv[3] : argv[2], &b);
#else
    cli_sscanf(argv[1], "%d", &a);
    cli_sscanf((argc == 4) ? argv[3] : argv[2], "%d", &b);
#endif
		
    cli_println(cli, "%d", a - b);
    rc = CLI_OK;
  }

  return rc;
}

/**
 * @brief Register all demo commands in the global command tree.
 */

static void cli_register_commands(cli_struct_t *cli) {
  cli_cmd_handle_t h_clear;
  cli_cmd_handle_t h_show;
  cli_cmd_handle_t h_calc;
  cli_cmd_handle_t h_arith;

  cli_register_command(cli, CLI_CMD_ROOT, "exit", cli_cmd_builtin_exit, CLI_PRIV_USER, CLI_MODE_ANY,
                  "Exit Context");
	
  cli_register_command(cli, CLI_CMD_ROOT, "help", cli_cmd_builtin_help, CLI_PRIV_USER, CLI_MODE_ANY,
                  "Show commands");
#if CLI_ENABLE_HISTORY
  cli_register_command(cli, CLI_CMD_ROOT, "history", cli_cmd_builtin_history, CLI_PRIV_USER,
                  CLI_MODE_ANY, "Command history");
#endif

//>! Uncomment on ANSI/VT100 compatabile terminal like PuTTY
  h_clear = cli_register_command(cli, CLI_CMD_ROOT, "clear", cli_cmd_builtin_clear_screen,
                            CLI_PRIV_USER, CLI_MODE_ANY, "Clear screen");
  (void)h_clear;

#if CLI_ENABLE_ALIASES
  cli_cmd_add_alias(cli, h_clear, "cls");
#endif
	
  h_show = cli_register_command(cli, CLI_CMD_ROOT, "show", NULL, CLI_PRIV_USER, CLI_MODE_ANY,
                                "Show info");

  cli_register_command(cli, h_show, "version", cmd_show_version, CLI_PRIV_USER, CLI_MODE_ANY,
                       "Version");

  cli_register_command(cli, CLI_CMD_ROOT, "led", cmd_led, CLI_PRIV_USER, CLI_MODE_ANY,
                       "Set LED state (on/off)");

  cli_register_command(cli, CLI_CMD_ROOT, "reboot", cmd_reboot, CLI_PRIV_USER, CLI_MODE_ANY,
                       "Reset the system");

  h_calc = cli_register_command(cli, CLI_CMD_ROOT, "calc", cmd_calculator, CLI_PRIV_USER,
                                 CLI_MODE_ANY, "Enter calc menu");
  h_arith = cli_register_command(cli, h_calc, "arith", cmd_arithmetic, CLI_PRIV_USER,
                                  CLI_MODE_ANY, "Enter arithmetic submenu");
  cli_register_command(cli, h_arith, "add", cmd_add, CLI_PRIV_USER, CLI_MODE_ANY,
                       "Add two numbers (add <a> <b>)");
  cli_register_command(cli, h_arith, "sub", cmd_sub, CLI_PRIV_USER, CLI_MODE_ANY,
                       "Subtract two numbers (sub <a> <b>)");
}

/**
 * @brief Return current system uptime in seconds.
 *
 * @param[in] ctx Unused context pointer.
 * @return Monotonic uptime in seconds.
 */

static uint32_t stm32_now_sec(void *ctx) {
  (void)ctx;
  return HAL_GetTick() / 1000U;
}
/*################################### END OF FILE ######################################*/
