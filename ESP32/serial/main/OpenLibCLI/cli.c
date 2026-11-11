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
#include "utils/cli_args_parse.h"

/*=======================================================================================
 * Compile-time Configuration Checks
 *=======================================================================================*/
#if CLI_ENABLE_HISTORY
#if (CLI_MAX_HISTORY < 1) || (CLI_MAX_HISTORY > 255)
#error "CLI_MAX_HISTORY must be in the range 1..255"
#endif
#if CLI_HISTORY_BUF_SIZE < CLI_MAX_INPUT_LEN
#error "CLI_HISTORY_BUF_SIZE must be >= CLI_MAX_INPUT_LEN so every input line fits"
#endif
#if CLI_HISTORY_BUF_SIZE > 32767
#error "CLI_HISTORY_BUF_SIZE must fit in cli_history_idx_t (int16_t)"
#endif
#endif /* CLI_ENABLE_HISTORY */

#if (CLI_MAX_CONTEXT_DEPTH < 1) || (CLI_MAX_CONTEXT_DEPTH > 255)
#error "CLI_MAX_CONTEXT_DEPTH must be in the range 1..255"
#endif

#if (CLI_MAX_EXEC_DEPTH < 1) || (CLI_MAX_EXEC_DEPTH > 255)
#error "CLI_MAX_EXEC_DEPTH must be in the range 1..255"
#endif

#if CLI_ENABLE_AUTH && ((CLI_MAX_USERNAME_LEN >= CLI_MAX_INPUT_LEN) ||                           \
                        (CLI_MAX_PASSWORD_LEN >= CLI_MAX_INPUT_LEN) ||                           \
                        (CLI_MAX_ENABLE_SECRET_LEN >= CLI_MAX_INPUT_LEN))
#error "CLI_MAX_USERNAME_LEN, CLI_MAX_PASSWORD_LEN and CLI_MAX_ENABLE_SECRET_LEN must be smaller than CLI_MAX_INPUT_LEN"
#endif

#if CLI_MAX_ARGS < 2
#error "CLI_MAX_ARGS must be at least 2: it holds up to CLI_MAX_ARGS - 1 tokens and the end marker"
#endif

#if CLI_MAX_RX_BYTES_PER_TICK < 1
#error "CLI_MAX_RX_BYTES_PER_TICK must be at least 1"
#endif

#if CLI_MAX_OUTPUT_BUF < 3
#error "CLI_MAX_OUTPUT_BUF must be at least 3: an error line holds the \"% \" prefix and a NUL"
#endif

#if (CLI_ENABLE_IDLE_TIMEOUT || CLI_ENABLE_PERIODIC_CALLBACK) && !CLI_ENABLE_TIME_SOURCE
#error "CLI_ENABLE_IDLE_TIMEOUT and CLI_ENABLE_PERIODIC_CALLBACK need a clock: leave CLI_ENABLE_TIME_SOURCE at its default (derived from them) or disable both features"
#endif

/*=======================================================================================
 * Private Defines
 *=======================================================================================*/
/* VT100 / ANSI Escape Parser States ----------------------------------------------------*/
#define ESC_STATE_NORMAL     (uint8_t)0 /**< No active escape sequence. */
#define ESC_STATE_ESC        (uint8_t)1 /**< Received ESC (0x1B); awaiting '[' or 'O'. */
#define ESC_STATE_CSI        (uint8_t)2 /**< ESC '[': accumulating the first numeric parameter. */
#define ESC_STATE_CSI_REST   (uint8_t)3 /**< CSI after the first parameter (e.g. ";5" modifier). */
#define ESC_STATE_CSI_IGNORE (uint8_t)4 /**< CSI with private / intermediate bytes; dropped. */
#define ESC_STATE_SS3        (uint8_t)5 /**< ESC 'O' (SS3): the next byte is the key code. */

#define ESC_PARAM_MAX 9999 /**< Saturation limit for the accumulated CSI parameter. */

/* Misc Limits --------------------------------------------------------------------------*/
#define CLI_AUTH_MAX_ATTEMPTS 3   /**< Failed login / enable attempts before the failure mode. */
#define CLI_HELP_NAME_COL     22U /**< Column at which help text starts in command listings. */
#define CLI_HELP_LINE_SIZE    (CLI_HELP_NAME_COL + CLI_MAX_NAME_LEN + CLI_MAX_HELP_LEN)

/* ASCII Control Codes ------------------------------------------------------------------*/
#define ASCII_NUL       0x00        /**< Null character. */
#define ASCII_BELL      0x07        /**< BEL — terminal bell / alert. */
#define ASCII_BS        0x08        /**< Backspace. */
#define ASCII_TAB       0x09        /**< Horizontal tab. */
#define ASCII_LF        0x0A        /**< Line feed. */
#define ASCII_CR        0x0D        /**< Carriage return. */
#define ASCII_ESC       0x1B        /**< Escape. */
#define ASCII_DEL       0x7F        /**< Delete / rubout. */
#define ASCII_CTRL(c)   ((c) - '@') /**< Compute ASCII value of a control character. */
#define ASCII_QUESTION  0x3F        /**< '?' question mark. */
#define ASCII_PIPE      0x7C        /**< '|' vertical bar / pipe. */
#define ASCII_LSBRACKET 0x5B        /**< '[' left square bracket. */
#define ASCII_CAP_O     0x4F        /**< 'O' capital letter. */
#define ASCII_DIGIT_0   0x30        /**< '0' digit zero. */
#define ASCII_DIGIT_9   0x39        /**< '9' digit nine. */

#define CLI_CRLF     "\r\n"
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
 *  CLI_TEXT_PROGMEM             — whether caller registration strings are in flash
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
#define CMD_STR(s)          PSTR(s)
#define CLI_WSTR(cli, s)    cli_write_str_P((cli), PSTR(s))
#define CLI_OUT_STR(cli, s) cli_print_str_P((cli), PSTR(s))
#define CLI_TEXT_PROGMEM    True /**< Registration strings passed by callers are in flash. */
#else
/* Non-AVR (STM32, ESP32, RP2040, …) or non-Arduino: plain RAM strings */
#define CMD_STR(s)          (s)
#define CLI_WSTR(cli, s)    cli_write_str((cli), (s))
#define CLI_OUT_STR(cli, s) cli_print_str((cli), (s))
#define CLI_TEXT_PROGMEM    False /**< Registration strings passed by callers are in RAM. */
#endif

/**
 * @brief Print a plain string literal followed by a newline as command output,
 *        avoiding vsnprintf overhead and (on AVR) keeping the string in flash.
 *
 * The text goes through the same output path as @c cli_println(), so it is
 * subject to the active pipe filter.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] s    String literal to write.
 */
#define CLI_PRINTLN_STR(cli, s)                                                                    \
  do {                                                                                             \
    CLI_OUT_STR((cli), (s));                                                                       \
    cli_print_process_stream((cli), "\n", 1);                                                      \
  } while (0)

#define CLI_CMD_IS_IN_USE(cmd_)  ((((cmd_)->flags) & CLI_CMD_FLAG_IN_USE) != 0U)
#define CLI_CMD_IS_HIDDEN(cmd_)  ((((cmd_)->flags) & CLI_CMD_FLAG_HIDDEN) != 0U)
#define CLI_CMD_SET_IN_USE(cmd_) ((cmd_)->flags = (uint8_t)((cmd_)->flags | CLI_CMD_FLAG_IN_USE))
#define CLI_CMD_SET_HIDDEN(cmd_) ((cmd_)->flags = (uint8_t)((cmd_)->flags | CLI_CMD_FLAG_HIDDEN))

/*=======================================================================================
 * Private Types
 *=======================================================================================*/
typedef enum {
  CLI_HISTORY_NAV_PREV = -1,
  CLI_HISTORY_NAV_NEXT = 1,
} cli_history_nav_dir_enum_t;

/** @brief Result of splitting an input line into tokens. */
typedef enum {
  CLI_TOKENIZE_OK = 0,      /**< All tokens stored. */
  CLI_TOKENIZE_TOO_MANY,    /**< More than @c max_args - 1 tokens; extra tokens not stored. */
  CLI_TOKENIZE_UNTERMINATED /**< A double-quoted string is not closed. */
} cli_tokenize_status_enum_t;

/** @brief State shared with @c cli_print_help_cb while listing commands. */
typedef struct {
  bool any;       /**< Set once at least one command was listed. */
  bool as_output; /**< true: command output (pipe filter applies); false: editor UI. */
} cli_help_ctx_struct_t;

#if CLI_ENABLE_TAB_COMPLETION
/** @brief State shared with @c cli_complete_cb while collecting Tab-completion candidates. */
typedef struct {
  cli_len_t count;        /**< Number of matching keywords. */
  const char *first_text; /**< First matching keyword. */
  cli_len_t common;       /**< Length of the prefix shared by all matching keywords. */
} cli_complete_ctx_struct_t;
#endif

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
static void cli_write_text(cli_struct_t *cli, const char *data, int len);
static void cli_flush(cli_struct_t *cli);

/* Session Timing Helpers ---------------------------------------------------------------*/

static void cli_touch_activity_internal(cli_struct_t *cli);
static void cli_idle_period_restart(cli_struct_t *cli);
#if CLI_ENABLE_TIME_SOURCE &&                                                                      \
    (CLI_ENABLE_IDLE_TIMEOUT || CLI_ENABLE_PERIODIC_CALLBACK || CLI_ENABLE_AUTH)
static uint32_t cli_rebase_ts(uint32_t ts, bool had_clock, uint32_t old_now, uint32_t new_now);
#endif
#if CLI_ENABLE_TIME_SOURCE && ENV_IS_OS_ENVIRONMENT
static uint32_t cli_default_time_source(void *ctx);
#endif
#if CLI_ENABLE_IDLE_TIMEOUT
static int8_t cli_handle_idle_timeout(cli_struct_t *cli);
#endif

/* Output Line Processing ---------------------------------------------------------------*/

#if CLI_HAS_LINE_BUFFER
static void cli_print_emit(cli_struct_t *cli, const char *line);
static void cli_line_buffer_emit(cli_struct_t *cli);
#endif
static void cli_print_process_stream(cli_struct_t *cli, const char *data, int len);
static void cli_output_end_line(cli_struct_t *cli);
#if !defined(__AVR__) && !defined(ARDUINO_ARCH_AVR)
static void cli_print_str(cli_struct_t *cli, const char *s);
#endif

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
static void cli_redisplay_input(cli_struct_t *cli);
#if CLI_ENABLE_AUTH
static void cli_input_prompt_changed(cli_struct_t *cli);
#endif

/* AVR PROGMEM Helpers ------------------------------------------------------------------*/

#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
static void cli_write_str_P(cli_struct_t *cli, const char *s_P);
static void cli_print_str_P(cli_struct_t *cli, const char *s_P);
static size_t cli_strlcpy_P(char *dst, const char *src_P, size_t size);
#endif

/* Internal String Helpers ---------------------------------------------------------------*/

static size_t cli_strlcpy(char *dst, const char *src, size_t size);
static size_t cli_text_len(const char *src, bool progmem);
static void cli_text_copy(char *dst, const char *src, size_t size, bool progmem);
static bool cli_case_prefix_match(const char *str, const char *prefix);
static bool cli_case_equal(const char *a, const char *b);
#if CLI_ENABLE_AUTH
static bool cli_secret_equal(const char *a, const char *b, size_t max_len);
static void cli_wipe_input(cli_struct_t *cli);
#endif
static bool cli_keyword_valid(const char *s);
static bool cli_quote_open(const char *s, cli_len_t len);
static inline char *cli_skip_spaces(char *p);
static inline char *cli_skip_non_spaces(char *p);

/* Navigation (mode / submenu context) --------------------------------------------------*/

static bool cli_handle_valid(const cli_struct_t *cli, cli_cmd_handle_t h);
static void cli_notify_mode_change(cli_struct_t *cli);
static int8_t cli_nav_push(cli_struct_t *cli);
static void cli_nav_return(cli_struct_t *cli, uint8_t level);
static void cli_nav_reset(cli_struct_t *cli);
static void cli_nav_relocate(cli_struct_t *cli, cli_cmd_handle_t removed,
                             cli_cmd_handle_t replacement);
static bool cli_is_within(const cli_struct_t *cli, cli_cmd_handle_t node,
                          cli_cmd_handle_t ancestor);
#if CLI_ENABLE_MODE_NAMES
static const char *cli_mode_name(const cli_struct_t *cli, cli_mode_t mode);
#endif

/* Prompt Construction and Display ------------------------------------------------------*/

static void cli_show_prompt(cli_struct_t *cli);

/* History ------------------------------------------------------------------------------*/

#if CLI_ENABLE_HISTORY
static void cli_history_add(cli_struct_t *cli, const char *line);
static bool cli_history_overlaps(const cli_struct_t *cli, cli_history_idx_t off,
                                 cli_history_idx_t len);
static cli_history_idx_t cli_history_index(const cli_struct_t *cli, cli_history_idx_t offset);
static void cli_history_clear_displayed_input(cli_struct_t *cli);
static void cli_history_navigate(cli_struct_t *cli, cli_history_nav_dir_enum_t dir);
#endif
static void cli_history_capture_edited_preview(cli_struct_t *cli);

/* Tokeniser ----------------------------------------------------------------------------*/

static cli_tokenize_status_enum_t cli_tokenize(char *buf, const char *argv[], cli_argc_t max_args,
                                               cli_argc_t *argc_out);

/* Command Tree Traversal ----------------------------------------------------------------*/

/** @brief Callback invoked by @c cli_foreach_match for each match; @p text is the matched
 *         keyword (name or alias). Returns @c false to stop the iteration. */
typedef bool (*cli_match_cb_t)(cli_struct_t *cli, cli_cmd_handle_t h, const char *text, void *ctx);

static bool cli_print_help_cb(cli_struct_t *cli, cli_cmd_handle_t h, const char *text, void *ctx);

static bool cli_cmd_selectable(const cli_struct_t *cli, const cli_cmd_struct_t *cmd,
                               cli_cmd_handle_t parent);
static const char *cli_cmd_prefix_text(const cli_cmd_struct_t *cmd, const char *word);
static bool cli_cmd_exact(const cli_cmd_struct_t *cmd, const char *word);
static bool cli_name_taken(const cli_struct_t *cli, cli_cmd_handle_t parent, cli_mode_t mode,
                           const char *name, cli_cmd_handle_t ignore);
static cli_cmd_handle_t cli_add_command_impl(cli_struct_t *cli, cli_cmd_handle_t parent,
                                             const char *name, bool name_progmem,
                                             cli_cmd_handler_fn callback, cli_priv_t privilege,
                                             cli_mode_t mode, const char *help,
                                             bool help_progmem);
static void cli_foreach_match(cli_struct_t *cli, const char *prefix, cli_cmd_handle_t parent,
                              bool with_root, cli_match_cb_t cb, void *ctx);
static int8_t cli_resolve_one(cli_struct_t *cli, const char *word, cli_cmd_handle_t parent,
                              cli_cmd_handle_t *out);
static bool cli_has_any_match(cli_struct_t *cli, const char *word, cli_cmd_handle_t parent);
static bool cli_cmd_has_children(const cli_struct_t *cli, cli_cmd_handle_t parent);
#if CLI_ENABLE_ALIASES
static bool cli_cmd_has_registered_children(const cli_struct_t *cli, cli_cmd_handle_t parent);
#endif
static cli_cmd_handle_t cli_first_word_parent(cli_struct_t *cli, const char *word);
static int8_t cli_walk_words(cli_struct_t *cli, const char *argv[], int count,
                             cli_cmd_handle_t *parent, bool *args);
static cli_cmd_handle_t cli_context_current(const cli_struct_t *cli);
static int8_t cli_context_enter(cli_struct_t *cli, cli_cmd_handle_t node, uint8_t depth_before);
static cli_cmd_handle_t cli_menu_context_parent(const cli_struct_t *cli);

/* Tab Completion -----------------------------------------------------------------------*/

#if CLI_ENABLE_TAB_COMPLETION
static void cli_tab_complete(cli_struct_t *cli);
static void cli_tab_complete_words(cli_struct_t *cli, char *scratch);
static bool cli_complete_cb(cli_struct_t *cli, cli_cmd_handle_t h, const char *text, void *raw);
#endif

/* Inline '?' Help ----------------------------------------------------------------------*/

static void cli_show_help(cli_struct_t *cli);
static void cli_show_help_words(cli_struct_t *cli, char *scratch);

/* Output Filter (Pipe Processing) ------------------------------------------------------*/

#if CLI_ENABLE_PIPE_FILTER
static void cli_filter_reset(cli_struct_t *cli);
static bool cli_filter_line(cli_struct_t *cli, const char *line);
static void cli_filter_flush(cli_struct_t *cli);
static char *cli_find_pipe(char *line);
static int8_t cli_parse_filter(cli_struct_t *cli, char *line);
static cli_filter_enum_t cli_filter_lookup(const char *kw, bool *ambiguous);
static bool cli_filter_split(char *tail, char **kw, char **pattern);
static void cli_filter_help_line(cli_struct_t *cli, const char *name, cli_filter_enum_t type);
static void cli_filter_show_help(cli_struct_t *cli, char *tail);
#if CLI_ENABLE_TAB_COMPLETION
static void cli_filter_tab_complete(cli_struct_t *cli, char *tail);
#endif
#endif

/* Command Execution --------------------------------------------------------------------*/
static int8_t cli_feed(cli_struct_t *cli, int byte);
static void cli_after_command(cli_struct_t *cli, int8_t exec_rc, int8_t *rc);

static bool cli_eol_pair(cli_struct_t *cli, int byte);

static int8_t cli_execute_command(cli_struct_t *cli, const char *argv[], cli_argc_t argc,
                                  cli_argc_t depth, cli_cmd_handle_t parent);
static int8_t cli_execute(cli_struct_t *cli, char *line, bool interactive);
static bool cli_command_begin(cli_struct_t *cli);
static void cli_command_end(cli_struct_t *cli, bool outermost);
static int8_t cli_quit_key(cli_struct_t *cli);
static int8_t cli_exec_area(cli_struct_t *cli, char **area, size_t *size);
static int8_t cli_exec_area_run(cli_struct_t *cli, char *line, size_t len);
#if CLI_ENABLE_HISTORY && CLI_HISTORY_INDEX_SELECTION
static bool cli_history_replay_load(cli_struct_t *cli);
#endif

/* Authentication -----------------------------------------------------------------------*/

static bool cli_state_is_auth_input(const cli_struct_t *cli);
#if CLI_ENABLE_AUTH
static void cli_auth_reset_work(cli_struct_t *cli);
static void cli_auth_prompt_username(cli_struct_t *cli);
static bool cli_auth_lockout_expired(const cli_struct_t *cli);
static bool cli_auth_lockout_unending(const cli_struct_t *cli);
static void cli_auth_lockout_end(cli_struct_t *cli);
static int8_t cli_auth_limit_reached(cli_struct_t *cli, cli_auth_state_enum_t resume_state);
static int8_t cli_auth_verify(cli_struct_t *cli);
static bool cli_auth_secret_input(const cli_struct_t *cli);
static cli_len_t cli_auth_input_limit(const cli_struct_t *cli);
static void cli_auth_backspace(cli_struct_t *cli);
static void cli_auth_clear_input(cli_struct_t *cli);
static void cli_auth_insert_char(cli_struct_t *cli, char c);
static int8_t cli_auth_submit_line(cli_struct_t *cli);
static int8_t cli_auth_run_callback(cli_struct_t *cli, cli_auth_fn fn);
static void cli_auth_enable_result(cli_struct_t *cli, int8_t ok, int8_t *rc);
static int8_t cli_auth_handle(cli_struct_t *cli, int byte);
#endif
static void cli_reset_input(cli_struct_t *cli);
static void cli_session_reset(cli_struct_t *cli);
static int8_t cli_submit_line(cli_struct_t *cli);

/* Escape-Sequence State Machine --------------------------------------------------------*/
static bool cli_process_escape(cli_struct_t *cli, int b, bool dispatch);
static void handle_csi(cli_struct_t *cli, char final);

/*=======================================================================================
 * Public Functions
 *=======================================================================================*/

/* Built-in Command Handlers ------------------------------------------------------------*/
OPENLIBCLI_API int8_t cli_add_builtin_cmds(cli_struct_t *cli) {
  cli_cmd_handle_t h_usage;
  cli_cmd_handle_t h_clear;
  bool ok = (cli != NULL);

  /* Every registration is attempted; any failure (pool full, a name that does not fit in
   * CLI_MAX_NAME_LEN) is reported, so a missing built-in does not go unnoticed. */
  ok = (cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("exit"), cli_cmd_builtin_exit, CLI_PRIV_USER,
                        CLI_MODE_ANY, CLI_STR("Exit session")) != CLI_CMD_INVALID) &&
       ok;

  ok = (cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("quit"), cli_cmd_builtin_quit, CLI_PRIV_USER,
                        CLI_MODE_ANY, CLI_STR("Quit session")) != CLI_CMD_INVALID) &&
       ok;

  ok = (cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("enable"), cli_cmd_builtin_enable,
                        CLI_PRIV_USER, CLI_MODE_EXEC,
                        CLI_STR("Activate Privileged mode")) != CLI_CMD_INVALID) &&
       ok;

  ok = (cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("disable"), cli_cmd_builtin_disable,
                        CLI_PRIV_PRIVILEGED, CLI_MODE_ENABLE,
                        CLI_STR("Leave Privileged mode")) != CLI_CMD_INVALID) &&
       ok;

  ok = (cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("end"), cli_cmd_builtin_end,
                        CLI_PRIV_PRIVILEGED, CLI_MODE_CONFIG,
                        CLI_STR("Return to exec")) != CLI_CMD_INVALID) &&
       ok;

  ok = (cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("configure"), cli_cmd_builtin_configure,
                        CLI_PRIV_PRIVILEGED, CLI_MODE_ENABLE,
                        CLI_STR("Config mode")) != CLI_CMD_INVALID) &&
       ok;

  ok = (cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("help"), cli_cmd_builtin_help, CLI_PRIV_USER,
                        CLI_MODE_ANY, CLI_STR("Show commands")) != CLI_CMD_INVALID) &&
       ok;

#if CLI_ENABLE_HISTORY
  ok = (cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("history"), cli_cmd_builtin_history,
                        CLI_PRIV_USER, CLI_MODE_ANY,
                        CLI_STR("Command history")) != CLI_CMD_INVALID) &&
       ok;
#endif

  if (cli != NULL && cli->ansi_supported) {
    h_clear = cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("clear"), cli_cmd_builtin_clear_screen,
                              CLI_PRIV_USER, CLI_MODE_ANY, CLI_STR("Clear screen"));
    ok = (h_clear != CLI_CMD_INVALID) && ok;
#if CLI_ENABLE_ALIASES
    ok = (cli_cmd_add_alias(cli, h_clear, CLI_STR("cls")) == CLI_OK) && ok;
#endif
  }

  h_usage = cli_add_command(cli, CLI_CMD_ROOT, CLI_STR("usage"), cli_cmd_builtin_usage,
                            CLI_PRIV_USER, CLI_MODE_ANY, CLI_STR("Usage information"));
  ok = (h_usage != CLI_CMD_INVALID) && ok;

  ok = (cli_add_command(cli, h_usage, CLI_STR("commands"), cli_cmd_builtin_usage_commands,
                        CLI_PRIV_USER, CLI_MODE_ANY, CLI_STR("Usage Commands")) != CLI_CMD_INVALID) &&
       ok;
  ok = (cli_add_command(cli, h_usage, CLI_STR("keys"), cli_cmd_builtin_usage_keys, CLI_PRIV_USER,
                        CLI_MODE_ANY, CLI_STR("Usage Keys")) != CLI_CMD_INVALID) &&
       ok;

  return ok ? CLI_OK : CLI_ERR;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_exit(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                           const char *argv[]) {
  int8_t rc = CLI_ERR; /* no session to act on */

  (void)cmd;
  (void)argc;
  (void)argv;

  if (cli != NULL) {
    rc = CLI_OK;
    if (cli->on_exit_cmd != NULL) {
      int8_t hook_rc = cli->on_exit_cmd(cli);
      if (hook_rc != CLI_OK) {
        rc = hook_rc;
      }
    }

    if (rc != CLI_OK) {
      /* Hook result takes precedence. */
    } else if (cli->nav_depth > 0U) {
      /* Leave one navigation level (submenu and/or mode, and the privilege it granted). */
      cli_nav_return(cli, (uint8_t)(cli->nav_depth - 1U));
    } else {
      /* Login floor: nothing left to leave. */
      switch (cli->exit_root_policy) {
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
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_quit(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                           const char *argv[]) {
  int8_t rc = CLI_ERR; /* no session to act on */

  (void)cmd;
  (void)argc;
  (void)argv;

  if (cli != NULL) {
    rc = CLI_OK;
    if (cli->on_quit_cmd != NULL) {
      int8_t hook_rc = cli->on_quit_cmd(cli);
      if (hook_rc != CLI_OK) {
        rc = hook_rc;
      }
    }

    if (rc != CLI_OK) {
      /* Hook result takes precedence. */
    } else {
      switch (cli->quit_policy) {
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
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_enable(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                             const char *argv[]) {
  int8_t rc = CLI_ERR; /* no session to act on */

  (void)cmd;
  (void)argc;
  (void)argv;

  if (cli != NULL) {
    /* Already privileged or no secret/callback configured: switch mode immediately. The
     * mode is entered before privilege is raised so the saved navigation level is the
     * unprivileged one that a later 'exit' returns to. */
#if CLI_ENABLE_AUTH
    bool needs_secret =
        (cli->enable_privilege_secret[0] != '\0' || cli->enable_privilege_cb != NULL);
#else
    bool needs_secret = False;
#endif

    rc = CLI_OK;
    if (cli_state_is_auth_input(cli)) {
      /* Run programmatically while credentials are being asked for (login, a pending enable
       * password, a lockout): the user is not at the command prompt, nothing to enable. */
      rc = CLI_ERR;
    } else if (cli->privilege >= CLI_PRIV_PRIVILEGED || !needs_secret) {
      rc = cli_set_mode(cli, CLI_MODE_ENABLE);
      if (rc == CLI_OK) {
        if (cli->privilege < CLI_PRIV_PRIVILEGED) {
          cli_set_privilege(cli, CLI_PRIV_PRIVILEGED); /* raised to it, never lowered */
        }
      } else {
        CLI_WSTR(cli, "% Navigation depth limit reached.\r\n");
      }
    }
#if CLI_ENABLE_AUTH
    /* Secret required — the session engine routes the next line to the password handler */
    else {
      cli->auth_resume_state = cli->auth_state;
      cli_reset_input(cli);
      cli->auth_state = CLI_AUTH_ENABLE_MODE_PASSWORD;
      cli_input_prompt_changed(cli);
    }
#endif
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_disable(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                              const char *argv[]) {
  int8_t rc = CLI_ERR; /* no session to act on */

  (void)cmd;
  (void)argc;
  (void)argv;

  if (cli != NULL) {
    /* User exec becomes the new floor: a following 'exit' ends the session. */
    cli_nav_reset(cli);
    cli_set_privilege(cli, CLI_PRIV_USER);
    rc = CLI_OK;
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_end(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                          const char *argv[]) {
  int8_t rc = CLI_ERR; /* no session to act on */

  (void)cmd;
  (void)argc;
  (void)argv;

  if (cli != NULL) {
    /* Return to privileged exec from any config mode / submenu entered from it */
    rc = cli_set_mode(cli, CLI_MODE_ENABLE);
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_configure(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                                const char *argv[]) {
  int8_t rc = CLI_ERR; /* no session to act on */

  (void)cmd;
  (void)argc;
  (void)argv;

  if (cli != NULL) {
    rc = cli_set_mode(cli, CLI_MODE_CONFIG);
    if (rc != CLI_OK) {
      CLI_WSTR(cli, "% Navigation depth limit reached.\r\n");
    }
  }

  return rc;
}

#if CLI_ENABLE_HISTORY
OPENLIBCLI_API int8_t cli_cmd_builtin_history(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                              const char *argv[]) {
  int8_t rc = CLI_ERR; /* no session to act on */

  (void)cmd;
#if !CLI_HISTORY_INDEX_SELECTION
  (void)argv;
  (void)argc;
#endif

  if (cli == NULL) {
    /* Nothing to list. */
  } else if (cli->history_count == 0) {
    CLI_PRINTLN_STR(cli, "History is empty.");
    rc = CLI_OK;
  } else {
    rc = CLI_OK;
#if CLI_HISTORY_INDEX_SELECTION
    uint32_t n_val = 0U;

    if (argc > 1) {
      if (argc > 2 || !parse_u32(argv[1], &n_val) || n_val == 0U ||
          n_val > (uint32_t)cli->history_count) {
        CLI_WSTR(cli, "% Invalid history index.\r\n");
        rc = CLI_ERR;
      } else {
        /* Only record the request: when this command was typed (or run by cli_exec() from
         * outside any command), the entry is loaded into the free line editor once it has
         * finished; a 'history N' run from inside another command is discarded. */
        cli->history_replay = cli_history_index(
            cli, (cli_history_idx_t)((uint32_t)cli->history_count - n_val + 1U));
        rc = CLI_EXEC_REPLAY_INPUT;
      }
    } else
#endif
    {
      for (cli_history_idx_t i = cli->history_count; i > 0; i--) {
        cli_history_idx_t idx = cli_history_index(cli, i);
        char line[16 + CLI_MAX_INPUT_LEN];
        int len = cli_snprintf(line, sizeof(line), "  %3d  %s", cli->history_count - i + 1,
                               &cli->history_buf[cli->history_off[idx]]);
        /* Sent as text, not through cli_println(): its format buffer (CLI_MAX_OUTPUT_BUF) may
         * be shorter than an entry. */
        if (len > 0) {
          int shown = (len < (int)sizeof(line)) ? len : (int)sizeof(line) - 1;
          cli_print_process_stream(cli, line, shown);
          cli_print_process_stream(cli, "\n", 1);
        }
      }
    }
  }

  return rc;
}
#endif /* CLI_ENABLE_HISTORY */

OPENLIBCLI_API int8_t cli_cmd_builtin_help(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                           const char *argv[]) {
  int8_t rc = CLI_ERR; /* no session to act on */

  (void)cmd;

  if (cli != NULL) {
    cli_cmd_handle_t parent = cli_menu_context_parent(cli);
    /* Without arguments inside a submenu, the root commands that still run there are listed
     * after the submenu's own commands. */
    bool with_root = (argc <= 1 && parent != CLI_CMD_ROOT);

    rc = CLI_OK;
    if (argc > 1) {
      cli_cmd_handle_t h;
      for (cli_argc_t i = 1; i < argc && rc == CLI_OK; i++) {
        if (i == 1) {
          parent = cli_first_word_parent(cli, argv[1]); /* same lookup as execution */
        }
        int8_t r = cli_resolve_one(cli, argv[i], parent, &h);
        if (r == CLI_OK) {
          parent = h;
        } else if (r == CLI_ERR) {
          CLI_WSTR(cli, "% Unknown command.\r\n");
          rc = CLI_ERR;
        } else {
          CLI_WSTR(cli, "% Ambiguous command.\r\n");
          rc = CLI_ERR_AMBIG;
        }
      }
    }

    if (rc == CLI_OK) {
      cli_help_ctx_struct_t ctx = {False, True};
      cli_foreach_match(cli, "", parent, with_root, cli_print_help_cb, &ctx);
      if (!ctx.any) {
        CLI_PRINTLN_STR(cli, "  <cr>");
      }
    }
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_clear_screen(cli_struct_t *cli, const char *cmd,
                                                   cli_argc_t argc, const char *argv[]) {
  int8_t rc = CLI_ERR; /* no session to act on */

  (void)cmd;
  (void)argc;
  (void)argv;

  if (cli != NULL) {
    /* ANSI clear screen + cursor home */
    if (cli->ansi_supported) {
      CLI_WSTR(cli, "\x1b"
                    "[2J\x1b"
                    "[H");
    } else {
      CLI_PRINTLN_STR(cli, "Clear not supported on Non-ANSI/VT100 Terminals");
    }
    rc = CLI_OK;
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_usage(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                            const char *argv[]) {
  int8_t rc = CLI_ERR; /* no session to act on */

  (void)cmd;
  (void)argc;
  (void)argv;

  if (cli != NULL) {
    CLI_PRINTLN_STR(cli, "Usage information. Type 'commands' or 'keys' for details.");
    rc = CLI_OK;
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_usage_commands(cli_struct_t *cli, const char *cmd,
                                                     cli_argc_t argc, const char *argv[]) {
  int8_t rc = CLI_ERR; /* no session to act on */
  cli_cmd_handle_t h_clear;

  (void)cmd;
  (void)argc;
  (void)argv;

  if (cli != NULL) {
    CLI_PRINTLN_STR(cli, "Common usage:");
    CLI_PRINTLN_STR(cli, "  ?                 List commands available here");
    CLI_PRINTLN_STR(cli, "  <cmd> ?           List what can follow <cmd>");
    CLI_PRINTLN_STR(cli, "  <prefix>?         List commands starting with <prefix>");
    CLI_PRINTLN_STR(cli, "  help [<cmd> ...]  List commands (or the subcommands of <cmd>)");
#if CLI_ENABLE_HISTORY
    CLI_PRINTLN_STR(cli, "  history           Command history");
#endif
    /* Only advertise 'clear' when it is actually registered (it needs an ANSI terminal). */
    if (cli_resolve_one(cli, "clear", CLI_CMD_ROOT, &h_clear) == CLI_OK &&
        cli_cmd_exact(&cli->cmd_pool[h_clear], "clear")) {
      if (cli_cmd_exact(&cli->cmd_pool[h_clear], "cls")) {
        CLI_PRINTLN_STR(cli, "  clear / cls       Clear screen");
      } else {
        CLI_PRINTLN_STR(cli, "  clear             Clear screen");
      }
    }
    CLI_PRINTLN_STR(cli, "  usage keys        Key shortcuts");
    rc = CLI_OK;
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_builtin_usage_keys(cli_struct_t *cli, const char *cmd,
                                                 cli_argc_t argc, const char *argv[]) {
  int8_t rc = CLI_ERR; /* no session to act on */

  (void)cmd;
  (void)argc;
  (void)argv;

  if (cli != NULL) {
    CLI_PRINTLN_STR(cli, "Line editing shortcuts:");
    CLI_PRINTLN_STR(cli, "  Tab      autocomplete");
    CLI_PRINTLN_STR(cli, "  Ctrl-P/N history prev/next");
    CLI_PRINTLN_STR(cli, "  Ctrl-A/E line start/end");
    CLI_PRINTLN_STR(cli, "  Ctrl-B/F cursor left/right");
    CLI_PRINTLN_STR(cli, "  Ctrl-U   clear line");
    CLI_PRINTLN_STR(cli, "  Ctrl-W   delete prev word");
    CLI_PRINTLN_STR(cli, "  Ctrl-K   delete to EOL");
    rc = CLI_OK;
  }

  return rc;
}

/* Session State Machine ----------------------------------------------------------------*/

OPENLIBCLI_API void cli_start(cli_struct_t *cli) {
  if (cli != NULL) {
    /* A new session on (possibly) a new connection: nothing of a previous run carries over,
     * including a half-received line ending and a write failure of the old peer. */
    cli_session_reset(cli);
    cli->eol_prev = 0U;
    cli->tx_error = False;
#if CLI_ENABLE_AUTH
    cli->auth_requested = False; /* a login requested for the previous connection */
#endif
    cli_idle_period_restart(cli);
#if CLI_ENABLE_PERIODIC_CALLBACK && CLI_ENABLE_TIME_SOURCE
    if (cli->time_source != NULL) {
      cli->last_periodic_ts = cli->time_source(cli->time_source_ctx);
    }
#endif
    cli->session_state = CLI_SESSION_INIT;

    cli_flush(cli);
  }
}

OPENLIBCLI_API int8_t cli_session_engine(cli_struct_t *cli) {
  int8_t result = CLI_ERR; /* no session to act on */

  if (cli == NULL) {
    /* Nothing to serve. */
  } else if (cli->transport.available == NULL || cli->transport.read == NULL ||
             cli->transport.write == NULL) {
    /* Incomplete transport vtable (e.g. a failed transport init): nothing can be served. */
    cli->session_state = CLI_SESSION_STOP;
  } else {
#if CLI_ENABLE_TIMMING_STATS
    /* The tick is timed with the clock it starts with: a clock installed, replaced or removed
     * during the tick (by a command or callback) has no start reading to subtract from. */
    cli_micros_fn tick_clock = cli->micros_source;
    void *tick_clock_ctx = cli->micros_source_ctx;
    uint32_t t0 = (tick_clock != NULL) ? tick_clock(tick_clock_ctx) : 0U;
#endif
    result = CLI_OK;
    bool active = True;

    if (cli->session_state == CLI_SESSION_STOP) {
      result = CLI_ERR_QUIT;
      active = False;
    } else if (cli->session_state == CLI_SESSION_INIT) {
      cli->session_state = CLI_SESSION_SHOW_BANNER;
    } else if (cli->session_state == CLI_SESSION_SHOW_BANNER) {
#if CLI_ENABLE_BANNER
      if (cli->banner[0] != '\0') {
        CLI_WSTR(cli, CLI_CRLF);
        cli_write_str(cli, cli->banner);
        CLI_WSTR(cli, CLI_CRLF);
      }
#endif

#if CLI_ENABLE_AUTH
      if (cli->require_auth || cli->auth_requested) {
        cli->auth_requested = False;
        cli->auth_attempts = 0;
        cli->auth_lockout_started_ts = 0UL;
        cli_auth_prompt_username(cli);
        cli->session_state = CLI_SESSION_RUN;
      } else {
        cli->auth_state = CLI_AUTH_NONE;
        cli->session_state = CLI_SESSION_RUN;
        cli_show_prompt(cli);
      }
#else
      cli->session_state = CLI_SESSION_RUN;
      cli_show_prompt(cli);
#endif
    } else if (cli->session_state == CLI_SESSION_RUN && active) {
#if CLI_ENABLE_PERIODIC_CALLBACK
      if (cli->periodic_cb != NULL && cli->periodic_interval > 0UL && cli->time_source != NULL) {
        uint32_t now = cli->time_source(cli->time_source_ctx);
        uint32_t elapsed = (now - cli->last_periodic_ts);
        if (elapsed >= cli->periodic_interval) {
          cli->last_periodic_ts = now;
          int8_t rr = cli->periodic_cb(cli);
          if (rr != CLI_OK) {
            cli->session_state = CLI_SESSION_STOP;
            result = rr;
            active = False;
          }
        }
      }
#endif

#if CLI_ENABLE_IDLE_TIMEOUT
      if (active) {
        int8_t timeout_rc = cli_check_idle_timeout(cli);
        if (timeout_rc != CLI_OK) {
          cli->session_state = CLI_SESSION_STOP;
          result = timeout_rc;
          active = False;
        }
      }
#endif

      if (active && cli->session_state != CLI_SESSION_RUN) {
        /* The idle policy or a callback restarted the session: pending input waits until the
         * banner and the login prompt / prompt have been shown. Or a callback stopped it. */
        active = False;
        if (cli->session_state == CLI_SESSION_STOP) {
          result = CLI_ERR_QUIT;
        }
      }

#if CLI_ENABLE_AUTH
      if (active && cli->auth_state == CLI_AUTH_LOCKOUT) {
        if (cli_auth_lockout_unending(cli)) {
          /* The clock was removed during a timed lockout, which can then never end: close the
           * session, as cli_auth_limit_reached() does when a lockout would start without one. */
          CLI_WSTR(cli, "% Too many failed attempts. Closing Session.\r\n");
          cli->session_state = CLI_SESSION_STOP;
          result = CLI_ERR_AUTH;
          active = False;
        } else if (cli_auth_lockout_expired(cli)) {
          cli_auth_lockout_end(cli);
        } else {
          /* Still locked out. */
        }
      }
#endif

      if (active) {
        /* At most CLI_MAX_RX_BYTES_PER_TICK bytes per call: the rest stays in the transport
         * for the next call, so a continuous input stream cannot keep this call running. */
        uint32_t budget = (uint32_t)CLI_MAX_RX_BYTES_PER_TICK;
        bool keep_going = True;

        while (keep_going && budget > 0U) {
          cli_transport_ret_t available_rc = cli->transport.available(cli->transport.ctx);
          cli_transport_ret_t b;
          int8_t rc;

          if (available_rc < 0) {
            cli->session_state = CLI_SESSION_STOP;
            result = CLI_ERR;
            active = False;
            keep_going = False;
          } else if (available_rc == 0) {
            keep_going = False;
          } else {
            budget--;
            b = cli->transport.read(cli->transport.ctx);
            if (b < 0) {
              cli->session_state = CLI_SESSION_STOP;
              result = CLI_ERR;
              active = False;
              keep_going = False;
            } else {
#if CLI_ENABLE_AUTH
              if (cli->auth_state == CLI_AUTH_LOCKOUT) {
                rc = CLI_OK; /* typed during the lockout: discarded, never replayed afterwards */
              } else if (cli_state_is_auth_input(cli)) {
                rc = cli_auth_handle(cli, b);
              } else
#endif
              {
                rc = cli_feed(cli, b);
              }
              if (rc != CLI_OK) {
                cli->session_state = CLI_SESSION_STOP;
                result = rc;
                active = False;
                keep_going = False;
              } else if (cli->tx_error) {
                /* The peer is gone: do not run further commands nobody will see. */
                keep_going = False;
              } else if (cli->session_state == CLI_SESSION_STOP) {
                /* A command stopped the session (e.g. cli_exec("quit") with its own result
                 * CLI_OK): report it now, like the callback path. */
                result = CLI_ERR_QUIT;
                active = False;
                keep_going = False;
              } else if (cli->session_state != CLI_SESSION_RUN) {
                /* Session restarted (quit/exit policy): leave the remaining input for the
                 * next tick, after the banner / login prompt has been shown. */
                keep_going = False;
              } else {
                /* Keep draining the transport (input during a lockout that just began is
                 * discarded by the same loop). */
              }
            }
          }
        }
      }
    } else {
      /* To silence MISRA */
    }

    if (cli->tx_error && cli->session_state != CLI_SESSION_STOP) {
      cli->session_state = CLI_SESSION_STOP;
      result = CLI_ERR;
    }

    /* Everything this tick wrote (echo, line edits, completions, the message before a close)
     * reaches the peer now, not only with the next prompt. */
    cli_flush(cli);

#if CLI_ENABLE_TIMMING_STATS
    if (tick_clock != NULL && cli->micros_source == tick_clock &&
        cli->micros_source_ctx == tick_clock_ctx) {
      uint32_t elapsed = tick_clock(tick_clock_ctx) - t0;
      cli_tick_stats_struct_t *s = &cli->tick_stats;
      /* Saturate rather than wrap: a wrapped count would divide by zero below. */
      if (s->count < UINT32_MAX) {
        s->count++;
      }
      if (s->count == 1U || elapsed < s->min_us) {
        s->min_us = elapsed;
      }
      if (elapsed > s->max_us) {
        s->max_us = elapsed;
      }
      int64_t avg_delta = ((int64_t)elapsed - (int64_t)s->avg_us) / (int64_t)s->count;
      s->avg_us = (uint32_t)((int64_t)s->avg_us + avg_delta);
    }
#endif /* CLI_ENABLE_TIMMING_STATS */
  }

  return result;
}

/* Tick Stats (compiled in only when CLI_ENABLE_TIMMING_STATS is set) -------------------*/

#if CLI_ENABLE_TIMMING_STATS

OPENLIBCLI_API void cli_tick_stats_get(const cli_struct_t *cli, cli_tick_stats_struct_t *out) {
  if (out != NULL) {
    if (cli != NULL) {
      *out = cli->tick_stats;
    } else {
      (void)memset(out, 0, sizeof(*out));
    }
  }
}

OPENLIBCLI_API void cli_tick_stats_reset(cli_struct_t *cli) {
  if (cli != NULL) {
    (void)memset(&cli->tick_stats, 0, sizeof(cli->tick_stats));
  }
}

OPENLIBCLI_API void cli_set_micros_source(cli_struct_t *cli, cli_micros_fn fn, void *ctx) {
  if (cli != NULL) {
    cli->micros_source = fn;
    cli->micros_source_ctx = ctx;
  }
}
#endif /* CLI_ENABLE_TIMMING_STATS */

/* Library / Session Lifecycle ----------------------------------------------------------*/

OPENLIBCLI_API void cli_lib_init(void) {
  /* No-op — built-in commands registered via cli_add_builtin_cmds(). */
}

OPENLIBCLI_API void cli_init(cli_struct_t *cli, const char *hostname,
                             const cli_transport_struct_t *transport,
                             const cli_platform_struct_t *platform, cli_cmd_struct_t *cmd_pool,
                             cli_poolsize_t cmd_pool_size) {
  if (cli != NULL && transport != NULL) {
    (void)memset(cli, 0, sizeof(*cli));
    if (cmd_pool == NULL || cmd_pool_size < 0) {
      cmd_pool_size = 0;
    }
    /* The session owns the pool: start from an empty command tree so slots left over
     * from a previous session using the same pool are not resurrected. */
    if (cmd_pool_size > 0) {
      (void)memset(cmd_pool, 0, (size_t)cmd_pool_size * sizeof(*cmd_pool));
    }
    cli->context = CLI_CMD_ROOT;
    cli->nav_depth = 0U;
    cli->cmd_pool = cmd_pool;
    cli->cmd_pool_size = cmd_pool_size;
    cli->cmd_pool_count = 0;
    cli->session_state = CLI_SESSION_STOP;
    cli->mode = CLI_MODE_EXEC;
    cli->privilege = CLI_PRIV_USER;
    cli->ansi_supported = True;
    cli->transport = *transport;
#if CLI_ENABLE_TIME_SOURCE
    if (platform && platform->now_sec) {
      cli->time_source = platform->now_sec;
      cli->time_source_ctx = platform->now_sec_ctx;
#if ENV_IS_OS_ENVIRONMENT
    } else {
      cli->time_source = cli_default_time_source;
      cli->time_source_ctx = NULL;
#else
    } else {
      cli->time_source = NULL;
      cli->time_source_ctx = NULL;
#endif
    }
#else
    (void)platform;
#endif
#if CLI_ENABLE_TIMMING_STATS
    if (platform && platform->micros) {
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

    /* "cli" stays when no hostname is given or it does not fit (see cli_set_hostname). */
    (void)cli_strlcpy(cli->hostname, "cli", CLI_MAX_PROMPT_LEN);
    if (hostname != NULL) {
      (void)cli_set_hostname(cli, hostname);
    }
#if CLI_ENABLE_HISTORY && CLI_HISTORY_INDEX_SELECTION
    cli->history_replay = (cli_history_idx_t)-1;
#endif

#if CLI_ENABLE_MODE_NAMES
    (void)cli_set_mode_name(cli, CLI_MODE_CONFIG, "config");
#endif
    cli_idle_period_restart(cli);
#if CLI_ENABLE_PERIODIC_CALLBACK && CLI_ENABLE_IDLE_TIMEOUT
    cli->last_periodic_ts = cli->last_action_ts;
#elif CLI_ENABLE_PERIODIC_CALLBACK && CLI_ENABLE_TIME_SOURCE
    if (cli->time_source != NULL) {
      cli->last_periodic_ts = cli->time_source(cli->time_source_ctx);
    }
#endif
  }
}

OPENLIBCLI_API void cli_done(cli_struct_t *cli) {
  if (cli != NULL) {
#if CLI_ENABLE_TIMMING_STATS
    if (cli->tick_stats.count > 0U) {
      (void)cli_println(cli, "\r\n--- Tick Timing Stats ---");
      /* unsigned long: the values are 32-bit, an unsigned int may have only 16 (AVR). */
      (void)cli_println(cli, "  Ticks   : %lu", (unsigned long)cli->tick_stats.count);
      (void)cli_println(cli, "  Min (us): %lu", (unsigned long)cli->tick_stats.min_us);
      (void)cli_println(cli, "  Avg (us): %lu", (unsigned long)cli->tick_stats.avg_us);
      (void)cli_println(cli, "  Max (us): %lu", (unsigned long)cli->tick_stats.max_us);
    }
#endif
    cli_flush(cli);
    (void)memset(cli, 0, sizeof(*cli));
    /* Zeroed state is CLI_SESSION_INIT: a released session must read as stopped. */
    cli->session_state = CLI_SESSION_STOP;
  }
}

OPENLIBCLI_API cli_session_state_enum_t cli_get_session_state(const cli_struct_t *cli) {
  return cli ? cli->session_state : CLI_SESSION_STOP;
}

OPENLIBCLI_API void cli_restart_session(cli_struct_t *cli) {
  if (cli != NULL) {
    if (cli->exec_depth == 0U && cli->session_state == CLI_SESSION_RUN) {
      /* Outside any command the prompt (with what was typed) is on screen: deliver what was
       * printed so far and start the new session below it, as cli_request_auth() does.
       * Inside a command, its output is finished when it returns (cli_command_end()). */
#if CLI_HAS_LINE_BUFFER
      if (cli->filter_line_buf_len > 0) {
        cli_line_buffer_emit(cli);
      }
#endif
      CLI_WSTR(cli, CLI_CRLF);
    }
    /* The line-ending state is kept: when 'quit' arrives with CR LF, the LF still belongs to
     * that line and must not submit an empty line to the restarted session. */
    cli_session_reset(cli);
    cli->session_state = CLI_SESSION_SHOW_BANNER;
  }
}

OPENLIBCLI_API void cli_request_auth(cli_struct_t *cli) {
  if (cli != NULL) {
#if CLI_ENABLE_AUTH
    if (cli->session_state == CLI_SESSION_INIT || cli->session_state == CLI_SESSION_SHOW_BANNER) {
      /* (Re)starting: the fresh session has no identity yet; it begins at the login prompt,
       * after the banner. */
      cli->auth_requested = True;
    } else {
      /* Nothing from the current identity survives into the next login. */
      cli_nav_reset(cli);
      cli_set_privilege(cli, CLI_PRIV_USER);
#if CLI_ENABLE_HISTORY
      (void)cli_clear_history(cli);
#endif
      cli->auth_attempts = 0;
      cli->enable_attempts = 0;
      cli->auth_lockout_started_ts = 0UL;
      cli_auth_reset_work(cli);
      cli->auth_state = CLI_AUTH_USERNAME;
      cli_input_prompt_changed(cli);
    }
#else
    (void)cli;
#endif
  }
}

/* Configuration Setters ----------------------------------------------------------------*/

OPENLIBCLI_API void cli_set_ansi_supported(cli_struct_t *cli, bool enabled) {
  if (cli != NULL) {
    cli->ansi_supported = enabled;
  }
}

OPENLIBCLI_API int8_t cli_set_hostname(cli_struct_t *cli, const char *hostname) {
  int8_t rc = CLI_ERR;
  const char *value = (hostname != NULL) ? hostname : "";

  /* A name that does not fit is refused, not cut: the prompt would show another name. */
  if (cli != NULL && strlen(value) < (size_t)CLI_MAX_PROMPT_LEN) {
    (void)cli_strlcpy(cli->hostname, value, CLI_MAX_PROMPT_LEN);
    rc = CLI_OK;
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_set_banner(cli_struct_t *cli, const char *banner) {
  int8_t rc = CLI_ERR;

#if CLI_ENABLE_BANNER
  const char *value = (banner != NULL) ? banner : "";

  /* A banner that does not fit is refused, not cut off in the middle of a line. */
  if (cli != NULL && strlen(value) < (size_t)CLI_MAX_BANNER_LEN) {
    (void)cli_strlcpy(cli->banner, value, CLI_MAX_BANNER_LEN);
    rc = CLI_OK;
  }
#else
  (void)cli;
  (void)banner;
#endif

  return rc;
}

OPENLIBCLI_API void cli_set_userdata(cli_struct_t *cli, void *data) {
  if (cli != NULL) {
    cli->user_data = data;
  }
}

OPENLIBCLI_API void *cli_get_userdata(const cli_struct_t *cli) {
  return cli ? cli->user_data : NULL;
}

cli_transport_kind_enum_t cli_get_transport_kind(const cli_struct_t *cli) {
  return cli ? cli->transport.kind : CLI_TRANSPORT_UNKNOWN;
}

OPENLIBCLI_API void cli_set_mode_change_cb(cli_struct_t *cli, cli_mode_fn fn) {
  if (cli != NULL) {
    cli->on_mode_change = fn;
  }
}

#if CLI_ENABLE_MODE_NAMES
OPENLIBCLI_API int8_t cli_set_mode_name(cli_struct_t *cli, cli_mode_t mode, const char *name) {
  int8_t rc = CLI_ERR;

  if (cli != NULL && mode != CLI_MODE_ANY) {
    int slot = -1;
    int free_slot = -1;

    for (int i = 0; i < (int)CLI_MAX_MODES; i++) {
      if (cli->mode_name_ids[i] == mode) {
        slot = i;
      } else if (free_slot < 0 && cli->mode_name_ids[i] == CLI_MODE_ANY) {
        free_slot = i;
      } else {
        /* Slot owned by another mode. */
      }
    }

    if (name == NULL || name[0] == '\0') {
      /* Empty name: forget the mode's entry. */
      if (slot >= 0) {
        cli->mode_name_ids[slot] = CLI_MODE_ANY;
        cli->mode_names[slot][0] = '\0';
      }
      rc = CLI_OK;
    } else {
      if (slot < 0) {
        slot = free_slot;
      }
      if (strlen(name) >= (size_t)CLI_MAX_MODE_NAME_LEN) {
        rc = CLI_ERR; /* a cut-down name would show a different mode in the prompt */
      } else if (slot < 0) {
        rc = CLI_ERR_NOMEM;
      } else {
        cli->mode_name_ids[slot] = mode;
        (void)cli_strlcpy(cli->mode_names[slot], name, CLI_MAX_MODE_NAME_LEN);
        rc = CLI_OK;
      }
    }
  }

  return rc;
}
#else
OPENLIBCLI_API int8_t cli_set_mode_name(cli_struct_t *cli, cli_mode_t mode, const char *name) {
  (void)cli;
  (void)mode;
  (void)name;
  return CLI_ERR;
}
#endif

OPENLIBCLI_API void cli_set_time_source(cli_struct_t *cli, cli_time_fn fn, void *ctx) {
#if CLI_ENABLE_TIME_SOURCE
  if (cli != NULL) {
#if CLI_ENABLE_IDLE_TIMEOUT || CLI_ENABLE_PERIODIC_CALLBACK || CLI_ENABLE_AUTH
    /* Timestamps taken from the old clock mean nothing on the new one. A running period
     * (idle time, periodic interval, login lockout) keeps the time it has already run and
     * continues from now on the new clock; without an old clock it starts now. */
    bool had_clock = (cli->time_source != NULL);
    uint32_t old_now = had_clock ? cli->time_source(cli->time_source_ctx) : 0U;
    uint32_t new_now = (fn != NULL) ? fn(ctx) : 0U;

#if CLI_ENABLE_IDLE_TIMEOUT
    cli->last_action_ts = cli_rebase_ts(cli->last_action_ts, had_clock, old_now, new_now);
#endif
#if CLI_ENABLE_PERIODIC_CALLBACK
    cli->last_periodic_ts = cli_rebase_ts(cli->last_periodic_ts, had_clock, old_now, new_now);
#endif
#if CLI_ENABLE_AUTH
    if (cli->auth_state == CLI_AUTH_LOCKOUT) {
      cli->auth_lockout_started_ts =
          cli_rebase_ts(cli->auth_lockout_started_ts, had_clock, old_now, new_now);
    }
#endif
#endif
    cli->time_source = fn;
    cli->time_source_ctx = ctx;
  }
#else
  (void)cli;
  (void)fn;
  (void)ctx;
#endif
}

OPENLIBCLI_API void cli_set_idle_timeout(cli_struct_t *cli, uint32_t seconds) {
#if CLI_ENABLE_IDLE_TIMEOUT
  if (cli != NULL) {
    cli->idle_timeout = seconds;
    cli_idle_period_restart(cli);
  }
#else
  (void)cli;
  (void)seconds;
#endif
}

OPENLIBCLI_API void cli_set_idle_timeout_cb(cli_struct_t *cli, uint32_t seconds,
                                            cli_idle_timeout_fn callback) {
#if CLI_ENABLE_IDLE_TIMEOUT
  if (cli != NULL) {
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
                                              cli_idle_timeout_mode_enum_t mode) {
#if CLI_ENABLE_IDLE_TIMEOUT
  if (cli != NULL) {
    cli->idle_timeout_policy = mode;
  }
#else
  (void)cli;
  (void)mode;
#endif
}

OPENLIBCLI_API void cli_set_periodic_cb(cli_struct_t *cli, cli_periodic_fn callback) {
#if CLI_ENABLE_PERIODIC_CALLBACK
  if (cli != NULL) {
    cli->periodic_cb = callback;
    if (cli->time_source != NULL) {
      cli->last_periodic_ts = cli->time_source(cli->time_source_ctx);
    }
  }
#else
  (void)cli;
  (void)callback;
#endif
}

OPENLIBCLI_API void cli_set_periodic_interval(cli_struct_t *cli, uint32_t seconds) {
#if CLI_ENABLE_PERIODIC_CALLBACK
  if (cli != NULL) {
    cli->periodic_interval = seconds;
    if (cli->time_source != NULL) {
      cli->last_periodic_ts = cli->time_source(cli->time_source_ctx);
    }
  }
#else
  (void)cli;
  (void)seconds;
#endif
}

OPENLIBCLI_API void cli_touch_activity(cli_struct_t *cli) {
  cli_touch_activity_internal(cli);
}

#if CLI_ENABLE_HISTORY

OPENLIBCLI_API int8_t cli_clear_history(cli_struct_t *cli) {
  int8_t rc = CLI_ERR;

  if (cli != NULL) {
    cli->history_head = (cli_history_idx_t)0;
    cli->history_count = (cli_history_idx_t)0;
    cli->history_nav = (cli_history_idx_t)0;
    cli->history_buf[0] = '\0';
#if CLI_HISTORY_RESTORE_PREBROWSE_LINE
    cli->history_saved[0] = '\0';
#endif
#if CLI_HISTORY_INDEX_SELECTION
    cli->history_cleared = True;
#endif

    rc = CLI_OK;
  }

  return rc;
}
#endif /* CLI_ENABLE_HISTORY */

OPENLIBCLI_API int8_t cli_check_idle_timeout(cli_struct_t *cli) {
  int8_t rc = CLI_OK;

#if CLI_ENABLE_IDLE_TIMEOUT
  /* A registered callback always owns the expiry; the policy applies only without one.
   * Closing runs from the start of the session (a connection nobody uses is closed), but a
   * reset only makes sense once the user has typed something: a freshly started or just
   * reset session is already in the state a reset would produce. */
  if (cli != NULL && cli->idle_timeout != 0UL && cli->time_source != NULL) {
    bool check_timeout =
        (cli->idle_timeout_cb != NULL ||
         cli->idle_timeout_policy == CLI_IDLE_TIMEOUT_POLICY_CLOSE ||
         (cli->idle_timeout_policy == CLI_IDLE_TIMEOUT_POLICY_RESET_SESSION && cli->idle_armed));
#if CLI_ENABLE_AUTH
    /* A lockout accepts no input and ends by itself; restarting the session would end it
     * early. The idle period starts again when the lockout is over. */
    check_timeout = check_timeout && (cli->auth_state != CLI_AUTH_LOCKOUT);
#endif

    if (check_timeout) {
      uint32_t now = cli->time_source(cli->time_source_ctx);
      uint32_t elapsed = (now - cli->last_action_ts);
      if (elapsed >= cli->idle_timeout) {
        rc = cli_handle_idle_timeout(cli);
        cli_flush(cli); /* also when called from the main loop, outside an engine tick */
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
                                   cli_priv_t privilege) {
  int8_t rc = CLI_OK;

#if CLI_ENABLE_AUTH
  if (cli == NULL || username == NULL || password == NULL || username[0] == '\0' ||
      strlen(username) >= (size_t)CLI_MAX_USERNAME_LEN ||
      strlen(password) >= (size_t)CLI_MAX_PASSWORD_LEN) {
    /* Truncated credentials would silently accept a different username / password. */
    rc = CLI_ERR;
  } else if (cli->num_users >= CLI_MAX_USERS) {
    rc = CLI_ERR_NOMEM;
  } else {
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

OPENLIBCLI_API void cli_set_authorization_cb(cli_struct_t *cli, cli_auth_fn fn) {
#if CLI_ENABLE_AUTH
  if (cli != NULL) {
    cli->authorization_cb = fn;
  }
#else
  (void)cli;
  (void)fn;
#endif
}

OPENLIBCLI_API void cli_require_authorization(cli_struct_t *cli, bool require) {
#if CLI_ENABLE_AUTH
  if (cli != NULL) {
    cli->require_auth = require;
  }
#else
  (void)cli;
  (void)require;
#endif
}

OPENLIBCLI_API void cli_set_auth_failure_mode(cli_struct_t *cli,
                                              cli_auth_failure_mode_enum_t mode) {
#if CLI_ENABLE_AUTH
  if (cli != NULL) {
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
OPENLIBCLI_API void cli_set_auth_lockout_duration(cli_struct_t *cli, uint32_t seconds) {
#if CLI_ENABLE_AUTH
  if (cli != NULL) {
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
                                                const char *help) {
  /* Public names / help live in program memory on AVR (CLI_STR), in RAM elsewhere. */
  return cli_add_command_impl(cli, parent, name, CLI_TEXT_PROGMEM, callback, privilege, mode, help,
                              CLI_TEXT_PROGMEM);
}

#if CLI_ENABLE_ALIASES
OPENLIBCLI_API cli_cmd_handle_t cli_add_command_duplicate(cli_struct_t *cli,
                                                          cli_cmd_handle_t original,
                                                          const char *alias_name) {
  cli_cmd_handle_t rc = CLI_CMD_INVALID;

  /* Sub-commands are not shared: a duplicate of a submenu would be reachable without them
   * (an inline alias, cli_cmd_add_alias(), keeps them). */
  if (cli_handle_valid(cli, original) && alias_name != NULL &&
      !cli_cmd_has_registered_children(cli, original)) {
    const cli_cmd_struct_t *orig = &cli->cmd_pool[original];
    /* The new name comes from the caller (program memory on AVR); the copied help text
     * already lives in the original node, i.e. in RAM. */
    rc = cli_add_command_impl(cli, orig->parent, alias_name, CLI_TEXT_PROGMEM, orig->callback,
                              orig->privilege, orig->mode,
#if CLI_ENABLE_COMMAND_HELP
                              orig->help,
#else
                              NULL,
#endif
                              False);
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_cmd_add_alias(cli_struct_t *cli, cli_cmd_handle_t handle,
                                        const char *alias_name) {
  int8_t rc = CLI_ERR;

  if (cli_handle_valid(cli, handle) && alias_name != NULL &&
      cli_text_len(alias_name, CLI_TEXT_PROGMEM) < (size_t)CLI_MAX_NAME_LEN) {
    cli_cmd_struct_t *cmd = &cli->cmd_pool[handle];

    if (cmd->num_aliases >= CLI_MAX_ALIASES) {
      rc = CLI_ERR_NOMEM;
    } else {
      /* Copy into the next alias slot; it only counts once num_aliases is incremented, so
       * the checks below do not see it yet. */
      char *alias = cmd->aliases[cmd->num_aliases];
      cli_text_copy(alias, alias_name, CLI_MAX_NAME_LEN, CLI_TEXT_PROGMEM);
      if (!cli_keyword_valid(alias) || cli_cmd_exact(cmd, alias) ||
          cli_name_taken(cli, cmd->parent, cmd->mode, alias, handle)) {
        alias[0] = '\0';
        rc = CLI_ERR;
      } else {
        cmd->num_aliases++;
        rc = CLI_OK;
      }
    }
  }

  return rc;
}
#endif /* CLI_ENABLE_ALIASES */

OPENLIBCLI_API int8_t cli_remove_command(cli_struct_t *cli, cli_cmd_handle_t handle) {
  int8_t rc = CLI_ERR;

  if (cli_handle_valid(cli, handle)) {
    cli_cmd_handle_t parent = cli->cmd_pool[handle].parent;

    /* Move any navigation level at or below the removed node up to its parent. This needs
     * the parent links intact, so it runs before the children are detached. */
    cli_nav_relocate(cli, handle, cli_handle_valid(cli, parent) ? parent : CLI_CMD_ROOT);

    /* Detach children so a command later registered in this slot does not adopt them. */
    for (cli_poolsize_t i = 0; i < cli->cmd_pool_size; i++) {
      if (CLI_CMD_IS_IN_USE(&cli->cmd_pool[i]) && cli->cmd_pool[i].parent == handle) {
        cli->cmd_pool[i].parent = CLI_CMD_INVALID;
      }
    }

    (void)memset(&cli->cmd_pool[handle], 0, sizeof(cli->cmd_pool[handle]));
    if (cli->cmd_pool_count > 0) {
      cli->cmd_pool_count--;
    }
    rc = CLI_OK;
  }

  return rc;
}

//-V::2565
OPENLIBCLI_API int8_t cli_remove_command_recursive(cli_struct_t *cli, cli_cmd_handle_t handle) {
  int8_t rc = CLI_ERR;

  if (cli_handle_valid(cli, handle)) {
    bool removed_child = True;

    while (removed_child) {
      removed_child = False;
      for (cli_poolsize_t i = 0; i < cli->cmd_pool_size; i++) {
        if (CLI_CMD_IS_IN_USE(&cli->cmd_pool[i]) && cli->cmd_pool[i].parent == handle) {
          (void)cli_remove_command_recursive(cli, i);
          removed_child = True;
          break;
        }
      }
    }

    rc = cli_remove_command(cli, handle);
  }

  return rc;
}

OPENLIBCLI_API void cli_hide_command(cli_struct_t *cli, cli_cmd_handle_t handle) {
  if (cli_handle_valid(cli, handle)) {
    CLI_CMD_SET_HIDDEN(&cli->cmd_pool[handle]);
  }
}

/* Mode / Privilege ---------------------------------------------------------------------*/

OPENLIBCLI_API int8_t cli_set_mode(cli_struct_t *cli, cli_mode_t mode) {
  int8_t rc = CLI_ERR;

  if (cli != NULL && mode != CLI_MODE_ANY) {
    int ancestor = -1;

    rc = CLI_OK;
    for (int i = (int)cli->nav_depth - 1; i >= 0 && ancestor < 0; i--) {
      if (cli->nav_stack[i].mode == mode) {
        ancestor = i;
      }
    }

    if (mode == cli->mode) {
      /* Already there. */
    } else if (ancestor >= 0) {
      /* Return to an enclosing level, restoring the submenu and privilege it had. */
      cli_nav_return(cli, (uint8_t)ancestor);
    } else {
      rc = cli_nav_push(cli);
      if (rc == CLI_OK) {
        /* A new mode starts at the root: the submenu's commands belong to the mode it was
         * entered in. The saved level keeps the submenu, so 'exit' returns to it. A submenu
         * command that switches mode is entered afterwards (cli_context_enter()). */
        cli->mode = mode;
        cli->context = CLI_CMD_ROOT;
        cli_notify_mode_change(cli);
      }
    }
  }

  return rc;
}

OPENLIBCLI_API void cli_set_privilege(cli_struct_t *cli, cli_priv_t privilege) {
  if (cli != NULL) {
    cli->privilege = privilege;
    cli_notify_mode_change(cli);
  }
}

OPENLIBCLI_API cli_mode_t cli_get_mode(const cli_struct_t *cli) {
  return (cli != NULL) ? cli->mode : (cli_mode_t)-1;
}

OPENLIBCLI_API cli_priv_t cli_get_privilege(const cli_struct_t *cli) {
  /* Fail closed: an unknown session has the lowest privilege. */
  return (cli != NULL) ? cli->privilege : CLI_PRIV_USER;
}

OPENLIBCLI_API int8_t cli_set_enable_secret(cli_struct_t *cli, const char *secret) {
  int8_t rc = CLI_ERR;

#if CLI_ENABLE_AUTH
  const char *value = (secret != NULL) ? secret : "";

  if (cli != NULL && strlen(value) < (size_t)CLI_MAX_ENABLE_SECRET_LEN) {
    (void)cli_strlcpy(cli->enable_privilege_secret, value, CLI_MAX_ENABLE_SECRET_LEN);
    rc = CLI_OK;
  }
#else
  (void)cli;
  (void)secret;
#endif

  return rc;
}

OPENLIBCLI_API void cli_set_enable_privilege_cb(cli_struct_t *cli, cli_enable_fn fn) {
#if CLI_ENABLE_AUTH
  if (cli != NULL) {
    cli->enable_privilege_cb = fn;
  }
#else
  (void)cli;
  (void)fn;
#endif
}

OPENLIBCLI_API void cli_set_exit_cb(cli_struct_t *cli, cli_session_cmd_fn fn) {
  if (cli != NULL) {
    cli->on_exit_cmd = fn;
  }
}

OPENLIBCLI_API void cli_set_quit_cb(cli_struct_t *cli, cli_session_cmd_fn fn) {
  if (cli != NULL) {
    cli->on_quit_cmd = fn;
  }
}

OPENLIBCLI_API void cli_set_exit_root_policy(cli_struct_t *cli,
                                             cli_exit_root_policy_enum_t policy) {
  if (cli != NULL) {
    cli->exit_root_policy = policy;
  }
}

OPENLIBCLI_API void cli_set_quit_policy(cli_struct_t *cli, cli_quit_policy_enum_t policy) {
  if (cli != NULL) {
    cli->quit_policy = policy;
  }
}

OPENLIBCLI_API int8_t cli_exec(cli_struct_t *cli, const char *cmd) {
  int8_t rc = CLI_ERR;

  if (cli != NULL && cmd != NULL) {
    char *line = NULL;
    size_t size = 0U;

    rc = cli_exec_area(cli, &line, &size);
    if (rc == CLI_OK) {
      size_t len = strlen(cmd);
      if (len >= size) {
        rc = CLI_ERR_NOMEM; /* never run a truncated command line */
      } else {
        (void)memcpy(line, cmd, len + 1U);
        rc = cli_exec_area_run(cli, line, len);
      }
    }
  }

  return rc;
}

OPENLIBCLI_API int8_t cli_exec_argv(cli_struct_t *cli, const char *const *argv, cli_argc_t argc) {
  size_t pos = 0;
  bool fits = True;
  int8_t rc = CLI_ERR;
  char *buf = NULL;
  size_t size = 0U;

  if (cli != NULL && argv != NULL && argc > 0) {
    rc = cli_exec_area(cli, &buf, &size);
  }

  /* The joined line is built directly in the free part of the input buffer. */
  if (buf != NULL && rc == CLI_OK) {
    for (cli_argc_t i = 0; i < argc && fits; i++) {
      const char *arg = (argv[i] != NULL) ? argv[i] : "";
      bool quote = (arg[0] == '\0');

      for (const char *p = arg; *p != '\0' && !quote; p++) {
        quote = (*p == ' ' || *p == '\t' || *p == '"' || *p == '|' || *p == '?');
      }

      /* Every write keeps one byte free for the terminating NUL. */
      if (i > 0) {
        fits = (pos + 1U < size);
        if (fits) {
          buf[pos++] = ' ';
        }
      }
      if (fits && quote) {
        fits = (pos + 1U < size);
        if (fits) {
          buf[pos++] = '"';
        }
      }
      for (const char *p = arg; *p != '\0' && fits; p++) {
        bool escape = quote && (*p == '"' || *p == '\\');
        fits = (pos + (escape ? 2U : 1U) < size);
        if (fits) {
          if (escape) {
            buf[pos++] = '\\';
          }
          buf[pos++] = *p;
        }
      }
      if (fits && quote) {
        fits = (pos + 1U < size);
        if (fits) {
          buf[pos++] = '"';
        }
      }
    }

    if (fits) {
      buf[pos] = '\0';
      rc = cli_exec_area_run(cli, buf, pos);
    } else {
      rc = CLI_ERR_NOMEM;
    }
  }

  return rc;
}

/**
 * @brief Locate the free tail of @c input[] for the next @c cli_exec() line.
 *
 * @c input[] is used as a small arena. When no command is running, its head
 * holds the line being edited (possibly a password), so the new line goes after
 * it. While commands run, @c exec_used covers the lines they are executing —
 * their @c argv strings point into them — and the new line goes after those.
 *
 * @param[in]  cli   Active CLI session.
 * @param[out] area  Start of the free space.
 * @param[out] size  Bytes available at @p area (including room for the NUL).
 *
 * @return @c CLI_OK, @c CLI_ERR when @c CLI_MAX_EXEC_DEPTH commands are already
 *         executing (reported to the user), or @c CLI_ERR_NOMEM when the buffer is full.
 */
static int8_t cli_exec_area(cli_struct_t *cli, char **area, size_t *size) {
  int8_t rc = CLI_ERR_NOMEM;
  cli_len_t base = (cli->exec_depth > 0U) ? cli->exec_used : (cli->input_len + 1);

  if (cli->exec_depth >= (uint8_t)CLI_MAX_EXEC_DEPTH) {
    /* Also stops a handler that (indirectly) runs itself from recursing without bound. */
    CLI_WSTR(cli, "% Command nesting too deep.\r\n");
    rc = CLI_ERR;
  } else if (base < (cli_len_t)CLI_MAX_INPUT_LEN) {
    *area = &cli->input[base];
    *size = (size_t)((cli_len_t)CLI_MAX_INPUT_LEN - base);
    rc = CLI_OK;
  } else {
    /* No free byte left after the lines in use. */
  }

  return rc;
}

/**
 * @brief Execute a programmatic line placed by @c cli_exec_area().
 *
 * The region is reserved for the duration of the call so nested @c cli_exec()
 * calls go after it, and released afterwards. The line is not added to the
 * history.
 *
 * @param[in]     cli   Active CLI session.
 * @param[in,out] line  Line inside @c input[] (tokenized in place).
 * @param[in]     len   Length of @p line.
 *
 * @return Result of the command.
 */
static int8_t cli_exec_area_run(cli_struct_t *cli, char *line, size_t len) {
  bool top_level = (cli->exec_depth == 0U);
  /* Run from outside any command (a callback, the main loop) while a prompt is on screen:
   * the output goes below it, and the prompt with what was typed so far is shown again. */
  bool on_prompt = top_level && (cli->session_state == CLI_SESSION_RUN);
  bool redrawn = False;
  cli_len_t saved_used = cli->exec_used;
  int8_t rc;

  if (on_prompt) {
    CLI_WSTR(cli, CLI_CRLF);
  }

  cli->exec_used = (cli_len_t)(line - cli->input) + (cli_len_t)len + 1;
  rc = cli_execute(cli, line, False);
  cli->exec_used = saved_used;

  if (top_level) {
    /* The command may have emptied the editor (password prompt, session restart) while
     * its bytes were still in use; terminate the editor's line now that nothing runs. */
    cli->input[cli->input_len] = '\0';
    if (rc == CLI_ERR_QUIT || rc == CLI_ERR_AUTH) {
      /* Ends the session exactly as the same command typed at the prompt does. Nested in
       * another command, the result is that command's to act on. */
      cli->session_state = CLI_SESSION_STOP;
    }
  }

#if CLI_ENABLE_HISTORY && CLI_HISTORY_INDEX_SELECTION
  if (rc == CLI_EXEC_REPLAY_INPUT) {
    /* Recall into the editor only when it is free: not nested, no password prompt and no
     * line being typed. */
    if (top_level && !cli_state_is_auth_input(cli) && cli->input_len == 0 &&
        cli_history_replay_load(cli)) {
      redrawn = True; /* loaded and redrawn for editing */
    } else {
      cli->history_replay = (cli_history_idx_t)-1;
      rc = CLI_OK;
    }
  }
#endif

  /* The prompt for what is asked for now: the command line, or a login / password prompt
   * the command requested. Not after a restart or stop (the banner or nothing follows). */
  if (on_prompt && !redrawn && cli->session_state == CLI_SESSION_RUN) {
    cli_redisplay_input(cli);
  }
  if (top_level) {
    /* Possibly called from the main loop, outside any engine tick. */
    cli_flush(cli);
  }

  return rc;
}

/* Output Helpers -----------------------------------------------------------------------*/

OPENLIBCLI_API int cli_print_p(cli_struct_t *cli, const char *fmt, ...) {
  //-V::2604
  va_list ap;
  int rc = -1;
  int n;

  if (cli == NULL || fmt == NULL) {
    /* Nothing to print to. */
  } else if (cli->print_cb_v != NULL) {
    //-V::2604
    (void)va_start(ap, fmt);
    rc = cli->print_cb_v(cli, CLI_PRINT_KIND_PRINT, fmt, ap);
    //-V::2604
    (void)va_end(ap);
  } else {
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

    rc = n;
    if (n < 0) {
      /* Encoding error: buf contents are unspecified, emit nothing. */
    } else if (cli->print_cb != NULL) {
      cli->print_cb(cli, buf, CLI_PRINT_KIND_PRINT);
    } else {
      int written = (n > (int)sizeof(buf) - 1) ? (int)sizeof(buf) - 1 : n;
      cli_print_process_stream(cli, buf, written);
    }
  }

  return rc;
}

OPENLIBCLI_API int cli_println_p(cli_struct_t *cli, const char *fmt, ...) {
  //-V::2604
  va_list ap;
  int rc = -1;
  int n;

  if (cli == NULL || fmt == NULL) {
    /* Nothing to print to. */
  } else if (cli->print_cb_v != NULL) {
    //-V::2604
    (void)va_start(ap, fmt);
    rc = cli->print_cb_v(cli, CLI_PRINT_KIND_PRINTLN, fmt, ap);
    //-V::2604
    (void)va_end(ap);
  } else {
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

    rc = (n < 0) ? n : (n + 2); /* the documented count includes the \r\n */
    if (n < 0) {
      /* Encoding error: buf contents are unspecified, emit nothing. */
    } else if (cli->print_cb != NULL) {
      cli->print_cb(cli, buf, CLI_PRINT_KIND_PRINTLN);
    } else {
      int written = (n > (int)sizeof(buf) - 1) ? (int)sizeof(buf) - 1 : n;
      cli_print_process_stream(cli, buf, written);
      cli_print_process_stream(cli, "\n", 1);
    }
  }

  return rc;
}

OPENLIBCLI_API int cli_error_p(cli_struct_t *cli, const char *fmt, ...) {
  //-V::2604
  va_list ap;
  int rc = -1;
  int n;

  if (cli == NULL || fmt == NULL) {
    /* Nothing to print to. */
  } else if (cli->print_cb_v != NULL) {
    //-V::2604
    (void)va_start(ap, fmt);
    rc = cli->print_cb_v(cli, CLI_PRINT_KIND_ERROR, fmt, ap);
    //-V::2604
    (void)va_end(ap);
  } else {
    /* The message is formatted after its "% " prefix, so the line with the prefix can be
     * delivered as a whole (print_transport_cb receives it like the terminal shows it). */
    char buf[CLI_MAX_OUTPUT_BUF];
    char *msg = &buf[2];

    buf[0] = '%';
    buf[1] = ' ';
    //-V::2604
    (void)va_start(ap, fmt);
#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
    //-V:vsnprintf_P:2600
    n = vsnprintf_P(msg, sizeof(buf) - 2U, fmt, ap);
#else
    n = cli_vsnprintf(msg, sizeof(buf) - 2U, fmt, ap);
#endif
    //-V::2604
    (void)va_end(ap);

    /* As for cli_println(): the line as vsnprintf counts it, with its "% " prefix and \r\n. */
    rc = (n < 0) ? n : (n + 4);
    if (n < 0) {
      /* Encoding error: buf contents are unspecified, emit nothing. */
    } else if (cli->print_cb != NULL) {
      cli->print_cb(cli, msg, CLI_PRINT_KIND_ERROR); /* the kind tells it is an error */
    } else {
      /* Errors bypass filters and always appear — after any output already produced, so a
       * buffered partial line (of a command, or of a callback outside any) is delivered
       * first. */
      bool in_command = (cli->exec_depth > 0U);
      cli_output_end_line(cli);
#if CLI_ENABLE_PRINT_TRANSPORT_CB
      if (cli->print_transport_cb != NULL) {
        cli->print_transport_cb(cli, buf);
      } else
#endif
      {
        /* Outside a command the cursor may sit on the prompt line: start a new line. */
        if (!in_command) {
          CLI_WSTR(cli, CLI_CRLF);
        }
        cli_write_str(cli, buf);
        CLI_WSTR(cli, CLI_CRLF);
      }
    }
  }

  return rc;
}

OPENLIBCLI_API void cli_print_flush(cli_struct_t *cli) {
#if CLI_HAS_LINE_BUFFER
  if (cli != NULL && cli->filter_line_buf_len > 0) {
    cli_line_buffer_emit(cli);
  }
#else
  (void)cli;
#endif
}

#if CLI_ENABLE_PRINT_TRANSPORT_CB
OPENLIBCLI_API void cli_set_print_transport_cb(cli_struct_t *cli, cli_print_transport_fn cb) {
  if (cli != NULL) {
    cli->print_transport_cb = cb;
  }
}
#endif

OPENLIBCLI_API void cli_set_print_cb(cli_struct_t *cli, cli_print_fn cb) {
  if (cli != NULL) {
    cli->print_cb = cb;
  }
}

OPENLIBCLI_API void cli_set_print_cb_v(cli_struct_t *cli, cli_print_v_fn cb) {
  if (cli != NULL) {
    cli->print_cb_v = cb;
  }
}

OPENLIBCLI_API int cli_snprintf(char *buf, size_t size, const char *fmt, ...) {
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
OPENLIBCLI_API int cli_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
  //-V::2600
  return vsnprintf(buf, size, fmt, ap);
}

OPENLIBCLI_API int cli_sscanf(const char *str, const char *fmt, void *out) {
  int result = 0;

  if (str != NULL && fmt != NULL && out != NULL) {
    /* 1 when the value was stored; no match (0) and an empty string (EOF) are failures. */
    //-V::2600
    result = (sscanf(str, fmt, out) == 1) ? 1 : 0;
  }

  return result;
}

/*=======================================================================================
 * Private Functions
 *=======================================================================================*/

/* Internal String Helpers ---------------------------------------------------------------*/

static size_t cli_strlcpy(char *dst, const char *src, size_t size) {
  size_t i = 0;
  if (size > 1U) {
    while (i < size - 1U && src[i] != '\0') {
      dst[i] = src[i];
      i++;
    }
  }
  if (size > 0U) {
    dst[i] = '\0';
  }
  return i;
}

/**
 * @brief Length of a string held in RAM or (on AVR) program memory.
 *
 * @param[in] src      NUL-terminated string.
 * @param[in] progmem  @p src is in program memory (ignored off AVR).
 *
 * @return Length in characters.
 */
static size_t cli_text_len(const char *src, bool progmem) {
#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
  return progmem ? strlen_P(src) : strlen(src);
#else
  (void)progmem;
  return strlen(src);
#endif
}

/**
 * @brief Bounded copy of a string held in RAM or (on AVR) program memory.
 *
 * @param[out] dst      Destination RAM buffer (always NUL-terminated).
 * @param[in]  src      NUL-terminated source.
 * @param[in]  size     Size of @p dst.
 * @param[in]  progmem  @p src is in program memory (ignored off AVR).
 */
static void cli_text_copy(char *dst, const char *src, size_t size, bool progmem) {
#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
  if (progmem) {
    (void)cli_strlcpy_P(dst, src, size);
  } else {
    (void)cli_strlcpy(dst, src, size);
  }
#else
  (void)progmem;
  (void)cli_strlcpy(dst, src, size);
#endif
}

#if CLI_ENABLE_AUTH
/**
 * @brief Compare two secrets in time independent of where they differ.
 *
 * Always inspects @p max_len bytes positions, so the comparison time reveals
 * neither the length of a matching prefix nor the stored secret's length.
 * Characters after a terminating NUL are never read.
 *
 * @param[in] a        First NUL-terminated string.
 * @param[in] b        Second NUL-terminated string.
 * @param[in] max_len  Positions compared. At least one of the strings must be shorter, so
 *                     a longer other string differs at that string's terminator.
 *
 * @return @c true when both strings are identical.
 */
static bool cli_secret_equal(const char *a, const char *b, size_t max_len) {
  unsigned diff = 0U;
  bool a_end = False;
  bool b_end = False;

  for (size_t i = 0; i < max_len; i++) {
    unsigned char ca = a_end ? 0U : (unsigned char)a[i];
    unsigned char cb = b_end ? 0U : (unsigned char)b[i];
    diff |= (unsigned)(ca ^ cb);
    a_end = a_end || (ca == 0U);
    b_end = b_end || (cb == 0U);
  }

  return (diff == 0U);
}

/**
 * @brief Erase the whole input buffer (not just its first byte) after a
 *        password has been processed, so the secret does not linger in memory.
 *
 * @param[in] cli  Active CLI session.
 */
static void cli_wipe_input(cli_struct_t *cli) {
  volatile char *p = cli->input;
  for (size_t i = 0; i < sizeof(cli->input); i++) {
    p[i] = '\0';
  }
  cli_reset_input(cli);
}
#endif

static bool cli_case_prefix_match(const char *str, const char *prefix) {
  bool match = True;

  while (*prefix != '\0' && match) {
    if (tolower((int)(unsigned char)*str) != tolower((int)(unsigned char)*prefix)) {
      match = False;
    } else {
      str++;
      prefix++;
    }
  }

  return match;
}

/**
 * @brief Case-insensitive string equality (ASCII).
 *
 * Used for exact command matching so it follows the same case rules as prefix
 * matching.
 *
 * @param[in] a  First NUL-terminated string.
 * @param[in] b  Second NUL-terminated string.
 *
 * @return @c true when both strings are equal ignoring ASCII case.
 */
static bool cli_case_equal(const char *a, const char *b) {
  while (*a != '\0' && tolower((int)(unsigned char)*a) == tolower((int)(unsigned char)*b)) {
    a++;
    b++;
  }
  return (*a == '\0' && *b == '\0');
}

/**
 * @brief Test whether @p s can be used as a command keyword.
 *
 * A keyword must be non-empty and must not contain characters that the line
 * editor or tokenizer treats specially (whitespace, control characters, @c ?,
 * @c | and @c "), otherwise it could never be typed as a single word.
 *
 * @param[in] s  NUL-terminated candidate keyword (RAM).
 *
 * @return @c true when @p s is a valid keyword.
 */
static bool cli_keyword_valid(const char *s) {
  bool valid = (s[0] != '\0');

  for (const char *p = s; *p != '\0' && valid; p++) {
    unsigned char c = (unsigned char)*p;
    valid = (c > 0x20U && c != (unsigned char)ASCII_DEL && c != (unsigned char)'?' &&
             c != (unsigned char)ASCII_PIPE && c != (unsigned char)'"');
  }

  return valid;
}

/**
 * @brief Report whether the first @p len characters of @p s leave a double-quoted
 *        string open.
 *
 * Follows the tokenizer rules: @c " toggles quoting, and inside quotes a
 * backslash escapes the following @c " or @c \\.
 *
 * @param[in] s    Input text.
 * @param[in] len  Number of characters of @p s to scan.
 *
 * @return @c true if a quote is open after @p len characters.
 */
static bool cli_quote_open(const char *s, cli_len_t len) {
  bool in_quote = False;

  for (cli_len_t i = 0; i < len; i++) {
    if (in_quote && s[i] == '\\' && i + 1 < len && (s[i + 1] == '"' || s[i + 1] == '\\')) {
      i++;
    } else if (s[i] == '"') {
      in_quote = !in_quote;
    } else {
      /* Ordinary character. */
    }
  }

  return in_quote;
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
static inline char *cli_skip_spaces(char *p) {
  while (*p == ' ' || *p == '\t') {
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
static inline char *cli_skip_non_spaces(char *p) {
  while (*p != '\0' && *p != ' ' && *p != '\t') {
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
static void cli_raw_write(cli_struct_t *cli, const uint8_t *buf, cli_transport_buflen_t len) {
  if (cli != NULL && cli->transport.write != NULL) {
    cli_transport_buflen_t sent = 0;

    /* Short writes are continued until everything is out. A negative result, or a write
     * that makes no progress, means the peer is gone (or stuck): remember it so the session
     * engine ends the session instead of writing into a dead transport forever. */
    while (sent < len && !cli->tx_error) {
      cli_transport_ret_t n = cli->transport.write(cli->transport.ctx, &buf[sent], len - sent);
      if (n <= 0) {
        cli->tx_error = True;
      } else {
        sent += (cli_transport_buflen_t)n;
        cli->tx_unflushed = True;
      }
    }
  }
}

/**
 * @brief Write a NUL-terminated string to the transport.
 * @param[in] cli  Active CLI session.
 * @param[in] s    String to transmit; no-op if NULL.
 */
static void cli_write_str(cli_struct_t *cli, const char *s) {
  if (s != NULL) {
    cli_raw_write(cli, (const uint8_t *)s, (cli_transport_buflen_t)strlen(s));
  }
}

/**
 * @brief Write a single character to the transport.
 * @param[in] cli  Active CLI session.
 * @param[in] c    Character to transmit.
 */
static void cli_write_char(cli_struct_t *cli, char c) {
  cli_raw_write(cli, (const uint8_t *)&c, 1);
}

/**
 * @brief Write output text to the transport, translating line endings.
 *
 * Each @c \\n becomes @c \\r\\n and bare @c \\r characters are dropped. Runs of
 * ordinary characters are written with a single transport call.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] data  Text to write (need not be NUL-terminated).
 * @param[in] len   Number of bytes in @p data.
 */
static void cli_write_text(cli_struct_t *cli, const char *data, int len) {
  int start = 0;

  for (int i = 0; i < len; i++) {
    if (data[i] == '\n' || data[i] == '\r') {
      cli_raw_write(cli, (const uint8_t *)&data[start], (cli_transport_buflen_t)(i - start));
      if (data[i] == '\n') {
        cli_write_str(cli, CLI_CRLF);
        cli->out_open = False;
      }
      start = i + 1;
    }
  }
  if (len > start) {
    cli_raw_write(cli, (const uint8_t *)&data[start], (cli_transport_buflen_t)(len - start));
    /* Text after the last newline: a command's line is still open. Output printed outside
     * any command (a callback) is not tracked: the next line ending typed ends its line. */
    cli->out_open = (cli->exec_depth > 0U);
  }
}

/**
 * @brief Flush the transport's send buffer if a flush function is registered and
 *        anything was written since the last flush.
 * @param[in] cli  Active CLI session.
 */
static void cli_flush(cli_struct_t *cli) {
  if (cli->tx_unflushed && cli->transport.flush != NULL) {
    (void)cli->transport.flush(cli->transport.ctx);
  }
  cli->tx_unflushed = False;
}

/* Session Timing Helpers ---------------------------------------------------------------*/

/**
 * @brief Record user activity: restart the idle period and arm the reset-session
 *        idle policy.
 * @param[in] cli  Active CLI session.
 */
static void cli_touch_activity_internal(cli_struct_t *cli) {
#if CLI_ENABLE_IDLE_TIMEOUT
  if (cli != NULL) {
    cli->idle_armed = True;
  }
#endif
  cli_idle_period_restart(cli);
}

/**
 * @brief Start a new idle period without counting it as user activity.
 * @param[in] cli  Active CLI session.
 */
#if CLI_ENABLE_TIME_SOURCE &&                                                                      \
    (CLI_ENABLE_IDLE_TIMEOUT || CLI_ENABLE_PERIODIC_CALLBACK || CLI_ENABLE_AUTH)
/**
 * @brief Move a timestamp from one clock to another, keeping the time elapsed since it.
 *
 * @param[in] ts         Timestamp on the old clock.
 * @param[in] had_clock  @c false when there was no old clock (the period starts now).
 * @param[in] old_now    Current reading of the old clock.
 * @param[in] new_now    Current reading of the new clock.
 *
 * @return The timestamp on the new clock.
 */
static uint32_t cli_rebase_ts(uint32_t ts, bool had_clock, uint32_t old_now, uint32_t new_now) {
  return had_clock ? new_now - (old_now - ts) : new_now;
}
#endif

static void cli_idle_period_restart(cli_struct_t *cli) {
#if CLI_ENABLE_IDLE_TIMEOUT && CLI_ENABLE_TIME_SOURCE
  if (cli != NULL && cli->time_source != NULL) {
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
static uint32_t cli_default_time_source(void *ctx) {
  (void)ctx;
  uint32_t now_sec = 0;

#if ENV_IS_WINDOWS
  now_sec = (uint32_t)(GetTickCount64() / 1000U);
#elif ENV_IS_OS_ENVIRONMENT
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    now_sec = (uint32_t)ts.tv_sec;
  } else {
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
static int8_t cli_handle_idle_timeout(cli_struct_t *cli) {
  int8_t rc = CLI_OK;

  if (cli->idle_timeout_cb != NULL) { //>! user callback
    rc = cli->idle_timeout_cb(cli);
    if (rc == CLI_OK) {
      /* Callback kept the session alive: start a new idle period. */
      cli_idle_period_restart(cli);
    }
  } else {
    if (cli->idle_timeout_policy == CLI_IDLE_TIMEOUT_POLICY_RESET_SESSION) {
      /* The restart moves below the prompt line (delivering what was printed so far); the
       * message follows there, before the new session's banner and prompt. The restarted
       * session is disarmed until the user types again, so an unattended session is reset
       * once instead of every timeout period. */
      cli_restart_session(cli);
      CLI_WSTR(cli, "% Idle timeout. Session restarted.\r\n");
    } else if (cli->idle_timeout_policy == CLI_IDLE_TIMEOUT_POLICY_CLOSE) {
      CLI_WSTR(cli, "\r\n% Idle timeout.\r\n");
      cli->session_state = CLI_SESSION_STOP;
      rc = CLI_ERR_QUIT;
    } else {
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
#if CLI_HAS_LINE_BUFFER
static void cli_print_emit(cli_struct_t *cli, const char *line) {
  bool emit = True;

#if CLI_ENABLE_PIPE_FILTER
  if (!cli_filter_line(cli, line)) {
    emit = False;
  }
#endif

  if (emit) {
#if CLI_ENABLE_PRINT_TRANSPORT_CB
    if (cli->print_transport_cb != NULL) {
      cli->print_transport_cb(cli, line);
    } else
#endif
    {
      cli_write_str(cli, line);
      cli_write_str(cli, CLI_CRLF);
    }
  }
}

/**
 * @brief Emit the buffered output text as one line and empty the buffer.
 * @param[in] cli  Active CLI session.
 */
static void cli_line_buffer_emit(cli_struct_t *cli) {
  cli->filter_line_buf[cli->filter_line_buf_len] = '\0';
  cli->filter_line_buf_len = 0;
  cli_print_emit(cli, cli->filter_line_buf);
}
#endif

/**
 * @brief Route command output text to the transport.
 *
 * While a pipe filter is active or a @c print_transport_cb is installed, output
 * must be judged / delivered as whole lines: characters are accumulated in
 * @c filter_line_buf until a @c \\n is encountered, at which point the buffered
 * text is emitted as one logical line. A line longer than
 * @c CLI_MAX_OUTPUT_BUF - 1 characters is emitted in pieces of that size, so no
 * output is lost. Otherwise the text is written straight to the transport, so
 * partial lines appear immediately. Carriage returns are ignored in both paths.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] data  Character stream to process.
 * @param[in] len   Number of bytes available in @p data.
 */
static void cli_print_process_stream(cli_struct_t *cli, const char *data, int len) {
#if CLI_HAS_LINE_BUFFER
  bool line_mode = (cli->filter_line_buf_len > 0);
#if CLI_ENABLE_PIPE_FILTER
  line_mode = line_mode || (cli->filter_type != CLI_FILTER_NONE);
#endif
#if CLI_ENABLE_PRINT_TRANSPORT_CB
  line_mode = line_mode || (cli->print_transport_cb != NULL);
#endif

  if (line_mode) {
    for (int i = 0; i < len; i++) {
      char c = data[i];

      if (c == '\n') {
        cli_line_buffer_emit(cli);
      } else if (c != '\r') {
        if (cli->filter_line_buf_len >= (int)CLI_MAX_OUTPUT_BUF - 1) {
          cli_line_buffer_emit(cli); /* buffer full: deliver this piece, keep going */
        }
        cli->filter_line_buf[cli->filter_line_buf_len++] = c;
      } else {
        /* To silence MISRA */
      }
    }
  } else {
    cli_write_text(cli, data, len);
  }
#else
  cli_write_text(cli, data, len);
#endif
}

/**
 * @brief Finish the current command-output line, if one is open.
 *
 * Delivers a buffered partial line and terminates a partial line already
 * written to the terminal, so whatever follows (the next prompt, an error)
 * starts on a fresh line in every output mode.
 *
 * @param[in] cli  Active CLI session.
 */
static void cli_output_end_line(cli_struct_t *cli) {
#if CLI_HAS_LINE_BUFFER
  if (cli->filter_line_buf_len > 0) {
    cli_line_buffer_emit(cli);
  }
#endif
  if (cli->out_open) {
    CLI_WSTR(cli, CLI_CRLF);
    cli->out_open = False;
  }
}

#if !defined(__AVR__) && !defined(ARDUINO_ARCH_AVR)
/**
 * @brief Send a NUL-terminated RAM string through the command output path.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] s    String to print.
 */
static void cli_print_str(cli_struct_t *cli, const char *s) {
  cli_print_process_stream(cli, s, (int)strlen(s));
}
#endif

/**
 * @brief While a recalled history entry is shown, keep the edit just made to it as the
 *        line restored at the tip (@c CLI_HISTORY_RESTORE_PREBROWSE_LINE), so moving on
 *        through the history does not lose it.
 * @param[in] cli  Active CLI session.
 */
static void cli_history_capture_edited_preview(cli_struct_t *cli) {
#if CLI_ENABLE_HISTORY && CLI_HISTORY_RESTORE_PREBROWSE_LINE
  if (cli != NULL && cli->history_nav > 0) {
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
static void cli_redraw_line(cli_struct_t *cli) {
  /* Move to start of line, clear to EOL, reprint input */
  CLI_WSTR(cli, "\r");
  cli_show_prompt(cli);

  cli_raw_write(cli, (const uint8_t *)cli->input, cli->input_len);

  if (cli->ansi_supported) {
    CLI_WSTR(cli, "\x1b"
                  "[K");
  }

  /* If cursor is not at end, move it back (plain BS, like all other cursor motion) */
  cli_len_t trail = cli->input_len - cli->cursor;
  for (cli_len_t i = 0; i < trail; i++) {
    cli_write_char(cli, ASCII_BS);
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
static void cli_move_cursor_delta(cli_struct_t *cli, int16_t delta) {
  if (delta >= 0) {
    cli_move_cursor(cli, cli->cursor + delta);
  } else {
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
static void cli_move_cursor_end(cli_struct_t *cli) {
  cli_move_cursor(cli, cli->input_len);
}

/**
 * @brief Move the logical and display cursor to an absolute position.
 * @param[in] cli  Active CLI session.
 * @param[in] pos  Target position (clamped to [0, input_len]).
 */
static void cli_move_cursor(cli_struct_t *cli, cli_len_t pos) {
  if (pos > cli->input_len) {
    pos = cli->input_len;
  }

  while (cli->cursor < pos) {
    cli_write_char(cli, cli->input[cli->cursor]);
    cli->cursor++;
  }
  while (cli->cursor > pos) {
    cli_write_char(cli, ASCII_BS);
    cli->cursor--;
  }
}

/**
 * @brief Insert a character at the current cursor position, shifting right.
 * @param[in] cli  Active CLI session.
 * @param[in] c    Character to insert.
 */
static void cli_insert_char(cli_struct_t *cli, char c) {
  if (cli->input_len >= (int)CLI_MAX_INPUT_LEN - 1) {
    cli_write_char(cli, '\a');
  } else {
    /* Shift right */
    (void)memmove(&cli->input[cli->cursor + 1], &cli->input[cli->cursor],
                  (size_t)(cli->input_len - cli->cursor));
    cli->input[cli->cursor] = c;
    cli->input_len++;
    cli->input[cli->input_len] = '\0';

    /* Rewrite from cursor to end, then reposition */
    cli_raw_write(cli, (const uint8_t *)&cli->input[cli->cursor], cli->input_len - cli->cursor);
    cli_len_t trail = cli->input_len - cli->cursor - 1;
    for (cli_len_t i = 0; i < trail; i++) {
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
static void cli_delete_char(cli_struct_t *cli) {
  if (cli->cursor < cli->input_len) {
    (void)memmove(&cli->input[cli->cursor], &cli->input[cli->cursor + 1],
                  (size_t)(cli->input_len - cli->cursor - 1));
    cli->input_len--;
    cli->input[cli->input_len] = '\0';

    /* Rewrite tail, append space to erase last char, reposition */
    cli_raw_write(cli, (const uint8_t *)&cli->input[cli->cursor], cli->input_len - cli->cursor);
    cli_write_char(cli, ' ');
    cli_len_t trail = cli->input_len - cli->cursor + 1;
    for (cli_len_t i = 0; i < trail; i++) {
      cli_write_char(cli, ASCII_BS);
    }
    cli_history_capture_edited_preview(cli);
  }
}

/**
 * @brief Delete the character before the cursor (backspace).
 * @param[in] cli  Active CLI session.
 */
static void cli_backspace(cli_struct_t *cli) {
  if (cli->cursor > 0) {
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
static void cli_kill_line(cli_struct_t *cli) {
  /* Erase from cursor to end */
  cli_len_t erase = cli->input_len - cli->cursor;
  for (cli_len_t i = 0; i < erase; i++) {
    cli_write_char(cli, ' ');
  }

  for (cli_len_t i = 0; i < erase; i++) {
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
static void cli_kill_word(cli_struct_t *cli) {
  /* Delete word (run of non-spaces) before cursor, skip leading spaces */
  while (cli->cursor > 0 && cli->input[cli->cursor - 1] == ' ') {
    cli_backspace(cli);
  }

  while (cli->cursor > 0 && cli->input[cli->cursor - 1] != ' ') {
    cli_backspace(cli);
  }
}

/**
 * @brief Clear the entire input buffer and erase the displayed line (Ctrl+U).
 * @param[in] cli  Active CLI session.
 */
static void cli_clear_input(cli_struct_t *cli) {
  cli_move_cursor(cli, cli->input_len);
  while (cli->input_len > 0) {
    cli_backspace(cli);
  }
}

/**
 * @brief Show the prompt for what is asked for now, with what was typed so far.
 *
 * The command prompt, or a credential prompt (a password is never shown); nothing during
 * a lockout. Used at the start of a line: after a command, and after output that was not
 * caused by the user's own line (a @c cli_exec() from a callback).
 *
 * @param[in] cli  Active CLI session.
 */
static void cli_redisplay_input(cli_struct_t *cli) {
  bool editor = True;

#if CLI_ENABLE_AUTH
  editor = !cli_state_is_auth_input(cli);
  if (cli->auth_state == CLI_AUTH_USERNAME) {
    CLI_WSTR(cli, "Username: ");
    cli_raw_write(cli, (const uint8_t *)cli->input, cli->input_len);
  } else if (cli_auth_secret_input(cli)) {
    CLI_WSTR(cli, "Password: ");
  } else {
    /* The command line, or a lockout (no prompt). */
  }
#endif
  if (!editor) {
    /* Credential prompt shown above. */
  } else if (cli->input_len > 0) {
    cli_redraw_line(cli);
  } else {
    cli_show_prompt(cli);
  }
  cli_flush(cli);
}

#if CLI_ENABLE_AUTH
/**
 * @brief Show the prompt after what is asked for changed (a login or password prompt
 *        requested by a command or a callback).
 *
 * Inside a command it is shown once the command has finished, after its output
 * (@c cli_after_command(), @c cli_exec()); outside any command it is shown now, on a
 * fresh line. Before the session runs, the session start shows it.
 *
 * @param[in] cli  Active CLI session.
 */
static void cli_input_prompt_changed(cli_struct_t *cli) {
  if (cli->exec_depth == 0U && cli->session_state == CLI_SESSION_RUN) {
    CLI_WSTR(cli, CLI_CRLF);
    cli_redisplay_input(cli);
  }
}
#endif

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
static void cli_write_str_P(cli_struct_t *cli, const char *s_P) {
  char c;
  while ((c = (char)pgm_read_byte(s_P++)) != '\0') {
    cli_write_char(cli, c);
  }
}

/**
 * @brief Send a PROGMEM string through the command output path.
 *
 * Copies the string from program memory in small chunks so the pipe filter
 * and line routing apply exactly as for RAM strings.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] s_P  NUL-terminated string stored in PROGMEM.
 */
static void cli_print_str_P(cli_struct_t *cli, const char *s_P) {
  char chunk[16];
  int n = 0;
  char c;

  while ((c = (char)pgm_read_byte(s_P++)) != '\0') {
    chunk[n++] = c;
    if (n == (int)sizeof(chunk)) {
      cli_print_process_stream(cli, chunk, n);
      n = 0;
    }
  }
  cli_print_process_stream(cli, chunk, n);
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
static size_t cli_strlcpy_P(char *dst, const char *src_P, size_t size) {
  size_t i = 0;
  if (size > 1) {
    char c;
    while (i < size - 1 && (c = (char)pgm_read_byte(src_P + i)) != '\0') {
      dst[i++] = c;
    }
  }
  if (size > 0) {
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
static void cli_show_prompt(cli_struct_t *cli) {
  if (cli != NULL) {
    /* Large enough for "hostname(mode)# " and for "<submenu name>> ". */
#if CLI_ENABLE_MODE_NAMES
    char prompt[CLI_MAX_PROMPT_LEN + CLI_MAX_MODE_NAME_LEN + CLI_MAX_NAME_LEN + 8];
#else
    char prompt[CLI_MAX_PROMPT_LEN + CLI_MAX_NAME_LEN + 8];
#endif
    size_t pos = 0;
    bool emitted = False;

    if (cli->context != CLI_CMD_ROOT) {
      cli_cmd_handle_t cur = cli->context;
      if (cli_handle_valid(cli, cur)) {
        pos = cli_strlcpy(&prompt[pos], cli->cmd_pool[cur].name, sizeof(prompt));
        prompt[pos++] = '>';
        prompt[pos++] = ' ';
        prompt[pos] = '\0';
        cli_write_str(cli, prompt);
        cli_flush(cli);
        emitted = True;
      }
    }

    if (!emitted) {
      pos = pos + cli_strlcpy(&prompt[pos], cli->hostname, sizeof(prompt) - pos);

#if CLI_ENABLE_MODE_NAMES
      const char *mode_name = cli_mode_name(cli, cli->mode);
      if (mode_name != NULL) {
        prompt[pos++] = '(';
        pos = pos + cli_strlcpy(&prompt[pos], mode_name, sizeof(prompt) - pos);
        prompt[pos++] = ')';
      }
#endif

      if (cli->privilege >= CLI_PRIV_PRIVILEGED) {
        prompt[pos++] = '#';
      } else {
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
 * Entries are stored contiguously in @c history_buf in insertion order; when the
 * new entry does not fit after the newest one it wraps to offset 0. Only the
 * oldest entries whose bytes would be overwritten are evicted, so the history
 * always keeps the most recent commands that fit. Duplicates of the most-recent
 * entry are silently dropped.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] line  Non-empty command line, shorter than @c CLI_MAX_INPUT_LEN: an input line,
 *                  so it fits (@c CLI_HISTORY_BUF_SIZE >= @c CLI_MAX_INPUT_LEN is checked at
 *                  compile time).
 */
static void cli_history_add(cli_struct_t *cli, const char *line) {
  bool add = True;

  if (cli->history_count > 0) {
    cli_history_idx_t prev =
        (cli_history_idx_t)((cli->history_head + CLI_MAX_HISTORY - 1) % CLI_MAX_HISTORY);
    if (strcmp(line, &cli->history_buf[cli->history_off[prev]]) == 0) {
      add = False;
    }
  }

  if (add) {
    cli_history_idx_t needed = (cli_history_idx_t)(strlen(line) + 1U);
    cli_history_idx_t off = (cli_history_idx_t)0;

    if (cli->history_count > 0) {
      cli_history_idx_t prev =
          (cli_history_idx_t)((cli->history_head + CLI_MAX_HISTORY - 1) % CLI_MAX_HISTORY);
      off = (cli_history_idx_t)(cli->history_off[prev] +
                                (int)strlen(&cli->history_buf[cli->history_off[prev]]) + 1);
    }

    if (off + needed > CLI_HISTORY_BUF_SIZE) {
      off = (cli_history_idx_t)0; /* Entries are never split across the buffer end. */
    }

    /* The slot at history_head is reused; when the ring is full it holds the oldest entry. */
    if (cli->history_count == (cli_history_idx_t)CLI_MAX_HISTORY) {
      cli->history_count--;
    }

    /* Evict from the oldest end until the new bytes no longer overlap a live entry. */
    while (cli->history_count > 0 && cli_history_overlaps(cli, off, needed)) {
      cli->history_count--;
    }

    (void)memcpy(&cli->history_buf[off], line, (size_t)needed);
    cli->history_off[cli->history_head] = (cli_history_idx_t)off;

    cli->history_head = (cli_history_idx_t)((cli->history_head + 1) % CLI_MAX_HISTORY);
    //-V::1051
    if (cli->history_count < CLI_MAX_HISTORY) {
      cli->history_count++;
    }
  }
}

/**
 * @brief Test whether the byte range [@p off, @p off + @p len) overlaps any live
 *        history entry.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] off  Start offset in @c history_buf.
 * @param[in] len  Length of the range in bytes.
 *
 * @return @c true if at least one stored entry would be overwritten.
 */
static bool cli_history_overlaps(const cli_struct_t *cli, cli_history_idx_t off,
                                 cli_history_idx_t len) {
  bool overlap = False;

  for (cli_history_idx_t k = 1; k <= cli->history_count && !overlap; k++) {
    cli_history_idx_t idx = cli_history_index(cli, k);
    int start = cli->history_off[idx];
    int end = start + (int)strlen(&cli->history_buf[start]) + 1;
    overlap = ((int)off < end && start < (int)off + (int)len);
  }

  return overlap;
}

/**
 * @brief Compute the ring-buffer index for history entry at @p offset.
 * @param[in] cli     Active CLI session.
 * @param[in] offset  1 = most recent, 2 = second most recent, etc.
 * @return Ring-buffer index into @c cli->history[].
 */
static cli_history_idx_t cli_history_index(const cli_struct_t *cli, cli_history_idx_t offset) {
  /* offset 1 = most recent, 2 = second most recent, ... */
  return (cli_history_idx_t)((cli->history_head - offset + CLI_MAX_HISTORY * 2) % CLI_MAX_HISTORY);
}

/**
 * @brief Erase the current input area without redrawing the prompt.
 * @param[in] cli  Active CLI session.
 */
static void cli_history_clear_displayed_input(cli_struct_t *cli) {
  cli_len_t len = cli->input_len;
  cli_len_t cursor = cli->cursor;

  for (cli_len_t i = 0; i < cursor; i++) {
    cli_write_char(cli, ASCII_BS);
  }

  for (cli_len_t i = 0; i < len; i++) {
    cli_write_char(cli, ' ');
  }

  for (cli_len_t i = 0; i < len; i++) {
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
 * @c history_saved so it can be restored when navigating back down to the tip; editing a
 * recalled entry replaces it (@c cli_history_capture_edited_preview()), so that edit is
 * what returns at the tip.
 * Rendering uses an in-place erase/write sequence so the prompt itself is not
 * redrawn.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] dir  Navigation direction: @c CLI_HISTORY_NAV_PREV or
 *                 @c CLI_HISTORY_NAV_NEXT.
 */
static void cli_history_navigate(cli_struct_t *cli, cli_history_nav_dir_enum_t dir) {
  bool can_navigate = True;

  if (dir == CLI_HISTORY_NAV_PREV) {
#if CLI_HISTORY_RESTORE_PREBROWSE_LINE
    if (cli->history_nav == 0) {
      /* Save current editing line. */
      (void)cli_strlcpy(cli->history_saved, cli->input, CLI_MAX_INPUT_LEN);
    }
#endif
    if (cli->history_nav >= cli->history_count) {
      can_navigate = False;
    } else {
      cli->history_nav++;
    }
  } else {
    if (cli->history_nav == 0) {
      can_navigate = False;
    } else {
      cli->history_nav--;
    }
  }

  if (!can_navigate) {
    cli_write_char(cli, '\a');
  } else {
    cli_history_clear_displayed_input(cli);
    if (cli->history_nav == 0) {
#if CLI_HISTORY_RESTORE_PREBROWSE_LINE
      (void)cli_strlcpy(cli->input, cli->history_saved, CLI_MAX_INPUT_LEN);
#else
      cli->input[0] = '\0';
#endif
    } else {
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
 * @brief Split @p buf in-place into tokens and fill @p argv.
 *
 * Tokens are separated by spaces / tabs. A double-quoted section (@c "a b")
 * is part of the current token and may contain spaces, @c | and @c ?; inside
 * quotes @c \\" and @c \\\\ stand for a literal quote and backslash. Quotes are
 * removed from the stored token, so @c ab"c d"e yields @c "abc de".
 *
 * @param[in,out] buf       Mutable input buffer (unquoted in-place).
 * @param[out]    argv      Receives pointers into @p buf; terminated with NULL.
 * @param[in]     max_args  Maximum entries in @p argv (including sentinel NULL).
 * @param[out]    argc_out  Number of tokens stored, always < @p max_args.
 *
 * @return @c CLI_TOKENIZE_OK, @c CLI_TOKENIZE_TOO_MANY (extra tokens not stored),
 *         or @c CLI_TOKENIZE_UNTERMINATED (the last stored token has an open quote).
 */
static cli_tokenize_status_enum_t cli_tokenize(char *buf, const char *argv[], cli_argc_t max_args,
                                               cli_argc_t *argc_out) {
  cli_tokenize_status_enum_t status = CLI_TOKENIZE_OK;
  cli_argc_t argc = 0;
  char *p = buf;

  while (status == CLI_TOKENIZE_OK) {
    p = cli_skip_spaces(p);
    if (*p == '\0') {
      break;
    }
    if (argc >= max_args - 1) {
      status = CLI_TOKENIZE_TOO_MANY;
      break;
    }

    char *start = p;
    char *w = p; /* Write position; never ahead of p. */
    bool in_quote = False;

    while (*p != '\0' && (in_quote || (*p != ' ' && *p != '\t'))) {
      if (*p == '"') {
        in_quote = !in_quote;
        p++;
      } else if (in_quote && *p == '\\' && (p[1] == '"' || p[1] == '\\')) {
        *w++ = p[1];
        p += 2;
      } else {
        *w++ = *p++;
      }
    }

    char *next = (*p != '\0') ? (p + 1) : p;
    *w = '\0';
    p = next;
    argv[argc++] = start;

    if (in_quote) {
      status = CLI_TOKENIZE_UNTERMINATED;
    }
  }

  argv[argc] = NULL;
  *argc_out = argc;
  return status;
}

/* Command Tree Traversal ----------------------------------------------------------------*/

/**
 * @brief Print-callback that displays a single command's name and help text.
 *
 * Intended for use as a @c cli_match_cb_t with @c cli_foreach_match. Help text
 * starts at column @c CLI_HELP_NAME_COL (at least one space after the name).
 *
 * @param[in]     cli   Active CLI session.
 * @param[in]     h     Command handle of the node to display.
 * @param[in]     text  Matched keyword (unused: the command's name is listed).
 * @param[in,out] raw   @c cli_help_ctx_struct_t: @c any is set; @c as_output selects the
 *                      command-output path (pipe filter applies) instead of a direct
 *                      terminal write.
 *
 * @return Always @c true (continue iteration).
 */
static bool cli_print_help_cb(cli_struct_t *cli, cli_cmd_handle_t h, const char *text, void *raw) {
  cli_help_ctx_struct_t *ctx = (cli_help_ctx_struct_t *)raw;
  const cli_cmd_struct_t *cmd = &cli->cmd_pool[h];
  char line[CLI_HELP_LINE_SIZE];
  size_t pos = 0;

  (void)text;
  ctx->any = True;
  pos = cli_strlcpy(line, "  ", sizeof(line));
  pos = pos + cli_strlcpy(&line[pos], cmd->name, sizeof(line) - pos);
#if CLI_ENABLE_COMMAND_HELP
  if (cmd->help[0] != '\0') {
    do {
      line[pos++] = ' ';
    } while (pos < CLI_HELP_NAME_COL);
    pos = pos + cli_strlcpy(&line[pos], cmd->help, sizeof(line) - pos);
  }
#endif
  line[pos] = '\0';

  if (ctx->as_output) {
    cli_print_process_stream(cli, line, (int)pos);
    cli_print_process_stream(cli, "\n", 1);
  } else {
    cli_write_str(cli, line);
    CLI_WSTR(cli, CLI_CRLF);
  }
  return True;
}

/**
 * @brief Find all commands under @p parent matching @p word (prefix).
 *
 * Scans the command pool and invokes @p cb for each match. When @p word is
 * empty every visible child of @p parent is reported.
 *
 * With @p with_root (first word inside a submenu), root commands follow the
 * submenu's own matches: execution falls back to the root for a first word that
 * matches nothing in the submenu, so a root command is reported when its keyword
 * is not taken over by a submenu command.
 *
 * @param[in] cli        Active CLI session.
 * @param[in] word       Prefix to match, or empty string for all.
 * @param[in] parent     Root of the sub-tree to search.
 * @param[in] with_root  Also report reachable root commands (see above).
 * @param[in] cb         Callback for each match.
 * @param[in] ctx        Passed through to @p cb.
 */
static void cli_foreach_match(cli_struct_t *cli, const char *word, cli_cmd_handle_t parent,
                              bool with_root, cli_match_cb_t cb, void *ctx) {
  bool keep_going = True;
  int passes = (with_root && parent != CLI_CMD_ROOT) ? 2 : 1;

  for (int pass = 0; pass < passes && keep_going; pass++) {
    cli_cmd_handle_t level = (pass == 0) ? parent : CLI_CMD_ROOT;

    for (cli_poolsize_t i = 0; i < cli->cmd_pool_size && keep_going; i++) {
      const cli_cmd_struct_t *cmd = &cli->cmd_pool[i];
      const char *text = NULL;

      if (cli_cmd_selectable(cli, cmd, level) && !CLI_CMD_IS_HIDDEN(cmd)) {
        text = cli_cmd_prefix_text(cmd, word);
      }
      if (text != NULL && pass == 1 && cli_has_any_match(cli, text, parent)) {
        text = NULL; /* typing it would select a submenu command instead */
      }
      if (text != NULL) {
        keep_going = cb(cli, i, text, ctx);
      }
    }
  }
}

/**
 * @brief Test whether @p cmd is a live child of @p parent usable in the current
 *        privilege level and mode (hidden commands included).
 *
 * A container without a handler counts only while one of its subcommands is usable:
 * otherwise it could do nothing and would only reveal that the subcommands exist.
 *
 * @param[in] cli     Active CLI session.
 * @param[in] cmd     Command node (an element of @c cli->cmd_pool).
 * @param[in] parent  Expected parent handle.
 *
 * @return @c true if the command may be matched.
 */
static bool cli_cmd_selectable(const cli_struct_t *cli, const cli_cmd_struct_t *cmd,
                               cli_cmd_handle_t parent) {
  bool allowed = (CLI_CMD_IS_IN_USE(cmd) && cmd->parent == parent &&
                  cmd->privilege <= cli->privilege &&
                  (cmd->mode == CLI_MODE_ANY || cmd->mode == cli->mode));

  /* Recursion follows the parent links downwards, so it ends at the tree's leaves. */
  return allowed && (cmd->callback != NULL ||
                     cli_cmd_has_children(cli, (cli_cmd_handle_t)(cmd - cli->cmd_pool)));
}

/**
 * @brief Return the keyword of @p cmd that @p word matched, ignoring case: the name or
 *        alias equal to @p word, otherwise the first (name, then aliases) starting with it.
 *
 * An exact keyword comes first: "int" typed for a command named "interface" with the
 * alias "int" matched the alias, and completion must not widen it to the name.
 *
 * @param[in] cmd   Command node.
 * @param[in] word  Typed (possibly partial) word; "" matches the name.
 *
 * @return The matching name / alias, or @c NULL when none matches.
 */
static const char *cli_cmd_prefix_text(const cli_cmd_struct_t *cmd, const char *word) {
  const char *text = NULL;

#if CLI_ENABLE_ALIASES
  for (cli_len_t a = 0; a < cmd->num_aliases && text == NULL; a++) {
    if (cli_case_equal(cmd->aliases[a], word)) {
      text = cmd->aliases[a];
    }
  }
#endif
  if (text == NULL && cli_case_prefix_match(cmd->name, word)) {
    text = cmd->name;
  }
#if CLI_ENABLE_ALIASES
  for (cli_len_t a = 0; a < cmd->num_aliases && text == NULL; a++) {
    if (cli_case_prefix_match(cmd->aliases[a], word)) {
      text = cmd->aliases[a];
    }
  }
#endif

  return text;
}

/**
 * @brief Test whether @p word is exactly the name or an alias of @p cmd,
 *        ignoring case.
 *
 * @param[in] cmd   Command node.
 * @param[in] word  Typed word.
 *
 * @return @c true on an exact (case-insensitive) match.
 */
static bool cli_cmd_exact(const cli_cmd_struct_t *cmd, const char *word) {
  bool exact = cli_case_equal(cmd->name, word);

#if CLI_ENABLE_ALIASES
  for (cli_len_t a = 0; a < cmd->num_aliases && !exact; a++) {
    exact = cli_case_equal(cmd->aliases[a], word);
  }
#endif

  return exact;
}

/**
 * @brief Test whether @p name is already the name or an alias of a sibling under
 *        @p parent that can be visible in the same mode.
 *
 * Two commands collide when their modes overlap (equal, or either is
 * @c CLI_MODE_ANY): the second one could never be selected by name.
 *
 * @param[in] cli     Active CLI session.
 * @param[in] parent  Parent of the new name.
 * @param[in] mode    Mode of the new name.
 * @param[in] name    Keyword to check (RAM).
 * @param[in] ignore  Node excluded from the check (the node receiving an alias), or
 *                    @c CLI_CMD_INVALID.
 *
 * @return @c true if the name is taken.
 */
static bool cli_name_taken(const cli_struct_t *cli, cli_cmd_handle_t parent, cli_mode_t mode,
                           const char *name, cli_cmd_handle_t ignore) {
  bool taken = False;

  for (cli_poolsize_t i = 0; i < cli->cmd_pool_size && !taken; i++) {
    const cli_cmd_struct_t *cmd = &cli->cmd_pool[i];
    if (i != ignore && CLI_CMD_IS_IN_USE(cmd) && cmd->parent == parent &&
        (cmd->mode == mode || cmd->mode == CLI_MODE_ANY || mode == CLI_MODE_ANY)) {
      taken = cli_cmd_exact(cmd, name);
    }
  }

  return taken;
}

/**
 * @brief Register a command node (shared by @c cli_add_command and
 *        @c cli_add_command_duplicate).
 *
 * @param[in] cli            Active CLI session.
 * @param[in] parent         Parent handle or @c CLI_CMD_ROOT.
 * @param[in] name           Keyword.
 * @param[in] name_progmem   @p name is in program memory (AVR).
 * @param[in] callback       Handler, or @c NULL for a container node.
 * @param[in] privilege      Required privilege.
 * @param[in] mode           Required mode.
 * @param[in] help           Help text, or @c NULL.
 * @param[in] help_progmem   @p help is in program memory (AVR).
 *
 * @return New handle, or @c CLI_CMD_INVALID.
 */
static cli_cmd_handle_t cli_add_command_impl(cli_struct_t *cli, cli_cmd_handle_t parent,
                                             const char *name, bool name_progmem,
                                             cli_cmd_handler_fn callback, cli_priv_t privilege,
                                             cli_mode_t mode, const char *help,
                                             bool help_progmem) {
  cli_cmd_handle_t handle = CLI_CMD_INVALID;

  if (cli == NULL || cli->cmd_pool == NULL || name == NULL) {
    /* Invalid arguments. */
  } else if (parent != CLI_CMD_ROOT && !cli_handle_valid(cli, parent)) {
    /* Parent must be the root or a live node (e.g. not the result of a failed add). */
  } else if (cli_text_len(name, name_progmem) >= (size_t)CLI_MAX_NAME_LEN) {
    /* A truncated keyword would silently register a different command name. */
  } else if (cli->cmd_pool_count >= cli->cmd_pool_size) {
    (void)cli_println(cli, "ERROR: Command pool full (%d/%d)", cli->cmd_pool_count,
                      cli->cmd_pool_size);
  } else {
    for (cli_cmd_handle_t i = 0; i < cli->cmd_pool_size; i++) {
      cli_cmd_struct_t *cmd = &cli->cmd_pool[i];

      if (!CLI_CMD_IS_IN_USE(cmd)) {
        /* Build the node in the free slot; it only becomes live once it is valid (a slot
         * that is not in use is ignored by cli_name_taken()). */
        (void)memset(cmd, 0, sizeof(*cmd));
        cli_text_copy(cmd->name, name, sizeof(cmd->name), name_progmem);

        /* Reject names the tokenizer can never produce as one word, and duplicates. */
        if (cli_keyword_valid(cmd->name) &&
            !cli_name_taken(cli, parent, mode, cmd->name, CLI_CMD_INVALID)) {
#if CLI_ENABLE_COMMAND_HELP
          if (help != NULL) {
            cli_text_copy(cmd->help, help, sizeof(cmd->help), help_progmem);
          }
#else
          (void)help;
          (void)help_progmem;
#endif
          cmd->callback = callback;
          cmd->privilege = privilege;
          cmd->mode = mode;
          cmd->parent = parent;
          CLI_CMD_SET_IN_USE(cmd);
          cli->cmd_pool_count++;
          handle = i;
        } else {
          (void)memset(cmd, 0, sizeof(*cmd));
        }
        break;
      }
    }
  }

  return handle;
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
                              cli_cmd_handle_t *out) {
  cli_cmd_handle_t first = CLI_CMD_INVALID;
  cli_cmd_handle_t exact = CLI_CMD_INVALID;
  cli_len_t count = 0;
  /* An empty word (a quoted "") names no command: as a prefix it would match them all. */
  cli_poolsize_t scan = (word[0] != '\0') ? cli->cmd_pool_size : 0;

  for (cli_poolsize_t i = 0; i < scan; i++) {
    const cli_cmd_struct_t *cmd = &cli->cmd_pool[i];
    if (!cli_cmd_selectable(cli, cmd, parent)) {
      continue;
    }

    /* An exact (case-insensitive) name always wins; hidden commands match only exactly. */
    if (cli_cmd_exact(cmd, word)) {
      if (exact == CLI_CMD_INVALID) {
        exact = i;
      }
    } else if (!CLI_CMD_IS_HIDDEN(cmd) && cli_cmd_prefix_text(cmd, word) != NULL) {
      count++;
      if (count == 1) {
        first = i;
      }
    } else {
      /* Not a match. */
    }
  }

  int8_t rc;
  if (exact != CLI_CMD_INVALID) {
    *out = exact;
    rc = CLI_OK;
  } else if (count == 0) {
    rc = CLI_ERR;
  } else if (count == 1) {
    *out = first;
    rc = CLI_OK;
  } else {
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
static bool cli_has_any_match(cli_struct_t *cli, const char *word, cli_cmd_handle_t parent) {
  cli_cmd_handle_t unused;
  /* Ambiguous still counts as "matches here": the error is reported against this level. */
  return (cli_resolve_one(cli, word, parent, &unused) != CLI_ERR);
}

/**
 * @brief Test whether @p parent has a direct child the session can use now.
 *
 * Only children allowed at the current privilege and mode count (hidden ones included):
 * a command whose subcommands are all out of reach takes arguments and is no submenu,
 * so it behaves the same as without them and does not reveal they exist.
 *
 * @param[in] cli     Active CLI session.
 * @param[in] parent  Parent command handle.
 *
 * @return @c true when such a child exists, otherwise @c false.
 */
static bool cli_cmd_has_children(const cli_struct_t *cli, cli_cmd_handle_t parent) {
  bool has_children = False;

  for (cli_poolsize_t i = 0; i < cli->cmd_pool_size && !has_children; i++) {
    has_children = cli_cmd_selectable(cli, &cli->cmd_pool[i], parent);
  }

  return has_children;
}

#if CLI_ENABLE_ALIASES
/**
 * @brief Test whether @p parent has any registered child, whatever its privilege or mode.
 *
 * @param[in] cli     Active CLI session.
 * @param[in] parent  Parent command handle.
 *
 * @return @c true when a child exists, otherwise @c false.
 */
static bool cli_cmd_has_registered_children(const cli_struct_t *cli, cli_cmd_handle_t parent) {
  bool found = False;

  for (cli_poolsize_t i = 0; i < cli->cmd_pool_size && !found; i++) {
    found = CLI_CMD_IS_IN_USE(&cli->cmd_pool[i]) && cli->cmd_pool[i].parent == parent;
  }

  return found;
}
#endif

/**
 * @brief Return the level at which the first word of a line is looked up.
 *
 * Inside a submenu the submenu's own commands come first; a word that matches
 * none of them is looked up among the root commands, which stay usable in every
 * submenu. Unknown words are reported against the submenu.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] word  First word of the line.
 *
 * @return Submenu handle or @c CLI_CMD_ROOT.
 */
static cli_cmd_handle_t cli_first_word_parent(cli_struct_t *cli, const char *word) {
  cli_cmd_handle_t menu = cli_menu_context_parent(cli);
  cli_cmd_handle_t parent = menu;

  if (menu != CLI_CMD_ROOT && !cli_has_any_match(cli, word, menu) &&
      cli_has_any_match(cli, word, CLI_CMD_ROOT)) {
    parent = CLI_CMD_ROOT;
  }

  return parent;
}

/**
 * @brief Resolve the complete words of a partly typed line the way execution does
 *        (used by Tab completion and inline @c ? help).
 *
 * Words select subcommands while the current command has subcommands; once a
 * command without subcommands is reached, everything after it is an argument.
 *
 * @param[in]  cli     Active CLI session.
 * @param[in]  argv    Complete words.
 * @param[in]  count   Number of words in @p argv.
 * @param[out] parent  Node whose subcommands may follow the words.
 * @param[out] args    @c true when the words reached a command that takes arguments.
 *
 * @return @c CLI_OK, @c CLI_ERR for an unknown word, or @c CLI_ERR_AMBIG.
 */
static int8_t cli_walk_words(cli_struct_t *cli, const char *argv[], int count,
                             cli_cmd_handle_t *parent, bool *args) {
  int8_t rc = CLI_OK;
  cli_cmd_handle_t level = cli_menu_context_parent(cli);
  bool in_args = False;

  for (int d = 0; d < count && rc == CLI_OK && !in_args; d++) {
    cli_cmd_handle_t h;

    if (d == 0) {
      level = cli_first_word_parent(cli, argv[0]);
    }
    rc = cli_resolve_one(cli, argv[d], level, &h);
    if (rc == CLI_OK) {
      level = h;
      in_args = !cli_cmd_has_children(cli, h);
    }
  }

  *parent = level;
  *args = in_args;
  return rc;
}

/**
 * @brief Return the handle at the top of the context stack, or @c
 * CLI_CMD_INVALID.
 * @param[in] cli  Active CLI session.
 * @return Top-of-stack handle, or @c CLI_CMD_INVALID if the stack is empty.
 */
static cli_cmd_handle_t cli_context_current(const cli_struct_t *cli) {
  return (cli && cli->context != CLI_CMD_ROOT) ? cli->context : CLI_CMD_INVALID;
}

/**
 * @brief Enter the submenu @p node after its command callback succeeded.
 *
 * If the callback already pushed a navigation level (e.g. by switching mode via
 * @c cli_set_mode), that level records where @c exit must return to and is
 * reused; otherwise the current mode and context are pushed first.
 *
 * @param[in] cli           Active CLI session.
 * @param[in] node          Submenu node to enter.
 * @param[in] depth_before  @c nav_depth before the callback ran.
 *
 * @return @c CLI_OK, or @c CLI_ERR_NOMEM when the navigation stack is full.
 */
static int8_t cli_context_enter(cli_struct_t *cli, cli_cmd_handle_t node, uint8_t depth_before) {
  int8_t rc = CLI_OK;

  if (cli->context != node) {
    if (cli->nav_depth <= depth_before) {
      rc = cli_nav_push(cli);
    }
    if (rc == CLI_OK) {
      cli->context = node;
    }
  }

  return rc;
}

/**
 * @brief Return the effective parent handle for command matching.
 *
 * Returns the current submenu node when one is active, otherwise
 * @c CLI_CMD_ROOT. Modes only filter which commands are visible; they never
 * change where matching starts.
 *
 * @param[in] cli  Active CLI session (not @c NULL).
 * @return Effective parent handle for the current menu context.
 */
static cli_cmd_handle_t cli_menu_context_parent(const cli_struct_t *cli) {
  cli_cmd_handle_t cur = cli_context_current(cli);

  return (cur != CLI_CMD_INVALID) ? cur : CLI_CMD_ROOT;
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
static void cli_tab_complete(cli_struct_t *cli) {
  /* With the cursor inside a word, that whole word is completed: the cursor moves to the
   * word's end first (as zsh does), so a completion never splits a word. A word ends where
   * the tokenizer ends it: at a space or an unquoted '|'; a quoted part belongs to it. Inside
   * an open quote there is no word to complete, and the cursor stays. */
  if (!cli_quote_open(cli->input, cli->cursor)) {
    cli_len_t end = cli->cursor;
    bool in_quote = False;
    bool word_end = False;

    while (end < cli->input_len && !word_end) {
      char c = cli->input[end];
      bool separator = (c == ' ' || c == '\t');
#if CLI_ENABLE_PIPE_FILTER
      separator = separator || (c == ASCII_PIPE);
#endif
      if (in_quote && c == '\\' && end + 1 < cli->input_len &&
          (cli->input[end + 1] == '"' || cli->input[end + 1] == '\\')) {
        end += 2; /* escaped character inside quotes */
      } else if (!in_quote && separator) {
        word_end = True;
      } else {
        if (c == '"') {
          in_quote = !in_quote;
        }
        end++;
      }
    }
    cli_move_cursor(cli, end);
  }

  /* Complete the word that ends at the cursor: only the text before it counts. */
  char scratch[CLI_MAX_INPUT_LEN];
  (void)cli_strlcpy(scratch, cli->input, CLI_MAX_INPUT_LEN);
  scratch[cli->cursor] = '\0';

#if CLI_ENABLE_PIPE_FILTER
  char *pipe = cli_find_pipe(scratch);
  if (pipe != NULL) {
    /* After '|': a filter keyword, then its pattern. */
    cli_filter_tab_complete(cli, pipe + 1);
  } else
#endif
  {
    cli_tab_complete_words(cli, scratch);
  }
}

/**
 * @brief Tab completion of command words (no pipe filter before the cursor).
 *
 * @param[in]     cli      Active CLI session.
 * @param[in,out] scratch  The line up to the cursor (tokenized in place).
 */
static void cli_tab_complete_words(cli_struct_t *cli, char *scratch) {
  const char *argv[CLI_MAX_ARGS];
  cli_argc_t argc = 0;
  cli_tokenize_status_enum_t tok = cli_tokenize(scratch, argv, CLI_MAX_ARGS, &argc);
  bool done = False;

  if (tok != CLI_TOKENIZE_OK) {
    /* Inside an open quote (free text) or beyond CLI_MAX_ARGS: nothing to complete. */
    cli_write_char(cli, '\a');
    done = True;
  }

  /* A separator right before the cursor means a new word is being started. */
  bool trailing_space =
      (cli->cursor > 0 && (cli->input[cli->cursor - 1] == ' ' || cli->input[cli->cursor - 1] == '\t'));

  const char *partial = (argc > 0 && !trailing_space) ? argv[argc - 1] : "";
  int depth = (trailing_space) ? argc : ((argc > 0) ? argc - 1 : 0);

  /* Walk the command tree through the complete words, exactly as execution would. */
  cli_cmd_handle_t parent = CLI_CMD_ROOT;
  bool args = False;
  if (!done && (cli_walk_words(cli, argv, depth, &parent, &args) != CLI_OK || args)) {
    /* Unknown word, or the word being typed is an argument: nothing to complete. */
    cli_write_char(cli, '\a');
    done = True;
  }
  bool with_root = (depth == 0);

  if (!done) {
    /* Pass 1: count matches and compute the common prefix of the matched keywords
     * (name or alias — whichever the partial word actually matched). */
    cli_complete_ctx_struct_t cc = {0, NULL, 0};
    cli_foreach_match(cli, partial, parent, with_root, cli_complete_cb, &cc);

    cli_len_t count = cc.count;
    const char *first_text = cc.first_text;
    cli_len_t common = cc.common;
    cli_len_t partial_len = (cli_len_t)strlen(partial);

    /* Characters the completion adds (the same when it rewrites the typed case): it is made
     * in full or not at all, never cut at CLI_MAX_INPUT_LEN. */
    cli_len_t grow = 0;
    if (count == 1) {
      grow = (cli_len_t)strlen(first_text) - partial_len + 1; /* + the space after it */
    } else if (count > 1 && common > partial_len) {
      grow = common - partial_len;
    } else {
      /* Nothing is inserted. */
    }
    bool fits = (grow <= (cli_len_t)CLI_MAX_INPUT_LEN - 1 - cli->input_len);

    /* Keyword characters already typed. A plainly typed word (no quotes) whose case
     * differs from the keyword ("SH" for "show") is rewritten in the keyword's case. */
    cli_len_t keep = partial_len;
    if (fits && count > 0 && (count == 1 || common > partial_len) && cli->cursor >= partial_len &&
        memcmp(&cli->input[cli->cursor - partial_len], partial, (size_t)partial_len) == 0 &&
        strncmp(first_text, partial, (size_t)partial_len) != 0) {
      for (cli_len_t i = 0; i < partial_len; i++) {
        cli_backspace(cli);
      }
      keep = 0;
    }

    if (count == 0 || !fits) {
      cli_write_char(cli, '\a'); /* no match, or the completion does not fit on the line */
    } else if (count == 1) {
      /* Unique match — complete the word and start the next one */
      for (cli_len_t i = keep; first_text[i] != '\0'; i++) {
        cli_insert_char(cli, first_text[i]);
      }
      cli_insert_char(cli, ' ');
    } else if (common > partial_len) {
      /* Complete to the common prefix */
      for (cli_len_t i = keep; i < common; i++) {
        cli_insert_char(cli, first_text[i]);
      }
    } else {
      /* Pass 2: list all candidates */
      cli_help_ctx_struct_t ctx = {False, False};
      CLI_WSTR(cli, CLI_CRLF);
      cli_foreach_match(cli, partial, parent, with_root, cli_print_help_cb, &ctx);
      cli_redraw_line(cli);
    }
  }
}

/**
 * @brief Match callback for Tab completion: counts the matches and narrows the
 *        common prefix of the matched keywords.
 *
 * @param[in]     cli   Active CLI session (unused).
 * @param[in]     h     Matched command (unused).
 * @param[in]     text  Matched keyword.
 * @param[in,out] raw   @c cli_complete_ctx_struct_t.
 *
 * @return Always @c true (continue iteration).
 */
static bool cli_complete_cb(cli_struct_t *cli, cli_cmd_handle_t h, const char *text, void *raw) {
  cli_complete_ctx_struct_t *cc = (cli_complete_ctx_struct_t *)raw;

  (void)cli;
  (void)h;
  cc->count++;
  if (cc->count == 1) {
    cc->first_text = text;
    cc->common = (cli_len_t)strlen(text);
  } else {
    cli_len_t j = 0;
    while (j < cc->common &&
           tolower((int)(unsigned char)cc->first_text[j]) == tolower((int)(unsigned char)text[j])) {
      j++;
    }
    cc->common = j;
  }
  return True;
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
static void cli_show_help(cli_struct_t *cli) {
  /* Help describes the words before the cursor (where '?' was typed). */
  char scratch[CLI_MAX_INPUT_LEN];
  (void)cli_strlcpy(scratch, cli->input, CLI_MAX_INPUT_LEN);
  scratch[cli->cursor] = '\0';

  /* Echo the '?' after the line, so the help request is visible on it without covering the
   * text after the cursor. The help follows on new lines, and the line is then redrawn with
   * the cursor where it was. */
  cli_raw_write(cli, (const uint8_t *)&cli->input[cli->cursor],
                (cli_transport_buflen_t)(cli->input_len - cli->cursor));
  cli_write_char(cli, '?');

#if CLI_ENABLE_PIPE_FILTER
  char *pipe = cli_find_pipe(scratch);
  if (pipe != NULL) {
    /* After '|': the filter keywords, or what the chosen filter takes. */
    CLI_WSTR(cli, CLI_CRLF);
    cli_filter_show_help(cli, pipe + 1);
    cli_redraw_line(cli);
  } else
#endif
  {
    cli_show_help_words(cli, scratch);
  }
}

/**
 * @brief Inline @c ? help for command words (no pipe filter before the cursor).
 *
 * @param[in]     cli      Active CLI session.
 * @param[in,out] scratch  The line up to the cursor (tokenized in place).
 */
static void cli_show_help_words(cli_struct_t *cli, char *scratch) {
  const char *argv[CLI_MAX_ARGS];
  cli_argc_t argc = 0;
  cli_tokenize_status_enum_t tok = cli_tokenize(scratch, argv, CLI_MAX_ARGS, &argc);

  bool trailing_space =
      (cli->cursor > 0 && (cli->input[cli->cursor - 1] == ' ' || cli->input[cli->cursor - 1] == '\t'));
  const char *partial = (argc > 0 && !trailing_space) ? argv[argc - 1] : "";
  int depth = (trailing_space) ? argc : ((argc > 0) ? argc - 1 : 0);
  bool show = True;

  if (tok == CLI_TOKENIZE_TOO_MANY) {
    CLI_WSTR(cli, "\r\n% Too many arguments.\r\n");
    cli_redraw_line(cli);
    show = False;
  }

  cli_cmd_handle_t parent = CLI_CMD_ROOT;
  bool args = False;
  if (show) {
    int8_t r = cli_walk_words(cli, argv, depth, &parent, &args);
    if (r != CLI_OK) {
      if (r == CLI_ERR) {
        CLI_WSTR(cli, "\r\n% Unknown command.\r\n");
      } else {
        CLI_WSTR(cli, "\r\n% Ambiguous command.\r\n");
      }
      cli_redraw_line(cli);
      show = False;
    }
  }

  if (show) {
    /* "word?" lists the commands starting with "word" at this level; "word ?" (with a
     * space) lists what may follow "word" — the partial is "" in that case. Words after a
     * command without subcommands are its arguments: only <cr> can be listed. */
    cli_help_ctx_struct_t ctx = {False, False};
    CLI_WSTR(cli, CLI_CRLF);
    if (!args) {
      cli_foreach_match(cli, partial, parent, depth == 0, cli_print_help_cb, &ctx);
    }
    if (!ctx.any) {
      if (partial[0] != '\0' && !args) {
        CLI_WSTR(cli, "% Unknown command.\r\n");
      } else {
        CLI_WSTR(cli, "  <cr>\r\n"); /* nothing more to type: the command is complete */
      }
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
static void cli_filter_reset(cli_struct_t *cli) {
  cli->filter_type = CLI_FILTER_NONE;
  cli->filter_pattern[0] = '\0';
  cli->filter_count = 0U;
  cli->filter_begin_found = False;
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
static bool cli_filter_line(cli_struct_t *cli, const char *line) {
  bool result = True;

  switch (cli->filter_type) {
  case CLI_FILTER_NONE:
    break;

  case CLI_FILTER_GREP:
    result = (strstr(line, cli->filter_pattern) != NULL);
    break;

  case CLI_FILTER_EXCLUDE:
    result = (strstr(line, cli->filter_pattern) == NULL);
    break;

  case CLI_FILTER_BEGIN:
    if (cli->filter_begin_found) {
    } else if (strstr(line, cli->filter_pattern) != NULL) {
      cli->filter_begin_found = True;
    } else {
      result = False;
    }
    break;

  case CLI_FILTER_COUNT:
    if (cli->filter_count < UINT32_MAX) {
      cli->filter_count++; /* saturates rather than wrapping to a small count */
    }
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
static void cli_filter_flush(cli_struct_t *cli) {
  if (cli->filter_type == CLI_FILTER_COUNT) {
    char buf[32];
#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
    snprintf_P(buf, sizeof(buf), PSTR("Count: %lu"), (unsigned long)cli->filter_count);
#else
    (void)cli_snprintf(buf, sizeof(buf), "Count: %lu", (unsigned long)cli->filter_count);
#endif
    /* The total itself must not pass through the count filter. */
#if CLI_ENABLE_PRINT_TRANSPORT_CB
    if (cli->print_transport_cb != NULL) {
      cli->print_transport_cb(cli, buf);
    } else
#endif
    {
      cli_write_str(cli, buf);
      CLI_WSTR(cli, CLI_CRLF);
    }
  }
}
#endif /* CLI_ENABLE_PIPE_FILTER */

#if CLI_ENABLE_PIPE_FILTER
/* Pipe Filter Parser -------------------------------------------------------------------*/

/**
 * @brief Find the first @c '|' outside double quotes.
 *
 * @param[in] line  Command line.
 *
 * @return Pointer to the pipe character, or @c NULL if the line has no filter.
 */
static char *cli_find_pipe(char *line) {
  char *pipe = NULL;
  bool in_quote = False;

  for (char *p = line; *p != '\0' && pipe == NULL; p++) {
    if (in_quote && *p == '\\' && (p[1] == '"' || p[1] == '\\')) {
      p++;
    } else if (*p == '"') {
      in_quote = !in_quote;
    } else if (!in_quote && *p == ASCII_PIPE) {
      pipe = p;
    } else {
      /* Part of the command. */
    }
  }

  return pipe;
}

/** @brief Pipe filter keyword table (any unambiguous prefix is accepted). */
static const struct {
  const char *keyword;
  cli_filter_enum_t type;
} s_filter_keywords[] = {
    {"grep", CLI_FILTER_GREP},     {"include", CLI_FILTER_GREP}, {"exclude", CLI_FILTER_EXCLUDE},
    {"begin", CLI_FILTER_BEGIN},   {"count", CLI_FILTER_COUNT},
};

/** @brief Number of entries in @c s_filter_keywords. */
#define CLI_FILTER_KEYWORD_COUNT (sizeof(s_filter_keywords) / sizeof(s_filter_keywords[0]))

/**
 * @brief Look up a pipe filter keyword, which may be abbreviated.
 *
 * @param[in]  kw         Keyword as typed.
 * @param[out] ambiguous  Set when @p kw abbreviates keywords of different filters.
 *
 * @return The filter, or @c CLI_FILTER_NONE when no keyword starts with @p kw.
 */
static cli_filter_enum_t cli_filter_lookup(const char *kw, bool *ambiguous) {
  cli_filter_enum_t type = CLI_FILTER_NONE;

  *ambiguous = False;
  for (size_t i = 0; i < CLI_FILTER_KEYWORD_COUNT; i++) {
    if (kw[0] != '\0' && cli_case_prefix_match(s_filter_keywords[i].keyword, kw)) {
      if (type != CLI_FILTER_NONE && type != s_filter_keywords[i].type) {
        *ambiguous = True;
      }
      type = s_filter_keywords[i].type;
    }
  }

  return type;
}

/**
 * @brief Split the filter part of a line being edited (the text after '|' up to the
 *        cursor) for @c ? and Tab.
 *
 * @param[in,out] tail     Text after the '|'; the keyword is terminated in place.
 * @param[out]    kw       Keyword typed so far (may be empty).
 * @param[out]    pattern  Pattern typed so far (empty while the keyword is being typed).
 *
 * @return @c true while the cursor is still on the keyword.
 */
static bool cli_filter_split(char *tail, char **kw, char **pattern) {
  char *kw_end;
  bool at_keyword;

  *kw = cli_skip_spaces(tail);
  kw_end = cli_skip_non_spaces(*kw);
  at_keyword = (*kw_end == '\0');
  *pattern = kw_end;
  if (!at_keyword) {
    *kw_end = '\0';
    *pattern = cli_skip_spaces(kw_end + 1);
  }

  return at_keyword;
}

/**
 * @brief Print one line of filter help: @p name, then the description of @p type in the
 *        help column (@c CLI_FILTER_NONE describes the pattern).
 *
 * @param[in] cli   Active CLI session.
 * @param[in] name  Keyword or placeholder shown.
 * @param[in] type  Filter described.
 */
static void cli_filter_help_line(cli_struct_t *cli, const char *name, cli_filter_enum_t type) {
  size_t col = 2U + strlen(name);

  CLI_WSTR(cli, "  ");
  cli_write_str(cli, name);
  do {
    cli_write_char(cli, ' ');
    col++;
  } while (col < CLI_HELP_NAME_COL);

  switch (type) {
  case CLI_FILTER_GREP:
    CLI_WSTR(cli, "Show the lines that contain the pattern");
    break;
  case CLI_FILTER_EXCLUDE:
    CLI_WSTR(cli, "Hide the lines that contain the pattern");
    break;
  case CLI_FILTER_BEGIN:
    CLI_WSTR(cli, "Show the output from the first line with the pattern");
    break;
  case CLI_FILTER_COUNT:
    CLI_WSTR(cli, "Count the output lines");
    break;
  default:
    CLI_WSTR(cli, "Text to look for (in double quotes to keep spaces)");
    break;
  }
  CLI_WSTR(cli, CLI_CRLF);
}

/**
 * @brief Inline @c ? help after a '|': the filter keywords that match what is typed, or
 *        what the chosen filter takes.
 *
 * @param[in]     cli   Active CLI session.
 * @param[in,out] tail  Text after the '|' up to the cursor.
 */
static void cli_filter_show_help(cli_struct_t *cli, char *tail) {
  char *kw;
  char *pattern;

  if (cli_filter_split(tail, &kw, &pattern)) {
    bool any = False;
    for (size_t i = 0; i < CLI_FILTER_KEYWORD_COUNT; i++) {
      if (cli_case_prefix_match(s_filter_keywords[i].keyword, kw)) {
        cli_filter_help_line(cli, s_filter_keywords[i].keyword, s_filter_keywords[i].type);
        any = True;
      }
    }
    if (!any) {
      CLI_WSTR(cli, "% Invalid filter (use grep, include, exclude, begin or count).\r\n");
    }
  } else {
    bool ambiguous;
    cli_filter_enum_t type = cli_filter_lookup(kw, &ambiguous);

    if (type == CLI_FILTER_NONE || ambiguous) {
      CLI_WSTR(cli, "% Invalid filter (use grep, include, exclude, begin or count).\r\n");
    } else if (type == CLI_FILTER_COUNT) {
      if (pattern[0] != '\0') {
        CLI_WSTR(cli, "% The count filter takes no pattern.\r\n");
      } else {
        CLI_WSTR(cli, "  <cr>\r\n");
      }
    } else if (pattern[0] == '\0') {
      cli_filter_help_line(cli, "<pattern>", CLI_FILTER_NONE);
    } else {
      CLI_WSTR(cli, "  <cr>\r\n");
    }
  }
}

#if CLI_ENABLE_TAB_COMPLETION
/**
 * @brief Tab completion after a '|': completes the filter keyword. The pattern is free
 *        text and is never completed.
 *
 * @param[in]     cli   Active CLI session.
 * @param[in,out] tail  Text after the '|' up to the cursor.
 */
static void cli_filter_tab_complete(cli_struct_t *cli, char *tail) {
  char *kw;
  char *pattern;

  if (!cli_filter_split(tail, &kw, &pattern)) {
    cli_write_char(cli, '\a');
  } else {
    size_t kw_len = strlen(kw);
    size_t count = 0U;
    size_t first = 0U;
    size_t common = 0U;

    for (size_t i = 0; i < CLI_FILTER_KEYWORD_COUNT; i++) {
      const char *name = s_filter_keywords[i].keyword;
      if (cli_case_prefix_match(name, kw)) {
        if (count == 0U) {
          first = i;
          common = strlen(name);
        } else {
          size_t j = 0U;
          while (j < common && name[j] == s_filter_keywords[first].keyword[j]) {
            j++;
          }
          common = j;
        }
        count++;
      }
    }

    /* Characters the completion adds (the same when it rewrites the typed case): made in
     * full or not at all. */
    size_t stop = (count == 1U) ? strlen(s_filter_keywords[first].keyword) : common;
    size_t grow = (count == 1U) ? stop - kw_len + 1U : ((stop > kw_len) ? stop - kw_len : 0U);
    bool fits = (grow <= (size_t)((cli_len_t)CLI_MAX_INPUT_LEN - 1 - cli->input_len));

    if (count == 0U || !fits) {
      cli_write_char(cli, '\a'); /* no match, or the completion does not fit on the line */
    } else if (count == 1U || common > kw_len) {
      const char *name = s_filter_keywords[first].keyword;
      size_t keep = kw_len;

      /* Typed in another case ("INC"): rewritten in the keyword's case, as commands are. */
      if (strncmp(name, kw, kw_len) != 0) {
        for (size_t i = 0; i < kw_len; i++) {
          cli_backspace(cli);
        }
        keep = 0U;
      }
      for (size_t i = keep; i < stop; i++) {
        cli_insert_char(cli, name[i]);
      }
      if (count == 1U) {
        cli_insert_char(cli, ' ');
      }
    } else {
      CLI_WSTR(cli, CLI_CRLF);
      for (size_t i = 0; i < CLI_FILTER_KEYWORD_COUNT; i++) {
        if (cli_case_prefix_match(s_filter_keywords[i].keyword, kw)) {
          cli_filter_help_line(cli, s_filter_keywords[i].keyword, s_filter_keywords[i].type);
        }
      }
      cli_redraw_line(cli);
    }
  }
}
#endif /* CLI_ENABLE_TAB_COMPLETION */

/**
 * @brief Parse and activate a pipe filter from the input line.
 *
 * Locates the first @c '|' outside double quotes, NUL-terminates the command
 * portion, and configures the session filter from the keyword and pattern that
 * follow. The keyword may be abbreviated (@c "| inc x"). The pattern is the rest
 * of the line with surrounding spaces removed; a pattern written as a single
 * double-quoted string keeps its inner spaces and may contain @c |.
 *
 * Errors are reported to the user and leave the filter inactive. The caller
 * resets the filter state beforehand.
 *
 * @param[in]     cli   Active CLI session (filter state updated in-place).
 * @param[in,out] line  Mutable command line string without trailing blanks (modified by
 *                      NUL insertion).
 *
 * @return @c CLI_OK (with or without a filter), or @c CLI_ERR on an invalid filter.
 */
static int8_t cli_parse_filter(cli_struct_t *cli, char *line) {
  int8_t rc = CLI_OK;
  char *pipe = cli_find_pipe(line);

  if (pipe != NULL) {
    bool ambiguous;

    *pipe = '\0'; /* truncate the command at '|' */

    char *kw = cli_skip_spaces(pipe + 1);
    char *kw_end = cli_skip_non_spaces(kw);
    char *pattern = cli_skip_spaces(kw_end);
    *kw_end = '\0';

    cli_filter_enum_t type = cli_filter_lookup(kw, &ambiguous);
    cli_len_t plen = (cli_len_t)strlen(pattern); /* no trailing blanks: the line is trimmed */

    if (type == CLI_FILTER_NONE || ambiguous) {
      CLI_WSTR(cli, "% Invalid filter: ");
      cli_write_str(cli, (kw[0] != '\0') ? kw : "(missing)");
      CLI_WSTR(cli, " (use grep, include, exclude, begin or count)\r\n");
      rc = CLI_ERR;
    } else if (type == CLI_FILTER_COUNT) {
      if (plen > 0) {
        CLI_WSTR(cli, "% The count filter takes no pattern.\r\n");
        rc = CLI_ERR;
      }
    } else if (plen == 0) {
      CLI_WSTR(cli, "% Missing filter pattern.\r\n");
      rc = CLI_ERR;
    } else if (pattern[0] == '"') {
      const char *argv[2];
      cli_argc_t argc = 0;
      if (cli_tokenize(pattern, argv, 2, &argc) != CLI_TOKENIZE_OK || argc != 1 ||
          argv[0][0] == '\0') {
        CLI_WSTR(cli, "% Invalid filter pattern.\r\n");
        rc = CLI_ERR;
      }
      /* On success the tokenizer unquoted the pattern in place, starting at pattern[0]. */
    } else {
      /* Unquoted pattern: the rest of the line, used verbatim. */
    }

    if (rc == CLI_OK) {
      cli->filter_type = type;
      (void)cli_strlcpy(cli->filter_pattern, pattern, CLI_MAX_INPUT_LEN);
    }
  }

  return rc;
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
static int8_t cli_feed(cli_struct_t *cli, int byte) {
  int8_t rc = CLI_ERR;
  bool printable = False;

  if (cli != NULL && cli->session_state != CLI_SESSION_STOP) {
    rc = CLI_OK;

    if (cli_eol_pair(cli, byte)) {
      /* Second byte of a CR LF / LF CR line ending: the line was already submitted (and
       * may have restarted the session, which must stay untouched). */
    } else {
      cli_touch_activity_internal(cli);
      /* Escape / CSI sequences */
      bool esc_processed = cli_process_escape(cli, byte, True);
      if (!esc_processed) {
        /* Control characters */
        switch (byte) {
        /* Enter */
        case ASCII_CR:
          /* fallthrough */
        case ASCII_LF:
          CLI_WSTR(cli, CLI_CRLF);
          cli_after_command(cli, cli_submit_line(cli), &rc);
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
          if (cli->input_len == 0) {
            /* EOF on empty line behaves exactly like the 'quit' command (hook + policy). */
            CLI_WSTR(cli, CLI_CRLF);
            cli_after_command(cli, cli_quit_key(cli), &rc);
          } else {
            cli_delete_char(cli);
          }
          break;

        /* Ctrl+L — clear screen and redraw (a dumb terminal just gets a fresh line) */
        case 0x0C:
          if (cli->ansi_supported) {
            CLI_WSTR(cli, "\x1b"
                          "[2J\x1b"
                          "[H");
          } else {
            CLI_WSTR(cli, CLI_CRLF);
          }
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
          if (byte >= 0x20 && byte <= 0x7E) {
            printable = True;
          }
          break;
        }

        if (printable) {
          if (byte == ASCII_QUESTION && !cli_quote_open(cli->input, cli->cursor)) {
            /* Inline help; the '?' is not part of the line. Inside quotes it is literal. */
            cli_show_help(cli);
          } else {
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
 * @brief Apply the result of a submitted line (or Ctrl-D quit) to the session.
 *
 * Stops the session on @c CLI_ERR_QUIT / @c CLI_ERR_AUTH, otherwise shows the
 * next prompt — unless the line was replayed into the editor, the session is
 * restarting, or the command is waiting for a password.
 *
 * @param[in]     cli      Active CLI session.
 * @param[in]     exec_rc  Result of the executed command.
 * @param[in,out] rc       Feed result; updated when the session must stop.
 */
static void cli_after_command(cli_struct_t *cli, int8_t exec_rc, int8_t *rc) {
  if (exec_rc == CLI_ERR_QUIT || exec_rc == CLI_ERR_AUTH) {
    cli->session_state = CLI_SESSION_STOP;
    *rc = exec_rc;
  } else if (exec_rc == CLI_EXEC_REPLAY_INPUT) {
    /* The recalled line is already redrawn for editing. */
  } else if (cli->session_state == CLI_SESSION_RUN) {
    /* The command prompt, or what the command asked for (Username:, Password:). */
    cli_redisplay_input(cli);
  } else {
    /* Restarting: the banner follows. */
  }
}

/**
 * @brief Ctrl-D on an empty line: run the quit command (hook and policy) as if 'quit'
 *        had been typed, with the same output handling as any command.
 *
 * @param[in] cli  Active CLI session.
 *
 * @return Result of @c cli_cmd_builtin_quit().
 */
static int8_t cli_quit_key(cli_struct_t *cli) {
  bool outermost;
  int8_t rc;

  /* Reserve the (empty) editor line like a typed one, so a cli_exec() made by the quit hook
   * places its line after it. */
  cli->exec_used = cli->input_len + 1;
  outermost = cli_command_begin(cli);
  rc = cli_cmd_builtin_quit(cli, "quit", 0, NULL);
  cli_command_end(cli, outermost);

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
                                  cli_argc_t depth, cli_cmd_handle_t parent) {
  int8_t rc = CLI_ERR;
  bool done = False;

  for (; depth < argc && !done; depth++) {
    cli_cmd_handle_t h;
    int8_t r = cli_resolve_one(cli, argv[depth], parent, &h);

    if (r == CLI_ERR) {
      CLI_WSTR(cli, "% Unknown command: ");
      cli_write_str(cli, argv[depth]);
      CLI_WSTR(cli, CLI_CRLF);
      done = True;
    } else if (r == CLI_ERR_AMBIG) {
      CLI_WSTR(cli, "% Ambiguous command: ");
      cli_write_str(cli, argv[depth]);
      CLI_WSTR(cli, CLI_CRLF);
      rc = CLI_ERR_AMBIG;
      done = True;
    } else {
      // --- Execution Path ---
      const cli_cmd_struct_t *cmd = &cli->cmd_pool[h];
      bool has_children = cli_cmd_has_children(cli, h);

      /* If this node has children and more tokens remain, descend to next level */
      if (has_children && depth + 1 < argc) {
        parent = h;
        /* continue implied by loop increment */
      } else {
        /* Execute this node's callback with remaining tokens as args */
        if (cmd->callback != NULL) {
          cli_argc_t rem_argc = argc - depth;
          uint8_t depth_before = cli->nav_depth;
          cli_session_state_enum_t state_before = cli->session_state;
          rc = cmd->callback(cli, cmd->name, rem_argc, &argv[depth]);

          /* A bare submenu command (no arguments) enters that submenu. Its subcommands are
           * looked up again now: the callback may have switched to the mode (or raised the
           * privilege) they need. The session may have been restarted / stopped by the
           * callback, in which case navigation is left alone. */
          if (rc == CLI_OK && rem_argc == 1 && cli->session_state == state_before &&
              cli_handle_valid(cli, h) && cli_cmd_has_children(cli, h)) {
            if (cli_context_enter(cli, h, depth_before) != CLI_OK) {
              CLI_WSTR(cli, "% Navigation depth limit reached.\r\n");
              rc = CLI_ERR_NOMEM;
            }
          }
        } else {
          CLI_WSTR(cli, "% Incomplete command.\r\n");
        }
        done = True;
      }
    }
  }

  return rc;
}

/**
 * @brief Parse and execute one command line.
 *
 * Trims whitespace, extracts any pipe filter, tokenises, adds interactive lines
 * to the history, and dispatches via @c cli_execute_command(). Command handlers
 * may run further lines through @c cli_exec(); those nested executions share
 * the outer command's output state (pipe filter, open output line), which only
 * the outermost execution sets up and finishes.
 *
 * @param[in]     cli          Active CLI session.
 * @param[in,out] line         Line to run (tokenized in place).
 * @param[in]     interactive  @c true for a line typed by the user (recorded in history).
 *
 * @return @c CLI_OK on success, @c CLI_ERR_QUIT on exit/quit, @c CLI_ERR on
 * failure.
 */
static int8_t cli_execute(cli_struct_t *cli, char *line, bool interactive) {
  int8_t rc = CLI_OK;
  bool outermost = cli_command_begin(cli);
#if !CLI_ENABLE_HISTORY
  (void)interactive; /* only decides whether the line is recorded */
#endif

  /* Trim leading/trailing whitespace */
  char *start = cli_skip_spaces(line);

  cli_len_t slen = (cli_len_t)strlen(start);
  while (slen > 0 && (start[slen - 1] == ' ' || start[slen - 1] == '\t')) {
    start[--slen] = '\0';
  }

  if (slen > 0) {
#if CLI_ENABLE_HISTORY
#if CLI_HISTORY_INDEX_SELECTION
    char history_line[CLI_MAX_INPUT_LEN];
    if (interactive) {
      (void)cli_strlcpy(history_line, start, CLI_MAX_INPUT_LEN);
      cli->history_cleared = False;
    }
#else
    if (interactive) {
      cli_history_add(cli, start);
    }
#endif
#endif

    const char *argv[CLI_MAX_ARGS];
    cli_argc_t argc = 0;

    /* Extract pipe filter before tokenising the command */
#if CLI_ENABLE_PIPE_FILTER
    if (outermost) {
      rc = cli_parse_filter(cli, start);
    } else if (cli_find_pipe(start) != NULL) {
      /* Nested output already flows through the outer command's filter. */
      CLI_WSTR(cli, "% Pipe filters cannot be used in a nested command.\r\n");
      rc = CLI_ERR;
    } else {
      /* Nested command without a filter. */
    }
#endif

    if (rc == CLI_OK) {
      cli_tokenize_status_enum_t tok = cli_tokenize(start, argv, CLI_MAX_ARGS, &argc);
      if (tok == CLI_TOKENIZE_TOO_MANY) {
        CLI_WSTR(cli, "% Too many arguments.\r\n");
        rc = CLI_ERR;
      } else if (tok == CLI_TOKENIZE_UNTERMINATED) {
        CLI_WSTR(cli, "% Unterminated quoted string.\r\n");
        rc = CLI_ERR;
      } else {
        /* Tokens ready. */
      }
    }

    if (rc == CLI_OK && argc > 0) {
      rc = cli_execute_command(cli, argv, argc, 0, cli_first_word_parent(cli, argv[0]));
    }

#if CLI_ENABLE_HISTORY
#if CLI_HISTORY_INDEX_SELECTION
    /* Recorded after it ran (so 'history N' counts the entries before it), unless the
     * command cleared the history: the next login must not see it. */
    if (interactive && rc != CLI_EXEC_REPLAY_INPUT && !cli->history_cleared) {
      cli_history_add(cli, history_line);
    }
#endif
#endif
  }

  cli_command_end(cli, outermost);
  return rc;
}

/**
 * @brief Enter command execution (shared by typed lines, @c cli_exec() and the Ctrl-D quit).
 *
 * Nested executions share the outer command's output state; the outermost one starts it
 * without a pipe filter.
 *
 * @param[in] cli  Active CLI session.
 *
 * @return @c true for the outermost execution, which must pass it to @c cli_command_end().
 */
static bool cli_command_begin(cli_struct_t *cli) {
  bool outermost;

  outermost = (cli->exec_depth == 0U);
#if CLI_HAS_LINE_BUFFER
  if (outermost && cli->filter_line_buf_len > 0) {
    /* A partial line printed outside any command (a callback) is a line of its own, not the
     * start of this command's output or input to its filter. */
    cli_line_buffer_emit(cli);
  }
#endif
  cli->exec_depth++;
#if CLI_ENABLE_PIPE_FILTER
  if (outermost) {
    cli_filter_reset(cli);
  }
#endif

  return outermost;
}

/**
 * @brief Leave command execution entered with @c cli_command_begin().
 *
 * The outermost execution delivers a trailing partial line (e.g. @c cli_print() without
 * '\\n') while the filter is still active, emits a deferred filter result, and makes sure
 * the next prompt starts on a fresh line.
 *
 * @param[in] cli        Active CLI session.
 * @param[in] outermost  Value returned by the matching @c cli_command_begin().
 */
static void cli_command_end(cli_struct_t *cli, bool outermost) {
  if (outermost) {
    cli_output_end_line(cli);
#if CLI_ENABLE_PIPE_FILTER
    cli_filter_flush(cli);
    cli_filter_reset(cli);
#endif
  }

  cli->exec_depth--;
}

/* Authentication -----------------------------------------------------------------------*/

#if CLI_ENABLE_AUTH
/**
 * @brief Clear the in-progress username and password collection buffers.
 * @param[in] cli  Active CLI session.
 */
static void cli_auth_reset_work(cli_struct_t *cli) {
  cli->auth_username[0] = '\0';
  cli_reset_input(cli);
}

/**
 * @brief Reset auth work buffers and print the @c "Username: " prompt.
 * @param[in] cli  Active CLI session.
 */
static void cli_auth_prompt_username(cli_struct_t *cli) {
  cli_auth_reset_work(cli);
  cli->auth_state = CLI_AUTH_USERNAME;
  CLI_WSTR(cli, "Username: ");
  cli_flush(cli);
}

/**
 * @brief Test whether the current lockout is timed but has no clock to time it.
 * @param[in] cli  Active CLI session.
 * @return @c true when the lockout could never expire.
 */
static bool cli_auth_lockout_unending(const cli_struct_t *cli) {
#if CLI_ENABLE_TIME_SOURCE
  return (cli->auth_lockout_seconds != 0UL && cli->time_source == NULL);
#else
  return (cli->auth_lockout_seconds != 0UL);
#endif
}

/**
 * @brief Test whether the current auth lockout period has expired.
 * @param[in] cli  Active CLI session.
 * @return @c true if the lockout has expired or is not active.
 */
static bool cli_auth_lockout_expired(const cli_struct_t *cli) {
  bool expired = True;

  if (cli != NULL && cli->auth_lockout_seconds != 0UL) {
#if CLI_ENABLE_TIME_SOURCE
    /* A timed lockout only exists with a clock (cli_auth_limit_reached(); the engine closes
     * the session when the clock is removed during one). */
    if (cli->time_source != NULL) {
      uint32_t now = cli->time_source(cli->time_source_ctx);
      uint32_t elapsed = now - cli->auth_lockout_started_ts;

      expired = (elapsed >= cli->auth_lockout_seconds);
    }
#endif
  }

  return expired;
}

/**
 * @brief Leave the lockout state once it has expired.
 *
 * A login lockout returns to the @c "Username:" prompt; an enable-secret lockout
 * returns the still-authenticated user to the command prompt.
 *
 * @param[in] cli  Active CLI session.
 */
static void cli_auth_lockout_end(cli_struct_t *cli) {
  cli->auth_lockout_started_ts = 0UL;
  cli_idle_period_restart(cli); /* the idle timer was paused during the lockout */

  if (cli->auth_resume_state == CLI_AUTH_USERNAME) {
    cli->auth_attempts = 0;
    cli_auth_prompt_username(cli); /* a new login: forgets the previous username too */
  } else {
    /* Still logged in: the username stays (the enable callback receives it). */
    cli_reset_input(cli);
    cli->enable_attempts = 0;
    cli->auth_state = cli->auth_resume_state;
    cli_show_prompt(cli);
  }
}

/**
 * @brief Apply @c auth_failure_mode after too many failed login / enable attempts.
 *
 * @param[in] cli           Active CLI session.
 * @param[in] resume_state  State to restore when a lockout expires
 *                          (@c CLI_AUTH_USERNAME = log in again).
 *
 * @return @c CLI_OK when locked out, @c CLI_ERR_AUTH when the session is closed.
 */
static int8_t cli_auth_limit_reached(cli_struct_t *cli, cli_auth_state_enum_t resume_state) {
  int8_t rc = CLI_ERR_AUTH;
  bool can_lock = (cli->auth_failure_mode == CLI_AUTH_FAILURE_MODE_LOCKOUT);

#if CLI_ENABLE_TIME_SOURCE
  /* Without a clock a timed lockout could never expire: close the session instead. */
  can_lock = can_lock && (cli->auth_lockout_seconds == 0UL || cli->time_source != NULL);
#else
  can_lock = can_lock && (cli->auth_lockout_seconds == 0UL);
#endif

  if (can_lock) {
    CLI_WSTR(cli, "% Too many failed attempts. Lock-Out Session.\r\n");
    cli->auth_state = CLI_AUTH_LOCKOUT;
    cli->auth_resume_state = resume_state;
#if CLI_ENABLE_TIME_SOURCE
    if (cli->time_source != NULL) {
      cli->auth_lockout_started_ts = cli->time_source(cli->time_source_ctx);
    }
#endif
    rc = CLI_OK;
  } else {
    CLI_WSTR(cli, "% Too many failed attempts. Closing Session.\r\n");
    cli->session_state = CLI_SESSION_STOP;
  }

  return rc;
}

/**
 * @brief Verify collected credentials and transition auth state.
 *
 * Calls @c authorization_cb or searches the local user table.  On success, marks the
 * session authenticated; privileged users start in @c CLI_MODE_ENABLE, which is
 * then the login floor.  On failure, increments the attempt counter and
 * re-prompts or applies the failure mode.
 *
 * @param[in] cli  Active CLI session.
 *
 * @return @c CLI_OK to continue, @c CLI_ERR_AUTH on permanent failure.
 */
static int8_t cli_auth_verify(cli_struct_t *cli) {
  int8_t ok = CLI_ERR_AUTH;
  int8_t rc = CLI_OK;

  /* Every login starts from a clean, unprivileged state. */
  cli_nav_reset(cli);
  cli_set_privilege(cli, CLI_PRIV_USER);

  if (cli->authorization_cb != NULL) {
    ok = cli_auth_run_callback(cli, cli->authorization_cb);
  } else {
    /* Every entry is checked with constant-time comparisons and without stopping at a
     * match, so response time does not reveal which usernames exist or how much of a
     * password was right. */
    cli_len_t match = -1;
    for (cli_len_t i = 0; i < cli->num_users; i++) {
      bool user_ok = cli_secret_equal(cli->users[i], cli->auth_username, CLI_MAX_USERNAME_LEN);
      bool pass_ok = cli_secret_equal(cli->passwords[i], cli->input, CLI_MAX_PASSWORD_LEN);
      if (user_ok && pass_ok && match < 0) {
        match = i;
      }
    }
    if (match >= 0) {
      ok = CLI_OK;
      cli_set_privilege(cli, cli->user_privileges[match]);
    }
  }

  if (cli->session_state != CLI_SESSION_RUN) {
    /* The callback restarted or stopped the session: this attempt belongs to the old one. */
  } else if (cli->auth_state != CLI_AUTH_PASSWORD) {
    /* The callback asked for a new login (cli_request_auth()). That replaces this attempt;
     * its prompt is shown now, after the callback. */
    cli_redisplay_input(cli);
  } else if (ok == CLI_OK) {
    cli->auth_state = CLI_AUTH_AUTHENTICATED;
    cli->auth_attempts = 0;
    if (cli->privilege >= CLI_PRIV_PRIVILEGED) {
      /* Privileged exec is this login's floor: no level to 'exit' back to. */
      cli->mode = CLI_MODE_ENABLE;
      cli_notify_mode_change(cli);
    }
    rc = CLI_OK;
  } else {
    cli_set_privilege(cli, CLI_PRIV_USER); /* Never keep what a rejecting callback granted. */
    cli->auth_attempts++;
    CLI_WSTR(cli, "% Authentication failed.\r\n");

    if (cli->auth_attempts >= CLI_AUTH_MAX_ATTEMPTS) {
      rc = cli_auth_limit_reached(cli, CLI_AUTH_USERNAME);
    } else {
      cli_auth_prompt_username(cli);
      rc = CLI_OK;
    }
  }

  return rc;
}

static bool cli_auth_secret_input(const cli_struct_t *cli) {
  return cli &&
         (cli->auth_state == CLI_AUTH_PASSWORD || cli->auth_state == CLI_AUTH_ENABLE_MODE_PASSWORD);
}

/**
 * @brief Number of characters the current credential prompt accepts.
 *
 * One more than the longest valid entry: an entry typed too long keeps its extra
 * character and therefore never matches, instead of being cut down to a valid one.
 *
 * @param[in] cli  Active CLI session.
 *
 * @return Maximum input length (below @c CLI_MAX_INPUT_LEN, see the config check).
 */
static cli_len_t cli_auth_input_limit(const cli_struct_t *cli) {
  cli_len_t limit = CLI_MAX_INPUT_LEN - 1;

  if (cli != NULL) {
    if (cli->auth_state == CLI_AUTH_USERNAME) {
      limit = CLI_MAX_USERNAME_LEN;
    } else if (cli->auth_state == CLI_AUTH_PASSWORD) {
      limit = CLI_MAX_PASSWORD_LEN;
    } else if (cli->auth_state == CLI_AUTH_ENABLE_MODE_PASSWORD) {
      limit = CLI_MAX_ENABLE_SECRET_LEN;
    } else {
      /* To silence MISRA */
    }
  }

  return limit;
}

static int8_t cli_auth_submit_line(cli_struct_t *cli) {
  int8_t rc = CLI_OK;

  if (cli->auth_state == CLI_AUTH_USERNAME) {
    (void)cli_strlcpy(cli->auth_username, cli->input, sizeof(cli->auth_username));
    cli_reset_input(cli);
    cli->auth_state = CLI_AUTH_PASSWORD;
    CLI_WSTR(cli, "Password: ");
    cli_flush(cli);

  } else if (cli->auth_state == CLI_AUTH_PASSWORD) {
    rc = cli_auth_verify(cli);
    cli_wipe_input(cli); /* do not leave the password in memory */

  } else if (cli->auth_state == CLI_AUTH_ENABLE_MODE_PASSWORD) {
    int8_t ok;

    if (cli->enable_privilege_cb != NULL) {
      ok = cli_auth_run_callback(cli, cli->enable_privilege_cb);
    } else {
      ok = cli_secret_equal(cli->input, cli->enable_privilege_secret, CLI_MAX_ENABLE_SECRET_LEN)
               ? CLI_OK
               : CLI_ERR_AUTH;
    }
    cli_wipe_input(cli); /* do not leave the secret in memory */

    if (cli->session_state != CLI_SESSION_RUN) {
      /* The callback restarted or stopped the session: nothing of this attempt is shown. */
    } else if (cli->auth_state != CLI_AUTH_ENABLE_MODE_PASSWORD) {
      /* The callback asked for something else instead (cli_request_auth(): a new login).
       * That replaces this attempt; its prompt is shown now, after the callback. */
      cli_redisplay_input(cli);
    } else {
      cli->auth_state = cli->auth_resume_state;
      cli_auth_enable_result(cli, ok, &rc);
    }
  } else {
    /* To silence MISRA */
  }

  return rc;
}

/**
 * @brief Run a credential check callback (login or enable secret) like a command.
 *
 * Output it prints is finished properly, and a prompt change it causes
 * (@c cli_request_auth()) is shown after it rather than in the middle. The typed secret
 * occupies the editor's line, so a @c cli_exec() it makes is placed after it.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] fn   Callback; receives the username and the typed secret.
 *
 * @return The callback's result.
 */
static int8_t cli_auth_run_callback(cli_struct_t *cli, cli_auth_fn fn) {
  bool outermost;
  int8_t ok;

  cli->exec_used = cli->input_len + 1;
  outermost = cli_command_begin(cli);
  ok = fn(cli, cli->auth_username, cli->input);
  cli_command_end(cli, outermost);

  return ok;
}

/**
 * @brief Apply the result of an enable-secret check: privileged exec, or a failed attempt.
 *
 * @param[in]     cli  Active CLI session.
 * @param[in]     ok   @c CLI_OK when the secret was accepted.
 * @param[in,out] rc   Set to @c CLI_ERR_AUTH when the attempt limit closes the session.
 */
static void cli_auth_enable_result(cli_struct_t *cli, int8_t ok, int8_t *rc) {
  if (ok == CLI_OK) {
    cli->enable_attempts = 0;
    /* Enter the mode before raising privilege so 'exit' returns to the unprivileged level. */
    if (cli_set_mode(cli, CLI_MODE_ENABLE) == CLI_OK) {
      cli_set_privilege(cli, CLI_PRIV_PRIVILEGED);
    } else {
      CLI_WSTR(cli, "% Navigation depth limit reached.\r\n");
    }
  } else {
    CLI_WSTR(cli, "% Access denied.\r\n");
    cli->enable_attempts++;
    if (cli->enable_attempts >= CLI_AUTH_MAX_ATTEMPTS) {
      *rc = cli_auth_limit_reached(cli, cli->auth_resume_state);
    }
  }
}

static int8_t cli_auth_handle(cli_struct_t *cli, int byte) {
  int8_t rc = CLI_OK;
  bool done = False;

  if (cli_eol_pair(cli, byte)) {
    done = True; /* second byte of a CR LF / LF CR line ending */
  } else {
    cli_touch_activity_internal(cli);
    /* Swallow escape sequences without acting on them: cursor keys, Delete and history
     * recall must not echo the hidden password or reveal previous commands. */
    done = cli_process_escape(cli, byte, False);
  }

  if (!done) {
    switch (byte) {
    case ASCII_CR:
    case ASCII_LF: {
      int8_t exec_rc;

      CLI_WSTR(cli, CLI_CRLF);
      exec_rc = cli_auth_submit_line(cli);

      if (exec_rc == CLI_ERR_AUTH) {
        cli->session_state = CLI_SESSION_STOP;
        rc = CLI_ERR_AUTH;
      } else if (cli->session_state != CLI_SESSION_RUN) {
        /* A callback restarted or stopped the session: the banner (or nothing) follows. */
      } else if (cli->auth_state == CLI_AUTH_NONE || cli->auth_state == CLI_AUTH_AUTHENTICATED) {
        cli_show_prompt(cli);
      } else {
        /* To silence MISRA */
      }
    } break;

    case ASCII_BS:
    case ASCII_DEL:
      cli_auth_backspace(cli);
      break;

    case 0x0B:
    case 0x15:
      cli_auth_clear_input(cli);
      break;

    default:
      if (byte >= 0x20 && byte <= 0x7E) {
        cli_auth_insert_char(cli, (char)byte);
      }
      break;
    }
  }

  return rc;
}
#endif

/**
 * @brief Recognise the second byte of a two-byte line ending.
 *
 * A line is submitted by the first CR or LF. If the very next byte is the other
 * one (CR LF or LF CR, as sent by different terminals), it belongs to the same
 * line ending and must not submit an empty line. Two identical bytes (CR CR,
 * LF LF) are two separate line endings.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] byte  Received byte.
 *
 * @return @c true if @p byte completes the previous line ending and is to be ignored.
 */
static bool cli_eol_pair(cli_struct_t *cli, int byte) {
  bool is_eol = (byte == ASCII_CR || byte == ASCII_LF);
  bool pair = (is_eol && cli->eol_prev != 0U && byte != (int)cli->eol_prev);

  cli->eol_prev = (is_eol && !pair) ? (uint8_t)byte : 0U;
  return pair;
}

/**
 * @brief Empty the line editor.
 *
 * While commands execute, the head of @c input[] holds the line(s) they run and their
 * @c argv strings point into it, so the bytes are left alone then: the editor only
 * forgets the line. The executing line is cleared once it has finished
 * (@c cli_submit_line(), @c cli_exec_area_run()).
 *
 * A partly typed password or enable secret is erased, not just forgotten (a restart,
 * a new login or Ctrl-U while it is typed). It always occupies @c input[0..input_len):
 * lines run by @c cli_exec() are placed after it.
 *
 * @param[in] cli  Active CLI session.
 */
static void cli_reset_input(cli_struct_t *cli) {
  if (cli != NULL) {
#if CLI_ENABLE_AUTH
    if (cli_auth_secret_input(cli)) {
      volatile char *p = cli->input;
      for (cli_len_t i = 0; i < cli->input_len; i++) {
        p[i] = '\0';
      }
    }
#endif
    if (cli->exec_depth == 0U) {
      cli->input[0] = '\0';
    }
    cli->input_len = 0;
    cli->cursor = 0;
  }
}

/**
 * @brief Return every per-session state to its initial value (shared by
 *        @c cli_start() and @c cli_restart_session()).
 *
 * Configuration (commands, users, callbacks, policies) is kept.
 *
 * @param[in] cli  Active CLI session.
 */
static void cli_session_reset(cli_struct_t *cli) {
  cli_nav_reset(cli);
  cli_set_privilege(cli, CLI_PRIV_USER);
  cli->esc_state = ESC_STATE_NORMAL;
  cli->esc_param = 0;
  cli_reset_input(cli);
#if CLI_ENABLE_IDLE_TIMEOUT
  cli->idle_armed = False; /* nothing to reset until the user types */
#endif
#if CLI_ENABLE_HISTORY
  /* A restarted session may belong to a different user: do not expose old commands. */
  (void)cli_clear_history(cli);
#if CLI_HISTORY_INDEX_SELECTION
  cli->history_replay = (cli_history_idx_t)-1;
#endif
#endif
  if (cli->exec_depth == 0U) {
    /* While a command runs (it restarted the session), the output state is its own:
     * cli_command_end() delivers the rest, applies the filter and resets it. */
#if CLI_ENABLE_PIPE_FILTER
    cli_filter_reset(cli);
#endif
#if CLI_HAS_LINE_BUFFER
    cli->filter_line_buf_len = 0;
#endif
    cli->out_open = False;
  }
#if CLI_ENABLE_AUTH
  cli->auth_state = CLI_AUTH_NONE;
  cli->auth_attempts = 0;
  cli->enable_attempts = 0;
  cli->auth_lockout_started_ts = 0UL;
  cli_auth_reset_work(cli);
#endif
}

/**
 * @brief Test whether input bytes currently belong to a login / password prompt
 *        (or a lockout) rather than the command line editor.
 *
 * @param[in] cli  Active CLI session.
 *
 * @return @c true while credentials are being collected or input is locked out.
 */
static bool cli_state_is_auth_input(const cli_struct_t *cli) {
#if CLI_ENABLE_AUTH
  return (cli != NULL) &&
         (cli->auth_state == CLI_AUTH_USERNAME || cli->auth_state == CLI_AUTH_PASSWORD ||
          cli->auth_state == CLI_AUTH_ENABLE_MODE_PASSWORD || cli->auth_state == CLI_AUTH_LOCKOUT);
#else
  (void)cli;
  return False;
#endif
}

/* Navigation (mode / submenu context) --------------------------------------------------*/

/**
 * @brief Test whether @p h refers to a live command node of @p cli.
 *
 * Rejects @c CLI_CMD_INVALID, @c CLI_CMD_ROOT and every other negative value,
 * out-of-range indices, and unused slots.
 *
 * @param[in] cli  CLI session (may be @c NULL).
 * @param[in] h    Handle to check.
 *
 * @return @c true when @c cli->cmd_pool[h] is an in-use node.
 */
static bool cli_handle_valid(const cli_struct_t *cli, cli_cmd_handle_t h) {
  return (cli != NULL && cli->cmd_pool != NULL && h >= 0 && h < cli->cmd_pool_size &&
          CLI_CMD_IS_IN_USE(&cli->cmd_pool[h]));
}

/**
 * @brief Invoke the mode / privilege change callback, if one is registered.
 * @param[in] cli  Active CLI session.
 */
static void cli_notify_mode_change(cli_struct_t *cli) {
  if (cli->on_mode_change != NULL) {
    cli->on_mode_change(cli, cli->mode, cli->privilege);
  }
}

/**
 * @brief Save the current mode and submenu context as a navigation level.
 *
 * @param[in] cli  Active CLI session.
 *
 * @return @c CLI_OK, or @c CLI_ERR_NOMEM when @c CLI_MAX_CONTEXT_DEPTH levels
 *         are already saved.
 */
static int8_t cli_nav_push(cli_struct_t *cli) {
  int8_t rc = CLI_ERR_NOMEM;

  if (cli->nav_depth < (uint8_t)CLI_MAX_CONTEXT_DEPTH) {
    cli->nav_stack[cli->nav_depth].mode = cli->mode;
    cli->nav_stack[cli->nav_depth].context = cli->context;
    cli->nav_stack[cli->nav_depth].privilege = cli->privilege;
    cli->nav_depth++;
    rc = CLI_OK;
  }

  return rc;
}

/**
 * @brief Return to the saved navigation level @p level, discarding it and every
 *        level above it.
 *
 * Mode, submenu context and privilege are restored from the level's frame; the
 * mode-change callback fires once if mode or privilege changed.
 *
 * @param[in] cli    Active CLI session.
 * @param[in] level  Index of the frame to return to (< @c nav_depth).
 */
static void cli_nav_return(cli_struct_t *cli, uint8_t level) {
  const cli_nav_frame_struct_t *frame = &cli->nav_stack[level];
  bool changed = (frame->mode != cli->mode || frame->privilege != cli->privilege);

  cli->nav_depth = level;
  cli->mode = frame->mode;
  cli->context = frame->context;
  cli->privilege = frame->privilege;
  if (changed) {
    cli_notify_mode_change(cli);
  }
}

/**
 * @brief Drop all saved navigation levels and return to user exec at the root.
 * @param[in] cli  Active CLI session.
 */
static void cli_nav_reset(cli_struct_t *cli) {
  cli_mode_t old_mode = cli->mode;

  cli->nav_depth = 0U;
  cli->context = CLI_CMD_ROOT;
  cli->mode = CLI_MODE_EXEC;
  if (cli->mode != old_mode) {
    cli_notify_mode_change(cli);
  }
}

/**
 * @brief Re-point the current context and every saved level that is @p removed, or a
 *        submenu below it, to @p replacement (used when a command node is deleted).
 *
 * @param[in] cli          Active CLI session.
 * @param[in] removed      Handle that is about to become invalid.
 * @param[in] replacement  Handle (or @c CLI_CMD_ROOT) to use instead.
 */
static void cli_nav_relocate(cli_struct_t *cli, cli_cmd_handle_t removed,
                             cli_cmd_handle_t replacement) {
  bool moved = False;

  if (cli_is_within(cli, cli->context, removed)) {
    cli->context = replacement;
    moved = True;
  }
  for (uint8_t i = 0U; i < cli->nav_depth; i++) {
    if (cli_is_within(cli, cli->nav_stack[i].context, removed)) {
      cli->nav_stack[i].context = replacement;
      moved = True;
    }
  }

  if (moved) {
    /* A level now identical to the level above it (or to the current state) records a
     * submenu that no longer exists: 'exit' would leave it without changing anything.
     * Such levels are dropped. */
    uint8_t kept = 0U;
    for (uint8_t i = 0U; i < cli->nav_depth; i++) {
      const cli_nav_frame_struct_t *f = &cli->nav_stack[i];
      bool last = (i + 1U == cli->nav_depth);
      cli_mode_t above_mode = last ? cli->mode : cli->nav_stack[i + 1U].mode;
      cli_cmd_handle_t above_context = last ? cli->context : cli->nav_stack[i + 1U].context;
      cli_priv_t above_privilege = last ? cli->privilege : cli->nav_stack[i + 1U].privilege;

      if (f->mode != above_mode || f->context != above_context ||
          f->privilege != above_privilege) {
        cli->nav_stack[kept] = *f;
        kept++;
      }
    }
    cli->nav_depth = kept;
  }
}

/**
 * @brief Test whether @p node is @p ancestor or lies in its subtree.
 *
 * @param[in] cli       Active CLI session.
 * @param[in] node      Node to test (may be @c CLI_CMD_ROOT).
 * @param[in] ancestor  Subtree root.
 *
 * @return @c true if following parent links from @p node reaches @p ancestor.
 */
static bool cli_is_within(const cli_struct_t *cli, cli_cmd_handle_t node,
                          cli_cmd_handle_t ancestor) {
  bool within = False;

  /* Each step moves one level up; a tree cannot be deeper than the pool. */
  for (cli_poolsize_t steps = 0; steps <= cli->cmd_pool_size && !within; steps++) {
    if (node == ancestor) {
      within = True;
    } else if (cli_handle_valid(cli, node)) {
      node = cli->cmd_pool[node].parent;
    } else {
      break; /* reached the root or a detached node */
    }
  }

  return within;
}

#if CLI_ENABLE_MODE_NAMES
/**
 * @brief Look up the display name registered for @p mode.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] mode  Mode value.
 *
 * @return The non-empty name, or @c NULL if the mode has none.
 */
static const char *cli_mode_name(const cli_struct_t *cli, cli_mode_t mode) {
  const char *name = NULL;

  for (int i = 0; i < (int)CLI_MAX_MODES && name == NULL; i++) {
    if (cli->mode_name_ids[i] == mode && mode != CLI_MODE_ANY && cli->mode_names[i][0] != '\0') {
      name = cli->mode_names[i];
    }
  }

  return name;
}
#endif

static int8_t cli_submit_line(cli_struct_t *cli) {
  int8_t rc = CLI_ERR;

  if (cli != NULL) {
#if CLI_ENABLE_HISTORY
    /* Browsing ends with every submitted line, also one that is not stored (empty, or the
     * same as the newest entry): the next Up starts again from the newest entry. */
    cli->history_nav = (cli_history_idx_t)0;
#if CLI_HISTORY_RESTORE_PREBROWSE_LINE
    cli->history_saved[0] = '\0';
#endif
#endif
    /* The typed line is executed in place; reserve it so cli_exec() calls made by the
     * command place their lines after it. */
    cli->exec_used = cli->input_len + 1;
    rc = cli_execute(cli, cli->input, True);
#if CLI_ENABLE_HISTORY && CLI_HISTORY_INDEX_SELECTION
    if (rc == CLI_EXEC_REPLAY_INPUT && cli_history_replay_load(cli)) {
      /* The recalled line is now in the editor, redrawn for editing. */
    } else
#endif
    {
      if (rc == CLI_EXEC_REPLAY_INPUT) {
        rc = CLI_OK;
      }
      cli_reset_input(cli);
    }
  }

  return rc;
}

#if CLI_ENABLE_HISTORY && CLI_HISTORY_INDEX_SELECTION
/**
 * @brief Load the entry selected by @c history N into the line editor.
 *
 * @param[in] cli  Active CLI session.
 *
 * @return @c true if an entry was pending and has been loaded (and redrawn).
 */
static bool cli_history_replay_load(cli_struct_t *cli) {
  bool loaded = False;

  if (cli->history_replay >= 0) {
    (void)cli_strlcpy(cli->input, &cli->history_buf[cli->history_off[cli->history_replay]],
                      CLI_MAX_INPUT_LEN);
    cli->input_len = (cli_len_t)strlen(cli->input);
    cli->cursor = cli->input_len;
    cli->history_replay = (cli_history_idx_t)-1;
    cli_redraw_line(cli);
    loaded = True;
  }

  return loaded;
}
#endif

#if CLI_ENABLE_AUTH
static void cli_auth_backspace(cli_struct_t *cli) {
  if (cli != NULL && cli->input_len > 0) {
    cli->input_len--;
    cli->input[cli->input_len] = '\0';
    cli->cursor = cli->input_len;
    if (!cli_auth_secret_input(cli)) {
      CLI_WSTR(cli, "\x08"
                    " \x08");
    }
  }
}

static void cli_auth_clear_input(cli_struct_t *cli) {
  if (cli != NULL) {
    if (!cli_auth_secret_input(cli)) {
      while (cli->input_len > 0) {
        cli_auth_backspace(cli);
      }
    } else {
      cli_reset_input(cli);
    }
  }
}

static void cli_auth_insert_char(cli_struct_t *cli, char c) {
  if (cli != NULL) {
    if (cli->input_len < cli_auth_input_limit(cli)) {
      cli->input[cli->input_len++] = c;
      cli->input[cli->input_len] = '\0';
      cli->cursor = cli->input_len;
      if (!cli_auth_secret_input(cli)) {
        cli_write_char(cli, c);
      }
    } else {
      cli_write_char(cli, ASCII_BELL);
    }
  }
}
#endif

/* Escape-Sequence State Machine --------------------------------------------------------*/

/**
 * @brief Feed one byte into the VT100 / ANSI escape-sequence state machine.
 *
 * Recognises @c ESC [ (CSI) and @c ESC O (SS3) sequences following the
 * ECMA-48 byte classes: parameter bytes 0x30-0x3F, intermediate bytes
 * 0x20-0x2F, final byte 0x40-0x7E. Only the first numeric parameter is kept;
 * extra parameters such as the @c ;5 modifier of Ctrl+Arrow are consumed and
 * ignored. Sequences with private markers (@c < @c = @c > @c ?) or
 * intermediate bytes are consumed without action. A control byte inside a
 * sequence aborts it and is processed normally.
 *
 * For example: Up Arrow sends: 0x1B (ESC), then [, then A often read as \033[A
 *
 * @param[in] cli       Active CLI session.
 * @param[in] b         Received byte.
 * @param[in] dispatch  @c true to act on completed key sequences; @c false to
 *                      only consume them (used while collecting credentials).
 *
 * @return @c true if the byte was consumed by the escape machine.
 */
static bool cli_process_escape(cli_struct_t *cli, int b, bool dispatch) {
  bool consumed = True;
  bool is_param = (b >= 0x30 && b <= 0x3F);
  bool is_inter = (b >= 0x20 && b <= 0x2F);
  bool is_final = (b >= 0x40 && b <= 0x7E);

  switch (cli->esc_state) {
  case ESC_STATE_NORMAL:
    if (b == ASCII_ESC) {
      cli->esc_state = ESC_STATE_ESC;
      cli->esc_param = 0;
    } else {
      consumed = False; /* not an escape byte */
    }
    break;

  case ESC_STATE_ESC:
    if (b == ASCII_LSBRACKET) {
      cli->esc_state = ESC_STATE_CSI;
    } else if (b == ASCII_CAP_O) {
      cli->esc_state = ESC_STATE_SS3;
    } else if (b == ASCII_ESC) {
      /* Repeated ESC: start over. */
    } else {
      /* ESC + other key (e.g. Alt+key): drop the ESC, process the key normally. */
      cli->esc_state = ESC_STATE_NORMAL;
      consumed = False;
    }
    break;

  case ESC_STATE_CSI:
  case ESC_STATE_CSI_REST:
  case ESC_STATE_CSI_IGNORE:
    if (b >= ASCII_DIGIT_0 && b <= ASCII_DIGIT_9 && cli->esc_state == ESC_STATE_CSI) {
      int digit = b - ASCII_DIGIT_0;
      /* Saturate before multiplying: 9999 * 10 does not fit a 16-bit int (AVR). */
      if (cli->esc_param <= (ESC_PARAM_MAX - digit) / 10) {
        cli->esc_param = cli->esc_param * 10 + digit;
      } else {
        cli->esc_param = ESC_PARAM_MAX;
      }
    } else if (is_param) {
      if (b == ';' || b == ':') {
        if (cli->esc_state == ESC_STATE_CSI) {
          cli->esc_state = ESC_STATE_CSI_REST; /* further parameters are ignored */
        }
      } else if (b >= '<') {
        cli->esc_state = ESC_STATE_CSI_IGNORE; /* private marker: not a key we know */
      } else {
        /* Digits of an ignored parameter. */
      }
    } else if (is_inter) {
      cli->esc_state = ESC_STATE_CSI_IGNORE;
    } else if (is_final) {
      if (dispatch && cli->esc_state != ESC_STATE_CSI_IGNORE) {
        handle_csi(cli, (char)b);
      }
      cli->esc_state = ESC_STATE_NORMAL;
      cli->esc_param = 0;
    } else if (b == ASCII_ESC) {
      cli->esc_state = ESC_STATE_ESC; /* Abort and start a new sequence. */
      cli->esc_param = 0;
    } else {
      /* Control byte inside the sequence: abort it and let the caller handle the byte. */
      cli->esc_state = ESC_STATE_NORMAL;
      cli->esc_param = 0;
      consumed = False;
    }
    break;

  case ESC_STATE_SS3:
    cli->esc_param = 0;
    if (is_final) {
      if (dispatch) {
        handle_csi(cli, (char)b);
      }
      cli->esc_state = ESC_STATE_NORMAL;
    } else if (b == ASCII_ESC) {
      cli->esc_state = ESC_STATE_ESC; /* Abort and start a new sequence. */
    } else {
      cli->esc_state = ESC_STATE_NORMAL;
      /* Control bytes are processed normally; a malformed printable byte is dropped. */
      consumed = (b >= 0x20 && b < ASCII_DEL);
    }
    break;

  default:
    cli->esc_state = ESC_STATE_NORMAL;
    consumed = False;
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
static void handle_csi(cli_struct_t *cli, char final) {
  switch (final) {
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
    if (cli->esc_param == 1 || cli->esc_param == 7) {
      cli_move_cursor(cli, 0);
    } else if (cli->esc_param == 3) {
      cli_delete_char(cli);
    } else if (cli->esc_param == 4 || cli->esc_param == 8) {
      cli_move_cursor_end(cli);
    } else {
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
