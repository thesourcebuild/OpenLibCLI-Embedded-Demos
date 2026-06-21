/**
 * @file cli_transport_serial.c
 * @brief STM32 HAL UART transport adapter implementation for OpenLibCLI.
 */

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#include "main.h"

#include <string.h>

#include "cli_transport_serial.h"

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

static uint8_t s_stm32_pending_byte;
static uint8_t s_stm32_pending_valid;

/*=======================================================================================
 * Private Function Prototypes
 *=======================================================================================*/

static cli_transport_ret_t stm32_serial_available(void *ctx);
static cli_transport_ret_t stm32_serial_read(void *ctx);
static cli_transport_ret_t stm32_serial_write(void *ctx, const uint8_t *buf, cli_transport_buflen_t len);
static cli_transport_ret_t stm32_serial_flush(void *ctx);

/*=======================================================================================
 * Public Functions
 *=======================================================================================*/

/**
 * @brief Initialize UART-backed serial transport callbacks.
 *
 * @param[out] transport CLI transport descriptor to initialize.
 * @param[in]  huart     STM32 HAL UART handle used for TX/RX operations.
 */

void cli_transport_serial_init(cli_transport_struct_t *transport, UART_HandleTypeDef *huart) {
  if ((transport != NULL) && (huart != NULL)) {
    memset(transport, 0, sizeof(*transport));
    transport->kind = CLI_TRANSPORT_SERIAL;
    transport->available = stm32_serial_available;
    transport->read = stm32_serial_read;
    transport->write = stm32_serial_write;
    transport->flush = stm32_serial_flush;
    transport->ctx = (void *)huart;
  }
}

/*=======================================================================================
 * Private Functions
 *=======================================================================================*/

/**
 * @brief Check if one UART byte is available without discarding it.
 *
 * @param[in] ctx Opaque transport context (UART handle).
 * @return Positive when a byte is ready, zero for no data, or CLI_ERR on failure.
 */

static cli_transport_ret_t stm32_serial_available(void *ctx) {
  UART_HandleTypeDef *huart = (UART_HandleTypeDef *)ctx;
  cli_transport_ret_t rc = 0;

  if (huart == NULL) {
    rc = CLI_ERR;
  } else if (s_stm32_pending_valid) {
    rc = 1;
  } else {
    HAL_StatusTypeDef st = HAL_UART_Receive(huart, &s_stm32_pending_byte, 1U, 0U);
    if (st == HAL_OK) {
      s_stm32_pending_valid = 1U;
      rc = 1;
    } else if (st != HAL_TIMEOUT) {
      rc = CLI_ERR;
    }
  }

  return rc;
}

/**
 * @brief Read one byte from UART in non-blocking mode.
 *
 * @param[in] ctx Opaque transport context (UART handle).
 * @return Byte value 0..255, or CLI_ERR on failure/no data.
 */

static cli_transport_ret_t stm32_serial_read(void *ctx) {
  UART_HandleTypeDef *huart = (UART_HandleTypeDef *)ctx;
  cli_transport_ret_t rc = CLI_ERR;
  uint8_t byte;

  if (huart != NULL) {
    if (s_stm32_pending_valid) {
      s_stm32_pending_valid = 0U;
      rc = s_stm32_pending_byte;
    } else if (HAL_UART_Receive(huart, &byte, 1U, 0U) == HAL_OK) {
      rc = byte;
    }
  }

  return rc;
}

/**
 * @brief Write exactly @p len bytes to UART.
 *
 * @param[in] ctx Opaque transport context (UART handle).
 * @param[in] buf Source byte buffer.
 * @param[in] len Number of bytes to transmit.
 * @return Bytes written on success, or CLI_ERR on failure.
 */

static cli_transport_ret_t stm32_serial_write(void *ctx, const uint8_t *buf, cli_transport_buflen_t len) {
  UART_HandleTypeDef *huart = (UART_HandleTypeDef *)ctx;
  cli_transport_ret_t rc = len;

  if ((huart == NULL) || (buf == NULL) || (len == 0)) {
    rc = CLI_ERR;
  } else {
    if (HAL_UART_Transmit(huart, (uint8_t *)buf, len, 1000U) != HAL_OK) {
      rc = CLI_ERR;
    }
  }

  return rc;
}

/**
 * @brief Flush serial transport (no-op for STM32 HAL UART polling mode).
 *
 * @param[in] ctx Opaque transport context.
 * @return CLI_OK always.
 */

static cli_transport_ret_t stm32_serial_flush(void *ctx) {
  (void)ctx;
  return CLI_OK;
}

/*################################### END OF FILE ######################################*/
