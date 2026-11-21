#ifndef OPENLIBCLI_ESP32_CLI_TRANSPORT_TELNET_H
#define OPENLIBCLI_ESP32_CLI_TRANSPORT_TELNET_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>
#include "OpenLibCLI/cli.h"

  typedef struct
  {
    int client_fd;
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

  void cli_transport_telnet_init(cli_telnet_ctx_struct_t *ctx,
                                 cli_transport_struct_t *transport,
                                 int client_fd);

  void cli_transport_telnet_set_client(cli_telnet_ctx_struct_t *ctx, int client_fd);

  void cli_transport_telnet_negotiate(cli_telnet_ctx_struct_t *ctx);

  /* Tell a CLI session the client's window width (NAWS), now and after each resize, for
   * the line editor (cli_set_terminal_width()). Call after cli_init(); NULL stops it. */
  void cli_transport_telnet_set_session(cli_telnet_ctx_struct_t *ctx, cli_struct_t *cli);

  bool cli_transport_telnet_client_connected(const cli_telnet_ctx_struct_t *ctx);

  int8_t cli_telnet_handler(cli_telnet_ctx_struct_t *ctx);

#ifdef __cplusplus
}
#endif

#endif
