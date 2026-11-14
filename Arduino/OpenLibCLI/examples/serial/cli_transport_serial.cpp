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

/* How a terminal holding the port open (DTR) is seen:
 * - ESP32 USB-Serial/JTAG port (HWCDC: ESP32-C3/C6/H2, and S3 in its default USB mode):
 *   not at all. The peripheral uses DTR / RTS only for chip reset and boot strapping and
 *   reports neither to firmware, so the port counts as always open. On an ESP32-S2/S3,
 *   "USB Mode: USB-OTG (TinyUSB)" puts Serial on the native USB stack, which reports DTR.
 * - ESP32 native USB (TinyUSB): the CDC interface's DTR, which its writes also follow
 *   (Serial's operator bool needs RTS as well).
 * - Any other core: arduino_serial_port_dtr(). */
#define CLI_SERIAL_HOST_ALWAYS  0
#define CLI_SERIAL_HOST_TINYUSB 1
#define CLI_SERIAL_HOST_PORT    2

#if defined(ARDUINO_ARCH_ESP32) && ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
#define CLI_SERIAL_HOST CLI_SERIAL_HOST_ALWAYS
#elif defined(ARDUINO_ARCH_ESP32) && ARDUINO_USB_CDC_ON_BOOT
#define CLI_SERIAL_HOST CLI_SERIAL_HOST_TINYUSB
#include "esp32-hal-tinyusb.h"
#else
#define CLI_SERIAL_HOST CLI_SERIAL_HOST_PORT
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

/* Whether a terminal holds the port open, as last seen; a change not reported yet. */
static bool s_host;
static bool s_host_changed;

/*=======================================================================================
 * Private Function Prototypes
 *=======================================================================================*/

static cli_transport_ret_t arduino_serial_available(void *ctx);
static cli_transport_ret_t arduino_serial_read(void *ctx);
static cli_transport_ret_t arduino_serial_write(void *ctx, const uint8_t *buf, cli_transport_buflen_t len);
static cli_transport_ret_t arduino_serial_flush(void *ctx);
static bool arduino_serial_host_present(void);
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

    s_host = arduino_serial_host_present();
    s_host_changed = false;
  }
}

/*=======================================================================================
 * Private Functions
 *=======================================================================================*/

static cli_transport_ret_t arduino_serial_available(void *ctx) {
  cli_transport_ret_t result = 0;

  (void)ctx;
  /* Read on every call, so a terminal that closes and another that opens the port right
   * after are seen as a change, not as the same terminal. */
  arduino_serial_check_host();

  if (s_host_changed) {
    /* A terminal closed or opened the port: the session ends, so the next terminal starts
     * at the banner and login instead of inside the previous user's session. */
    s_host_changed = false;
    result = CLI_ERR;
  } else if (s_host) {
    result = (Serial.available() > 0) ? 1 : 0;
  } else {
    /* No terminal: what arrives (a program writing to the port without holding it open)
     * is nobody's input, and must not become the next terminal's first login line. */
    int pending = Serial.available();

    while (pending > 0) {
      (void)Serial.read();
      pending--;
    }
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
  /* Output of a session whose terminal has gone (a change not reported yet) is dropped,
   * also when another terminal has already opened the port; the next available() ends
   * the session. */
  arduino_serial_check_host();
  if (s_host && !s_host_changed) {
    while (written < len && Serial.write(buf[written]) == 1) {
      written++;
    }
    if (written < len) {
      /* Native-USB ports refuse output once the terminal has gone: then it is dropped as
       * above. Anything else is a write failure for the engine to handle. */
      arduino_serial_check_host();
      if (s_host && !s_host_changed) {
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

#if CLI_SERIAL_HOST == CLI_SERIAL_HOST_PORT
/* DTR, where the port's class reports it: the native-USB serial of the AVR (ATmega32U4),
 * SAMD and RP2040 cores and Adafruit TinyUSB. It is a stored flag there, unlike operator
 * bool, which the ATmega32U4 core follows with delay(10). The int argument makes the call
 * prefer this overload; it drops out when the class has no dtr(). */
template <typename Port>
static auto arduino_serial_port_dtr(Port &port, int) -> decltype(static_cast<bool>(port.dtr())) {
  return static_cast<bool>(port.dtr());
}

/* Any other port: operator bool (always true on a UART). */
template <typename Port>
static bool arduino_serial_port_dtr(Port &port, long) {
  return static_cast<bool>(port);
}
#endif

/* Whether a terminal holds the port open now (see CLI_SERIAL_HOST). */
static bool arduino_serial_host_present(void) {
#if CLI_SERIAL_HOST == CLI_SERIAL_HOST_TINYUSB
  bool host = tud_cdc_connected(); /* Serial is CDC interface 0 */
#elif CLI_SERIAL_HOST == CLI_SERIAL_HOST_PORT
  bool host = arduino_serial_port_dtr(Serial, 0);
#else
  bool host = true;
#endif

  return host;
}

/* Read whether a terminal holds the port open; a change is reported by the next
 * arduino_serial_available(). */
static void arduino_serial_check_host(void) {
  bool host = arduino_serial_host_present();

  if (host != s_host) {
    s_host = host;
    s_host_changed = true;
  }
}
#endif
/*################################### END OF FILE ######################################*/
