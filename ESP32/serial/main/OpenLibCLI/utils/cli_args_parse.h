/**
 * @file cli_args_parse.h
 * @brief Lightweight integer & floating-point parsing and formatting utilities.
 *
 * Provides two families of parse APIs:
 *   - `parse_*`       — hand-rolled, zero-libc dependency (~200 bytes each)
 *   - `strtol_parse_*` — backed by `strtol`/`strtoul`/`strtof`/`strtod`
 *                       (auto hex/octal detection, whitespace skips, float)
 *
 * All format functions write bounded output (no sprintf dependency).
 *
 * @copyright Copyright (c) 2026 Muhammad Hassaan Shah.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CLI_ARGS_PARSE_UTILS_H
#define CLI_ARGS_PARSE_UTILS_H

/* C++ detection */
#ifdef __cplusplus
extern "C" {
#endif

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*=======================================================================================
 * Public Defines
 *=======================================================================================*/

/** @defgroup CLI_ARGS_PARSE_Defines CLI Parse Utils Defines
 *  @{
 */

/** @brief No public defines declared in this header. */
#define CLI_ARGS_PARSE_NO_DEFINES (0U)

/** @} */

/*=======================================================================================
 * Public Macros
 *=======================================================================================*/

/** @defgroup CLI_ARGS_PARSE_Macros CLI Parse Utils Macros
 *  @{
 */

/**
 * @brief Return the smaller of two values.
 *
 * @param[in] a First value.
 * @param[in] b Second value.
 * @return The minimum of @p a and @p b.
 */
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/**
 * @brief Return the larger of two values.
 *
 * @param[in] a First value.
 * @param[in] b Second value.
 * @return The maximum of @p a and @p b.
 */
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

/**
 * @brief Clamp a value between a lower and upper bound.
 *
 * @param[in] x  Value to clamp.
 * @param[in] lo Lower bound.
 * @param[in] hi Upper bound (must be >= @p lo).
 * @return @p lo if @p x < @p lo, @p hi if @p x > @p hi, otherwise @p x.
 */
#ifndef CLAMP
#define CLAMP(x, lo, hi) (MAX((lo), MIN((x), (hi))))
#endif

/**
 * @brief Return the number of elements in a statically allocated array.
 *
 * @param[in] a Array expression (not a pointer).
 * @return Number of elements in @p a.
 */
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/** @} */

/*=======================================================================================
 * Public Types
 *=======================================================================================*/

/** @defgroup CLI_ARGS_PARSE_Types CLI Parse Utils Types
 *  @{
 */

/* No public types declared in this header. */

/** @} */

/*=======================================================================================
 * External Data Variables
 *=======================================================================================*/

/** @defgroup CLI_ARGS_PARSE_ExternVars CLI Parse Utils External Variables
 *  @{
 */

/* No externally visible data variables declared in this header. */

/** @} */

/*=======================================================================================
 * Public Function Prototypes — hand-rolled integer and float parsing
 *=======================================================================================*/

/** @defgroup CLI_ARGS_PARSE_HandRolled_Parse Hand-rolled parsers (zero libc)
 *  @{
 */

/**
 * @brief Parse a decimal string into a uint64_t.
 *
 * Rejects leading whitespace, leading sign, and trailing junk.
 * Returns false on overflow (wraparound detected via monotonicity check).
 *
 * @param[in]  s   NUL-terminated decimal string.
 * @param[out] out Parsed value on success; unchanged on failure.
 * @return true on success, false on empty string, invalid chars, or overflow.
 */
bool parse_u64(const char *s, uint64_t *out);

/**
 * @brief Parse an optionally signed decimal string into an int64_t.
 *
 * Leading '+' or '-' is accepted.  Rejects trailing junk and overflow.
 *
 * @param[in]  s   NUL-terminated decimal string (optional leading '+' or '-').
 * @param[out] out Parsed value on success.
 * @return true on success, false on failure.
 */
bool parse_i64(const char *s, int64_t *out);

/**
 * @brief Parse a hexadecimal string into a uint64_t.
 *
 * Accepts optional "0x" or "0X" prefix.  Upper and lower case hex digits
 * are both accepted.
 *
 * @param[in]  s   NUL-terminated hex string (e.g. "0xABCD" or "ABCD").
 * @param[out] out Parsed value on success.
 * @return true on success, false on failure.
 */
bool parse_hex_u64(const char *s, uint64_t *out);

/**
 * @brief Parse a decimal string into a uint32_t.
 *
 * @param[in]  s   NUL-terminated decimal string.
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds UINT32_MAX.
 */
bool parse_u32(const char *s, uint32_t *out);

/**
 * @brief Parse a signed decimal string into an int32_t.
 *
 * @param[in]  s   NUL-terminated decimal string (optional leading '+' or '-').
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds INT32_MAX or underflows INT32_MIN.
 */
bool parse_i32(const char *s, int32_t *out);

/**
 * @brief Parse a hexadecimal string into a uint32_t.
 *
 * @param[in]  s   NUL-terminated hex string.
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds UINT32_MAX.
 */
bool parse_hex_u32(const char *s, uint32_t *out);

/**
 * @brief Parse a decimal string into a uint16_t.
 *
 * @param[in]  s   NUL-terminated decimal string.
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds UINT16_MAX.
 */
bool parse_u16(const char *s, uint16_t *out);

/**
 * @brief Parse a signed decimal string into an int16_t.
 *
 * @param[in]  s   NUL-terminated decimal string (optional leading '+' or '-').
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds INT16 range.
 */
bool parse_i16(const char *s, int16_t *out);

/**
 * @brief Parse a decimal string into a uint8_t.
 *
 * @param[in]  s   NUL-terminated decimal string.
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds UINT8_MAX.
 */
bool parse_u8(const char *s, uint8_t *out);

/**
 * @brief Parse a signed decimal string into an int8_t.
 *
 * @param[in]  s   NUL-terminated decimal string (optional leading '+' or '-').
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds INT8 range.
 */
bool parse_i8(const char *s, int8_t *out);

/**
 * @brief Parse a decimal floating-point string into a double.
 *
 * Handles optional sign (+/-), decimal point, and scientific notation (e/E).
 * Zero libc dependency — uses only built-in double arithmetic.
 *
 * Overflow returns false.  Underflow returns 0.0 with true.
 *
 * @param[in]  s   NUL-terminated decimal string.
 * @param[out] out Parsed value on success.
 * @return true on success, false on empty/invalid/overflow.
 */
bool parse_f64(const char *s, double *out);

/**
 * @brief Parse a decimal floating-point string into a float.
 *
 * Delegates to parse_f64 then narrows to float.  Returns false if the
 * value overflows float (finite double but inf float).
 *
 * @param[in]  s   NUL-terminated decimal string.
 * @param[out] out Parsed value on success.
 * @return true on success, false on empty/invalid/overflow.
 */
bool parse_f32(const char *s, float *out);

/** @} */

/*=======================================================================================
 * Public Function Prototypes — strtol-backed parsing
 *=======================================================================================*/

/** @defgroup CLI_ARGS_PARSE_Strtol_Parse strtol/strtod-backed parsers (libc dependent)
 *  @{
 */

/**
 * @brief Parse a string into a uint64_t via strtoull (base 0).
 *
 * Skips leading whitespace.  Base 0 means "0x" → hex, "0" → octal,
 * otherwise decimal.  Returns false on overflow or trailing junk.
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false on failure.
 */
bool strtol_parse_u64(const char *s, uint64_t *out);

/**
 * @brief Parse a string into an int64_t via strtoll (base 0).
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false on failure.
 */
bool strtol_parse_i64(const char *s, int64_t *out);

/**
 * @brief Parse a string into a uint32_t via strtoul (base 0).
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false on failure or if value exceeds UINT32_MAX.
 */
bool strtol_parse_u32(const char *s, uint32_t *out);

/**
 * @brief Parse a string into an int32_t via strtol (base 0).
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false on failure or if value exceeds INT32 range.
 */
bool strtol_parse_i32(const char *s, int32_t *out);

/**
 * @brief Parse a string into a uint16_t via strtol delegation.
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds UINT16_MAX.
 */
bool strtol_parse_u16(const char *s, uint16_t *out);

/**
 * @brief Parse a string into an int16_t via strtol delegation.
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds INT16 range.
 */
bool strtol_parse_i16(const char *s, int16_t *out);

/**
 * @brief Parse a string into a uint8_t via strtol delegation.
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds UINT8_MAX.
 */
bool strtol_parse_u8(const char *s, uint8_t *out);

/**
 * @brief Parse a string into an int8_t via strtol delegation.
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds INT8 range.
 */
bool strtol_parse_i8(const char *s, int8_t *out);

/**
 * @brief Parse a string into a float via strtof.
 *
 * Supports decimal and scientific notation.  Requires toolchain to link
 * strtof (newlib-nano may need `-u _scanf_float`).
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false on parse failure or overflow/underflow.
 */
bool strtol_parse_float(const char *s, float *out);

/**
 * @brief Parse a string into a double via strtod.
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false on parse failure or overflow/underflow.
 */
bool strtol_parse_double(const char *s, double *out);

/** @} */

/*=======================================================================================
 * Public Function Prototypes — integer / hex formatting (zero libc)
 *=======================================================================================*/

/** @defgroup CLI_ARGS_PARSE_Format Integer and hex formatters (no sprintf)
 *  @{
 */

/**
 * @brief Format a uint64_t as a decimal string.
 *
 * Writes at most @p size - 1 characters, always NUL-terminates.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written (excluding NUL terminator).
 */
int format_u64(char *buf, int size, uint64_t val);

/**
 * @brief Format an int64_t as a signed decimal string.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written (excluding NUL terminator).
 */
int format_i64(char *buf, int size, int64_t val);

/**
 * @brief Format a uint64_t as "0xABCD..." (uppercase hex, no leading zeros).
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written (excluding NUL terminator).
 */
int format_hex64(char *buf, int size, uint64_t val);

/**
 * @brief Format a uint32_t as a decimal string.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written (excluding NUL terminator).
 */
int format_u32(char *buf, int size, uint32_t val);

/**
 * @brief Format an int32_t as a signed decimal string.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written (excluding NUL terminator).
 */
int format_i32(char *buf, int size, int32_t val);

/**
 * @brief Format a uint32_t as "0x1234ABCD" (uppercase hex, no leading zeros).
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written.
 */
int format_hex32(char *buf, int size, uint32_t val);

/**
 * @brief Format a uint16_t as a decimal string.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written.
 */
int format_u16(char *buf, int size, uint16_t val);

/**
 * @brief Format an int16_t as a signed decimal string.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written.
 */
int format_i16(char *buf, int size, int16_t val);

/**
 * @brief Format a uint16_t as "0xABCD" (uppercase hex, zero-padded to 4 digits).
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written.
 */
int format_hex16(char *buf, int size, uint16_t val);

/**
 * @brief Format a uint8_t as a decimal string.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written.
 */
int format_u8(char *buf, int size, uint8_t val);

/**
 * @brief Format an int8_t as a signed decimal string.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written.
 */
int format_i8(char *buf, int size, int8_t val);

/**
 * @brief Format a uint8_t as "AB" (uppercase hex, always 2 digits).
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written (always 2).
 */
int format_hex8(char *buf, int size, uint8_t val);

/**
 * @brief Format a byte array as "0xAB 0xCD 0xEF ..." (space separated).
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  data Pointer to byte array.
 * @param[in]  len  Number of bytes to format.
 * @return Number of characters written (excluding NUL terminator).
 */
int format_bytes(char *buf, int size, const uint8_t *data, uint16_t len);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CLI_ARGS_PARSE_UTILS_H */

/*################################### END OF FILE ######################################*/
