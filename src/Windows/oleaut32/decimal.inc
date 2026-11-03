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

/* DECIMAL (96-bit integer, scale 0..28, sign) and CURRENCY (64-bit integer scaled by 10000): conversions, arithmetic, rounding and the double helpers VarR8Round,
 * VarR4CmpR8 and VarR8Pow. The arithmetic is exact: it runs on a small fixed-size big integer (320 bits) and rounds half to even where digits are dropped. */

/* ---- a 320-bit unsigned integer, little-endian words ---------------------------------------------------------------------------------------------------- */
#define DB_WORDS 10
typedef struct { unsigned int w[DB_WORDS]; } DB;

static void db_zero(DB *a) { for (unsigned i = 0; i < DB_WORDS; i++) a->w[i] = 0; }
static void db_set_u64(DB *a, unsigned long long v) { db_zero(a); a->w[0] = (unsigned int)v; a->w[1] = (unsigned int)(v >> 32); }
static int db_is_zero(const DB *a) { for (unsigned i = 0; i < DB_WORDS; i++) if (a->w[i]) return 0; return 1; }
static int db_cmp(const DB *a, const DB *b) {
  for (unsigned i = DB_WORDS; i-- > 0;) if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
  return 0;
}
/* a = a * m; returns 1 if the product did not fit */
static int db_mul_u32(DB *a, unsigned int m) {
  unsigned long long carry = 0;
  for (unsigned i = 0; i < DB_WORDS; i++) { unsigned long long t = (unsigned long long)a->w[i] * m + carry; a->w[i] = (unsigned int)t; carry = t >> 32; }
  return carry != 0;
}
/* a = a / d; returns the remainder */
static unsigned int db_divmod_u32(DB *a, unsigned int d) {
  unsigned long long rem = 0;
  for (unsigned i = DB_WORDS; i-- > 0;) { unsigned long long t = (rem << 32) | a->w[i]; a->w[i] = (unsigned int)(t / d); rem = t % d; }
  return (unsigned int)rem;
}
static int db_add(DB *a, const DB *b) {
  unsigned long long carry = 0;
  for (unsigned i = 0; i < DB_WORDS; i++) { unsigned long long t = (unsigned long long)a->w[i] + b->w[i] + carry; a->w[i] = (unsigned int)t; carry = t >> 32; }
  return carry != 0;
}
/* a = a - b (a >= b) */
static void db_sub(DB *a, const DB *b) {
  unsigned long long borrow = 0;
  for (unsigned i = 0; i < DB_WORDS; i++) {
    unsigned long long t = (unsigned long long)a->w[i] - b->w[i] - borrow;
    a->w[i] = (unsigned int)t;
    borrow = (t >> 32) & 1;
  }
}
static int db_add_u32(DB *a, unsigned int v) { DB b; db_zero(&b); b.w[0] = v; return db_add(a, &b); }
static void db_shl1(DB *a) { unsigned int carry = 0; for (unsigned i = 0; i < DB_WORDS; i++) { unsigned int next = a->w[i] >> 31; a->w[i] = (a->w[i] << 1) | carry; carry = next; } }
static unsigned db_bitlen(const DB *a) {
  for (unsigned i = DB_WORDS; i-- > 0;) if (a->w[i]) { unsigned n = 32; unsigned int v = a->w[i]; while (!(v & 0x80000000u)) { v <<= 1; n--; } return i * 32 + n; }
  return 0;
}
/* r = a * b; returns 1 on overflow */
static int db_mul(DB *r, const DB *a, const DB *b) {
  DB out;
  db_zero(&out);
  int overflow = 0;
  for (unsigned i = 0; i < DB_WORDS; i++) {
    unsigned long long carry = 0;
    for (unsigned j = 0; j < DB_WORDS; j++) {
      unsigned long long t = (unsigned long long)a->w[i] * b->w[j];
      if (i + j >= DB_WORDS) { if (t || carry) overflow = 1; continue; }
      t += out.w[i + j] + carry;
      out.w[i + j] = (unsigned int)t;
      carry = t >> 32;
    }
    if (carry) overflow = 1;
  }
  *r = out;
  return overflow;
}
/* q = a / b, r = a % b (b != 0) */
static void db_divmod(const DB *a, const DB *b, DB *q, DB *r) {
  DB quot, rem;
  db_zero(&quot); db_zero(&rem);
  for (unsigned i = db_bitlen(a); i-- > 0;) {
    db_shl1(&rem);
    rem.w[0] |= (a->w[i / 32] >> (i % 32)) & 1;
    if (db_cmp(&rem, b) >= 0) { db_sub(&rem, b); quot.w[i / 32] |= 1u << (i % 32); }
  }
  if (q) *q = quot;
  if (r) *r = rem;
}
/* a = a * 10^n; returns 1 on overflow */
static int db_mul_pow10(DB *a, unsigned n) {
  while (n >= 9) { if (db_mul_u32(a, 1000000000u)) return 1; n -= 9; }
  static const unsigned int small[9] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000 };
  return n ? db_mul_u32(a, small[n]) : 0;
}
/* a = a / 10^n, rounded to nearest, ties to even (n <= 90) */
static void db_div_pow10_round(DB *a, unsigned n) {
  if (!n) return;
  DB p, q, r;
  db_set_u64(&p, 1);
  db_mul_pow10(&p, n);
  db_divmod(a, &p, &q, &r);
  db_shl1(&r);
  const int c = db_cmp(&r, &p);
  if (c > 0 || (c == 0 && (q.w[0] & 1))) db_add_u32(&q, 1);
  *a = q;
}
static int db_fits96(const DB *a) { for (unsigned i = 3; i < DB_WORDS; i++) if (a->w[i]) return 0; return 1; }
static int db_fits64(const DB *a) { for (unsigned i = 2; i < DB_WORDS; i++) if (a->w[i]) return 0; return 1; }
static unsigned long long db_u64(const DB *a) { return ((unsigned long long)a->w[1] << 32) | a->w[0]; }

/* ---- DECIMAL <-> DB ------------------------------------------------------------------------------------------------------------------------------------ */
/* a decimal result is stored without touching wReserved (it overlays the type of a VARIANT that the caller has set up) */
static void dec_copy(DECIMAL *dst, const DECIMAL *src) { const USHORT reserved = dst->wReserved; *dst = *src; dst->wReserved = reserved; }
static int dec_valid(const DECIMAL *d) { return d->scale <= 28 && !(d->sign & 0x7f); }
static void dec_unpack(const DECIMAL *d, DB *mag) { db_zero(mag); mag->w[0] = d->Lo32; mag->w[1] = d->Mid32; mag->w[2] = d->Hi32; }

/* Stores mag * 10^-scale: when the magnitude does not fit 96 bits or the scale is beyond 28, digits are dropped (rounded) as little as possible. */
static HRESULT dec_pack(DECIMAL *out, const DB *mag, unsigned scale, int neg) {
  for (unsigned n = scale > 28 ? scale - 28 : 0;; n++) {
    DB t = *mag;
    db_div_pow10_round(&t, n);
    if (db_fits96(&t)) {
      out->scale = (BYTE)(scale - n);
      out->sign = neg ? DECIMAL_NEG : 0;
      out->Hi32 = t.w[2];
      out->Lo32 = t.w[0];
      out->Mid32 = t.w[1];
      return S_OK;
    }
    if (n >= scale) return DISP_E_OVERFLOW;
  }
}
static void dec_set_u64(DECIMAL *out, unsigned long long v, int neg) {
  out->scale = 0; out->sign = neg ? DECIMAL_NEG : 0; out->Hi32 = 0; out->Lo64 = v;
}

/* Integer value of a decimal, rounded half to even: the magnitude (fits 64 bits or HRESULT overflow) and the sign. */
static HRESULT dec_round_u64(const DECIMAL *d, unsigned long long *mag, int *neg) {
  if (!dec_valid(d)) return E_INVALIDARG;
  DB m;
  dec_unpack(d, &m);
  db_div_pow10_round(&m, d->scale);
  if (!db_fits64(&m)) return DISP_E_OVERFLOW;
  *mag = db_u64(&m);
  *neg = (d->sign & DECIMAL_NEG) != 0;
  return S_OK;
}
static HRESULT dec_to_i64(const DECIMAL *d, long long *out) {
  unsigned long long m;
  int neg;
  HRESULT hr = dec_round_u64(d, &m, &neg);
  if (FAILED(hr)) return hr;
  if (neg) { if (m > 0x8000000000000000ull) return DISP_E_OVERFLOW; *out = (long long)(0ull - m); }
  else { if (m > 0x7fffffffffffffffull) return DISP_E_OVERFLOW; *out = (long long)m; }
  return S_OK;
}
static HRESULT dec_to_u64(const DECIMAL *d, unsigned long long *out) {
  unsigned long long m;
  int neg;
  HRESULT hr = dec_round_u64(d, &m, &neg);
  if (FAILED(hr)) return hr;
  if (neg && m) return DISP_E_OVERFLOW;
  *out = m;
  return S_OK;
}

static const double dec_pow10_table[29] = {
  1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22, 1e23, 1e24, 1e25, 1e26, 1e27, 1e28
};
static HRESULT dec_to_r8(const DECIMAL *d, double *out) {
  if (!dec_valid(d)) return E_INVALIDARG;
  double v = (double)d->Lo64 + (double)d->Hi32 * 18446744073709551616.0;
  v /= dec_pow10_table[d->scale];
  *out = (d->sign & DECIMAL_NEG) ? -v : v;
  return S_OK;
}

/* ---- double -> decimal digits (exact, so the rounding is exactly that of printf("%.*g")) ------------------------------------------------------------------ */
/* Writes the first `sig` significant decimal digits of x (finite, > 0), rounded half to even on the exact binary value, to digits[]; the value is about
 * int(digits) * 10^*q. Returns the number of digits. `digits` holds at least 800 characters. */
static unsigned dbl_digits(double x, unsigned sig, char *digits, int *q) {
  unsigned long long bits;
  memcpy(&bits, &x, sizeof(bits));
  const int bexp = (int)((bits >> 52) & 0x7ff);
  unsigned long long m = bits & 0xfffffffffffffull;
  int e;
  if (bexp) { m |= 1ull << 52; e = bexp - 1075; } else e = -1074;
  unsigned int n[96];
  unsigned len = 0;
  for (unsigned i = 0; i < 96; i++) n[i] = 0;
  n[0] = (unsigned int)m; n[1] = (unsigned int)(m >> 32);
  len = n[1] ? 2 : 1;
  int q0 = 0;
  if (e >= 0) {                 /* m << e */
    unsigned words = (unsigned)e / 32, bitsh = (unsigned)e % 32;
    for (unsigned i = len; i-- > 0;) {
      unsigned long long v = (unsigned long long)n[i] << bitsh;
      n[i] = 0;
      n[i + words] |= (unsigned int)v;
      n[i + words + 1] |= (unsigned int)(v >> 32);
    }
    len += words + 1;
  } else {                      /* m * 5^-e, value = that * 10^e */
    unsigned k = (unsigned)-e;
    while (k) {
      unsigned int mult = 5;
      unsigned step = 1;
      if (k >= 13) { mult = 1220703125u; step = 13; } else { for (unsigned i = 1; i < k; i++) mult *= 5; step = k; }
      unsigned long long carry = 0;
      for (unsigned i = 0; i < len; i++) { unsigned long long t = (unsigned long long)n[i] * mult + carry; n[i] = (unsigned int)t; carry = t >> 32; }
      if (carry) n[len++] = (unsigned int)carry;
      k -= step;
    }
    q0 = e;
  }
  while (len > 1 && !n[len - 1]) len--;
  /* decimal digits, least significant first */
  char rev[820];
  unsigned count = 0;
  while (len > 1 || n[0]) {
    unsigned long long rem = 0;
    for (unsigned i = len; i-- > 0;) { unsigned long long t = (rem << 32) | n[i]; n[i] = (unsigned int)(t / 1000000000u); rem = t % 1000000000u; }
    while (len > 1 && !n[len - 1]) len--;
    const int more = len > 1 || n[0];
    for (unsigned i = 0; i < 9 && (more || rem); i++) { rev[count++] = (char)('0' + rem % 10); rem /= 10; }
    if (count > 810) break;
  }
  if (!count) { digits[0] = '0'; *q = 0; return 1; }
  unsigned total = count;
  unsigned keep = total < sig ? total : sig;
  for (unsigned i = 0; i < keep; i++) digits[i] = rev[total - 1 - i];
  *q = q0 + (int)(total - keep);
  if (total > keep) {
    const int d = rev[total - 1 - keep] - '0';
    int sticky = 0;
    for (unsigned i = 0; i + 1 + keep < total; i++) if (rev[i] != '0') { sticky = 1; break; }
    if (d > 5 || (d == 5 && (sticky || ((digits[keep - 1] - '0') & 1)))) {
      unsigned i = keep;
      while (i-- > 0) { if (digits[i] == '9') digits[i] = '0'; else { digits[i]++; break; } }
      if (i == (unsigned)-1) { digits[0] = '1'; for (unsigned j = 1; j < keep; j++) digits[j] = '0'; *q += 1; }
    }
  }
  return keep;
}

/* A decimal from `sig` significant digits of a double (what Windows gets by formatting the number with that many digits and parsing it back). */
static HRESULT dec_from_double(double x, unsigned sig, DECIMAL *out) {
  if (x != x) return DISP_E_OVERFLOW;
  const int neg = x < 0;
  if (neg) x = -x;
  if (x >= 79228162514264337593543950336.0) return DISP_E_OVERFLOW;
  if (x == 0.0) { dec_set_u64(out, 0, 0); return S_OK; }
  char digits[820];
  int q;
  unsigned nd = dbl_digits(x, sig, digits, &q);
  while (nd > 1 && digits[nd - 1] == '0') { nd--; q++; }
  DB mag;
  db_zero(&mag);
  for (unsigned i = 0; i < nd; i++) { db_mul_u32(&mag, 10); db_add_u32(&mag, (unsigned int)(digits[i] - '0')); }
  if (q >= 0) {
    if (nd + (unsigned)q > 29 || db_mul_pow10(&mag, (unsigned)q)) return DISP_E_OVERFLOW;
    return dec_pack(out, &mag, 0, neg);
  }
  unsigned scale = (unsigned)-q;
  if (scale > 28 && scale - 28 > nd) { dec_set_u64(out, 0, 0); return S_OK; }      /* far below the smallest decimal */
  return dec_pack(out, &mag, scale, neg);
}

/* ---- DECIMAL arithmetic ---------------------------------------------------------------------------------------------------------------------------------- */
EXPORT HRESULT WINAPI VarDecAbs(LPDECIMAL in, LPDECIMAL result) { DECIMAL d = *in; d.sign &= (BYTE)~DECIMAL_NEG; dec_copy(result, &d); return S_OK; }
EXPORT HRESULT WINAPI VarDecNeg(LPDECIMAL in, LPDECIMAL result) { DECIMAL d = *in; d.sign ^= DECIMAL_NEG; dec_copy(result, &d); return S_OK; }

/* the sum of two decimals given as magnitude and sign, at scale `scale` */
static HRESULT dec_add_signed(DECIMAL *out, DB a, int aneg, DB b, int bneg, unsigned scale) {
  int neg;
  if (aneg == bneg) { db_add(&a, &b); neg = aneg; }
  else if (db_cmp(&a, &b) >= 0) { db_sub(&a, &b); neg = aneg; }
  else { db_sub(&b, &a); a = b; neg = bneg; }
  if (db_is_zero(&a)) neg = 0;
  return dec_pack(out, &a, scale, neg);
}
static HRESULT dec_add_impl(const DECIMAL *l, const DECIMAL *r, int negate_right, DECIMAL *out) {
  if (!dec_valid(l) || !dec_valid(r)) return E_INVALIDARG;
  DB a, b;
  dec_unpack(l, &a);
  dec_unpack(r, &b);
  const unsigned scale = l->scale > r->scale ? l->scale : r->scale;
  db_mul_pow10(&a, scale - l->scale);
  db_mul_pow10(&b, scale - r->scale);
  return dec_add_signed(out, a, (l->sign & DECIMAL_NEG) != 0, b, ((r->sign & DECIMAL_NEG) != 0) != (negate_right != 0), scale);
}
EXPORT HRESULT WINAPI VarDecAdd(LPDECIMAL l, LPDECIMAL r, LPDECIMAL result) { DECIMAL out; HRESULT hr = dec_add_impl(l, r, 0, &out); if (SUCCEEDED(hr)) dec_copy(result, &out); return hr; }
EXPORT HRESULT WINAPI VarDecSub(LPDECIMAL l, LPDECIMAL r, LPDECIMAL result) { DECIMAL out; HRESULT hr = dec_add_impl(l, r, 1, &out); if (SUCCEEDED(hr)) dec_copy(result, &out); return hr; }

EXPORT HRESULT WINAPI VarDecMul(LPDECIMAL l, LPDECIMAL r, LPDECIMAL result) {
  if (!dec_valid(l) || !dec_valid(r)) return E_INVALIDARG;
  DB a, b, p;
  dec_unpack(l, &a);
  dec_unpack(r, &b);
  db_mul(&p, &a, &b);
  DECIMAL out;
  HRESULT hr = dec_pack(&out, &p, (unsigned)l->scale + r->scale, ((l->sign ^ r->sign) & DECIMAL_NEG) != 0);
  if (SUCCEEDED(hr)) dec_copy(result, &out);
  return hr;
}

EXPORT HRESULT WINAPI VarDecDiv(LPDECIMAL l, LPDECIMAL r, LPDECIMAL result) {
  if (!dec_valid(l) || !dec_valid(r)) return E_INVALIDARG;
  DB a, b, q, rem;
  dec_unpack(l, &a);
  dec_unpack(r, &b);
  if (db_is_zero(&b)) return DISP_E_DIVBYZERO;
  const int neg = ((l->sign ^ r->sign) & DECIMAL_NEG) != 0;
  const int sa = l->scale, sb = r->scale;
  DECIMAL out;
  HRESULT hr;
  db_divmod(&a, &b, &q, &rem);
  if (db_is_zero(&rem)) {                         /* exact: the quotient keeps the difference of the scales */
    if (sa >= sb) hr = dec_pack(&out, &q, (unsigned)(sa - sb), neg);
    else if (db_mul_pow10(&q, (unsigned)(sb - sa))) hr = DISP_E_OVERFLOW;
    else hr = dec_pack(&out, &q, 0, neg);
  } else {                                        /* as many fraction digits as fit 96 bits (at most 28) */
    hr = DISP_E_OVERFLOW;
    for (int s = 28; s >= 0; s--) {
      DB num = a, den = b;
      const int k = s + sb - sa;
      if (k >= 0) db_mul_pow10(&num, (unsigned)k); else db_mul_pow10(&den, (unsigned)-k);
      DB quo, re;
      db_divmod(&num, &den, &quo, &re);
      db_shl1(&re);
      const int c = db_cmp(&re, &den);
      if (c > 0 || (c == 0 && (quo.w[0] & 1))) db_add_u32(&quo, 1);
      if (!db_fits96(&quo)) continue;
      unsigned scale = (unsigned)s;
      while (scale > 0) {
        DB t = quo;
        if (db_divmod_u32(&t, 10) != 0) break;
        quo = t; scale--;
      }
      hr = dec_pack(&out, &quo, scale, neg);
      break;
    }
  }
  if (SUCCEEDED(hr)) dec_copy(result, &out);
  return hr;
}

EXPORT HRESULT WINAPI VarDecCmp(LPDECIMAL l, LPDECIMAL r) {
  if (!dec_valid(l) || !dec_valid(r)) return E_INVALIDARG;
  DB a, b;
  dec_unpack(l, &a);
  dec_unpack(r, &b);
  const int az = db_is_zero(&a), bz = db_is_zero(&b);
  const int an = !az && (l->sign & DECIMAL_NEG), bn = !bz && (r->sign & DECIMAL_NEG);
  if (az && bz) return VARCMP_EQ;
  if (an != bn) return an ? VARCMP_LT : VARCMP_GT;
  const unsigned scale = l->scale > r->scale ? l->scale : r->scale;
  db_mul_pow10(&a, scale - l->scale);
  db_mul_pow10(&b, scale - r->scale);
  int c = db_cmp(&a, &b);
  if (an) c = -c;
  return c < 0 ? VARCMP_LT : c > 0 ? VARCMP_GT : VARCMP_EQ;
}
EXPORT HRESULT WINAPI VarDecCmpR8(LPDECIMAL l, double r) {
  DECIMAL right;
  right.wReserved = 0;
  HRESULT hr = VarDecFromR8(r, &right);
  if (FAILED(hr)) return hr;
  return VarDecCmp(l, &right);
}

EXPORT HRESULT WINAPI VarDecRound(LPDECIMAL in, int decimals, LPDECIMAL result) {
  if (!dec_valid(in) || decimals < 0) return E_INVALIDARG;
  if (decimals >= in->scale) { dec_copy(result, in); return S_OK; }
  DB m;
  dec_unpack(in, &m);
  db_div_pow10_round(&m, (unsigned)(in->scale - decimals));
  return dec_pack(result, &m, (unsigned)decimals, (in->sign & DECIMAL_NEG) != 0);
}
/* the integer part, dropping the fraction (toward zero) or rounding down (toward minus infinity) */
static HRESULT dec_integer(const DECIMAL *in, int floor_negative, DECIMAL *result) {
  if (!dec_valid(in)) return E_INVALIDARG;
  if (!in->scale) { dec_copy(result, in); return S_OK; }
  DB m, p, q, r;
  dec_unpack(in, &m);
  db_set_u64(&p, 1);
  db_mul_pow10(&p, in->scale);
  db_divmod(&m, &p, &q, &r);
  const int neg = (in->sign & DECIMAL_NEG) != 0;
  if (neg && floor_negative && !db_is_zero(&r)) db_add_u32(&q, 1);
  return dec_pack(result, &q, 0, neg);
}
EXPORT HRESULT WINAPI VarDecFix(LPDECIMAL in, LPDECIMAL result) { DECIMAL out; HRESULT hr = dec_integer(in, 0, &out); if (SUCCEEDED(hr)) dec_copy(result, &out); return hr; }
EXPORT HRESULT WINAPI VarDecInt(LPDECIMAL in, LPDECIMAL result) { DECIMAL out; HRESULT hr = dec_integer(in, 1, &out); if (SUCCEEDED(hr)) dec_copy(result, &out); return hr; }

/* ---- conversions to a decimal ----------------------------------------------------------------------------------------------------------------------------- */
EXPORT HRESULT WINAPI VarDecFromI1(CHAR v, DECIMAL *out) { dec_set_u64(out, v < 0 ? 0ull - (unsigned long long)(long long)v : (unsigned long long)v, v < 0); return S_OK; }
EXPORT HRESULT WINAPI VarDecFromI2(SHORT v, DECIMAL *out) { dec_set_u64(out, v < 0 ? 0ull - (unsigned long long)(long long)v : (unsigned long long)v, v < 0); return S_OK; }
EXPORT HRESULT WINAPI VarDecFromI4(LONG v, DECIMAL *out) { dec_set_u64(out, v < 0 ? 0ull - (unsigned long long)(long long)v : (unsigned long long)v, v < 0); return S_OK; }
EXPORT HRESULT WINAPI VarDecFromI8(LONG64 v, DECIMAL *out) { dec_set_u64(out, v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v, v < 0); return S_OK; }
EXPORT HRESULT WINAPI VarDecFromUI1(BYTE v, DECIMAL *out) { dec_set_u64(out, v, 0); return S_OK; }
EXPORT HRESULT WINAPI VarDecFromUI2(USHORT v, DECIMAL *out) { dec_set_u64(out, v, 0); return S_OK; }
EXPORT HRESULT WINAPI VarDecFromUI4(ULONG v, DECIMAL *out) { dec_set_u64(out, v, 0); return S_OK; }
EXPORT HRESULT WINAPI VarDecFromUI8(ULONG64 v, DECIMAL *out) { dec_set_u64(out, v, 0); return S_OK; }
EXPORT HRESULT WINAPI VarDecFromBool(VARIANT_BOOL v, DECIMAL *out) { dec_set_u64(out, v ? 1 : 0, v != 0); return S_OK; }
EXPORT HRESULT WINAPI VarDecFromR4(FLOAT v, DECIMAL *out) { DECIMAL d; HRESULT hr = dec_from_double((double)v, 7, &d); if (SUCCEEDED(hr)) dec_copy(out, &d); return hr; }
EXPORT HRESULT WINAPI VarDecFromR8(DOUBLE v, DECIMAL *out) { DECIMAL d; HRESULT hr = dec_from_double(v, 15, &d); if (SUCCEEDED(hr)) dec_copy(out, &d); return hr; }
EXPORT HRESULT WINAPI VarDecFromDate(DATE v, DECIMAL *out) { return VarDecFromR8(v, out); }
EXPORT HRESULT WINAPI VarDecFromCy(CY v, DECIMAL *out) {
  const int neg = v.int64 < 0;
  out->scale = 4;
  out->sign = neg ? DECIMAL_NEG : 0;
  out->Hi32 = 0;
  out->Lo64 = neg ? 0ull - (unsigned long long)v.int64 : (unsigned long long)v.int64;
  return S_OK;
}

/* The default value of an object, converted: what the Var*FromDisp functions do. */
static const GUID oa_null_iid = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };
static HRESULT oa_value_from_disp(IDispatch *disp, LCID lcid, VARTYPE vt, VARIANT *dest) {
  if (!disp) return DISP_E_BADVARTYPE;
  DISPPARAMS params = { NULL, NULL, 0, 0 };
  VARIANT src;
  VariantInit(&src);
  HRESULT hr = IDispatch_Invoke(disp, DISPID_VALUE, &oa_null_iid, lcid, DISPATCH_PROPERTYGET, &params, &src, NULL, NULL);
  if (hr != S_OK) return DISP_E_TYPEMISMATCH;
  VariantInit(dest);
  hr = VariantChangeTypeEx(dest, &src, lcid, 0, vt);
  VariantClear(&src);
  return hr;
}
EXPORT HRESULT WINAPI VarDecFromDisp(IDispatch *disp, LCID lcid, DECIMAL *out) {
  VARIANT v;
  HRESULT hr = oa_value_from_disp(disp, lcid, VT_DECIMAL, &v);
  if (hr == S_OK) { dec_copy(out, &V_DECIMAL(&v)); }
  return hr;
}
EXPORT HRESULT WINAPI VarCyFromDisp(IDispatch *disp, LCID lcid, CY *out) {
  VARIANT v;
  HRESULT hr = oa_value_from_disp(disp, lcid, VT_CY, &v);
  if (hr == S_OK) { *out = V_CY(&v); }
  return hr;
}

/* A number parsed by the shared parser, as the types in `bits` (VTBIT_*). */
static HRESULT oa_number_from_str(LPCOLESTR str, LCID lcid, ULONG flags, ULONG bits, ULONG parse_flags, VARIANT *out) {
  if (!str) return DISP_E_TYPEMISMATCH;
  BYTE digits[256];
  NUMPARSE np;
  np.cDig = sizeof(digits);
  np.dwInFlags = parse_flags;
  HRESULT hr = VarParseNumFromStr(str, lcid, flags & (LOCALE_NOUSEROVERRIDE | VARIANT_NOUSEROVERRIDE | VARIANT_USE_NLS), &np, digits);
  if (FAILED(hr)) return hr;
  VariantInit(out);
  return VarNumFromParseNum(&np, digits, bits, out);
}
EXPORT HRESULT WINAPI VarDecFromStr(LPCOLESTR str, LCID lcid, ULONG flags, DECIMAL *out) {
  VARIANT v;
  HRESULT hr = oa_number_from_str(str, lcid, flags, VTBIT_DECIMAL, NUMPRS_STD, &v);
  if (SUCCEEDED(hr)) {
    if (V_VT(&v) == VT_DECIMAL) dec_copy(out, &V_DECIMAL(&v));
    else hr = DISP_E_TYPEMISMATCH;
  }
  return hr;
}

/* ---- conversions from a decimal ---------------------------------------------------------------------------------------------------------------------------- */
EXPORT HRESULT WINAPI VarI8FromDec(DECIMAL *d, LONG64 *out) { long long v; HRESULT hr = dec_to_i64(d, &v); if (SUCCEEDED(hr)) *out = v; return hr; }
EXPORT HRESULT WINAPI VarUI8FromDec(DECIMAL *d, ULONG64 *out) { unsigned long long v; HRESULT hr = dec_to_u64(d, &v); if (SUCCEEDED(hr)) *out = v; return hr; }
static HRESULT dec_to_signed(DECIMAL *d, long long lo, long long hi, long long *out) {
  long long v;
  HRESULT hr = dec_to_i64(d, &v);
  if (FAILED(hr)) return hr;
  if (v < lo || v > hi) return DISP_E_OVERFLOW;
  *out = v;
  return S_OK;
}
static HRESULT dec_to_unsigned(DECIMAL *d, unsigned long long hi, unsigned long long *out) {
  unsigned long long v;
  HRESULT hr = dec_to_u64(d, &v);
  if (FAILED(hr)) return hr;
  if (v > hi) return DISP_E_OVERFLOW;
  *out = v;
  return S_OK;
}
EXPORT HRESULT WINAPI VarI1FromDec(DECIMAL *d, CHAR *out) { long long v; HRESULT hr = dec_to_signed(d, -128, 127, &v); if (SUCCEEDED(hr)) *out = (CHAR)v; return hr; }
EXPORT HRESULT WINAPI VarI2FromDec(DECIMAL *d, SHORT *out) { long long v; HRESULT hr = dec_to_signed(d, -32768, 32767, &v); if (SUCCEEDED(hr)) *out = (SHORT)v; return hr; }
EXPORT HRESULT WINAPI VarI4FromDec(DECIMAL *d, LONG *out) { long long v; HRESULT hr = dec_to_signed(d, -2147483647LL - 1, 2147483647LL, &v); if (SUCCEEDED(hr)) *out = (LONG)v; return hr; }
EXPORT HRESULT WINAPI VarUI1FromDec(DECIMAL *d, BYTE *out) { unsigned long long v; HRESULT hr = dec_to_unsigned(d, 255, &v); if (SUCCEEDED(hr)) *out = (BYTE)v; return hr; }
EXPORT HRESULT WINAPI VarUI2FromDec(DECIMAL *d, USHORT *out) { unsigned long long v; HRESULT hr = dec_to_unsigned(d, 65535, &v); if (SUCCEEDED(hr)) *out = (USHORT)v; return hr; }
EXPORT HRESULT WINAPI VarUI4FromDec(DECIMAL *d, ULONG *out) { unsigned long long v; HRESULT hr = dec_to_unsigned(d, 0xffffffffull, &v); if (SUCCEEDED(hr)) *out = (ULONG)v; return hr; }
EXPORT HRESULT WINAPI VarR8FromDec(DECIMAL *d, DOUBLE *out) { double v; HRESULT hr = dec_to_r8(d, &v); if (SUCCEEDED(hr)) *out = v; return hr; }
EXPORT HRESULT WINAPI VarR4FromDec(DECIMAL *d, FLOAT *out) { double v; HRESULT hr = dec_to_r8(d, &v); if (SUCCEEDED(hr)) *out = (FLOAT)v; return hr; }
EXPORT HRESULT WINAPI VarDateFromDec(DECIMAL *d, DATE *out) { double v; HRESULT hr = dec_to_r8(d, &v); if (SUCCEEDED(hr)) *out = v; return hr; }
EXPORT HRESULT WINAPI VarBoolFromDec(DECIMAL *d, VARIANT_BOOL *out) {
  if (!dec_valid(d)) return E_INVALIDARG;
  *out = (d->Hi32 || d->Mid32 || d->Lo32) ? VARIANT_TRUE : VARIANT_FALSE;
  return S_OK;
}

/* Digits and sign of a decimal as text: no grouping, no trailing zeros, the locale's decimal separator. */
static HRESULT dec_to_bstr(const DECIMAL *d, LCID lcid, ULONG flags, BSTR *out) {
  if (!out || !dec_valid(d)) return E_INVALIDARG;
  DB m;
  dec_unpack(d, &m);
  char rev[40];
  unsigned count = 0;
  do { rev[count++] = (char)('0' + db_divmod_u32(&m, 10)); } while (!db_is_zero(&m));
  unsigned scale = d->scale;
  if (count == 1 && rev[0] == '0') scale = 0;
  while (scale > 0 && rev[0] == '0' && count > 1) {          /* trailing zeros of the fraction */
    for (unsigned i = 1; i < count; i++) rev[i - 1] = rev[i];
    count--; scale--;
  }
  while (count <= scale) rev[count++] = '0';                  /* leading zeros of the fraction */
  WCHAR sep[8] = { '.', 0 };
  if (scale) {
    WCHAR tmp[8];
    if (GetLocaleInfoW(lcid, LOCALE_SDECIMAL | (flags & LOCALE_NOUSEROVERRIDE), tmp, 8) > 0 && tmp[0]) { for (unsigned i = 0; i < 8; i++) sep[i] = tmp[i]; }
  }
  WCHAR text[64];
  unsigned len = 0;
  const int zero = count == 1 && rev[0] == '0';
  if ((d->sign & DECIMAL_NEG) && !zero) text[len++] = '-';
  for (unsigned i = count; i-- > 0;) {
    text[len++] = (WCHAR)rev[i];
    if (i == scale && scale) { for (unsigned j = 0; sep[j] && len < 62; j++) text[len++] = sep[j]; }
  }
  text[len] = 0;
  *out = SysAllocStringLen(text, len);
  return *out ? S_OK : E_OUTOFMEMORY;
}
EXPORT HRESULT WINAPI VarBstrFromDec(DECIMAL *d, LCID lcid, ULONG flags, BSTR *out) { return dec_to_bstr(d, lcid, flags, out); }

/* ---- CURRENCY ------------------------------------------------------------------------------------------------------------------------------------------ */
#define CY_UNIT 10000LL

/* x / 10000, rounded half to even */
static long long cy_round_int(long long x) {
  long long q = x / CY_UNIT, r = x % CY_UNIT;
  if (r < 0) { r = -r; if (r > 5000 || (r == 5000 && (q & 1))) q--; }
  else if (r > 5000 || (r == 5000 && (q & 1))) q++;
  return q;
}
static double cy_to_double(CY c) { return (double)c.int64 / 10000.0; }

/* (exact: the 53-bit mantissa times 10000, shifted by the exponent, rounded half to even; a floating-point multiply would round, and a Windows process runs the x87 unit at 53 bits) */
EXPORT HRESULT WINAPI VarCyFromR8(DOUBLE v, CY *out) {
  unsigned long long bits;
  memcpy(&bits, &v, sizeof(bits));
  const int neg = (bits >> 63) != 0, bexp = (int)((bits >> 52) & 0x7ff);
  if (bexp == 0x7ff) return DISP_E_OVERFLOW;
  unsigned long long m = bits & 0xfffffffffffffull;
  int e;
  if (bexp) { m |= 1ull << 52; e = bexp - 1075; } else e = -1074;
  DB mag;
  db_set_u64(&mag, m);
  db_mul_u32(&mag, 10000);
  if (e > 0) {
    if (m && e > 70) return DISP_E_OVERFLOW;
    for (int i = 0; i < e; i++) db_shl1(&mag);
  } else if (e < 0) {
    if (-e > 130) db_zero(&mag);
    else {
      DB p, q, r;
      db_set_u64(&p, 1);
      for (int i = 0; i < -e; i++) db_shl1(&p);
      db_divmod(&mag, &p, &q, &r);
      db_shl1(&r);
      const int c = db_cmp(&r, &p);
      if (c > 0 || (c == 0 && (q.w[0] & 1))) db_add_u32(&q, 1);
      mag = q;
    }
  }
  if (!db_fits64(&mag)) return DISP_E_OVERFLOW;
  const unsigned long long u = db_u64(&mag);
  if (neg) { if (u > 0x8000000000000000ull) return DISP_E_OVERFLOW; out->int64 = (long long)(0ull - u); }
  else { if (u > 0x7fffffffffffffffull) return DISP_E_OVERFLOW; out->int64 = (long long)u; }
  return S_OK;
}
EXPORT HRESULT WINAPI VarCyFromR4(FLOAT v, CY *out) { return VarCyFromR8((double)v, out); }
EXPORT HRESULT WINAPI VarCyFromDate(DATE v, CY *out) { return VarCyFromR8(v, out); }
EXPORT HRESULT WINAPI VarCyFromI1(CHAR v, CY *out) { out->int64 = (long long)v * CY_UNIT; return S_OK; }
EXPORT HRESULT WINAPI VarCyFromI2(SHORT v, CY *out) { out->int64 = (long long)v * CY_UNIT; return S_OK; }
EXPORT HRESULT WINAPI VarCyFromI4(LONG v, CY *out) { out->int64 = (long long)v * CY_UNIT; return S_OK; }
EXPORT HRESULT WINAPI VarCyFromUI1(BYTE v, CY *out) { out->int64 = (long long)v * CY_UNIT; return S_OK; }
EXPORT HRESULT WINAPI VarCyFromUI2(USHORT v, CY *out) { out->int64 = (long long)v * CY_UNIT; return S_OK; }
EXPORT HRESULT WINAPI VarCyFromUI4(ULONG v, CY *out) { out->int64 = (long long)v * CY_UNIT; return S_OK; }
EXPORT HRESULT WINAPI VarCyFromBool(VARIANT_BOOL v, CY *out) { out->int64 = (long long)v * CY_UNIT; return S_OK; }
/* (Windows rejects one more value than the 64-bit range holds, below as above) */
EXPORT HRESULT WINAPI VarCyFromI8(LONG64 v, CY *out) {
  if (v >= 922337203685477LL || v <= -922337203685477LL) return DISP_E_OVERFLOW;
  out->int64 = v * CY_UNIT;
  return S_OK;
}
EXPORT HRESULT WINAPI VarCyFromUI8(ULONG64 v, CY *out) {
  if (v > 922337203685477ull) return DISP_E_OVERFLOW;
  out->int64 = (long long)v * CY_UNIT;
  return S_OK;
}
EXPORT HRESULT WINAPI VarCyFromDec(DECIMAL *d, CY *out) {
  if (!dec_valid(d)) return E_INVALIDARG;
  DB m;
  dec_unpack(d, &m);
  if (d->scale > 4) db_div_pow10_round(&m, (unsigned)d->scale - 4);
  else if (db_mul_pow10(&m, 4u - d->scale)) return DISP_E_OVERFLOW;
  if (!db_fits64(&m)) return DISP_E_OVERFLOW;
  const unsigned long long v = db_u64(&m);
  if (d->sign & DECIMAL_NEG) { if (v > 0x8000000000000000ull) return DISP_E_OVERFLOW; out->int64 = (long long)(0ull - v); }
  else { if (v > 0x7fffffffffffffffull) return DISP_E_OVERFLOW; out->int64 = (long long)v; }
  return S_OK;
}
EXPORT HRESULT WINAPI VarCyFromStr(LPCOLESTR str, LCID lcid, ULONG flags, CY *out) {
  VARIANT v;
  HRESULT hr = oa_number_from_str(str, lcid, flags, VTBIT_CY, NUMPRS_STD | NUMPRS_CURRENCY, &v);
  if (SUCCEEDED(hr)) {
    if (V_VT(&v) == VT_CY) *out = V_CY(&v);
    else hr = DISP_E_TYPEMISMATCH;
  }
  return hr;
}

/* from a currency: the integer types round half to even (the 64-bit signed one is, as in Windows, one less than the next lower integer for a negative value) */
EXPORT HRESULT WINAPI VarI8FromCy(CY c, LONG64 *out) {
  if (c.int64 < 0) *out = c.int64 / CY_UNIT - 1;
  else *out = cy_round_int(c.int64);
  return S_OK;
}
EXPORT HRESULT WINAPI VarUI8FromCy(CY c, ULONG64 *out) {
  const long long v = cy_round_int(c.int64);
  if (v < 0) return DISP_E_OVERFLOW;
  *out = (ULONG64)v;
  return S_OK;
}
EXPORT HRESULT WINAPI VarI1FromCy(CY c, CHAR *out) { const long long v = cy_round_int(c.int64); if (v < -128 || v > 127) return DISP_E_OVERFLOW; *out = (CHAR)v; return S_OK; }
EXPORT HRESULT WINAPI VarI2FromCy(CY c, SHORT *out) { const long long v = cy_round_int(c.int64); if (v < -32768 || v > 32767) return DISP_E_OVERFLOW; *out = (SHORT)v; return S_OK; }
EXPORT HRESULT WINAPI VarI4FromCy(CY c, LONG *out) { const long long v = cy_round_int(c.int64); if (v < -2147483647LL - 1 || v > 2147483647LL) return DISP_E_OVERFLOW; *out = (LONG)v; return S_OK; }
EXPORT HRESULT WINAPI VarUI1FromCy(CY c, BYTE *out) { const long long v = cy_round_int(c.int64); if (v < 0 || v > 255) return DISP_E_OVERFLOW; *out = (BYTE)v; return S_OK; }
EXPORT HRESULT WINAPI VarUI2FromCy(CY c, USHORT *out) { const long long v = cy_round_int(c.int64); if (v < 0 || v > 65535) return DISP_E_OVERFLOW; *out = (USHORT)v; return S_OK; }
EXPORT HRESULT WINAPI VarUI4FromCy(CY c, ULONG *out) { const long long v = cy_round_int(c.int64); if (v < 0 || v > 0xffffffffLL) return DISP_E_OVERFLOW; *out = (ULONG)v; return S_OK; }
EXPORT HRESULT WINAPI VarR8FromCy(CY c, DOUBLE *out) { *out = cy_to_double(c); return S_OK; }
EXPORT HRESULT WINAPI VarR4FromCy(CY c, FLOAT *out) { *out = (FLOAT)cy_to_double(c); return S_OK; }
EXPORT HRESULT WINAPI VarDateFromCy(CY c, DATE *out) { *out = cy_to_double(c); return S_OK; }
EXPORT HRESULT WINAPI VarBoolFromCy(CY c, VARIANT_BOOL *out) { *out = c.int64 ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
EXPORT HRESULT WINAPI VarBstrFromCy(CY c, LCID lcid, ULONG flags, BSTR *out) {
  DECIMAL d;
  VarDecFromCy(c, &d);
  return dec_to_bstr(&d, lcid, flags, out);
}

/* ---- currency arithmetic ------------------------------------------------------------------------------------------------------------------------------- */
EXPORT HRESULT WINAPI VarCyAdd(CY l, CY r, LPCY out) { long long v; if (__builtin_add_overflow(l.int64, r.int64, &v)) return DISP_E_OVERFLOW; out->int64 = v; return S_OK; }
EXPORT HRESULT WINAPI VarCySub(CY l, CY r, LPCY out) { long long v; if (__builtin_sub_overflow(l.int64, r.int64, &v)) return DISP_E_OVERFLOW; out->int64 = v; return S_OK; }
EXPORT HRESULT WINAPI VarCyAbs(CY c, LPCY out) { out->int64 = c.int64 < 0 ? (long long)(0ull - (unsigned long long)c.int64) : c.int64; return S_OK; }
EXPORT HRESULT WINAPI VarCyNeg(CY c, LPCY out) { out->int64 = (long long)(0ull - (unsigned long long)c.int64); return S_OK; }
EXPORT HRESULT WINAPI VarCyCmp(CY l, CY r) { return l.int64 < r.int64 ? VARCMP_LT : l.int64 > r.int64 ? VARCMP_GT : VARCMP_EQ; }
EXPORT HRESULT WINAPI VarCyCmpR8(CY l, double r) { const double d = cy_to_double(l); return d < r ? VARCMP_LT : d > r ? VARCMP_GT : VARCMP_EQ; }
EXPORT HRESULT WINAPI VarCyFix(CY c, LPCY out) { out->int64 = c.int64 / CY_UNIT * CY_UNIT; return S_OK; }
EXPORT HRESULT WINAPI VarCyInt(CY c, LPCY out) {
  long long q = c.int64 / CY_UNIT;
  if (c.int64 % CY_UNIT < 0) q--;
  out->int64 = q * CY_UNIT;
  return S_OK;
}
EXPORT HRESULT WINAPI VarCyRound(CY c, int decimals, LPCY out) {
  if (decimals < 0) return E_INVALIDARG;
  if (decimals >= 4) { *out = c; return S_OK; }
  DECIMAL d, rounded;
  VarDecFromCy(c, &d);
  HRESULT hr = VarDecRound(&d, decimals, &rounded);
  if (SUCCEEDED(hr)) hr = VarCyFromDec(&rounded, out);
  return hr;
}
static HRESULT cy_mul_dec(CY l, DECIMAL *r, LPCY out) {
  DECIMAL a, p;
  VarDecFromCy(l, &a);
  HRESULT hr = VarDecMul(&a, r, &p);
  if (SUCCEEDED(hr)) hr = VarCyFromDec(&p, out);
  return hr;
}
EXPORT HRESULT WINAPI VarCyMul(CY l, CY r, LPCY out) { DECIMAL d; VarDecFromCy(r, &d); return cy_mul_dec(l, &d, out); }
EXPORT HRESULT WINAPI VarCyMulI4(CY l, LONG r, LPCY out) { DECIMAL d; VarDecFromI4(r, &d); return cy_mul_dec(l, &d, out); }
EXPORT HRESULT WINAPI VarCyMulI8(CY l, LONG64 r, LPCY out) { DECIMAL d; VarDecFromI8(r, &d); return cy_mul_dec(l, &d, out); }

/* ---- doubles ------------------------------------------------------------------------------------------------------------------------------------------- */
/* rounds to `decimals` digits after the point, half to even */
EXPORT HRESULT WINAPI VarR8Round(double v, int decimals, double *out) {
  if (decimals < 0) return E_INVALIDARG;
  const double scale = oa_pow(10.0, decimals);
  v *= scale;
  const double whole = v < 0 ? oa_ceil(v) : oa_floor(v), fract = v - whole;
  if (fract > 0.5) v = whole + 1.0;
  else if (fract == 0.5) v = whole + oa_fmod(whole, 2.0);
  else if (fract >= 0.0) v = whole;
  else if (fract == -0.5) v = whole + oa_fmod(whole, 2.0);
  else if (fract > -0.5) v = whole;
  else v = whole - 1.0;
  *out = v / scale;
  return S_OK;
}
EXPORT HRESULT WINAPI VarR4CmpR8(float l, double r) { const float f = (float)r; return l < f ? VARCMP_LT : l > f ? VARCMP_GT : VARCMP_EQ; }
EXPORT HRESULT WINAPI VarR8Pow(double l, double r, double *out) { *out = oa_pow(l, r); return S_OK; }
