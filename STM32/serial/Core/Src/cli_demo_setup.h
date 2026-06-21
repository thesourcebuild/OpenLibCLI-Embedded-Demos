/**
 * @file cli_demo_setup.h
 * @brief OpenLibCLI serial demo lifecycle API for STM32 HAL.
 *
 * Provides setup, polling, and shutdown entry points for a single UART-backed
 * CLI session on STM32 targets using HAL.
 */

#ifndef OPENLIBCLI_STM32_CLI_DEMO_SETUP_H
#define OPENLIBCLI_STM32_CLI_DEMO_SETUP_H

#ifdef __cplusplus
extern "C" {
#endif

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#include "main.h"

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

/* No public demo-specific types. */

/*=======================================================================================
 * External Data Variables
 *=======================================================================================*/

/* No externally visible data variables. */

/*=======================================================================================
 * Public Function Prototypes
 *=======================================================================================*/

/**
 * @brief Initialize and start the STM32 serial CLI demo session.
 *
 * @param[in] huart UART handle used by the serial transport.
 */
void cli_demo_setup(UART_HandleTypeDef *huart);

/**
 * @brief Execute one non-blocking poll iteration of the active session.
 */
void cli_demo_poll(void);

/**
 * @brief Shut down the active session and release the CLI instance.
 */
void cli_demo_shutdown(void);

/**
 * @brief Return whether a session is currently active.
 *
 * @return 1 when running, otherwise 0.
 */
uint8_t cli_demo_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENLIBCLI_STM32_CLI_DEMO_SETUP_H */

/*################################### END OF FILE ######################################*/
