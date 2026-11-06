/**
 * @file cli_args_parse.c
 * @brief Lightweight integer, hex, and floating-point parse/format routines.
 *
 * Implements:
 *   - hand-rolled integer and hex parsers, and strictly checked decimal float parsers
 *     converted by strtod / strtof (parse_*)
 *   - strtol/strtoul/strtoll/strtoull/strtof/strtod wrapped parsers (strtol_parse_*)
 *   - integer and hex formatters (format_*) — no sprintf dependency
 *
 *
 * @copyright Copyright (c) 2026 Muhammad Hassaan Shah.
 *
 * SPDX-License-Identifier: MIT
 */

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <math.h>

#include "cli_args_parse.h"

/*=======================================================================================
 * Private Defines
 *=======================================================================================*/

/** @brief Maximum decimal digits in a uint64_t without overflow. */
#define U64_MAX_DIGITS (20U)

/** @brief Maximum decimal digits in a uint32_t without overflow. */
#define U32_MAX_DIGITS (10U)

/*=======================================================================================
 * Private Macros
 *=======================================================================================*/

/* No private function-like macros defined in this translation unit. */

/*=======================================================================================
 * Private Types
 *=======================================================================================*/

/* No private types defined in this translation unit. */

/*=======================================================================================
 * External Data Variables
 *=======================================================================================*/

/* No externally visible data variables defined in this translation unit. */

/*=======================================================================================
 * Private Variables
 *=======================================================================================*/

/** @brief Lookup table for hex digit conversion (0-9 A-F). */
static const char s_hex_digit[16] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F',
};

/*=======================================================================================
 * Private Function Prototypes
 *=======================================================================================*/

static bool is_dec_digit(char c);
static bool is_hex_digit(char c);
static uint8_t dec_value(char c);
static uint8_t hex_value(char c);
static bool is_decimal_float_text(const char *s);
static bool strtol_unsigned_text_ok(const char *s);
static bool parse_dec_u32(const char *s, uint32_t max, uint32_t *out);
static bool parse_dec_u64(const char *s, uint64_t max, uint64_t *out);
static bool parse_dec_i32(const char *s, int32_t min, int32_t max, int32_t *out);
static int put_digits(char *buf, int size, const char *tmp, int len);
static int put_minus(char *buf, int size, int digits);

/*=======================================================================================
 * Public Functions — hand-rolled integer parsing
 *=======================================================================================*/

bool parse_u64(const char *s, uint64_t *out) {
  return (s != NULL && out != NULL && parse_dec_u64(s, UINT64_MAX, out));
}

bool parse_i64(const char *s, int64_t *out) {
  uint64_t mag = 0ULL;
  bool ok = (s != NULL && out != NULL);
  bool neg = false;

  if (ok) {
    neg = (*s == '-');
    if (neg || *s == '+') {
      s++;
    }
    ok = parse_dec_u64(s, neg ? ((uint64_t)INT64_MAX + 1ULL) : (uint64_t)INT64_MAX, &mag);
  }

  if (ok) {
    if (!neg) {
      *out = (int64_t)mag;
    } else if (mag == 0ULL) {
      *out = 0;
    } else {
      /* -(mag - 1) - 1 reaches INT64_MIN without negating it. */
      *out = -(int64_t)(mag - 1ULL) - 1;
    }
  }

  return ok;
}

bool parse_hex_u64(const char *s, uint64_t *out) {
  uint64_t val = 0;
  bool ok = (s != NULL && out != NULL);

  if (s != NULL && ok) {
    ok = (s[0] != '\0');

    if (ok && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
      s = &s[2];
    }

    ok = (ok && *s != '\0');

    while (ok && *s != '\0') {
      uint8_t nib = hex_value(*s);
      ok = (is_hex_digit(*s) && val <= (UINT64_MAX >> 4U));
      if (ok) {
        val = (val << 4U) | (uint64_t)nib;
        s++;
      }
    }

    if (ok) {
      *out = val;
    }
  }

  return ok;
}

bool parse_u32(const char *s, uint32_t *out) {
  return (s != NULL && out != NULL && parse_dec_u32(s, UINT32_MAX, out));
}

bool parse_i32(const char *s, int32_t *out) {
  return (s != NULL && out != NULL && parse_dec_i32(s, INT32_MIN, INT32_MAX, out));
}

bool parse_hex_u32(const char *s, uint32_t *out) {
  uint64_t tmp = 0ULL;
  bool ok = (out != NULL);

  if (ok) {
    ok = parse_hex_u64(s, &tmp);
    ok = (ok && tmp <= 0xFFFFFFFFULL);
  }

  if (out != NULL && ok) {
    *out = (uint32_t)tmp;
  }

  return ok;
}

bool parse_u16(const char *s, uint16_t *out) {
  uint32_t tmp = 0U;
  bool ok = (s != NULL && out != NULL && parse_dec_u32(s, UINT16_MAX, &tmp));

  if (ok) {
    *out = (uint16_t)tmp;
  }

  return ok;
}

bool parse_i16(const char *s, int16_t *out) {
  int32_t tmp = 0;
  bool ok = (s != NULL && out != NULL && parse_dec_i32(s, INT16_MIN, INT16_MAX, &tmp));

  if (ok) {
    *out = (int16_t)tmp;
  }

  return ok;
}

bool parse_u8(const char *s, uint8_t *out) {
  uint32_t tmp = 0U;
  bool ok = (s != NULL && out != NULL && parse_dec_u32(s, UINT8_MAX, &tmp));

  if (ok) {
    *out = (uint8_t)tmp;
  }

  return ok;
}

bool parse_i8(const char *s, int8_t *out) {
  int32_t tmp = 0;
  bool ok = (s != NULL && out != NULL && parse_dec_i32(s, INT8_MIN, INT8_MAX, &tmp));

  if (ok) {
    *out = (int8_t)tmp;
  }

  return ok;
}

bool parse_f64(const char *s, double *out) {
  char *end = NULL;
  double val = 0.0;
  bool ok = (s != NULL && out != NULL && is_decimal_float_text(s));

  if (ok) {
    /* The text is plain decimal, so strtod() reads all of it and rounds correctly. Overflow
     * returns +-HUGE_VAL; underflow returns 0 or a subnormal value and is accepted. */
    val = strtod(s, &end);
    ok = (*end == '\0' && isfinite(val));
  }

  if (ok) {
    *out = val;
  }

  return ok;
}

bool parse_f32(const char *s, float *out) {
  char *end = NULL;
  float val = 0.0F;
  bool ok = (s != NULL && out != NULL && is_decimal_float_text(s));

  if (ok) {
    /* Converted to float directly: going through double first would round twice. */
#if defined(__AVR__) && (__SIZEOF_DOUBLE__ == __SIZEOF_FLOAT__)
    /* avr-libc has no strtof(); with a 32-bit double, strtod() is the float parser. */
    val = (float)strtod(s, &end);
#else
    val = strtof(s, &end);
#endif
    ok = (*end == '\0' && isfinite(val));
  }

  if (ok) {
    *out = val;
  }

  return ok;
}

/*=======================================================================================
 * Public Functions — strtol-backed parsing
 *=======================================================================================*/

#if CLI_ARGS_PARSE_HAVE_STRTOLL
bool strtol_parse_u64(const char *s, uint64_t *out) {
  char *end = NULL;
  unsigned long long val = 0ULL;
  bool ok = (s != NULL && out != NULL);

  if (ok) {
    ok = (s[0] != '\0' && strtol_unsigned_text_ok(s));
  }

  if (s != NULL && ok) {
    errno = 0;
    val = strtoull(s, &end, 0);
    ok = (end != s && *end == '\0');
    if (ok) {
      ok = (errno != ERANGE);
    }
  }

  if (out != NULL && ok) {
    *out = (uint64_t)val;
  }

  return ok;
}

bool strtol_parse_i64(const char *s, int64_t *out) {
  char *end = NULL;
  long long val = 0LL;
  bool ok = (s != NULL && out != NULL);

  if (ok) {
    ok = (s[0] != '\0');
  }

  if (s != NULL && ok) {
    errno = 0;
    val = strtoll(s, &end, 0);
    ok = (end != s && *end == '\0');
    if (ok) {
      ok = (errno != ERANGE);
    }
  }

  if (out != NULL && ok) {
    *out = (int64_t)val;
  }

  return ok;
}
#endif /* CLI_ARGS_PARSE_HAVE_STRTOLL */

bool strtol_parse_u32(const char *s, uint32_t *out) {
  char *end = NULL;
  unsigned long val = 0UL;
  bool ok = (s != NULL && out != NULL);

  if (ok) {
    ok = (s[0] != '\0' && strtol_unsigned_text_ok(s));
  }

  if (s != NULL && ok) {
    errno = 0;
    val = strtoul(s, &end, 0);
    ok = (end != s && *end == '\0');
    if (ok) {
      ok = (errno != ERANGE);
    }
  }
#if ULONG_MAX > 0xFFFFFFFFUL
  ok = (ok && val <= 0xFFFFFFFFUL);
#endif

  if (out != NULL && ok) {
    *out = (uint32_t)val;
  }

  return ok;
}

bool strtol_parse_i32(const char *s, int32_t *out) {
  char *end = NULL;
  long val = 0L;
  bool ok = (s != NULL && out != NULL);

  if (ok) {
    ok = (s[0] != '\0');
  }

  if (s != NULL && ok) {
    errno = 0;
    val = strtol(s, &end, 0);
    ok = (end != s && *end == '\0');
    if (ok) {
      ok = (errno != ERANGE);
    }
  }
#if LONG_MAX > INT32_MAX
  ok = (ok && val >= INT32_MIN && val <= INT32_MAX);
#endif

  if (out != NULL && ok) {
    *out = (int32_t)val;
  }

  return ok;
}

bool strtol_parse_u16(const char *s, uint16_t *out) {
  uint32_t tmp = 0U;
  bool ok = (out != NULL);

  if (ok) {
    ok = strtol_parse_u32(s, &tmp);
    ok = (ok && tmp <= 0xFFFFu);
  }

  if (out != NULL && ok) {
    *out = (uint16_t)tmp;
  }

  return ok;
}

bool strtol_parse_i16(const char *s, int16_t *out) {
  int32_t tmp = 0;
  bool ok = (out != NULL);

  if (ok) {
    ok = strtol_parse_i32(s, &tmp);
    ok = (ok && tmp >= INT16_MIN && tmp <= INT16_MAX);
  }

  if (out != NULL && ok) {
    *out = (int16_t)tmp;
  }

  return ok;
}

bool strtol_parse_u8(const char *s, uint8_t *out) {
  uint32_t tmp = 0U;
  bool ok = (out != NULL);

  if (ok) {
    ok = strtol_parse_u32(s, &tmp);
    ok = (ok && tmp <= 0xFFu);
  }

  if (out != NULL && ok) {
    *out = (uint8_t)tmp;
  }

  return ok;
}

bool strtol_parse_i8(const char *s, int8_t *out) {
  int32_t tmp = 0;
  bool ok = (out != NULL);

  if (ok) {
    ok = strtol_parse_i32(s, &tmp);
    ok = (ok && tmp >= INT8_MIN && tmp <= INT8_MAX);
  }

  if (out != NULL && ok) {
    *out = (int8_t)tmp;
  }

  return ok;
}

bool strtol_parse_float(const char *s, float *out) {
  char *end = NULL;
  float val = 0.0F;
  bool ok = (s != NULL && out != NULL);

  if (ok) {
    ok = (s[0] != '\0');
  }

  if (s != NULL && ok) {
#if defined(__AVR__) && (__SIZEOF_DOUBLE__ == __SIZEOF_FLOAT__)
    /* avr-libc has no strtof(); with a 32-bit double, strtod() is the float parser. */
    val = (float)strtod(s, &end);
#else
    val = strtof(s, &end);
#endif
    /* Overflow returns ±HUGE_VALF and "inf" / "nan" are parsed as such: none of them is a
     * usable argument. Underflow yields 0 or a subnormal value and succeeds, like
     * parse_f32(). */
    ok = (end != s && *end == '\0' && isfinite(val));
  }

  if (out != NULL && ok) {
    *out = val;
  }

  return ok;
}

bool strtol_parse_double(const char *s, double *out) {
  char *end = NULL;
  double val = 0.0;
  bool ok = (s != NULL && out != NULL);

  if (ok) {
    ok = (s[0] != '\0');
  }

  if (s != NULL && ok) {
    val = strtod(s, &end);
    /* Overflow (±HUGE_VAL), "inf" and "nan" fail; underflow succeeds, like parse_f64(). */
    ok = (end != s && *end == '\0' && isfinite(val));
  }

  if (out != NULL && ok) {
    *out = val;
  }

  return ok;
}

/*=======================================================================================
 * Public Functions — integer / hex formatting
 *=======================================================================================*/

int format_u64(char *buf, int size, uint64_t val) {
  char tmp[U64_MAX_DIGITS];
  int len = 0;

  do {
    uint64_t digit = val % 10ULL;
    tmp[len] = (char)('0' + digit);
    len++;
    val /= 10ULL;
  } while (val != 0ULL);

  return put_digits(buf, size, tmp, len);
}

int format_i64(char *buf, int size, int64_t val) {
  int result = 0;

  if (val < 0) {
    /* Format the magnitude after the sign; it only succeeds if sign + digits + NUL fit. */
    int digits = (size >= 2) ? format_u64(&buf[1], size - 1, (uint64_t)(-(val + 1)) + 1ULL) : 0;
    result = put_minus(buf, size, digits);
  } else {
    result = format_u64(buf, size, (uint64_t)val);
  }

  return result;
}

int format_hex64(char *buf, int size, uint64_t val) {
  int digits = 1;
  int result = 0;

  while (digits < 16 && (val >> (unsigned)(digits * 4)) != 0ULL) {
    digits++;
  }

  /* All or nothing: "0x" + digits + NUL must fit. */
  if (2 + digits < size) {
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < digits; i++) {
      unsigned shift = (unsigned)((digits - 1 - i) * 4);
      buf[2 + i] = s_hex_digit[(val >> shift) & 0xFULL];
    }
    result = 2 + digits;
    buf[result] = '\0';
  } else if (size > 0) {
    buf[0] = '\0';
  } else {
    /* No space to write a terminator. */
  }

  return result;
}

int format_u32(char *buf, int size, uint32_t val) {
  char tmp[U32_MAX_DIGITS];
  int len = 0;

  do {
    uint32_t digit = val % 10U;
    tmp[len] = (char)('0' + digit);
    len++;
    val /= 10U;
  } while (val != 0U);

  return put_digits(buf, size, tmp, len);
}

int format_i32(char *buf, int size, int32_t val) {
  int result = 0;

  if (val < 0) {
    /* Format the magnitude after the sign; it only succeeds if sign + digits + NUL fit. */
    int digits = (size >= 2) ? format_u32(&buf[1], size - 1, (uint32_t)(-(val + 1)) + 1U) : 0;
    result = put_minus(buf, size, digits);
  } else {
    result = format_u32(buf, size, (uint32_t)val);
  }

  return result;
}

int format_hex32(char *buf, int size, uint32_t val) {
  return format_hex64(buf, size, (uint64_t)val);
}

int format_u16(char *buf, int size, uint16_t val) {
  return format_u32(buf, size, (uint32_t)val);
}

int format_i16(char *buf, int size, int16_t val) {
  return format_i32(buf, size, (int32_t)val);
}

int format_hex16(char *buf, int size, uint16_t val) {
  return format_hex64(buf, size, (uint64_t)val);
}

int format_u8(char *buf, int size, uint8_t val) {
  return format_u32(buf, size, (uint32_t)val);
}

int format_i8(char *buf, int size, int8_t val) {
  return format_i32(buf, size, (int32_t)val);
}

int format_hex8(char *buf, int size, uint8_t val) {
  int result = 0;

  if (size < 3) {
    if (size > 0) {
      buf[0] = '\0';
    }
  } else {
    uint8_t high = (uint8_t)((val >> 4) & 0xFU);
    uint8_t low = (uint8_t)(val & 0xFU);
    buf[0] = s_hex_digit[high];
    buf[1] = s_hex_digit[low];
    buf[2] = '\0';
    result = 2;
  }

  return result;
}

int format_bytes(char *buf, int size, const uint8_t *data, uint16_t len) {
  int pos = 0;
  for (uint16_t i = 0; i < len; i++) {
    /* Room for [' '] + 2 hex digits + NUL terminator. */
    int needed = (i != 0U) ? 4 : 3;
    if (pos + needed > size) {
      break;
    }
    if (i != 0U) {
      buf[pos] = ' ';
      pos++;
    }
    int r = format_hex8(&buf[pos], size - pos, data[i]);
    pos += r;
  }
  if (pos < size) {
    buf[pos] = '\0';
  }
  return pos;
}

/*=======================================================================================
 * Private Functions
 *=======================================================================================*/

static bool is_dec_digit(char c) {
  return (c >= '0' && c <= '9');
}

static bool is_hex_digit(char c) {
  return ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
}

static uint8_t dec_value(char c) {
  return (uint8_t)((uint8_t)c - (uint8_t)'0');
}

static uint8_t hex_value(char c) {
  uint8_t value;

  if (c >= '0' && c <= '9') {
    value = dec_value(c);
  } else if (c >= 'a' && c <= 'f') {
    value = (uint8_t)(((uint8_t)c - (uint8_t)'a') + 10U);
  } else {
    value = (uint8_t)(((uint8_t)c - (uint8_t)'A') + 10U);
  }

  return value;
}

/**
 * @brief Read the decimal digits of @p s (at least one, nothing else) as a value up to @p max.
 *
 * Overflow is checked against constants, so no division runs: on targets without a hardware
 * divider (or for 64-bit values on 32-bit targets) a runtime division is a library call per digit.
 *
 * @param[in]  s    NUL-terminated input.
 * @param[in]  max  Largest accepted value.
 * @param[out] out  Receives the value on success.
 *
 * @return @c true on success; @c false for empty input, a non-digit or a value above @p max.
 */
static bool parse_dec_u32(const char *s, uint32_t max, uint32_t *out) {
  uint32_t val = 0U;
  bool ok = is_dec_digit(*s);

  while (ok && is_dec_digit(*s)) {
    uint32_t digit = dec_value(*s);
    ok = (val < (UINT32_MAX / 10U)) || (val == (UINT32_MAX / 10U) && digit <= (UINT32_MAX % 10U));
    if (ok) {
      val = (val * 10U) + digit;
      ok = (val <= max);
      s++;
    }
  }

  ok = (ok && *s == '\0');

  if (ok) {
    *out = val;
  }

  return ok;
}

/** @brief 64-bit form of @c parse_dec_u32, with the same rules. */
static bool parse_dec_u64(const char *s, uint64_t max, uint64_t *out) {
  uint64_t val = 0ULL;
  bool ok = is_dec_digit(*s);

  while (ok && is_dec_digit(*s)) {
    uint64_t digit = dec_value(*s);
    ok = (val < (UINT64_MAX / 10ULL)) ||
         (val == (UINT64_MAX / 10ULL) && digit <= (UINT64_MAX % 10ULL));
    if (ok) {
      val = (val * 10ULL) + digit;
      ok = (val <= max);
      s++;
    }
  }

  ok = (ok && *s == '\0');

  if (ok) {
    *out = val;
  }

  return ok;
}

/**
 * @brief Read an optionally signed decimal value in [@p min, @p max] in 32-bit arithmetic.
 *
 * @param[in]  s    NUL-terminated input: optional '+' or '-', then at least one digit.
 * @param[in]  min  Smallest accepted value (negative).
 * @param[in]  max  Largest accepted value.
 * @param[out] out  Receives the value on success.
 *
 * @return @c true on success.
 */
static bool parse_dec_i32(const char *s, int32_t min, int32_t max, int32_t *out) {
  uint32_t mag = 0U;
  bool neg = (*s == '-');
  bool ok;

  if (neg || *s == '+') {
    s++;
  }

  ok = parse_dec_u32(s, neg ? ((uint32_t)(-(min + 1)) + 1U) : (uint32_t)max, &mag);

  if (ok) {
    if (!neg) {
      *out = (int32_t)mag;
    } else if (mag == 0U) {
      *out = 0;
    } else {
      /* -(mag - 1) - 1 reaches INT32_MIN without negating it. */
      *out = -(int32_t)(mag - 1U) - 1;
    }
  }

  return ok;
}

/**
 * @brief Copy digits stored least significant first into @p buf, all or nothing.
 *
 * A cut-off number would read as a different value, so nothing is written unless all
 * digits and the NUL fit; @p buf is still terminated when @p size allows.
 *
 * @return Characters written, excluding the NUL (0 if they did not fit).
 */
static int put_digits(char *buf, int size, const char *tmp, int len) {
  int pos = 0;

  if (len < size) {
    while (len > 0) {
      len--;
      buf[pos] = tmp[len];
      pos++;
    }
  }
  if (size > 0) {
    buf[pos] = '\0';
  }
  return pos;
}

/**
 * @brief Put the '-' before a magnitude already formatted at @p buf[1].
 *
 * @param[in] digits  Length of that magnitude; 0 when it did not fit, which empties @p buf.
 *
 * @return Characters written, excluding the NUL.
 */
static int put_minus(char *buf, int size, int digits) {
  int result = 0;

  if (digits > 0) {
    buf[0] = '-';
    result = digits + 1;
  } else if (size > 0) {
    buf[0] = '\0';
  } else {
    /* No space to write a terminator. */
  }

  return result;
}

/**
 * @brief Test whether @p s is a plain decimal number as accepted by @c parse_f64.
 *
 * Grammar: optional sign, digits with at most one '.', at least one digit, then an
 * optional exponent ('e' or 'E', optional sign, at least one digit). Nothing else:
 * no whitespace, hexadecimal floats, "inf" or "nan".
 *
 * @param[in] s  NUL-terminated input.
 *
 * @return @c true when the whole string matches.
 */
static bool is_decimal_float_text(const char *s) {
  bool digits = false;

  if (*s == '-' || *s == '+') {
    s++;
  }
  while (is_dec_digit(*s)) {
    digits = true;
    s++;
  }
  if (*s == '.') {
    s++;
    while (is_dec_digit(*s)) {
      digits = true;
      s++;
    }
  }
  if (digits && (*s == 'e' || *s == 'E')) {
    s++;
    if (*s == '-' || *s == '+') {
      s++;
    }
    digits = is_dec_digit(*s);
    while (is_dec_digit(*s)) {
      s++;
    }
  }

  return (digits && *s == '\0');
}

/**
 * @brief Reject text that strtoul()/strtoull() would silently negate.
 *
 * The C library accepts a leading '-' for unsigned conversions and returns the
 * negated value modulo 2^N, whose width depends on the platform's @c long.
 *
 * @param[in] s  NUL-terminated input.
 *
 * @return @c false if the first non-whitespace character is '-'.
 */
static bool strtol_unsigned_text_ok(const char *s) {
  while (*s == ' ' || (*s >= '\t' && *s <= '\r')) {
    s++;
  }
  return (*s != '-');
}

/*################################### END OF FILE ######################################*/
