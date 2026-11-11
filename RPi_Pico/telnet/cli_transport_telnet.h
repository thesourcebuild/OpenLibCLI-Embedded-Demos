#ifndef OPENLIBCLI_PICO_CLI_TRANSPORT_TELNET_H
#define OPENLIBCLI_PICO_CLI_TRANSPORT_TELNET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "OpenLibCLI/cli.h"
#include "lwip/tcp.h"

typedef struct {
  struct tcp_pcb *client_pcb;
  uint8_t state;
  uint8_t verb;
  uint16_t sb_len; /* bytes consumed in the current sub-negotiation */
  bool negotiated;
  bool local_echo;
  bool local_sga;
  bool remote_naws;
  bool remote_linemode_rejected;
  uint8_t rx_buf[256];   /* decoded input for the CLI engine */
  uint16_t rx_len;
  uint16_t rx_pos;
  uint8_t raw_buf[256];  /* raw bytes taken from rx_queue for decoding */
  struct pbuf *rx_queue; /* received segments not yet decoded (lwIP lock) */
  bool peer_closed;      /* the client closed its side (lwIP lock) */
  bool started;
  bool input_started;
} cli_telnet_ctx_t;

void cli_transport_telnet_init(cli_telnet_ctx_t *ctx,
                               cli_transport_struct_t *transport,
                               struct tcp_pcb *client_pcb);

void cli_transport_telnet_set_client(cli_telnet_ctx_t *ctx,
                                     struct tcp_pcb *client_pcb);

void cli_transport_telnet_negotiate(cli_telnet_ctx_t *ctx);

/* Close the client connection (if any) and free the input still queued. */
void cli_transport_telnet_close(cli_telnet_ctx_t *ctx);

bool cli_transport_telnet_client_connected(const cli_telnet_ctx_t *ctx);

int8_t cli_telnet_handler(cli_telnet_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif
