#ifndef OPENLIBCLI_PICO_CLI_DEMO_SETUP_H
#define OPENLIBCLI_PICO_CLI_DEMO_SETUP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "lwip/tcp.h"

/* Take a new connection: call from the tcp_accept callback (lwIP context). Returns false
 * while another client is being served (the caller then refuses the connection). */
bool cli_demo_accept(struct tcp_pcb *client_pcb);
/* A connection was accepted and its CLI session has not been started yet. */
bool cli_demo_client_waiting(void);
/* Start the CLI session for the accepted connection (main loop). */
void cli_demo_setup(void);
void cli_demo_poll(void);
void cli_demo_shutdown(void);
uint8_t cli_demo_is_running(void);

#ifdef __cplusplus
}
#endif

#endif
