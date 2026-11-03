/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* Number parsing: VarParseNumFromStr splits a string into digits, a decimal exponent and flags (following the locale's separators and the NUMPRS_* flags),
 * VarNumFromParseNum turns the result into the smallest of the VARIANT number types that the caller allows. The string conversions of conv.inc are built on both. */

/* The characters of a locale that matter to a number: the decimal and thousands separators (plain and monetary) and the currency symbol. 0 means none. */
typedef struct {
  WCHAR decimal, money_decimal, thousand, money_thousand;
  WCHAR currency[16];
  int currency_length;
} PARSE_CHARS;

static BOOL parse_locale_text(LCID lcid, ULONG flags, LCTYPE type, WCHAR *out, int capacity) {
  out[0] = 0;
  return GetLocaleInfoW(lcid, type | (LCTYPE)(flags & LOCALE_NOUSEROVERRIDE), out, capacity) > 0 && out[0];
}

static void parse_load_chars(PARSE_CHARS *chars, LCID lcid, ULONG flags) {
  WCHAR text[16];
  /* (SDECIMAL, SMONTHOUSANDSEP and SCURRENCY have defaults; STHOUSAND and SMONDECIMALSEP have none) */
  chars->decimal = parse_locale_text(lcid, flags, LOCALE_SDECIMAL, text, 16) ? text[0] : '.';
  chars->money_decimal = parse_locale_text(lcid, flags, LOCALE_SMONDECIMALSEP, text, 16) ? text[0] : 0;
  chars->thousand = parse_locale_text(lcid, flags, LOCALE_STHOUSAND, text, 16) ? text[0] : 0;
  chars->money_thousand = parse_locale_text(lcid, flags, LOCALE_SMONTHOUSANDSEP, text, 16) ? text[0] : ',';
  if (parse_locale_text(lcid, flags, LOCALE_SCURRENCY, chars->currency, 16)) chars->currency_length = (int)oa_wcslen(chars->currency);
  else { chars->currency[0] = '$'; chars->currency[1] = 0; chars->currency_length = 1; }
}

static BOOL parse_is_space(WCHAR c) {
  return (c >= 9 && c <= 13) || c == ' ' || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}
static BOOL parse_is_nbsp_like(WCHAR c) { return c == 0xA0 || c == 0x202F; }

/* Is `c` the plain thousands separator? A no-break space there also stands for the plain space (French: today's narrow no-break space is typed as a space), but not the other way. */
static BOOL parse_is_thousand(const PARSE_CHARS *chars, WCHAR c) {
  if (!chars->thousand || c == chars->decimal) return FALSE;
  if (c == chars->thousand) return TRUE;
  return parse_is_nbsp_like(chars->thousand) && (c == ' ' || parse_is_nbsp_like(c));
}
static BOOL parse_is_money_thousand(const PARSE_CHARS *chars, WCHAR c) { return c == chars->money_thousand && c != chars->decimal; }

static BOOL parse_currency_at(const PARSE_CHARS *chars, const WCHAR *p) {
  for (int i = 0; i < chars->currency_length; i++) if (p[i] != chars->currency[i]) return FALSE;
  return TRUE;
}

#define PARSE_EXPONENT_LIMIT 1000000

EXPORT HRESULT WINAPI VarParseNumFromStr(const OLECHAR *string, LCID lcid, ULONG flags, NUMPARSE *parse, BYTE *digits) {
  if (!parse || !digits) return E_INVALIDARG;
  const ULONG in = parse->dwInFlags;
  const int capacity = parse->cDig < 0 ? 0 : parse->cDig;
  parse->cDig = 0; parse->dwOutFlags = 0; parse->cchUsed = 0; parse->nBaseShift = 0; parse->nPwr10 = 0;
  if (!string) return DISP_E_TYPEMISMATCH;

  PARSE_CHARS chars;
  parse_load_chars(&chars, lcid, flags);
  ULONG out = 0;
  const WCHAR *p = string;
  BOOL seen_plus = FALSE, seen_minus = FALSE, seen_currency = FALSE, seen_parens = FALSE;

  /* what may come before the digits: white space, a sign, the currency symbol and an opening parenthesis, each once and in any order */
  for (;;) {
    if ((in & NUMPRS_LEADING_WHITE) && parse_is_space(*p)) { out |= NUMPRS_LEADING_WHITE; while (parse_is_space(*p)) p++; }
    else if ((in & NUMPRS_LEADING_PLUS) && *p == '+' && !seen_plus && !seen_minus) { out |= NUMPRS_LEADING_PLUS; seen_plus = TRUE; p++; }
    else if ((in & NUMPRS_LEADING_MINUS) && *p == '-' && !seen_plus && !seen_minus) { out |= NUMPRS_LEADING_MINUS | NUMPRS_NEG; seen_minus = TRUE; p++; }
    else if ((in & NUMPRS_CURRENCY) && !seen_currency && parse_currency_at(&chars, p)) { out |= NUMPRS_CURRENCY; seen_currency = TRUE; p += chars.currency_length; }
    else if ((in & NUMPRS_PARENS) && *p == '(' && !seen_parens) { out |= NUMPRS_PARENS | NUMPRS_NEG; seen_parens = TRUE; p++; }
    else break;
  }

  int stored = 0, power = 0;
  BOOL seen_digit = FALSE, seen_decimal = FALSE, hex = FALSE;

  if ((in & NUMPRS_HEX_OCT) && !seen_currency && p[0] == '&' && (p[1] == 'h' || p[1] == 'H' || p[1] == 'o' || p[1] == 'O')) {
    /* &H1F or &O17: only integers */
    const BOOL octal = p[1] == 'o' || p[1] == 'O';
    const int shift = octal ? 3 : 4;
    p += 2;
    hex = TRUE;
    BOOL leading = TRUE;
    for (;; p++) {
      int d;
      const WCHAR c = *p;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (!octal && c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (!octal && c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else break;
      if (octal && d > 7) break;
      seen_digit = TRUE;
      if (leading && d == 0) continue;
      leading = FALSE;
      if (stored < capacity) digits[stored++] = (BYTE)d;
      else out |= NUMPRS_INEXACT;
    }
    if (!seen_digit) return DISP_E_TYPEMISMATCH;
    if (!stored && capacity > 0) digits[stored++] = 0;
    out |= NUMPRS_HEX_OCT;
    parse->nBaseShift = shift;
  } else {
    for (;; p++) {
      const WCHAR c = *p;
      if (c >= '0' && c <= '9') {
        const int d = c - '0';
        seen_digit = TRUE;
        if (stored == 0 && d == 0) { if (seen_decimal && power > -PARSE_EXPONENT_LIMIT) power--; continue; }       /* a leading zero */
        if (stored < capacity) { digits[stored++] = (BYTE)d; if (seen_decimal) power--; }
        else {
          if (d) out |= NUMPRS_INEXACT;
          if (!seen_decimal && power < PARSE_EXPONENT_LIMIT) power++;
        }
        continue;
      }
      if (c == chars.decimal) {                                  /* (masks the plain thousands separator even when it is not wanted) */
        if ((in & NUMPRS_DECIMAL) && !seen_decimal) { seen_decimal = TRUE; out |= NUMPRS_DECIMAL; continue; }
        break;
      }
      if ((in & NUMPRS_THOUSANDS) && seen_digit && !seen_decimal && parse_is_thousand(&chars, c)) { out |= NUMPRS_THOUSANDS; continue; }
      if ((in & NUMPRS_THOUSANDS) && (in & NUMPRS_CURRENCY) && seen_digit && !seen_decimal && parse_is_money_thousand(&chars, c)) { out |= NUMPRS_THOUSANDS | NUMPRS_CURRENCY; continue; }
      if ((in & NUMPRS_DECIMAL) && !seen_decimal && chars.money_decimal && c == chars.money_decimal) { seen_decimal = TRUE; out |= NUMPRS_DECIMAL | NUMPRS_CURRENCY; continue; }
      break;
    }
    if (!seen_digit) return DISP_E_TYPEMISMATCH;
    if (stored == 0 && capacity > 0) digits[stored++] = 0;                      /* zero keeps one digit (and the exponent of its decimals) */
    else while (stored > 1 && digits[stored - 1] == 0) { stored--; power++; }   /* trailing zeros are not digits (they may stay in the buffer) */

    if ((in & NUMPRS_EXPONENT) && (*p == 'e' || *p == 'E')) {
      const WCHAR *q = p + 1;
      BOOL negative = FALSE;
      if (*q == '+' || *q == '-') { negative = *q == '-'; q++; }
      if (*q >= '0' && *q <= '9') {
        long long exponent = 0;
        for (; *q >= '0' && *q <= '9'; q++) if (exponent < PARSE_EXPONENT_LIMIT) exponent = exponent * 10 + (*q - '0');
        power += (int)(negative ? -exponent : exponent);
        out |= NUMPRS_EXPONENT;
        p = q;
      }
    }
  }

  /* what may follow: white space, a sign (only without a leading one), the currency symbol (any number of times), trailing separators and the closing parenthesis */
  BOOL closed = FALSE;
  for (;;) {
    const WCHAR c = *p;
    if (!c) break;
    if ((in & NUMPRS_TRAILING_WHITE) && parse_is_space(c) && !parse_is_thousand(&chars, c) && !((in & NUMPRS_CURRENCY) && parse_is_money_thousand(&chars, c))) {
      out |= NUMPRS_TRAILING_WHITE;
      while (parse_is_space(*p) && !parse_is_thousand(&chars, *p) && !((in & NUMPRS_CURRENCY) && parse_is_money_thousand(&chars, *p))) p++;
      continue;
    }
    if (!hex && (in & NUMPRS_THOUSANDS) && parse_is_thousand(&chars, c)) { out |= NUMPRS_THOUSANDS; p++; continue; }
    if (!hex && (in & NUMPRS_THOUSANDS) && (in & NUMPRS_CURRENCY) && parse_is_money_thousand(&chars, c)) { out |= NUMPRS_THOUSANDS | NUMPRS_CURRENCY; p++; continue; }
    if ((in & NUMPRS_TRAILING_PLUS) && c == '+' && !seen_plus && !seen_minus && !(out & (NUMPRS_TRAILING_PLUS | NUMPRS_TRAILING_MINUS))) { out |= NUMPRS_TRAILING_PLUS; p++; continue; }
    if ((in & NUMPRS_TRAILING_MINUS) && c == '-' && !seen_plus && !seen_minus && !(out & (NUMPRS_TRAILING_PLUS | NUMPRS_TRAILING_MINUS))) { out |= NUMPRS_TRAILING_MINUS | NUMPRS_NEG; p++; continue; }
    if (!hex && (in & NUMPRS_CURRENCY) && parse_currency_at(&chars, p)) { out |= NUMPRS_CURRENCY; p += chars.currency_length; continue; }
    if (seen_parens && !closed && c == ')') { closed = TRUE; p++; continue; }
    break;
  }
  if (seen_parens && !closed) return DISP_E_TYPEMISMATCH;
  if ((in & NUMPRS_USE_ALL) && *p) return DISP_E_TYPEMISMATCH;

  parse->cDig = stored;
  parse->dwOutFlags = out;
  parse->cchUsed = (int)(p - string);
  parse->nPwr10 = power;
  return S_OK;
}

/* ---- from the parsed digits to a value ------------------------------------------------------------------------------------------------------------------ */

typedef unsigned __int128 oa_u128;
#define PARSE_BIG ((oa_u128)1 << 120)

/* The value of the digits times 10^extra, rounded to a whole number (halves to even) in `result`; FALSE when it does not fit 120 bits. */
static BOOL parse_magnitude(const NUMPARSE *parse, const BYTE *digits, int extra, oa_u128 *result) {
  const int count = parse->cDig < 0 ? 0 : parse->cDig;
  if (parse->nBaseShift) {
    oa_u128 value = 0;
    for (int i = 0; i < count; i++) {
      if (value >= (PARSE_BIG >> parse->nBaseShift)) return FALSE;
      value = (value << parse->nBaseShift) | (digits[i] & 0xF);
    }
    int power = extra;
    for (; power > 0 && value; power--) { if (value >= PARSE_BIG / 10) return FALSE; value *= 10; }
    *result = value;
    return TRUE;
  }
  const long long scale = (long long)parse->nPwr10 + extra;
  long long keep = scale >= 0 ? count : (long long)count + scale;     /* the digits that stay in the whole part */
  oa_u128 value = 0;
  for (long long i = 0; i < keep && i < count; i++) {
    if (value >= PARSE_BIG / 10) return FALSE;
    value = value * 10 + digits[i];
  }
  if (scale >= 0) {
    for (long long i = 0; i < scale && value; i++) { if (value >= PARSE_BIG / 10) return FALSE; value *= 10; }
    *result = value;
    return TRUE;
  }
  /* round with the first dropped digit and whether anything non-zero follows it */
  if (keep < 0) { *result = 0; return TRUE; }
  const int first = keep < count ? digits[keep] : 0;
  BOOL rest = FALSE;
  for (long long i = keep + 1; i < count; i++) if (digits[i]) { rest = TRUE; break; }
  if (first > 5 || (first == 5 && (rest || (value & 1)))) value++;
  *result = value;
  return TRUE;
}

static BOOL parse_is_whole(const NUMPARSE *parse, const BYTE *digits) {
  if (parse->nBaseShift || parse->nPwr10 >= 0) return TRUE;
  const long long count = parse->cDig < 0 ? 0 : parse->cDig;
  long long drop = -(long long)parse->nPwr10;
  if (drop > count) drop = count;
  for (long long i = count - drop; i < count; i++) if (digits[i]) return FALSE;
  return TRUE;
}

/* The value as a double (not negated). FALSE when it is too big for a double; tiny values are 0. */
static BOOL parse_double(const NUMPARSE *parse, const BYTE *digits, double *result) {
  oa_u128 magnitude;
  if (parse->nBaseShift) {
    if (!parse_magnitude(parse, digits, 0, &magnitude)) return FALSE;
    unsigned long long hi = (unsigned long long)(magnitude >> 64), lo = (unsigned long long)magnitude;
    *result = (double)hi * 18446744073709551616.0 + (double)lo;
    return TRUE;
  }
  const int count = parse->cDig < 0 ? 0 : parse->cDig;
  unsigned long long mantissa = 0;
  int used = 0;
  long long power = parse->nPwr10;
  for (int i = 0; i < count; i++) {
    if (used < 19) { mantissa = mantissa * 10 + digits[i]; used++; }
    else power++;
  }
  if (mantissa == 0) { *result = 0.0; return TRUE; }
  int length = 0;
  for (unsigned long long m = mantissa; m; m /= 10) length++;
  const long long magnitude10 = power + length;              /* the value is below 10^magnitude10 */
  if (magnitude10 > 310) return FALSE;
  if (magnitude10 < -330) { *result = 0.0; return TRUE; }
  double value = (double)mantissa;
  if (power >= 0) {
    for (; power > 22; power -= 22) value *= 1e22;
    value *= oa_powi(10.0, (int)power);
  } else if (power >= -22) {
    value /= oa_powi(10.0, (int)-power);
  } else {
    for (; power < -22 && value > 1e-290; power += 22) value /= 1e22;
    if (power < -22) { *result = 0.0; return TRUE; }
    value /= oa_powi(10.0, (int)-power);
  }
  if (value > 1.7976931348623157e308) return FALSE;
  *result = value;
  return TRUE;
}

/* The parsed digits as a DECIMAL (96 bits, scale 0..28). */
static BOOL parse_decimal(const NUMPARSE *parse, const BYTE *digits, DECIMAL *out) {
  const oa_u128 limit = (oa_u128)1 << 96;
  oa_u128 mantissa = 0;
  int scale = 0;
  if (parse->nBaseShift || parse->nPwr10 >= 0) {
    if (!parse_magnitude(parse, digits, 0, &mantissa) || mantissa >= limit) return FALSE;      /* (a hex number has no power of ten; a non-negative one is applied by parse_magnitude) */
  } else {
    const long long count = parse->cDig < 0 ? 0 : parse->cDig;
    long long keep = count, drop_scale = -(long long)parse->nPwr10;
    if (drop_scale > 28) { keep = count - (drop_scale - 28); scale = 28; } else scale = (int)drop_scale;
    for (;;) {
      mantissa = 0;
      BOOL fits = TRUE;
      for (long long i = 0; i < keep && i < count; i++) {
        mantissa = mantissa * 10 + digits[i];
        if (mantissa >= limit) { fits = FALSE; break; }
      }
      if (fits && keep >= 0) {
        const int first = keep < count ? digits[keep] : 0;
        BOOL rest = FALSE;
        for (long long i = keep + 1; i < count; i++) if (digits[i]) { rest = TRUE; break; }
        if (first > 5 || (first == 5 && (rest || (mantissa & 1)))) mantissa++;
        if (mantissa < limit) break;
      }
      if (keep < 0) { mantissa = 0; break; }
      keep--; scale--;                                          /* too many digits: give up the last one */
      if (scale < 0) return FALSE;
    }
    if (keep <= 0 && count > 0 && keep < 0) mantissa = 0;
  }
  memset(out, 0, sizeof(*out));
  out->scale = (BYTE)scale;
  out->sign = (parse->dwOutFlags & NUMPRS_NEG) ? DECIMAL_NEG : 0;
  out->Lo64 = (ULONG64)mantissa;
  out->Hi32 = (ULONG)(mantissa >> 64);
  return TRUE;
}

#define PARSE_INTBITS (VTBIT_I1 | VTBIT_UI1 | VTBIT_I2 | VTBIT_UI2 | VTBIT_I4 | VTBIT_UI4 | VTBIT_I8 | VTBIT_UI8)
#define PARSE_FLOATBITS (VTBIT_R4 | VTBIT_R8 | VTBIT_CY | VTBIT_DECIMAL)

EXPORT HRESULT WINAPI VarNumFromParseNum(NUMPARSE *parse, BYTE *digits, ULONG vtbits, VARIANT *dest) {
  if (!parse || !digits || !dest) return E_INVALIDARG;
  const BOOL negative = (parse->dwOutFlags & NUMPRS_NEG) != 0;
  VARIANT result;
  memset(&result, 0, sizeof(result));

  if ((vtbits & PARSE_INTBITS) && (parse_is_whole(parse, digits) || !(vtbits & PARSE_FLOATBITS))) {
    oa_u128 m;
    if (!parse_magnitude(parse, digits, 0, &m)) { if (!(vtbits & PARSE_FLOATBITS)) return DISP_E_OVERFLOW; }
    else {
      BOOL done = TRUE;
      if (parse->nBaseShift && !negative) {
        /* the digits of a hex or octal number are bits: the smallest type that holds them, signed types showing the top bit as the sign */
        if ((vtbits & VTBIT_I1) && m <= 0xFF) { V_VT(&result) = VT_I1; V_I1(&result) = (signed char)(unsigned char)m; }
        else if ((vtbits & VTBIT_UI1) && m <= 0xFF) { V_VT(&result) = VT_UI1; V_UI1(&result) = (BYTE)m; }
        else if ((vtbits & VTBIT_I2) && m <= 0xFFFF) { V_VT(&result) = VT_I2; V_I2(&result) = (SHORT)(USHORT)m; }
        else if ((vtbits & VTBIT_UI2) && m <= 0xFFFF) { V_VT(&result) = VT_UI2; V_UI2(&result) = (USHORT)m; }
        else if ((vtbits & VTBIT_I4) && m <= 0xFFFFFFFFu) { V_VT(&result) = VT_I4; V_I4(&result) = (LONG)(ULONG)m; }
        else if ((vtbits & VTBIT_UI4) && m <= 0xFFFFFFFFu) { V_VT(&result) = VT_UI4; V_UI4(&result) = (ULONG)m; }
        else if ((vtbits & VTBIT_I8) && m <= 0xFFFFFFFFFFFFFFFFull) { V_VT(&result) = VT_I8; V_I8(&result) = (LONG64)(ULONG64)m; }
        else if ((vtbits & VTBIT_UI8) && m <= 0xFFFFFFFFFFFFFFFFull) { V_VT(&result) = VT_UI8; V_UI8(&result) = (ULONG64)m; }
        else done = FALSE;
      } else {
        if ((vtbits & VTBIT_I1) && m <= (negative ? 128u : 127u)) { V_VT(&result) = VT_I1; V_I1(&result) = (CHAR)(negative ? -(long long)m : (long long)m); }
        else if ((vtbits & VTBIT_UI1) && !(negative && m) && m <= 255u) { V_VT(&result) = VT_UI1; V_UI1(&result) = (BYTE)m; }
        else if ((vtbits & VTBIT_I2) && m <= (negative ? 32768u : 32767u)) { V_VT(&result) = VT_I2; V_I2(&result) = (SHORT)(negative ? -(long long)m : (long long)m); }
        else if ((vtbits & VTBIT_UI2) && !(negative && m) && m <= 65535u) { V_VT(&result) = VT_UI2; V_UI2(&result) = (USHORT)m; }
        else if ((vtbits & VTBIT_I4) && m <= (negative ? 2147483648u : 2147483647u)) { V_VT(&result) = VT_I4; V_I4(&result) = (LONG)(negative ? -(long long)m : (long long)m); }
        else if ((vtbits & VTBIT_UI4) && !(negative && m) && m <= 4294967295u) { V_VT(&result) = VT_UI4; V_UI4(&result) = (ULONG)m; }
        else if ((vtbits & VTBIT_I8) && m <= (negative ? (oa_u128)1 << 63 : ((oa_u128)1 << 63) - 1)) { V_VT(&result) = VT_I8; V_I8(&result) = negative ? (LONG64)(0 - (ULONG64)m) : (LONG64)m; }
        else if ((vtbits & VTBIT_UI8) && !(negative && m) && m <= 0xFFFFFFFFFFFFFFFFull) { V_VT(&result) = VT_UI8; V_UI8(&result) = (ULONG64)m; }
        else done = FALSE;
      }
      if (done) { *dest = result; return S_OK; }
      if (!(vtbits & PARSE_FLOATBITS)) return DISP_E_OVERFLOW;
    }
  }

  if (vtbits & (VTBIT_R4 | VTBIT_R8)) {
    double value;
    if (parse_double(parse, digits, &value)) {
      if (negative) value = -value;
      if ((vtbits & VTBIT_R4) && oa_fabs(value) <= 3.4028234663852886e38) { V_VT(&result) = VT_R4; V_R4(&result) = (float)value; *dest = result; return S_OK; }
      if (vtbits & VTBIT_R8) { V_VT(&result) = VT_R8; V_R8(&result) = value; *dest = result; return S_OK; }
    }
  }
  if (vtbits & VTBIT_CY) {
    oa_u128 m;
    if (parse_magnitude(parse, digits, 4, &m) && m <= (negative ? (oa_u128)1 << 63 : ((oa_u128)1 << 63) - 1)) {
      V_VT(&result) = VT_CY;
      V_CY(&result).int64 = negative ? (LONG64)(0 - (ULONG64)m) : (LONG64)m;
      *dest = result;
      return S_OK;
    }
  }
  if (vtbits & VTBIT_DECIMAL) {
    DECIMAL decimal;
    if (parse_decimal(parse, digits, &decimal)) {
      memset(&result, 0, sizeof(result));
      V_DECIMAL(&result) = decimal;
      V_VT(&result) = VT_DECIMAL;
      *dest = result;
      return S_OK;
    }
  }
  return DISP_E_OVERFLOW;
}
