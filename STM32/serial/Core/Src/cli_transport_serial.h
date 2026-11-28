/**
 * @file cli_transport_serial.h
 * @brief STM32 HAL UART transport adapter for OpenLibCLI.
 *
 * Declares transport initialization for wiring STM32 HAL UART into
 * @c cli_transport_struct_t.
 *
 * Input is received by the UART interrupt: its global interrupt must be enabled, with its
 * IRQ handler calling @c HAL_UART_IRQHandler() (CubeMX: NVIC Settings, "USARTx global
 * interrupt"). The transport implements @c HAL_UART_RxCpltCallback() and
 * @c HAL_UART_ErrorCallback(), so the application must not define them too.
 */

#ifndef OPENLIBCLI_STM32_CLI_TRANSPORT_SERIAL_H
#define OPENLIBCLI_STM32_CLI_TRANSPORT_SERIAL_H

#ifdef __cplusplus
extern "C" {
#endif

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#include "OpenLibCLI/cli.h"

/*=======================================================================================
 * Public Defines
 *=======================================================================================*/

/* No public constants. */

/*=======================================================================================
 * Public Macros
 *=======================================================================================*/

/* No public function-like macros. */

/*=======================================================================================
 * Public Types
 *=======================================================================================*/

typedef struct __UART_HandleTypeDef UART_HandleTypeDef;

/*=======================================================================================
 * External Data Variables
 *=======================================================================================*/

/* No externally visible data variables. */

/*=======================================================================================
 * Public Function Prototypes
 *=======================================================================================*/

/**
 * @brief Initialize serial transport vtable for the supplied UART handle and start
 *        receiving from it.
 *
 * One UART receives at a time: a later call moves the reception to its UART and discards
 * the input not read yet.
 *
 * @param[out] transport Transport descriptor to initialize.
 * @param[in]  huart     STM32 HAL UART handle used for TX/RX.
 */
void cli_transport_serial_init(cli_transport_struct_t *transport, UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif /* OPENLIBCLI_STM32_CLI_TRANSPORT_SERIAL_H */

/*################################### END OF FILE ######################################*/
