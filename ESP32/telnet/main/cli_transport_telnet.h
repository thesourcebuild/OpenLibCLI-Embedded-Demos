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

  bool cli_transport_telnet_client_connected(const cli_telnet_ctx_struct_t *ctx);

  int8_t cli_telnet_handler(cli_telnet_ctx_struct_t *ctx);

#ifdef __cplusplus
}
#endif

#endif
