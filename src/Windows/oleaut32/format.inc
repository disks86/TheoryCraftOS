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

/* Formatting: VarFormat and its tokenizer, the number/currency/percent/date-time helpers, month and weekday names.
 *
 * VarTokenizeFormatString splits a format string into its sections (separated by ';') and records for each the kind of values it applies to; the token buffer holds only
 * that, and VarFormatFromTokens reads the section texts again from the format string. Layout of the buffer: size, section count, first day of the week, first week of the
 * year, then per section its kind, its named-format number, and its start and length in the format string (16 bits each). */

#define FM_MAX_SECTIONS 4
#define FM_HEADER_SIZE 4
#define FM_SECTION_SIZE 6

/* the kinds of a section (the numbers are Windows') */
#define FM_T_UNKNOWN 0          /* only literal text: fits every value */
#define FM_T_GENERAL 1
#define FM_T_NUMBER  2
#define FM_T_DATE    4
#define FM_T_STRING  5

/* the named formats */
enum {
  FM_N_NONE, FM_N_GENERAL_NUMBER, FM_N_CURRENCY, FM_N_FIXED, FM_N_STANDARD, FM_N_PERCENT, FM_N_SCIENTIFIC, FM_N_YES_NO, FM_N_TRUE_FALSE, FM_N_ON_OFF,
  FM_N_GENERAL_DATE, FM_N_LONG_DATE, FM_N_MEDIUM_DATE, FM_N_SHORT_DATE, FM_N_LONG_TIME, FM_N_MEDIUM_TIME, FM_N_SHORT_TIME
};
static const struct { const WCHAR *name; BYTE id, type; } fm_named[] = {
  { L"General Number", FM_N_GENERAL_NUMBER, FM_T_GENERAL }, { L"Currency", FM_N_CURRENCY, FM_T_NUMBER }, { L"Fixed", FM_N_FIXED, FM_T_NUMBER },
  { L"Standard", FM_N_STANDARD, FM_T_NUMBER }, { L"Percent", FM_N_PERCENT, FM_T_NUMBER }, { L"Scientific", FM_N_SCIENTIFIC, FM_T_NUMBER },
  { L"Yes/No", FM_N_YES_NO, FM_T_NUMBER }, { L"True/False", FM_N_TRUE_FALSE, FM_T_NUMBER }, { L"On/Off", FM_N_ON_OFF, FM_T_NUMBER },
  { L"General Date", FM_N_GENERAL_DATE, FM_T_DATE }, { L"Long Date", FM_N_LONG_DATE, FM_T_DATE }, { L"Medium Date", FM_N_MEDIUM_DATE, FM_T_DATE },
  { L"Short Date", FM_N_SHORT_DATE, FM_T_DATE }, { L"Long Time", FM_N_LONG_TIME, FM_T_DATE }, { L"Medium Time", FM_N_MEDIUM_TIME, FM_T_DATE },
  { L"Short Time", FM_N_SHORT_TIME, FM_T_DATE },
};

/* ---- text output ------------------------------------------------------------------------------------------------------------------------------------ */

typedef struct { WCHAR *text; size_t length, capacity; BOOL failed; } FmOut;

static void fm_put(FmOut *o, WCHAR c) {
  if (o->failed) return;
  if (o->length + 1 >= o->capacity) {
    if (o->capacity > 0x10000000u) { o->failed = TRUE; return; }
    const size_t capacity = o->capacity ? o->capacity * 2 : 64;
    WCHAR *grown = oa_realloc(o->text, capacity * sizeof(WCHAR));
    if (!grown) { o->failed = TRUE; return; }
    o->text = grown;
    o->capacity = capacity;
  }
  o->text[o->length++] = c;
}
static void fm_puts(FmOut *o, const WCHAR *s, size_t n) { for (size_t i = 0; i < n; i++) fm_put(o, s[i]); }
static void fm_put_z(FmOut *o, const WCHAR *s) { fm_puts(o, s, oa_wcslen(s)); }
static void fm_put_ascii(FmOut *o, const char *s) { for (; *s; s++) fm_put(o, (WCHAR)*s); }
static void fm_free(FmOut *o) { oa_free(o->text); o->text = NULL; o->length = o->capacity = 0; }
/* The text as a BSTR (the buffer is freed). */
static HRESULT fm_finish(FmOut *o, BSTR *out) {
  if (o->failed) { fm_free(o); return E_OUTOFMEMORY; }
  BSTR result = SysAllocStringLen(o->text, (UINT)o->length);
  fm_free(o);
  if (!result) return E_OUTOFMEMORY;
  *out = result;
  return S_OK;
}

/* ---- locale ----------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  LCID lcid;
  ULONG flags;
  int first_day, first_week;            /* the arguments of VarFormat (0 = the locale's) */
  WCHAR decimal[8], thousand[8], date_separator[8], time_separator[8];
  int group[8], group_count;            /* the digit group sizes, e.g. 3 (and the last repeats if group_repeat) */
  BOOL group_repeat;
} FmLocale;

static void fm_parse_groups(const WCHAR *spec, int *sizes, int *count, BOOL *repeat) {
  *count = 0;
  *repeat = FALSE;
  for (const WCHAR *p = spec; *p;) {
    int size = 0;
    while (*p >= '0' && *p <= '9') size = size * 10 + (*p++ - '0');
    if (size == 0) { *repeat = *count > 0; break; }        /* "3;0": repeat the last size; "0" alone: no grouping */
    if (*count < 8) sizes[(*count)++] = size;
    if (*p == ';') p++; else break;
  }
}
static void fm_load_locale(FmLocale *loc, LCID lcid, ULONG flags, int first_day, int first_week) {
  WCHAR grouping[32];
  loc->lcid = lcid;
  loc->flags = flags;
  loc->first_day = first_day;
  loc->first_week = first_week;
  dt_locale_string(lcid, flags, LOCALE_SDECIMAL, loc->decimal, 8);
  dt_locale_string(lcid, flags, LOCALE_STHOUSAND, loc->thousand, 8);
  dt_locale_string(lcid, flags, LOCALE_SDATE, loc->date_separator, 8);
  dt_locale_string(lcid, flags, LOCALE_STIME, loc->time_separator, 8);
  dt_locale_string(lcid, flags, LOCALE_SGROUPING, grouping, 32);
  if (!loc->decimal[0]) { loc->decimal[0] = '.'; loc->decimal[1] = 0; }
  if (!loc->date_separator[0]) { loc->date_separator[0] = '/'; loc->date_separator[1] = 0; }
  if (!loc->time_separator[0]) { loc->time_separator[0] = ':'; loc->time_separator[1] = 0; }
  fm_parse_groups(grouping, loc->group, &loc->group_count, &loc->group_repeat);
}
/* Does a separator follow the digit of this power of ten (is it the last digit of a group)? */
static BOOL fm_group_boundary(const int *sizes, int count, BOOL repeat, int power) {
  if (power <= 0 || count == 0) return FALSE;
  int position = 0;
  for (int i = 0; i < count; i++) { position += sizes[i]; if (position == power) return TRUE; if (position > power) return FALSE; }
  if (!repeat) return FALSE;
  const int last = sizes[count - 1];
  return (power - position) % last == 0;
}

/* ---- decimal numbers -------------------------------------------------------------------------------------------------------------------------------- */

/* value = 0.d[0]d[1]...d[n-1] * 10^pt, d[0] != '0' (n = 0 is zero) */
typedef struct { BOOL neg; int n, pt; char d[40]; } FmNum;

static void fm_strip(FmNum *x) { while (x->n > 0 && x->d[x->n - 1] == '0') x->n--; if (!x->n) x->pt = 0; }

static void fm_from_u64(FmNum *x, unsigned long long v, BOOL neg) {
  char text[24];
  int length = 0;
  while (v) { text[length++] = (char)('0' + v % 10); v /= 10; }
  memset(x, 0, sizeof(*x));
  x->neg = neg;
  x->n = x->pt = length;
  for (int i = 0; i < length; i++) x->d[i] = text[length - 1 - i];
  fm_strip(x);
}
static void fm_from_i64(FmNum *x, long long v) { fm_from_u64(x, v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v, v < 0); }

/* Keep `index` digits (half up). A negative index leaves nothing. */
static void fm_round(FmNum *x, int index) {
  if (index >= x->n) return;
  if (index < 0) { x->n = 0; x->pt = 0; return; }
  const BOOL up = x->d[index] >= '5';
  x->n = index;
  if (up) {
    int i = index - 1;
    while (i >= 0 && x->d[i] == '9') i--;
    if (i < 0) { x->d[0] = '1'; x->n = 1; x->pt++; }
    else { x->d[i]++; x->n = i + 1; }
  }
  fm_strip(x);
}
static int fm_digit(const FmNum *x, int power) {
  const int index = x->pt - 1 - power;
  return index >= 0 && index < x->n ? x->d[index] - '0' : 0;
}

static void fm_big_multiply(DWORD *limb, int *count, DWORD factor) {
  unsigned long long carry = 0;
  for (int i = 0; i < *count; i++) {
    const unsigned long long t = (unsigned long long)limb[i] * factor + carry;
    limb[i] = (DWORD)(t % 1000000000u);
    carry = t / 1000000000u;
  }
  while (carry) { limb[(*count)++] = (DWORD)(carry % 1000000000u); carry /= 1000000000u; }
}

/* The exact decimal expansion of a double, rounded to `significant` digits; FALSE for NaN and the infinities. */
static BOOL fm_from_double(FmNum *x, double v, int significant) {
  memset(x, 0, sizeof(*x));
  if (v != v || v - v != 0.0) return FALSE;
  unsigned long long bits;
  memcpy(&bits, &v, sizeof(bits));
  x->neg = (BOOL)(bits >> 63);
  int exponent = (int)((bits >> 52) & 0x7ff);
  unsigned long long mantissa = bits & 0xFFFFFFFFFFFFFULL;
  if (exponent == 0) exponent = 1; else mantissa |= 1ULL << 52;
  exponent -= 1075;
  if (!mantissa) return TRUE;
  DWORD limb[112];
  int count = 0, decimal_exponent = 0;
  for (unsigned long long m = mantissa; m; m /= 1000000000u) limb[count++] = (DWORD)(m % 1000000000u);
  if (exponent > 0) {
    for (int left = exponent; left > 0;) { const int step = left > 29 ? 29 : left; fm_big_multiply(limb, &count, 1u << step); left -= step; }
  } else {
    static const DWORD fives[14] = { 1, 5, 25, 125, 625, 3125, 15625, 78125, 390625, 1953125, 9765625, 48828125, 244140625, 1220703125 };
    for (int left = -exponent; left > 0;) { const int step = left > 13 ? 13 : left; fm_big_multiply(limb, &count, fives[step]); left -= step; }
    decimal_exponent = exponent;
  }
  char text[1024];
  int length = 0;
  for (int i = count - 1; i >= 0; i--) {
    char part[9];
    DWORD value = limb[i];
    for (int k = 8; k >= 0; k--) { part[k] = (char)('0' + value % 10); value /= 10; }
    int first = 0;
    if (i == count - 1) while (first < 8 && part[first] == '0') first++;
    for (int k = first; k < 9; k++) text[length++] = part[k];
  }
  x->pt = length + decimal_exponent;
  x->n = length > 39 ? 39 : length;
  for (int i = 0; i < x->n; i++) x->d[i] = text[i];
  fm_round(x, significant);
  fm_strip(x);
  return TRUE;
}

/* ---- the values to format --------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  BOOL is_string;
  BSTR string;            /* (owned by the VARIANT copy) */
  double number;          /* as a double (for dates) */
  FmNum digits;           /* the decimal digits of the number */
} FmValue;

/* The number in a VARIANT (strings are converted with the locale). */
static HRESULT fm_variant_number(VARIANT *v, LCID lcid, double *d, FmNum *x) {
  long long i = 0;
  switch (V_VT(v)) {
    case VT_EMPTY: i = 0; break;
    case VT_I1: i = V_I1(v); break;
    case VT_UI1: i = V_UI1(v); break;
    case VT_I2: i = V_I2(v); break;
    case VT_UI2: i = V_UI2(v); break;
    case VT_I4: case VT_INT: i = V_I4(v); break;
    case VT_UI4: case VT_UINT: i = V_UI4(v); break;
    case VT_BOOL: i = V_BOOL(v); break;
    case VT_I8: i = V_I8(v); break;
    case VT_UI8:
      *d = (double)V_UI8(v);
      fm_from_u64(x, V_UI8(v), FALSE);
      return S_OK;
    case VT_R4:
      *d = V_R4(v);
      return fm_from_double(x, V_R4(v), 7) ? S_OK : DISP_E_OVERFLOW;
    case VT_R8:
      *d = V_R8(v);
      return fm_from_double(x, V_R8(v), 15) ? S_OK : DISP_E_OVERFLOW;
    case VT_DATE:
      *d = V_DATE(v);
      return fm_from_double(x, V_DATE(v), 15) ? S_OK : DISP_E_OVERFLOW;
    case VT_CY:
      *d = (double)V_CY(v).int64 / 10000.0;
      fm_from_i64(x, V_CY(v).int64);
      if (x->n) x->pt -= 4;
      return S_OK;
    case VT_DECIMAL: case VT_BSTR: {
      VARIANT t;
      VariantInit(&t);
      HRESULT hr = VariantChangeTypeEx(&t, v, lcid, 0, VT_R8);
      if (FAILED(hr)) return hr;
      *d = V_R8(&t);
      return fm_from_double(x, V_R8(&t), 15) ? S_OK : DISP_E_OVERFLOW;
    }
    default: return DISP_E_TYPEMISMATCH;
  }
  *d = (double)i;
  fm_from_i64(x, i);
  return S_OK;
}

/* The general text of a VARIANT. */
static HRESULT fm_general(VARIANT *v, LCID lcid, BSTR *out) {
  if (V_VT(v) == VT_BSTR) {
    *out = SysAllocStringLen(V_BSTR(v), SysStringLen(V_BSTR(v)));
    return *out ? S_OK : E_OUTOFMEMORY;
  }
  VARIANT t;
  VariantInit(&t);
  HRESULT hr = VariantChangeTypeEx(&t, v, lcid, 0, VT_BSTR);
  if (FAILED(hr)) return hr;
  *out = V_BSTR(&t);
  return S_OK;
}

/* ---- number formats --------------------------------------------------------------------------------------------------------------------------------- */

enum { FI_ZERO, FI_HASH, FI_POINT, FI_GROUP, FI_PERCENT, FI_EXP, FI_LIT, FI_SPACE };
typedef struct { BYTE kind, aux; WORD off, len; } FmItem;       /* FI_EXP: aux bit 0 = '+' form, bit 1 = lower case, off = count of '0' placeholders */

/* The characters of a quoted string or an escape: the literal's start and length, and the index after it. */
static size_t fm_literal_at(const WCHAR *s, size_t len, size_t i, size_t *start, size_t *length) {
  if (s[i] == '"') {
    size_t j = i + 1;
    while (j < len && s[j] != '"') j++;
    *start = i + 1; *length = j - i - 1;
    return j < len ? j + 1 : j;
  }
  *start = i + 1; *length = i + 1 < len ? 1 : 0;      /* a backslash */
  return i + 1 < len ? i + 2 : i + 1;
}

static size_t fm_lex_number(const WCHAR *s, size_t len, FmItem *items) {
  size_t count = 0;
  BOOL point = FALSE, digits_seen = FALSE;
  for (size_t i = 0; i < len;) {
    const WCHAR c = s[i];
    FmItem *it = &items[count];
    it->aux = 0; it->off = 0; it->len = 0;
    if (c == '"' || c == '\\') {
      size_t start, length;
      i = fm_literal_at(s, len, i, &start, &length);
      it->kind = FI_LIT; it->off = (WORD)start; it->len = (WORD)length;
      count++;
    } else if (c == '*') { i += 2; }                                      /* a fill character: dropped */
    else if (c == '_') {
      it->kind = FI_SPACE; count++;
      i += 2;
    } else if (c == '#' || c == '0') { it->kind = c == '#' ? FI_HASH : FI_ZERO; digits_seen = TRUE; count++; i++; }
    else if (c == '.' && !point) { it->kind = FI_POINT; point = TRUE; count++; i++; }
    else if (c == ',' && count > 0 && (items[count - 1].kind == FI_HASH || items[count - 1].kind == FI_ZERO) && i + 1 < len && (s[i + 1] == '#' || s[i + 1] == '0')) {
      it->kind = FI_GROUP; count++; i++;
    } else if (c == '%') { it->kind = FI_PERCENT; count++; i++; }
    else if ((c == 'e' || c == 'E') && digits_seen && i + 1 < len && (s[i + 1] == '+' || s[i + 1] == '-')) {
      it->kind = FI_EXP;
      it->aux = (BYTE)((s[i + 1] == '+' ? 1 : 0) | (c == 'e' ? 2 : 0));
      i += 2;
      while (i < len && (s[i] == '0' || s[i] == '#')) { if (s[i] == '0') it->off++; i++; }
      count++;
    } else { it->kind = FI_LIT; it->off = (WORD)i; it->len = 1; count++; i++; }
  }
  return count;
}

/* Writes a number with the picture of a format section. */
static void fm_emit_number(FmOut *o, const FmNum *value, BOOL show_minus, const WCHAR *s, size_t len, const FmLocale *loc) {
  FmItem *items = oa_alloc((len + 1) * sizeof(FmItem));
  if (!items) { o->failed = TRUE; return; }
  const size_t count = fm_lex_number(s, len, items);
  FmNum x = *value;
  int percent = 0, int_slots = 0, frac_slots = 0, last_frac_zero = -1;
  BOOL has_point = FALSE, has_exp = FALSE, grouping = FALSE;
  int exp_zeros = 0;
  BOOL exp_plus = FALSE, exp_lower = FALSE;
  for (size_t i = 0; i < count; i++) {
    switch (items[i].kind) {
      case FI_ZERO: case FI_HASH:
        if (has_exp) break;
        if (has_point) { if (items[i].kind == FI_ZERO) last_frac_zero = frac_slots; frac_slots++; } else int_slots++;
        break;
      case FI_POINT: has_point = TRUE; break;
      case FI_GROUP: grouping = TRUE; break;
      case FI_PERCENT: percent++; break;
      case FI_EXP:
        if (!has_exp) { has_exp = TRUE; exp_zeros = items[i].off; exp_plus = items[i].aux & 1; exp_lower = (items[i].aux & 2) != 0; }
        break;
      default: break;
    }
  }
  int leftmost_zero = -1;           /* the power of ten of the leftmost '0' in the integer part */
  {
    int slot = 0;
    for (size_t i = 0; i < count && items[i].kind != FI_POINT && items[i].kind != FI_EXP; i++) {
      if (items[i].kind == FI_ZERO && leftmost_zero < 0) leftmost_zero = int_slots - 1 - slot;
      if (items[i].kind == FI_ZERO || items[i].kind == FI_HASH) slot++;
    }
  }
  if (x.n) x.pt += 2 * percent;
  int exponent = 0;
  if (has_exp) {
    if (x.n) { fm_round(&x, int_slots + frac_slots); exponent = x.pt - int_slots; x.pt = int_slots; }
  } else fm_round(&x, x.pt + frac_slots);
  const int integer_digits = x.n && x.pt > 0 ? x.pt : 0;
  int fraction_shown = last_frac_zero + 1;
  for (int t = frac_slots - 1; t >= fraction_shown; t--) if (fm_digit(&x, -(t + 1))) { fraction_shown = t + 1; break; }

  if (show_minus && value->neg && value->n) fm_put(o, '-');
  int int_slot = 0, frac_slot = 0;
  BOOL excess_done = FALSE, seen_point = FALSE;
#define FM_DIGIT(power) do { fm_put(o, (WCHAR)('0' + fm_digit(&x, (power)))); \
    if (grouping && fm_group_boundary(loc->group, loc->group_count, loc->group_repeat, (power))) fm_put_z(o, loc->thousand); } while (0)
#define FM_EXCESS() do { if (!excess_done) { excess_done = TRUE; for (int p = integer_digits - 1; p >= int_slots; p--) FM_DIGIT(p); } } while (0)
  for (size_t i = 0; i < count && !o->failed; i++) {
    const FmItem *it = &items[i];
    switch (it->kind) {
      case FI_ZERO: case FI_HASH:
        if (seen_point) {
          if (frac_slot < fraction_shown) fm_put(o, (WCHAR)('0' + fm_digit(&x, -(frac_slot + 1))));
          frac_slot++;
        } else {
          if (int_slot == 0) FM_EXCESS();
          const int power = int_slots - 1 - int_slot;
          if (power < integer_digits || power <= leftmost_zero) FM_DIGIT(power);
          int_slot++;
        }
        break;
      case FI_POINT:
        if (int_slots == 0) FM_EXCESS();
        seen_point = TRUE;
        fm_put_z(o, loc->decimal);
        break;
      case FI_PERCENT: fm_put(o, '%'); break;
      case FI_LIT: fm_puts(o, s + it->off, it->len); break;
      case FI_SPACE: fm_put(o, ' '); break;
      case FI_EXP: {
        fm_put(o, exp_lower ? 'e' : 'E');
        if (exponent < 0) fm_put(o, '-'); else if (exp_plus) fm_put(o, '+');
        char digits[12];
        int n = 0;
        for (unsigned e = (unsigned)(exponent < 0 ? -exponent : exponent); e || n == 0; e /= 10) digits[n++] = (char)('0' + e % 10);
        for (int pad = n; pad < exp_zeros; pad++) fm_put(o, '0');
        while (n) fm_put(o, (WCHAR)digits[--n]);
        break;
      }
      default: break;
    }
  }
#undef FM_DIGIT
#undef FM_EXCESS
  oa_free(items);
}

/* A plain number as the locale writes it: `digits` decimals, a leading zero or not, digit groups or not. */
static void fm_plain_number(FmOut *o, FmNum x, int digits, BOOL leading_zero, BOOL group, const WCHAR *decimal, const WCHAR *thousand, const int *sizes, int size_count, BOOL repeat) {
  if (x.n) fm_round(&x, x.pt + digits);
  const int integer_digits = x.n && x.pt > 0 ? x.pt : 0;
  if (!integer_digits) { if (leading_zero || digits == 0) fm_put(o, '0'); }
  else for (int p = integer_digits - 1; p >= 0; p--) {
    fm_put(o, (WCHAR)('0' + fm_digit(&x, p)));
    if (group && fm_group_boundary(sizes, size_count, repeat, p)) fm_put_z(o, thousand);
  }
  if (digits > 0) {
    fm_put_z(o, decimal);
    for (int t = 1; t <= digits; t++) fm_put(o, (WCHAR)('0' + fm_digit(&x, -t)));
  }
}

/* A pattern of the locale's number and currency layouts: 'n' is the number, '$' the currency symbol. */
static void fm_put_pattern(FmOut *o, const char *pattern, const WCHAR *number, size_t number_length, const WCHAR *symbol) {
  for (; *pattern; pattern++) {
    if (*pattern == 'n') fm_puts(o, number, number_length);
    else if (*pattern == '$') fm_put_z(o, symbol);
    else fm_put(o, (WCHAR)*pattern);
  }
}

/* nDigits, nLeadingDigit, nParens and nGrouping of VarFormatNumber/Currency/Percent: -2 is "use the default", -1 is true */
static BOOL fm_tristate(int value, BOOL fallback) { return value == -2 ? fallback : value != 0; }

/* VarFormatNumber, VarFormatPercent (percent = TRUE) and VarFormatCurrency (currency = TRUE) once the value is known. */
static HRESULT fm_format_locale_number(FmNum x, int digits, int leading, int parens, int group, ULONG flags, BOOL percent, BOOL currency, BSTR *out) {
  const LCID lcid = LOCALE_USER_DEFAULT;
  FmLocale loc;
  fm_load_locale(&loc, lcid, flags, 0, 0);
  WCHAR decimal[8], thousand[8], symbol[16], grouping[32];
  int sizes[8], size_count;
  BOOL repeat;
  memcpy(decimal, loc.decimal, sizeof(decimal));
  memcpy(thousand, loc.thousand, sizeof(thousand));
  symbol[0] = 0;
  DWORD positive_style = 0, negative_style;
  if (currency) {
    dt_locale_string(lcid, flags, LOCALE_SMONDECIMALSEP, decimal, 8);
    dt_locale_string(lcid, flags, LOCALE_SMONTHOUSANDSEP, thousand, 8);
    dt_locale_string(lcid, flags, LOCALE_SMONGROUPING, grouping, 32);
    dt_locale_string(lcid, flags, LOCALE_SCURRENCY, symbol, 16);
    if (digits < 0) digits = (int)dt_locale_number(lcid, flags, LOCALE_ICURRDIGITS, 2);
    positive_style = dt_locale_number(lcid, flags, LOCALE_ICURRENCY, 0);
    negative_style = dt_locale_number(lcid, flags, LOCALE_INEGCURR, 0);
    fm_parse_groups(grouping, sizes, &size_count, &repeat);
  } else {
    memcpy(sizes, loc.group, sizeof(sizes));
    size_count = loc.group_count;
    repeat = loc.group_repeat;
    if (digits < 0) digits = (int)dt_locale_number(lcid, flags, LOCALE_IDIGITS, 2);
    negative_style = dt_locale_number(lcid, flags, LOCALE_INEGNUMBER, 1);
  }
  const BOOL use_leading = fm_tristate(leading, dt_locale_number(lcid, flags, LOCALE_ILZERO, 1) != 0);
  const BOOL use_group = fm_tristate(group, size_count > 0);
  const BOOL negative = x.neg && x.n;
  if (percent && x.n) x.pt += 2;

  FmOut body = { 0 };
  fm_plain_number(&body, x, digits, use_leading, use_group, decimal, thousand, sizes, size_count, repeat);
  if (percent) fm_put(&body, '%');
  if (body.failed) { fm_free(&body); return E_OUTOFMEMORY; }

  /* the layout of the sign and the symbol */
  static const char *const number_negative[5] = { "(n)", "-n", "- n", "n-", "n -" };
  static const char *const currency_positive[4] = { "$n", "n$", "$ n", "n $" };
  static const char *const currency_negative[16] = { "($n)", "-$n", "$-n", "$n-", "(n$)", "-n$", "n-$", "n$-", "-n $", "-$ n", "n $-", "$ n-", "$ -n", "n- $", "($ n)", "(n $)" };
  const char *pattern;
  if (!negative) pattern = currency ? currency_positive[positive_style & 3] : "n";
  else {
    if (!currency) {
      if (parens == -1 || (parens == -2 && negative_style == 0)) negative_style = 0;
      else if (parens == 0 && negative_style == 0) negative_style = 1;
      pattern = number_negative[negative_style > 4 ? 1 : negative_style];
    } else {
      static const BYTE paren_of[4] = { 0, 4, 14, 15 }, minus_of[4] = { 1, 5, 9, 8 };
      if (parens == -1) negative_style = paren_of[positive_style & 3];
      else if (parens == 0 && (negative_style == 0 || negative_style == 4 || negative_style == 14 || negative_style == 15)) negative_style = minus_of[positive_style & 3];
      pattern = currency_negative[negative_style > 15 ? 1 : negative_style];
    }
  }
  FmOut result = { 0 };
  fm_put_pattern(&result, pattern, body.text ? body.text : L"", body.length, symbol);
  fm_free(&body);
  return fm_finish(&result, out);
}

/* ---- date and time formats -------------------------------------------------------------------------------------------------------------------------- */

static int fm_run(const WCHAR *s, size_t len, size_t i) {
  int run = 1;
  const WCHAR c = oa_towlower(s[i]);
  while (i + (size_t)run < len && oa_towlower(s[i + (size_t)run]) == c) run++;
  return run;
}
/* Does the text at s[i] spell the token (case-insensitive)? */
static BOOL fm_matches(const WCHAR *s, size_t len, size_t i, const char *token) {
  for (; *token; token++, i++) if (i >= len || oa_towlower(s[i]) != (WCHAR)*token) return FALSE;
  return TRUE;
}
/* The length of an am/pm token at s[i] (0 if there is none) */
static size_t fm_ampm_length(const WCHAR *s, size_t len, size_t i) {
  if (fm_matches(s, len, i, "am/pm")) return 5;
  if (fm_matches(s, len, i, "ampm")) return 4;
  if (fm_matches(s, len, i, "a/p")) return 3;
  return 0;
}

static void fm_put_number(FmOut *o, unsigned value, int width) {
  char digits[12];
  int n = 0;
  do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
  for (int pad = n; pad < width; pad++) fm_put(o, '0');
  while (n) fm_put(o, (WCHAR)digits[--n]);
}

/* The week of the year of a date. mode: 1 = week 1 contains January 1st, 2 = week 1 has at least four days of the year, 3 = week 1 is the first full week. */
static int fm_week_of_year(long long dn, int mode, int first_day) {
  long long year;
  int month, day;
  dt_civil_from_days(dn, &year, &month, &day);
  const long long jan1 = dt_days_from_civil(year, 1, 1);
  const int doy = (int)(dn - jan1) + 1;
  const int offset = (dt_weekday(jan1) - first_day + 7) % 7;             /* days of January 1st's week before January 1st */
  if (mode == 3) {
    const int first_start = 1 + (7 - offset) % 7;
    if (doy < first_start) return fm_week_of_year(jan1 - 1, mode, first_day);
    return (doy - first_start) / 7 + 1;
  }
  if (mode == 2) {
    const int week = (doy - 1 + offset) / 7 + (7 - offset >= 4 ? 1 : 0);
    if (week == 0) return fm_week_of_year(jan1 - 1, mode, first_day);
    const long long next_jan1 = dt_days_from_civil(year + 1, 1, 1);
    const int next_offset = (dt_weekday(next_jan1) - first_day + 7) % 7;
    if (7 - next_offset >= 4 && dn >= next_jan1 - next_offset) return 1;
    return week;
  }
  return (doy - 1 + offset) / 7 + 1;
}

static BOOL fm_long_date(const FmLocale *loc, const SYSTEMTIME *st, WCHAR *out, int capacity) {
  if (st->wYear >= 1601 && GetDateFormatW(loc->lcid, (loc->flags & LOCALE_NOUSEROVERRIDE) | DATE_LONGDATE, st, NULL, out, capacity)) return TRUE;
  WCHAR picture[120];
  dt_locale_string(loc->lcid, loc->flags, LOCALE_SLONGDATE, picture, 120);
  return picture[0] && dt_picture_date(loc->lcid, loc->flags, picture, st, out, (size_t)capacity) < (size_t)capacity;
}
static BOOL fm_time_text(const FmLocale *loc, const SYSTEMTIME *st, WCHAR *out, int capacity) {
  SYSTEMTIME clock = *st;
  clock.wYear = 2000; clock.wMonth = 1; clock.wDay = 1;
  return GetTimeFormatW(loc->lcid, loc->flags & LOCALE_NOUSEROVERRIDE, &clock, NULL, out, capacity) != 0;
}

/* Writes a date with the picture of a format section. */
static HRESULT fm_emit_date(FmOut *o, const WCHAR *s, size_t len, double date, const FmLocale *loc) {
  UDATE ud;
  if (FAILED(VarUdateFromDate(date, 0, &ud))) return E_INVALIDARG;
  const SYSTEMTIME *st = &ud.st;
  const long long dn = (long long)(date < 0.0 ? oa_ceil(date) : oa_floor(date));
  BOOL twelve_hour = FALSE, previous_hour = FALSE;
  for (size_t i = 0; i < len; i++) if (fm_ampm_length(s, len, i)) { twelve_hour = TRUE; break; }
  WCHAR text[160];
  for (size_t i = 0; i < len && !o->failed;) {
    const WCHAR c = s[i];
    const WCHAR lower = oa_towlower(c);
    int run = 1;
    size_t consumed;
    BOOL date_token = TRUE;
    switch (lower) {
      case 'd': {
        run = fm_run(s, len, i); if (run > 6) run = 6;
        if (run <= 2) fm_put_number(o, st->wDay, run);
        else if (run <= 4) { dt_locale_string(loc->lcid, loc->flags, (run == 3 ? LOCALE_SABBREVDAYNAME1 : LOCALE_SDAYNAME1) + (st->wDayOfWeek + 6) % 7, text, 80); fm_put_z(o, text); }
        else {
          text[0] = 0;
          if (run == 5) dt_short_date(loc->lcid, loc->flags, st, text, 160); else fm_long_date(loc, st, text, 160);
          fm_put_z(o, text);
        }
        consumed = (size_t)run;
        break;
      }
      case 'm': {
        run = fm_run(s, len, i); if (run > 4) run = 4;
        if (run <= 2) fm_put_number(o, previous_hour ? st->wMinute : st->wMonth, run);
        else { dt_locale_string(loc->lcid, loc->flags, (run == 3 ? LOCALE_SABBREVMONTHNAME1 : LOCALE_SMONTHNAME1) + st->wMonth - 1, text, 80); fm_put_z(o, text); }
        consumed = (size_t)run;
        break;
      }
      case 'y': {
        run = fm_run(s, len, i); if (run > 4) run = 4;
        if (run == 1) fm_put_number(o, ud.wDayOfYear, 1);
        else if (run == 2) fm_put_number(o, st->wYear % 100, 2);
        else if (run == 3) fm_put_number(o, ud.wDayOfYear, 3);
        else fm_put_number(o, st->wYear, 4);
        consumed = (size_t)run;
        break;
      }
      case 'h': {
        run = fm_run(s, len, i); if (run > 2) run = 2;
        unsigned hour = st->wHour;
        if (twelve_hour) { hour %= 12; if (!hour) hour = 12; }
        fm_put_number(o, hour, run);
        consumed = (size_t)run;
        break;
      }
      case 'n': run = fm_run(s, len, i); if (run > 2) run = 2; fm_put_number(o, st->wMinute, run); consumed = (size_t)run; break;
      case 's': run = fm_run(s, len, i); if (run > 2) run = 2; fm_put_number(o, st->wSecond, run); consumed = (size_t)run; break;
      case 'q': fm_put_number(o, (unsigned)(st->wMonth - 1) / 3 + 1, 1); consumed = 1; break;
      case 'w': {
        run = fm_run(s, len, i); if (run > 2) run = 2;
        int first_day = loc->first_day ? loc->first_day - 1 : (int)((dt_locale_number(loc->lcid, loc->flags, LOCALE_IFIRSTDAYOFWEEK, 0) + 1) % 7);
        if (run == 1) fm_put_number(o, (unsigned)((st->wDayOfWeek - first_day + 7) % 7 + 1), 1);
        else {
          int mode = loc->first_week;
          if (!mode) {
            const DWORD locale_mode = dt_locale_number(loc->lcid, loc->flags, LOCALE_IFIRSTWEEKOFYEAR, 0);
            mode = locale_mode == 1 ? 3 : locale_mode == 2 ? 2 : 1;
          }
          fm_put_number(o, (unsigned)fm_week_of_year(dn, mode, first_day), 1);
        }
        consumed = (size_t)run;
        break;
      }
      case 'c': {
        text[0] = 0;
        if (dn != 0) dt_short_date(loc->lcid, loc->flags, st, text, 80);
        fm_put_z(o, text);
        if (dn == 0 || st->wHour || st->wMinute || st->wSecond) {
          if (dn != 0) fm_put(o, ' ');
          text[0] = 0;
          fm_time_text(loc, st, text, 80);
          fm_put_z(o, text);
        }
        consumed = 1;
        break;
      }
      case 't':
        run = fm_run(s, len, i);
        if (run >= 5) { text[0] = 0; fm_time_text(loc, st, text, 80); fm_put_z(o, text); consumed = 5; }
        else { fm_puts(o, s + i, (size_t)run); consumed = (size_t)run; date_token = FALSE; }
        break;
      case 'a': {
        const size_t n = fm_ampm_length(s, len, i);
        if (!n) { fm_put(o, c); consumed = 1; date_token = FALSE; break; }
        const BOOL pm = st->wHour >= 12;
        if (n == 4) { dt_locale_string(loc->lcid, loc->flags, pm ? LOCALE_S2359 : LOCALE_S1159, text, 40); fm_put_z(o, text); }
        else {
          const BOOL lower_case = c == 'a' || c == 'A' ? c == 'a' : FALSE;
          if (n == 5) dt_locale_string(loc->lcid, loc->flags, pm ? LOCALE_S2359 : LOCALE_S1159, text, 40);
          else { text[0] = pm ? 'P' : 'A'; text[1] = 0; }
          for (const WCHAR *t = text; *t; t++) fm_put(o, lower_case ? oa_towlower(*t) : oa_towupper(*t));
        }
        consumed = n;
        break;
      }
      case '/': fm_put_z(o, loc->date_separator); consumed = 1; date_token = FALSE; break;
      case ':': fm_put_z(o, loc->time_separator); consumed = 1; date_token = FALSE; break;
      case '"': case '\\': {
        size_t start, length;
        consumed = fm_literal_at(s, len, i, &start, &length) - i;
        fm_puts(o, s + start, length);
        date_token = FALSE;
        break;
      }
      case '*': consumed = i + 1 < len ? 2 : 1; date_token = FALSE; break;
      case '_': fm_put(o, ' '); consumed = i + 1 < len ? 2 : 1; date_token = FALSE; break;
      default: fm_put(o, c); consumed = 1; date_token = FALSE; break;
    }
    if (date_token) previous_hour = lower == 'h';
    i += consumed;
  }
  return o->failed ? E_OUTOFMEMORY : S_OK;
}

/* ---- string formats --------------------------------------------------------------------------------------------------------------------------------- */

static void fm_emit_string(FmOut *o, const WCHAR *s, size_t len, const WCHAR *text, size_t text_length) {
  size_t placeholders = 0;
  BOOL upper = FALSE, lower = FALSE, left_align = FALSE;
  for (size_t i = 0; i < len;) {
    const WCHAR c = s[i];
    if (c == '"' || c == '\\') { size_t start, length; i = fm_literal_at(s, len, i, &start, &length); continue; }
    if (c == '*' || c == '_') { i += 2; continue; }
    if (c == '@' || c == '&') placeholders++;
    else if (c == '>') upper = TRUE;
    else if (c == '<') lower = TRUE;
    else if (c == '!') left_align = TRUE;
    i++;
  }
#define FM_CASED(ch) (lower ? oa_towlower(ch) : upper ? oa_towupper(ch) : (ch))
  size_t skip = 0, used = 0;       /* placeholders left blank at the start (right aligned text), characters used */
  if (text_length < placeholders && !left_align) skip = placeholders - text_length;
  size_t slot = 0;
  for (size_t i = 0; i < len && !o->failed;) {
    const WCHAR c = s[i];
    if (c == '"' || c == '\\') { size_t start, length; i = fm_literal_at(s, len, i, &start, &length); fm_puts(o, s + start, length); continue; }
    if (c == '*') { i += 2; continue; }
    if (c == '_') { fm_put(o, ' '); i += 2; continue; }
    if (c == '@' || c == '&') {
      if (slot < skip || used >= text_length) { if (c == '@') fm_put(o, ' '); }
      else fm_put(o, FM_CASED(text[used++]));
      slot++;
    } else if (c != '<' && c != '>' && c != '!') fm_put(o, c);
    i++;
  }
  while (used < text_length) { fm_put(o, FM_CASED(text[used])); used++; }
#undef FM_CASED
}

/* ---- tokenizing ------------------------------------------------------------------------------------------------------------------------------------- */

/* The kind of values a section is for: that of the first format character in it. */
static int fm_section_kind(const WCHAR *s, size_t len) {
  for (size_t i = 0; i < len;) {
    const WCHAR c = s[i], lower = oa_towlower(c);
    if (c == '"' || c == '\\') { size_t start, length; i = fm_literal_at(s, len, i, &start, &length); continue; }
    if (c == '*' || c == '_') { i += 2; continue; }
    if (c == '#' || c == '0') return FM_T_NUMBER;
    if (c == '@' || c == '&' || c == '<' || c == '>' || c == '!') return FM_T_STRING;
    if (lower == 'd' || lower == 'm' || lower == 'y' || lower == 'h' || lower == 'n' || lower == 's' || lower == 'q' || lower == 'w' || lower == 'c') return FM_T_DATE;
    if (lower == 't' && fm_run(s, len, i) >= 5) return FM_T_DATE;
    if (lower == 'a' && fm_ampm_length(s, len, i)) return FM_T_DATE;
    i++;
  }
  return FM_T_UNKNOWN;
}

EXPORT HRESULT WINAPI VarTokenizeFormatString(LPOLESTR format, LPBYTE tokens, int token_size, int first_day, int first_week, LCID lcid, int *actual) {
  (void)lcid;
  if (actual) *actual = 0;
  if (!tokens || token_size < FM_HEADER_SIZE) return E_INVALIDARG;
  if (first_day < 0 || first_day > 7 || first_week < 0 || first_week > 3) return E_INVALIDARG;
  size_t length = format ? oa_wcslen(format) : 0;
  if (length > 0xFFFF) return E_INVALIDARG;
  unsigned starts[FM_MAX_SECTIONS], lengths[FM_MAX_SECTIONS], kinds[FM_MAX_SECTIONS], named[FM_MAX_SECTIONS];
  int sections = 0;
  if (length) {
    BOOL is_named = FALSE;
    for (size_t k = 0; k < OA_ARRAY_SIZE(fm_named); k++) {
      if (oa_wcslen(fm_named[k].name) == length && CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, format, (int)length, fm_named[k].name, -1) == CSTR_EQUAL) {
        starts[0] = 0; lengths[0] = (unsigned)length; kinds[0] = fm_named[k].type; named[0] = fm_named[k].id;
        sections = 1;
        is_named = TRUE;
        break;
      }
    }
    if (!is_named) {
      size_t begin = 0;
      for (size_t i = 0; i <= length; i++) {
        if (i < length && (format[i] == '"' || format[i] == '\\')) { size_t start, n; i = fm_literal_at(format, length, i, &start, &n) - 1; continue; }
        if (i < length && (format[i] == '*' || format[i] == '_')) { i++; continue; }
        if (i == length || format[i] == ';') {
          if (sections == FM_MAX_SECTIONS) return DISP_E_TYPEMISMATCH;
          starts[sections] = (unsigned)begin; lengths[sections] = (unsigned)(i - begin);
          kinds[sections] = (unsigned)fm_section_kind(format + begin, i - begin);
          named[sections] = 0;
          sections++;
          begin = i + 1;
        }
      }
    }
  }
  const int needed = FM_HEADER_SIZE + sections * FM_SECTION_SIZE;
  if (actual) *actual = needed;
  if (token_size < needed) return DISP_E_BUFFERTOOSMALL;
  tokens[0] = (BYTE)needed;
  tokens[1] = (BYTE)sections;
  tokens[2] = (BYTE)first_day;
  tokens[3] = (BYTE)first_week;
  for (int k = 0; k < sections; k++) {
    BYTE *section = tokens + FM_HEADER_SIZE + k * FM_SECTION_SIZE;
    section[0] = (BYTE)kinds[k];
    section[1] = (BYTE)named[k];
    section[2] = (BYTE)(starts[k] & 0xff); section[3] = (BYTE)(starts[k] >> 8);
    section[4] = (BYTE)(lengths[k] & 0xff); section[5] = (BYTE)(lengths[k] >> 8);
  }
  return S_OK;
}

/* ---- formatting a VARIANT --------------------------------------------------------------------------------------------------------------------------- */

typedef struct { int kind, named; size_t start, length; } FmSection;

static BOOL fm_read_tokens(const BYTE *tokens, size_t format_length, int *first_day, int *first_week, FmSection *sections, int *count) {
  if (tokens[0] < FM_HEADER_SIZE || tokens[1] > FM_MAX_SECTIONS || tokens[0] != FM_HEADER_SIZE + tokens[1] * FM_SECTION_SIZE) return FALSE;
  *count = tokens[1];
  *first_day = tokens[2];
  *first_week = tokens[3];
  for (int k = 0; k < *count; k++) {
    const BYTE *section = tokens + FM_HEADER_SIZE + k * FM_SECTION_SIZE;
    sections[k].kind = section[0];
    sections[k].named = section[1];
    sections[k].start = (size_t)section[2] | ((size_t)section[3] << 8);
    sections[k].length = (size_t)section[4] | ((size_t)section[5] << 8);
    if (sections[k].start > format_length || sections[k].length > format_length - sections[k].start) return FALSE;
  }
  return TRUE;
}

/* Does a date text parse of the string work? A string is a number if it converts to one, else a date if it converts to one. */
static HRESULT fm_string_to_date(VARIANT *v, LCID lcid, ULONG flags, double *date) {
  VARIANT t;
  VariantInit(&t);
  if (SUCCEEDED(VariantChangeTypeEx(&t, v, lcid, 0, VT_R8))) { *date = V_R8(&t); return S_OK; }
  return VarDateFromStr(V_BSTR(v), lcid, flags & LOCALE_NOUSEROVERRIDE, date);
}

/* Formats the value of `v` (not a string) or the number `x` with one numeric/date/string section. */
static HRESULT fm_run_section(FmOut *o, const FmSection *sec, const WCHAR *format, VARIANT *v, BOOL is_text, double number, const FmNum *digits, BOOL show_minus, const FmLocale *loc) {
  const WCHAR *s = format + sec->start;
  size_t len = sec->length;
  WCHAR synth[48];
  int kind = sec->kind;
  if (sec->named) {
    switch (sec->named) {
      case FM_N_YES_NO: case FM_N_TRUE_FALSE: case FM_N_ON_OFF: {
        const BOOL on = digits->n != 0;
        fm_put_z(o, sec->named == FM_N_YES_NO ? (on ? L"Yes" : L"No") : sec->named == FM_N_TRUE_FALSE ? (on ? L"True" : L"False") : (on ? L"On" : L"Off"));
        return o->failed ? E_OUTOFMEMORY : S_OK;
      }
      case FM_N_FIXED: case FM_N_STANDARD: {
        int n = 0;
        const DWORD places = dt_locale_number(loc->lcid, loc->flags, LOCALE_IDIGITS, 2);
        if (sec->named == FM_N_STANDARD) { synth[n++] = '#'; synth[n++] = ','; synth[n++] = '#'; synth[n++] = '#'; }
        synth[n++] = '0';
        if (places) { synth[n++] = '.'; for (DWORD k = 0; k < places && k < 9; k++) synth[n++] = '0'; }
        s = synth; len = (size_t)n;
        break;
      }
      case FM_N_PERCENT: s = L"0.00%"; len = 5; break;
      case FM_N_SCIENTIFIC: s = L"0.00E+00"; len = 8; break;
      case FM_N_GENERAL_DATE: s = L"c"; len = 1; break;
      case FM_N_LONG_DATE: s = L"dddddd"; len = 6; break;
      case FM_N_MEDIUM_DATE: s = L"dd-mmm-yy"; len = 9; break;
      case FM_N_SHORT_DATE: s = L"ddddd"; len = 5; break;
      case FM_N_LONG_TIME: s = L"ttttt"; len = 5; break;
      case FM_N_MEDIUM_TIME: s = L"hh:mm AMPM"; len = 10; break;
      case FM_N_SHORT_TIME: s = L"hh:mm"; len = 5; break;
      default: break;
    }
  }
  if (sec->named == FM_N_CURRENCY) {
    BSTR text;
    HRESULT hr = fm_format_locale_number(*digits, -1, -2, -2, -2, loc->flags, FALSE, TRUE, &text);
    if (FAILED(hr)) return hr;
    fm_puts(o, text, SysStringLen(text));
    SysFreeString(text);
    return o->failed ? E_OUTOFMEMORY : S_OK;
  }
  if (kind == FM_T_DATE) return fm_emit_date(o, s, len, number, loc);
  if (kind == FM_T_STRING || is_text) {
    BSTR text = NULL;
    HRESULT hr = fm_general(v, loc->lcid, &text);
    if (FAILED(hr)) return hr;
    fm_emit_string(o, s, len, text, SysStringLen(text));
    SysFreeString(text);
    return o->failed ? E_OUTOFMEMORY : S_OK;
  }
  fm_emit_number(o, digits, show_minus, s, len, loc);
  return o->failed ? E_OUTOFMEMORY : S_OK;
}

EXPORT HRESULT WINAPI VarFormatFromTokens(LPVARIANT var, LPOLESTR format, LPBYTE tokens, ULONG flags, BSTR *out, LCID lcid) {
  if (!var || !out || !tokens) return E_INVALIDARG;
  *out = NULL;
  const size_t format_length = format ? oa_wcslen(format) : 0;
  FmSection sections[FM_MAX_SECTIONS];
  int count, first_day, first_week;
  if (!fm_read_tokens(tokens, format_length, &first_day, &first_week, sections, &count)) return E_INVALIDARG;
  if (count > 0 && !format) return E_INVALIDARG;

  VARIANT v;
  VariantInit(&v);
  HRESULT hr = VariantCopyInd(&v, var);
  if (FAILED(hr)) return hr;
  if (V_VT(&v) == VT_NULL) { VariantClear(&v); return S_OK; }
  FmLocale loc;
  fm_load_locale(&loc, lcid, flags, first_day, first_week);

  if (count == 0 || sections[0].named == FM_N_GENERAL_NUMBER) {
    hr = fm_general(&v, lcid, out);
    VariantClear(&v);
    return hr;
  }

  FmOut o = { 0 };
  double number = 0.0;
  FmNum digits;
  memset(&digits, 0, sizeof(digits));
  const BOOL is_bstr = V_VT(&v) == VT_BSTR;
  BOOL is_text = FALSE, have_number = FALSE;
  int string_section = -1;
  for (int k = 0; k < count; k++) if (sections[k].kind == FM_T_STRING) { string_section = k; break; }
  if (is_bstr) {
    if (sections[0].kind == FM_T_STRING) is_text = TRUE;
    else {
      hr = fm_variant_number(&v, lcid, &number, &digits);
      if (SUCCEEDED(hr)) have_number = TRUE;
      else if (sections[0].kind == FM_T_DATE && SUCCEEDED(fm_string_to_date(&v, lcid, flags, &number)) && fm_from_double(&digits, number, 15)) { have_number = TRUE; hr = S_OK; }
      else is_text = TRUE;
    }
  } else {
    hr = fm_variant_number(&v, lcid, &number, &digits);
    if (FAILED(hr)) { VariantClear(&v); return hr; }
    have_number = TRUE;
  }

  int index = 0;
  BOOL show_minus = FALSE;
  if (have_number) {
    const BOOL negative = digits.neg && digits.n;
    if (count == 1) { index = 0; show_minus = TRUE; }
    else if (negative) index = 1;
    else if (!digits.n && count >= 3) index = 2;
    FmNum magnitude = digits;
    if (count > 1 && negative) { magnitude.neg = FALSE; digits = magnitude; if (number < 0) number = -number; }
  } else if (string_section >= 0) index = string_section;
  else if (count >= 4) index = 3;
  else index = 0;
  /* a string value in a section made for numbers or dates is copied as it is */
  if (is_text && sections[index].kind != FM_T_STRING && sections[index].kind != FM_T_UNKNOWN) {
    VariantClear(&v);
    hr = fm_general(var, lcid, out);
    return hr;
  }
  if (is_text && sections[index].kind == FM_T_UNKNOWN) {
    const WCHAR *s = format + sections[index].start;
    FmOut lit = { 0 };
    for (size_t i = 0; i < sections[index].length;) {
      size_t start, length;
      if (s[i] == '"' || s[i] == '\\') { i = fm_literal_at(s, sections[index].length, i, &start, &length); fm_puts(&lit, s + start, length); }
      else { fm_put(&lit, s[i]); i++; }
    }
    VariantClear(&v);
    return fm_finish(&lit, out);
  }
  hr = fm_run_section(&o, &sections[index], format, &v, is_text, number, &digits, show_minus, &loc);
  VariantClear(&v);
  if (FAILED(hr)) { fm_free(&o); return hr; }
  return fm_finish(&o, out);
}

EXPORT HRESULT WINAPI VarFormat(LPVARIANT var, LPOLESTR format, int first_day, int first_week, ULONG flags, BSTR *out) {
  BYTE tokens[256];
  if (!out) return E_INVALIDARG;
  *out = NULL;
  HRESULT hr = VarTokenizeFormatString(format, tokens, sizeof(tokens), first_day, first_week, LOCALE_USER_DEFAULT, NULL);
  if (SUCCEEDED(hr)) hr = VarFormatFromTokens(var, format, tokens, flags, out, LOCALE_USER_DEFAULT);
  return hr;
}

EXPORT HRESULT WINAPI VarFormatDateTime(LPVARIANT var, int named_format, ULONG flags, BSTR *out) {
  static const WCHAR *const names[5] = { L"General Date", L"Long Date", L"Short Date", L"Long Time", L"Short Time" };
  if (!var || !out) return E_INVALIDARG;
  if (named_format < 0 || named_format > 4) return E_INVALIDARG;
  return VarFormat(var, (LPOLESTR)names[named_format], 0, 0, flags, out);
}

/* The number of a VARIANT for the number-formatting calls. */
static HRESULT fm_number_argument(LPVARIANT var, FmNum *x) {
  VARIANT v;
  double d;
  VariantInit(&v);
  HRESULT hr = VariantCopyInd(&v, var);
  if (FAILED(hr)) return hr;
  hr = fm_variant_number(&v, LOCALE_USER_DEFAULT, &d, x);
  VariantClear(&v);
  return hr;
}

EXPORT HRESULT WINAPI VarFormatNumber(LPVARIANT var, int digits, int leading, int parens, int group, ULONG flags, BSTR *out) {
  FmNum x;
  if (!var || !out || digits > 9) return E_INVALIDARG;
  *out = NULL;
  HRESULT hr = fm_number_argument(var, &x);
  if (FAILED(hr)) return hr;
  return fm_format_locale_number(x, digits, leading, parens, group, flags, FALSE, FALSE, out);
}
EXPORT HRESULT WINAPI VarFormatPercent(LPVARIANT var, int digits, int leading, int parens, int group, ULONG flags, BSTR *out) {
  FmNum x;
  if (!var || !out || digits > 9) return E_INVALIDARG;
  *out = NULL;
  HRESULT hr = fm_number_argument(var, &x);
  if (FAILED(hr)) return hr;
  return fm_format_locale_number(x, digits, leading, parens, group, flags, TRUE, FALSE, out);
}
EXPORT HRESULT WINAPI VarFormatCurrency(LPVARIANT var, int digits, int leading, int parens, int group, ULONG flags, BSTR *out) {
  FmNum x;
  if (!var || !out || digits > 9) return E_INVALIDARG;
  *out = NULL;
  HRESULT hr = fm_number_argument(var, &x);
  if (FAILED(hr)) return hr;
  return fm_format_locale_number(x, digits, leading, parens, group, flags, FALSE, TRUE, out);
}

EXPORT HRESULT WINAPI VarMonthName(INT month, INT abbreviate, ULONG flags, BSTR *out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (month < 1 || month > 12) return E_INVALIDARG;
  if (flags & VAR_CALENDAR_HIJRI) {
    *out = SysAllocString(dt_hijri_months[month - 1]);
    return *out ? S_OK : E_OUTOFMEMORY;
  }
  WCHAR name[80];
  if (GetLocaleInfoW(LOCALE_USER_DEFAULT, (abbreviate ? LOCALE_SABBREVMONTHNAME1 : LOCALE_SMONTHNAME1) + (month - 1), name, 80) <= 0) return E_INVALIDARG;
  *out = SysAllocString(name);
  return *out ? S_OK : E_OUTOFMEMORY;
}

EXPORT HRESULT WINAPI VarWeekdayName(INT weekday, INT abbreviate, INT first_day, ULONG flags, BSTR *out) {
  (void)flags;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (weekday < 1 || weekday > 7 || first_day < 0 || first_day > 7) return E_INVALIDARG;
  int first = first_day - 2;                 /* 0 = Monday */
  if (first_day == 0) first = (int)dt_locale_number(LOCALE_USER_DEFAULT, 0, LOCALE_IFIRSTDAYOFWEEK, 0);
  const int day = (7 + weekday - 1 + first) % 7;
  WCHAR name[80];
  if (GetLocaleInfoW(LOCALE_USER_DEFAULT, (abbreviate ? LOCALE_SABBREVDAYNAME1 : LOCALE_SDAYNAME1) + day, name, 80) <= 0) return E_INVALIDARG;
  *out = SysAllocString(name);
  return *out ? S_OK : E_OUTOFMEMORY;
}
