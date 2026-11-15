#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#if LIB_PICO_STDIO_USB
#include "pico/stdio_usb.h"
#include "tusb.h"
#endif
#include "cli_transport_serial.h"

static int s_pending_byte = PICO_ERROR_TIMEOUT;

#if LIB_PICO_STDIO_USB
/* Whether a terminal holds the USB serial port open (DTR), as last seen. */
static bool s_usb_host;

/* stdio hands over input only while a terminal holds the port: anything else is nobody's,
 * and left in the USB receive FIFO it would become the next terminal's first login line.
 * TinyUSB calls these from its task, which stdio runs under its own lock: the context
 * that owns that FIFO. */

/* Input that arrives while no terminal holds the port. */
void tud_cdc_rx_cb(uint8_t itf)
{
  if (!stdio_usb_connected())
  {
    tud_cdc_n_read_flush(itf);
  }
}

/* Input the terminal that has just closed the port sent but the CLI has not read. */
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
  (void)dtr;
  (void)rts;
  if (!stdio_usb_connected())
  {
    tud_cdc_n_read_flush(itf);
  }
}
#endif

static cli_transport_ret_t pico_serial_available(void *ctx) {
  cli_transport_ret_t result = 0;

  (void)ctx;
#if LIB_PICO_STDIO_USB
  if (stdio_usb_connected() != s_usb_host) {
    /* A terminal closed or opened the port: the session ends, so the next terminal starts
     * at the banner and login instead of inside the previous user's session. */
    s_usb_host = !s_usb_host;
    s_pending_byte = PICO_ERROR_TIMEOUT;
    result = CLI_ERR;
  } else
#endif
  {
    if (s_pending_byte == PICO_ERROR_TIMEOUT) {
      s_pending_byte = getchar_timeout_us(0); /* kept for pico_serial_read() */
    }
    result = (s_pending_byte != PICO_ERROR_TIMEOUT) ? 1 : 0;
  }
  return result;
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
  cli_transport_ret_t result = CLI_ERR;

  (void)ctx;
  if (buf != NULL && len > 0) {
    for (cli_transport_buflen_t i = 0; i < len; i++) {
      putchar(buf[i]);
    }
    result = (cli_transport_ret_t)len;
  }
  return result;
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
#if LIB_PICO_STDIO_USB
    s_usb_host = stdio_usb_connected();
#endif
  }
}
