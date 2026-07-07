/**
 * @file cli.c
 * @brief Core OpenLibCLI engine implementation.
 *
 * 100% static allocation: No malloc, no free, no global mutable state between sessions.
 *
 * This translation unit implements:
 *  - Static command and session pools
 *  - Raw I/O primitives and prompt rendering
 *  - Line editing, history, tokenisation, and tab-completion
 *  - Pipe-filter processing
 *  - Built-in command handlers (exit, quit, enable, disable, help, …)
 *  - Authentication state machine
 *  - VT100 / ANSI escape-sequence parser
 *  - Session lifecycle (cli_session_engine, cli_feed)
 *  - All public configuration setters and command-registration functions
 *
 * @copyright Copyright (c) 2026 Muhammad Hassaan Shah.
 *
 * SPDX-License-Identifier: MIT
 */

/*=======================================================================================
 * Includes
 *=======================================================================================*/
#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64) || defined(__WINDOWS__) || defined(WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif ENV_IS_OS_ENVIRONMENT
#include <time.h>
#endif

#include <time.h>

#include "cli_env_detect.h"
#include "cli.h"

/*=======================================================================================
 * Private Defines
 *=======================================================================================*/
/* VT100 / ANSI Escape Parser States ----------------------------------------------------*/
#define ESC_STATE_NORMAL (uint8_t)0 /**< No active escape sequence. */
#define ESC_STATE_ESC (uint8_t)1    /**< Received ESC (0x1B); awaiting '[' or 'O'. */
#define ESC_STATE_CSI (uint8_t)2    /**< Received ESC '[' (CSI); accumulating parameters. */

/* ASCII Control Codes ------------------------------------------------------------------*/
#define ASCII_NUL 0x00            /**< Null character. */
#define ASCII_BELL 0x07           /**< BEL — terminal bell / alert. */
#define ASCII_BS 0x08             /**< Backspace. */
#define ASCII_TAB 0x09            /**< Horizontal tab. */
#define ASCII_LF 0x0A             /**< Line feed. */
#define ASCII_CR 0x0D             /**< Carriage return. */
#define ASCII_ESC 0x1B            /**< Escape. */
#define ASCII_DEL 0x7F            /**< Delete / rubout. */
#define ASCII_CTRL(c) ((c) - '@') /**< Compute ASCII value of a control character. */
#define ASCII_QUESTION 0x3F       /**< '?' question mark. */
#define ASCII_PIPE 0x7C           /**< '|' vertical bar / pipe. */
#define ASCII_LSBRACKET 0x5B      /**< '[' left square bracket. */
#define ASCII_CAP_O 0x4F          /**< 'O' capital letter. */
#define ASCII_DIGIT_0 0x30        /**< '0' digit zero. */
#define ASCII_DIGIT_9 0x39        /**< '9' digit nine. */

#define CLI_CRLF "\r\n"
#define CLI_ANSI_CLS "\x1b[2J\x1b[H"

/*=======================================================================================
 * Private Macros
 *=======================================================================================*/
/* ======================================================================
 *  Arduino PROGMEM string support
 *
 *  On AVR (Uno, Mega, …) every C string literal is copied from flash
 *  to SRAM at boot unless explicitly placed in PROGMEM.  The helpers
 *  below keep all internal string constants in flash on Arduino builds;
 *  on other platforms the macros are transparent no-ops.
 *
 *  CLI_WSTR(cli, "lit")         — write a literal string from PROGMEM
 *  CMD_STR("lit")               — PSTR wrapper for register_command args
 *  CLI_REGCPY(dst, src_P, n)    — strlcpy from PROGMEM source
 *  cli_println(cli, fmt, …)     — println with PROGMEM format string
 * ====================================================================== */
/*
 * PROGMEM string helpers — AVR only.
 *
 * On AVR (Uno, Mega, …) every C string literal is copied from flash to SRAM
 * at boot unless placed in PROGMEM.  The macros below keep all internal
 * string constants in flash on AVR builds.
 *
 * On non-AVR Arduino targets (STM32, ESP32, RP2040, …) PROGMEM is already
 * a no-op and pgm_read_byte() is a plain dereference, so we fall through to
 * the simple #else branch which uses normal RAM strings — no avr/pgmspace.h
 * needed and no behavioural difference.
 */
#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
#define CMD_STR(s) PSTR(s)
#define CLI_WSTR(cli, s) cli_write_str_P((cli), PSTR(s))
#define CLI_REGCPY(dst, src, n) cli_strlcpy_P((dst), (src), (n))
#else
/* Non-AVR (STM32, ESP32, RP2040, …) or non-Arduino: plain RAM strings */
#define CMD_STR(s) (s)
#define CLI_WSTR(cli, s) cli_write_str((cli), (s))
#define CLI_REGCPY(dst, src, n) cli_strlcpy((dst), (src), (n))
#endif

/**
 * @brief Write a plain string literal followed by a newline, avoiding
 *        vsnprintf overhead and (on AVR) keeping the string in flash.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] s    String literal to write.
 */
#define CLI_PRINTLN_STR(cli, s) \
  do                            \
  {                             \
    CLI_WSTR((cli), (s));       \
    CLI_WSTR((cli), CLI_CRLF);  \
  } while (0)

#define CLI_CMD_IS_IN_USE(cmd_) ((((cmd_)->flags) & CLI_CMD_FLAG_IN_USE) != 0U)
#define CLI_CMD_IS_HIDDEN(cmd_) ((((cmd_)->flags) & CLI_CMD_FLAG_HIDDEN) != 0U)
#define CLI_CMD_SET_IN_USE(cmd_) ((cmd_)->flags = (uint8_t)((cmd_)->flags | CLI_CMD_FLAG_IN_USE))
#define CLI_CMD_SET_HIDDEN(cmd_) ((cmd_)->flags = (uint8_t)((cmd_)->flags | CLI_CMD_FLAG_HIDDEN))

/*=======================================================================================
 * Private Types
 *=======================================================================================*/
typedef enum
{
  CLI_HISTORY_NAV_PREV = -1,
  CLI_HISTORY_NAV_NEXT = 1,
} cli_history_nav_dir_enum_t;

/*=======================================================================================
 * External Data Variables
 *=======================================================================================*/

/*=======================================================================================
 * Private Variables
 *=======================================================================================*/

/*=======================================================================================
 * Private Function Prototypes
 *=======================================================================================*/

/* Raw I/O Primitives -------------------------------------------------------------------*/

static void cli_raw_write(cli_struct_t *cli, const uint8_t *buf, cli_transport_buflen_t len);
static void cli_write_str(cli_struct_t *cli, const char *s);
static void cli_write_char(cli_struct_t *cli, char c);
static void cli_flush(cli_struct_t *cli);

/* Session Timing Helpers ---------------------------------------------------------------*/

static void cli_touch_activity_internal(cli_struct_t *cli);
#if CLI_ENABLE_TIME_SOURCE && ENV_IS_OS_ENVIRONMENT
static uint32_t cli_default_time_source(void *ctx);
#endif
#if CLI_ENABLE_IDLE_TIMEOUT
static int8_t cli_handle_idle_timeout(cli_struct_t *cli);
#endif

/* Output Line Processing ---------------------------------------------------------------*/

#if CLI_ENABLE_PIPE_FILTER
static void cli_print_emit(cli_struct_t *cli, const char *line);
#endif
static void cli_print_process_stream(cli_struct_t *cli, const char *data, int len);

/* Line Editing -------------------------------------------------------------------------*/

static void cli_redraw_line(cli_struct_t *cli);
static void cli_move_cursor(cli_struct_t *cli, cli_len_t pos);
static void cli_move_cursor_delta(cli_struct_t *cli, int16_t delta);
static void cli_move_cursor_end(cli_struct_t *cli);
static void cli_insert_char(cli_struct_t *cli, char c);
static void cli_delete_char(cli_struct_t *cli);
static void cli_backspace(cli_struct_t *cli);
static void cli_kill_line(cli_struct_t *cli);
static void cli_kill_word(cli_struct_t *cli);
static void cli_clear_input(cli_struct_t *cli);

/* AVR PROGMEM Helpers ------------------------------------------------------------------*/

#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
static void cli_write_str_P(cli_struct_t *cli, const char *s_P);
static size_t cli_strlcpy_P(char *dst, const char *src_P, size_t size);
#endif

/* Internal String Helpers ---------------------------------------------------------------*/

static size_t cli_strlcpy(char *dst, const char *src, size_t size);
static bool cli_case_prefix_match(const char *str, const char *prefix);
static inline char *cli_skip_spaces(char *p);
static inline char *cli_skip_non_spaces(char *p);

/* Prompt Construction and Display ------------------------------------------------------*/

static void cli_show_prompt(cli_struct_t *cli);

/* History ------------------------------------------------------------------------------*/

#if CLI_ENABLE_HISTORY
static void cli_history_add(cli_struct_t *cli, const char *line);
static cli_history_idx_t cli_history_index(const cli_struct_t *cli, cli_history_idx_t offset);
static void cli_history_clear_displayed_input(cli_struct_t *cli);
static void cli_history_navigate(cli_struct_t *cli, cli_history_nav_dir_enum_t dir);
#endif
static void cli_history_capture_edited_preview(cli_struct_t *cli);

/* Tokeniser ----------------------------------------------------------------------------*/

static cli_argc_t cli_tokenize(char *buf, const char *argv[], cli_argc_t max_args);

/* Command Tree Traversal ----------------------------------------------------------------*/

/** @brief Callback invoked by @c cli_foreach_match for each match. */
typedef bool (*cli_match_cb_t)(cli_struct_t *cli, cli_cmd_handle_t h, bool *ctx);

static bool cli_print_help_cb(cli_struct_t *cli, cli_cmd_handle_t h, bool *ctx);

static void cli_foreach_match(cli_struct_t *cli, const char *prefix, cli_cmd_handle_t parent,
                              cli_match_cb_t cb, bool *ctx);
static int8_t cli_resolve_one(cli_struct_t *cli, const char *word, cli_cmd_handle_t parent,
                              cli_cmd_handle_t *out);
static bool cli_has_any_match(cli_struct_t *cli, const char *word, cli_cmd_handle_t parent);
static cli_cmd_handle_t cli_context_current(const cli_struct_t *cli);
static void cli_context_set_to_node(cli_struct_t *cli, cli_cmd_handle_t node);
static cli_cmd_handle_t cli_menu_context_parent(const cli_struct_t *cli);

/* Tab Completion -----------------------------------------------------------------------*/

#if CLI_ENABLE_TAB_COMPLETION
static void cli_tab_complete(cli_struct_t *cli);
#endif

/* Inline '?' Help ----------------------------------------------------------------------*/

static void cli_show_help(cli_struct_t *cli);

/* Output Filter (Pipe Processing) ------------------------------------------------------*/

#if CLI_ENABLE_PIPE_FILTER
static void cli_filter_reset(cli_struct_t *cli);
static bool cli_filter_line(cli_struct_t *cli, const char *line);
static void cli_filter_flush(cli_struct_t *cli);
static ptrdiff_t cli_parse_filter(cli_struct_t *cli, const char *line);
#endif

/* Command Execution --------------------------------------------------------------------*/
static int8_t cli_feed(cli_struct_t *cli, int byte);

static int8_t cli_execute_command(cli_struct_t *cli, const char *argv[], cli_argc_t argc,
                                  cli_argc_t depth, cli_cmd_handle_t parent);
static int8_t cli_execute(cli_struct_t *cli);

/* Authentication -----------------------------------------------------------------------*/

#if CLI_ENABLE_AUTH
static void cli_auth_reset_work(cli_struct_t *cli);
static void cli_auth_prompt_username(cli_struct_t *cli);
static bool cli_auth_lockout_expired(const cli_struct_t *cli);
static bool cli_state_is_auth_input(const cli_struct_t *cli);
static int8_t cli_auth_verify(cli_struct_t *cli);
static bool cli_auth_secret_input(const cli_struct_t *cli);
static cli_len_t cli_auth_input_limit(const cli_struct_t *cli);
static void cli_auth_backspace(cli_struct_t *cli);
static void cli_auth_clear_input(cli_struct_t *cli);
static void cli_auth_insert_char(cli_struct_t *cli, char c);
static int8_t cli_auth_submit_line(cli_struct_t *cli);
static int8_t cli_auth_handle(cli_struct_t *cli, int byte);
#endif
static void cli_reset_input(cli_struct_t *cli);
static int8_t cli_submit_line(cli_struct_t *cli);

/* Escape-Sequence State Machine --------------------------------------------------------*/
static bool cli_process_escape(cli_struct_t *cli, int b);
static void handle_csi(cli_struct_t *cli, char final);

/*=======================================================================================
 * Public Functions
 *=======================================================================================*/

/* Built-in Command Handlers ------------------------------------------------------------*/
OPENLIBCLI_API void cli_add_builtin_cmds(cli_struct_t *cli)
{
  cli_cmd_handle_t h_usage;
  cli_cmd_handle_t h_clear;

  (void)cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("exit"), cli_cmd_builtin_exit, CLI_PRIV_USER,
                        CLI_MODE_ANY, CLI_STR("Exit session"));

  (void)cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("quit"), cli_cmd_builtin_quit, CLI_PRIV_USER,
                        CLI_MODE_ANY, CLI_STR("Quit session"));

  (void)cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("enable"), cli_cmd_builtin_enable, CLI_PRIV_USER,
                        CLI_MODE_EXEC, CLI_STR("Activate Privileged mode"));

  (void)cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("disable"), cli_cmd_builtin_disable,
                        CLI_PRIV_PRIVILEGED, CLI_MODE_ENABLE, CLI_STR("Leave Privileged mode"));

  (void)cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("end"), cli_cmd_builtin_end, CLI_PRIV_PRIVILEGED,
                        CLI_MODE_CONFIG, CLI_STR("Return to exec"));

  (void)cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("configure"), cli_cmd_builtin_configure,
                        CLI_PRIV_PRIVILEGED, CLI_MODE_ENABLE, CLI_STR("Config mode"));

  (void)cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("help"), cli_cmd_builtin_help, CLI_PRIV_USER,
                        CLI_MODE_ANY, CLI_STR("Show commands"));

#if CLI_ENABLE_HISTORY
  (void)cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("history"), cli_cmd_builtin_history,
                        CLI_PRIV_USER, CLI_MODE_ANY, CLI_STR("Command history"));
#endif

  if (cli->ansi_supported)
  {
    h_clear = cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("clear"), cli_cmd_builtin_clear_screen,
                              CLI_PRIV_USER, CLI_MODE_ANY, CLI_STR("Clear screen"));
    (void)h_clear;
#if CLI_ENABLE_ALIASES
    (void)cli_cmd_add_alias(cli, h_clear, "cls");
#endif
  }

  h_usage = cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("usage"), cli_cmd_builtin_usage,
                            CLI_PRIV_USER, CLI_MODE_ANY, CLI_STR("Usage information"));

  (void)cli_add_command(cli, h_usage, CLI_STR("commands"), cli_cmd_builtin_usage_commands,
                        CLI_PRIV_USER, CLI_MODE_ANY, CLI_STR("Usage Commands"));
  (void)cli_add_command(cli, h_usage, CLI_STR("keys"), cli_cmd_builtin_usage_keys, CLI_PRIV_USER,
                        CLI_MODE_ANY, CLI_STR("Usage Keys"));
}

OPENLIBCLI_API int8_t cli_cmd_builtin_exit(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                           const char *argv[])
{
  int8_t rc = CLI_OK;

  (void)cmd;
  (void)argc;
  (void)argv;

  if (cli->on_exit_cmd != NULL)
  {
    int8_t hook_rc = cli->on_exit_cmd(cli);
    if (hook_rc != CLI_OK)
    {
      rc = hook_rc;
    }
  }

  if (rc != CLI_OK)
  {
    /* Hook result takes precedence. */
  }
  else if (cli->context != CLI_CMD_ROOT)
  {
    cli->context = cli->cmd_pool[cli->context].parent;
    if (cli->context == CLI_CMD_ROOT && cli->mode >= CLI_MODE_USER_BASE)
    {
      cli_set_mode(cli, CLI_MODE_EXEC);
    }
  }
  else if (cli->mode == cli->mode_login)
  {
    switch (cli->exit_root_policy)
    {
    case CLI_EXIT_ROOT_POLICY_RESET_SESSION:
      cli_restart_session(cli);
      rc = CLI_OK;
      break;

    case CLI_EXIT_ROOT_POLICY_IGNORE:
      rc = CLI_OK;
      break;

    case CLI_EXIT_ROOT_POLICY_CLOSE_SESSION:
    default:
      cli->session_state = CLI_SESSION_STOP;
      rc = CLI_ERR_QUIT;
      break;
    }
  }
  else
  {
    /* Leaving enable mode also drops privilege (mirrors cmd_disable) */
    if (cli->mode == CLI_MODE_ENABLE)
    {
      cli_set_privilege(cli, CLI_PRIV_USER);
    }

    /* Drop one mode level */
    cli->mode = cli->mode_prev;
    if (cli->on_mode_change != NULL)
    {
      cli->on_mode_change(cli, cli->mode, cli->privilege);
    }
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_quit(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                           const char *argv[])
{
  int8_t rc = CLI_OK;

  (void)cmd;
  (void)argc;
  (void)argv;

  if (cli->on_quit_cmd != NULL)
  {
    int8_t hook_rc = cli->on_quit_cmd(cli);
    if (hook_rc != CLI_OK)
    {
      rc = hook_rc;
    }
  }

  if (rc != CLI_OK)
  {
    /* Hook result takes precedence. */
  }
  else
  {
    switch (cli->quit_policy)
    {
    case CLI_QUIT_POLICY_RESET_SESSION:
      cli_restart_session(cli);
      rc = CLI_OK;
      break;

    case CLI_QUIT_POLICY_IGNORE:
      rc = CLI_OK;
      break;

    case CLI_QUIT_POLICY_CLOSE_SESSION:
    default:
      cli->session_state = CLI_SESSION_STOP;
      rc = CLI_ERR_QUIT;
      break;
    }
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_enable(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                             const char *argv[])
{
  int8_t rc = CLI_OK;

  (void)cmd;
  (void)argc;
  (void)argv;

  /* If already privileged, just switch mode */
  if (cli->privilege >= CLI_PRIV_PRIVILEGED)
  {
    cli->mode_prev = cli->mode;
    cli_set_mode(cli, CLI_MODE_ENABLE);
  }
#if CLI_ENABLE_AUTH
  /* No secret/callback configured — elevate immediately */
  else if (cli->enable_privilege_secret[0] == '\0' && cli->enable_privilege_cb == NULL)
  {
    cli->mode_prev = cli->mode;
    cli_set_privilege(cli, CLI_PRIV_PRIVILEGED);
    cli_set_mode(cli, CLI_MODE_ENABLE);
  }
  /* Secret required — let CLI_SESSION_RUN collect the password */
  else
  {
    cli->mode_prev = cli->mode;
    cli->enable_privilege_prev = cli->privilege;
    cli_reset_input(cli);
    cli->auth_state = CLI_AUTH_ENABLE_MODE_PASSWORD;
    CLI_WSTR(cli, "Password: ");
    cli_flush(cli);
  }
#else
  else
  {
    cli->mode_prev = cli->mode;
    cli_set_privilege(cli, CLI_PRIV_PRIVILEGED);
    cli_set_mode(cli, CLI_MODE_ENABLE);
  }
#endif

  return rc;
}

int8_t cli_cmd_builtin_disable(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                               const char *argv[])
{
  (void)cmd;
  (void)argc;
  (void)argv;

  int8_t rc = CLI_OK;

  cli_set_privilege(cli, CLI_PRIV_USER);
  cli_set_mode(cli, CLI_MODE_EXEC);
  return rc;
}

int8_t cli_cmd_builtin_end(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                           const char *argv[])
{
  (void)cmd;
  (void)argc;
  (void)argv;

  int8_t rc = CLI_OK;

  /* Return to privileged exec from any config mode */
  cli_set_mode(cli, CLI_MODE_ENABLE);
  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_configure(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                                const char *argv[])
{
  (void)cmd;
  (void)argc;
  (void)argv;

  int8_t rc = CLI_OK;

  cli->mode_prev = cli->mode;
  cli_set_mode(cli, CLI_MODE_CONFIG);
  return rc;
}

#if CLI_ENABLE_HISTORY
OPENLIBCLI_API int8_t cli_cmd_builtin_history(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                              const char *argv[])
{
  (void)cmd;
#if !CLI_HISTORY_INDEX_SELECTION
  (void)argv;
  (void)argc;
#endif
  int8_t rc = CLI_OK;

  if (cli->history_count == 0)
  {
    CLI_PRINTLN_STR(cli, "History is empty.");
  }
  else
  {
#if CLI_HISTORY_INDEX_SELECTION
    if (argc > 1)
    {
      cli_len_t n_val = (cli_len_t)strtol(argv[1], NULL, 10);

      if (n_val == 0 || n_val > cli->history_count)
      {
        CLI_WSTR(cli, "% Invalid history index.\r\n");
        rc = CLI_ERR;
      }
      else
      {
        cli_history_idx_t idx =
            cli_history_index(cli, (cli_history_idx_t)(cli->history_count - n_val + 1));
        (void)cli_strlcpy(cli->input, cli->history_buf + cli->history_off[idx], CLI_MAX_INPUT_LEN);
        cli->input_len = (int)strlen(cli->input);
        cli->cursor = cli->input_len;
        rc = CLI_EXEC_REPLAY_INPUT;
      }
    }
    else
#endif
    {
      for (cli_history_idx_t i = cli->history_count; i > 0; i--)
      {
        cli_history_idx_t idx = cli_history_index(cli, i);
        char line[16 + CLI_MAX_INPUT_LEN];
        (void)cli_snprintf(line, sizeof(line), "  %3d  %s", cli->history_count - i + 1,
                           &cli->history_buf[cli->history_off[idx]]);
        (void)cli_println(cli, "%s", line);
      }
    }
  }

  return rc;
}
#endif /* CLI_ENABLE_HISTORY */

OPENLIBCLI_API int8_t cli_cmd_builtin_help(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                           const char *argv[])
{
  (void)cmd;

  int8_t rc = CLI_OK;

  cli_cmd_handle_t parent = cli_menu_context_parent(cli);

  if (argc > 1)
  {
    cli_cmd_handle_t h;
    for (cli_argc_t i = 1; i < argc; i++)
    {
      int8_t r = cli_resolve_one(cli, argv[i], parent, &h);
      if (r != CLI_OK)
      {
        if (r == CLI_ERR)
        {
          CLI_WSTR(cli, "% Unknown command.\r\n");
          rc = CLI_ERR;
        }
        else
        {
          CLI_WSTR(cli, "% Ambiguous command.\r\n");
          rc = CLI_ERR_AMBIG;
        }
        break;
      }
      parent = h;
    }
  }

  if (rc == CLI_OK)
  {
    bool any = False;
    cli_foreach_match(cli, "", parent, cli_print_help_cb, &any);
    if (!any)
    {
      CLI_WSTR(cli, "  <cr>\r\n");
    }
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_clear_screen(cli_struct_t *cli, const char *cmd,
                                                   cli_argc_t argc, const char *argv[])
{
  (void)cmd;
  (void)argc;
  (void)argv;

  int8_t rc = CLI_OK;

  /* ANSI clear screen + cursor home */
  if (cli->ansi_supported)
  {
    CLI_WSTR(cli, "\x1b"
                  "[2J\x1b"
                  "[H");
  }
  else
  {
    CLI_PRINTLN_STR(cli, "Clear not supported on Non-ANSI/VT100 Terminals");
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_usage(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                            const char *argv[])
{
  (void)cmd;
  (void)argc;
  (void)argv;

  CLI_PRINTLN_STR(cli, "Usage information. Type 'commands' or 'keys' for details.");
  return CLI_OK;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_usage_commands(cli_struct_t *cli, const char *cmd,
                                                     cli_argc_t argc, const char *argv[])
{
  (void)cmd;
  (void)argc;
  (void)argv;

  int8_t rc = CLI_OK;

  CLI_PRINTLN_STR(cli, "Common usage:");
  CLI_PRINTLN_STR(cli, "  ?                 Inline context help");
  CLI_PRINTLN_STR(cli, "  help              List top-level commands");
  CLI_PRINTLN_STR(cli, "  help show         List subcommands under help");
  CLI_PRINTLN_STR(cli, "  show ?            Show show subcommands");
  CLI_PRINTLN_STR(cli, "  history           Command history");
#if ENV_IS_OS_ENVIRONMENT
  CLI_PRINTLN_STR(cli, "  clear / cls       Clear screen");
#endif
  CLI_PRINTLN_STR(cli, "  usage keys        Key shortcuts");

  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_usage_keys(cli_struct_t *cli, const char *cmd,
                                                 cli_argc_t argc, const char *argv[])
{
  (void)cmd;
  (void)argc;
  (void)argv;

  int8_t rc = CLI_OK;

  CLI_PRINTLN_STR(cli, "Line editing shortcuts:");
  CLI_PRINTLN_STR(cli, "  Tab      autocomplete");
  CLI_PRINTLN_STR(cli, "  Ctrl-P/N history prev/next");
  CLI_PRINTLN_STR(cli, "  Ctrl-A/E line start/end");
  CLI_PRINTLN_STR(cli, "  Ctrl-B/F cursor left/right");
  CLI_PRINTLN_STR(cli, "  Ctrl-U   clear line");
  CLI_PRINTLN_STR(cli, "  Ctrl-W   delete prev word");
  CLI_PRINTLN_STR(cli, "  Ctrl-K   delete to EOL");
  return rc;
}

/* Session State Machine ----------------------------------------------------------------*/

OPENLIBCLI_API void cli_start(cli_struct_t *cli)
{
  if (cli != NULL)
  {
    cli->session_state = CLI_SESSION_INIT;

    cli_flush(cli);
  }
}

OPENLIBCLI_API int8_t cli_session_engine(cli_struct_t *cli)
{
#if CLI_ENABLE_TIMMING_STATS
  uint32_t t0 = cli->micros_source ? cli->micros_source(cli->micros_source_ctx) : 0U;
#endif
  int8_t result = CLI_OK;
  bool active = True;

  // cli_println(cli, "DEBUG::%d %d\n", active, cli->session_state);

  if (cli->session_state == CLI_SESSION_STOP)
  {
    result = CLI_ERR_QUIT;
    active = False;
  }
  else if (cli->session_state == CLI_SESSION_INIT)
  {
    cli->session_state = CLI_SESSION_SHOW_BANNER;
  }
  else if (cli->session_state == CLI_SESSION_SHOW_BANNER)
  {
#if CLI_ENABLE_BANNER
    if (cli->banner[0] != '\0')
    {
      CLI_WSTR(cli, CLI_CRLF);
      cli_write_str(cli, cli->banner);
      CLI_WSTR(cli, CLI_CRLF);
    }
#endif

#if CLI_ENABLE_AUTH
    if (cli->require_auth)
    {
      cli->auth_attempts = 0;
      cli->auth_lockout_started_ts = 0UL;
      cli_auth_prompt_username(cli);
      cli->session_state = CLI_SESSION_RUN;
    }
    else
    {
      cli->session_state = CLI_SESSION_RUN;
      cli_show_prompt(cli);
    }
#else
    cli->session_state = CLI_SESSION_RUN;
    cli_show_prompt(cli);
#endif
  }
  else if (cli->session_state == CLI_SESSION_RUN && active)
  {
#if CLI_ENABLE_PERIODIC_CALLBACK
    if (cli->periodic_cb != NULL && cli->periodic_interval > 0UL && cli->time_source != NULL)
    {
      uint32_t now = cli->time_source(cli->time_source_ctx);
      uint32_t elapsed = (now - cli->last_periodic_ts);
      if (elapsed >= cli->periodic_interval)
      {
        cli->last_periodic_ts = now;
        int8_t rr = cli->periodic_cb(cli);
        if (rr != CLI_OK)
        {
          cli->session_state = CLI_SESSION_STOP;
          result = rr;
          active = False;
        }
      }
    }
#endif

#if CLI_ENABLE_IDLE_TIMEOUT
    if (active)
    {
      int8_t timeout_rc = cli_check_idle_timeout(cli);
      if (timeout_rc != CLI_OK)
      {
        cli->session_state = CLI_SESSION_STOP;
        result = timeout_rc;
        active = False;
      }
    }
#endif

#if CLI_ENABLE_AUTH
    if (active && cli->auth_state == CLI_AUTH_LOCKOUT)
    {
      if (cli_auth_lockout_expired(cli))
      {
        cli->auth_attempts = 0;
        cli->auth_lockout_started_ts = 0UL;
        cli_auth_reset_work(cli);
        cli_auth_prompt_username(cli);
      }
      else
      {
        active = False;
      }
    }
#endif

    if (active)
    {
      cli_transport_ret_t b;
      cli_transport_ret_t available_rc = CLI_ERR;
      int8_t rc;
      bool keep_going = True;

      // cli_println(cli, "DEBUG: I am running...");
      while (keep_going)
      {
        available_rc = cli->transport.available(cli->transport.ctx);
        if (available_rc <= 0)
        {
          keep_going = False;
        }
        else
        {
          b = cli->transport.read(cli->transport.ctx);
          if (b < 0)
          {
            cli->session_state = CLI_SESSION_STOP;
            result = CLI_ERR;
            active = False;
            keep_going = False;
          }
          else
          {
            cli_touch_activity_internal(cli);

#if CLI_ENABLE_AUTH
            if (cli_state_is_auth_input(cli))
            {
              rc = cli_auth_handle(cli, b);
            }
            else
#endif
            {
              rc = cli_feed(cli, b);
            }
            if (rc != CLI_OK)
            {
              cli->session_state = CLI_SESSION_STOP;
              result = rc;
              active = False;
              keep_going = False;
            }
            else
            {
              cli_flush(cli);
            }
          }
        }
      }

      if (available_rc < 0)
      {
        cli->session_state = CLI_SESSION_STOP;
        result = CLI_ERR;
      }
    }
  }
  else
  {
    /* To silence MISRA */
  }

#if CLI_ENABLE_TIMMING_STATS
  if (cli->micros_source != NULL)
  {
    uint32_t elapsed = cli->micros_source(cli->micros_source_ctx) - t0;
    cli_tick_stats_struct_t *s = &cli->tick_stats;
    s->count++;
    if (s->count == 1U || elapsed < s->min_us)
    {
      s->min_us = elapsed;
    }
    if (elapsed > s->max_us)
    {
      s->max_us = elapsed;
    }
    int32_t avg_delta = ((int32_t)elapsed - (int32_t)s->avg_us) / (int32_t)s->count;
    int32_t new_avg = (int32_t)s->avg_us + avg_delta;
    s->avg_us = (uint32_t)new_avg;
  }
#endif /* CLI_ENABLE_TIMMING_STATS */

  return result;
}

/* Tick Stats (compiled in only when CLI_ENABLE_TIMMING_STATS is set) -------------------*/

#if CLI_ENABLE_TIMMING_STATS

OPENLIBCLI_API void cli_tick_stats_get(const cli_struct_t *cli, cli_tick_stats_struct_t *out)
{
  if (out != NULL)
  {
    if (cli != NULL)
    {
      *out = cli->tick_stats;
    }
    else
    {
      (void)memset(out, 0, sizeof(*out));
    }
  }
}

OPENLIBCLI_API void cli_tick_stats_reset(cli_struct_t *cli)
{
  if (cli != NULL)
  {
    (void)memset(&cli->tick_stats, 0, sizeof(cli->tick_stats));
  }
}

OPENLIBCLI_API void cli_set_micros_source(cli_struct_t *cli, cli_micros_fn fn, void *ctx)
{
  if (cli != NULL)
  {
    cli->micros_source = fn;
    cli->micros_source_ctx = ctx;
  }
}
#endif /* CLI_ENABLE_TIMMING_STATS */

/* Library / Session Lifecycle ----------------------------------------------------------*/

OPENLIBCLI_API void cli_lib_init(void)
{
  /* No-op — built-in commands registered via cli_add_builtin_cmds(). */
}

OPENLIBCLI_API void cli_init(cli_struct_t *cli, const char *hostname,
                             const cli_transport_struct_t *transport,
                             const cli_platform_struct_t *platform, cli_cmd_struct_t *cmd_pool,
                             cli_poolsize_t cmd_pool_size)
{
  (void)memset(cli, 0, sizeof(*cli));
  cli->context = CLI_CMD_ROOT;
  cli->cmd_pool = cmd_pool;
  cli->cmd_pool_size = cmd_pool_size;
  cli->cmd_pool_count = 0;
  cli->session_state = CLI_SESSION_STOP;
  cli->mode = CLI_MODE_EXEC;
  cli->mode_prev = CLI_MODE_EXEC;
  cli->mode_login = CLI_MODE_EXEC;
  cli->privilege = CLI_PRIV_USER;
  cli->ansi_supported = True;
  cli->transport = *transport;
#if CLI_ENABLE_TIME_SOURCE
  if (platform && platform->now_sec)
  {
    cli->time_source = platform->now_sec;
    cli->time_source_ctx = platform->now_sec_ctx;
#if ENV_IS_OS_ENVIRONMENT
  }
  else
  {
    cli->time_source = cli_default_time_source;
    cli->time_source_ctx = NULL;
#else
  }
  else
  {
    cli->time_source = NULL;
    cli->time_source_ctx = NULL;
#endif
  }
#else
  (void)platform;
#endif
#if CLI_ENABLE_TIMMING_STATS
  if (platform && platform->micros)
  {
    cli->micros_source = platform->micros;
    cli->micros_source_ctx = platform->micros_ctx;
  }
#endif
#if CLI_ENABLE_IDLE_TIMEOUT
  cli->idle_timeout = (cli_time_t)CLI_DEFAULT_IDLE_TIMEOUT_SEC;
  cli->idle_timeout_policy = CLI_IDLE_TIMEOUT_POLICY_RESET_SESSION;
#endif
  cli->exit_root_policy = CLI_EXIT_ROOT_POLICY_CLOSE_SESSION;
  cli->quit_policy = CLI_QUIT_POLICY_CLOSE_SESSION;
#if CLI_ENABLE_PERIODIC_CALLBACK
  cli->periodic_interval = 1UL;
#endif
#if CLI_ENABLE_AUTH
  cli->auth_attempts = 0;
  cli->auth_failure_mode = CLI_AUTH_FAILURE_MODE_CLOSE;
  cli->auth_lockout_seconds = 30UL;
  cli->auth_lockout_started_ts = 0UL;
  cli->auth_username[0] = '\0';
#endif

  (void)cli_strlcpy(cli->hostname, hostname ? hostname : "cli", CLI_MAX_PROMPT_LEN);

#if CLI_ENABLE_MODE_NAMES
  (void)cli_strlcpy(cli->mode_names[CLI_MODE_EXEC], "", CLI_MAX_MODE_NAME_LEN);
  (void)cli_strlcpy(cli->mode_names[CLI_MODE_ENABLE], "", CLI_MAX_MODE_NAME_LEN);
  (void)cli_strlcpy(cli->mode_names[CLI_MODE_CONFIG], "config", CLI_MAX_MODE_NAME_LEN);
#endif
  cli_touch_activity_internal(cli);
#if CLI_ENABLE_PERIODIC_CALLBACK && CLI_ENABLE_IDLE_TIMEOUT
  cli->last_periodic_ts = cli->last_action_ts;
#elif CLI_ENABLE_PERIODIC_CALLBACK && CLI_ENABLE_TIME_SOURCE
  if (cli->time_source != NULL)
  {
    cli->last_periodic_ts = cli->time_source(cli->time_source_ctx);
  }
#endif
}

OPENLIBCLI_API void cli_done(cli_struct_t *cli)
{
  if (cli != NULL)
  {
#if CLI_ENABLE_TIMMING_STATS
    if (cli->tick_stats.count > 0U)
    {
      (void)cli_println(cli, "\r\n--- Tick Timing Stats ---");
      (void)cli_println(cli, "  Ticks   : %u", (unsigned)cli->tick_stats.count);
      (void)cli_println(cli, "  Min (us): %u", (unsigned)cli->tick_stats.min_us);
      (void)cli_println(cli, "  Avg (us): %u", (unsigned)cli->tick_stats.avg_us);
      (void)cli_println(cli, "  Max (us): %u", (unsigned)cli->tick_stats.max_us);
    }
#endif
    cli->session_state = CLI_SESSION_STOP;
    (void)memset(cli, 0, sizeof(*cli));
  }
}

OPENLIBCLI_API cli_session_state_enum_t cli_get_session_state(const cli_struct_t *cli)
{
  return cli ? cli->session_state : CLI_SESSION_STOP;
}

OPENLIBCLI_API void cli_restart_session(cli_struct_t *cli)
{
  if (cli != NULL)
  {
    cli->mode = CLI_MODE_EXEC;
    cli->mode_login = CLI_MODE_EXEC;
    cli->mode_prev = CLI_MODE_EXEC;
    cli->privilege = CLI_PRIV_USER;
    cli->context = CLI_CMD_ROOT;
    cli->esc_state = ESC_STATE_NORMAL;
    cli->esc_param = 0;
    cli->saw_cr = False;
    cli_reset_input(cli);
#if CLI_ENABLE_AUTH
    cli->auth_state = CLI_AUTH_NONE;
    cli->auth_attempts = 0;
    cli->auth_lockout_started_ts = 0UL;
    cli_auth_reset_work(cli);
#endif
    cli->session_state = CLI_SESSION_SHOW_BANNER;
  }
}

OPENLIBCLI_API void cli_request_auth(cli_struct_t *cli)
{
  if (cli != NULL)
  {
#if CLI_ENABLE_AUTH
    cli->auth_attempts = 0;
    cli->auth_lockout_started_ts = 0UL;
    cli_auth_reset_work(cli);
    cli_auth_prompt_username(cli);
#else
    (void)cli;
#endif
  }
}

/* Configuration Setters ----------------------------------------------------------------*/

OPENLIBCLI_API void cli_set_ansi_supported(cli_struct_t *cli, bool enabled)
{
  if (cli != NULL)
  {
    cli->ansi_supported = enabled;
  }
}

OPENLIBCLI_API void cli_set_hostname(cli_struct_t *cli, const char *hostname)
{
  if (cli != NULL)
  {
    (void)cli_strlcpy(cli->hostname, hostname, CLI_MAX_PROMPT_LEN);
  }
}

OPENLIBCLI_API void cli_set_banner(cli_struct_t *cli, const char *banner)
{
#if CLI_ENABLE_BANNER
  if (cli != NULL)
  {
    (void)cli_strlcpy(cli->banner, banner, CLI_MAX_BANNER_LEN);
  }
#else
  (void)cli;
  (void)banner;
#endif
}

OPENLIBCLI_API void cli_set_userdata(cli_struct_t *cli, void *data)
{
  if (cli != NULL)
  {
    cli->user_data = data;
  }
}

OPENLIBCLI_API void *cli_get_userdata(const cli_struct_t *cli)
{
  return cli ? cli->user_data : NULL;
}

cli_transport_kind_enum_t cli_get_transport_kind(const cli_struct_t *cli)
{
  return cli ? cli->transport.kind : CLI_TRANSPORT_UNKNOWN;
}

OPENLIBCLI_API void cli_set_mode_change_cb(cli_struct_t *cli, cli_mode_fn fn)
{
  if (cli != NULL)
  {
    cli->on_mode_change = fn;
  }
}

#if CLI_ENABLE_MODE_NAMES
OPENLIBCLI_API void cli_set_mode_name(cli_struct_t *cli, cli_mode_t mode, const char *name)
{
  if (cli != NULL && mode < (int)CLI_MAX_MODES)
  {
    (void)cli_strlcpy(cli->mode_names[mode], name, CLI_MAX_MODE_NAME_LEN);
  }
}
#endif

OPENLIBCLI_API void cli_set_time_source(cli_struct_t *cli, cli_time_fn fn, void *ctx)
{
#if CLI_ENABLE_TIME_SOURCE
  if (cli != NULL)
  {
    cli->time_source = fn;
    cli->time_source_ctx = ctx;
    cli_touch_activity_internal(cli);
  }
#else
  (void)cli;
  (void)fn;
  (void)ctx;
#endif
}

OPENLIBCLI_API void cli_set_idle_timeout(cli_struct_t *cli, uint32_t seconds)
{
#if CLI_ENABLE_IDLE_TIMEOUT
  if (cli != NULL)
  {
    cli->idle_timeout = seconds;
    cli_touch_activity_internal(cli);
  }
#else
  (void)cli;
  (void)seconds;
#endif
}

OPENLIBCLI_API void cli_set_idle_timeout_cb(cli_struct_t *cli, uint32_t seconds,
                                            cli_idle_timeout_fn callback)
{
#if CLI_ENABLE_IDLE_TIMEOUT
  if (cli != NULL)
  {
    cli->idle_timeout_cb = callback;
    cli_set_idle_timeout(cli, seconds);
  }
#else
  (void)cli;
  (void)seconds;
  (void)callback;
#endif
}

OPENLIBCLI_API void cli_set_idle_timeout_mode(cli_struct_t *cli,
                                              cli_idle_timeout_mode_enum_t mode)
{
#if CLI_ENABLE_IDLE_TIMEOUT
  if (cli != NULL)
  {
    cli->idle_timeout_policy = mode;
  }
#else
  (void)cli;
  (void)mode;
#endif
}

OPENLIBCLI_API void cli_set_periodic_cb(cli_struct_t *cli, cli_periodic_fn callback)
{
#if CLI_ENABLE_PERIODIC_CALLBACK
  if (cli != NULL)
  {
    cli->periodic_cb = callback;
    if (cli->time_source != NULL)
    {
      cli->last_periodic_ts = cli->time_source(cli->time_source_ctx);
    }
  }
#else
  (void)cli;
  (void)callback;
#endif
}

OPENLIBCLI_API void cli_set_periodic_interval(cli_struct_t *cli, uint32_t seconds)
{
#if CLI_ENABLE_PERIODIC_CALLBACK
  if (cli != NULL)
  {
    cli->periodic_interval = seconds;
    if (cli->time_source != NULL)
    {
      cli->last_periodic_ts = cli->time_source(cli->time_source_ctx);
    }
  }
#else
  (void)cli;
  (void)seconds;
#endif
}

OPENLIBCLI_API void cli_touch_activity(cli_struct_t *cli)
{
  cli_touch_activity_internal(cli);
}

#if CLI_ENABLE_HISTORY

OPENLIBCLI_API int8_t cli_clear_history(cli_struct_t *cli)
{
  int8_t rc = CLI_ERR;

  if (cli != NULL)
  {
    cli->history_head = (cli_history_idx_t)0;
    cli->history_count = (cli_history_idx_t)0;
    cli->history_nav = (cli_history_idx_t)0;
    cli->history_buf[0] = '\0';
#if CLI_HISTORY_RESTORE_PREBROWSE_LINE
    cli->history_saved[0] = '\0';
#endif

    rc = CLI_OK;
  }

  return rc;
}
#endif /* CLI_ENABLE_HISTORY */

OPENLIBCLI_API int8_t cli_check_idle_timeout(cli_struct_t *cli)
{
  bool check_timeout = False;
  int8_t rc = CLI_OK;

#if CLI_ENABLE_IDLE_TIMEOUT
  if (cli != NULL && cli->idle_timeout != 0UL && cli->time_source != NULL)
  {
    check_timeout = True;

    if (cli->idle_timeout_policy == CLI_IDLE_TIMEOUT_POLICY_IGNORE)
    {
      check_timeout = False;
    }

#if CLI_ENABLE_AUTH
    /* In no-auth flow, RESET_SESSION behaves as IGNORE. */
    if (check_timeout && cli->idle_timeout_policy == CLI_IDLE_TIMEOUT_POLICY_RESET_SESSION &&
        !cli->require_auth)
    {
      check_timeout = False;
    }

    /* In auth flow, avoid repeated reset while waiting at login prompts. */
    if (check_timeout && cli->idle_timeout_policy == CLI_IDLE_TIMEOUT_POLICY_RESET_SESSION &&
        cli->auth_state != CLI_AUTH_AUTHENTICATED)
    {
      check_timeout = False;
    }
#endif

    if (check_timeout)
    {
      uint32_t now = cli->time_source(cli->time_source_ctx);
      uint32_t elapsed = (now - cli->last_action_ts);
      if (elapsed >= cli->idle_timeout)
      {
        rc = cli_handle_idle_timeout(cli);
        if (rc == CLI_OK)
        {
          cli_touch_activity_internal(cli);
        }
      }
    }
  }
#else
  (void)cli;
#endif

  return rc;
}

/* Authentication ----------------------------------------------------------------------*/

OPENLIBCLI_API int8_t cli_add_user(cli_struct_t *cli, const char *username, const char *password,
                                   cli_priv_t privilege)
{
  int8_t rc = CLI_OK;

#if CLI_ENABLE_AUTH
  if (cli == NULL || cli->num_users >= CLI_MAX_USERS)
  {
    rc = CLI_ERR_NOMEM;
  }
  else
  {
    (void)cli_strlcpy(cli->users[cli->num_users], username, CLI_MAX_USERNAME_LEN);
    (void)cli_strlcpy(cli->passwords[cli->num_users], password, CLI_MAX_PASSWORD_LEN);
    cli->user_privileges[cli->num_users] = privilege;
    cli->num_users++;
  }
#else
  (void)cli;
  (void)username;
  (void)password;
  (void)privilege;
  rc = CLI_ERR;
#endif

  return rc;
}

OPENLIBCLI_API void cli_set_authorization_cb(cli_struct_t *cli, cli_auth_fn fn)
{
#if CLI_ENABLE_AUTH
  if (cli != NULL)
  {
    cli->authorization_cb = fn;
  }
#else
  (void)cli;
  (void)fn;
#endif
}

OPENLIBCLI_API void cli_require_authorization(cli_struct_t *cli, bool require)
{
#if CLI_ENABLE_AUTH
  if (cli != NULL)
  {
    cli->require_auth = require;
  }
#else
  (void)cli;
  (void)require;
#endif
}

OPENLIBCLI_API void cli_set_auth_failure_mode(cli_struct_t *cli,
                                              cli_auth_failure_mode_enum_t mode)
{
#if CLI_ENABLE_AUTH
  if (cli != NULL)
  {
    cli->auth_failure_mode = mode;
  }
#else
  (void)cli;
  (void)mode;
#endif
}

/**
 * @brief Set the lockout duration after too many failed login attempts.
 *
 * @param[in] cli      Active CLI session.
 * @param[in] seconds  Lockout window in seconds; 0 disables lockout.
 */
OPENLIBCLI_API void cli_set_auth_lockout_duration(cli_struct_t *cli, uint32_t seconds)
{
#if CLI_ENABLE_AUTH
  if (cli != NULL)
  {
    cli->auth_lockout_seconds = seconds;
  }
#else
  (void)cli;
  (void)seconds;
#endif
}

/* Command Registration -----------------------------------------------------------------*/

OPENLIBCLI_API cli_cmd_handle_t cli_add_command(cli_struct_t *cli, cli_cmd_handle_t parent,
                                                const char *name, cli_cmd_handler_fn callback,
                                                cli_priv_t privilege, cli_mode_t mode,
                                                const char *help)
{
  cli_cmd_handle_t handle = CLI_CMD_INVALID;

  if (name != NULL && cli != NULL && cli->cmd_pool != NULL &&
      cli->cmd_pool_count < cli->cmd_pool_size)
  {
    for (cli_cmd_handle_t i = 0; i < cli->cmd_pool_size; i++)
    {
      if (!CLI_CMD_IS_IN_USE(&cli->cmd_pool[i]))
      {
        handle = i;
        break;
      }
    }
    if (handle != CLI_CMD_INVALID)
    {
      cli_cmd_struct_t *cmd = &cli->cmd_pool[handle];
      (void)memset(cmd, 0, sizeof(*cmd));
      (void)CLI_REGCPY(cmd->name, name, CLI_MAX_NAME_LEN);
      if (cmd->name[0] != '\0')
      {
#if CLI_ENABLE_COMMAND_HELP
        if (help != NULL)
        {
          (void)CLI_REGCPY(cmd->help, help, CLI_MAX_HELP_LEN);
        }
        else
        {
          cmd->help[0] = '\0';
        }
#else
        (void)help;
#endif
        cmd->callback = callback;
        cmd->privilege = privilege;
        cmd->mode = mode;
        cmd->parent = parent;
        CLI_CMD_SET_IN_USE(cmd);
        cli->cmd_pool_count++;
      }
    }
  }
  else if (cli != NULL && cli->cmd_pool != NULL)
  {
    (void)cli_println(cli, "ERROR: Command pool full (%d/%d)", cli->cmd_pool_count,
                      cli->cmd_pool_size);
  }
  else
  {
    /* To silence MISRA */
  }

  return handle;
}

#if CLI_ENABLE_ALIASES
OPENLIBCLI_API cli_cmd_handle_t cli_add_command_duplicate(cli_struct_t *cli,
                                                          cli_cmd_handle_t original,
                                                          const char *alias_name)
{
  cli_cmd_handle_t rc = CLI_CMD_INVALID;

  if (cli != NULL && cli->cmd_pool != NULL && original < cli->cmd_pool_size &&
      CLI_CMD_IS_IN_USE(&cli->cmd_pool[original]) && alias_name && alias_name[0] != '\0')
  {
    cli_cmd_struct_t *orig = &cli->cmd_pool[original];
    rc = cli_add_command(cli, orig->parent, alias_name, orig->callback, orig->privilege, orig->mode,
#if CLI_ENABLE_COMMAND_HELP
                         orig->help
#else
                         NULL
#endif
    );
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_add_alias(cli_struct_t *cli, cli_cmd_handle_t handle,
                                        const char *alias_name)
{
  int8_t rc = CLI_ERR;

  if (cli != NULL && cli->cmd_pool != NULL && handle != CLI_CMD_INVALID &&
      handle < cli->cmd_pool_size && CLI_CMD_IS_IN_USE(&cli->cmd_pool[handle]) && alias_name &&
      alias_name[0] != '\0')
  {
    cli_cmd_struct_t *cmd = &cli->cmd_pool[handle];
    if (cmd->num_aliases >= CLI_MAX_ALIASES)
    {
      rc = CLI_ERR_NOMEM;
    }
    else
    {
      (void)CLI_REGCPY(cmd->aliases[cmd->num_aliases], alias_name, CLI_MAX_NAME_LEN);
      cmd->num_aliases++;
      rc = CLI_OK;
    }
  }

  return rc;
}
#endif /* CLI_ENABLE_ALIASES */

OPENLIBCLI_API int8_t cli_remove_command(cli_struct_t *cli, cli_cmd_handle_t handle)
{
  int8_t rc = CLI_ERR;

  if (cli != NULL && cli->cmd_pool != NULL && handle != CLI_CMD_INVALID &&
      handle < cli->cmd_pool_size)
  {
    cli_cmd_struct_t *cmd = &cli->cmd_pool[handle];
    if (CLI_CMD_IS_IN_USE(cmd))
    {
      (void)memset(cmd, 0, sizeof(*cmd));
      if (cli->cmd_pool_count > 0)
      {
        cli->cmd_pool_count--;
      }
      rc = CLI_OK;
    }
  }

  return rc;
}

//-V::2565
OPENLIBCLI_API int8_t cli_remove_command_recursive(cli_struct_t *cli, cli_cmd_handle_t handle)
{
  int8_t rc = CLI_ERR;

  if (cli != NULL && cli->cmd_pool != NULL && handle != CLI_CMD_INVALID &&
      handle < cli->cmd_pool_size)
  {
    const cli_cmd_struct_t *cmd = &cli->cmd_pool[handle];
    if (CLI_CMD_IS_IN_USE(cmd))
    {
      bool removed_child = True;

      while (removed_child)
      {
        removed_child = False;
        for (cli_poolsize_t i = 0; i < cli->cmd_pool_size; i++)
        {
          if (CLI_CMD_IS_IN_USE(&cli->cmd_pool[i]) && cli->cmd_pool[i].parent == handle)
          {
            (void)cli_remove_command_recursive(cli, i);
            removed_child = True;
            break;
          }
        }
      }

      rc = cli_remove_command(cli, handle);
    }
  }

  return rc;
}

OPENLIBCLI_API void cli_hide_command(cli_struct_t *cli, cli_cmd_handle_t handle)
{
  if (cli != NULL && cli->cmd_pool != NULL && handle != CLI_CMD_INVALID &&
      handle < cli->cmd_pool_size)
  {
    CLI_CMD_SET_HIDDEN(&cli->cmd_pool[handle]);
  }
}

/* Mode / Privilege ---------------------------------------------------------------------*/

OPENLIBCLI_API void cli_set_mode(cli_struct_t *cli, cli_mode_t mode)
{
  if (cli != NULL)
  {
    cli->mode_prev = cli->mode;
    cli->mode = mode;
    if (cli->on_mode_change != NULL)
    {
      cli->on_mode_change(cli, cli->mode, cli->privilege);
    }
  }
}

OPENLIBCLI_API void cli_set_privilege(cli_struct_t *cli, cli_priv_t privilege)
{
  if (cli != NULL)
  {
    cli->privilege = privilege;
    if (cli->on_mode_change != NULL)
    {
      cli->on_mode_change(cli, cli->mode, cli->privilege);
    }
  }
}

OPENLIBCLI_API cli_mode_t cli_get_mode(const cli_struct_t *cli)
{
  return cli->mode;
}

OPENLIBCLI_API cli_priv_t cli_get_privilege(const cli_struct_t *cli)
{
  return cli->privilege;
}

OPENLIBCLI_API void cli_set_enable_secret(cli_struct_t *cli, const char *secret)
{
#if CLI_ENABLE_AUTH
  if (cli != NULL)
  {
    (void)cli_strlcpy(cli->enable_privilege_secret, secret ? secret : "",
                      CLI_MAX_ENABLE_SECRET_LEN);
  }
#else
  (void)cli;
  (void)secret;
#endif
}

OPENLIBCLI_API void cli_set_enable_privilege_cb(cli_struct_t *cli, cli_enable_fn fn)
{
#if CLI_ENABLE_AUTH
  if (cli != NULL)
  {
    cli->enable_privilege_cb = fn;
  }
#else
  (void)cli;
  (void)fn;
#endif
}

OPENLIBCLI_API void cli_set_exit_cb(cli_struct_t *cli, cli_session_cmd_fn fn)
{
  if (cli != NULL)
  {
    cli->on_exit_cmd = fn;
  }
}

OPENLIBCLI_API void cli_set_quit_cb(cli_struct_t *cli, cli_session_cmd_fn fn)
{
  if (cli != NULL)
  {
    cli->on_quit_cmd = fn;
  }
}

OPENLIBCLI_API void cli_set_exit_root_policy(cli_struct_t *cli,
                                             cli_exit_root_policy_enum_t policy)
{
  if (cli != NULL)
  {
    cli->exit_root_policy = policy;
  }
}

OPENLIBCLI_API void cli_set_quit_policy(cli_struct_t *cli, cli_quit_policy_enum_t policy)
{
  if (cli != NULL)
  {
    cli->quit_policy = policy;
  }
}

OPENLIBCLI_API int8_t cli_exec(cli_struct_t *cli, const char *cmd)
{
  int8_t rc = CLI_ERR;

  if (cli != NULL && cmd != NULL)
  {
    (void)cli_strlcpy(cli->input, cmd, CLI_MAX_INPUT_LEN);
    cli->input_len = (int)strlen(cli->input);
    cli->cursor = cli->input_len;
    rc = cli_execute(cli);
    if (rc == CLI_EXEC_REPLAY_INPUT)
    {
      cli_redraw_line(cli);
    }
    else
    {
      cli_reset_input(cli);
    }
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_exec_argv(cli_struct_t *cli, const char *const *argv, cli_argc_t argc)
{
  char buf[CLI_MAX_INPUT_LEN];
  size_t pos = 0;
  int8_t rc = CLI_ERR;

  if (cli != NULL && argv != NULL && argc > 0)
  {
    buf[0] = '\0';
    for (cli_argc_t i = 0; i < argc; i++)
    {
      if (pos >= sizeof(buf) - 1U)
      {
        break;
      }
      if (i > 0)
      {
        pos += cli_strlcpy(&buf[pos], " ", sizeof(buf) - pos);
      }
      pos += cli_strlcpy(&buf[pos], argv[i], sizeof(buf) - pos);
    }
    rc = cli_exec(cli, buf);
  }

  return rc;
}

/* Output Helpers -----------------------------------------------------------------------*/

OPENLIBCLI_API int cli_print_p(cli_struct_t *cli, const char *fmt, ...)
{
  char buf[CLI_MAX_OUTPUT_BUF];
  //-V::2604
  va_list ap;
  int rc = 0;
  int n;

  if (cli->print_cb_v != NULL)
  {
    //-V::2604
    (void)va_start(ap, fmt);
    cli->print_cb_v(cli, fmt, ap);
    //-V::2604
    (void)va_end(ap);
  }
  else
  {
    //-V::2604
    (void)va_start(ap, fmt);
#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
    //-V:vsnprintf_P:2600
    n = vsnprintf_P(buf, sizeof(buf), fmt, ap);
#else
    n = cli_vsnprintf(buf, sizeof(buf), fmt, ap);
#endif
    //-V::2604
    (void)va_end(ap);

    if (cli->print_cb != NULL)
    {
      cli->print_cb(cli, buf, CLI_PRINT_KIND_PRINT);
      rc = n;
    }
    else if (n > 0)
    {
      int written = (n > (int)sizeof(buf) - 1) ? (int)sizeof(buf) - 1 : n;
      cli_print_process_stream(cli, buf, written);
      rc = n;
    }
    else
    {
      /* To silence MISRA */
    }
  }

  return rc;
}

OPENLIBCLI_API int cli_println_p(cli_struct_t *cli, const char *fmt, ...)
{
  //-V::2604
  va_list ap;
  int rc = 0;
  int n;

  if (cli->print_cb_v != NULL)
  {
    //-V::2604
    (void)va_start(ap, fmt);
    cli->print_cb_v(cli, fmt, ap);
    //-V::2604
    (void)va_end(ap);
  }
  else
  {
    char buf[CLI_MAX_OUTPUT_BUF];

    //-V::2604
    (void)va_start(ap, fmt);
#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
    //-V:vsnprintf_P:2600
    n = vsnprintf_P(buf, sizeof(buf), fmt, ap);
#else
    n = cli_vsnprintf(buf, sizeof(buf), fmt, ap);
#endif
    //-V::2604
    (void)va_end(ap);

    if (cli->print_cb != NULL)
    {
      cli->print_cb(cli, buf, CLI_PRINT_KIND_PRINTLN);
      rc = (n + 1);
    }
    else if (n >= 0)
    {
      int written = (n > (int)sizeof(buf) - 1) ? (int)sizeof(buf) - 1 : n;
      cli_print_process_stream(cli, buf, written);
      cli_print_process_stream(cli, "\n", 1);
      rc = (n + 1);
    }
    else
    {
      /* To silence MISRA */
    }
  }

  return rc;
}

OPENLIBCLI_API int cli_error_p(cli_struct_t *cli, const char *fmt, ...)
{
  //-V::2604
  va_list ap;
  int rc = 0;

  if (cli->print_cb_v != NULL)
  {
    //-V::2604
    (void)va_start(ap, fmt);
    cli->print_cb_v(cli, fmt, ap);
    //-V::2604
    (void)va_end(ap);
  }
  else
  {
    char buf[CLI_MAX_OUTPUT_BUF];

    //-V::2604
    (void)va_start(ap, fmt);
#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
    //-V:vsnprintf_P:2600
    vsnprintf_P(buf, sizeof(buf), fmt, ap);
#else
    (void)cli_vsnprintf(buf, sizeof(buf), fmt, ap);
#endif
    //-V::2604
    (void)va_end(ap);

    if (cli->print_cb != NULL)
    {
      cli->print_cb(cli, buf, CLI_PRINT_KIND_ERROR);
      rc = (int)strlen(buf);
    }
    else
    {
      /* Errors bypass filters and always appear. */
      if (cli->print_transport_cb != NULL)
      {
        cli->print_transport_cb(cli, buf);
      }
      else
      {
        cli_write_str(cli, "\r\n% ");
        cli_write_str(cli, buf);
        cli_write_str(cli, CLI_CRLF);
      }
      rc = (int)strlen(buf);
    }
  }

  return rc;
}

OPENLIBCLI_API void cli_print_flush(cli_struct_t *cli)
{
#if CLI_ENABLE_PIPE_FILTER
  if (cli->filter_line_buf_len > 0)
  {
    cli->filter_line_buf[cli->filter_line_buf_len] = '\0';
    cli_print_emit(cli, cli->filter_line_buf);
    cli->filter_line_buf_len = 0;
  }
#else
  (void)cli;
#endif
}

OPENLIBCLI_API void cli_set_print_transport_cb(cli_struct_t *cli, cli_print_transport_fn cb)
{
  if (cli != NULL)
  {
    cli->print_transport_cb = cb;
  }
}

OPENLIBCLI_API void cli_set_print_cb(cli_struct_t *cli, cli_print_fn cb)
{
  if (cli != NULL)
  {
    cli->print_cb = cb;
  }
}

OPENLIBCLI_API void cli_set_print_cb_v(cli_struct_t *cli, cli_print_v_fn cb)
{
  if (cli != NULL)
  {
    cli->print_cb_v = cb;
  }
}

OPENLIBCLI_API int cli_snprintf(char *buf, size_t size, const char *fmt, ...)
{
  //-V::2604
  va_list ap;
  int n;

  //-V::2604
  (void)va_start(ap, fmt);
  n = cli_vsnprintf(buf, size, fmt, ap);
  //-V::2604
  (void)va_end(ap);

  return n;
}

//-V::2604
OPENLIBCLI_API int cli_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
  //-V::2600
  return vsnprintf(buf, size, fmt, ap);
}

OPENLIBCLI_API int cli_sscanf(const char *str, const char *fmt, void *out)
{
  int result = 0;

  if (str != NULL && fmt != NULL && out != NULL)
  {
    //-V::2600
    result = sscanf(str, fmt, out);
  }

  return result;
}

/*=======================================================================================
 * Private Functions
 *=======================================================================================*/

/* Internal String Helpers ---------------------------------------------------------------*/

static size_t cli_strlcpy(char *dst, const char *src, size_t size)
{
  size_t i = 0;
  if (size > 1U)
  {
    while (i < size - 1U && src[i] != '\0')
    {
      dst[i] = src[i];
      i++;
    }
  }
  if (size > 0U)
  {
    dst[i] = '\0';
  }
  return i;
}

static bool cli_case_prefix_match(const char *str, const char *prefix)
{
  bool match = True;

  while (*prefix != '\0' && match)
  {
    if (tolower((int)(unsigned char)*str) != tolower((int)(unsigned char)*prefix))
    {
      match = False;
    }
    else
    {
      str++;
      prefix++;
    }
  }

  return match;
}

/**
 * @brief Skip leading whitespace characters (space, tab).
 *
 * Advances @p p past any consecutive @c ' ' and @c '\\t' characters.
 *
 * @param[in] p  Pointer to the start of a string.
 *
 * @return Pointer to the first non-whitespace character (or the NUL
 *         terminator if the string is all whitespace).
 */
static inline char *cli_skip_spaces(char *p)
{
  while (*p == ' ' || *p == '\t')
  {
    p++;
  }
  return p;
}

/**
 * @brief Skip non-whitespace characters (the inverse of @c cli_skip_spaces).
 *
 * Advances @p p past any characters that are neither @c ' ' nor @c '\\t',
 * stopping at the first whitespace or NUL terminator.
 *
 * @param[in] p  Pointer to the start of a token.
 *
 * @return Pointer to the first whitespace or NUL after the token.
 */
static inline char *cli_skip_non_spaces(char *p)
{
  while (*p != '\0' && *p != ' ' && *p != '\t')
  {
    p++;
  }
  return p;
}

/* Raw I/O Primitives -------------------------------------------------------------------*/

/**
 * @brief Write a raw byte buffer to the transport.
 * @param[in] cli  Active CLI session.
 * @param[in] buf  Buffer to transmit.
 * @param[in] len  Number of bytes to transmit (no-op if <= 0).
 */
static void cli_raw_write(cli_struct_t *cli, const uint8_t *buf, cli_transport_buflen_t len)
{
  if (cli != NULL && len > 0)
  {
    (void)cli->transport.write(cli->transport.ctx, buf, len);
  }
}

/**
 * @brief Write a NUL-terminated string to the transport.
 * @param[in] cli  Active CLI session.
 * @param[in] s    String to transmit; no-op if NULL.
 */
static void cli_write_str(cli_struct_t *cli, const char *s)
{
  if (s != NULL)
  {
    cli_raw_write(cli, (const uint8_t *)s, (cli_transport_buflen_t)strlen(s));
  }
}

/**
 * @brief Write a single character to the transport.
 * @param[in] cli  Active CLI session.
 * @param[in] c    Character to transmit.
 */
static void cli_write_char(cli_struct_t *cli, char c)
{
  cli_raw_write(cli, (const uint8_t *)&c, 1);
}

/**
 * @brief Flush the transport's send buffer if a flush function is registered.
 * @param[in] cli  Active CLI session.
 */
static void cli_flush(cli_struct_t *cli)
{
  if (cli->transport.flush != NULL)
  {
    (void)cli->transport.flush(cli->transport.ctx);
  }
}

/* Session Timing Helpers ---------------------------------------------------------------*/

/**
 * @brief Record the current time as the last user-activity timestamp.
 * @param[in] cli  Active CLI session.
 */
static void cli_touch_activity_internal(cli_struct_t *cli)
{
#if CLI_ENABLE_IDLE_TIMEOUT && CLI_ENABLE_TIME_SOURCE
  if (cli != NULL && cli->time_source != NULL)
  {
    cli->last_action_ts = cli->time_source(cli->time_source_ctx);
  }
#else
  (void)cli;
#endif
}

#if CLI_ENABLE_TIME_SOURCE && ENV_IS_OS_ENVIRONMENT
/**
 * @brief Default monotonic time source installed by @c cli_init().
 *
 * Uses @c GetTickCount64 on Windows and @c clock_gettime(CLOCK_MONOTONIC) on
 * POSIX. Bare-metal targets do not have a library default; supply
 * @c cli_platform_struct_t.now_sec to @c cli_init().
 *
 * @param[in] ctx  Unused; required by the @c cli_time_fn signature.
 *
 * @return Monotonic time in seconds.
 */
static uint32_t cli_default_time_source(void *ctx)
{
  (void)ctx;
  uint32_t now_sec = 0;

#if ENV_IS_WINDOWS
  now_sec = (uint32_t)(GetTickCount64() / 1000U);
#elif ENV_IS_OS_ENVIRONMENT
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
  {
    now_sec = (uint32_t)ts.tv_sec;
  }
  else
  {
    now_sec = (uint32_t)time(NULL);
  }
#else
  now_sec = (uint32_t)time(NULL);
#endif
#endif

  return now_sec;
}
#endif

#if CLI_ENABLE_IDLE_TIMEOUT
/**
 * @brief Execute the idle-timeout action for a session.
 *
 * Invokes the registered @c idle_timeout_cb if set; otherwise applies the
 * configured @c idle_timeout_policy (reset or close).
 *
 * @param[in] cli  Active CLI session.
 *
 * @return @c CLI_OK if the session was reset and should continue, or
 *         @c CLI_ERR_QUIT if the session should terminate.
 */
static int8_t cli_handle_idle_timeout(cli_struct_t *cli)
{
  int8_t rc = CLI_OK;

  if (cli->idle_timeout_cb != NULL)
  { //>! user callback
    rc = cli->idle_timeout_cb(cli);
  }
  else
  {
    if (cli->idle_timeout_policy == CLI_IDLE_TIMEOUT_POLICY_RESET_SESSION)
    {
      CLI_WSTR(cli, "\r\n% Idle timeout. Session restarted.\r\n");
      cli_restart_session(cli);
      /* Prevent immediate re-trigger: update last activity timestamps */
      if (cli->time_source != NULL)
      {
        cli->last_action_ts = cli->time_source(cli->time_source_ctx);
#if CLI_ENABLE_PERIODIC_CALLBACK
        cli->last_periodic_ts = cli->last_action_ts;
#endif
      }
    }
    else if (cli->idle_timeout_policy == CLI_IDLE_TIMEOUT_POLICY_CLOSE)
    {
      CLI_WSTR(cli, "\r\n% Idle timeout.\r\n");
      cli->session_state = CLI_SESSION_STOP;
      rc = CLI_ERR_QUIT;
    }
    else
    {
      /* CLI_IDLE_TIMEOUT_POLICY_IGNORE */
    }
  }

  return rc;
}
#endif

/* Output Line Processing ---------------------------------------------------------------*/

/**
 * @brief Emit one complete output line through the active print path.
 *
 * The line is first checked against the active pipe filter when filtering is
 * enabled.  If the line passes, it is delivered either to @c print_transport_cb as a
 * NUL-terminated string without a trailing newline, or to the default
 * transport followed by @c \r\n.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] line  NUL-terminated logical output line.
 */
#if CLI_ENABLE_PIPE_FILTER
static void cli_print_emit(cli_struct_t *cli, const char *line)
{
  bool emit = True;

#if CLI_ENABLE_PIPE_FILTER
  if (!cli_filter_line(cli, line))
  {
    emit = False;
  }
#endif

  if (emit)
  {
    if (cli->print_transport_cb != NULL)
    {
      cli->print_transport_cb(cli, line);
    }
    else
    {
      cli_write_str(cli, line);
      cli_write_str(cli, CLI_CRLF);
    }
  }
}
#endif

/**
 * @brief Feed raw output characters into the per-line print buffer.
 *
 * Characters are accumulated in @c filter_line_buf until a @c \n is encountered, at
 * which point the buffered text is emitted as one logical line.  Carriage
 * returns are ignored.  If the buffered line reaches @c CLI_MAX_OUTPUT_BUF - 1
 * characters, additional non-newline characters are dropped until the line is
 * emitted or flushed.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] data  Character stream to process.
 * @param[in] len   Number of bytes available in @p data.
 */
static void cli_print_process_stream(cli_struct_t *cli, const char *data, int len)
{
#if CLI_ENABLE_PIPE_FILTER
  for (int i = 0; i < len; i++)
  {
    char c = data[i];

    if (c == '\n')
    {
      cli->filter_line_buf[cli->filter_line_buf_len] = '\0';
      cli_print_emit(cli, cli->filter_line_buf);
      cli->filter_line_buf_len = 0;
    }
    else if (c != '\r')
    {
      if (cli->filter_line_buf_len < (int)CLI_MAX_OUTPUT_BUF - 1)
      {
        cli->filter_line_buf[cli->filter_line_buf_len++] = c;
      }
    }
    else
    {
      /* To silence MISRA */
    }
  }
#else
  for (int i = 0; i < len; i++)
  {
    char c = data[i];

    if (c == '\n')
    {
      cli_write_str(cli, CLI_CRLF);
    }
    else if (c != '\r')
    {
      cli_write_char(cli, c);
    }
    else
    {
      /* To silence MISRA */
    }
  }
#endif
}

static void cli_history_capture_edited_preview(cli_struct_t *cli)
{
#if CLI_ENABLE_HISTORY && CLI_HISTORY_RESTORE_PREBROWSE_LINE
  if (cli != NULL && cli->history_nav > 0)
  {
    (void)cli_strlcpy(cli->history_saved, cli->input, CLI_MAX_INPUT_LEN);
  }
#else
  (void)cli;
#endif
}

/* Line Editing -------------------------------------------------------------------------*/

/**
 * @brief Redraw the entire input line from column 0.
 *
 * Moves to the beginning of the line, reprints the prompt and input buffer,
 * then repositions the cursor at @c cli->cursor.
 *
 * @param[in] cli  Active CLI session.
 */
static void cli_redraw_line(cli_struct_t *cli)
{
  /* Move to start of line, clear to EOL, reprint input */
  CLI_WSTR(cli, "\r");
  cli_show_prompt(cli);

  cli_raw_write(cli, (const uint8_t *)cli->input, cli->input_len);

  if (cli->ansi_supported)
  {
    CLI_WSTR(cli, "\x1b"
                  "[K");

    /* If cursor is not at end, move it back */
    cli_len_t trail = cli->input_len - cli->cursor;
    for (cli_len_t i = 0; i < trail; i++)
    {
      cli_write_char(cli, ASCII_BS);
    }
  }
}

/**
 * @brief Move the cursor by a signed relative offset.
 *
 * Applies @p delta to the current cursor position and forwards the result to
 * @c cli_move_cursor, which clamps the final position to valid input bounds.
 *
 * @param[in] cli    Active CLI session.
 * @param[in] delta  Relative movement (+right, -left).
 */
static void cli_move_cursor_delta(cli_struct_t *cli, int16_t delta)
{
  if (delta >= 0)
  {
    cli_move_cursor(cli, cli->cursor + delta);
  }
  else
  {
    cli_len_t d = -delta;
    cli_move_cursor(cli, (d > cli->cursor) ? 0 : cli->cursor - d);
  }
}

/**
 * @brief Move the cursor to the end of the current input line.
 *
 * Convenience wrapper around @c cli_move_cursor using @c input_len as the
 * target position.
 *
 * @param[in] cli  Active CLI session.
 */
static void cli_move_cursor_end(cli_struct_t *cli)
{
  cli_move_cursor(cli, cli->input_len);
}

/**
 * @brief Move the logical and display cursor to an absolute position.
 * @param[in] cli  Active CLI session.
 * @param[in] pos  Target position (clamped to [0, input_len]).
 */
static void cli_move_cursor(cli_struct_t *cli, cli_len_t pos)
{
  if (pos > cli->input_len)
  {
    pos = cli->input_len;
  }

  while (cli->cursor < pos)
  {
    cli_write_char(cli, cli->input[cli->cursor]);
    cli->cursor++;
  }
  while (cli->cursor > pos)
  {
    cli_write_char(cli, ASCII_BS);
    cli->cursor--;
  }
}

/**
 * @brief Insert a character at the current cursor position, shifting right.
 * @param[in] cli  Active CLI session.
 * @param[in] c    Character to insert.
 */
static void cli_insert_char(cli_struct_t *cli, char c)
{
  if (cli->input_len >= (int)CLI_MAX_INPUT_LEN - 1)
  {
    cli_write_char(cli, '\a');
  }
  else
  {
    /* Shift right */
    (void)memmove(&cli->input[cli->cursor + 1], &cli->input[cli->cursor],
                  (size_t)(cli->input_len - cli->cursor));
    cli->input[cli->cursor] = c;
    cli->input_len++;
    cli->input[cli->input_len] = '\0';

    /* Rewrite from cursor to end, then reposition */
    cli_raw_write(cli, (const uint8_t *)&cli->input[cli->cursor], cli->input_len - cli->cursor);
    cli_len_t trail = cli->input_len - cli->cursor - 1;
    for (cli_len_t i = 0; i < trail; i++)
    {
      cli_write_char(cli, ASCII_BS);
    }
    cli->cursor++;
    cli_history_capture_edited_preview(cli);
  }
}

/**
 * @brief Delete the character under the cursor (forward delete).
 * @param[in] cli  Active CLI session.
 */
static void cli_delete_char(cli_struct_t *cli)
{
  if (cli->cursor < cli->input_len)
  {
    (void)memmove(&cli->input[cli->cursor], &cli->input[cli->cursor + 1],
                  (size_t)(cli->input_len - cli->cursor - 1));
    cli->input_len--;
    cli->input[cli->input_len] = '\0';

    /* Rewrite tail, append space to erase last char, reposition */
    cli_raw_write(cli, (const uint8_t *)&cli->input[cli->cursor], cli->input_len - cli->cursor);
    cli_write_char(cli, ' ');
    cli_len_t trail = cli->input_len - cli->cursor + 1;
    for (cli_len_t i = 0; i < trail; i++)
    {
      cli_write_char(cli, ASCII_BS);
    }
    cli_history_capture_edited_preview(cli);
  }
}

/**
 * @brief Delete the character before the cursor (backspace).
 * @param[in] cli  Active CLI session.
 */
static void cli_backspace(cli_struct_t *cli)
{
  if (cli->cursor > 0)
  {
    /* Keep terminal cursor in sync with logical cursor before delete. */
    cli_write_char(cli, ASCII_BS);
    cli->cursor--;
    cli_delete_char(cli);
  }
}

/**
 * @brief Erase from the cursor to the end of the input line (Ctrl+K).
 * @param[in] cli  Active CLI session.
 */
static void cli_kill_line(cli_struct_t *cli)
{
  /* Erase from cursor to end */
  cli_len_t erase = cli->input_len - cli->cursor;
  for (cli_len_t i = 0; i < erase; i++)
  {
    cli_write_char(cli, ' ');
  }

  for (cli_len_t i = 0; i < erase; i++)
  {
    cli_write_char(cli, ASCII_BS);
  }

  cli->input_len = cli->cursor;
  cli->input[cli->input_len] = '\0';
  cli_history_capture_edited_preview(cli);
}

/**
 * @brief Delete the word immediately before the cursor (Ctrl+W).
 * @param[in] cli  Active CLI session.
 */
static void cli_kill_word(cli_struct_t *cli)
{
  /* Delete word (run of non-spaces) before cursor, skip leading spaces */
  while (cli->cursor > 0 && cli->input[cli->cursor - 1] == ' ')
  {
    cli_backspace(cli);
  }

  while (cli->cursor > 0 && cli->input[cli->cursor - 1] != ' ')
  {
    cli_backspace(cli);
  }
}

/**
 * @brief Clear the entire input buffer and erase the displayed line (Ctrl+U).
 * @param[in] cli  Active CLI session.
 */
static void cli_clear_input(cli_struct_t *cli)
{
  cli_move_cursor(cli, cli->input_len);
  while (cli->input_len > 0)
  {
    cli_backspace(cli);
  }
}

/* AVR PROGMEM Helpers ------------------------------------------------------------------*/

#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
/**
 * @brief Write a PROGMEM string to the active transport.
 *
 * Reads bytes from AVR program memory and emits them one-by-one through
 * @c cli_write_char().
 *
 * @param[in] cli  Active CLI session.
 * @param[in] s_P  NUL-terminated string stored in PROGMEM.
 */
static void cli_write_str_P(cli_struct_t *cli, const char *s_P)
{
  char c;
  while ((c = (char)pgm_read_byte(s_P++)) != '\0')
  {
    cli_write_char(cli, c);
  }
}

/**
 * @brief Copy a PROGMEM string into a RAM buffer.
 *
 * Behaves like a bounded @c strlcpy for AVR program-memory sources.
 *
 * @param[out] dst    Destination RAM buffer.
 * @param[in]  src_P  Source NUL-terminated string in PROGMEM.
 * @param[in]  size   Size of @p dst in bytes.
 *
 * @return Number of bytes copied to @p dst, excluding the terminator.
 */
static size_t cli_strlcpy_P(char *dst, const char *src_P, size_t size)
{
  size_t i = 0;
  if (size > 1)
  {
    char c;
    while (i < size - 1 && (c = (char)pgm_read_byte(src_P + i)) != '\0')
    {
      dst[i++] = c;
    }
  }
  if (size > 0)
  {
    dst[i] = '\0';
  }
  return i;
}
#endif

/* Prompt Construction and Display ------------------------------------------------------*/

/**
 * @brief Print the current mode/privilege prompt to the transport.
 *
 * Builds a prompt of the form @c "hostname(mode)# " or @c "hostname> ",
 * respecting the active context stack and @c show_prompt flag.
 *
 * @param[in] cli  Active CLI session.
 */
static void cli_show_prompt(cli_struct_t *cli)
{
  if (cli != NULL)
  {
#if CLI_ENABLE_MODE_NAMES
    char prompt[CLI_MAX_PROMPT_LEN + CLI_MAX_MODE_NAME_LEN + 8];
#else
    char prompt[CLI_MAX_PROMPT_LEN + 8];
#endif
    size_t pos = 0;
    bool emitted = False;

    if (cli->context != CLI_CMD_ROOT)
    {
      cli_cmd_handle_t cur = cli->context;
      if (cur < cli->cmd_pool_size && CLI_CMD_IS_IN_USE(&cli->cmd_pool[cur]))
      {
        pos = cli_strlcpy(&prompt[pos], cli->cmd_pool[cur].name, sizeof(prompt));
        prompt[pos++] = '>';
        prompt[pos++] = ' ';
        prompt[pos] = '\0';
        cli_write_str(cli, prompt);
        cli_flush(cli);
        emitted = True;
      }
    }

    if (!emitted)
    {
      pos = pos + cli_strlcpy(&prompt[pos], cli->hostname, sizeof(prompt) - pos);

#if CLI_ENABLE_MODE_NAMES
      if (cli->mode < (int)CLI_MAX_MODES && cli->mode_names[cli->mode][0] != '\0')
      {
        prompt[pos++] = '(';
        pos = pos + cli_strlcpy(&prompt[pos], cli->mode_names[cli->mode], sizeof(prompt) - pos);
        prompt[pos++] = ')';
      }
#endif

      if (cli->privilege >= CLI_PRIV_PRIVILEGED)
      {
        prompt[pos++] = '#';
      }
      else
      {
        prompt[pos++] = '>';
      }

      prompt[pos++] = ' ';
      prompt[pos] = '\0';

      cli_write_str(cli, prompt);
      cli_flush(cli);
    }
  }
}

/* History ------------------------------------------------------------------------------*/

#if CLI_ENABLE_HISTORY
/**
 * @brief Append a line to the session command history ring.
 *
 * Duplicates of the most-recent entry are silently dropped.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] line  NUL-terminated command string to store.
 */
static void cli_history_add(cli_struct_t *cli, const char *line)
{
  bool add = (cli != NULL && line != NULL && line[0] != '\0');

  if (cli != NULL && line != NULL && add && cli->history_count > 0)
  {
    cli_history_idx_t prev =
        (cli_history_idx_t)((cli->history_head + CLI_MAX_HISTORY - 1) % CLI_MAX_HISTORY);
    if (strcmp(line, &cli->history_buf[cli->history_off[prev]]) == 0)
    {
      add = False;
    }
  }

  if (cli != NULL && line != NULL && add)
  {
    cli_len_t len = (int)strlen(line);
    cli_len_t needed = len + 1;
    cli_history_idx_t off;

    if (cli->history_count == 0)
    {
      off = (cli_history_idx_t)0;
    }
    else
    {
      cli_history_idx_t prev =
          (cli_history_idx_t)((cli->history_head + CLI_MAX_HISTORY - 1) % CLI_MAX_HISTORY);
      off = (cli_history_idx_t)(cli->history_off[prev] +
                                (int)strlen(&cli->history_buf[cli->history_off[prev]]) + 1);
    }

    if (off + needed > CLI_HISTORY_BUF_SIZE)
    {
      off = (cli_history_idx_t)0;
      cli->history_count = (cli_history_idx_t)0;
      cli->history_head = (cli_history_idx_t)0;
    }

    (void)memcpy(&cli->history_buf[off], line, (size_t)needed);
    cli->history_off[cli->history_head] = (cli_history_idx_t)off;

    cli->history_head = (cli_history_idx_t)((cli->history_head + 1) % CLI_MAX_HISTORY);
    //-V::1051
    if (cli->history_count < CLI_MAX_HISTORY)
    {
      cli->history_count++;
    }
    cli->history_nav = (cli_history_idx_t)0;
#if CLI_HISTORY_RESTORE_PREBROWSE_LINE
    cli->history_saved[0] = '\0';
#endif
  }
}

/**
 * @brief Compute the ring-buffer index for history entry at @p offset.
 * @param[in] cli     Active CLI session.
 * @param[in] offset  1 = most recent, 2 = second most recent, etc.
 * @return Ring-buffer index into @c cli->history[].
 */
static cli_history_idx_t cli_history_index(const cli_struct_t *cli, cli_history_idx_t offset)
{
  /* offset 1 = most recent, 2 = second most recent, ... */
  return (cli_history_idx_t)((cli->history_head - offset + CLI_MAX_HISTORY * 2) % CLI_MAX_HISTORY);
}

/**
 * @brief Erase the current input area without redrawing the prompt.
 * @param[in] cli  Active CLI session.
 */
static void cli_history_clear_displayed_input(cli_struct_t *cli)
{
  cli_len_t len = cli->input_len;
  cli_len_t cursor = cli->cursor;

  for (cli_len_t i = 0; i < cursor; i++)
  {
    cli_write_char(cli, ASCII_BS);
  }

  for (cli_len_t i = 0; i < len; i++)
  {
    cli_write_char(cli, ' ');
  }

  for (cli_len_t i = 0; i < len; i++)
  {
    cli_write_char(cli, ASCII_BS);
  }

  cli->input[0] = '\0';
  cli->input_len = 0;
  cli->cursor = 0;
}

/**
 * @brief Move the history cursor backward or forward by one entry.
 *
 * When moving up from the tip, the current editable line is saved into
 * @c history_saved so it can be restored when navigating back down to the tip.
 * Rendering uses an in-place erase/write sequence so the prompt itself is not
 * redrawn.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] dir  Navigation direction: @c CLI_HISTORY_NAV_PREV or
 *                 @c CLI_HISTORY_NAV_NEXT.
 */
static void cli_history_navigate(cli_struct_t *cli, cli_history_nav_dir_enum_t dir)
{
  bool can_navigate = True;

  if (dir == CLI_HISTORY_NAV_PREV)
  {
#if CLI_HISTORY_RESTORE_PREBROWSE_LINE
    if (cli->history_nav == 0)
    {
      /* Save current editing line. */
      (void)cli_strlcpy(cli->history_saved, cli->input, CLI_MAX_INPUT_LEN);
    }
#endif
    if (cli->history_nav >= cli->history_count)
    {
      can_navigate = False;
    }
    else
    {
      cli->history_nav++;
    }
  }
  else
  {
    if (cli->history_nav == 0)
    {
      can_navigate = False;
    }
    else
    {
      cli->history_nav--;
    }
  }

  if (!can_navigate)
  {
    cli_write_char(cli, '\a');
  }
  else
  {
    cli_history_clear_displayed_input(cli);
    if (cli->history_nav == 0)
    {
#if CLI_HISTORY_RESTORE_PREBROWSE_LINE
      (void)cli_strlcpy(cli->input, cli->history_saved, CLI_MAX_INPUT_LEN);
#else
      cli->input[0] = '\0';
#endif
    }
    else
    {
      cli_history_idx_t idx = cli_history_index(cli, cli->history_nav);
      (void)cli_strlcpy(cli->input, &cli->history_buf[cli->history_off[idx]], CLI_MAX_INPUT_LEN);
    }
    cli->input_len = (int)strlen(cli->input);
    cli->cursor = cli->input_len;
    cli_write_str(cli, cli->input);
  }
}

#endif /* CLI_ENABLE_HISTORY */

/* Tokeniser ----------------------------------------------------------------------------*/

/**
 * @brief Split @p buf in-place on whitespace and fill @p argv.
 *
 * @param[in,out] buf       Mutable input buffer (NUL bytes inserted in-place).
 * @param[out]    argv      Receives pointers into @p buf; terminated with NULL.
 * @param[in]     max_args  Maximum entries in @p argv (including sentinel
 * NULL).
 *
 * @return Number of tokens found (argc), always < @p max_args.
 */
static cli_argc_t cli_tokenize(char *buf, const char *argv[], cli_argc_t max_args)
{
  cli_argc_t argc = 0;
  char *p = buf;

  while (*p != '\0' && argc < max_args - 1)
  {
    p = cli_skip_spaces(p);
    if (*p == '\0')
    {
      break;
    }
    argv[argc++] = p;
    p = cli_skip_non_spaces(p);
    if (*p != '\0')
    {
      *p++ = '\0';
    }
  }
  argv[argc] = NULL;
  return argc;
}

/* Command Tree Traversal ----------------------------------------------------------------*/

/**
 * @brief Print-callback that displays a single command's name and help text.
 *
 * Intended for use as a @c cli_match_cb_t with @c cli_foreach_match.
 * Sets @c *(bool *)ctx to @c true on any invocation.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] h    Command handle of the node to display.
 * @param[in] ctx  Pointer to @c bool, set to @c true.
 *
 * @return Always @c true (continue iteration).
 */
static bool cli_print_help_cb(cli_struct_t *cli, cli_cmd_handle_t h, bool *any)
{
  *any = True;
  const cli_cmd_struct_t *cmd = &cli->cmd_pool[h];
#if CLI_ENABLE_COMMAND_HELP
  char line[CLI_MAX_NAME_LEN + CLI_MAX_HELP_LEN + 4];
#else
  char line[22U + 1U];
#endif
  size_t pos = 0;
  pos = cli_strlcpy(&line[pos], "  ", sizeof(line));
  pos = pos + cli_strlcpy(&line[pos], cmd->name, sizeof(line) - pos);
  while (pos < 22U)
  {
    line[pos++] = ' ';
  }
#if CLI_ENABLE_COMMAND_HELP
  if (cmd->help[0] != '\0')
  {
    (void)cli_strlcpy(&line[pos], cmd->help, sizeof(line) - pos);
  }
#endif
  line[pos] = '\0';
  cli_write_str(cli, line);
  CLI_WSTR(cli, CLI_CRLF);
  return True;
}

/**
 * @brief Find all commands under @p parent matching @p word (prefix).
 *
 * Scans the command pool and invokes @p cb for each match. When @p word is
 * empty every visible child of @p parent is reported.
 *
 * @param[in] cli     Active CLI session.
 * @param[in] word    Prefix to match, or empty string for all.
 * @param[in] parent  Root of the sub-tree to search.
 * @param[in] cb      Callback for each match.
 * @param[in] ctx     Passed through to @p cb.
 */
static void cli_foreach_match(cli_struct_t *cli, const char *word, cli_cmd_handle_t parent,
                              cli_match_cb_t cb, bool *ctx)
{
  cli_len_t word_len = (cli_len_t)strlen(word);
  for (cli_poolsize_t i = 0; i < cli->cmd_pool_size; i++)
  {
    const cli_cmd_struct_t *cmd = &cli->cmd_pool[i];
    bool visible =
        (CLI_CMD_IS_IN_USE(cmd) && cmd->parent == parent && !CLI_CMD_IS_HIDDEN(cmd) &&
         cmd->privilege <= cli->privilege && (cmd->mode == CLI_MODE_ANY || cmd->mode == cli->mode));
    if (visible && word_len > 0)
    {
      visible = cli_case_prefix_match(cmd->name, word);
#if CLI_ENABLE_ALIASES
      if (!visible)
      {
        for (cli_len_t a = 0; a < cmd->num_aliases && !visible; a++)
        {
          visible = cli_case_prefix_match(cmd->aliases[a], word);
        }
      }
#endif
    }
    if (visible)
    {
      if (!cb(cli, i, ctx))
      {
        return;
      }
    }
  }
}

/**
 * @brief Resolve @p word under @p parent to a single command handle.
 *
 * @param[in]  cli     Active CLI session.
 * @param[in]  word    Exact or prefix name / alias to resolve.
 * @param[in]  parent  Root of the sub-tree to search.
 * @param[out] out     Receives the matched handle on @c CLI_OK.
 *
 * @retval CLI_OK        Unique or exact match found — stored in @p *out.
 * @retval CLI_ERR       No match.
 * @retval CLI_ERR_AMBIG Multiple prefix matches with no exact match.
 */
static int8_t cli_resolve_one(cli_struct_t *cli, const char *word, cli_cmd_handle_t parent,
                              cli_cmd_handle_t *out)
{
  cli_cmd_handle_t first = CLI_CMD_INVALID;
  cli_cmd_handle_t exact = CLI_CMD_INVALID;
  cli_len_t count = 0;

  for (cli_poolsize_t i = 0; i < cli->cmd_pool_size; i++)
  {
    const cli_cmd_struct_t *cmd = &cli->cmd_pool[i];
    if (!CLI_CMD_IS_IN_USE(cmd) || cmd->parent != parent || CLI_CMD_IS_HIDDEN(cmd) ||
        cmd->privilege > cli->privilege)
    {
      continue;
    }
    if (cmd->mode != CLI_MODE_ANY && cmd->mode != cli->mode)
    {
      continue;
    }

    bool match = cli_case_prefix_match(cmd->name, word);
#if CLI_ENABLE_ALIASES
    if (!match)
    {
      for (cli_len_t a = 0; a < cmd->num_aliases && !match; a++)
      {
        match = cli_case_prefix_match(cmd->aliases[a], word);
      }
    }
#endif
    if (!match)
    {
      continue;
    }

    count++;
    if (count == 1)
    {
      first = i;
    }

    if (exact == CLI_CMD_INVALID && strcmp(cmd->name, word) == 0)
    {
      exact = i;
    }
#if CLI_ENABLE_ALIASES
    if (exact == CLI_CMD_INVALID)
    {
      for (cli_len_t a = 0; a < cmd->num_aliases; a++)
      {
        if (strcmp(cmd->aliases[a], word) == 0)
        {
          exact = i;
          break;
        }
      }
    }
#endif
  }

  int8_t rc;
  if (count == 0)
  {
    rc = CLI_ERR;
  }
  else if (exact != CLI_CMD_INVALID)
  {
    *out = exact;
    rc = CLI_OK;
  }
  else if (count == 1)
  {
    *out = first;
    rc = CLI_OK;
  }
  else
  {
    rc = CLI_ERR_AMBIG;
  }
  return rc;
}

/**
 * @brief Test whether any command under @p parent matches @p word.
 *
 * @param[in] cli     Active CLI session.
 * @param[in] word    Prefix to test.
 * @param[in] parent  Root of the sub-tree to search.
 *
 * @return @c true when at least one match exists, otherwise @c false.
 */
static bool cli_has_any_match(cli_struct_t *cli, const char *word, cli_cmd_handle_t parent)
{
  bool found = False;
  for (cli_poolsize_t i = 0; i < cli->cmd_pool_size && !found; i++)
  {
    const cli_cmd_struct_t *cmd = &cli->cmd_pool[i];
    if (!CLI_CMD_IS_IN_USE(cmd) || cmd->parent != parent || CLI_CMD_IS_HIDDEN(cmd) ||
        cmd->privilege > cli->privilege)
    {
      continue;
    }
    if (cmd->mode != CLI_MODE_ANY && cmd->mode != cli->mode)
    {
      continue;
    }
    if (cli_case_prefix_match(cmd->name, word))
    {
      found = True;
    }
#if CLI_ENABLE_ALIASES
    if (!found)
    {
      for (cli_len_t a = 0; a < cmd->num_aliases && !found; a++)
      {
        if (cli_case_prefix_match(cmd->aliases[a], word))
        {
          found = True;
        }
      }
    }
#endif
  }
  return found;
}

/**
 * @brief Test whether @p parent has at least one registered direct child.
 *
 * @param[in] parent  Parent command handle.
 *
 * @return @c true when a direct child exists, otherwise @c false.
 */
static bool cli_cmd_has_children(cli_struct_t *cli, cli_cmd_handle_t parent)
{
  bool has_children = False;

  for (cli_poolsize_t i = 0; i < cli->cmd_pool_size; i++)
  {
    if (CLI_CMD_IS_IN_USE(&cli->cmd_pool[i]) && cli->cmd_pool[i].parent == parent)
    {
      has_children = True;
      break;
    }
  }

  return has_children;
}

/**
 * @brief Return the handle at the top of the context stack, or @c
 * CLI_CMD_INVALID.
 * @param[in] cli  Active CLI session.
 * @return Top-of-stack handle, or @c CLI_CMD_INVALID if the stack is empty.
 */
static cli_cmd_handle_t cli_context_current(const cli_struct_t *cli)
{
  return (cli && cli->context != CLI_CMD_ROOT) ? cli->context : CLI_CMD_INVALID;
}

/**
 * @brief Set the active submenu context to @p node.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] node  Target node handle.
 */
static void cli_context_set_to_node(cli_struct_t *cli, cli_cmd_handle_t node)
{
  cli->context = node;
}

/**
 * @brief Return the effective parent handle for command matching.
 *
 * Returns the top context-stack node when set; otherwise looks up the
 * hostname node for user-defined modes; otherwise @c CLI_CMD_ROOT.
 *
 * @param[in] cli  Active CLI session.
 * @return Effective parent handle for the current menu context.
 */
static cli_cmd_handle_t cli_menu_context_parent(const cli_struct_t *cli)
{
  cli_cmd_handle_t ret = CLI_CMD_ROOT;

  if (cli == NULL)
  {
    ret = CLI_CMD_INVALID;
  }
  else
  {
    cli_cmd_handle_t cur = cli_context_current(cli);
    if (cur != CLI_CMD_INVALID)
    {
      ret = cur;
    }
    else if (cli->mode >= CLI_MODE_USER_BASE && cli->hostname[0] != '\0')
    {
      for (cli_poolsize_t i = 0; i < cli->cmd_pool_size; i++)
      {
        if (!CLI_CMD_IS_IN_USE(&cli->cmd_pool[i]))
        {
          continue;
        }
        if (strcmp(cli->cmd_pool[i].name, cli->hostname) == 0)
        {
          ret = i;
          break;
        }
      }
    }
    else
    {
      /* To silence MISRA */
    }
  }

  return ret;
}

/* Tab Completion -----------------------------------------------------------------------*/

#if CLI_ENABLE_TAB_COMPLETION
/**
 * @brief Attempt tab-completion on the current input buffer.
 *
 * On a unique match: completes the word and appends a space.
 * On multiple matches: extends to the longest common prefix, then lists all
 * candidates on a new line.
 * On no match: rings the terminal bell.
 *
 * @param[in] cli  Active CLI session.
 */
static void cli_tab_complete(cli_struct_t *cli)
{
  /* Work on a scratch copy so we can tokenise */
  char scratch[CLI_MAX_INPUT_LEN];
  (void)cli_strlcpy(scratch, cli->input, CLI_MAX_INPUT_LEN);

  const char *argv[CLI_MAX_ARGS];
  cli_argc_t argc = cli_tokenize(scratch, argv, CLI_MAX_ARGS);
  bool done = False;

  /* Determine if last char is a space (completing next word) */
  bool trailing_space = (cli->input_len > 0 && cli->input[cli->input_len - 1] == ' ');

  const char *partial = (argc > 0 && !trailing_space) ? argv[argc - 1] : "";
  int depth = (trailing_space) ? argc : ((argc > 0) ? argc - 1 : 0);

  /* Walk command tree through fully-matched tokens */
  cli_cmd_handle_t parent = cli_menu_context_parent(cli);
  for (int d = 0; d < depth; d++)
  {
    cli_cmd_handle_t h;
    int8_t r = cli_resolve_one(cli, argv[d], parent, &h);
    if (r != CLI_OK)
    {
      cli_write_char(cli, '\a');
      done = True;
      break;
    }
    parent = h;
  }

  if (!done)
  {
    /* Pass 1: count matches, find first, compute common prefix */
    cli_len_t count = 0;
    cli_cmd_handle_t first = CLI_CMD_INVALID;
    cli_len_t common = 0;
    cli_len_t partial_len = (cli_len_t)strlen(partial);

    for (cli_poolsize_t i = 0; i < cli->cmd_pool_size; i++)
    {
      const cli_cmd_struct_t *cmd = &cli->cmd_pool[i];
      bool visible = (CLI_CMD_IS_IN_USE(cmd) && cmd->parent == parent && !CLI_CMD_IS_HIDDEN(cmd) &&
                      cmd->privilege <= cli->privilege &&
                      (cmd->mode == CLI_MODE_ANY || cmd->mode == cli->mode));
      if (!visible)
      {
        continue;
      }

      bool name_match = cli_case_prefix_match(cmd->name, partial);
#if CLI_ENABLE_ALIASES
      if (!name_match)
      {
        for (cli_len_t a = 0; a < cmd->num_aliases && !name_match; a++)
        {
          name_match = cli_case_prefix_match(cmd->aliases[a], partial);
        }
      }
#endif
      if (!name_match)
      {
        continue;
      }

      count++;
      if (count == 1)
      {
        first = i;
        common = (cli_len_t)strlen(cmd->name);
      }
      else
      {
        cli_len_t j = 0;
        while (j < common && cli->cmd_pool[first].name[j] != '\0' &&
               tolower((int)(unsigned char)cli->cmd_pool[first].name[j]) ==
                   tolower((int)(unsigned char)cmd->name[j]))
        {
          j++;
        }
        common = j;
      }
    }

    if (count == 0)
    {
      cli_write_char(cli, '\a');
      done = True;
    }
    else if (count == 1)
    {
      /* Unique match — complete the word */
      const char *full = cli->cmd_pool[first].name;
#if CLI_ENABLE_ALIASES
      bool match = cli_case_prefix_match(full, partial);
      if (*partial != '\0' && !match)
      {
        const cli_cmd_struct_t *m = &cli->cmd_pool[first];
        for (cli_len_t a = 0; a < m->num_aliases; a++)
        {
          if (cli_case_prefix_match(m->aliases[a], partial))
          {
            full = m->aliases[a];
            break;
          }
        }
      }
#endif
      size_t pfx_len = strlen(partial);
      size_t i = pfx_len;
      while (full[i] != '\0')
      {
        cli_insert_char(cli, full[i]);
        i++;
      }
      cli_insert_char(cli, ' ');
      done = True;
    }
    else
    {
      /* To silence MISRA */
    }

    if (!done && common > partial_len)
    {
      /* Complete to common prefix */
      const char *full = cli->cmd_pool[first].name;
      for (cli_len_t i = partial_len; i < common; i++)
      {
        cli_insert_char(cli, full[i]);
      }
      done = True;
    }

    if (!done)
    {
      /* Pass 2: print all matches */
      bool dummy = False;
      CLI_WSTR(cli, CLI_CRLF);
      cli_foreach_match(cli, partial, parent, cli_print_help_cb, &dummy);
      cli_redraw_line(cli);
    }
  }
}
#endif /* CLI_ENABLE_TAB_COMPLETION */

/* Inline '?' Help ----------------------------------------------------------------------*/

/**
 * @brief Print context-sensitive @c ? help for the current input line.
 *
 * Lists all commands / sub-commands that match the tokens typed so far,
 * then redraws the input line.
 *
 * @param[in] cli  Active CLI session.
 */
static void cli_show_help(cli_struct_t *cli)
{
  char scratch[CLI_MAX_INPUT_LEN];
  /* Remove the trailing '?' before tokenising */
  cli_len_t len = cli->input_len;
  if (len > 0 && cli->input[len - 1] == '?')
  {
    len--;
  }
  (void)cli_strlcpy(scratch, cli->input, CLI_MAX_INPUT_LEN);
  if (len < (int)CLI_MAX_INPUT_LEN)
  {
    scratch[len] = '\0';
  }

  const char *argv[CLI_MAX_ARGS];
  cli_argc_t argc = cli_tokenize(scratch, argv, CLI_MAX_ARGS);

  bool trailing_space = (len > 0 && scratch[len - 1] == ' ');
  const char *partial = (argc > 0 && !trailing_space) ? argv[argc - 1] : "";
  int depth = (trailing_space) ? argc : ((argc > 0) ? argc - 1 : 0);
  bool show = True;

  cli_cmd_handle_t parent = cli_menu_context_parent(cli);
  cli_cmd_handle_t h;
  for (int d = 0; d < depth; d++)
  {
    int8_t r = cli_resolve_one(cli, argv[d], parent, &h);
    if (r != CLI_OK)
    {
      if (r == CLI_ERR)
      {
        CLI_WSTR(cli, "\r\n% Unknown command.\r\n");
        cli_redraw_line(cli);
        show = False;
      }
      else
      {
        CLI_WSTR(cli, "\r\n% Ambiguous command.\r\n");
        cli_redraw_line(cli);
        show = False;
      }
      break;
    }
    parent = h;
  }

  if (show)
  {
    if (!trailing_space && partial[0] != '\0')
    {
      if (cli_resolve_one(cli, partial, parent, &h) == CLI_OK)
      {
        parent = h;
        partial = "";
      }
    }

    bool any = False;
    CLI_WSTR(cli, CLI_CRLF);
    cli_foreach_match(cli, partial, parent, cli_print_help_cb, &any);
    if (!any)
    {
      CLI_WSTR(cli, "  <cr>\r\n");
    }
    cli_redraw_line(cli);
  }
}

/* Output Filter (Pipe Processing) ------------------------------------------------------*/

#if CLI_ENABLE_PIPE_FILTER
/**
 * @brief Reset all pipe-filter state to @c CLI_FILTER_NONE.
 * @param[in] cli  Active CLI session.
 */
static void cli_filter_reset(cli_struct_t *cli)
{
  cli->filter_type = CLI_FILTER_NONE;
  cli->filter_pattern[0] = '\0';
  cli->filter_count = 0;
  cli->filter_begin_found = False;
  cli->filter_line_buf_len = 0;
}

/**
 * @brief Decide whether @p line should be emitted given the active filter.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] line  NUL-terminated output line to test.
 *
 * @return @c true if the line should be shown, @c false if it should be
 * suppressed.
 */
static bool cli_filter_line(cli_struct_t *cli, const char *line)
{
  bool result = True;

  switch (cli->filter_type)
  {
  case CLI_FILTER_NONE:
    break;

  case CLI_FILTER_GREP:
    result = (strstr(line, cli->filter_pattern) != NULL);
    break;

  case CLI_FILTER_EXCLUDE:
    result = (strstr(line, cli->filter_pattern) == NULL);
    break;

  case CLI_FILTER_BEGIN:
    if (cli->filter_begin_found)
    {
    }
    else if (strstr(line, cli->filter_pattern) != NULL)
    {
      cli->filter_begin_found = True;
    }
    else
    {
      result = False;
    }
    break;

  case CLI_FILTER_COUNT:
    cli->filter_count++;
    result = False; /* accumulate, print in flush */
    break;

  default:
    /* To silence MISRA */
    break;
  }

  return result;
}

/**
 * @brief Flush any deferred filter output (e.g. the count total for @c |
 * count).
 * @param[in] cli  Active CLI session.
 */
static void cli_filter_flush(cli_struct_t *cli)
{
  if (cli->filter_type == CLI_FILTER_COUNT)
  {
    char buf[32];
#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
    snprintf_P(buf, sizeof(buf), PSTR("Count: %d\r\n"), cli->filter_count);
#else
    (void)cli_snprintf(buf, sizeof(buf), "Count: %d\r\n", cli->filter_count);
#endif
    cli_write_str(cli, buf);
  }
}
#endif /* CLI_ENABLE_PIPE_FILTER */

#if CLI_ENABLE_PIPE_FILTER
/* Pipe Filter Parser -------------------------------------------------------------------*/

/**
 * @brief Parse and activate a pipe filter from the input line.
 *
 * Locates the first @c '|' in @p line, NUL-terminates the command portion,
 * and configures the session filter state from the filter keyword and pattern
 * that follow.
 *
 * @param[in] cli   Active CLI session (filter state updated in-place).
 * @param[in] line  Mutable command line string (modified by NUL insertion).
 *
 * @return Position of @c '|' in the original line, or -1 if no pipe was found.
 */
static ptrdiff_t cli_parse_filter(cli_struct_t *cli, const char *line)
{
  cli_filter_reset(cli);

  char *pipe = strchr(line, ASCII_PIPE);
  ptrdiff_t ret = -1;

  if (pipe != NULL)
  {
    *pipe = '\0'; /* truncate the command at '|' */
    ptrdiff_t idx = 1;
    while (pipe[idx] == ' ')
    {
      idx++;
    }

    const char *kw = &pipe[idx];
    bool is_grep = cli_case_prefix_match(kw, "grep");
    bool is_include = cli_case_prefix_match(kw, "include");
    if (is_grep || is_include)
    {
      cli->filter_type = CLI_FILTER_GREP;
      idx += (cli_case_prefix_match(kw, "grep")) ? 4 : 7;
    }
    else if (cli_case_prefix_match(kw, "exclude"))
    {
      cli->filter_type = CLI_FILTER_EXCLUDE;
      idx += 7;
    }
    else if (cli_case_prefix_match(kw, "begin"))
    {
      cli->filter_type = CLI_FILTER_BEGIN;
      idx += 5;
    }
    else if (cli_case_prefix_match(kw, "count"))
    {
      cli->filter_type = CLI_FILTER_COUNT;
    }
    else
    {
      /* To silence MISRA */
    }

    if (cli->filter_type != CLI_FILTER_COUNT)
    {
      while (pipe[idx] == ' ')
      {
        idx++;
      }
      (void)cli_strlcpy(cli->filter_pattern, &pipe[idx], CLI_MAX_INPUT_LEN);
      cli_len_t plen = (cli_len_t)strlen(cli->filter_pattern);
      while (plen > 0 && cli->filter_pattern[plen - 1] == ' ')
      {
        cli->filter_pattern[--plen] = '\0';
      }
    }

    ret = pipe - line;
  }

  return ret;
}
#endif /* CLI_ENABLE_PIPE_FILTER */

/* Command Execution --------------------------------------------------------------------*/

/* Single-Byte Processor  (heart of the non-blocking API) -------------------------------*/
/**
 * @brief Feed one received byte into the CLI engine.
 *
 * Handles escape sequences, control characters (Ctrl+A/E/B/F/K/W/U/D/L,
 * Tab, Backspace, Enter), printable characters, and the @c ? inline-help
 * trigger.  Call from an RTOS task, ISR-driven drain, or select/poll loop.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] byte  The received byte to process.
 *
 * @return @c CLI_OK keep feeding. @n
 *         @c CLI_ERR_QUIT session ended cleanly. @n
 *         @c CLI_ERR transport / internal error.
 */
static int8_t cli_feed(cli_struct_t *cli, int byte)
{
  int8_t rc = CLI_ERR;
  bool printable = False;

  if (cli != NULL && cli->session_state != CLI_SESSION_STOP)
  {
    cli_touch_activity_internal(cli);
    rc = CLI_OK;

    if (cli->saw_cr && byte == ASCII_LF)
    {
      cli->saw_cr = False;
    }
    else
    {
      cli->saw_cr = (bool)(byte == ASCII_CR);

      cli->suppress_help_newline = False;

      /* Escape / CSI sequences */
      bool esc_processed = cli_process_escape(cli, byte);
      if (!esc_processed)
      {
        /* Control characters */
        switch (byte)
        {
        /* Enter */
        case ASCII_CR:
          /* fallthrough */
        case ASCII_LF:
        {
          CLI_WSTR(cli, "\n\r");
          int8_t exec_rc = cli_submit_line(cli);
          if (exec_rc == CLI_ERR_QUIT)
          {
            cli->session_state = CLI_SESSION_STOP;
            rc = CLI_ERR_QUIT;
          }
          else if (exec_rc == CLI_ERR_AUTH)
          {
            cli->session_state = CLI_SESSION_STOP;
            rc = exec_rc;
          }
          else if (exec_rc == CLI_EXEC_REPLAY_INPUT)
          {
          }
          else if (cli->session_state == CLI_SESSION_RUN)
          {
            cli_show_prompt(cli);
          }
          else
          {
            /* To silence MISRA */
          }
        }
        break;

        /* Tab — completion */
#if CLI_ENABLE_TAB_COMPLETION
        case ASCII_TAB:
          cli_tab_complete(cli);
          break;
#endif

        /* Backspace / DEL */
        case ASCII_BS:
        case ASCII_DEL:
          cli_backspace(cli);
          break;

        /* Ctrl+A — beginning of line */
        case 0x01:
          cli_move_cursor(cli, 0);
          break;

        /* Ctrl+E — end of line */
        case 0x05:
          cli_move_cursor_end(cli);
          break;

        /* Ctrl+B — back one char */
        case 0x02:
          cli_move_cursor_delta(cli, -1);
          break;

        /* Ctrl+F — forward one char */
        case 0x06:
          cli_move_cursor_delta(cli, 1);
          break;

        /* Ctrl+K — kill to EOL */
        case 0x0B:
          cli_kill_line(cli);
          break;

        /* Ctrl+W — kill word backward */
        case 0x17:
          cli_kill_word(cli);
          break;

        /* Ctrl+U — kill whole line */
        case 0x15:
          cli_clear_input(cli);
          break;

        /* Ctrl+D — delete char under cursor */
        case 0x04:
          if (cli->input_len == 0)
          {
            /* EOF on empty line -> quit */
            CLI_WSTR(cli, CLI_CRLF);
            cli->session_state = CLI_SESSION_STOP;
            rc = CLI_ERR_QUIT;
          }
          else
          {
            cli_delete_char(cli);
          }
          break;

        /* Ctrl+L — clear screen and redraw */
        case 0x0C:
          CLI_WSTR(cli, "\x1b"
                        "[2J\x1b"
                        "[H");
          cli_redraw_line(cli);
          break;

#if CLI_ENABLE_HISTORY
        /* Ctrl+P — history up (previous command) */
        case 0x10:
          cli_history_navigate(cli, CLI_HISTORY_NAV_PREV);
          break;

        /* Ctrl+N — history down (next command) */
        case 0x0E:
          cli_history_navigate(cli, CLI_HISTORY_NAV_NEXT);
          break;
#endif /* CLI_ENABLE_HISTORY */

        /* Ignore other control characters */
        default:
          if (byte >= 0x20 && byte <= 0x7E)
          {
            printable = True;
          }
          break;
        }

        if (printable)
        {
          if (byte == ASCII_QUESTION)
          {
            cli_len_t help_cursor = cli->cursor;
            cli_insert_char(cli, '?');
            if (cli->ansi_supported)
            {
              cli_show_help(cli);
              cli_backspace(cli); /* remove the '?' after showing help */
            }
            else
            {
              /* Dumb terminal: remove '?' before redraw; do not emit backspace cleanup bytes. */
              if (help_cursor < cli->input_len)
              {
                (void)memmove(&cli->input[help_cursor], &cli->input[help_cursor + 1],
                              (size_t)(cli->input_len - help_cursor - 1));
                cli->input_len--;
                cli->input[cli->input_len] = '\0';
                cli->cursor = help_cursor;
              }
              cli_show_help(cli);
            }
            cli->suppress_help_newline = True;
          }
          else
          {
            /* Printable character — insert at cursor */
            cli_insert_char(cli, (char)byte);
          }
        }
      }
    }
  }

  return rc;
}

/**
 * @brief Iterative command tree walk and execution.
 *
 * Matches tokens in @p argv against the command tree, descending through
 * intermediate nodes, and calls the leaf callback with remaining tokens.
 *
 * @param[in] cli     Active CLI session.
 * @param[in] argv    Full token array.
 * @param[in] argc    Total number of tokens.
 * @param[in] depth   Current token index being matched.
 * @param[in] parent  Node handle to match children of.
 *
 * @return @c CLI_OK, @c CLI_ERR, @c CLI_ERR_QUIT, or @c CLI_ERR_AMBIG.
 */
static int8_t cli_execute_command(cli_struct_t *cli, const char *argv[], cli_argc_t argc,
                                  cli_argc_t depth, cli_cmd_handle_t parent)
{
  int8_t rc = CLI_ERR;
  bool done = False;

  for (; depth < argc && !done; depth++)
  {
    cli_cmd_handle_t h;
    int8_t r = cli_resolve_one(cli, argv[depth], parent, &h);

    if (r == CLI_ERR)
    {
      CLI_WSTR(cli, "% Unknown command: ");
      cli_write_str(cli, argv[depth]);
      CLI_WSTR(cli, CLI_CRLF);
      done = True;
    }
    else if (r == CLI_ERR_AMBIG)
    {
      CLI_WSTR(cli, "% Ambiguous command: ");
      cli_write_str(cli, argv[depth]);
      CLI_WSTR(cli, CLI_CRLF);
      rc = CLI_ERR_AMBIG;
      done = True;
    }
    else
    {
      // --- Execution Path ---
      const cli_cmd_struct_t *cmd = &cli->cmd_pool[h];
      bool has_children = cli_cmd_has_children(cli, h);

      /* If this node has children and more tokens remain, descend to next level */
      if (has_children && depth + 1 < argc)
      {
        parent = h;
        /* continue implied by loop increment */
      }
      else
      {
        /* Execute this node's callback with remaining tokens as args */
        if (cmd->callback != NULL)
        {
          cli_argc_t rem_argc = argc - depth;
          rc = cmd->callback(cli, cmd->name, rem_argc, &argv[depth]);

          if (rc == CLI_OK && has_children && rem_argc == 1)
          {
            cli_context_set_to_node(cli, h);
          }
        }
        else
        {
          CLI_WSTR(cli, "% Incomplete command.\r\n");
        }
        done = True;
      }
    }
  }

  /* Handle natural exhaustion (partial match) */
  /* If the loop finished naturally (depth == argc) and the parent node has a
     callback, call it (allows parent aggregation, e.g. "usage commands"). */
  if (depth == argc && (cli_cmd_handle_t)0 <= parent && parent < cli->cmd_pool_size &&
      cli->cmd_pool[parent].callback)
  {
    rc = cli->cmd_pool[parent].callback(cli, cli->cmd_pool[parent].name, 0, &argv[depth]);
  }

  return rc;
}

/**
 * @brief Parse and execute the current input buffer.
 *
 * Trims whitespace, extracts any pipe filter, tokenises, adds to history,
 * and dispatches via @c cli_execute_command().
 *
 * @param[in] cli  Active CLI session.
 *
 * @return @c CLI_OK on success, @c CLI_ERR_QUIT on exit/quit, @c CLI_ERR on
 * failure.
 */
static int8_t cli_execute(cli_struct_t *cli)
{
  int8_t rc = CLI_OK;

  /* Trim leading/trailing whitespace */
  char *start = cli_skip_spaces(cli->input);

  cli_len_t slen = (cli_len_t)strlen(start);
  while (slen > 0 && (start[slen - 1] == ' ' || start[slen - 1] == '\t'))
  {
    start[--slen] = '\0';
  }

  if (slen > 0)
  {
#if CLI_ENABLE_HISTORY
#if CLI_HISTORY_INDEX_SELECTION
    char history_line[CLI_MAX_INPUT_LEN];
    (void)cli_strlcpy(history_line, start, CLI_MAX_INPUT_LEN);
#else
    cli_history_add(cli, cli->input);
#endif
#endif

    /* Extract pipe filter before tokenising the command */
#if CLI_ENABLE_PIPE_FILTER
    (void)cli_parse_filter(cli, start);
#endif

    const char *argv[CLI_MAX_ARGS];
    cli_argc_t argc = cli_tokenize(start, argv, CLI_MAX_ARGS);

    if (argc > 0)
    {
      cli_cmd_handle_t start_parent = CLI_CMD_INVALID;
      cli_cmd_handle_t menu_parent = cli_menu_context_parent(cli);

      bool has_match = cli_has_any_match(cli, argv[0], menu_parent);
      if (menu_parent != CLI_CMD_ROOT && has_match)
      {
        start_parent = menu_parent;
      }

      if (start_parent == CLI_CMD_INVALID)
      {
        start_parent = cli_has_any_match(cli, argv[0], CLI_CMD_ROOT) ? CLI_CMD_ROOT : menu_parent;
      }

      rc = cli_execute_command(cli, argv, argc, 0, start_parent);
    }

#if CLI_ENABLE_HISTORY
#if CLI_HISTORY_INDEX_SELECTION
    if (rc != CLI_EXEC_REPLAY_INPUT)
    {
      cli_history_add(cli, history_line);
    }
#endif
#endif
  }

#if CLI_ENABLE_PIPE_FILTER
  cli_filter_flush(cli);
  cli_filter_reset(cli);
#endif

  return rc;
}

/* Authentication -----------------------------------------------------------------------*/

#if CLI_ENABLE_AUTH
/**
 * @brief Clear the in-progress username and password collection buffers.
 * @param[in] cli  Active CLI session.
 */
static void cli_auth_reset_work(cli_struct_t *cli)
{
  cli->auth_username[0] = '\0';
  cli_reset_input(cli);
}

/**
 * @brief Reset auth work buffers and print the @c "Username: " prompt.
 * @param[in] cli  Active CLI session.
 */
static void cli_auth_prompt_username(cli_struct_t *cli)
{
  cli_auth_reset_work(cli);
  cli->auth_state = CLI_AUTH_USERNAME;
  CLI_WSTR(cli, "Username: ");
  cli_flush(cli);
}

/**
 * @brief Test whether the current auth lockout period has expired.
 * @param[in] cli  Active CLI session.
 * @return @c true if the lockout has expired or is not active.
 */
static bool cli_auth_lockout_expired(const cli_struct_t *cli)
{
  bool expired = True;

  if (cli != NULL)
  {
#if CLI_ENABLE_TIME_SOURCE
    if (cli->auth_failure_mode == CLI_AUTH_FAILURE_MODE_LOCKOUT &&
        cli->auth_lockout_seconds != 0UL && cli->time_source)
    {

      uint32_t now = cli->time_source(cli->time_source_ctx);
      uint32_t elapsed = now - cli->auth_lockout_started_ts;

      expired = (elapsed >= cli->auth_lockout_seconds);
    }
#endif
  }

  return expired;
}

static bool cli_state_is_auth_input(const cli_struct_t *cli)
{
  return cli &&
         (cli->auth_state == CLI_AUTH_USERNAME || cli->auth_state == CLI_AUTH_PASSWORD ||
          cli->auth_state == CLI_AUTH_ENABLE_MODE_PASSWORD || cli->auth_state == CLI_AUTH_LOCKOUT);
}

/**
 * @brief Verify collected credentials and transition auth state.
 *
 * Calls @c authorization_cb or searches the local user table.  On success, marks the
 * session authenticated and transitions to @c CLI_SESSION_RUN.  On failure,
 * increments the attempt counter and re-prompts or locks out as configured.
 *
 * @param[in] cli  Active CLI session.
 *
 * @return @c CLI_OK to continue, @c CLI_ERR_AUTH on permanent failure.
 */
static int8_t cli_auth_verify(cli_struct_t *cli)
{
  int8_t ok = CLI_ERR_AUTH;
  int8_t rc = CLI_OK;

  cli->mode_login = CLI_MODE_EXEC;

  if (cli->authorization_cb != NULL)
  {
    ok = cli->authorization_cb(cli->auth_username, cli->input);
  }
  else
  {
    for (cli_len_t i = 0; i < cli->num_users; i++)
    {
      if (strcmp(cli->users[i], cli->auth_username) == 0 &&
          strcmp(cli->passwords[i], cli->input) == 0)
      {
        ok = CLI_OK;
        cli_set_privilege(cli, cli->user_privileges[i]);
        if (cli->user_privileges[i] >= CLI_PRIV_PRIVILEGED)
        {
          cli->mode_login = CLI_MODE_ENABLE;
          cli->mode_prev = cli->mode;
          cli_set_mode(cli, CLI_MODE_ENABLE);
        }
        break;
      }
    }
  }

  if (ok == CLI_OK)
  {
    cli->auth_state = CLI_AUTH_AUTHENTICATED;
    rc = CLI_OK;
  }
  else
  {
    cli->auth_attempts++;
    CLI_WSTR(cli, "% Authentication failed.\r\n");

    if (cli->auth_attempts >= 3)
    {
      CLI_WSTR(cli, "% Too many failed attempts.\r\n");
      if (cli->auth_failure_mode == CLI_AUTH_FAILURE_MODE_LOCKOUT)
      {
        cli->auth_state = CLI_AUTH_LOCKOUT;
#if CLI_ENABLE_TIME_SOURCE
        if (cli->time_source != NULL)
        {
          cli->auth_lockout_started_ts = cli->time_source(cli->time_source_ctx);
        }
#endif
        rc = CLI_OK;
      }
      else
      {
        cli->session_state = CLI_SESSION_STOP;
        rc = CLI_ERR_AUTH;
      }
    }
    else
    {
      cli_auth_prompt_username(cli);
      rc = CLI_OK;
    }
  }

  return rc;
}

static bool cli_auth_secret_input(const cli_struct_t *cli)
{
  return cli &&
         (cli->auth_state == CLI_AUTH_PASSWORD || cli->auth_state == CLI_AUTH_ENABLE_MODE_PASSWORD);
}

static cli_len_t cli_auth_input_limit(const cli_struct_t *cli)
{
  cli_len_t limit = CLI_MAX_INPUT_LEN;

  if (cli != NULL)
  {
    if (cli->auth_state == CLI_AUTH_USERNAME)
    {
      limit = CLI_MAX_USERNAME_LEN;
    }
    else if (cli->auth_state == CLI_AUTH_PASSWORD)
    {
      limit = CLI_MAX_PASSWORD_LEN;
    }
    else if (cli->auth_state == CLI_AUTH_ENABLE_MODE_PASSWORD)
    {
      limit = CLI_MAX_ENABLE_SECRET_LEN;
    }
    else
    {
      /* To silence MISRA */
    }
  }

  return limit;
}

static int8_t cli_auth_submit_line(cli_struct_t *cli)
{
  int8_t rc = CLI_OK;

  if (cli->auth_state == CLI_AUTH_USERNAME)
  {
    (void)cli_strlcpy(cli->auth_username, cli->input, CLI_MAX_USERNAME_LEN);
    cli_reset_input(cli);
    cli->auth_state = CLI_AUTH_PASSWORD;
    CLI_WSTR(cli, "Password: ");
    cli_flush(cli);
  }
  else if (cli->auth_state == CLI_AUTH_PASSWORD)
  {
    rc = cli_auth_verify(cli);
    cli_reset_input(cli);
  }
  else if (cli->auth_state == CLI_AUTH_ENABLE_MODE_PASSWORD)
  {
    int ok;

    if (cli->enable_privilege_cb != NULL)
    {
      ok = cli->enable_privilege_cb(cli->auth_username, cli->input);
    }
    else
    {
      ok = (strcmp(cli->input, cli->enable_privilege_secret) == 0) ? CLI_OK : CLI_ERR_AUTH;
    }

    if (ok == CLI_OK)
    {
      cli_set_privilege(cli, CLI_PRIV_PRIVILEGED);
      cli_set_mode(cli, CLI_MODE_ENABLE);
    }
    else
    {
      CLI_WSTR(cli, "% Access denied.\r\n");
      cli->mode = cli->mode_prev;
      cli->privilege = cli->enable_privilege_prev;
    }

    cli->auth_state = CLI_AUTH_AUTHENTICATED;
    cli_reset_input(cli);
  }
  else
  {
    /* To silence MISRA */
  }

  return rc;
}

static int8_t cli_auth_handle(cli_struct_t *cli, int byte)
{
  int8_t rc = CLI_OK;
  bool done = False;

  if (cli->saw_cr && byte == ASCII_LF)
  {
    cli->saw_cr = False;
    done = True;
  }
  else
  {
    cli->saw_cr = (bool)(byte == ASCII_CR);

    if (cli->esc_state != ESC_STATE_NORMAL || byte == 0x1B)
    {
      (void)cli_process_escape(cli, byte);
      done = True;
    }
  }

  if (!done)
  {
    switch (byte)
    {
    case ASCII_CR:
    case ASCII_LF:
    {
      int8_t exec_rc;

      CLI_WSTR(cli, "\n\r");
      exec_rc = cli_auth_submit_line(cli);

      if (exec_rc == CLI_ERR_AUTH)
      {
        cli->session_state = CLI_SESSION_STOP;
        rc = CLI_ERR_AUTH;
      }
      else if (cli->auth_state == CLI_AUTH_NONE || cli->auth_state == CLI_AUTH_AUTHENTICATED)
      {
        cli_show_prompt(cli);
      }
      else
      {
        /* To silence MISRA */
      }
    }
    break;

    case ASCII_BS:
    case ASCII_DEL:
      cli_auth_backspace(cli);
      break;

    case 0x0B:
    case 0x15:
      cli_auth_clear_input(cli);
      break;

    default:
      if (byte >= 0x20 && byte <= 0x7E)
      {
        cli_auth_insert_char(cli, (char)byte);
      }
      break;
    }
  }

  return rc;
}
#endif

static void cli_reset_input(cli_struct_t *cli)
{
  if (cli != NULL)
  {
    cli->input[0] = '\0';
    cli->input_len = 0;
    cli->cursor = 0;
  }
}

static int8_t cli_submit_line(cli_struct_t *cli)
{
  int8_t rc = CLI_ERR;

  if (cli != NULL)
  {
    rc = cli_execute(cli);
    if (rc == CLI_EXEC_REPLAY_INPUT)
    {
      cli_redraw_line(cli);
    }
    else
    {
      cli_reset_input(cli);
    }
  }

  return rc;
}

#if CLI_ENABLE_AUTH
static void cli_auth_backspace(cli_struct_t *cli)
{
  if (cli != NULL && cli->input_len > 0)
  {
    cli->input_len--;
    cli->input[cli->input_len] = '\0';
    cli->cursor = cli->input_len;
    if (!cli_auth_secret_input(cli))
    {
      CLI_WSTR(cli, "\x08"
                    " \x08");
    }
  }
}

static void cli_auth_clear_input(cli_struct_t *cli)
{
  if (cli != NULL)
  {
    if (!cli_auth_secret_input(cli))
    {
      while (cli->input_len > 0)
      {
        cli_auth_backspace(cli);
      }
    }
    else
    {
      cli_reset_input(cli);
    }
  }
}

static void cli_auth_insert_char(cli_struct_t *cli, char c)
{
  if (cli != NULL)
  {
    if (cli->input_len < cli_auth_input_limit(cli) - 1)
    {
      cli->input[cli->input_len++] = c;
      cli->input[cli->input_len] = '\0';
      cli->cursor = cli->input_len;
      if (!cli_auth_secret_input(cli))
      {
        cli_write_char(cli, c);
      }
    }
    else
    {
      cli_write_char(cli, ASCII_BELL);
    }
  }
}
#endif

/* Escape-Sequence State Machine --------------------------------------------------------*/

/**
 * @brief Feed one byte into the VT100 / ANSI escape-sequence state machine.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] b    Received byte.
 *
 * For example: Up Arrow sends: 0x1B (ESC), then [, then A often read as \033[A
 *
 * @return 1 if the byte was consumed by the escape machine, 0 if not.
 */
static bool cli_process_escape(cli_struct_t *cli, int b)
{
  bool consumed = False;

  switch (cli->esc_state)
  {
  case ESC_STATE_NORMAL:
    if (b == ASCII_ESC)
    {
      cli->esc_state = ESC_STATE_ESC;
      cli->esc_param = 0;
      consumed = True; /* consumed */
    }
    break; /* not an escape byte */

  case ESC_STATE_ESC:
    if (b == ASCII_LSBRACKET || b == ASCII_CAP_O)
    {
      cli->esc_state = ESC_STATE_CSI;
      consumed = True;
    }
    else
    {
      cli->esc_state = ESC_STATE_NORMAL;
    }
    break;

  case ESC_STATE_CSI:
    if (b >= ASCII_DIGIT_0 && b <= ASCII_DIGIT_9)
    {
      cli->esc_param = cli->esc_param * 10 + (b - ASCII_DIGIT_0);
      consumed = True;
    }
    else
    {
      handle_csi(cli, (char)b);
      consumed = True;
    }
    break;

  default:
    cli->esc_state = ESC_STATE_NORMAL;
    break;
  }

  return consumed;
}

/**
 * @brief Dispatch a completed CSI (Control Sequence Introducer) final byte.
 *
 * Translates arrow keys, Home, End, and Delete VT sequences into line-editing
 * operations.
 *
 * @param[in] cli    Active CLI session.
 * @param[in] final  The final byte of the CSI sequence.
 */
static void handle_csi(cli_struct_t *cli, char final)
{
  switch (final)
  {
  case 'A':
#if CLI_ENABLE_HISTORY
    cli_history_navigate(cli, CLI_HISTORY_NAV_PREV);
#endif
    break; /* Up    */

  case 'B':
#if CLI_ENABLE_HISTORY
    cli_history_navigate(cli, CLI_HISTORY_NAV_NEXT);
#endif
    break; /* Down  */

  case 'C':
    cli_move_cursor_delta(cli, 1);
    break; /* Right */

  case 'D':
    cli_move_cursor_delta(cli, -1);
    break; /* Left  */

  case 'H':
    cli_move_cursor(cli, 0);
    break; /* Home  */

  case 'F':
    cli_move_cursor_end(cli);
    break; /* End   */

  case '~':
    if (cli->esc_param == 1 || cli->esc_param == 7)
    {
      cli_move_cursor(cli, 0);
    }
    else if (cli->esc_param == 3)
    {
      cli_delete_char(cli);
    }
    else if (cli->esc_param == 4 || cli->esc_param == 8)
    {
      cli_move_cursor_end(cli);
    }
    else
    {
      /* To silence MISRA */
    }
    break;

  default:
    /* To silence MISRA */
    break;
  }
  cli->esc_state = ESC_STATE_NORMAL;
  cli->esc_param = 0;
}

/*################################### END OF FILE ######################################*/
