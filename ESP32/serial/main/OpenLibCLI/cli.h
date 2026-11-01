/**
 * @file cli.h
 * @brief Public API for the OpenLibCLI Library.
 *
 * A command-line interface library written in **pure C99** with **100 % static memory allocation**.
 * Runs on Windows, Linux/macOS, and MCUs (AVR, ARM, RISC-V) with any byte-stream transport (Telnet,
 * TCP, serial, pipes, UNIX sockets, or custom).
 *
 * **Features**
 *  - Hierarchical command tree with privilege levels and modes
 *  - Tab-completion and inline @c ? help
 *  - Command history with arrow-key navigation
 *  - Pipe filtering ( @c |grep  @c |begin  @c |exclude  @c |count )
 *  - Login authentication (local table or custom callback)
 *  - Banner / MOTD
 *  - Transport-agnostic: plug in Telnet, raw TCP, UART, pipe, â€¦
 *  - Session lifecycle (@c cli_session_engine) modes
 *  - Single-threaded safe; no global mutable state between sessions
 *
 * **Quick-start**
 * @code
 *   #define CLI_MAX_COMMANDS 64
 *
 *   static cli_struct_t      s_cli;
 *   static cli_cmd_struct_t  s_cmd_pool[CLI_MAX_COMMANDS];
 *
 *   cli_cmd_handle_t h_show =
 *       cli_add_command(&s_cli, CLI_CMD_ROOT,
 *                        "show", NULL,
 *                        CLI_PRIV_UNPRIVILEGED,
 *                        CLI_MODE_ANY, "Show commands");
 *   cli_add_command(&s_cli, h_show,
 *                    "version", cmd_show_version,
 *                    CLI_PRIV_UNPRIVILEGED,
 *                    CLI_MODE_ANY, "Show version");
 *
 *   cli_telnet_ctx_struct_t tctx;
 *   cli_transport_struct_t  tp;
 *   cli_telnet_init(&tctx, &tp, client_fd);
 *
 *   cli_init(&s_cli, "router", &tp, NULL, s_cmd_pool, CLI_MAX_COMMANDS);
 *   cli_add_builtin_cmds(&s_cli);
 *
 *   cli_start(&s_cli);
 *
 *   while (1) {
 *       if (cli_session_engine(&s_cli) != CLI_OK) break;
 *   }
 *
 *   cli_done(&s_cli);
 * @endcode
 *
 * **Pool capacity**
 * Define @c CLI_MAX_COMMANDS in your application to set the command-pool size.  This macro is no
 * longer provided by the library each application chooses its own pool dimension.
 *
 * @copyright Copyright (c) 2026 Muhammad Hassaan Shah.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENLIBCLI_H
#define OPENLIBCLI_H

/* C++ detection */
#ifdef __cplusplus
extern "C" {
#endif

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* Pull in centralized compile-time configuration.
 *
 * Allow user sketches (Arduino) to override the library default by providing
 * a top-level `cli_config.h` in the sketch directory or include path. If the
 * compiler supports `__has_include` we prefer that; otherwise fall back to
 * the bundled `config/cli_config.h`.
 *
 * If CLI_BUILD_CONFIG_HEADER_ENABLED is defined, include the header specified
 * by CLI_CONFIG_HEADER_PATH (defaults to "cli_config.h") instead of the
 * built-in fallback logic. This is useful when:
 *   - The compiler lacks __has_include (MSVC pre-VS 2019)
 *   - The custom config header is at a non-standard path
 */
#if defined(CLI_BUILD_CONFIG_HEADER_ENABLED)
#ifdef CLI_CONFIG_HEADER_PATH
#include CLI_CONFIG_HEADER_PATH
#else
#include "cli_config.h"
#endif
#elif defined(__has_include)
#if __has_include("cli_config.h")
#include "cli_config.h"
#else
#include "config/cli_config.h"
#endif
#else
#include "config/cli_config.h"
// #pragma message("cli_config: config/cli_config.h (legacy fallback)")
#endif

#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
#include <avr/pgmspace.h>
#endif

#include "cli_env_detect.h"

/** @brief Internal: the per-line output buffer is needed by pipe filters and the
 *         per-line print callback. */
#define CLI_HAS_LINE_BUFFER (CLI_ENABLE_PIPE_FILTER || CLI_ENABLE_PRINT_TRANSPORT_CB)

/*=======================================================================================
 * Public Defines
 *=======================================================================================*/
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(CLI_SHARED)
#if defined(BUILD_LIB_SHARED)
#define OPENLIBCLI_API __declspec(dllexport)
#else
#define OPENLIBCLI_API __declspec(dllimport)
#endif
#else
#define OPENLIBCLI_API
#endif
#elif defined(CLI_SHARED) && defined(__GNUC__)
#define OPENLIBCLI_API __attribute__((visibility("default")))
#else
#define OPENLIBCLI_API
#endif

/* Boolean value constants with explicit cast for MISRA compliance. */
#ifndef True
#define True ((bool)1)
#endif
#ifndef False
#define False ((bool)0)
#endif

/** @brief Sentinel value meaning "invalid command handle" / error. */
#define CLI_CMD_INVALID (-1)

/** @brief Explicit root-level parent handle for @c cli_add_command(). */
#define CLI_CMD_ROOT (-2)

/* Return Codes -------------------------------------------------------------------------*/

/** @defgroup CLI_ReturnCodes Return Codes
 *  @brief Status values returned by most OpenLibCLI functions.
 *  @{
 */

#define CLI_OK                ((int8_t)0)  /**< Operation succeeded. */
#define CLI_ERR               ((int8_t)-1) /**< Generic / unspecified error. */
#define CLI_ERR_QUIT          ((int8_t)-2) /**< Session ended cleanly by user (exit / quit). */
#define CLI_ERR_NOMEM         ((int8_t)-3) /**< Static pool exhausted; no free slot available. */
#define CLI_ERR_AUTH          ((int8_t)-4) /**< Authentication failure. */
#define CLI_ERR_AMBIG         ((int8_t)-5) /**< Ambiguous command prefix. */
#define CLI_EXEC_REPLAY_INPUT ((int8_t)1)  /**< Redraw recalled input instead of prompting. */

/** @} */

/* Privilege Levels ---------------------------------------------------------------------*/

/** @defgroup CLI_Privilege Privilege Levels
 *  @{
 */

#define CLI_PRIV_UNPRIVILEGED ((cli_priv_t)0)  /**< Normal (user) privilege level.*/
#define CLI_PRIV_USER         ((cli_priv_t)0)  /**< Alias for @c CLI_PRIV_UNPRIVILEGED. */
#define CLI_PRIV_PRIVILEGED   ((cli_priv_t)10) /**< Privileged / enable mode level. */
#define CLI_PRIV_SUPERADMIN   ((cli_priv_t)15) /**< Privileged / super-admin mode level. */

/** @} */

/* Mode Identifiers ---------------------------------------------------------------------*/

/** @defgroup CLI_Modes Mode Identifiers
 *  @brief Modes control which commands are visible and executable.
 *        @c CLI_MODE_ANY matches every mode and is used for commands valid
 * everywhere. User-defined modes must start at @c CLI_MODE_USER_BASE.
 *  @{
 */

#define CLI_MODE_ANY       ((cli_mode_t)0)  /**< Match every mode always visible. */
#define CLI_MODE_EXEC      ((cli_mode_t)1)  /**< Unprivileged exec mode. */
#define CLI_MODE_ENABLE    ((cli_mode_t)2)  /**< Privileged exec ("enable") mode. */
#define CLI_MODE_CONFIG    ((cli_mode_t)3)  /**< Global configuration mode. */
#define CLI_MODE_USER_BASE ((cli_mode_t)20) /**< First available user-defined mode index. */

#define CLI_CMD_FLAG_HIDDEN ((uint8_t)0x01)
#define CLI_CMD_FLAG_IN_USE ((uint8_t)0x02)

/** @} */

/*=======================================================================================
 * Public Macros
 *=======================================================================================*/

/** @defgroup CLI_Macros Public Macros
 *  @{
 */

#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
/** @brief Wrap a string literal for AVR PROGMEM storage. */
#define CLI_STR(s)                 PSTR(s)
#define cli_print(cli, fmt, ...)   cli_print_p((cli), PSTR(fmt), ##__VA_ARGS__)
#define cli_println(cli, fmt, ...) cli_println_p((cli), PSTR(fmt), ##__VA_ARGS__)
#define cli_error(cli, fmt, ...)   cli_error_p((cli), PSTR(fmt), ##__VA_ARGS__)

#else
/** @brief Windows/Linux/MacOS, Non-AVR (STM32, ESP32, RP2040, â€¦) or Plain RAM strings. */
#define CLI_STR(s)            (s)
#define cli_print(cli, ...)   cli_print_p((cli), __VA_ARGS__)
#define cli_println(cli, ...) cli_println_p((cli), __VA_ARGS__)
#define cli_error(cli, ...)   cli_error_p((cli), __VA_ARGS__)
#endif

/**
 * @brief Helper macro for cli_add_command that automatically handles
 *        PROGMEM string wrapping for AVR targets.
 *
 * @note @p name and @p help must be string literals (e.g., "show").
 *       If you do not want a help string, pass an empty string "".
 *       Passing NULL to @p help via this macro will cause a compiler error on
 *       AVR.
 */
#define cli_register_command(cli, parent, name, callback, privilege, mode, help)                   \
  cli_add_command((cli), (parent), CLI_STR(name), (callback), (privilege), (mode), CLI_STR(help))

#define cli_unregister_command(cli, handle)           cli_remove_command((cli), (handle))
#define cli_unregister_command_recursive(cli, handle) cli_remove_command_recursive((cli), (handle))

/** @} */

/*=======================================================================================
 * Public Types
 *=======================================================================================*/

/* Forward Declarations -----------------------------------------------------------------*/

/* Enumerations -------------------------------------------------------------------------*/

/** @defgroup CLI_Enums Enumeration Types
 *  @{
 */

/**
 * @brief Behaviour when an idle timeout fires and no callback is set.
 */
typedef enum {
  CLI_IDLE_TIMEOUT_POLICY_CLOSE = 0,         /**< Terminate the session. */
  CLI_IDLE_TIMEOUT_POLICY_RESET_SESSION = 1, /**< Restart the banner / auth flow (default). Armed
                                                by user input: an untouched session is never
                                                reset, so it fires once per idle period. */
  CLI_IDLE_TIMEOUT_POLICY_IGNORE = 2,        /**< Ignore idle timeout events. */
} cli_idle_timeout_mode_enum_t;

/**
 * @brief Behaviour when @c exit is typed at the root / login mode.
 */
typedef enum {
  CLI_EXIT_ROOT_POLICY_CLOSE_SESSION = 0, /**< Terminate the session (default). */
  CLI_EXIT_ROOT_POLICY_RESET_SESSION = 1, /**< Restart the banner / auth flow. */
  CLI_EXIT_ROOT_POLICY_IGNORE = 2,        /**< Ignore the exit command. */
} cli_exit_root_policy_enum_t;

/**
 * @brief Behaviour when the @c quit command is typed.
 */
typedef enum {
  CLI_QUIT_POLICY_CLOSE_SESSION = 0, /**< Terminate the session (default). */
  CLI_QUIT_POLICY_RESET_SESSION = 1, /**< Restart the banner / auth flow. */
  CLI_QUIT_POLICY_IGNORE = 2,        /**< Ignore the quit command. */
} cli_quit_policy_enum_t;

/**
 * @brief Session lifecycle state machine states.
 */
typedef enum {
  CLI_SESSION_INIT = 0,    /**< Initial state; transitions to SHOW_BANNER on first tick. */
  CLI_SESSION_SHOW_BANNER, /**< Emitting the MOTD banner. */
  CLI_SESSION_RUN,         /**< Normal command processing loop. */
  CLI_SESSION_STOP         /**< Terminal state; session is ending. */
} cli_session_state_enum_t;

/**
 * @brief Authorization lifecycle state machine states.
 */
typedef enum {
  CLI_AUTH_NONE = 0,
  CLI_AUTH_USERNAME,             /**< Collecting the username. */
  CLI_AUTH_PASSWORD,             /**< Collecting the login password. */
  CLI_AUTH_ENABLE_MODE_PASSWORD, /**< Collecting the enable password. */
  CLI_AUTH_LOCKOUT,              /**< Waiting for auth lockout to expire. */
  CLI_AUTH_AUTHENTICATED         /**< User has passed authentication. */
} cli_auth_state_enum_t;

/**
 * @brief Behaviour after all authentication attempts are exhausted.
 */
typedef enum {
  CLI_AUTH_FAILURE_MODE_CLOSE = 0,   /**< Terminate the session with @c CLI_ERR_AUTH. */
  CLI_AUTH_FAILURE_MODE_LOCKOUT = 1, /**< Lock input for @c auth_lockout_seconds. */
} cli_auth_failure_mode_enum_t;

/**
 * @brief Runtime transport kind attached to a session transport vtable.
 */
typedef enum {
  CLI_TRANSPORT_UNKNOWN = 0,     /**< Transport kind not set. */
  CLI_TRANSPORT_TELNET = 1,      /**< Telnet transport. */
  CLI_TRANSPORT_TCP = 2,         /**< Raw TCP transport. */
  CLI_TRANSPORT_SERIAL = 3,      /**< Serial/fd transport. */
  CLI_TRANSPORT_PIPE = 4,        /**< Named/anonymous pipe transport. */
  CLI_TRANSPORT_UNIX_SOCKET = 5, /**< UNIX domain socket transport. */
} cli_transport_kind_enum_t;

/**
 * @brief Kind of print operation passed to @c cli_print_fn callbacks.
 */
typedef enum {
  CLI_PRINT_KIND_PRINT = 0,   /**< @c cli_print()  no trailing newline. */
  CLI_PRINT_KIND_PRINTLN = 1, /**< @c cli_println() line-oriented output. */
  CLI_PRINT_KIND_ERROR = 2    /**< @c cli_error()   error, bypasses pipe filter. */
} cli_print_kind_enum_t;

/** @} */

/* Type Aliases -------------------------------------------------------------------------*/
typedef struct cli_s cli_struct_t;         /** @brief Opaque CLI session instance. */
typedef struct cli_cmd_s cli_cmd_struct_t; /** @brief Opaque command node. */

typedef int cli_poolsize_t; /** @brief Pool size / capacity. */

/**
 * @brief Opaque integer index into the command pool.
 *
 * Use @c CLI_CMD_ROOT as the "root-level parent" value when registering a
 * top-level command.
 */
typedef cli_poolsize_t cli_cmd_handle_t;

typedef int cli_len_t;             /** @brief Count, Len. */
typedef int16_t cli_history_idx_t; /** @brief History index/offset. */

typedef int cli_argc_t; /** @brief Argument count for command handlers. */

typedef int cli_mode_t;     /** @brief CLI mode identifier. */
typedef uint8_t cli_priv_t; /** @brief Privilege level. */

typedef unsigned long cli_time_t; /** @brief time i.e at least 32 bit*/

#if ENV_IS_WINDOWS
typedef int cli_transport_buflen_t;
typedef int cli_transport_ret_t;
#elif ENV_IS_UNIX_LIKE
#include <sys/types.h>
typedef size_t cli_transport_buflen_t;
typedef ssize_t cli_transport_ret_t;
#else
typedef int cli_transport_buflen_t;
typedef int cli_transport_ret_t;
#endif

/**
 * @brief Timing statistics collected by @c cli_session_engine() per tick.
 *
 * Populated automatically when a microsecond source is registered.
 * Reset with @c cli_tick_stats_reset().
 */
typedef struct {
  uint32_t min_us; /**< Minimum observed tick duration in microseconds. */
  uint32_t max_us; /**< Maximum observed tick duration in microseconds. */
  uint32_t avg_us; /**< Running mean tick duration (Welford online algorithm). */
  uint32_t count;  /**< PVS-Studio: false positive — struct member, no external linkage. */

} cli_tick_stats_struct_t;

/* Callback Types -----------------------------------------------------------------------*/

/** @defgroup CLI_Callbacks Callback Function Types
 *  @{
 */

/**
 * @brief Command handler callback.
 *
 * @param[in] cli   The CLI session that invoked the command.
 * @param[in] cmd   The full command string as typed (before tokenisation).
 * @param[in] argc  Number of tokens (@p argv[0] is the leaf command word).
 * @param[in] argv  NULL-terminated token array.
 *
 * @return @c CLI_OK on success, @c CLI_ERR on failure.
 */
typedef int8_t (*cli_cmd_handler_fn)(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                     const char *argv[]);

/**
 * @brief Authentication callback.
 *
 * Called instead of the local user table when set. The session privilege is
 * reset to @c CLI_PRIV_USER before the call; to grant a higher level, call
 * @c cli_set_privilege(cli, ...) before returning @c CLI_OK. A privilege of
 * @c CLI_PRIV_PRIVILEGED or above starts the session in @c CLI_MODE_ENABLE.
 *
 * @param[in] cli       The CLI session being authenticated.
 * @param[in] username  Username string typed by the user.
 * @param[in] password  Password string typed by the user.
 *
 * @return @c CLI_OK to allow access, @c CLI_ERR_AUTH to deny.
 */
typedef int8_t (*cli_auth_fn)(cli_struct_t *cli, const char *username, const char *password);

/**
 * @brief Enable-secret verification callback.
 *
 * Called instead of the static @c enable_privilege_secret string when set.
 *
 * @param[in] cli       The CLI session requesting privilege.
 * @param[in] username  The authenticated username for this session ("" without login).
 * @param[in] password  The password typed at the @c "Password:" prompt.
 *
 * @return @c CLI_OK to grant privilege, @c CLI_ERR_AUTH to deny.
 */
typedef int8_t (*cli_enable_fn)(cli_struct_t *cli, const char *username, const char *password);

/**
 * @brief Mode / privilege change notification callback.
 *
 * Called after every successful mode or privilege transition.
 *
 * @param[in] cli       The CLI session that changed state.
 * @param[in] new_mode  The new mode index.
 * @param[in] new_priv  The new privilege level.
 */
typedef void (*cli_mode_fn)(cli_struct_t *cli, cli_mode_t new_mode, cli_priv_t new_priv);

/**
 * @brief Monotonic time source callback for idle-timeout handling.
 *
 * @param[in] ctx  Opaque context pointer supplied via @c cli_set_time_source.
 *
 * @return Current monotonic time in seconds (uint32_t wrap-around is handled).
 */
typedef uint32_t (*cli_time_fn)(void *ctx);

#if CLI_ENABLE_TIMMING_STATS
/**
 * @brief Microsecond time source for @c cli_session_engine() profiling.
 *
 * Optional pass @c NULL to @c cli_set_micros_source to skip tick timing.
 *
 * @param[in] ctx  Opaque context pointer supplied via @c cli_set_micros_source.
 *
 * @return Free-running monotonic microsecond counter (uint32_t wrap is fine).
 */
typedef uint32_t (*cli_micros_fn)(void *ctx);

#endif /* CLI_ENABLE_TIMMING_STATS */

/**
 * @brief Idle-timeout expiry callback.
 *
 * @param[in] cli  The CLI session whose idle timer expired.
 *
 * @return @c CLI_OK to keep the session running, or any negative code to end
 * it.
 */
typedef int8_t (*cli_idle_timeout_fn)(cli_struct_t *cli);

/**
 * @brief Periodic callback invoked by @c cli_session_engine() at a configured interval.
 *
 * @param[in] cli  The active CLI session.
 *
 * @return @c CLI_OK to continue, or any negative code to end the session.
 */
typedef int8_t (*cli_periodic_fn)(cli_struct_t *cli);

/**
 * @brief Session command callback for built-in exit / quit hooks.
 *
 * @param[in] cli  The active CLI session.
 *
 * @return @c CLI_OK to proceed with default behaviour, any other code to abort.
 */
typedef int8_t (*cli_session_cmd_fn)(cli_struct_t *cli);

/**
 * @brief Per-line print callback.
 *
 * When set, @c cli_print() and @c cli_error() invoke this callback once per
 * emitted line with a NUL-terminated string that does not include trailing
 * newlines.
 *
 * @param[in] cli   The active CLI session.
 * @param[in] line  NUL-terminated output line (no trailing newline).
 */
typedef void (*cli_print_transport_fn)(cli_struct_t *cli, const char *line);

/**
 * @brief Complete output override receives pre-formatted text + kind.
 *
 * When set, @c cli_print(), @c cli_println(), and @c cli_error() call this
 * with the already-formatted string and a @c cli_print_kind_t indicating the
 * source function.  Currently all internal processing (pipe filtering, @c print_transport_cb,
 * and transport writes) is skipped.
 *
 * @param[in] cli   The active CLI session.
 * @param[in] text  NUL-terminated formatted text.
 * @param[in] kind  Identifies the calling function (@c cli_print_kind_t).
 */
typedef void (*cli_print_fn)(cli_struct_t *cli, const char *text, cli_print_kind_enum_t kind);

/**
 * @brief Complete output override receives raw format + va_list.
 *
 * Higher priority than @c cli_print_fn. When set, @c cli_print(),
 * @c cli_println(), and @c cli_error() forward the raw format string and
 * variadic argument list directly. The callback is responsible for all
 * formatting and output, including the trailing newline for
 * @c CLI_PRINT_KIND_PRINTLN and @c CLI_PRINT_KIND_ERROR. All internal processing
 * (pipe filtering, @c print_transport_cb, and transport writes) is skipped.
 *
 * @param[in] cli   The active CLI session.
 * @param[in] kind  Identifies the calling function (@c cli_print_kind_enum_t).
 * @param[in] fmt   printf-style format string.
 * @param[in] ap    Variadic argument list.
 *
 * @return Number of characters produced (returned to the @c cli_print* caller),
 *         or a negative value on error.
 */
typedef int (*cli_print_v_fn)(cli_struct_t *cli, cli_print_kind_enum_t kind, const char *fmt,
                              va_list ap);

/** @} */

/* Transport Interface ------------------------------------------------------------------*/

/** @defgroup CLI_Transport Transport Interface
 *  @brief Every byte going to/from the user passes through this vtable,
 *         making the engine completely agnostic about the underlying medium
 *         (Telnet, TCP, UART, loopback pipe, â€¦).
 *
 *  - **available()** Return positive when at least one decoded input byte is
 *                    ready, zero when none is available, negative on error /
 *                    disconnect.
 *  - **read()** Return one decoded input byte as 0..255, or negative on error.
 *  - **write()** Return bytes written; negative on error.
 *  - **flush()** Optional (may be NULL).  Called after every prompt and output
 *                burst to drain any internal send buffer.
 *  @{
 */

/**
 * @brief Transport vtable filled by transport initialisation helpers
 *        (e.g. @c cli_telnet_init, @c cli_serial_init, @c cli_fd_init).
 */
typedef struct {
  cli_transport_kind_enum_t kind;              /**< Runtime transport kind. */
  cli_transport_ret_t (*available)(void *ctx); /**< Check for at least one decoded input byte. */
  cli_transport_ret_t (*read)(void *ctx);      /**< Read one decoded input byte, or <0 on error. */
  cli_transport_ret_t (*write)(void *ctx, const uint8_t *buf,
                               cli_transport_buflen_t len); /**< Write @p len bytes; returns
                                                              bytes written, <0 on error. */
  cli_transport_ret_t (*flush)(void *ctx);                  /**< Flush send buffer; may be NULL. */
  void *ctx; /**< Opaque context pointer passed verbatim to every call. */
} cli_transport_struct_t;

/** @} */

/* Pipe / Filter Types ------------------------------------------------------------------*/

/** @defgroup CLI_Filter Pipe Filter Types
 *  @{
 */

#if CLI_ENABLE_PIPE_FILTER
/**
 * @brief Active pipe-filter type for output processing.
 */
typedef enum {
  CLI_FILTER_NONE = 0,    /**< No filter; all output passes through. */
  CLI_FILTER_GREP = 1,    /**< @c | grep @c \<pattern\>  show matching lines. */
  CLI_FILTER_EXCLUDE = 2, /**< @c | exclude @c \<pattern\> hide matching lines. */
  CLI_FILTER_BEGIN = 3,   /**< @c | begin @c \<pattern\>  show from first match. */
  CLI_FILTER_COUNT = 4,   /**< @c | count              count and display total. */
} cli_filter_enum_t;
#endif /* CLI_ENABLE_PIPE_FILTER */

/** @} */

/* Command Handle -----------------------------------------------------------------------*/

/** @defgroup CLI_CommandHandle Command Handle
 *  @{
 */

/** @} */

/**
 * @brief Platform services supplied by the application at session creation.
 *
 * The CLI core consumes this vtable during @c cli_init() and copies any hooks
 * it needs into the session. Pass @c NULL when the default host OS services
 * are sufficient.
 */
typedef struct {
  cli_time_fn now_sec; /**< Optional monotonic seconds source. */
  void *now_sec_ctx;   /**< Opaque context passed to @c now_sec. */
#if CLI_ENABLE_TIMMING_STATS
  cli_micros_fn micros; /**< Optional microsecond source for tick profiling. */
  void *micros_ctx;     /**< Opaque context passed to @c micros. */
#endif
} cli_platform_struct_t;

/* Command Node -------------------------------------------------------------------------*/

/** @defgroup CLI_CommandNode Command Node Structure
 *  @{
 */

/**
 * @brief A single node in the hierarchical command tree.
 *
 * Stored in a global static pool; referenced only by pool indices and no heap allocation is ever
 * required.
 */
struct cli_cmd_s {
  cli_cmd_handler_fn callback; /**< Command handler; NULL for container (branch) nodes. */
  char name[CLI_MAX_NAME_LEN]; /**< Primary keyword for this command. */
#if CLI_ENABLE_COMMAND_HELP
  char help[CLI_MAX_HELP_LEN]; /**< One-line help string shown by @c ?. */
#endif

  cli_cmd_handle_t parent; /**< Pool index of the parent node, or @c CLI_CMD_ROOT. */
  cli_mode_t mode;         /**< Required mode (@c CLI_MODE_ANY or a specific mode). */
  cli_priv_t privilege;    /**< Minimum privilege level required to run this command. */
  uint8_t flags;           /**< Bitmask of @c CLI_CMD_FLAG_* values. */

#if CLI_ENABLE_ALIASES
#if CLI_MAX_ALIASES < 1
#error "CLI_MAX_ALIASES must be at least 1 when CLI_ENABLE_ALIASES is set"
#endif
  char aliases[CLI_MAX_ALIASES][CLI_MAX_NAME_LEN]; /**< Inline alias keywords
                                                      no extra tree nodes. */
  cli_len_t num_aliases;                           /**< Number of valid entries in @c aliases[]. */
#endif
};

/** @} */

/* Navigation Frame ---------------------------------------------------------------------*/

/**
 * @brief One saved navigation level (mode + submenu context + privilege).
 *
 * A frame is pushed whenever the session enters a new mode (@c cli_set_mode) or a
 * submenu context, and popped by the built-in @c exit command (or by @c cli_set_mode
 * returning to an enclosing mode), which restores the mode, context and privilege
 * recorded in the frame. Leaving the level entered by @c enable therefore also drops
 * the privilege it granted.
 */
typedef struct {
  cli_mode_t mode;          /**< Mode to restore when this level is left. */
  cli_cmd_handle_t context; /**< Submenu context to restore, or @c CLI_CMD_ROOT. */
  cli_priv_t privilege;     /**< Privilege to restore when this level is left. */
} cli_nav_frame_struct_t;

/* CLI Session Instance -----------------------------------------------------------------*/

/** @defgroup CLI_Session CLI Session Structure
 *  @brief All fields are value types no heap pointers.
 *        Obtain via @c cli_init(); release via @c cli_done().
 *  @{
 */

/**
 * @brief A complete CLI session context.
 *
 * All state is stored by value inside this struct.  Each session is fully
 * independent no shared mutable state between instances.
 */
struct cli_s {
  /* --- transport
   * ------------------------------------------------------------------- */
  cli_transport_struct_t transport; /**< Active transport vtable for this session. */

  /* --- pool management
   * ------------------------------------------------------------- */
  cli_cmd_struct_t *cmd_pool;    /**< User-provided command storage (per-instance). */
  cli_poolsize_t cmd_pool_size;  /**< Capacity of @c cmd_pool[]. */
  cli_poolsize_t cmd_pool_count; /**< Number of in-use slots in @c cmd_pool[]. */

  /* --- command context stack (submenu navigation)
   * ---------------------------------- */
  cli_cmd_handle_t context; /**< Current submenu context handle, or @c CLI_CMD_ROOT. */
  cli_nav_frame_struct_t nav_stack[CLI_MAX_CONTEXT_DEPTH]; /**< Saved mode/context levels;
                                                              empty at the login floor. */
  uint8_t nav_depth; /**< Number of valid entries in @c nav_stack[]. */

  /* --- hostname / prompt
   * ----------------------------------------------------------- */
  char hostname[CLI_MAX_PROMPT_LEN]; /**< Current prompt hostname string. */

  /* --- MOTD banner
   * ----------------------------------------------------------------- */
#if CLI_ENABLE_BANNER
  char banner[CLI_MAX_BANNER_LEN]; /**< MOTD banner text emitted at session
                                      start. */
#endif

  /* --- input line buffer
   * ----------------------------------------------------------- */
  char input[CLI_MAX_INPUT_LEN]; /**< Current input line being assembled. */
  cli_len_t input_len;           /**< Number of valid characters in @c input[]. */
  cli_len_t cursor;              /**< Current cursor position within @c input[]. */

  /* --- session state
   * --------------------------------------------------------------- */
  cli_mode_t mode; /**< Current CLI mode index. */
  /* --- mode names for prompt suffix
   * ------------------------------------------------ */
#if CLI_ENABLE_MODE_NAMES
  cli_mode_t mode_name_ids[CLI_MAX_MODES]; /**< Mode value owning each @c mode_names[] slot;
                                              @c CLI_MODE_ANY marks a free slot. */
  char mode_names[CLI_MAX_MODES][CLI_MAX_MODE_NAME_LEN]; /**< Display names shown inside the
                                                            prompt brackets. */
#endif
  cli_priv_t privilege; /**< Current privilege level. */

  cli_session_state_enum_t session_state; /**< Current lifecycle state-machine state. */

  /**< Behaviour of @c exit at the root / login mode. */
  cli_exit_root_policy_enum_t exit_root_policy;
  cli_quit_policy_enum_t quit_policy; /**< Behaviour of the @c quit command. */

  /* --- callbacks
   * ------------------------------------------------------------------- */
  cli_mode_fn on_mode_change;     /**< Called after every mode/privilege transition. */
  cli_session_cmd_fn on_exit_cmd; /**< Hook called before the built-in @c exit handler. */
  cli_session_cmd_fn on_quit_cmd; /**< Hook called before the built-in @c quit handler. */

  /* --- authentication
   * -------------------------------------------------------------- */
#if CLI_ENABLE_AUTH
  cli_auth_state_enum_t auth_state;
  char users[CLI_MAX_USERS][CLI_MAX_USERNAME_LEN];     /**< Local user table usernames. */
  char passwords[CLI_MAX_USERS][CLI_MAX_PASSWORD_LEN]; /**< Local user table passwords. */
  char auth_username[CLI_MAX_USERNAME_LEN + 1]; /**< Username captured before password entry;
                                                     one byte longer than a valid username, so
                                                     a name typed too long never matches. */
  cli_priv_t user_privileges[CLI_MAX_USERS]; /**< Privilege level per local user. */
  cli_len_t num_users;     /**< Number of entries currently in the local user table. */
  cli_len_t auth_attempts;   /**< Number of failed login attempts in this auth cycle. */
  cli_len_t enable_attempts; /**< Number of consecutive failed enable-secret attempts. */
  cli_auth_failure_mode_enum_t auth_failure_mode; /**< What to do when max attempts are exceeded. */
  cli_time_t auth_lockout_seconds;    /**< Lockout duration in seconds (0 = immediate). */
  cli_time_t auth_lockout_started_ts; /**< Timestamp when the current lockout began. */
  cli_auth_state_enum_t auth_resume_state; /**< State restored after the enable prompt or a
                                              lockout; @c CLI_AUTH_USERNAME means re-login. */
  bool auth_requested; /**< @c cli_request_auth() during a (re)start: the session begins at
                            the login prompt even without @c require_auth. */
  bool require_auth;                  /**< True when the session requires login before running
                                         commands. */
  cli_auth_fn authorization_cb;       /**< Custom auth callback; overrides the local user table. */

  /* --- cisco like enable mode
   * ------------------------------------------------------ */
  char enable_privilege_secret[CLI_MAX_ENABLE_SECRET_LEN]; /**< Static enable password
                                                    (compared on enable). */
  cli_enable_fn enable_privilege_cb; /**< Custom enable-secret callback; overrides the
                              static secret. */

#endif

#if CLI_ENABLE_HISTORY
  /* --- history ring (flat buffer)
   * ---------------------------------------------------------------- */
  char history_buf[CLI_HISTORY_BUF_SIZE];         /**< Flat buffer of NUL-terminated entries. */
  cli_history_idx_t history_off[CLI_MAX_HISTORY]; /**< Offset of each slot in @c history_buf[]. */
  cli_history_idx_t history_head;                 /**< Next write slot index (wraps at
                                                       CLI_MAX_HISTORY). */
  cli_history_idx_t history_count;                /**< Valid entries (CLI_MAX_HISTORY). */
  cli_history_idx_t history_nav; /**< Current browse offset (0 = tip / current line). */
#if CLI_HISTORY_RESTORE_PREBROWSE_LINE
  char history_saved[CLI_MAX_INPUT_LEN]; /**< Snapshot of the line before
                                             history navigation started. */
#endif
#endif

#if CLI_ENABLE_IDLE_TIMEOUT
  cli_time_t idle_timeout;   /**< Idle timeout in seconds; 0 disables timeout. */
  cli_time_t last_action_ts; /**< Timestamp of the most recent user activity. */
  cli_idle_timeout_mode_enum_t idle_timeout_policy; /**< Fallback behaviour when no
                                                idle callback is set. */
  cli_idle_timeout_fn idle_timeout_cb;              /**< Called when the idle timer expires. */
  bool idle_armed; /**< The reset-session idle policy applies: set by user input, cleared
                        when the session (re)starts — an untouched session has nothing to
                        reset. The close policy and the idle callback do not depend on it. */
#endif

#if CLI_ENABLE_PERIODIC_CALLBACK
  cli_time_t periodic_interval; /**< Periodic callback interval in seconds. */
  cli_time_t last_periodic_ts;  /**< Timestamp of the most recent periodic callback. */
  cli_periodic_fn periodic_cb;  /**< Called periodically by @c cli_session_engine(). */
#endif

#if CLI_ENABLE_TIME_SOURCE
  cli_time_fn time_source; /**< Monotonic time source for idle-timeout. */
  void *time_source_ctx;   /**< Opaque context passed to @c time_source. */
#endif

#if CLI_ENABLE_TIMMING_STATS
  cli_micros_fn micros_source;        /**< Microsecond time source for tick profiling. */
  void *micros_source_ctx;            /**< Opaque context passed to @c micros_source. */
  cli_tick_stats_struct_t tick_stats; /**< Accumulated tick-timing statistics. */
#endif

#if CLI_ENABLE_PIPE_FILTER
  /* --- output filter (pipe)
   * -------------------------------------------------------- */
  cli_filter_enum_t filter_type;          /**< Active filter type for the current command. */
  char filter_pattern[CLI_MAX_INPUT_LEN]; /**< Pattern string for grep / exclude
                                             / begin filters. */
  cli_len_t filter_count;                 /**< Line count accumulated by the @c count filter. */
  bool filter_begin_found;                /**< True once the begin-pattern has been seen. */
#endif                                    /* CLI_ENABLE_PIPE_FILTER */

#if CLI_HAS_LINE_BUFFER
  /* --- per-line output buffer (pipe filter / per-line callback)
   * ------------------------------------------------------ */
  char filter_line_buf[CLI_MAX_OUTPUT_BUF]; /**< Accumulates partial output lines. */
  cli_len_t filter_line_buf_len; /**< Number of bytes currently in @c filter_line_buf[]. */
#endif

  /* --- command execution
   * ------------------------------------------------------ */
  uint8_t exec_depth; /**< Commands currently executing (> 1 = nested @c cli_exec()). */
  cli_len_t exec_used; /**< While a command runs: bytes of @c input[] holding the executing
                          lines; @c cli_exec() places its line after them. */
  bool out_open;       /**< Command output so far did not end with a newline. */
#if CLI_ENABLE_HISTORY && CLI_HISTORY_INDEX_SELECTION
  cli_history_idx_t history_replay; /**< Ring slot recalled by @c history N, or -1. */
  bool history_cleared; /**< Set by @c cli_clear_history(): the typed line being executed
                             is not recorded afterwards (it cleared the history, e.g. by
                             starting a new login). */
#endif

  /* --- ANSI / VT100 escape state machine
   * ------------------------------------------ */
  uint8_t esc_state; /**< Current escape-sequence parser state. */
  int esc_param;     /**< First CSI numeric parameter (saturated). */
  uint8_t eol_prev;  /**< Line-ending byte (CR or LF) that just submitted a line, else 0; the
                        complementary byte of a CR LF / LF CR pair is then swallowed. */

  bool ansi_supported; /**< Terminal supports ANSI/VT100 escape sequences (default true). */
  bool tx_error;       /**< A transport write failed; the session ends on the next tick. */

  /* --- arbitrary user payload
   * ------------------------------------------------------ */
  void *user_data; /**< Application-defined pointer; set/get via
                      cli_set_userdata / cli_get_userdata. */

#if CLI_ENABLE_PRINT_TRANSPORT_CB
  /**< Per-line output intercept callback. */
  cli_print_transport_fn print_transport_cb;
#endif

  /**< Formatted-text output override (receives text + kind; NULL = disabled). */
  cli_print_fn print_cb;

  /**< Raw fmt+va_list output override (NULL = disabled, takes priority). */
  cli_print_v_fn print_cb_v;
};

/** @} */

/*=======================================================================================
 * External Data Variables
 *=======================================================================================*/

/** @defgroup CLI_ExternVars External Data Variables
 *  @{
 */

/* No public external data variables. */

/** @} */

/*=======================================================================================
 * Public Function Prototypes
 *=======================================================================================*/

/* Library Initialisation ---------------------------------------------------------------*/

/** @defgroup CLI_Init Library Initialisation
 *  @{
 */

/** @} */

/* Session Lifecycle --------------------------------------------------------------------*/

/** @defgroup CLI_Lifecycle Session Lifecycle
 *  @{
 */

/**
 * @brief Initialise a CLI session with user-provided memory.
 *
 * The caller supplies both the @c cli_struct_t instance and the command pool array.
 * The session owns the pool: every slot is cleared here, so commands must be
 * registered after this call (call @c cli_add_builtin_cmds() for the built-ins).
 * Re-initialising a session with the same pool therefore starts from an empty
 * command tree. Does nothing if @p cli or @p transport is @c NULL.
 *
 * @param[out] cli         Zeroed and initialised session.
 * @param[in]  hostname    Initial hostname string shown in the prompt (@c NULL = "cli").
 * @param[in]  transport   Pointer to a filled @c cli_transport_struct_t; contents are
 *                         copied.
 * @param[in]  platform    Optional platform services; pass @c NULL for host OS
 *                         defaults.
 * @param[in]  cmd_pool    User-provided command storage array.
 * @param[in]  cmd_pool_size  Number of entries in @p cmd_pool.
 */
OPENLIBCLI_API void cli_init(cli_struct_t *cli, const char *hostname,
                             const cli_transport_struct_t *transport,
                             const cli_platform_struct_t *platform, cli_cmd_struct_t *cmd_pool,
                             cli_poolsize_t cmd_pool_size);

/**
 * @brief One-time library initialisation.
 *
 * Must be called once before any @c cli_init() call.  Currently a no-op
 * (built-in commands are registered per session with @c cli_add_builtin_cmds()), but
 * calling it ensures forward compatibility should global state be added
 * in a future release.
 */
OPENLIBCLI_API void cli_lib_init(void);

/**
 * @brief Release a CLI session. Zeros the @c cli_struct_t struct; command pool
 * memory is untouched.
 *
 * @param[in] cli  Session to release; may be @c NULL (no-op).
 */
OPENLIBCLI_API void cli_done(cli_struct_t *cli);

/**
 * @brief Get the current lifecycle state of a session.
 *
 * @param[in] cli  Session to query; may be @c NULL (@c CLI_SESSION_STOP
 * returned).
 *
 * @return Current @c cli_session_state_t value.
 */
OPENLIBCLI_API cli_session_state_enum_t cli_get_session_state(const cli_struct_t *cli);

/**
 * @brief Restart the session flow from banner â†’ auth (optional) â†’ run.
 *
 * Starts a fresh logical session: mode, privilege, submenu navigation, line
 * editor state, and command history are reset. Usable from a command handler
 * (the @c quit / @c exit reset policies call it); unlike @c cli_start(), a
 * pending transport write failure is kept, so a dead peer still ends the session.
 *
 * @param[in] cli  Active CLI session.
 */
OPENLIBCLI_API void cli_restart_session(cli_struct_t *cli);

/**
 * @brief Request an authentication flow for the current session.
 *
 * Drops the session to @c CLI_MODE_EXEC / @c CLI_PRIV_USER, clears command
 * history, and shows the @c "Username:" prompt.
 *
 * @param[in] cli  Active CLI session.
 */
OPENLIBCLI_API void cli_request_auth(cli_struct_t *cli);

/** @} */

/* Session Configuration ----------------------------------------------------------------*/

/** @defgroup CLI_Config_Setters Session Configuration Setters
 *  @{
 */

/**
 * @brief Set whether the terminal supports ANSI/VT100 escape sequences.
 *
 * @param[in] cli      Active CLI session.
 * @param[in] enabled  True if terminal supports ANSI, false otherwise.
 */
OPENLIBCLI_API void cli_set_ansi_supported(cli_struct_t *cli, bool enabled);

/**
 * @brief Set the session hostname string (appears in the prompt).
 *
 * @param[in] cli       Active CLI session.
 * @param[in] hostname  New hostname string (@c NULL clears it).
 */
OPENLIBCLI_API void cli_set_hostname(cli_struct_t *cli, const char *hostname);

/**
 * @brief Set the MOTD banner text.
 *
 * @param[in] cli     Active CLI session.
 * @param[in] banner  Banner string to emit at session start (@c NULL clears it).
 */
OPENLIBCLI_API void cli_set_banner(cli_struct_t *cli, const char *banner);

/**
 * @brief Set the enable-mode secret (privileged exec password).
 *
 * The secret is stored in plain text inside the session; use
 * @c cli_set_enable_privilege_cb() to verify against a hash instead.
 *
 * @param[in] cli     Active CLI session.
 * @param[in] secret  Plaintext secret string, or @c NULL to clear it.
 *
 * @return @c CLI_OK, or @c CLI_ERR if @p cli is @c NULL or @p secret does not fit in
 *         @c CLI_MAX_ENABLE_SECRET_LEN (the previous secret is kept; a truncated
 *         secret would accept a shorter password).
 */
OPENLIBCLI_API int8_t cli_set_enable_secret(cli_struct_t *cli, const char *secret);

/**
 * @brief Set a custom enable-secret verification callback. Register a callback to verify the
 * enable-mode secret dynamically.
 *
 * When set, the callback is called with the entered secret instead of
 * comparing it against the static @c enable_privilege_secret field. Pass @c NULL to
 * revert to static string comparison.
 *
 * Failed attempts are counted per session; after three consecutive failures the
 * configured @c cli_auth_failure_mode_enum_t is applied (close or lock out).
 *
 * @param[in] cli  Active CLI session.
 * @param[in] fn   Enable verification callback, or @c NULL to use static
 *                 secret comparison.
 */
OPENLIBCLI_API void cli_set_enable_privilege_cb(cli_struct_t *cli, cli_enable_fn fn);

/**
 * @brief Register a callback invoked when the @c exit command is executed.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] fn   Callback; return @c CLI_OK to proceed, non-zero to abort.
 */
OPENLIBCLI_API void cli_set_exit_cb(cli_struct_t *cli, cli_session_cmd_fn fn);

/**
 * @brief Register a callback invoked when the @c quit command is executed.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] fn   Callback; return @c CLI_OK to proceed, non-zero to abort.
 */
OPENLIBCLI_API void cli_set_quit_cb(cli_struct_t *cli, cli_session_cmd_fn fn);

/**
 * @brief Set behaviour for @c exit when invoked at the root / login mode.
 *
 * @param[in] cli     Active CLI session.
 * @param[in] policy  One of the @c cli_exit_root_policy_t values
 *                    (e.g. do nothing, drop to unprivileged, or end session).
 */
OPENLIBCLI_API void cli_set_exit_root_policy(cli_struct_t *cli, cli_exit_root_policy_enum_t policy);

/**
 * @brief Set behaviour for the @c quit command.
 *
 * @param[in] cli     Active CLI session.
 * @param[in] policy  One of the @c cli_quit_policy_t values
 *                    (e.g. always end session or pop context first).
 */
OPENLIBCLI_API void cli_set_quit_policy(cli_struct_t *cli, cli_quit_policy_enum_t policy);

/**
 * @brief Store an application-defined pointer in the session.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] data  Arbitrary pointer stored verbatim.
 */
OPENLIBCLI_API void cli_set_userdata(cli_struct_t *cli, void *data);

/**
 * @brief Retrieve the application-defined pointer stored in the session.
 *
 * @param[in] cli  Session to query.
 *
 * @return The pointer stored by @c cli_set_userdata, or @c NULL.
 */
OPENLIBCLI_API void *cli_get_userdata(const cli_struct_t *cli);

/**
 * @brief Get the runtime transport kind for a session.
 *
 * @param[in] cli  Session to query.
 *
 * @return Transport kind, or @c CLI_TRANSPORT_UNKNOWN when @p cli is NULL.
 */
OPENLIBCLI_API cli_transport_kind_enum_t cli_get_transport_kind(const cli_struct_t *cli);

/**
 * @brief Register a mode / privilege change notification callback.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] fn   Callback invoked after every successful transition.
 */
OPENLIBCLI_API void cli_set_mode_change_cb(cli_struct_t *cli, cli_mode_fn fn);

/**
 * @brief Assign a display name for a mode (shown inside the prompt brackets).
 *
 * Any mode value may be named, including user-defined modes at or above
 * @c CLI_MODE_USER_BASE. Up to @c CLI_MAX_MODES modes can carry a name at once;
 * an empty (or @c NULL) name removes the mode's entry.
 *
 * @code
 *   cli_set_mode_name(cli, CLI_MODE_CONFIG, "config");
 * @endcode
 *
 * @param[in] cli   Active CLI session.
 * @param[in] mode  Mode value to name (not @c CLI_MODE_ANY).
 * @param[in] name  Display string (e.g. @c "config"), shorter than
 *                  @c CLI_MAX_MODE_NAME_LEN.
 *
 * @return @c CLI_OK, @c CLI_ERR on invalid arguments or a name that does not fit
 *         (the previous name is kept), or @c CLI_ERR_NOMEM when @c CLI_MAX_MODES
 *         names are already stored.
 */
OPENLIBCLI_API int8_t cli_set_mode_name(cli_struct_t *cli, cli_mode_t mode, const char *name);

/**
 * @brief Override the monotonic time source used by idle-timeout checks.
 *
 * By default, @c cli_init() installs a host OS monotonic clock when one is
 * available. Bare-metal targets should pass @c cli_platform_struct_t.now_sec to
 * @c cli_init().
 *
 * @param[in] cli  Active CLI session.
 * @param[in] fn   Time source callback.
 * @param[in] ctx  Opaque context passed to @p fn.
 */
OPENLIBCLI_API void cli_set_time_source(cli_struct_t *cli, cli_time_fn fn, void *ctx);

#if CLI_ENABLE_TIMMING_STATS
/**
 * @brief Register a microsecond time source for @c cli_session_engine()
 * profiling.
 *
 * Once set, every tick call updates the stats accessible via @c
 * cli_tick_stats_get(). Pass @c NULL to disable timing.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] fn   Microsecond time source, or @c NULL to disable.
 * @param[in] ctx  Opaque context passed to @p fn.
 */
OPENLIBCLI_API void cli_set_micros_source(cli_struct_t *cli, cli_micros_fn fn, void *ctx);
#endif /* CLI_ENABLE_TIMMING_STATS */

/**
 * @brief Set the idle timeout in seconds (0 disables).
 *
 * @param[in] cli      Active CLI session.
 * @param[in] seconds  Timeout value; 0 disables timeout.
 */
OPENLIBCLI_API void cli_set_idle_timeout(cli_struct_t *cli, uint32_t seconds);

/**
 * @brief Set the idle timeout and a callback invoked when it expires.
 *
 * The callback replaces the idle-timeout policy: it is invoked on every expiry
 * regardless of @c cli_set_idle_timeout_mode(). Returning @c CLI_OK re-arms the
 * timer; any other value ends the session with that code.
 *
 * @param[in] cli       Active CLI session.
 * @param[in] seconds   Timeout value; 0 disables timeout.
 * @param[in] callback  Callback invoked on expiry.
 */
OPENLIBCLI_API void cli_set_idle_timeout_cb(cli_struct_t *cli, uint32_t seconds,
                                            cli_idle_timeout_fn callback);

/**
 * @brief Set idle-timeout behaviour when no expiry callback is registered.
 *
 * The default is @c CLI_IDLE_TIMEOUT_POLICY_RESET_SESSION. It applies only once the
 * user has typed something since the session started (or was last reset): a
 * session waiting untouched at its first prompt is already in the state a reset
 * would produce, so it is not restarted. @c CLI_IDLE_TIMEOUT_POLICY_CLOSE counts
 * from the start of the session, so a connection nobody uses is closed.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] mode  One of @c CLI_IDLE_TIMEOUT_POLICY_CLOSE, @c
 *                  CLI_IDLE_TIMEOUT_POLICY_RESET_SESSION, or @c
 *                  CLI_IDLE_TIMEOUT_POLICY_IGNORE.
 */
OPENLIBCLI_API void cli_set_idle_timeout_mode(cli_struct_t *cli, cli_idle_timeout_mode_enum_t mode);

/**
 * @brief Register a periodic callback invoked by @c cli_session_engine() (default: 1 s).
 *
 * Default interval is 1 second.
 *
 * @param[in] cli       Active CLI session.
 * @param[in] callback  Function to invoke periodically.
 */
OPENLIBCLI_API void cli_set_periodic_cb(cli_struct_t *cli, cli_periodic_fn callback);

/**
 * @brief Set the periodic callback interval in seconds (0 disables).
 *
 * @param[in] cli      Active CLI session.
 * @param[in] seconds  Interval in seconds; 0 disables periodic execution.
 */
OPENLIBCLI_API void cli_set_periodic_interval(cli_struct_t *cli, uint32_t seconds);

/**
 * @brief Mark the current time as the last activity (manual keepalive).
 *
 * @param[in] cli  Active CLI session.
 */
OPENLIBCLI_API void cli_touch_activity(cli_struct_t *cli);

#if CLI_ENABLE_HISTORY
/**
 * @brief Clear all session command history entries.
 *
 * @param[in] cli  Active CLI session.
 *
 * @return @c CLI_OK on success, @c CLI_ERR if @p cli is @c NULL.
 */
OPENLIBCLI_API int8_t cli_clear_history(cli_struct_t *cli);
#endif

/**
 * @brief Check the idle timeout once; call periodically in non-blocking
 * integrations.
 *
 * Nothing happens during an authentication lockout: it ends by itself, and the
 * idle period starts again then.
 *
 * @param[in] cli  Active CLI session.
 *
 * @return @c CLI_OK if the session is still active, negative if the timeout
 * fired.
 */
OPENLIBCLI_API int8_t cli_check_idle_timeout(cli_struct_t *cli);

/** @} */

/* Authentication -----------------------------------------------------------------------*/

/** @defgroup CLI_Auth Authentication
 *  @{
 */

/**
 * @brief Add a username / password / privilege entry to the local static table.
 *
 * Passwords are kept in plain text in the session and compared in constant
 * time. To verify against hashed credentials, use @c cli_set_authorization_cb().
 *
 * @param[in] cli        Active CLI session.
 * @param[in] username   Username string (non-empty, shorter than @c CLI_MAX_USERNAME_LEN).
 * @param[in] password   Password string (shorter than @c CLI_MAX_PASSWORD_LEN).
 * @param[in] privilege  @c CLI_PRIV_USER or @c CLI_PRIV_PRIVILEGED.
 *
 * @return @c CLI_OK on success, @c CLI_ERR on invalid / too long credentials,
 *         @c CLI_ERR_NOMEM if the table is full.
 */
OPENLIBCLI_API int8_t cli_add_user(cli_struct_t *cli, const char *username, const char *password,
                                   cli_priv_t privilege);

/**
 * @brief Set a custom authentication callback.
 *
 * When set, the callback is invoked during the login sequence instead of
 * the built-in local user-table lookup. The callback grants privilege by
 * calling @c cli_set_privilege() on the session it receives (see @c cli_auth_fn).
 *
 * @param[in] cli  Active CLI session.
 * @param[in] fn   Authentication callback.
 */
OPENLIBCLI_API void cli_set_authorization_cb(cli_struct_t *cli, cli_auth_fn fn);

/**
 * @brief Enable or disable mandatory login authentication.
 *
 * Default: disabled (no auth required).
 *
 * @param[in] cli      Active CLI session.
 * @param[in] require  @c true to require login; @c false to skip auth entirely.
 */
OPENLIBCLI_API void cli_require_authorization(cli_struct_t *cli, bool require);

/**
 * @brief Choose behaviour when authentication attempts are exhausted.
 *
 * Applies after three failed logins and after three consecutive failed enable
 * secrets. @c CLI_AUTH_FAILURE_MODE_LOCKOUT needs a time source; without one
 * (and with a non-zero lockout duration) the session is closed instead, because
 * the lockout could never expire. Input received during a lockout is discarded,
 * and the idle timeout is paused until the lockout ends (an idle session restart
 * would otherwise end the lockout early).
 *
 * @param[in] cli   Active CLI session.
 * @param[in] mode  One of the @c cli_auth_failure_mode_enum_t values.
 */
OPENLIBCLI_API void cli_set_auth_failure_mode(cli_struct_t *cli, cli_auth_failure_mode_enum_t mode);

/**
 * @brief Set the lockout duration after too many failed login attempts. Set lockout duration used
 * by
 * @c CLI_AUTH_FAILURE_MODE_LOCKOUT.
 *
 * @param[in] cli      Active CLI session.
 * @param[in] seconds  Lockout duration; 0 = immediate unlock/reset.
 */
OPENLIBCLI_API void cli_set_auth_lockout_duration(cli_struct_t *cli, uint32_t seconds);

/** @} */

/* Command Registration -----------------------------------------------------------------*/

/** @defgroup CLI_Register Command Registration
 *  @{
 */

/**
 * @brief Register a command in a session's per-instance command pool.
 *
 * A name (or alias) may appear only once among the children of @p parent whose
 * modes overlap (same mode, or either one @c CLI_MODE_ANY), compared without
 * regard to case; otherwise the command could never be selected.
 *
 * On AVR, @p name and @p help must be program-memory strings (@c CLI_STR() /
 * @c cli_register_command()); the same applies to alias names.
 *
 * @param[in] cli         Active CLI session.
 * @param[in] parent      Handle of an existing command node, or
 *                        @c CLI_CMD_ROOT for a root-level command.
 * @param[in] name        Command keyword string: non-empty, shorter than
 *                        @c CLI_MAX_NAME_LEN, and free of whitespace, control
 *                        characters, @c ?, @c | and @c ".
 * @param[in] callback    Handler invoked when the command is executed, or
 *                        @c NULL for pure sub-menu nodes.
 * @param[in] privilege   Minimum privilege level needed to see and run the command
 *                        (@c CLI_PRIV_USER / @c CLI_PRIV_UNPRIVILEGED or
 *                        @c CLI_PRIV_PRIVILEGED).
 * @param[in] mode        CLI mode in which the command is visible: a specific mode
 *                        constant, or @c CLI_MODE_ANY for all modes.
 * @param[in] help        Short help string shown by the @c ? / help commands,
 *                        or @c NULL for no help text.
 *
 * @return A handle (â‰¥ 0) usable as @p parent for sub-commands, or
 *         @c CLI_CMD_INVALID on error (pool full, invalid / too long / duplicate
 *         name, unknown parent).
 */
OPENLIBCLI_API cli_cmd_handle_t cli_add_command(cli_struct_t *cli, cli_cmd_handle_t parent,
                                                const char *name, cli_cmd_handler_fn callback,
                                                cli_priv_t privilege, cli_mode_t mode,
                                                const char *help);

#if CLI_ENABLE_ALIASES
/**
 * @brief Register an alias for an existing command. Register a duplicate command node that shares
 * @p original's callback.
 *
 * Creates a new command node that shares the same parent, callback, privilege,
 * mode, and help text as @p original, but is reachable under @p alias_name.
 * Both are fully independent either can be hidden or unregistered without
 * affecting the other. The new node is a full command entry with its own slot in the pool; it is
 * not a lightweight alias.  Prefer @c cli_cmd_add_alias when possible.
 *
 * @code
 *   cli_cmd_handle_t h = cli_add_command(cli, CLI_CMD_ROOT,
 *       "clear", cmd_clear, CLI_PRIV_USER, CLI_MODE_ANY, "Clear screen");
 *   cli_add_command_duplicate(cli, h, "cls");
 * @endcode
 *
 * @param[in] cli         Active CLI session.
 * @param[in] original    Handle of the command to duplicate. Handle returned by a prior @c
 * cli_add_command call.
 * @param[in] alias_name  New keyword for the alias (no spaces, no @c ?).
 *
 * @return A handle for the alias node, or @c CLI_CMD_INVALID on error.
 */
OPENLIBCLI_API cli_cmd_handle_t cli_add_command_duplicate(cli_struct_t *cli,
                                                          cli_cmd_handle_t original,
                                                          const char *alias_name);

/**
 * @brief Add an alias name inline to an existing command node.
 *
 * No new node is allocated the alias is stored inside the node itself
 * (up to @c CLI_MAX_ALIASES per node).  Aliases participate in prefix matching,
 * tab-completion, and inline @c ? help exactly like the primary name. The alias is stored directly
 * in the command's @c aliases[] array and matched during tab-completion and execution without
 * consuming a pool slot.
 *
 * @code
 *   cli_cmd_handle_t h = cli_add_command(cli, CLI_CMD_ROOT,
 *       "clear", cmd_clear, CLI_PRIV_USER, CLI_MODE_ANY, "Clear screen");
 *   cli_cmd_add_alias(cli, h, "cls");
 *   cli_cmd_add_alias(cli, h, "refresh");
 * @endcode
 *
 * @param[in] cli         Active CLI session.
 * @param[in] handle      Handle of the target command. Handle returned by @c cli_add_command.
 * @param[in] alias_name  Alias keyword to attach; same rules as a command name.
 *
 * @return @c CLI_OK, @c CLI_ERR (bad handle, invalid name, or the name is already used by
 *         an overlapping sibling), or @c CLI_ERR_NOMEM (slots full).
 */
OPENLIBCLI_API int8_t cli_cmd_add_alias(cli_struct_t *cli, cli_cmd_handle_t handle,
                                        const char *alias_name);
#endif /* CLI_ENABLE_ALIASES */

/**
 * @brief Remove a command node from the command tree.
 *
 * Zeroes the pool slot and decrements the pool counter. Child nodes are left
 * in place but detached (their parent becomes @c CLI_CMD_INVALID), so they stay
 * unreachable even if the freed slot is reused by a new command; use
 * @c cli_remove_command_recursive to remove a subtree. A session currently inside
 * the removed submenu, or inside any submenu below it, is moved to the removed
 * node's parent.
 *
 * @param[in] cli     Active CLI session.
 * @param[in] handle  Handle of the command to remove. Handle returned by @c cli_add_command.
 *
 * @return @c CLI_OK on success, @c CLI_ERR on invalid / unused handle.
 */
OPENLIBCLI_API int8_t cli_remove_command(cli_struct_t *cli, cli_cmd_handle_t handle);

/**
 * @brief Remove a command node and all of its descendants from the command
 * tree.  Recursively remove a command subtree.
 * Depth-first: removes all descendant nodes before removing @p handle itself.
 *
 * @param[in] cli     Active CLI session.
 * @param[in] handle  Handle of the root command to remove.
 *
 * @return @c CLI_OK if @p handle was successfully removed (descendant errors
 *         are silently ignored), or @c CLI_ERR for an invalid handle.
 */
OPENLIBCLI_API int8_t cli_remove_command_recursive(cli_struct_t *cli, cli_cmd_handle_t handle);

/**
 * @brief Mark a command as hidden so it is excluded from help listings.
 *
 * Hidden commands are still executable if their full name (or alias) is typed;
 * they are omitted from @c ?, @c help, prefix matching and tab-completion.
 *
 * @param[in] cli     Active CLI session.
 * @param[in] handle  Handle of the command to hide.
 */
OPENLIBCLI_API void cli_hide_command(cli_struct_t *cli, cli_cmd_handle_t handle);

/** @} */

/* Running the CLI ----------------------------------------------------------------------*/

/** @defgroup CLI_Run Running the CLI
 *  @{
 */

/**
 * @brief Start a feed-based session.
 *
 * Starts a fresh logical session (banner → optional login → prompt): mode,
 * privilege, submenu navigation, line editor, history, pipe filter, login state
 * and a previous transport write failure are reset, and the idle timer restarts.
 * Configuration (commands, users, callbacks, policies) is kept.
 *
 * Call it before the first @c cli_session_engine() call, and again to begin a new
 * session on the same @c cli_struct_t after @c cli_session_engine() stopped (for
 * example when a new client connects to the same transport).
 *
 * @param[in] cli  CLI session to start.
 */
OPENLIBCLI_API void cli_start(cli_struct_t *cli);

/**
 * @brief Session state machine. Single non-blocking iteration of the session state machine.
 *
 * Drives banner emission, authentication, periodic callbacks, idle-timeout
 * checking, and input processing in one call.  Suitable for bare-metal
 * poll / super-loop integrations. Call this from a bare-metal poll / super-loop to get full session
 * handling.
 *
 * At most @c CLI_MAX_RX_BYTES_PER_TICK input bytes are processed per call; the
 * rest stays in the transport (still reported by @c available()) for the next
 * call, so a continuous input stream cannot keep the call from returning.
 * Input processing also stops as soon as a transport write fails.
 *
 * @param[in] cli  Active CLI session.
 *
 * @return @c CLI_OK session active, call again next tick. @n
 *         @c CLI_ERR_QUIT session ended cleanly. @n
 *         @c CLI_ERR transport / internal error. @n
 *         @c CLI_ERR_AUTH authentication failed.
 */
OPENLIBCLI_API int8_t cli_session_engine(cli_struct_t *cli);

#if CLI_ENABLE_TIMMING_STATS
/**
 * @brief Read a snapshot of accumulated tick-timing statistics.
 *
 * @param[in]  cli     Session to query; @c NULL zeroes @p out.
 * @param[out] out     Filled with the tick stats snapshot.
 */
OPENLIBCLI_API void cli_tick_stats_get(const cli_struct_t *cli, cli_tick_stats_struct_t *out);

/**
 * @brief Reset tick-timing statistics to zero.
 *
 * @param[in] cli  Active CLI session.
 */
OPENLIBCLI_API void cli_tick_stats_reset(cli_struct_t *cli);
#endif /* CLI_ENABLE_TIMMING_STATS */

/**
 * @brief Execute a command string programmatically.
 *
 * Copies @p cmd into the internal input buffer and runs the full execution
 * pipeline (trim, tokenize, resolve, callback). Output is still sent through the
 * active transport. Called from outside any command (a callback, the main loop)
 * while the session runs, the output starts on a new line and the prompt, with
 * whatever the user had typed, is shown again afterwards.
 *
 * Arguments containing spaces, @c | or @c ? are written in double quotes
 * (@c "a b"); inside quotes @c \\" and @c \\\\ escape a quote and a backslash.
 *
 * The command is copied into the free part of the session's input buffer, after
 * the line being edited (or a password being typed) and after the lines of any
 * commands still executing, so none of those are disturbed and no extra buffer
 * is used. The command is not added to the session history. It may be called
 * from inside a command handler; such a nested command's output flows through
 * the outer command's pipe filter, so the nested line itself may not contain a
 * filter. Nesting deeper than @c CLI_MAX_EXEC_DEPTH is refused.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] cmd  NUL-terminated command string (e.g. @c "show version").
 *
 * @return @c CLI_OK on success, @c CLI_ERR on failure (including nesting too deep),
 *         @c CLI_ERR_QUIT if the session ended, @c CLI_ERR_AMBIG on ambiguity,
 *         @c CLI_ERR_NOMEM if @p cmd does not fit in the free part of the input
 *         buffer (at most @c CLI_MAX_INPUT_LEN - 1 characters; less while a line is
 *         being typed or commands are executing) — nothing is executed then.
 */
OPENLIBCLI_API int8_t cli_exec(cli_struct_t *cli, const char *cmd);

/**
 * @brief Execute a command from pre-parsed argument tokens.
 *
 * Joins @p argv[0] .. argv[argc-1] with spaces and passes the result
 * to @c cli_exec(). Arguments that are empty or contain whitespace, quotes,
 * @c | or @c ? are quoted, so each entry reaches the handler as exactly one
 * token.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] argv  Argument vector (argv[0] = command name).
 * @param[in] argc  Number of entries in @p argv.
 *
 * @return Same as @c cli_exec(); @c CLI_ERR_NOMEM if the joined line does not
 *         fit in @c CLI_MAX_INPUT_LEN (nothing is executed).
 */
OPENLIBCLI_API int8_t cli_exec_argv(cli_struct_t *cli, const char *const *argv, cli_argc_t argc);

/** @} */

/* Mode / Privilege Control -------------------------------------------------------------*/

/** @defgroup CLI_ModePriv Mode and Privilege Control
 *  @{
 */

/**
 * @brief Switch the session to a different CLI mode.
 *
 * Navigation is stack based:
 *  - If @p mode is already on the navigation stack (an enclosing level), the
 *    session returns to that level, restoring its submenu context and privilege.
 *  - Otherwise the current mode, context and privilege are pushed, so a later
 *    @c exit returns here, and @p mode becomes active.
 *
 * The @c on_mode_change callback fires when the mode or privilege actually changes.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] mode  New mode value (application-defined; use predefined
 *                  @c CLI_MODE_* constants or custom values; not @c CLI_MODE_ANY).
 *
 * @return @c CLI_OK, @c CLI_ERR if @p cli is @c NULL or @p mode is @c CLI_MODE_ANY, or
 *         @c CLI_ERR_NOMEM when the navigation stack (@c CLI_MAX_CONTEXT_DEPTH) is full
 *         (mode unchanged).
 */
OPENLIBCLI_API int8_t cli_set_mode(cli_struct_t *cli, cli_mode_t mode);

/**
 * @brief Set the session privilege level.
 *
 * Fires the @c on_mode_change callback so the application can update the
 * prompt to reflect the new privilege.
 *
 * @param[in] cli        Active CLI session.
 * @param[in] privilege  New privilege level (@c CLI_PRIV_USER or
 *                       @c CLI_PRIV_PRIVILEGED).
 */
OPENLIBCLI_API void cli_set_privilege(cli_struct_t *cli, cli_priv_t privilege);

/**
 * @brief Return the current session mode.
 *
 * @param[in] cli  Active CLI session.
 * @return Current mode value, or @c -1 if @p cli is @c NULL.
 */
OPENLIBCLI_API cli_mode_t cli_get_mode(const cli_struct_t *cli);

/**
 * @brief Get the current privilege level.
 *
 * @param[in] cli  Session to query.
 *
 * @return Current privilege level, or @c CLI_PRIV_USER (the lowest level) if
 *         @p cli is @c NULL.
 */
OPENLIBCLI_API cli_priv_t cli_get_privilege(const cli_struct_t *cli);

/** @} */

/* Output Helpers -----------------------------------------------------------------------*/

/** @defgroup CLI_Output Output Helpers
 *  @brief Formatted print functions and per-line callback setter.
 *  @{
 */

/**
 * @brief Formatted print without an automatic trailing newline.
 *
 * Formats the string using @c vsnprintf. While a pipe filter is active or a
 * @c print_transport_cb is set, output is collected per line: when a @c \\n is
 * encountered the accumulated line is passed to the filter and lines that pass
 * are sent to the transport (or @c print_transport_cb). Otherwise text is written
 * to the transport immediately. A partial line left at the end of a command is
 * emitted when the command finishes; use @c cli_print_flush() to emit it earlier.
 *
 * When either @c print_cb or @c print_cb_v is set, all internal processing
 * is bypassed and the formatted text (or raw fmt+va_list) is forwarded to
 * the callback.
 *
 * Use @c cli_println for line-at-a-time output to ensure pipe filtering
 * operates correctly on complete lines.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] fmt  printf-style format string.
 * @param[in] ...  Format arguments.
 *
 * @return Number of characters that would have been written (like @c vsnprintf),
 *         or a negative value on encoding error.
 */
OPENLIBCLI_API int cli_print_p(cli_struct_t *cli, const char *fmt, ...);

/**
 * @brief Formatted print that forces the current logical line to flush.
 *
 * Formats the string using @c vsnprintf, passes the complete line through
 * the active pipe filter, and appends @c \\r\\n.  Prefer this function over
 * @c cli_print for line-at-a-time output so that pipe filtering sees whole
 * logical lines.  When @c print_transport_cb is not set, the default transport writes the emitted
 * line followed by @c \\r\\n; when @c print_transport_cb is set, the callback receives only the
 * line text.
 *
 * When either @c print_cb or @c print_cb_v is set, all internal processing
 * is bypassed and the formatted text (or raw fmt+va_list) is forwarded to
 * the callback.
 *
 * Prefer this function over @c cli_print for line-at-a-time output so that
 * pipe filtering sees whole logical lines.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] fmt  printf-style format string.
 * @param[in] ...  Format arguments.
 *
 * @return Number of characters that would have been written by @c vsnprintf,
 *        including the @c \\r\\n suffix, or a negative value on encoding error.
 */
OPENLIBCLI_API int cli_println_p(cli_struct_t *cli, const char *fmt, ...);

/**
 * @brief Print an error line that bypasses the active pipe filter.
 *
 * Formats the message with a @c "% " IOS-style prefix and always delivers it
 * to the transport (or @c print_transport_cb), regardless of the current pipe filter
 * state.
 *
 * When either @c print_cb or @c print_cb_v is set, all internal processing
 * is bypassed and the formatted text (or raw fmt+va_list) is forwarded to
 * the callback.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] fmt  printf-style format string.
 * @param[in] ...  Format arguments.
 *
 * @return Number of characters written to the output, or a negative value on
 *         encoding error.
 */
OPENLIBCLI_API int cli_error_p(cli_struct_t *cli, const char *fmt, ...);

/**
 * @brief Flush any partially buffered output line.
 *
 * If the per-line output buffer contains characters that have not yet been
 * terminated by @c \n, this function null-terminates the buffered text,
 * emits it as a complete logical line through the active output path, and
 * clears the buffer.  If no partial line is pending, the function does
 * nothing. Called automatically at the end of every command.
 *
 * @param[in] cli  Active CLI session (@c NULL is a no-op).
 */
OPENLIBCLI_API void cli_print_flush(cli_struct_t *cli);

#if CLI_ENABLE_PRINT_TRANSPORT_CB
/**
 * @brief Register a per-line print callback.
 *
 * When set, every line emitted by @c cli_print, @c cli_println, and
 * @c cli_error is delivered to @p cb instead of the transport.  Pass
 * @c NULL to restore default transport-based output. Lines longer than
 * @c CLI_MAX_OUTPUT_BUF - 1 characters are delivered in several pieces.
 * The callback must not call @c cli_print / @c cli_println / @c cli_error itself
 * (the line it receives lives in the session's line buffer).
 * Available when @c CLI_ENABLE_PRINT_TRANSPORT_CB is set.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] cb   Callback to install, or @c NULL to restore default output.
 */
OPENLIBCLI_API void cli_set_print_transport_cb(cli_struct_t *cli, cli_print_transport_fn cb);
#endif

/**
 * @brief Register a formatted-text output override.
 *
 * When set, @c cli_print(), @c cli_println(), and @c cli_error() call @p cb
 * with the already-formatted string and a @c cli_print_kind_t indicating the
 * source function, instead of the internal output pipeline.
 * Pass @c NULL to restore default behaviour.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] cb   Callback to install, or @c NULL to restore default output.
 */
OPENLIBCLI_API void cli_set_print_cb(cli_struct_t *cli, cli_print_fn cb);

/**
 * @brief Register a raw fmt+va_list output override.
 *
 * Higher priority than @c cli_set_print_cb().  When set, @c cli_print(),
 * @c cli_println(), and @c cli_error() forward the raw format string and
 * variadic argument list directly to @p cb.  The callback handles all
 * formatting and output.  Pass @c NULL to restore default behaviour.
 *
 * @param[in] cli  Active CLI session.
 * @param[in] cb   Callback to install, or @c NULL to restore default output.
 */
OPENLIBCLI_API void cli_set_print_cb_v(cli_struct_t *cli, cli_print_v_fn cb);

/**
 * @brief Safe wrapper around @c snprintf.
 *
 * @param[out] buf   Destination buffer.
 * @param[in]  size  Size of the destination buffer.
 * @param[in]  fmt   Format string.
 *
 * @return The number of characters that would have been written (excluding
 *         the null terminator), or a negative value on error.
 */
OPENLIBCLI_API int cli_snprintf(char *buf, size_t size, const char *fmt, ...);

/**
 * @brief Safe wrapper around @c vsnprintf.
 *
 * @param[out] buf   Destination buffer.
 * @param[in]  size  Size of the destination buffer.
 * @param[in]  fmt   Format string.
 * @param[in]  ap    Variadic argument list.
 *
 * @return The number of characters that would have been written (excluding
 *         the null terminator), or a negative value on error.
 */
OPENLIBCLI_API int cli_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/**
 * @brief Parse an integer or float from a string using a format specifier.
 *
 * @param[in] str   The input string to parse.
 * @param[in] fmt   The format specifier (e.g., "%u", "%d", "%f", "%x").
 * @param[out] out  Pointer to the target variable.
 *
 * @return @c CLI_OK (1) on success, or 0 on failure.
 */
OPENLIBCLI_API int cli_sscanf(const char *str, const char *fmt, void *out);

/* Built-in Command Handlers ------------------------------------------------------------*/

/**
 * @brief Register all built-in commands into a CLI instance.
 *
 * Registers @c exit, @c quit, @c enable, @c disable, @c end, @c configure,
 * @c help, @c usage (with @c commands and @c keys), @c clear / @c cls (ANSI
 * terminals; the alias needs @c CLI_ENABLE_ALIASES) and, if enabled, @c history.
 * They take up to 12 pool entries; the longest name, @c configure, needs
 * @c CLI_MAX_NAME_LEN of at least 10.
 *
 * @param[in] cli  Pointer to an initialised CLI instance.
 *
 * @return @c CLI_OK, or @c CLI_ERR if any of them could not be registered (pool
 *         full, name too long); the others are registered regardless.
 */
OPENLIBCLI_API int8_t cli_add_builtin_cmds(cli_struct_t *cli);

/**
 * @brief Built-in @c exit command handler.
 *
 * Pops one navigation level, restoring the mode and submenu context that were
 * active before it was entered (leaving @c CLI_MODE_ENABLE also drops privilege
 * to @c CLI_PRIV_USER). At the login floor (no saved level) the configured
 * @c exit_root_policy applies.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] cmd   Command string (unused).
 * @param[in] argc  Argument count (unused).
 * @param[in] argv  Argument vector (unused).
 *
 * @return @c CLI_OK or @c CLI_ERR_QUIT.
 */
OPENLIBCLI_API int8_t cli_cmd_builtin_exit(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                           const char *argv[]);

/**
 * @brief Built-in @c quit command handler.
 *
 * Behaviour is governed by the configured @c quit_policy.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] cmd   Command string (unused).
 * @param[in] argc  Argument count (unused).
 * @param[in] argv  Argument vector (unused).
 *
 * @return @c CLI_OK or @c CLI_ERR_QUIT.
 */
OPENLIBCLI_API int8_t cli_cmd_builtin_quit(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                           const char *argv[]);

/**
 * @brief Built-in @c enable command handler.
 *
 * Elevates to privileged mode.  Prompts for the enable secret when one is
 * configured.
 *
 * @param[in] cli   Active CLI session.
 * @param[in] cmd   Command string (unused).
 * @param[in] argc  Argument count (unused).
 * @param[in] argv  Argument vector (unused).
 *
 * @return @c CLI_OK.
 */
OPENLIBCLI_API int8_t cli_cmd_builtin_enable(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                             const char *argv[]);

/**
 * @brief Built-in @c disable command handler drops to user privilege and exec
 * mode, which becomes the new login floor (a following @c exit ends the session).
 * @param[in] cli   Active CLI session.
 * @param[in] cmd   Command string (unused).
 * @param[in] argc  Argument count (unused).
 * @param[in] argv  Argument vector (unused).
 * @return @c CLI_OK.
 */
OPENLIBCLI_API int8_t cli_cmd_builtin_disable(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                              const char *argv[]);

/**
 * @brief Built-in @c end command handler returns to privileged exec from any
 * config mode (and any submenu entered from it).
 * @param[in] cli   Active CLI session.
 * @param[in] cmd   Command string (unused).
 * @param[in] argc  Argument count (unused).
 * @param[in] argv  Argument vector (unused).
 * @return @c CLI_OK.
 */
OPENLIBCLI_API int8_t cli_cmd_builtin_end(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                          const char *argv[]);

/**
 * @brief Built-in @c configure command handler enters global configuration
 * mode.
 * @param[in] cli   Active CLI session.
 * @param[in] cmd   Command string (unused).
 * @param[in] argc  Argument count (unused).
 * @param[in] argv  Argument vector (unused).
 * @return @c CLI_OK.
 */
OPENLIBCLI_API int8_t cli_cmd_builtin_configure(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                                const char *argv[]);

#if CLI_ENABLE_HISTORY
/**
 * @brief Built-in @c history command handler lists recent commands.
 * @param[in] cli   Active CLI session.
 * @param[in] cmd   Command string (unused).
 * @param[in] argc  Argument count (unused).
 * @param[in] argv  Argument vector (unused).
 * @return @c CLI_OK.
 */
OPENLIBCLI_API int8_t cli_cmd_builtin_history(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                              const char *argv[]);
#endif

/**
 * @brief Built-in @c help command handler lists commands or sub-commands.
 * @param[in] cli   Active CLI session.
 * @param[in] cmd   Command string (unused).
 * @param[in] argc  Argument count; optional path tokens to sub-command to list.
 * @param[in] argv  Argument vector.
 * @return @c CLI_OK, @c CLI_ERR (unknown), or @c CLI_ERR_AMBIG (ambiguous).
 */
OPENLIBCLI_API int8_t cli_cmd_builtin_help(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                           const char *argv[]);

/**
 * @brief Built-in @c clear / @c cls command handler sends ANSI clear-screen.
 * @param[in] cli   Active CLI session.
 * @param[in] cmd   Command string (unused).
 * @param[in] argc  Argument count (unused).
 * @param[in] argv  Argument vector (unused).
 * @return @c CLI_OK.
 */
OPENLIBCLI_API int8_t cli_cmd_builtin_clear_screen(cli_struct_t *cli, const char *cmd,
                                                   cli_argc_t argc, const char *argv[]);

/**
 * @brief Built-in @c usage command enters usage submenu with @c commands
 *        and @c keys subcommands.
 * @param[in] cli   Active CLI session.
 * @param[in] cmd   Command string (unused).
 * @param[in] argc  Argument count (unused).
 * @param[in] argv  Argument vector (unused).
 * @return @c CLI_OK.
 */
OPENLIBCLI_API int8_t cli_cmd_builtin_usage(cli_struct_t *cli, const char *cmd, cli_argc_t argc,
                                            const char *argv[]);

/**
 * @brief Built-in @c usage commands handler prints common usage examples.
 * @param[in] cli   Active CLI session.
 * @param[in] cmd   Command string (unused).
 * @param[in] argc  Argument count (unused).
 * @param[in] argv  Argument vector (unused).
 * @return @c CLI_OK.
 */
OPENLIBCLI_API int8_t cli_cmd_builtin_usage_commands(cli_struct_t *cli, const char *cmd,
                                                     cli_argc_t argc, const char *argv[]);

/**
 * @brief Built-in @c usage keys handler prints line-editing key shortcuts.
 * @param[in] cli   Active CLI session.
 * @param[in] cmd   Command string (unused).
 * @param[in] argc  Argument count (unused).
 * @param[in] argv  Argument vector (unused).
 * @return @c CLI_OK.
 */
OPENLIBCLI_API int8_t cli_cmd_builtin_usage_keys(cli_struct_t *cli, const char *cmd,
                                                 cli_argc_t argc, const char *argv[]);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* OPENLIBCLI_H */

/*################################### END OF FILE ######################################*/
