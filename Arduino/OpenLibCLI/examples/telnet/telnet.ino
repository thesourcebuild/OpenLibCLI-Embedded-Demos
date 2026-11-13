/**
 * @file arduino_telnet.ino
 * @brief Arduino Telnet server sketch for OpenLibCLI.
 *
 * This sketch is self-contained: it starts WiFi station mode, accepts one
 * Telnet client at a time, wires client I/O into the CLI transport, and runs
 * the CLI session lifecycle directly.
 *
 * @copyright Copyright (c) 2026 Muhammad Hassaan Shah
 */

#if defined(ARDUINO)

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#include <Arduino.h>
#include <string.h>
#include <inttypes.h>

#include <WiFi.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <lwip/sockets.h> /* keepalive socket options */
#endif

//>! UNCOMMENT:: If you want to use your own configuration 'cli_config' present in your sketch dir.
// Note: The use of "" instead of <> 
// #include "cli_config.h"

#include <cli_version.h>
#include <cli.h>
#include <utils/cli_args_parse.h>

#include "cli_transport_telnet.hpp"

/*=======================================================================================
 * Private Defines
 *=======================================================================================*/
#ifndef CLI_MAX_COMMANDS
#define CLI_MAX_COMMANDS 13 /* every command below, 'clear' included (ANSI terminals) */
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

/** @brief Fallback built-in LED pin for boards that do not define LED_BUILTIN. */
#if defined(STM32) || defined(STM32F1) || defined(ARDUINO_ARCH_STM32)
  #define LED_PIN PA5   // STM32 Nucleo F103RB: LED on GPIOA pin 5
#elif defined(ESP32) || defined(ARDUINO_ARCH_ESP32) || defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
  #define LED_PIN 2    // ESP32/ESP8266 Dev Module: LED on GPIO2
#else
  #ifndef LED_BUILTIN
  #define LED_BUILTIN 13
  #endif
  #define LED_PIN LED_BUILTIN
#endif

#ifndef CLI_WIFI_SSID
#define CLI_WIFI_SSID "YOUR_WIFI_SSID"
#endif

#ifndef CLI_WIFI_PASSWORD
#define CLI_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

#ifndef CLI_TELNET_PORT
#define CLI_TELNET_PORT 2323U
#endif

/* Dead-client detection (TCP keepalive): a client that sends nothing for
 * CLI_TELNET_KEEPALIVE_IDLE_S is probed; when CLI_TELNET_KEEPALIVE_COUNT probes, sent
 * CLI_TELNET_KEEPALIVE_INTERVAL_S apart, go unanswered (powered off, out of range, its
 * WiFi dropped), the connection is dropped and the session ends, so the server is free for
 * the next client. A connected client answers the probes whether or not anyone types. */
#ifndef CLI_TELNET_KEEPALIVE_IDLE_S
#define CLI_TELNET_KEEPALIVE_IDLE_S 60
#endif
#ifndef CLI_TELNET_KEEPALIVE_INTERVAL_S
#define CLI_TELNET_KEEPALIVE_INTERVAL_S 10
#endif
#ifndef CLI_TELNET_KEEPALIVE_COUNT
#define CLI_TELNET_KEEPALIVE_COUNT 3
#endif

#define CLI_WIFI_CONNECT_TIMEOUT_MS 15000UL

#define CLI_STATS_PERIOD_MS 1000UL

/** @brief Serial line speed used by the Arduino demo. */
#define CLI_BAUD_RATE (115200UL)

/** @brief Maximum time to wait for USB CDC serial to attach. */
#define USB_WAIT_MS (3000UL)

/** @brief MOTD banner displayed by the Arduino telnet demo session. */
#define CLI_DEMO_BANNER_TELNET                                                                     \
  "************************************************************\r\n"                               \
  "*             OpenLibCLI  --  Telnet Transport             *\r\n"                                \
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
} demo_app_data_t;

/*=======================================================================================
 * Private Variables
 *=======================================================================================*/

static WiFiServer s_telnet_server((uint16_t)CLI_TELNET_PORT);
static WiFiClient s_telnet_client;
static bool s_client_active = false;
static bool s_server_started = false; /* WiFi connected and the server listening */

static demo_app_data_t s_app;
static cli_struct_t s_cli;
static cli_cmd_struct_t s_cmd_pool[CLI_MAX_COMMANDS];
static cli_telnet_ctx_struct_t s_telnet_ctx;
static cli_transport_struct_t s_transport;
static uint32_t s_session_id = 0;

#if CLI_ENABLE_TIMMING_STATS
static uint32_t s_last_stats_ms = 0;
#endif

/*=======================================================================================
 * Private Function Prototypes
 *=======================================================================================*/
static bool wifi_connect_blocking(void);

static int8_t cmd_show_version(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_led(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_reboot(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_calculator(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_arithmetic(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_add(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static int8_t cmd_sub(cli_struct_t *cli, const char *cmd, cli_argc_t argc, const char *argv[]);
static void cli_register_commands(cli_struct_t *cli);

static void cli_telnet_demo_setup(void *client_handle);
static void cli_telnet_poll(void);
static void cli_telnet_demo_shutdown(void);
static void telnet_client_keepalive(WiFiClient &client);

static uint32_t arduino_now_sec(void *ctx);
#if CLI_ENABLE_TIMMING_STATS
static uint32_t arduino_micros(void *ctx);
#endif

/*=======================================================================================
 * Public Functions
 *=======================================================================================*/

void setup(void) {
  Serial.begin(CLI_BAUD_RATE);
  pinMode(LED_PIN, OUTPUT);

  uint32_t t0 = (uint32_t)millis();
  while (!Serial && (millis() - t0) < USB_WAIT_MS) {
    ;
  }

  Serial.println();
  Serial.println("OpenLibCLI Arduino Telnet demo");
  Serial.println("Connecting WiFi...");

  if (!wifi_connect_blocking()) {
    Serial.println("WiFi connection failed. Check CLI_WIFI_SSID / CLI_WIFI_PASSWORD.");
  } else {
    s_telnet_server.begin();
    s_server_started = true;
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Telnet server listening on port ");
    Serial.println((uint16_t)CLI_TELNET_PORT);
  }
}

void loop(void) {
  WiFiClient incoming;

  /* Without WiFi the server was never started: there is nothing to accept or serve. */
  if (s_server_started) {
    /* accept() on the cores that have it (ESP32 core 3 deprecates available() for it). */
#if defined(ARDUINO_ARCH_RP2040) || (defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3)
    incoming = s_telnet_server.accept();
#else
    incoming = s_telnet_server.available();
#endif
  }
  if (incoming) {
    if (!s_client_active || !cli_transport_telnet_client_connected(&s_telnet_ctx)) {
      if (s_client_active) {
        s_telnet_client.stop();
      }

      s_telnet_client = incoming;
      telnet_client_keepalive(s_telnet_client);
      s_client_active = true;
      cli_telnet_demo_setup(&s_telnet_client);

      Serial.println("Telnet client connected.");
    } else {
      incoming.stop();
      Serial.println("Rejected second Telnet client.");
    }
  }

  if (s_client_active) {
    /* Ends the session (and the connection) when the client went away or the session
     * stopped itself (e.g. a write to the client failed). */
    cli_telnet_poll();

#if CLI_ENABLE_TIMMING_STATS
    if (s_client_active) {
      uint32_t now_ms = (uint32_t)millis();
      if ((now_ms - s_last_stats_ms) >= CLI_STATS_PERIOD_MS) {
        cli_tick_stats_struct_t stats;
        cli_tick_stats_get(&s_cli, &stats);
        Serial.print("poll us  min=");
        Serial.print(stats.min_us);
        Serial.print("  max=");
        Serial.print(stats.max_us);
        Serial.print("  avg=");
        Serial.print(stats.avg_us);
        Serial.print("  n=");
        Serial.println(stats.count);
        s_last_stats_ms = now_ms;
      }
    }
#endif
  }
}

/*=======================================================================================
 * Private Functions
 *=======================================================================================*/

static bool wifi_connect_blocking(void) {
  uint32_t start_ms;
  int8_t retries = 0;

  WiFi.mode(WIFI_STA);
  delay(100);

  do {
    if (retries > 0) {
      Serial.print("Retry ");
      Serial.print(retries);
      Serial.println("...");
      WiFi.disconnect();
      delay(500);
      WiFi.mode(WIFI_STA);
      delay(100);
    }

    WiFi.begin(CLI_WIFI_SSID, CLI_WIFI_PASSWORD);
    start_ms = (uint32_t)millis();

    while (WiFi.status() != WL_CONNECTED) {
      if (((uint32_t)millis() - start_ms) >= CLI_WIFI_CONNECT_TIMEOUT_MS) {
        break;
      }
      delay(250);
    }

    retries++;
  } while (WiFi.status() != WL_CONNECTED && retries < 5);

  bool connected = (WiFi.status() == WL_CONNECTED);
  if (!connected) {
    Serial.print("WiFi status code: ");
    Serial.println((int)WiFi.status());
  }

  return connected;
}


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
    cli_println(cli, "Usage: led <on|off>");
    rc = CLI_ERR;
  } else if (strcmp(argv[1], "on") == 0) {
    digitalWrite(LED_PIN, HIGH);
    cli_println(cli, "LED turned on");
  } else if (strcmp(argv[1], "off") == 0) {
    digitalWrite(LED_PIN, LOW);
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
#elif defined(ARDUINO_ARCH_RP2040)
  rp2040.reboot();
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

/* Parse one operand with the selected backend: false when it is not a whole number in
 * the int32_t range (nothing is stored then). */
static bool parse_operand(const char *s, int32_t *out) {
  bool ok;

#if CLI_PARSE_BACKEND == CLI_PARSE_BACKEND_HANDROLLED
  ok = parse_i32(s, out);
#elif CLI_PARSE_BACKEND == CLI_PARSE_BACKEND_STRTOL
  ok = strtol_parse_i32(s, out);
#else
  ok = (cli_sscanf(s, "%" SCNd32, out) == 1);
#endif

  return ok;
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

#if CLI_ENABLE_ALIASES
    cli_cmd_add_alias(cli, h_clear, "cls");
#endif
  }

  h_show = cli_register_command(cli, CLI_CMD_ROOT, "show", NULL, CLI_PRIV_USER,
                                                  CLI_MODE_ANY, "Show info");
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

static void cli_telnet_demo_setup(void *client_handle) {
  cli_platform_struct_t platform;

  if (client_handle) {
    cli_transport_telnet_init(&s_telnet_ctx, &s_transport, client_handle);
    cli_transport_telnet_negotiate(&s_telnet_ctx);

    memset(&platform, 0, sizeof(platform));
    platform.now_sec = arduino_now_sec;
    platform.now_sec_ctx = NULL;
#if CLI_ENABLE_TIMMING_STATS
    platform.micros = arduino_micros;
    platform.micros_ctx = NULL;
#endif

    cli_init(&s_cli, "router", &s_transport, &platform, s_cmd_pool, (sizeof(s_cmd_pool) / sizeof(s_cmd_pool[0])));

	// Arduino Serial Monitor is a dumb terminal no (ANSI/VT100) support
	// Uncomment the below line
	cli_set_ansi_supported(&s_cli, false);
	  
    /* 'exit' / 'quit' and the idle timeout restart the banner and login on the same
     * connection. */
    cli_set_exit_root_policy(&s_cli, CLI_EXIT_ROOT_POLICY_RESET_SESSION);
    cli_set_quit_policy(&s_cli, CLI_QUIT_POLICY_RESET_SESSION);
    cli_set_idle_timeout(&s_cli, 60);
    cli_set_idle_timeout_mode(&s_cli, CLI_IDLE_TIMEOUT_POLICY_RESET_SESSION);
    /* Too many failed logins lock input for a while instead of closing the connection. */
    cli_set_auth_failure_mode(&s_cli, CLI_AUTH_FAILURE_MODE_LOCKOUT);

    s_session_id++;
    memset(&s_app, 0, sizeof(s_app));
    s_app.session_id = s_session_id;
    s_app.rx_packets = 12345;
    s_app.tx_packets = 11000;
    cli_set_userdata(&s_cli, &s_app);
	
#if CLI_ENABLE_BANNER
    cli_set_banner(&s_cli, CLI_DEMO_BANNER_TELNET);
#endif

#if CLI_ENABLE_AUTH
    cli_set_enable_secret(&s_cli, "cisco");
    cli_add_user(&s_cli, "admin", "admin", CLI_PRIV_PRIVILEGED);
    cli_require_authorization(&s_cli, true);
#endif

#if CLI_ENABLE_MODE_NAMES
    cli_set_mode_name(&s_cli, CLI_MODE_CONFIG, "config");
#endif

	cli_register_commands(&s_cli);

    cli_start(&s_cli);
  }
}

static void cli_telnet_poll(void) {
  if (cli_transport_telnet_client_connected(&s_telnet_ctx)) {
    cli_telnet_handler(&s_telnet_ctx);
    if (cli_session_engine(&s_cli) != CLI_OK) {
      cli_telnet_demo_shutdown();
    }
  } else {
    cli_telnet_demo_shutdown();
  }
}

static void cli_telnet_demo_shutdown(void) {
  /* The session is over: drop the connection too, so the client is not left hanging and
   * the next client is accepted. */
  cli_done(&s_cli);
  s_telnet_client.stop();
  s_client_active = false;
  Serial.println("Telnet session ended.");
}

/**
 * @brief Enable dead-client detection on @p client: a client that vanished without
 *        closing then reads as disconnected and its session ends (cli_telnet_poll()).
 *        Cores without a keepalive API leave the connection as it is.
 */
static void telnet_client_keepalive(WiFiClient &client) {
#if defined(ARDUINO_ARCH_RP2040)
  client.keepAlive(CLI_TELNET_KEEPALIVE_IDLE_S, CLI_TELNET_KEEPALIVE_INTERVAL_S,
                   CLI_TELNET_KEEPALIVE_COUNT);
#elif defined(ARDUINO_ARCH_ESP32)
  int on = 1;
  int keep_idle = CLI_TELNET_KEEPALIVE_IDLE_S;
  int keep_interval = CLI_TELNET_KEEPALIVE_INTERVAL_S;
  int keep_count = CLI_TELNET_KEEPALIVE_COUNT;

  (void)client.setSocketOption(SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
  (void)client.setSocketOption(IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
  (void)client.setSocketOption(IPPROTO_TCP, TCP_KEEPINTVL, &keep_interval, sizeof(keep_interval));
  (void)client.setSocketOption(IPPROTO_TCP, TCP_KEEPCNT, &keep_count, sizeof(keep_count));
#else
  (void)client;
#endif
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

#endif

/*################################### END OF FILE ######################################*/
