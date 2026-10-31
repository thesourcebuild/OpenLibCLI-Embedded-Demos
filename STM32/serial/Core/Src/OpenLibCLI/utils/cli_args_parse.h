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

/* Symbol export for shared-library builds (same rules as OPENLIBCLI_API in cli.h). */
#ifndef OPENLIBCLI_API
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(CLI_SHARED)
#if defined(BUILD_LIB_SHARED)
#define OPENLIBCLI_API __declspec(dllexport)
#else
#define OPENLIBCLI_API __declspec(dllimport)
#endif
#else
#define OPENLIBCLI_API
#endif
#elif defined(CLI_SHARED) && defined(__GNUC__)
#define OPENLIBCLI_API __attribute__((visibility("default")))
#else
#define OPENLIBCLI_API
#endif
#endif

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
 * Returns false on overflow (checked before every multiply-add).
 *
 * @param[in]  s   NUL-terminated decimal string.
 * @param[out] out Parsed value on success; unchanged on failure.
 * @return true on success, false on empty string, invalid chars, or overflow.
 */
OPENLIBCLI_API bool parse_u64(const char *s, uint64_t *out);

/**
 * @brief Parse an optionally signed decimal string into an int64_t.
 *
 * Leading '+' or '-' is accepted but must be followed by at least one digit
 * (a sign alone is rejected).  Rejects trailing junk and overflow.
 *
 * @param[in]  s   NUL-terminated decimal string (optional leading '+' or '-').
 * @param[out] out Parsed value on success.
 * @return true on success, false on failure.
 */
OPENLIBCLI_API bool parse_i64(const char *s, int64_t *out);

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
OPENLIBCLI_API bool parse_hex_u64(const char *s, uint64_t *out);

/**
 * @brief Parse a decimal string into a uint32_t.
 *
 * @param[in]  s   NUL-terminated decimal string.
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds UINT32_MAX.
 */
OPENLIBCLI_API bool parse_u32(const char *s, uint32_t *out);

/**
 * @brief Parse a signed decimal string into an int32_t.
 *
 * @param[in]  s   NUL-terminated decimal string (optional leading '+' or '-').
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds INT32_MAX or underflows INT32_MIN.
 */
OPENLIBCLI_API bool parse_i32(const char *s, int32_t *out);

/**
 * @brief Parse a hexadecimal string into a uint32_t.
 *
 * @param[in]  s   NUL-terminated hex string.
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds UINT32_MAX.
 */
OPENLIBCLI_API bool parse_hex_u32(const char *s, uint32_t *out);

/**
 * @brief Parse a decimal string into a uint16_t.
 *
 * @param[in]  s   NUL-terminated decimal string.
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds UINT16_MAX.
 */
OPENLIBCLI_API bool parse_u16(const char *s, uint16_t *out);

/**
 * @brief Parse a signed decimal string into an int16_t.
 *
 * @param[in]  s   NUL-terminated decimal string (optional leading '+' or '-').
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds INT16 range.
 */
OPENLIBCLI_API bool parse_i16(const char *s, int16_t *out);

/**
 * @brief Parse a decimal string into a uint8_t.
 *
 * @param[in]  s   NUL-terminated decimal string.
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds UINT8_MAX.
 */
OPENLIBCLI_API bool parse_u8(const char *s, uint8_t *out);

/**
 * @brief Parse a signed decimal string into an int8_t.
 *
 * @param[in]  s   NUL-terminated decimal string (optional leading '+' or '-').
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds INT8 range.
 */
OPENLIBCLI_API bool parse_i8(const char *s, int8_t *out);

/**
 * @brief Parse a decimal floating-point string into a double.
 *
 * Handles optional sign (+/-), decimal point, and scientific notation (e/E).
 * Zero libc dependency — uses only built-in double arithmetic.  Any number of
 * digits is accepted: the first 19-20 significant digits form the mantissa
 * (rounded half-up on the next digit) and further digits only affect the
 * decimal exponent. The result is within about one unit in the last place of
 * the correctly rounded value; use @c strtol_parse_double when exact IEEE
 * rounding is required.
 *
 * Overflow returns false.  Underflow returns 0.0 with true.
 *
 * @param[in]  s   NUL-terminated decimal string.
 * @param[out] out Parsed value on success.
 * @return true on success, false on empty/invalid/overflow.
 */
OPENLIBCLI_API bool parse_f64(const char *s, double *out);

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
OPENLIBCLI_API bool parse_f32(const char *s, float *out);

/** @} */

/*=======================================================================================
 * Public Function Prototypes — strtol-backed parsing
 *=======================================================================================*/

/** @defgroup CLI_ARGS_PARSE_Strtol_Parse strtol/strtod-backed parsers (libc dependent)
 *  @{
 */

#ifndef CLI_ARGS_PARSE_HAVE_STRTOLL
#if defined(__AVR__)
/** @brief avr-libc provides no strtoll() / strtoull(): the 64-bit strtol parsers are omitted
 *         (use @c parse_u64 / @c parse_i64 / @c parse_hex_u64 instead). */
#define CLI_ARGS_PARSE_HAVE_STRTOLL 0
#else
/** @brief The C library provides strtoll() / strtoull(). */
#define CLI_ARGS_PARSE_HAVE_STRTOLL 1
#endif
#endif

#if CLI_ARGS_PARSE_HAVE_STRTOLL
/**
 * @brief Parse a string into a uint64_t via strtoull (base 0).
 *
 * Skips leading whitespace.  Base 0 means "0x" → hex, "0" → octal,
 * otherwise decimal.  Returns false on overflow, trailing junk, or a leading
 * '-' (which strtoull would silently wrap to a large positive value).
 * Not available when @c CLI_ARGS_PARSE_HAVE_STRTOLL is 0 (avr-libc).
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false on failure.
 */
OPENLIBCLI_API bool strtol_parse_u64(const char *s, uint64_t *out);

/**
 * @brief Parse a string into an int64_t via strtoll (base 0).
 *
 * Not available when @c CLI_ARGS_PARSE_HAVE_STRTOLL is 0 (avr-libc).
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false on failure.
 */
OPENLIBCLI_API bool strtol_parse_i64(const char *s, int64_t *out);
#endif /* CLI_ARGS_PARSE_HAVE_STRTOLL */

/**
 * @brief Parse a string into a uint32_t via strtoul (base 0).
 *
 * Same input rules as @c strtol_parse_u64; negative input is rejected on every
 * platform regardless of the width of @c long.
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false on failure or if value exceeds UINT32_MAX.
 */
OPENLIBCLI_API bool strtol_parse_u32(const char *s, uint32_t *out);

/**
 * @brief Parse a string into an int32_t via strtol (base 0).
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false on failure or if value exceeds INT32 range.
 */
OPENLIBCLI_API bool strtol_parse_i32(const char *s, int32_t *out);

/**
 * @brief Parse a string into a uint16_t via strtol delegation.
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds UINT16_MAX.
 */
OPENLIBCLI_API bool strtol_parse_u16(const char *s, uint16_t *out);

/**
 * @brief Parse a string into an int16_t via strtol delegation.
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds INT16 range.
 */
OPENLIBCLI_API bool strtol_parse_i16(const char *s, int16_t *out);

/**
 * @brief Parse a string into a uint8_t via strtol delegation.
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds UINT8_MAX.
 */
OPENLIBCLI_API bool strtol_parse_u8(const char *s, uint8_t *out);

/**
 * @brief Parse a string into an int8_t via strtol delegation.
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success, false if value exceeds INT8 range.
 */
OPENLIBCLI_API bool strtol_parse_i8(const char *s, int8_t *out);

/**
 * @brief Parse a string into a float via strtof.
 *
 * Supports decimal and scientific notation.  Requires toolchain to link
 * strtof (newlib-nano may need `-u _scanf_float`).
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success (underflow yields 0 or a subnormal value, like
 *         @c parse_f32), false on parse failure or overflow.
 */
OPENLIBCLI_API bool strtol_parse_float(const char *s, float *out);

/**
 * @brief Parse a string into a double via strtod.
 *
 * @param[in]  s   NUL-terminated string.
 * @param[out] out Parsed value on success.
 * @return true on success (underflow yields 0 or a subnormal value, like
 *         @c parse_f64), false on parse failure or overflow.
 */
OPENLIBCLI_API bool strtol_parse_double(const char *s, double *out);

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
 * All decimal and hex formatters are all-or-nothing: if the complete text plus
 * its NUL terminator does not fit in @p size bytes, an empty string is written
 * and 0 is returned (a cut-off number would read as a different value). The
 * output is always NUL-terminated when @p size > 0.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written (excluding NUL terminator).
 */
OPENLIBCLI_API int format_u64(char *buf, int size, uint64_t val);

/**
 * @brief Format an int64_t as a signed decimal string.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written (excluding NUL terminator).
 */
OPENLIBCLI_API int format_i64(char *buf, int size, int64_t val);

/**
 * @brief Format a uint64_t as "0xABCD..." (uppercase hex, no leading zeros).
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written (excluding NUL terminator).
 */
OPENLIBCLI_API int format_hex64(char *buf, int size, uint64_t val);

/**
 * @brief Format a uint32_t as a decimal string.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written (excluding NUL terminator).
 */
OPENLIBCLI_API int format_u32(char *buf, int size, uint32_t val);

/**
 * @brief Format an int32_t as a signed decimal string.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written (excluding NUL terminator).
 */
OPENLIBCLI_API int format_i32(char *buf, int size, int32_t val);

/**
 * @brief Format a uint32_t as "0x1234ABCD" (uppercase hex, no leading zeros).
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written.
 */
OPENLIBCLI_API int format_hex32(char *buf, int size, uint32_t val);

/**
 * @brief Format a uint16_t as a decimal string.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written.
 */
OPENLIBCLI_API int format_u16(char *buf, int size, uint16_t val);

/**
 * @brief Format an int16_t as a signed decimal string.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written.
 */
OPENLIBCLI_API int format_i16(char *buf, int size, int16_t val);

/**
 * @brief Format a uint16_t as "0xABCD" (uppercase hex, no leading zeros).
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written.
 */
OPENLIBCLI_API int format_hex16(char *buf, int size, uint16_t val);

/**
 * @brief Format a uint8_t as a decimal string.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written.
 */
OPENLIBCLI_API int format_u8(char *buf, int size, uint8_t val);

/**
 * @brief Format an int8_t as a signed decimal string.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written.
 */
OPENLIBCLI_API int format_i8(char *buf, int size, int8_t val);

/**
 * @brief Format a uint8_t as "AB" (uppercase hex, always 2 digits).
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  val  Value to format.
 * @return Number of characters written (always 2).
 */
OPENLIBCLI_API int format_hex8(char *buf, int size, uint8_t val);

/**
 * @brief Format a byte array as "AB CD EF ..." (uppercase hex, space separated).
 *
 * Only whole bytes are written: output stops at the last byte that fits
 * together with its separator and the NUL terminator.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  size Buffer size in bytes.
 * @param[in]  data Pointer to byte array.
 * @param[in]  len  Number of bytes to format.
 * @return Number of characters written (excluding NUL terminator).
 */
OPENLIBCLI_API int format_bytes(char *buf, int size, const uint8_t *data, uint16_t len);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CLI_ARGS_PARSE_UTILS_H */

/*################################### END OF FILE ######################################*/
