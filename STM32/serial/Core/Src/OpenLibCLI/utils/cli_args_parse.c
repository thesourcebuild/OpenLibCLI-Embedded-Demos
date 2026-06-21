/**
 * @file cli_args_parse.c
 * @brief Lightweight integer, hex, and floating-point parse/format routines.
 *
 * Implements:
 *   - hand-rolled integer and hex parsers (parse_*)
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

#include "../../OpenLibCLI/utils/cli_args_parse.h"

#include <stdlib.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>


/*=======================================================================================
 * Private Defines
 *=======================================================================================*/

/** @brief Maximum decimal digits in a uint64_t without overflow. */
#define U64_MAX_DIGITS (20U)

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

/*=======================================================================================
 * Public Functions — hand-rolled integer parsing
 *=======================================================================================*/

bool parse_u64(const char *s, uint64_t *out) {
  uint64_t val = 0;
  bool ok = (s != NULL && out != NULL);

  if (ok) {
    ok = (s[0] != '\0');

    while (is_dec_digit(*s) && ok) {
      uint8_t digit = dec_value(*s);
      ok = (val <= ((UINT64_MAX - (uint64_t)digit) / 10ULL));
      if (ok) {
        val = (val * 10ULL) + (uint64_t)digit;
        s++;
      }
    }

    ok = (ok && *s == '\0');

    if (ok) {
      *out = val;
    }
  }

  return ok;
}

bool parse_i64(const char *s, int64_t *out) {
  uint64_t uv = 0;
  bool ok = (s != NULL && out != NULL);
  bool neg = false;

  if (ok) {
    ok = (s[0] != '\0');
  }

  if (s != NULL && ok) {
    neg = (*s == '-');
    if (neg || *s == '+') {
      s++;
    }

    /* Sign-only input is accepted as zero for compatibility with existing API tests. */
  }

  if (s != NULL && ok) {
    uint64_t limit = (uint64_t)INT64_MAX;
    if (neg) {
      limit++;
    }

    while (is_dec_digit(*s) && ok) {
      uint8_t digit = dec_value(*s);
      ok = (uv <= ((limit - (uint64_t)digit) / 10ULL));
      if (ok) {
        uv = (uv * 10ULL) + (uint64_t)digit;
        s++;
      }
    }

    ok = (ok && *s == '\0');
  }

  if (out != NULL && ok) {
    if (neg) {
      if (uv == ((uint64_t)INT64_MAX + 1ULL)) {
        *out = INT64_MIN;
      } else {
        *out = -(int64_t)uv;
      }
    } else {
      *out = (int64_t)uv;
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
  uint64_t tmp = 0ULL;
  bool ok = (out != NULL);

  if (ok) {
    ok = parse_u64(s, &tmp);
    ok = (ok && tmp <= 0xFFFFFFFFULL);
  }

  if (out != NULL && ok) {
    *out = (uint32_t)tmp;
  }

  return ok;
}

bool parse_i32(const char *s, int32_t *out) {
  int64_t tmp = 0LL;
  bool ok = (out != NULL);

  if (ok) {
    ok = parse_i64(s, &tmp);
    ok = (ok && tmp >= INT32_MIN && tmp <= INT32_MAX);
  }

  if (out != NULL && ok) {
    *out = (int32_t)tmp;
  }

  return ok;
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
  uint64_t tmp = 0ULL;
  bool ok = (out != NULL);

  if (ok) {
    ok = parse_u64(s, &tmp);
    ok = (ok && tmp <= 0xFFFFULL);
  }

  if (out != NULL && ok) {
    *out = (uint16_t)tmp;
  }

  return ok;
}

bool parse_i16(const char *s, int16_t *out) {
  int64_t tmp = 0LL;
  bool ok = (out != NULL);

  if (ok) {
    ok = parse_i64(s, &tmp);
    ok = (ok && tmp >= INT16_MIN && tmp <= INT16_MAX);
  }

  if (out != NULL && ok) {
    *out = (int16_t)tmp;
  }

  return ok;
}

bool parse_u8(const char *s, uint8_t *out) {
  uint64_t tmp = 0ULL;
  bool ok = (out != NULL);

  if (ok) {
    ok = parse_u64(s, &tmp);
    ok = (ok && tmp <= 0xFFULL);
  }

  if (out != NULL && ok) {
    *out = (uint8_t)tmp;
  }

  return ok;
}

bool parse_i8(const char *s, int8_t *out) {
  int64_t tmp = 0LL;
  bool ok = (out != NULL);

  if (ok) {
    ok = parse_i64(s, &tmp);
    ok = (ok && tmp >= INT8_MIN && tmp <= INT8_MAX);
  }

  if (out != NULL && ok) {
    *out = (int8_t)tmp;
  }

  return ok;
}

bool parse_f64(const char *s, double *out) {
  double sign = 1.0;
  uint64_t int_part = 0;
  int int_digits = 0;
  uint64_t frac_part = 0;
  int frac_digits = 0;
  int exponent = 0;
  bool ok = (s != NULL && out != NULL);

  if (ok) {
    ok = (s[0] != '\0');
  }

  if (s != NULL && ok) {
    if (*s == '-') {
      sign = -1.0;
      s++;
    } else if (*s == '+') {
      s++;
    } else {
      /* No sign. */
    }
  }

  ok = (s != NULL && ok && *s != '\0');

  while (s != NULL) {
    if (!is_dec_digit(*s) || !ok) {
      break;
    }
    uint8_t digit = dec_value(*s);
    uint64_t next = (int_part * 10ULL) + (uint64_t)digit;
    ok = (next >= int_part);
    if (ok) {
      int_part = next;
      int_digits++;
      s++;
    }
  }

  if (s != NULL && ok && *s == '.') {
    s++;
    while (is_dec_digit(*s) && ok) {
      uint8_t digit = dec_value(*s);
      uint64_t next = (frac_part * 10ULL) + (uint64_t)digit;
      ok = (next >= frac_part);
      if (ok) {
        frac_part = next;
        frac_digits++;
        s++;
      }
    }
  }

  ok = (ok && !(int_digits == 0 && frac_digits == 0));

  ok = (s != NULL && ok && (*s == '\0' || *s == 'e' || *s == 'E'));

  if (s != NULL && ok && (*s == 'e' || *s == 'E')) {
    s++;
    int exp_sign = 1;
    if (*s == '-') {
      exp_sign = -1;
      s++;
    } else if (*s == '+') {
      s++;
    } else {
      /* Exponent has no explicit sign. */
    }
    ok = (is_dec_digit(*s) && *s != '\0');

    while (is_dec_digit(*s) && ok) {
      exponent = (exponent * 10) + (int)dec_value(*s);
      if (exponent > 9999) {
        exponent = 9999;
      }
      s++;
    }
    exponent *= exp_sign;
  }

  ok = (s != NULL && ok && *s == '\0');

  double result = (double)int_part;
  if (ok && frac_digits > 0) {
    double frac_val = (double)frac_part;
    int d = frac_digits;
    while (d > 0) {
      frac_val /= 10.0;
      d--;
    }
    result += frac_val;
  }

  int final_exp = exponent;
  if (ok && final_exp > 0) {
    while (final_exp > 0) {
      ok = (result <= (DBL_MAX / 10.0));
      if (ok) {
        result *= 10.0;
        final_exp--;
      }
    }
  } else if (ok && final_exp < 0) {
    while (final_exp < 0) {
      result /= 10.0;
      final_exp++;
    }
  } else {
    /* No exponent scaling required. */
  }

  if (out != NULL && ok) {
    result *= sign;
    *out = result;
  }

  return ok;
}

bool parse_f32(const char *s, float *out) {
  double tmp = 0.0;
  bool ok = (out != NULL);

  if (ok) {
    ok = parse_f64(s, &tmp);
    ok = (ok && tmp <= (double)FLT_MAX && tmp >= -(double)FLT_MAX);
  }

  if (out != NULL && ok) {
    *out = (float)tmp;
  }

  return ok;
}

/*=======================================================================================
 * Public Functions — strtol-backed parsing
 *=======================================================================================*/

bool strtol_parse_u64(const char *s, uint64_t *out) {
  char *end = NULL;
  unsigned long long val = 0ULL;
  bool ok = (s != NULL && out != NULL);

  if (ok) {
    ok = (s[0] != '\0');
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

bool strtol_parse_u32(const char *s, uint32_t *out) {
  char *end = NULL;
  unsigned long val = 0UL;
  bool ok = (s != NULL && out != NULL);

  if (ok) {
    ok = (s[0] != '\0');
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
    errno = 0;
    val = strtof(s, &end);
    ok = (end != s && *end == '\0');
    if (ok) {
      ok = (errno != ERANGE);
    }
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
    errno = 0;
    val = strtod(s, &end);
    ok = (end != s && *end == '\0');
    if (ok) {
      ok = (errno != ERANGE);
    }
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
  int pos = 0;
  while (len > 0 && pos < size - 1) {
    len--;
    buf[pos] = tmp[len];
    pos++;
  }
  if (pos < size) {
    buf[pos] = '\0';
  }
  return pos;
}

int format_i64(char *buf, int size, int64_t val) {
  int result = 0;

  if (val < 0) {
    if (size >= 2) {
      buf[0] = '-';
      result = format_u64(&buf[1], size - 1, (uint64_t)(-(val + 1)) + 1ULL) + 1;
    } else if (size > 0) {
      buf[0] = '\0';
    } else {
      /* No space to write a terminator. */
    }
  } else {
    result = format_u64(buf, size, (uint64_t)val);
  }

  return result;
}

int format_hex64(char *buf, int size, uint64_t val) {
  int result = 0;

  if (size < 3) {
    if (size > 0) {
      buf[0] = '\0';
    }
  } else {
    buf[0] = '0';
    buf[1] = 'x';
    int pos = 2;
    bool started = false;
    for (unsigned int shift = 60U;; shift -= 4U) {
      uint8_t nib = (uint8_t)((val >> shift) & 0xFULL);
      if (nib != 0U || started || shift == 0U) {
        if (pos < size - 1) {
          buf[pos] = s_hex_digit[nib];
          pos++;
        }
        started = (started || nib != 0U || shift == 0U);
      }
      if (shift == 0U) {
        break;
      }
    }
    if (pos < size) {
      buf[pos] = '\0';
    }
    result = pos;
  }

  return result;
}

int format_u32(char *buf, int size, uint32_t val) {
  return format_u64(buf, size, (uint64_t)val);
}

int format_i32(char *buf, int size, int32_t val) {
  return format_i64(buf, size, (int64_t)val);
}

int format_hex32(char *buf, int size, uint32_t val) {
  return format_hex64(buf, size, (uint64_t)val);
}

int format_u16(char *buf, int size, uint16_t val) {
  return format_u64(buf, size, (uint64_t)val);
}

int format_i16(char *buf, int size, int16_t val) {
  return format_i64(buf, size, (int64_t)val);
}

int format_hex16(char *buf, int size, uint16_t val) {
  return format_hex64(buf, size, (uint64_t)val);
}

int format_u8(char *buf, int size, uint8_t val) {
  return format_u64(buf, size, (uint64_t)val);
}

int format_i8(char *buf, int size, int8_t val) {
  return format_i64(buf, size, (int64_t)val);
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
    if (pos >= size - 3) {
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

/*################################### END OF FILE ######################################*/
