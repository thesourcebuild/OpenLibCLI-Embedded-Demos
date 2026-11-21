/**
 * @file serial.ino
 * @brief Arduino sketch - OpenLibCLI serial (UART) demo.
 *
 * This sketch is self-contained: it wires the transport, registers commands,
 * and runs the OpenLibCLI session directly.
 *
 * Supported boards: any Arduino-compatible board with a hardware UART. On
 * USB-CDC boards, the sketch waits briefly for the host to open the COM port
 * before starting the CLI.
 *
 * @copyright Copyright (c) 2026 thesourcebuild and Muhammad Hassaan Shah
 *
 * SPDX-License-Identifier: MIT
 */

#if defined(ARDUINO)

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#include <Arduino.h>
#include <string.h>
#include <inttypes.h>
#if defined(USE_TINYUSB)
/* Adafruit TinyUSB stack (RP2040 "USB Stack: Adafruit TinyUSB"): Serial comes from its
 * library, which is linked only when the sketch includes it. */
#include <Adafruit_TinyUSB.h>
#endif

//>! UNCOMMENT:: If you want to use your own configuration 'cli_config' present in your sketch dir.
// Note: The use of "" instead of <> 
// #include "cli_config.h"

#include <cli_version.h>
#include <cli.h>
#include <utils/cli_args_parse.h>

#include "cli_transport_serial.hpp"

/*=======================================================================================
 * Private Defines
 *=======================================================================================*/
#ifndef CLI_MAX_COMMANDS
#if CLI_ENABLE_TIMMING_STATS
#define CLI_MAX_COMMANDS 14 /* every command below, 'clear' (ANSI terminals) and 'show stats' included */
#else
#define CLI_MAX_COMMANDS 13 /* every command below, 'clear' included (ANSI terminals) */
#endif
#endif

/** Uncomment exactly one: */
#define CLI_PARSE_BACKEND_HANDROLLED  1
#define CLI_PARSE_BACKEND_STRTOL      2
#define CLI_PARSE_BACKEND_SSCANF      3

/** Select active backend here: */
#define CLI_PARSE_BACKEND  CLI_PARSE_BACKEND_HANDROLLED

/* 'add <a> + <b>' is four words: it needs CLI_MAX_ARGS >= 5 (the AVR profile allows 3). */
#if CLI_MAX_ARGS >= 5
#define CLI_DEMO_ADD_USAGE "Usage: add <a> <b>  or  add <a> + <b>"
#define CLI_DEMO_SUB_USAGE "Usage: sub <a> <b>  or  sub <a> - <b>"
#else
#define CLI_DEMO_ADD_USAGE "Usage: add <a> <b>"
#define CLI_DEMO_SUB_USAGE "Usage: sub <a> <b>"
#endif

/** @brief LED the led command drives: the board's LED_BUILTIN, or a fallback pin. */
#if defined(ARDUINO_ARCH_STM32)
  /* STM32duino defines LED_BUILTIN for every board: an unconnected pin (PNUM_NOT_DEFINED)
   * on a generic one, which then gets PA5 (the Nucleo-64 LED). */
  #define LED_PIN ((LED_BUILTIN != PNUM_NOT_DEFINED) ? LED_BUILTIN : PA5)
#elif defined(STM32) || defined(STM32F1)
  #define LED_PIN PA5   // STM32 Nucleo F103RB: LED on GPIOA pin 5
#elif (defined(ESP32) || defined(ARDUINO_ARCH_ESP32)) && defined(LED_BUILTIN)
  #define LED_PIN LED_BUILTIN // the board's LED; digitalWrite() also drives an RGB one (S3/C3/C6 DevKits)
#elif defined(ESP32) || defined(ARDUINO_ARCH_ESP32) || defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
  #define LED_PIN 2    // ESP32/ESP8266 Dev Module: LED on GPIO2
#else
  #ifndef LED_BUILTIN
  #define LED_BUILTIN 13
  #endif
  #define LED_PIN LED_BUILTIN
#endif

/** @brief Pin levels that light and clear the LED. The Blue Pill and Black Pill boards wire
 * it from 3.3 V to the pin: a low pin lights it. */
#if defined(ARDUINO_BLUEPILL_F103C6) || defined(ARDUINO_BLUEPILL_F103C8) ||                   \
    defined(ARDUINO_BLUEPILL_F103CB) || defined(ARDUINO_BLACKPILL_F103C8) ||                   \
    defined(ARDUINO_BLACKPILL_F103CB) || defined(ARDUINO_BLACKPILL_F303CC) ||                  \
    defined(ARDUINO_BLACKPILL_F401CC) || defined(ARDUINO_BLACKPILL_F401CE) ||                  \
    defined(ARDUINO_BLACKPILL_F411CE) || defined(ARDUINO_BLACKPILL_F411CE_8M)
  #define LED_LEVEL_ON  LOW
  #define LED_LEVEL_OFF HIGH
#else
  #define LED_LEVEL_ON  HIGH
  #define LED_LEVEL_OFF LOW
#endif

/** @brief Serial line speed used by the Arduino demo. */
#define CLI_BAUD_RATE (115200UL)

/** @brief Maximum time to wait for USB CDC serial to attach. */
#define USB_WAIT_MS (3000UL)

/** @brief MOTD banner displayed by the Arduino serial demo session. */
#define CLI_DEMO_BANNER_SERIAL                                                                     \
  "************************************************************\r\n"                               \
  "*             OpenLibCLI  --  Serial (UART)                *\r\n"                               \
  "*         Pure-C, 100% Static Allocation CLI Engine        *\r\n"                               \
  "************************************************************"

/** @brief Calculator mode. */
#define APP_MODE_CALCULATOR (CLI_MODE_USER_BASE)

/** @brief Arithmetic sub-menu mode. It will be sub-menu(child) of APP_MODE_CALCULATOR */
#define APP_MODE_ARITHMETIC (CLI_MODE_USER_BASE + 1)

/*=======================================================================================
 * Private Types
 *=======================================================================================*/

typedef struct {
  uint32_t session_id;
  uint32_t rx_packets;
  uint32_t tx_packets;
} cli_serial_app_data_t;

/*=======================================================================================
 * Private Variables
 *=======================================================================================*/

static cli_serial_app_data_t s_app;
static cli_struct_t s_cli;
static cli_cmd_struct_t s_cmd_pool[CLI_MAX_COMMANDS];

/*=======================================================================================
 * Private Function Prototypes
 *=======================================================================================*/

static int8_t cmd_show_version(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
#if CLI_ENABLE_TIMMING_STATS
static int8_t cmd_show_stats(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
#endif
static int8_t cmd_led(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_reboot(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_calculator(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_arithmetic(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_add(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_sub(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);

static void cli_register_commands(cli_struct_t *cli);

static void cli_serial_setup(void);
static void cli_serial_poll(void);

static uint32_t arduino_now_sec(void *ctx);
#if CLI_ENABLE_TIMMING_STATS
static uint32_t arduino_micros(void *ctx);
#endif


static bool parse_operand(const char *s, int32_t *out);

/*=======================================================================================
 * Public Functions
 *=======================================================================================*/

/**
 * @brief Initialise Arduino Serial and start the shared CLI application.
 *
 * Waits briefly for USB CDC boards, then starts the serial CLI session.
 */
void setup(void) {
  Serial.begin(CLI_BAUD_RATE);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_LEVEL_OFF);

  uint32_t t0 = (uint32_t)millis();
  while (!Serial && (millis() - t0) < USB_WAIT_MS) {
    delay(10); /* lets the core run (watchdog) while USB enumerates */
  }

  cli_serial_setup();
}

/**
 * @brief Poll the CLI session once per pass of the Arduino super-loop.
 *
 * The CLI engine is non-blocking in this demo, so each pass services any
 * available serial input. Returning lets the core run its own work between
 * passes (e.g. the ESP8266 WiFi stack and software watchdog, serialEvent()).
 */
void loop(void) {
  cli_serial_poll();
}

/*=======================================================================================
 * Private Functions
 *=======================================================================================*/

static int8_t cmd_show_version(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]) {
  (void)cmd;
  (void)argc;
  (void)argv;

  cli_println(cli, "OpenLibCLI Library version: %s", CLI_VERSION_STRING);
  cli_println(cli, "Platforms   : Linux / macOS / Windows / Embedded");
  cli_println(cli, "Transports  : Telnet, TCP, UDP, Serial (UART, stdin/stdout), pipe");
  return CLI_OK;
}

#if CLI_ENABLE_TIMMING_STATS
/* Engine tick timing, shown on request: printed straight to Serial it would land in the
 * middle of the line being typed. */
static int8_t cmd_show_stats(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]) {
  cli_tick_stats_struct_t stats;

  (void)cmd;
  (void)argc;
  (void)argv;
  cli_tick_stats_get(cli, &stats);
  cli_println(cli, "poll us  min=%" PRIu32 "  max=%" PRIu32 "  avg=%" PRIu32 "  n=%" PRIu32,
              stats.min_us, stats.max_us, stats.avg_us, stats.count);
  return CLI_OK;
}
#endif

static int8_t cmd_led(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]) {
  (void)cmd;
  int8_t rc = CLI_OK;

  if (argc < 2) {
    cli_println(cli, "Usage: led <on|off>");
    rc = CLI_ERR;
  } else if (strcmp(argv[1], "on") == 0) {
    digitalWrite(LED_PIN, LED_LEVEL_ON);
    cli_println(cli, "LED turned on");
  } else if (strcmp(argv[1], "off") == 0) {
    digitalWrite(LED_PIN, LED_LEVEL_OFF);
    cli_println(cli, "LED turned off");
  } else {
    cli_println(cli, "Invalid state. Use 'on' or 'off'");
    rc = CLI_ERR;
  }

  return rc;
}

static int8_t cmd_reboot(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]) {
  (void)cmd;
  (void)argc;
  (void)argv;
  int8_t rc = CLI_OK;

  cli_println(cli, "Resetting the system...");
  delay(100);

#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
  ESP.restart();
#elif defined(ARDUINO_ARCH_MBED)
  NVIC_SystemReset(); /* mbed cores (also the mbed RP2040 one, which defines ARDUINO_ARCH_RP2040) */
#elif defined(ARDUINO_ARCH_RP2040)
  rp2040.reboot(); /* Arduino-Pico core */
#elif defined(ARDUINO_ARCH_RP2350) || defined(ARDUINO_ARCH_STM32) || defined(__arm__)
  NVIC_SystemReset();
#else
  cli_println(cli, "Reset not supported on this platform");
  rc = CLI_ERR;
#endif

  return rc;
}

int8_t cmd_calculator(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]) {
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

int8_t cmd_arithmetic(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]) {
  (void)cmd;
  (void)argc;
  (void)argv;
  cli_set_mode(cli, APP_MODE_ARITHMETIC);
  return CLI_OK;
}

int8_t cmd_add(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]) {
  (void)cmd;
  int8_t rc = CLI_ERR;

  if (argc < 3 || argc > 4) {
    cli_println(cli, CLI_DEMO_ADD_USAGE);
  } else if (argc == 4 && strcmp(argv[2], "+") != 0) {
    cli_println(cli, "Usage: add <a> + <b>");
  } else {
    int32_t a = 0;
    int32_t b = 0;
    const char *rhs = (argc == 4) ? argv[3] : argv[2];

    if (!parse_operand(argv[1], &a) || !parse_operand(rhs, &b)) {
      cli_error(cli, "Invalid number");
    } else if ((b > 0 && a > INT32_MAX - b) || (b < 0 && a < INT32_MIN - b)) {
      cli_error(cli, "Result out of range");
    } else {
      cli_println(cli, "%" PRId32, a + b);
      rc = CLI_OK;
    }
  }

  return rc;
}

int8_t cmd_sub(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]) {
  (void)cmd;
  int8_t rc = CLI_ERR;

  if (argc < 3 || argc > 4) {
    cli_println(cli, CLI_DEMO_SUB_USAGE);
  } else if (argc == 4 && strcmp(argv[2], "-") != 0) {
    cli_println(cli, "Usage: sub <a> - <b>");
  } else {
    int32_t a = 0;
    int32_t b = 0;
    const char *rhs = (argc == 4) ? argv[3] : argv[2];

    if (!parse_operand(argv[1], &a) || !parse_operand(rhs, &b)) {
      cli_error(cli, "Invalid number");
    } else if ((b < 0 && a > INT32_MAX + b) || (b > 0 && a < INT32_MIN + b)) {
      cli_error(cli, "Result out of range");
    } else {
      cli_println(cli, "%" PRId32, a - b);
      rc = CLI_OK;
    }
  }

  return rc;
}

static void cli_register_commands(cli_struct_t *cli) {
  cli_cmd_handle_t h_clear;
  cli_cmd_handle_t h_show;
  cli_cmd_handle_t h_calc;
  cli_cmd_handle_t h_arith;

  cli_register_command(cli, CLI_CMD_ROOT, "exit", cli_cmd_builtin_exit, CLI_PRIV_USER, CLI_MODE_ANY,
                  "Exit");
  cli_register_command(cli, CLI_CMD_ROOT, "quit", cli_cmd_builtin_quit, CLI_PRIV_USER, CLI_MODE_ANY,
                  "Quit");

  cli_register_command(cli, CLI_CMD_ROOT, "help", cli_cmd_builtin_help, CLI_PRIV_USER, CLI_MODE_ANY,
                  "Help");
#if CLI_ENABLE_HISTORY
  cli_register_command(cli, CLI_CMD_ROOT, "history", cli_cmd_builtin_history, CLI_PRIV_USER,
                  CLI_MODE_ANY, "History");
#endif

//>! Uncomment on ANSI/VT100 compatabile terminal like PuTTY
  if (cli->ansi_supported == true) {
    h_clear = cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("clear"), cli_cmd_builtin_clear_screen,
                              CLI_PRIV_USER, CLI_MODE_ANY, CLI_STR("Clear screen"));
    (void)h_clear; /* used only for the alias */

#if CLI_ENABLE_ALIASES
    cli_cmd_add_alias(cli, h_clear, "cls");
#endif
  }
					  
  h_show = cli_register_command(cli, CLI_CMD_ROOT, "show", NULL, CLI_PRIV_USER,
                                                  CLI_MODE_ANY, "Show info");
  cli_register_command(cli, h_show, "version", cmd_show_version, CLI_PRIV_USER, CLI_MODE_ANY,
                       "Version");
#if CLI_ENABLE_TIMMING_STATS
  cli_register_command(cli, h_show, "stats", cmd_show_stats, CLI_PRIV_USER, CLI_MODE_ANY,
                       "Engine tick timing");
#endif

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

static void cli_serial_setup(void) {
  cli_platform_struct_t platform;
  cli_transport_struct_t transport;

  memset(&platform, 0, sizeof(platform));
  platform.now_sec = arduino_now_sec;
  platform.now_sec_ctx = NULL;

#if CLI_ENABLE_TIMMING_STATS
  platform.micros = arduino_micros;
  platform.micros_ctx = NULL;
#endif

  cli_transport_serial_init(&transport);

  cli_init(&s_cli, "MCU", &transport, &platform, s_cmd_pool, (sizeof(s_cmd_pool) / sizeof(s_cmd_pool[0])));

  // Arduino Serial Monitor is a dumb terminal no (ANSI/VT100) support
  // Uncomment the below line
  cli_set_ansi_supported(&s_cli, false);

  cli_set_exit_root_policy(&s_cli, CLI_EXIT_ROOT_POLICY_RESET_SESSION);
  cli_set_quit_policy(&s_cli, CLI_QUIT_POLICY_RESET_SESSION);
  /* Too many failed logins lock input for a while instead of ending the session. */
  cli_set_auth_failure_mode(&s_cli, CLI_AUTH_FAILURE_MODE_LOCKOUT);

  memset(&s_app, 0, sizeof(s_app));
  s_app.session_id = 1;
  s_app.rx_packets = 12345;
  s_app.tx_packets = 11000;
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

  cli_register_commands(&s_cli);

  cli_start(&s_cli);
}

static void cli_serial_poll(void) {
  if (cli_session_engine(&s_cli) != CLI_OK) {
    /* A serial line cannot reconnect: a session that ended (e.g. after failed logins)
     * starts again with the banner and login. */
    cli_start(&s_cli);
  }
}

static uint32_t arduino_now_sec(void *ctx) {
  (void)ctx;
  return (uint32_t)(millis() / 1000UL);
}

#if CLI_ENABLE_TIMMING_STATS
static uint32_t arduino_micros(void *ctx) {
  (void)ctx;
  return (uint32_t)micros();
}
#endif

#if CLI_PARSE_BACKEND == CLI_PARSE_BACKEND_SSCANF
/* sscanf() neither rejects trailing text nor reports a value out of range (C leaves that
 * conversion undefined): the operand is checked first. True for an optional sign and
 * 1 to 10 digits whose value fits in int32_t. */
static bool operand_fits_int32(const char *s) {
  const char *digits = (*s == '+' || *s == '-') ? s + 1 : s;
  size_t count;
  bool ok;

  while (digits[0] == '0' && digits[1] != '\0') {
    digits++; /* leading zeros do not count */
  }
  count = strspn(digits, "0123456789");
  ok = (count > 0U && digits[count] == '\0');
  if (ok && count == 10U) {
    ok = (strcmp(digits, (*s == '-') ? "2147483648" : "2147483647") <= 0);
  } else if (count > 10U) {
    ok = false;
  }
  return ok;
}
#endif

/* Parse one operand with the selected backend: false when it is not a whole number in
 * the int32_t range (nothing is stored then). */
static bool parse_operand(const char *s, int32_t *out) {
  bool ok;

#if CLI_PARSE_BACKEND == CLI_PARSE_BACKEND_HANDROLLED
  ok = parse_i32(s, out);
#elif CLI_PARSE_BACKEND == CLI_PARSE_BACKEND_STRTOL
  ok = strtol_parse_i32(s, out);
#else
  ok = operand_fits_int32(s) && (cli_sscanf(s, "%" SCNd32, out) == 1);
#endif

  return ok;
}

#endif

/*################################### END OF FILE ######################################*/
