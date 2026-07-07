#include <string.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "lwip/tcp.h"
#include "OpenLibCLI/cli.h"
#include "OpenLibCLI/cli_version.h"
#include "OpenLibCLI/utils/cli_args_parse.h"
#include "cli_transport_telnet.h"
#include "cli_demo_setup.h"

/*---------------------------------------------------------------------
 * LED selection is compile-time only.
 *
 *  CMake sets CLI_HAS_CYW43 for Pico W / Pico 2 W.
 *  Otherwise we fall back to PICO_DEFAULT_LED_PIN when available.
 *  On other boards the LED hooks are disabled.
 *--------------------------------------------------------------------*/
#if defined(CLI_HAS_CYW43)
#include "pico/cyw43_arch.h"
#define CLI_LED_INIT() \
  do                   \
  {                    \
  } while (0)
#define CLI_LED_ON() cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1)
#define CLI_LED_OFF() cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0)
#elif defined(PICO_DEFAULT_LED_PIN)
#include "hardware/gpio.h"
#define CLI_LED_INIT()                            \
  do                                              \
  {                                               \
    gpio_init(PICO_DEFAULT_LED_PIN);              \
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT); \
    gpio_put(PICO_DEFAULT_LED_PIN, 0);            \
  } while (0)
#define CLI_LED_ON() gpio_put(PICO_DEFAULT_LED_PIN, 1)
#define CLI_LED_OFF() gpio_put(PICO_DEFAULT_LED_PIN, 0)
#else
#define CLI_LED_INIT() \
  do                   \
  {                    \
  } while (0)
#define CLI_LED_ON()
#define CLI_LED_OFF()
#endif

#ifndef CLI_MAX_COMMANDS
#define CLI_MAX_COMMANDS 20
#endif

#define CLI_PARSE_BACKEND_HANDROLLED 1
#define CLI_PARSE_BACKEND_STRTOL 2
#define CLI_PARSE_BACKEND_SSCANF 3

#define CLI_PARSE_BACKEND CLI_PARSE_BACKEND_HANDROLLED

#define CLI_DEMO_BANNER_TELNET                                       \
  "************************************************************\r\n" \
  "*       OpenLibCLI  --  Telnet on Raspberry Pi Pico W      *\r\n" \
  "*      Pure-C, 100%% Static Allocation CLI Engine          *\r\n" \
  "************************************************************"

#define APP_MODE_CALCULATOR (CLI_MODE_USER_BASE)
#define APP_MODE_ARITHMETIC (CLI_MODE_USER_BASE + 1)

typedef struct
{
  uint32_t session_id;
  uint32_t rx_packets;
  uint32_t tx_packets;
} cli_telnet_app_data_t;

static cli_telnet_app_data_t s_app;
static cli_struct_t s_cli;
static cli_cmd_struct_t s_cmd_pool[CLI_MAX_COMMANDS];
static uint32_t s_session_id = 0;

static cli_telnet_ctx_t s_telnet_ctx;
static cli_transport_struct_t s_transport;

static bool s_initialised = false;

static int8_t cmd_show_version(cli_struct_t *cli, const char *cmd,
                               cli_argc_t argc, const char *argv[]);
static int8_t cmd_led(cli_struct_t *cli, const char *cmd,
                      cli_argc_t argc, const char *argv[]);
static int8_t cmd_reboot(cli_struct_t *cli, const char *cmd,
                         cli_argc_t argc, const char *argv[]);
static int8_t cmd_calculator(cli_struct_t *cli, const char *cmd,
                             cli_argc_t argc, const char *argv[]);
static int8_t cmd_arithmetic(cli_struct_t *cli, const char *cmd,
                             cli_argc_t argc, const char *argv[]);
static int8_t cmd_add(cli_struct_t *cli, const char *cmd,
                      cli_argc_t argc, const char *argv[]);
static int8_t cmd_sub(cli_struct_t *cli, const char *cmd,
                      cli_argc_t argc, const char *argv[]);
static void cli_register_commands(cli_struct_t *cli);

static uint32_t pico_now_sec(void *ctx);

void cli_demo_setup(struct tcp_pcb *client_pcb)
{
  cli_platform_struct_t platform;

  cli_demo_shutdown();

  CLI_LED_INIT();

  memset(&platform, 0, sizeof(platform));
  platform.now_sec = pico_now_sec;
  platform.now_sec_ctx = NULL;

  cli_transport_telnet_init(&s_telnet_ctx, &s_transport, client_pcb);
  cli_transport_telnet_negotiate(&s_telnet_ctx);

  cli_init(&s_cli, "Pico", &s_transport, &platform,
           s_cmd_pool, sizeof(s_cmd_pool) / sizeof(s_cmd_pool[0]));
  cli_register_commands(&s_cli);

  cli_set_ansi_supported(&s_cli, true);

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
  cli_set_banner(&s_cli, CLI_DEMO_BANNER_TELNET);
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

  s_initialised = true;
  cli_start(&s_cli);
}

void cli_demo_poll(void)
{
  if (!s_initialised)
    return;

  /* Process CLI engine */
  if (cli_session_engine(&s_cli) != CLI_OK)
  {
    cli_demo_shutdown();
  }
}

void cli_demo_shutdown(void)
{
  if (s_initialised)
  {
    cli_done(&s_cli);
    if (s_telnet_ctx.client_pcb)
    {
      tcp_close(s_telnet_ctx.client_pcb);
      s_telnet_ctx.client_pcb = NULL;
    }
    s_initialised = false;
  }
}

uint8_t cli_demo_is_running(void)
{
  return s_initialised ? 1U : 0U;
}

/* ---- Command handlers ---- */

static int8_t cmd_show_version(cli_struct_t *cli, const char *cmd,
                               cli_argc_t argc, const char *argv[])
{
  (void)cmd;
  (void)argc;
  (void)argv;

  cli_println(cli, "OpenLibCLI Library version: %s", CLI_VERSION_STRING);
  cli_println(cli, "Platform   : Raspberry Pi Pico W (Telnet over WiFi)");
  cli_println(cli, "Transports : Telnet (TCP :2323 via lwIP)");
  return CLI_OK;
}

static int8_t cmd_led(cli_struct_t *cli, const char *cmd,
                      cli_argc_t argc, const char *argv[])
{
  (void)cmd;
  int8_t rc = CLI_OK;

  if (argc < 2)
  {
    cli_error(cli, "Usage: led <on|off>");
    rc = CLI_ERR;
  }
  else if (strcmp(argv[1], "on") == 0)
  {
    CLI_LED_ON();
    cli_println(cli, "LED turned on");
  }
  else if (strcmp(argv[1], "off") == 0)
  {
    CLI_LED_OFF();
    cli_println(cli, "LED turned off");
  }
  else
  {
    cli_error(cli, "Invalid state. Use 'on' or 'off'");
    rc = CLI_ERR;
  }

  return rc;
}

static int8_t cmd_reboot(cli_struct_t *cli, const char *cmd,
                         cli_argc_t argc, const char *argv[])
{
  (void)cmd;
  (void)argc;
  (void)argv;
  cli_println(cli, "Resetting the system...");
  sleep_ms(100);
  watchdog_reboot(0, 0, 0);
  while (1)
  {
  }
  return CLI_OK;
}

static int8_t cmd_calculator(cli_struct_t *cli, const char *cmd,
                             cli_argc_t argc, const char *argv[])
{
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

static int8_t cmd_arithmetic(cli_struct_t *cli, const char *cmd,
                             cli_argc_t argc, const char *argv[])
{
  (void)cmd;
  (void)argc;
  (void)argv;
  cli_set_mode(cli, APP_MODE_ARITHMETIC);
  return CLI_OK;
}

static int8_t cmd_add(cli_struct_t *cli, const char *cmd,
                      cli_argc_t argc, const char *argv[])
{
  (void)cmd;
  int8_t rc = CLI_ERR;

  if (argc < 3 || argc > 4)
  {
    cli_error(cli, "Usage: add <a> <b>  or  add <a> + <b>");
  }
  else if (argc == 4 && strcmp(argv[2], "+") != 0)
  {
    cli_error(cli, "Usage: add <a> + <b>");
  }
  else
  {
    int32_t a, b;
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

static int8_t cmd_sub(cli_struct_t *cli, const char *cmd,
                      cli_argc_t argc, const char *argv[])
{
  (void)cmd;
  int8_t rc = CLI_ERR;

  if (argc < 3 || argc > 4)
  {
    cli_error(cli, "Usage: sub <a> <b>");
  }
  else if (argc == 4 && strcmp(argv[2], "-") != 0)
  {
    cli_error(cli, "Usage: sub <a> - <b>");
  }
  else
  {
    int32_t a, b;
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

static void cli_register_commands(cli_struct_t *cli)
{
  cli_cmd_handle_t h_clear;
  cli_cmd_handle_t h_show;
  cli_cmd_handle_t h_calc;
  cli_cmd_handle_t h_arith;

  cli_register_command(cli, CLI_CMD_ROOT, "exit",
                       cli_cmd_builtin_exit, CLI_PRIV_USER, CLI_MODE_ANY, "Exit Context");
  cli_register_command(cli, CLI_CMD_ROOT, "quit",
                       cli_cmd_builtin_quit, CLI_PRIV_USER, CLI_MODE_ANY, "Quit Session");
  cli_register_command(cli, CLI_CMD_ROOT, "help",
                       cli_cmd_builtin_help, CLI_PRIV_USER, CLI_MODE_ANY, "Show commands");
#if CLI_ENABLE_HISTORY
  cli_register_command(cli, CLI_CMD_ROOT, "history",
                       cli_cmd_builtin_history, CLI_PRIV_USER, CLI_MODE_ANY, "Command history");
#endif
  h_clear = cli_register_command(cli, CLI_CMD_ROOT, "clear",
                                 cli_cmd_builtin_clear_screen, CLI_PRIV_USER, CLI_MODE_ANY, "Clear screen");
  (void)h_clear;

#if CLI_ENABLE_ALIASES
  cli_cmd_add_alias(cli, h_clear, "cls");
#endif
  h_show = cli_register_command(cli, CLI_CMD_ROOT, "show",
                                NULL, CLI_PRIV_USER, CLI_MODE_ANY, "Show info");
  cli_register_command(cli, h_show, "version",
                       cmd_show_version, CLI_PRIV_USER, CLI_MODE_ANY, "Version");

  cli_register_command(cli, CLI_CMD_ROOT, "led",
                       cmd_led, CLI_PRIV_USER, CLI_MODE_ANY, "Set LED state (on/off)");
  cli_register_command(cli, CLI_CMD_ROOT, "reboot",
                       cmd_reboot, CLI_PRIV_USER, CLI_MODE_ANY, "Reset the system");

  h_calc = cli_register_command(cli, CLI_CMD_ROOT, "calc",
                                cmd_calculator, CLI_PRIV_USER, CLI_MODE_ANY, "Enter calc menu");
  h_arith = cli_register_command(cli, h_calc, "arith",
                                 cmd_arithmetic, CLI_PRIV_USER, CLI_MODE_ANY, "Enter arithmetic submenu");
  cli_register_command(cli, h_arith, "add",
                       cmd_add, CLI_PRIV_USER, CLI_MODE_ANY, "Add two numbers (add <a> <b>)");
  cli_register_command(cli, h_arith, "sub",
                       cmd_sub, CLI_PRIV_USER, CLI_MODE_ANY, "Subtract two numbers (sub <a> <b>)");
}

static uint32_t pico_now_sec(void *ctx)
{
  (void)ctx;
  return (uint32_t)(time_us_64() / 1000000ULL);
}
