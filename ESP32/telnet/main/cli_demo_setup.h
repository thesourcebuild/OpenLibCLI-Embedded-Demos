#ifndef OPENLIBCLI_ESP32_CLI_DEMO_SETUP_H
#define OPENLIBCLI_ESP32_CLI_DEMO_SETUP_H

#ifdef __cplusplus
extern "C" {
#endif

void cli_demo_setup(int client_fd);
void cli_demo_poll(void);
void cli_demo_shutdown(void);
uint8_t cli_demo_is_running(void);

#ifdef __cplusplus
}
#endif

#endif
