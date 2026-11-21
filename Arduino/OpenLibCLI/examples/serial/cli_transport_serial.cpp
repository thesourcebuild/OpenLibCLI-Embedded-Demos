/**
 * @file cli_transport_serial.cpp
 * @brief Arduino Serial transport adapter implementation for open-libcli.
 *
 * Implements byte read/write/flush callbacks backed by Arduino @c Serial and
 * binds them into a @c cli_transport_struct_t descriptor.
 *
 * @author Muhammad Hassaan Shah
 * @date 2026-04-19
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 thesourcebuild and Muhammad Hassaan Shah
 *
 * SPDX-License-Identifier: MIT
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

/* How a terminal holding the port open is seen: the same test the core's own write()
 * makes before it sends, read without side effects (no operator bool: it waits or runs
 * the USB stack on several cores).
 * - ESP32 USB-Serial/JTAG port (HWCDC: ESP32-C3/C6/H2, and S3 in its default USB mode):
 *   not at all. The peripheral uses DTR / RTS only for chip reset and boot strapping and
 *   reports neither to firmware, so the port counts as always open. On an ESP32-S2/S3,
 *   "USB Mode: USB-OTG (TinyUSB)" puts Serial on the native USB stack, which reports DTR.
 * - TinyUSB stacks: tud_cdc_connected() (DTR), which the writes of the ESP32 native USB
 *   port, the UNO R4 Minima / Nano R4 port and the Arduino-Pico (Pico SDK USB) port test.
 *   Not the UNO R4 SerialUSB.dtr(): it also turns off that core's own check for good.
 * - Adafruit TinyUSB library: Serial.dtr(), the tud_cdc_n_connected() of Serial's own CDC
 *   interface that its write() tests.
 * - STM32duino with Serial on USB ("USB support: CDC (generic 'Serial' supersede U(S)ART)"):
 *   CDC_connected(), what its write() tests: the device is configured and DTR is set (DTR
 *   alone stays set when the cable is pulled). Its Serial.flush() only waits, without a
 *   limit, for queued output the core sends by itself: the transport has no flush there.
 * - Arduino mbed cores (e.g. the mbed RP2040 core): the USB serial's connected(), the
 *   terminal flag its write() tests (DTR, cleared on a USB reset too).
 * - ATmega32U4 and SAMD native USB: DTR or RTS (line state > 0), the "port open" test of
 *   the ATmega32U4 write() and of both cores' operator bool.
 * - Anything else (a UART): always open. */
#define CLI_SERIAL_HOST_ALWAYS     0
#define CLI_SERIAL_HOST_TINYUSB    1
#define CLI_SERIAL_HOST_PORT_DTR   2
#define CLI_SERIAL_HOST_MBED       3
#define CLI_SERIAL_HOST_LINE_STATE 4
#define CLI_SERIAL_HOST_STM32_CDC  5

#if defined(ARDUINO_ARCH_ESP32) && ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
#define CLI_SERIAL_HOST CLI_SERIAL_HOST_ALWAYS
#elif defined(ARDUINO_ARCH_ESP32) && ARDUINO_USB_CDC_ON_BOOT
#define CLI_SERIAL_HOST CLI_SERIAL_HOST_TINYUSB
#include "esp32-hal-tinyusb.h"
#elif defined(ARDUINO_ARCH_ESP32)
#define CLI_SERIAL_HOST CLI_SERIAL_HOST_ALWAYS
#elif defined(USE_TINYUSB)
#define CLI_SERIAL_HOST CLI_SERIAL_HOST_PORT_DTR
#elif defined(ARDUINO_ARCH_STM32) && defined(USBCON) && defined(USBD_USE_CDC) && \
    !defined(DISABLE_GENERIC_SERIALUSB)
#define CLI_SERIAL_HOST CLI_SERIAL_HOST_STM32_CDC
#include <usbd_cdc_if.h>
#elif defined(ARDUINO_ARCH_MBED) && defined(SERIAL_CDC)
#define CLI_SERIAL_HOST CLI_SERIAL_HOST_MBED
#include <USB/PluggableUSBSerial.h>
#elif (defined(ARDUINO_ARCH_RENESAS) || defined(ARDUINO_ARCH_RP2040)) && !defined(ARDUINO_ARCH_MBED) && \
    !defined(NO_USB)
#define CLI_SERIAL_HOST CLI_SERIAL_HOST_TINYUSB
#include <tusb.h>
#elif (defined(ARDUINO_ARCH_AVR) || defined(ARDUINO_ARCH_SAMD)) && defined(USBCON)
#define CLI_SERIAL_HOST CLI_SERIAL_HOST_LINE_STATE
#else
#define CLI_SERIAL_HOST CLI_SERIAL_HOST_ALWAYS
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
#if CLI_SERIAL_HOST != CLI_SERIAL_HOST_STM32_CDC
static cli_transport_ret_t arduino_serial_flush(void *ctx);
#endif
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
#if CLI_SERIAL_HOST == CLI_SERIAL_HOST_STM32_CDC
    transport->flush = NULL; /* see CLI_SERIAL_HOST: writes are sent without one */
#else
    transport->flush = arduino_serial_flush;
#endif

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
    bool progress = true;

    /* The rest of the buffer per call: the single-byte write() does not return 1 on every
     * core (mbed returns the byte's value). A count above what was asked for is a failure
     * (SAMD returns (size_t)-1). */
    while (written < len && progress) {
      size_t sent = Serial.write(&buf[written], (size_t)(len - written));

      progress = (sent > 0U && sent <= (size_t)(len - written));
      if (progress) {
        written = (cli_transport_buflen_t)(written + sent);
      }
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

#if CLI_SERIAL_HOST != CLI_SERIAL_HOST_STM32_CDC
static cli_transport_ret_t arduino_serial_flush(void *ctx) {
  (void)ctx;
  Serial.flush();
  return 0;
}
#endif

#if CLI_SERIAL_HOST == CLI_SERIAL_HOST_LINE_STATE
/* The native USB serial port (Serial_): open while the host sets DTR or RTS. Both are
 * stored line-state bits; operator bool tests the same, but adds delay(10). Inline: a
 * board whose Serial is a UART does not call it. */
static inline bool arduino_serial_port_open(Serial_ &port) {
  return port.dtr() || port.rts();
}

/* A board whose Serial is a UART (e.g. the Arduino Zero's EDBG port): always open. */
template <typename Port>
static bool arduino_serial_port_open(Port &port) {
  (void)port;
  return true;
}
#endif

/* Whether a terminal holds the port open now (see CLI_SERIAL_HOST). */
static bool arduino_serial_host_present(void) {
#if CLI_SERIAL_HOST == CLI_SERIAL_HOST_TINYUSB
  bool host = tud_cdc_connected(); /* Serial is CDC interface 0 */
#elif CLI_SERIAL_HOST == CLI_SERIAL_HOST_PORT_DTR
  bool host = (Serial.dtr() != 0);
#elif CLI_SERIAL_HOST == CLI_SERIAL_HOST_MBED
  bool host = _SerialUSB.connected(); /* the USB serial behind Serial */
#elif CLI_SERIAL_HOST == CLI_SERIAL_HOST_STM32_CDC
  bool host = CDC_connected();
#elif CLI_SERIAL_HOST == CLI_SERIAL_HOST_LINE_STATE
  bool host = arduino_serial_port_open(Serial);
#else
  bool host = true;
#endif

  return host;
}

#if defined(ARDUINO_ARCH_RENESAS) && (CLI_SERIAL_HOST == CLI_SERIAL_HOST_TINYUSB)
/* The UNO R4 core's read() refuses input while no terminal holds the port, so
 * arduino_serial_available() cannot drain it: it is dropped here as it arrives instead.
 * TinyUSB calls this from tud_task() (mostly run by the USB interrupt on this core), the
 * same place that fills the receive FIFO. */
extern "C" void tud_cdc_rx_cb(uint8_t itf) {
  if (!tud_cdc_n_connected(itf)) {
    tud_cdc_n_read_flush(itf);
  }
}
#endif

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
