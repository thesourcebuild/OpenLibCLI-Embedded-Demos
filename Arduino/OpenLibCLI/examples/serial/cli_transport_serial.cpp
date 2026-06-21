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

/* No private constants. */

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

/* No private file-scope variables. */

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
  if (!transport) {
    return;
  }

  memset(transport, 0, sizeof(*transport));
  transport->kind = CLI_TRANSPORT_SERIAL;

  transport->available = arduino_serial_available;
  transport->read = arduino_serial_read;
  transport->write = arduino_serial_write;
  transport->flush = arduino_serial_flush;
}

/*=======================================================================================
 * Private Functions
 *=======================================================================================*/

static cli_transport_ret_t arduino_serial_available(void *ctx) {
  (void)ctx;
  return (Serial.available() > 0) ? 1 : 0;
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
