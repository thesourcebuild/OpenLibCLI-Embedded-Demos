#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include "cli_transport_serial.h"

/* Console UART port */
#define CLI_UART_NUM CONFIG_ESP_CONSOLE_UART_NUM
#define CLI_UART_BAUD 115200
#define CLI_UART_BUF_SIZE 256

static bool s_uart_installed = false;

static cli_transport_ret_t esp_serial_available(void *ctx)
{
  (void)ctx;
  if (!s_uart_installed) return 0;
  size_t len;
  esp_err_t err = uart_get_buffered_data_len(CLI_UART_NUM, &len);
  return (err == ESP_OK && len > 0) ? 1 : 0;
}

static cli_transport_ret_t esp_serial_read(void *ctx)
{
  (void)ctx;
  if (!s_uart_installed) return CLI_ERR;
  uint8_t c;
  int len = uart_read_bytes(CLI_UART_NUM, &c, 1, 0);
  return (len > 0) ? (cli_transport_ret_t)c : CLI_ERR;
}

static cli_transport_ret_t esp_serial_write(void *ctx, const uint8_t *buf, cli_transport_buflen_t len)
{
  (void)ctx;
  if (!s_uart_installed || buf == NULL || len == 0) {
    return CLI_ERR;
  }
  uart_write_bytes(CLI_UART_NUM, (const char *)buf, len);
  return (cli_transport_ret_t)len;
}

static cli_transport_ret_t esp_serial_flush(void *ctx)
{
  (void)ctx;
  if (!s_uart_installed) return CLI_ERR;
  uart_wait_tx_done(CLI_UART_NUM, portMAX_DELAY);
  return CLI_OK;
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

  /* Install UART driver so uart_get_buffered_data_len() etc. work.
   * May already be installed by boot/console code (ESP_ERR_INVALID_STATE). */
  esp_err_t err = uart_driver_install(CLI_UART_NUM, CLI_UART_BUF_SIZE * 2, 0, 0, NULL, 0);
  if (err == ESP_ERR_INVALID_STATE) {
    s_uart_installed = true;
  } else {
    ESP_ERROR_CHECK(err);
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
    s_uart_installed = true;
  }
}
