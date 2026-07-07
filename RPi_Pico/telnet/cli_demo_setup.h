#ifndef OPENLIBCLI_PICO_CLI_DEMO_SETUP_H
#define OPENLIBCLI_PICO_CLI_DEMO_SETUP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "lwip/tcp.h"

void cli_demo_setup(struct tcp_pcb *client_pcb);
void cli_demo_poll(void);
void cli_demo_shutdown(void);
uint8_t cli_demo_is_running(void);

#ifdef __cplusplus
}
#endif

#endif
