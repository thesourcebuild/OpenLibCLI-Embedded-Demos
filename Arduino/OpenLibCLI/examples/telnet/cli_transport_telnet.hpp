/**
 * @file cli_transport_telnet.hpp
 * @brief Arduino Telnet transport adapter for open-libcli.
 *
 * Declares a Telnet transport context and initialization helpers that wire an
 * Arduino network client into @c cli_transport_struct_t while handling Telnet IAC
 * negotiation and stream parsing.
 *
 * @author Muhammad Hassaan Shah
 * @date 2026-04-19
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 thesourcebuild and Muhammad Hassaan Shah
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENLIBCLI_ARDUINO_CLI_TRANSPORT_TELNET_HPP
#define OPENLIBCLI_ARDUINO_CLI_TRANSPORT_TELNET_HPP

/* C++ detection */
#ifdef __cplusplus
extern "C" {
#endif

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#include <stdbool.h>
#include <stdint.h>

#include <cli.h>

/*=======================================================================================
 * Public Defines
 *=======================================================================================*/

#define CLI_TELNET_OK  (0)
#define CLI_TELNET_ERR (-1)

/*=======================================================================================
 * Public Macros
 *=======================================================================================*/

/* No public function-like macros. */

/*=======================================================================================
 * Public Types
 *=======================================================================================*/

typedef struct {
  void *client_handle;
  uint8_t state;
  uint8_t verb;
  uint16_t sb_len; /* bytes consumed in the current sub-negotiation */
  uint8_t sb_buf[5]; /* its start: option and the NAWS window size (width, height) */
  uint16_t window_width; /* client window width (NAWS), 0 until reported */
  bool window_width_due;    /* window_width still waits for the input sent before it */
  uint16_t window_width_at; /* how much of rx_buf that input is */
  cli_struct_t *cli;     /* session told about window size changes, or NULL */
  bool negotiated;
  bool local_echo;
  bool local_sga;
  bool remote_naws;
  bool remote_linemode_rejected;
  uint8_t rx_buf[256];
  uint16_t rx_len;
  uint16_t rx_pos;
} cli_telnet_ctx_struct_t;

/*=======================================================================================
 * External Data Variables
 *=======================================================================================*/

/* No externally visible data variables. */

/*=======================================================================================
 * Public Function Prototypes
 *=======================================================================================*/

/**
 * @brief Initialise a Telnet transport context and wire it into the CLI engine.
 *
 * configures the context with the provided client handle and maps the
 * read, write, and flush callbacks to the @c transport vtable.
 *
 * @param[out] ctx        Caller-allocated context (must outlive the session).
 * @param[out] transport  Filled with @c read, @c write, and @c flush callbacks.
 * @param[in]  client_handle Handle to the Arduino network client (e.g., WiFiClient).
 */
void cli_transport_telnet_init(cli_telnet_ctx_struct_t *ctx, cli_transport_struct_t *transport,
                                void *client_handle);

/**
 * @brief Update the network client handle used by the Telnet transport.
 *
 * Useful when the underlying connection is dropped and a new client is established.
 *
 * @param[in] ctx            Telnet context.
 * @param[in] client_handle  New handle to the Arduino network client.
 */
void cli_transport_telnet_set_client(cli_telnet_ctx_struct_t *ctx, void *client_handle);

/**
 * @brief Send the initial IAC option bundle to the connected client.
 *
 * Transmits @c WILL ECHO, @c WILL SGA, @c DONT LINEMODE, and @c DO NAWS.
 * Call once immediately after @c cli_transport_telnet_init().
 *
 * @param[in] ctx  Telnet context.
 */
void cli_transport_telnet_negotiate(cli_telnet_ctx_struct_t *ctx);

/**
 * @brief Tell a CLI session the client's window width, now and whenever it changes.
 *
 * The client reports its window size (NAWS) when the connection starts and after each
 * resize; the session's line editor uses the width for lines longer than one row
 * (@c cli_set_terminal_width()). Call after @c cli_init(); a width already reported is
 * passed on at once.
 *
 * @param[in] ctx  Telnet context.
 * @param[in] cli  Session using this connection, or @c NULL to stop.
 */
void cli_transport_telnet_set_session(cli_telnet_ctx_struct_t *ctx, cli_struct_t *cli);

/**
 * @brief Check if the underlying network client is still connected.
 *
 * @param[in] ctx  Telnet context.
 * @return @c true if connected, @c false otherwise.
 */
bool cli_transport_telnet_client_connected(const cli_telnet_ctx_struct_t *ctx);

/**
 * @brief Read raw socket data and process IAC negotiation.
 *
 * Reads available data from the Arduino Client, runs the telnet IAC state
 * machine, and appends decoded user bytes to the internal rx buffer.
 * Call from the main loop before @c cli_session_engine():
 *
 * @code
 *   cli_telnet_handler(&telnet_ctx);
 *   cli_session_engine(cli);
 * @endcode
 *
 * @param[in] ctx  Telnet context.
 * @return @c CLI_OK on success (or no data available),
 *         @c CLI_ERR on disconnect or fatal socket error.
 */
int8_t cli_telnet_handler(cli_telnet_ctx_struct_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* OPENLIBCLI_ARDUINO_CLI_TRANSPORT_TELNET_HPP */

/*################################### END OF FILE ######################################*/
