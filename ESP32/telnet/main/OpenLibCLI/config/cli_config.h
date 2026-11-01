/**
 * @file cli_config.h
 * @brief Compile-time capacity and feature constants for OpenLibCLI.
 *
 * All tunable macros are guarded with @c #ifndef so they can be overridden
 * on the compiler command line or by editing this header before it is included.
 *
 * Quick reference (desktop defaults):
 *   Capacity: CLI_MAX_ARGS 32, CLI_MAX_NAME_LEN 64,
 *             CLI_MAX_HELP_LEN 128, CLI_MAX_ALIASES 2
 *   Input:    CLI_MAX_INPUT_LEN 512, CLI_MAX_HISTORY 10
 *   Session:  CLI_MAX_CONTEXT_DEPTH 16, CLI_MAX_MODES 16, CLI_MAX_MODE_NAME_LEN 32
 *   Auth:     CLI_MAX_USERS 8, CLI_MAX_USERNAME_LEN 32, CLI_MAX_PASSWORD_LEN 64,
 *             CLI_MAX_ENABLE_SECRET_LEN 64
 *   Display:  CLI_MAX_PROMPT_LEN 64, CLI_MAX_BANNER_LEN 512, CLI_MAX_OUTPUT_BUF 1024
 *   Timing:   CLI_TRANSPORT_POLL_MS 1000, CLI_DEFAULT_IDLE_TIMEOUT_SEC 10
 *   Features: CLI_ENABLE_HISTORY 1, TAB_COMPLETION 1, COMMAND_HELP 1, BANNER 1,
 *             AUTH 1, MODE_NAMES 1, PIPE_FILTER 1, IDLE_TIMEOUT 1,
 *             PERIODIC_CALLBACK 1, TIMMING_STATS 1, ALIASES 1
 *
 * @copyright Copyright (c) 2026 Muhammad Hassaan Shah.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENLIBCLI_CONFIG_H
#define OPENLIBCLI_CONFIG_H

/* C++ detection */
#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup CLI_Config Compile-time Configuration
 *  @brief Override any of these macros before including cli.h to tune the
 *         library for your target.
 *  @{
 */

/*=======================================================================================
 *  Command / Capacity
 *=======================================================================================*/

#ifndef CLI_MAX_ARGS
#define CLI_MAX_ARGS 32 /**< Token slots per line: up to CLI_MAX_ARGS - 1 tokens (min 2). */
#endif

#ifndef CLI_MAX_NAME_LEN
#define CLI_MAX_NAME_LEN 64 /**< Command keyword buffer incl. NUL (built-ins need >= 10). */
#endif

#ifndef CLI_MAX_HELP_LEN
#define CLI_MAX_HELP_LEN 128 /**< Maximum length of a command help string. */
#endif

#ifndef CLI_MAX_ALIASES
#define CLI_MAX_ALIASES 2 /**< Alias names stored inline per command node. */
#endif

/*=======================================================================================
 *  Input / History
 *=======================================================================================*/

#ifndef CLI_MAX_INPUT_LEN
#define CLI_MAX_INPUT_LEN 512 /**< Maximum characters per input line (including NUL). */
#endif

#ifndef CLI_ENABLE_HISTORY
#define CLI_ENABLE_HISTORY 1 /**< Compile in command history: 1 = enabled, 0 = omitted. */
#endif

#ifndef CLI_HISTORY_INDEX_SELECTION
/**< Compile in index based selection from history table: 1 = enabled, 0 = omitted.*/
#define CLI_HISTORY_INDEX_SELECTION 0
#endif

#ifndef CLI_MAX_HISTORY
#define CLI_MAX_HISTORY 10 /**< Command history ring-buffer depth (entries). Max=255 */
#endif

#ifndef CLI_HISTORY_BUF_SIZE
#define CLI_HISTORY_BUF_SIZE 2048 /**< Flat history buffer size (bytes). */
#endif

#ifndef CLI_HISTORY_RESTORE_PREBROWSE_LINE
/**< When history browsing returns to the tip, restore the pre-browse edit line:
       1 = enabled, 0 = clear the line instead. */
#define CLI_HISTORY_RESTORE_PREBROWSE_LINE 1
#endif

#ifndef CLI_ENABLE_TAB_COMPLETION
#define CLI_ENABLE_TAB_COMPLETION 1 /**< Compile in tab-completion: 1 = enabled, 0 = omitted. */
#endif

/*=======================================================================================
 *  Session / Lifecycle
 *=======================================================================================*/

#ifndef CLI_MAX_CONTEXT_DEPTH
#define CLI_MAX_CONTEXT_DEPTH 16 /**< Maximum command context / submenu stack depth. */
#endif

#ifndef CLI_MAX_MODES
#define CLI_MAX_MODES 16 /**< Maximum distinct CLI modes (including built-in modes). */
#endif

#ifndef CLI_MAX_MODE_NAME_LEN
#define CLI_MAX_MODE_NAME_LEN 32 /**< Maximum mode name string length (including NUL). */
#endif

#ifndef CLI_ENABLE_MODE_NAMES
#define CLI_ENABLE_MODE_NAMES 1 /**< Display mode name in prompt suffix. */
#endif

#ifndef CLI_ENABLE_IDLE_TIMEOUT
#define CLI_ENABLE_IDLE_TIMEOUT 1 /**< Compile in idle-timeout state and checks. */
#endif

#ifndef CLI_ENABLE_PERIODIC_CALLBACK
#define CLI_ENABLE_PERIODIC_CALLBACK 1 /**< Compile in periodic callback state and checks. */
#endif

/*=======================================================================================
 *  Auth / Security
 *=======================================================================================*/

#ifndef CLI_MAX_USERS
#define CLI_MAX_USERS 8 /**< Local authentication table entries. */
#endif

#ifndef CLI_MAX_USERNAME_LEN
#define CLI_MAX_USERNAME_LEN 32 /**< Maximum username string length (including NUL). */
#endif

#ifndef CLI_MAX_PASSWORD_LEN
#define CLI_MAX_PASSWORD_LEN 64 /**< Maximum password string length (including NUL). */
#endif

#ifndef CLI_MAX_ENABLE_SECRET_LEN
#define CLI_MAX_ENABLE_SECRET_LEN 64 /**< Maximum enable secret string length (including NUL). */
#endif

#ifndef CLI_ENABLE_AUTH
#define CLI_ENABLE_AUTH 1 /**< Compile in username/password auth and enable-secret prompts. */
#endif

/*=======================================================================================
 *  Display / Output
 *=======================================================================================*/

#ifndef CLI_MAX_PROMPT_LEN
#define CLI_MAX_PROMPT_LEN 64 /**< Maximum hostname / prompt string length. */
#endif

#ifndef CLI_MAX_BANNER_LEN
#define CLI_MAX_BANNER_LEN 512 /**< Maximum MOTD / banner string length. */
#endif

#ifndef CLI_ENABLE_BANNER
#define CLI_ENABLE_BANNER 1 /**< Store and emit a per-session MOTD banner. */
#endif

#ifndef CLI_MAX_OUTPUT_BUF
#define CLI_MAX_OUTPUT_BUF 1024 /**< Per-line output buffer used by the pipe filter. */
#endif

#ifndef CLI_ENABLE_PIPE_FILTER
/**< Compile in IOS-style pipe filters (grep/exclude/begin/count):
       1 = enabled, 0 = omitted (saves filter struct fields + logic). */
#define CLI_ENABLE_PIPE_FILTER 1
#endif

#ifndef CLI_ENABLE_PRINT_TRANSPORT_CB
/**< Compile in cli_set_print_transport_cb() (per-line output callback). Needs the
       CLI_MAX_OUTPUT_BUF line buffer even when pipe filters are omitted. */
#define CLI_ENABLE_PRINT_TRANSPORT_CB 1
#endif

#ifndef CLI_MAX_EXEC_DEPTH
/**< Maximum commands executing at once (a command handler calling cli_exec() counts as a
       second level). Guards against unbounded recursion; costs no memory. */
#define CLI_MAX_EXEC_DEPTH 4
#endif

#ifndef CLI_MAX_RX_BYTES_PER_TICK
/**< Maximum input bytes one cli_session_engine() call processes. Bytes beyond it stay in the
       transport for the next call, so a continuous input stream cannot keep the engine from
       returning to the caller's loop. Costs no memory. */
#define CLI_MAX_RX_BYTES_PER_TICK 1024
#endif

/*=======================================================================================
 *  Transport / Timing
 *=======================================================================================*/

#ifndef CLI_TRANSPORT_POLL_MS
/** Transport poll interval in milliseconds used as the blocking read timeout for WIN/POSIX socket
 *  transports; not applicable to Arduino. Arduino Client (and WiFiClient) uses available(). */
#define CLI_TRANSPORT_POLL_MS 1000
#endif

#ifndef CLI_DEFAULT_IDLE_TIMEOUT_SEC
#define CLI_DEFAULT_IDLE_TIMEOUT_SEC 10 /**< Default idle timeout in seconds; 0 disables it. */
#endif

#ifndef CLI_ENABLE_TIMMING_STATS
/**< Enable tick-timing statistics: 0x01 = enabled, 0x00 = disabled. */
#define CLI_ENABLE_TIMMING_STATS 1
#endif

/*=======================================================================================
 *  Feature Flags — computed
 *=======================================================================================*/

#ifndef CLI_ENABLE_COMMAND_HELP
#define CLI_ENABLE_COMMAND_HELP 1 /**< Store per-command help strings for @c ?. */
#endif

#ifndef CLI_ENABLE_ALIASES
#define CLI_ENABLE_ALIASES 1 /**< Command alias support: 1 = enabled, 0 = omitted. */
#endif

#ifndef CLI_ENABLE_TIME_SOURCE
#define CLI_ENABLE_TIME_SOURCE (CLI_ENABLE_IDLE_TIMEOUT || CLI_ENABLE_PERIODIC_CALLBACK)
#endif

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* OPENLIBCLI_CONFIG_H */

/*################################### END OF FILE ######################################*/
