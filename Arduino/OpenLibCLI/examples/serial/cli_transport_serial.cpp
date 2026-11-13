/**
 * @file cli_transport_serial.cpp
 * @brief Arduino Serial transport adapter implementation for open-libcli.
 *
 * Implements byte read/write/flush callbacks backed by Arduino @c Serial and
 * binds them into a @c cli_transport_struct_t descriptor.
 *
 * @author open-libcli Project
 * @date 2026-04-19
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 open-libcli Project. All rights reserved.
 */

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#if defined(ARDUINO)
#include <Arduino.h>
#include <string.h>

#include "cli_transport_serial.hpp"

/*=======================================================================================
 * Private Defines
 *=======================================================================================*/

#ifndef CLI_SERIAL_HOST_CHECK_MS
/* How often the port's host state is read: on some cores the query itself waits (the
 * ATmega32U4 USB core's Serial_::operator bool() spends 10 ms in delay()). */
#define CLI_SERIAL_HOST_CHECK_MS 250UL
#endif

/*=======================================================================================
 * Private Macros
 *=======================================================================================*/

/* No private function-like macros. */

/*=======================================================================================
 * Private Types
 *=======================================================================================*/

/* No private types. */

/*=======================================================================================
 * External Data Variables
 *=======================================================================================*/

/* No externally visible data variables. */

/*=======================================================================================
 * Private Variables
 *=======================================================================================*/

/* Whether a terminal holds the port open, as last seen: native-USB boards report a
 * terminal opening or closing the port (DTR); a UART always reads as open. */
static bool s_host;
static uint32_t s_host_checked_ms;

/*=======================================================================================
 * Private Function Prototypes
 *=======================================================================================*/

static cli_transport_ret_t arduino_serial_available(void *ctx);
static cli_transport_ret_t arduino_serial_read(void *ctx);
static cli_transport_ret_t arduino_serial_write(void *ctx, const uint8_t *buf, cli_transport_buflen_t len);
static cli_transport_ret_t arduino_serial_flush(void *ctx);

/*=======================================================================================
 * Public Functions
 *=======================================================================================*/

void cli_transport_serial_init(cli_transport_struct_t *transport) {
  if (transport) {
    memset(transport, 0, sizeof(*transport));
    transport->kind = CLI_TRANSPORT_SERIAL;

    transport->available = arduino_serial_available;
    transport->read = arduino_serial_read;
    transport->write = arduino_serial_write;
    transport->flush = arduino_serial_flush;

    s_host = (bool)Serial;
    s_host_checked_ms = (uint32_t)millis();
  }
}

/*=======================================================================================
 * Private Functions
 *=======================================================================================*/

static cli_transport_ret_t arduino_serial_available(void *ctx) {
  cli_transport_ret_t result = 0;
  bool host_changed = false;
  uint32_t now_ms = (uint32_t)millis();

  (void)ctx;
  if ((uint32_t)(now_ms - s_host_checked_ms) >= CLI_SERIAL_HOST_CHECK_MS) {
    bool host = (bool)Serial;

    s_host_checked_ms = now_ms;
    host_changed = (host != s_host);
    s_host = host;
  }

  if (host_changed) {
    /* A terminal closed or opened the port: the session ends, so the next terminal starts
     * at the banner and login instead of inside the previous user's session. */
    result = CLI_ERR;
  } else {
    result = (Serial.available() > 0) ? 1 : 0;
  }

  return result;
}

static cli_transport_ret_t arduino_serial_read(void *ctx) {
  (void)ctx;
  int byte = Serial.read();
  return (byte >= 0) ? (cli_transport_ret_t)byte : CLI_ERR;
}

static cli_transport_ret_t arduino_serial_write(void *ctx, const uint8_t *buf, cli_transport_buflen_t len) {
  (void)ctx;
  cli_transport_buflen_t written = 0;

  while (written < len) {
    if (Serial.write(buf[written]) != 1) {
      break;
    }
    written++;
  }

  return written;
}

static cli_transport_ret_t arduino_serial_flush(void *ctx) {
  (void)ctx;
  Serial.flush();
  return 0;
}
#endif
/*################################### END OF FILE ######################################*/
