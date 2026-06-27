/**
 * @file cli_version.h
 * @brief Version information for the OpenLibCLI Library.
 *
 * AUTO-GENERATED — do not edit manually.
 * Source: version file + git describe
 *
 * Defines the library version as individual numeric components, as a packed
 * 32-bit word, and as a human-readable string.
 *
 * @copyright Copyright (c) 2026 Muhammad Hassaan Shah
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENLIBCLI_VERSION_H
#define OPENLIBCLI_VERSION_H

/* C++ detection */
#ifdef __cplusplus
extern "C" {
#endif

/*=======================================================================================
 * Includes
 *=======================================================================================*/

/* No public includes required. */

/*=======================================================================================
 * Public Defines
 *=======================================================================================*/

/** @defgroup CLI_Version_Defines Version Defines
 *  @{
 */

/** @brief Major version number. Incremented on breaking API changes. */
#define CLI_VERSION_MAJOR 0

/** @brief Minor version number. Incremented on backward-compatible additions. */
#define CLI_VERSION_MINOR 1

/** @brief Revision / patch number. Incremented on bug-fix releases. */
#define CLI_VERSION_REVISION 3

/**
 * @brief Packed version word: 0x00MMmmrr (major, minor, revision).
 *
 * Useful for compile-time comparisons:
 * @code
 *   #if CLI_VERSION >= 0x00010000
 * @endcode
 */
#define CLI_VERSION ((CLI_VERSION_MAJOR << 16) | (CLI_VERSION_MINOR << 8) | CLI_VERSION_REVISION)

/** @} */

/*=======================================================================================
 * Public Macros
 *=======================================================================================*/

/** @defgroup CLI_Version_Macros Version Macros
 *  @{
 */

/**
 * @brief Stringify helper (step 1 of 2).
 * @param v Token to convert to a string literal.
 */
#define CLI_VERSION_STR(v) #v

/**
 * @brief Expand-then-stringify helper (step 2 of 2).
 * @param v Token to expand before stringification.
 */
#define CLI_VERSION_XSTR(v) CLI_VERSION_STR(v)

/**
 * @brief Human-readable version string, for example @c "85c5f35-dirty".
 *
 * Built from the individual numeric components via the stringify helpers.
 */
#define CLI_VERSION_STRING                                                                         \
  CLI_VERSION_XSTR(CLI_VERSION_MAJOR)                                                              \
  "." CLI_VERSION_XSTR(CLI_VERSION_MINOR) "." CLI_VERSION_XSTR(CLI_VERSION_REVISION)

/** @} */

/*=======================================================================================
 * Public Types
 *=======================================================================================*/

/** @defgroup CLI_Version_Types Version Types
 *  @{
 */

/* No public types declared in this header. */

/** @} */

/*=======================================================================================
 * External Data Variables
 *=======================================================================================*/

/** @defgroup CLI_Version_ExternVars Version External Variables
 *  @{
 */

/* No public external data variables. */

/** @} */

/*=======================================================================================
 * Public Function Prototypes
 *=======================================================================================*/

/** @defgroup CLI_Version_Functions Version Public Functions
 *  @{
 */

/* No public functions declared in this header. */

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* OPENLIBCLI_VERSION_H */

/*################################### END OF FILE ######################################*/
