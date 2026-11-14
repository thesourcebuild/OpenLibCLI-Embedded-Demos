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
/* How often the port's host state is read while a terminal is attached: on some cores the
 * query itself waits (the ATmega32U4 USB core's Serial_::operator bool() spends 10 ms in
 * delay()). */
#define CLI_SERIAL_HOST_CHECK_MS 250UL
#endif

/* (bool)Serial tells whether a terminal holds the port open (DTR) on native-USB boards
 * and is always true on a UART. The ESP32 USB-Serial/JTAG port (HWCDC: ESP32-C3/C6/H2,
 * and S3 in its default USB mode) has no such state: the peripheral uses DTR / RTS only
 * for chip reset and boot strapping and reports neither to firmware, and its operator
 * bool follows USB traffic, not the terminal. There the port counts as always open. On
 * an ESP32-S2/S3, "USB Mode: USB-OTG (TinyUSB)" puts Serial on the native USB stack,
 * which follows DTR. */
#if defined(ARDUINO_ARCH_ESP32) && ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
#define CLI_SERIAL_HOST_DETECTION 0
#else
#define CLI_SERIAL_HOST_DETECTION 1
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

/* Whether a terminal holds the port open, as last seen; a change not reported yet; when
 * the state was read. */
static bool s_host;
static bool s_host_changed;
static uint32_t s_host_checked_ms;

/*=======================================================================================
 * Private Function Prototypes
 *=======================================================================================*/

static cli_transport_ret_t arduino_serial_available(void *ctx);
static cli_transport_ret_t arduino_serial_read(void *ctx);
static cli_transport_ret_t arduino_serial_write(void *ctx, const uint8_t *buf, cli_transport_buflen_t len);
static cli_transport_ret_t arduino_serial_flush(void *ctx);
static void arduino_serial_check_host(void);

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

    s_host = true;
    s_host_changed = false;
    arduino_serial_check_host();
    s_host_changed = false; /* the state at start is no change */
  }
}

/*=======================================================================================
 * Private Functions
 *=======================================================================================*/

static cli_transport_ret_t arduino_serial_available(void *ctx) {
  cli_transport_ret_t result = 0;

  (void)ctx;
  /* Without a terminal the state is read on every call (there is nothing else to do then),
   * so input from a terminal that just opened the port never reaches the old session. */
  if (!s_host || (uint32_t)((uint32_t)millis() - s_host_checked_ms) >= CLI_SERIAL_HOST_CHECK_MS) {
    arduino_serial_check_host();
  }

  if (s_host_changed) {
    /* A terminal closed or opened the port: the session ends, so the next terminal starts
     * at the banner and login instead of inside the previous user's session. */
    s_host_changed = false;
    result = CLI_ERR;
  } else if (s_host) {
    result = (Serial.available() > 0) ? 1 : 0;
  } else {
    /* No terminal: nothing to read. */
  }

  return result;
}

static cli_transport_ret_t arduino_serial_read(void *ctx) {
  (void)ctx;
  int byte = Serial.read();
  return (byte >= 0) ? (cli_transport_ret_t)byte : CLI_ERR;
}

static cli_transport_ret_t arduino_serial_write(void *ctx, const uint8_t *buf, cli_transport_buflen_t len) {
  cli_transport_ret_t result = (cli_transport_ret_t)len; /* no terminal: nowhere to go */
  cli_transport_buflen_t written = 0;

  (void)ctx;
  if (s_host) {
    while (written < len && Serial.write(buf[written]) == 1) {
      written++;
    }
    if (written < len) {
      /* Native-USB ports refuse output once the terminal has gone: then it is dropped like
       * all output without a terminal, and the next available() ends the session. Anything
       * else is a write failure for the engine to handle. */
      arduino_serial_check_host();
      if (s_host) {
        result = (cli_transport_ret_t)written;
      }
    }
  }

  return result;
}

static cli_transport_ret_t arduino_serial_flush(void *ctx) {
  (void)ctx;
  Serial.flush();
  return 0;
}

/* Read whether a terminal holds the port open; a change is reported by the next
 * arduino_serial_available(). */
static void arduino_serial_check_host(void) {
#if CLI_SERIAL_HOST_DETECTION
  bool host = (bool)Serial;

  if (host != s_host) {
    s_host = host;
    s_host_changed = true;
  }
#endif
  s_host_checked_ms = (uint32_t)millis();
}
#endif
/*################################### END OF FILE ######################################*/
