#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "cli_transport_serial.h"

static int s_pending_byte = PICO_ERROR_TIMEOUT;

static cli_transport_ret_t pico_serial_available(void *ctx) {
  (void)ctx;
  if (s_pending_byte != PICO_ERROR_TIMEOUT) {
    return 1;
  }
  s_pending_byte = getchar_timeout_us(0);
  return (s_pending_byte != PICO_ERROR_TIMEOUT) ? 1 : 0;
}

static cli_transport_ret_t pico_serial_read(void *ctx) {
  (void)ctx;
  cli_transport_ret_t rc = CLI_ERR;
  if (s_pending_byte != PICO_ERROR_TIMEOUT) {
    rc = (unsigned char)s_pending_byte;
    s_pending_byte = PICO_ERROR_TIMEOUT;
  } else {
    int c = getchar_timeout_us(0);
    if (c != PICO_ERROR_TIMEOUT) {
      rc = (unsigned char)c;
    }
  }
  return rc;
}

static cli_transport_ret_t pico_serial_write(void *ctx, const uint8_t *buf, cli_transport_buflen_t len) {
  (void)ctx;
  if (buf == NULL || len == 0) {
    return CLI_ERR;
  }
  for (cli_transport_buflen_t i = 0; i < len; i++) {
    putchar(buf[i]);
  }
  return (cli_transport_ret_t)len;
}

static cli_transport_ret_t pico_serial_flush(void *ctx) {
  (void)ctx;
  fflush(stdout);
  return CLI_OK;
}

void cli_transport_serial_init(cli_transport_struct_t *transport) {
  if (transport != NULL) {
    memset(transport, 0, sizeof(*transport));
    transport->kind = CLI_TRANSPORT_SERIAL;
    transport->available = pico_serial_available;
    transport->read = pico_serial_read;
    transport->write = pico_serial_write;
    transport->flush = pico_serial_flush;
    transport->ctx = NULL;
  }
}
