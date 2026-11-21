#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include "cli_transport_serial.h"

/* The CLI runs on the console port set in menuconfig ("ESP-STDIO" -> "Channel for console
 * output"): a UART, or the USB-Serial/JTAG port of the chips that have one. */
#if CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"
#define CLI_UART_NUM CONFIG_ESP_CONSOLE_UART_NUM
#define CLI_UART_BAUD CONFIG_ESP_CONSOLE_UART_BAUDRATE
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#if CONFIG_VFS_SUPPORT_IO
#include "driver/usb_serial_jtag_vfs.h"
#endif
/* Output that cannot be queued within this time is dropped: nothing reads the port (no
 * terminal, or the cable is unplugged). As with the console's own output on this port,
 * once output is dropped the next writes do not wait until one is queued again. */
#define CLI_USJ_TX_WAIT_MS 50
/* Output is queued at most one USB packet (64 bytes) at a time: the driver queues a write
 * whole or not at all, and a driver installed by other code may have a TX buffer smaller
 * than this transport's own (it needs at least one packet). The port sends one packet at
 * a time anyway. */
#define CLI_USJ_TX_CHUNK 64U
#else
#error "The CLI needs the console on a UART or on the USB-Serial/JTAG port: set menuconfig \"ESP-STDIO\" -> \"Channel for console output\" (USB CDC and None are not supported)."
#endif

#define CLI_SERIAL_BUF_SIZE 256

static bool s_port_ready = false;

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static bool s_usj_tx_stalled = false; /* the last output was dropped */
static bool s_usj_rx_pending = false; /* s_usj_rx_byte was taken by esp_serial_available() */
static uint8_t s_usj_rx_byte;
#endif

static cli_transport_ret_t esp_serial_available(void *ctx)
{
  cli_transport_ret_t result = 0;

  (void)ctx;
#if CONFIG_ESP_CONSOLE_UART
  size_t len = 0;

  if (s_port_ready && uart_get_buffered_data_len(CLI_UART_NUM, &len) == ESP_OK && len > 0)
  {
    result = 1;
  }
#else
  if (s_port_ready && !s_usj_rx_pending)
  {
    /* The driver cannot tell how much is waiting: a byte is read now, kept for read(). */
    s_usj_rx_pending = (usb_serial_jtag_read_bytes(&s_usj_rx_byte, 1, 0) > 0);
  }
  result = s_usj_rx_pending ? 1 : 0;
#endif
  return result;
}

static cli_transport_ret_t esp_serial_read(void *ctx)
{
  cli_transport_ret_t result = CLI_ERR;
  uint8_t c;

  (void)ctx;
#if CONFIG_ESP_CONSOLE_UART
  if (s_port_ready && uart_read_bytes(CLI_UART_NUM, &c, 1, 0) > 0)
  {
    result = (cli_transport_ret_t)c;
  }
#else
  if (s_usj_rx_pending)
  {
    result = (cli_transport_ret_t)s_usj_rx_byte;
    s_usj_rx_pending = false;
  }
  else if (s_port_ready && usb_serial_jtag_read_bytes(&c, 1, 0) > 0)
  {
    result = (cli_transport_ret_t)c;
  }
#endif
  return result;
}

static cli_transport_ret_t esp_serial_write(void *ctx, const uint8_t *buf, cli_transport_buflen_t len)
{
  cli_transport_ret_t result = CLI_ERR;

  (void)ctx;
  if (s_port_ready && buf != NULL && len > 0)
  {
#if CONFIG_ESP_CONSOLE_UART
    /* uart_write_bytes() queues all of it (blocking while the ring buffer is full) and
     * returns -1 on a parameter error: report that instead of claiming success. */
    int written = uart_write_bytes(CLI_UART_NUM, (const char *)buf, len);
    if (written > 0)
    {
      result = (cli_transport_ret_t)written;
    }
#else
    TickType_t wait = s_usj_tx_stalled ? 0 : pdMS_TO_TICKS(CLI_USJ_TX_WAIT_MS);
    cli_transport_buflen_t sent = 0;
    bool queued = true;

    /* A packet at a time (CLI_USJ_TX_CHUNK). */
    while (sent < len && queued)
    {
      size_t chunk = (size_t)(len - sent);

      if (chunk > CLI_USJ_TX_CHUNK)
      {
        chunk = CLI_USJ_TX_CHUNK;
      }
      queued = (usb_serial_jtag_write_bytes(&buf[sent], chunk, wait) > 0);
      if (queued)
      {
        sent = (cli_transport_buflen_t)(sent + chunk);
      }
    }
    s_usj_tx_stalled = !queued;
    result = (cli_transport_ret_t)len; /* queued, or dropped as no one reads it */
#endif
  }
  return result;
}

static cli_transport_ret_t esp_serial_flush(void *ctx)
{
  cli_transport_ret_t result = CLI_ERR;

  (void)ctx;
  if (s_port_ready)
  {
#if CONFIG_ESP_CONSOLE_UART
    uart_wait_tx_done(CLI_UART_NUM, portMAX_DELAY);
#else
    /* Bounded like the writes: a port no one reads never drains. */
    (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(CLI_USJ_TX_WAIT_MS));
#endif
    result = CLI_OK;
  }
  return result;
}

void cli_transport_serial_init(cli_transport_struct_t *transport)
{
  if (transport != NULL) {
    memset(transport, 0, sizeof(*transport));
    transport->kind = CLI_TRANSPORT_SERIAL;
    transport->available = esp_serial_available;
    transport->read = esp_serial_read;
    transport->write = esp_serial_write;
    transport->flush = esp_serial_flush;
    transport->ctx = NULL;
  }

#if CONFIG_ESP_CONSOLE_UART
  /* Install UART driver so uart_get_buffered_data_len() etc. work. It may already be
   * installed by other code (e.g. a console): then it is used as configured there. */
  if (uart_is_driver_installed(CLI_UART_NUM)) {
    s_port_ready = true;
  } else {
    ESP_ERROR_CHECK(uart_driver_install(CLI_UART_NUM, CLI_SERIAL_BUF_SIZE * 2, 0, 0, NULL, 0));
    /* The console's own line settings (menuconfig "UART console baud rate"). */
    uart_config_t uart_config = {
      .baud_rate  = CLI_UART_BAUD,
      .data_bits  = UART_DATA_8_BITS,
      .parity     = UART_PARITY_DISABLE,
      .stop_bits  = UART_STOP_BITS_1,
      .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(CLI_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(CLI_UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    s_port_ready = true;
  }
#else
  /* Interrupt-driven reads and writes need the driver. It may already be installed by
   * other code (e.g. a console): then it is used as configured there. */
  if (usb_serial_jtag_is_driver_installed()) {
    s_port_ready = true;
  } else {
    usb_serial_jtag_driver_config_t usj_config = {
      .tx_buffer_size = CLI_SERIAL_BUF_SIZE,
      .rx_buffer_size = CLI_SERIAL_BUF_SIZE,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj_config));
#if CONFIG_VFS_SUPPORT_IO
    /* printf() and logs then go through the driver as well, instead of writing to the
     * port's FIFO behind its back. */
    usb_serial_jtag_vfs_use_driver();
#endif
    s_port_ready = true;
  }
#endif
}
