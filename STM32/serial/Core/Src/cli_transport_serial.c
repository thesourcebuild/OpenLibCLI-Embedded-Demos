/**
 * @file cli_transport_serial.c
 * @brief STM32 HAL UART transport adapter implementation for OpenLibCLI.
 *
 * Received bytes are taken in by the UART interrupt (@c HAL_UART_Receive_IT()) and kept
 * in a circular buffer until the CLI reads them, so input that arrives while the CLI is
 * busy (sending a command's output) is not lost. Output is sent with the blocking
 * @c HAL_UART_Transmit().
 */

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#include "main.h"

#include <string.h>

#include "circular_buffer.h"
#include "cli_transport_serial.h"

/*=======================================================================================
 * Private Defines
 *=======================================================================================*/

/** @brief Bytes of received input held until the CLI reads them. They cover what arrives
 *         at 115200 baud while the longest output (the command list) is being sent. */
#define STM32_SERIAL_RX_BUFFER_SIZE 256U

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

static uint8_t s_stm32_rx_storage[STM32_SERIAL_RX_BUFFER_SIZE];
static circular_buffer_struct_t s_stm32_rx;      /**< Filled by the interrupt, read by the CLI. */
static uint8_t s_stm32_rx_byte;                  /**< Byte the HAL is receiving into. */
static UART_HandleTypeDef *volatile s_stm32_huart; /**< UART receiving into s_stm32_rx. */

/*=======================================================================================
 * Private Function Prototypes
 *=======================================================================================*/

static void stm32_serial_rx_start(UART_HandleTypeDef *huart);
static cli_transport_ret_t stm32_serial_available(void *ctx);
static cli_transport_ret_t stm32_serial_read(void *ctx);
static cli_transport_ret_t stm32_serial_write(void *ctx, const uint8_t *buf, cli_transport_buflen_t len);
static cli_transport_ret_t stm32_serial_flush(void *ctx);

/*=======================================================================================
 * Public Functions
 *=======================================================================================*/

/**
 * @brief Initialize UART-backed serial transport callbacks and start receiving.
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

    /* A reception left running by an earlier set-up is stopped before the buffer it fills
     * is emptied: the interrupt never sees the buffer half set up. */
    if (s_stm32_huart != NULL) {
      (void)HAL_UART_AbortReceive(s_stm32_huart);
    }
    (void)circular_buffer_init(&s_stm32_rx, s_stm32_rx_storage, sizeof(s_stm32_rx_storage));
    s_stm32_huart = huart;
    stm32_serial_rx_start(huart);
  }
}

/**
 * @brief HAL receive-complete callback (replaces the HAL's weak one): keeps the byte
 *        received and starts receiving the next one.
 *
 * Runs in the UART interrupt. A byte that arrives while the buffer is full (the CLI has
 * not read for a long time) is dropped.
 *
 * @param[in] huart UART whose reception completed.
 */

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
  if ((huart != NULL) && (huart == s_stm32_huart)) {
    (void)circular_buffer_put(&s_stm32_rx, s_stm32_rx_byte); /* full: the byte is dropped */
    stm32_serial_rx_start(huart);
  }
}

/**
 * @brief HAL error callback (replaces the HAL's weak one): keeps the reception going.
 *
 * Runs in the UART interrupt. An overrun ends the reception (the byte that came in
 * meanwhile is lost); a noise, framing or parity error does not (the byte is delivered
 * and the reception goes on). The reception is started again only when it ended.
 *
 * @param[in] huart UART that reported the error.
 */

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
  if ((huart != NULL) && (huart == s_stm32_huart) && (huart->RxState == HAL_UART_STATE_READY)) {
    stm32_serial_rx_start(huart);
  }
}

/*=======================================================================================
 * Private Functions
 *=======================================================================================*/

/**
 * @brief Start receiving one byte into @c s_stm32_rx_byte, interrupt driven.
 *
 * Called only when no reception is running: at set-up, and from the callbacks once one
 * has ended. The HAL refuses only a reception already running or an empty buffer, so the
 * status is not checked.
 *
 * @param[in] huart UART to receive from.
 */

static void stm32_serial_rx_start(UART_HandleTypeDef *huart) {
  (void)HAL_UART_Receive_IT(huart, &s_stm32_rx_byte, 1U);
}

/**
 * @brief Number of received bytes waiting to be read.
 *
 * @param[in] ctx Opaque transport context (UART handle).
 * @return Bytes waiting (zero for none), or CLI_ERR on failure.
 */

static cli_transport_ret_t stm32_serial_available(void *ctx) {
  cli_transport_ret_t rc = CLI_ERR;

  if (ctx != NULL) {
    rc = (cli_transport_ret_t)circular_buffer_count(&s_stm32_rx);
  }

  return rc;
}

/**
 * @brief Read one received byte without waiting.
 *
 * @param[in] ctx Opaque transport context (UART handle).
 * @return Byte value 0..255, or CLI_ERR on failure/no data.
 */

static cli_transport_ret_t stm32_serial_read(void *ctx) {
  cli_transport_ret_t rc = CLI_ERR;
  uint8_t byte;

  if ((ctx != NULL) && circular_buffer_get(&s_stm32_rx, &byte)) {
    rc = (cli_transport_ret_t)byte;
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
 * @brief Flush serial transport (no-op: transmission is blocking, nothing is held back).
 *
 * @param[in] ctx Opaque transport context.
 * @return CLI_OK always.
 */

static cli_transport_ret_t stm32_serial_flush(void *ctx) {
  (void)ctx;
  return CLI_OK;
}

/*################################### END OF FILE ######################################*/
