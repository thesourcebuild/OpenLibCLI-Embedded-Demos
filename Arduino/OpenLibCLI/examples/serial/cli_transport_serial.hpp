/**
 * @file cli_transport_serial.hpp
 * @brief Arduino Serial transport adapter for open-libcli.
 *
 * Declares transport initialisation for wiring Arduino @c Serial I/O into
 * @c cli_transport_struct_t.
 *
 * @author open-libcli Project
 * @date 2026-04-19
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 open-libcli Project. All rights reserved.
 */

#ifndef OPENLIBCLI_ARDUINO_CLI_TRANSPORT_SERIAL_HPP
#define OPENLIBCLI_ARDUINO_CLI_TRANSPORT_SERIAL_HPP

/* C++ detection */
#ifdef __cplusplus
extern "C" {
#endif

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#include <cli.h>

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

/* No public transport-specific types. */

/*=======================================================================================
 * External Data Variables
 *=======================================================================================*/

/* No externally visible data variables. */

/*=======================================================================================
 * Public Function Prototypes
 *=======================================================================================*/

void cli_transport_serial_init(cli_transport_struct_t *transport);

#ifdef __cplusplus
}
#endif

#endif /* OPENLIBCLI_ARDUINO_CLI_TRANSPORT_SERIAL_HPP */

/*################################### END OF FILE ######################################*/
