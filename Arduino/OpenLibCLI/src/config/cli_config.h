/**
 * @file cli_arduino_config.h
 * @brief Embedded-specific compile-time capacity profile for OpenLibCLI.
 *
 * Two profiles are provided:
 *
 *  - **AVR profile** (@c __AVR__ defined): Ultra-small defaults targeting the
 *    UNO / Nano (≤ 2 KB SRAM).  History and tab-completion are enabled with
 *    compact command metadata.
 *  - **Non-AVR Embedded profile**: Richer defaults for STM32, ESP32, RP2040,
 *    etc.  History and tab-completion are enabled.
 *
 * All macros are guarded with @c #ifndef so any value can be overridden on
 * the compiler command line.
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

/** @defgroup CLI_EmbeddedConfig Embedded Capacity Profiles
 *  @brief Memory-constrained defaults for Embedded targets.
 *  @{
 */

/*=======================================================================================
 *  AVR Profile  (ATmega328P: UNO / Nano — ≤ 2 KB SRAM)
 *=======================================================================================*/

#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)

/*-----------------------------------------------------------------------------
 *  Command / Capacity
 *-----------------------------------------------------------------------------*/

#ifndef CLI_MAX_ARGS
#define CLI_MAX_ARGS 4 /**< AVR: max tokenised arguments. */
#endif
#ifndef CLI_MAX_NAME_LEN
#define CLI_MAX_NAME_LEN 8 /**< AVR: max command keyword length. */
#endif
#ifndef CLI_MAX_HELP_LEN
#define CLI_MAX_HELP_LEN 10 /**< AVR: max help string length. */
#endif
#ifndef CLI_MAX_ALIASES
#define CLI_MAX_ALIASES 0 /**< AVR: alias slots per node. */
#endif

/*-----------------------------------------------------------------------------
 *  Input / History
 *-----------------------------------------------------------------------------*/

#ifndef CLI_MAX_INPUT_LEN
#define CLI_MAX_INPUT_LEN 48 /**< AVR: max input line length (bytes). */
#endif

#ifndef CLI_ENABLE_HISTORY
#define CLI_ENABLE_HISTORY 1 /**< AVR: history enabled. */
#endif

#ifndef CLI_HISTORY_INDEX_SELECTION
/**< Compile in index based selection from history table: 1 = enabled, 0 = omitted.*/
#define CLI_HISTORY_INDEX_SELECTION 0
#endif

#ifndef CLI_MAX_HISTORY
#define CLI_MAX_HISTORY 3 /**< AVR: history ring depth. */
#endif

#ifndef CLI_HISTORY_BUF_SIZE
#define CLI_HISTORY_BUF_SIZE 48 /**< AVR: flat history buffer size (bytes). */
#endif

#ifndef CLI_HISTORY_RESTORE_PREBROWSE_LINE
/**< AVR: do not preserve the pre-browse edit line. */
#define CLI_HISTORY_RESTORE_PREBROWSE_LINE 0
#endif

#ifndef CLI_ENABLE_TAB_COMPLETION
#define CLI_ENABLE_TAB_COMPLETION 1 /**< AVR: tab completion enabled. */
#endif

/*-----------------------------------------------------------------------------
 *  Session / Lifecycle
 *-----------------------------------------------------------------------------*/

#ifndef CLI_MAX_CONTEXT_DEPTH
#define CLI_MAX_CONTEXT_DEPTH 2 /**< AVR: context / submenu stack depth. */
#endif
#ifndef CLI_MAX_MODES
#define CLI_MAX_MODES 3 /**< AVR: distinct CLI modes. */
#endif
#ifndef CLI_MAX_MODE_NAME_LEN
#define CLI_MAX_MODE_NAME_LEN 4 /**< AVR: max mode name length. */
#endif
#ifndef CLI_ENABLE_MODE_NAMES
#define CLI_ENABLE_MODE_NAMES 0 /**< AVR: mode-name storage disabled to save 12B SRAM. */
#endif
#ifndef CLI_ENABLE_IDLE_TIMEOUT
#define CLI_ENABLE_IDLE_TIMEOUT 0 /**< AVR: idle timeout disabled to save SRAM. */
#endif
#ifndef CLI_ENABLE_PERIODIC_CALLBACK
#define CLI_ENABLE_PERIODIC_CALLBACK 0 /**< AVR: periodic callback disabled to save SRAM. */
#endif

/*-----------------------------------------------------------------------------
 *  Auth / Security
 *-----------------------------------------------------------------------------*/

#ifndef CLI_MAX_USERS
#define CLI_MAX_USERS 1 /**< AVR: local auth table entries. */
#endif
#ifndef CLI_MAX_USERNAME_LEN
#define CLI_MAX_USERNAME_LEN 6 /**< AVR: max username length. */
#endif
#ifndef CLI_MAX_PASSWORD_LEN
#define CLI_MAX_PASSWORD_LEN 6 /**< AVR: max password length. */
#endif
#ifndef CLI_MAX_ENABLE_SECRET_LEN
#define CLI_MAX_ENABLE_SECRET_LEN 6 /**< AVR: max enable secret length. */
#endif
#ifndef CLI_ENABLE_AUTH
#define CLI_ENABLE_AUTH 0 /**< AVR: omit username/password auth state from SRAM. */
#endif

/*-----------------------------------------------------------------------------
 *  Display / Output
 *-----------------------------------------------------------------------------*/

#ifndef CLI_MAX_PROMPT_LEN
#define CLI_MAX_PROMPT_LEN 8 /**< AVR: max hostname / prompt length. */
#endif
#ifndef CLI_MAX_BANNER_LEN
#define CLI_MAX_BANNER_LEN 16 /**< AVR: max MOTD / banner length. */
#endif
#ifndef CLI_ENABLE_BANNER
#define CLI_ENABLE_BANNER 0 /**< AVR: omit MOTD banner storage from SRAM. */
#endif
#ifndef CLI_MAX_OUTPUT_BUF
#define CLI_MAX_OUTPUT_BUF 48 /**< AVR: per-line output buffer (bytes). */
#endif

/*-----------------------------------------------------------------------------
 *  Transport / Timing
 *-----------------------------------------------------------------------------*/

#define CLI_ENABLE_TIMMING_STATS 0 /**< AVR: tick-timing stats disabled to save SRAM. */

/*-----------------------------------------------------------------------------
 *  Feature Flags — computed
 *-----------------------------------------------------------------------------*/

#ifndef CLI_ENABLE_COMMAND_HELP
#define CLI_ENABLE_COMMAND_HELP 0 /**< AVR: omit per-command help strings from SRAM. */
#endif

#ifndef CLI_ENABLE_ALIASES
#define CLI_ENABLE_ALIASES 0 /**< AVR: alias support disabled to save RAM. */
#endif

/*=======================================================================================
 *  Non-AVR Embedded Profile  (STM32, ESP32, RP2040, …)
 *=======================================================================================*/

#else

/*-----------------------------------------------------------------------------
 *  Command / Capacity
 *-----------------------------------------------------------------------------*/

#ifndef CLI_MAX_ARGS
#define CLI_MAX_ARGS 12 /**< max tokenised arguments. */
#endif
#ifndef CLI_MAX_NAME_LEN
#define CLI_MAX_NAME_LEN 20 /**< max command keyword length. */
#endif
#ifndef CLI_MAX_HELP_LEN
#define CLI_MAX_HELP_LEN 64 /**< max help string length. */
#endif
#ifndef CLI_MAX_ALIASES
#define CLI_MAX_ALIASES 2 /**< alias slots per node. */
#endif

/*-----------------------------------------------------------------------------
 *  Input / History
 *-----------------------------------------------------------------------------*/

#ifndef CLI_MAX_INPUT_LEN
#define CLI_MAX_INPUT_LEN 96 /**< max input line length (bytes). */
#endif

#ifndef CLI_ENABLE_HISTORY
#define CLI_ENABLE_HISTORY 1 /**< history enabled. */
#endif

#ifndef CLI_HISTORY_INDEX_SELECTION
/**< Compile in index based selection from history table: 1 = enabled, 0 = omitted.*/
#define CLI_HISTORY_INDEX_SELECTION 0
#endif

#ifndef CLI_MAX_HISTORY
#define CLI_MAX_HISTORY 5 /**< history ring depth. */
#endif

#ifndef CLI_HISTORY_BUF_SIZE
#define CLI_HISTORY_BUF_SIZE 128 /**< flat history buffer size (bytes). */
#endif

#ifndef CLI_HISTORY_RESTORE_PREBROWSE_LINE
/**< restore the pre-browse edit line when history returns to the tip. */
#define CLI_HISTORY_RESTORE_PREBROWSE_LINE 1
#endif

#ifndef CLI_ENABLE_TAB_COMPLETION
#define CLI_ENABLE_TAB_COMPLETION 1 /**< tab completion enabled. */
#endif

/*-----------------------------------------------------------------------------
 *  Session / Lifecycle
 *-----------------------------------------------------------------------------*/

#ifndef CLI_MAX_CONTEXT_DEPTH
#define CLI_MAX_CONTEXT_DEPTH 8 /**< context / submenu stack depth. */
#endif
#ifndef CLI_MAX_MODES
#define CLI_MAX_MODES 12 /**< distinct CLI modes. */
#endif
#ifndef CLI_MAX_MODE_NAME_LEN
#define CLI_MAX_MODE_NAME_LEN 16 /**< max mode name length. */
#endif
#ifndef CLI_ENABLE_MODE_NAMES
#define CLI_ENABLE_MODE_NAMES 1 /**< mode-name storage enabled. */
#endif
#ifndef CLI_ENABLE_IDLE_TIMEOUT
#define CLI_ENABLE_IDLE_TIMEOUT 1 /**< idle timeout enabled. */
#endif
#ifndef CLI_ENABLE_PERIODIC_CALLBACK
#define CLI_ENABLE_PERIODIC_CALLBACK 1 /**< periodic callback enabled. */
#endif

/*-----------------------------------------------------------------------------
 *  Auth / Security
 *-----------------------------------------------------------------------------*/

#ifndef CLI_MAX_USERS
#define CLI_MAX_USERS 2 /**< local auth table entries. */
#endif
#ifndef CLI_MAX_USERNAME_LEN
#define CLI_MAX_USERNAME_LEN 16 /**< max username length. */
#endif
#ifndef CLI_MAX_PASSWORD_LEN
#define CLI_MAX_PASSWORD_LEN 16 /**< max password length. */
#endif
#ifndef CLI_MAX_ENABLE_SECRET_LEN
#define CLI_MAX_ENABLE_SECRET_LEN 16 /**< max enable secret length. */
#endif
#ifndef CLI_ENABLE_AUTH
#define CLI_ENABLE_AUTH 1 /**< username/password auth enabled. */
#endif

/*-----------------------------------------------------------------------------
 *  Display / Output
 *-----------------------------------------------------------------------------*/

#ifndef CLI_MAX_PROMPT_LEN
#define CLI_MAX_PROMPT_LEN 24 /**< max hostname / prompt length. */
#endif
#ifndef CLI_MAX_BANNER_LEN
#define CLI_MAX_BANNER_LEN 256 /**< max MOTD / banner length. */
#endif
#ifndef CLI_ENABLE_BANNER
#define CLI_ENABLE_BANNER 1 /**< store MOTD banner text. */
#endif
#ifndef CLI_MAX_OUTPUT_BUF
#define CLI_MAX_OUTPUT_BUF 160 /**< per-line output buffer (bytes). */
#endif

/*-----------------------------------------------------------------------------
 *  Transport / Timing
 *-----------------------------------------------------------------------------*/

#define CLI_ENABLE_TIMMING_STATS 0 /**< tick-timing stats disabled. */

/*-----------------------------------------------------------------------------
 *  Feature Flags — computed
 *-----------------------------------------------------------------------------*/

#ifndef CLI_ENABLE_COMMAND_HELP
#define CLI_ENABLE_COMMAND_HELP 1 /**< store command help strings. */
#endif

#ifndef CLI_ENABLE_ALIASES
#define CLI_ENABLE_ALIASES 0 /**< alias support (0 = disabled). */
#endif

#endif /* Non-AVR Embedded profile */

/*=======================================================================================
 *  Shared Embedded Settings
 *=======================================================================================*/

#ifndef CLI_DEFAULT_IDLE_TIMEOUT_SEC
#define CLI_DEFAULT_IDLE_TIMEOUT_SEC 0 /**< Default idle timeout in seconds; 0 disables it. */
#endif

#ifndef CLI_TRANSPORT_POLL_MS
/** Transport poll interval in milliseconds used as the blocking read timeout for WIN/POSIX socket
 *  transports; not applicable to Arduino. Arduino Client (and WiFiClient) uses available(). */
#define CLI_TRANSPORT_POLL_MS 1000
#endif

#ifndef CLI_ENABLE_PIPE_FILTER
#define CLI_ENABLE_PIPE_FILTER 0 /**< Pipe filters disabled on Embedded to save SRAM. */
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
