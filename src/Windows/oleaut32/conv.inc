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

/* Conversions between the VARIANT types: the Var<T>From<S> functions (numbers, BOOL, DATE, BSTR, IDispatch) and VariantChangeType[Ex]. The conversions that involve CY or DECIMAL are in decimal.inc,
 * those from and to strings of dates in date.inc; the number parsing for the string conversions is in parse.inc. */

#ifndef VARIANT_ALPHABOOL
#define VARIANT_ALPHABOOL 0x02
#endif
#ifndef VARIANT_NOUSEROVERRIDE
#define VARIANT_NOUSEROVERRIDE 0x04
#endif

/* ---- rounding and range checks --------------------------------------------------------------------------------------------------------------------------- */

#define CV_TWO63 9223372036854775808.0
#define CV_TWO64 18446744073709551616.0
#define CV_DATE_MIN (-657434.0)
#define CV_DATE_MAX 2958465.0

/* Rounds halves to even ("banker's rounding", as Windows does for every float to integer conversion). */
static double cv_round_even(double d) {
  if (!(oa_fabs(d) < 4503599627370496.0)) return d;
  const double f = oa_floor(d), diff = d - f;
  if (diff > 0.5) return f + 1.0;
  if (diff < 0.5) return f;
  return ((long long)f & 1) ? f + 1.0 : f;
}

static HRESULT cv_chk_s(long long v, long long lo, unsigned long long hi) {
  if (v < lo || (v > 0 && (unsigned long long)v > hi)) return DISP_E_OVERFLOW;
  return S_OK;
}
static HRESULT cv_chk_u(unsigned long long v, unsigned long long hi) { return v > hi ? DISP_E_OVERFLOW : S_OK; }

/* A float, rounded, as a 64-bit integer when it lies in [lo, hi) (the bounds are exactly representable). */
static HRESULT cv_f2ll(double d, double lo, double hi, long long *out) {
  const double r = cv_round_even(d);
  if (!(r >= lo && r < hi)) return DISP_E_OVERFLOW;
  *out = (long long)r;
  return S_OK;
}
static HRESULT cv_f2ull(double d, double hi, unsigned long long *out) {
  const double r = cv_round_even(d);
  if (!(r >= 0.0 && r < hi)) return DISP_E_OVERFLOW;
  *out = r >= CV_TWO63 ? (unsigned long long)(long long)(r - CV_TWO63) + (1ull << 63) : (unsigned long long)(long long)r;
  return S_OK;
}

/* ---- the integer, float, BOOL and DATE conversions among each other --------------------------------------------------------------------------------- */

/* @begin ints */
EXPORT HRESULT WINAPI VarI1FromI2(SHORT in, CHAR *out) { HRESULT hr = cv_chk_s((long long)in, -128, 127); if (hr == S_OK) *out = (CHAR)in; return hr; }
EXPORT HRESULT WINAPI VarI1FromI4(LONG in, CHAR *out) { HRESULT hr = cv_chk_s((long long)in, -128, 127); if (hr == S_OK) *out = (CHAR)in; return hr; }
EXPORT HRESULT WINAPI VarI1FromI8(LONG64 in, CHAR *out) { HRESULT hr = cv_chk_s((long long)in, -128, 127); if (hr == S_OK) *out = (CHAR)in; return hr; }
EXPORT HRESULT WINAPI VarI1FromUI1(BYTE in, CHAR *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 127); if (hr == S_OK) *out = (CHAR)in; return hr; }
EXPORT HRESULT WINAPI VarI1FromUI2(USHORT in, CHAR *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 127); if (hr == S_OK) *out = (CHAR)in; return hr; }
EXPORT HRESULT WINAPI VarI1FromUI4(ULONG in, CHAR *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 127); if (hr == S_OK) *out = (CHAR)in; return hr; }
EXPORT HRESULT WINAPI VarI1FromUI8(ULONG64 in, CHAR *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 127); if (hr == S_OK) *out = (CHAR)in; return hr; }
EXPORT HRESULT WINAPI VarI1FromR4(FLOAT in, CHAR *out) { long long v; HRESULT hr = cv_f2ll((double)in, -128.0, 128.0, &v); if (hr == S_OK) *out = (CHAR)v; return hr; }
EXPORT HRESULT WINAPI VarI1FromR8(DOUBLE in, CHAR *out) { long long v; HRESULT hr = cv_f2ll((double)in, -128.0, 128.0, &v); if (hr == S_OK) *out = (CHAR)v; return hr; }
EXPORT HRESULT WINAPI VarI1FromBool(VARIANT_BOOL in, CHAR *out) { *out = (CHAR)in; return S_OK; }
EXPORT HRESULT WINAPI VarI1FromDate(DATE in, CHAR *out) { return VarI1FromR8(in, out); }

EXPORT HRESULT WINAPI VarUI1FromI1(CHAR in, BYTE *out) { HRESULT hr = cv_chk_s((long long)in, 0, 255); if (hr == S_OK) *out = (BYTE)in; return hr; }
EXPORT HRESULT WINAPI VarUI1FromI2(SHORT in, BYTE *out) { HRESULT hr = cv_chk_s((long long)in, 0, 255); if (hr == S_OK) *out = (BYTE)in; return hr; }
EXPORT HRESULT WINAPI VarUI1FromI4(LONG in, BYTE *out) { HRESULT hr = cv_chk_s((long long)in, 0, 255); if (hr == S_OK) *out = (BYTE)in; return hr; }
EXPORT HRESULT WINAPI VarUI1FromI8(LONG64 in, BYTE *out) { HRESULT hr = cv_chk_s((long long)in, 0, 255); if (hr == S_OK) *out = (BYTE)in; return hr; }
EXPORT HRESULT WINAPI VarUI1FromUI2(USHORT in, BYTE *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 255); if (hr == S_OK) *out = (BYTE)in; return hr; }
EXPORT HRESULT WINAPI VarUI1FromUI4(ULONG in, BYTE *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 255); if (hr == S_OK) *out = (BYTE)in; return hr; }
EXPORT HRESULT WINAPI VarUI1FromUI8(ULONG64 in, BYTE *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 255); if (hr == S_OK) *out = (BYTE)in; return hr; }
EXPORT HRESULT WINAPI VarUI1FromR4(FLOAT in, BYTE *out) { long long v; HRESULT hr = cv_f2ll((double)in, 0.0, 256.0, &v); if (hr == S_OK) *out = (BYTE)v; return hr; }
EXPORT HRESULT WINAPI VarUI1FromR8(DOUBLE in, BYTE *out) { long long v; HRESULT hr = cv_f2ll((double)in, 0.0, 256.0, &v); if (hr == S_OK) *out = (BYTE)v; return hr; }
EXPORT HRESULT WINAPI VarUI1FromBool(VARIANT_BOOL in, BYTE *out) { *out = (BYTE)in; return S_OK; }
EXPORT HRESULT WINAPI VarUI1FromDate(DATE in, BYTE *out) { return VarUI1FromR8(in, out); }

EXPORT HRESULT WINAPI VarI2FromI1(CHAR in, SHORT *out) { HRESULT hr = cv_chk_s((long long)in, -32768, 32767); if (hr == S_OK) *out = (SHORT)in; return hr; }
EXPORT HRESULT WINAPI VarI2FromI4(LONG in, SHORT *out) { HRESULT hr = cv_chk_s((long long)in, -32768, 32767); if (hr == S_OK) *out = (SHORT)in; return hr; }
EXPORT HRESULT WINAPI VarI2FromI8(LONG64 in, SHORT *out) { HRESULT hr = cv_chk_s((long long)in, -32768, 32767); if (hr == S_OK) *out = (SHORT)in; return hr; }
EXPORT HRESULT WINAPI VarI2FromUI1(BYTE in, SHORT *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 32767); if (hr == S_OK) *out = (SHORT)in; return hr; }
EXPORT HRESULT WINAPI VarI2FromUI2(USHORT in, SHORT *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 32767); if (hr == S_OK) *out = (SHORT)in; return hr; }
EXPORT HRESULT WINAPI VarI2FromUI4(ULONG in, SHORT *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 32767); if (hr == S_OK) *out = (SHORT)in; return hr; }
EXPORT HRESULT WINAPI VarI2FromUI8(ULONG64 in, SHORT *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 32767); if (hr == S_OK) *out = (SHORT)in; return hr; }
EXPORT HRESULT WINAPI VarI2FromR4(FLOAT in, SHORT *out) { long long v; HRESULT hr = cv_f2ll((double)in, -32768.0, 32768.0, &v); if (hr == S_OK) *out = (SHORT)v; return hr; }
EXPORT HRESULT WINAPI VarI2FromR8(DOUBLE in, SHORT *out) { long long v; HRESULT hr = cv_f2ll((double)in, -32768.0, 32768.0, &v); if (hr == S_OK) *out = (SHORT)v; return hr; }
EXPORT HRESULT WINAPI VarI2FromBool(VARIANT_BOOL in, SHORT *out) { *out = (SHORT)in; return S_OK; }
EXPORT HRESULT WINAPI VarI2FromDate(DATE in, SHORT *out) { return VarI2FromR8(in, out); }

EXPORT HRESULT WINAPI VarUI2FromI1(CHAR in, USHORT *out) { HRESULT hr = cv_chk_s((long long)in, 0, 65535); if (hr == S_OK) *out = (USHORT)in; return hr; }
EXPORT HRESULT WINAPI VarUI2FromI2(SHORT in, USHORT *out) { HRESULT hr = cv_chk_s((long long)in, 0, 65535); if (hr == S_OK) *out = (USHORT)in; return hr; }
EXPORT HRESULT WINAPI VarUI2FromI4(LONG in, USHORT *out) { HRESULT hr = cv_chk_s((long long)in, 0, 65535); if (hr == S_OK) *out = (USHORT)in; return hr; }
EXPORT HRESULT WINAPI VarUI2FromI8(LONG64 in, USHORT *out) { HRESULT hr = cv_chk_s((long long)in, 0, 65535); if (hr == S_OK) *out = (USHORT)in; return hr; }
EXPORT HRESULT WINAPI VarUI2FromUI1(BYTE in, USHORT *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 65535); if (hr == S_OK) *out = (USHORT)in; return hr; }
EXPORT HRESULT WINAPI VarUI2FromUI4(ULONG in, USHORT *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 65535); if (hr == S_OK) *out = (USHORT)in; return hr; }
EXPORT HRESULT WINAPI VarUI2FromUI8(ULONG64 in, USHORT *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 65535); if (hr == S_OK) *out = (USHORT)in; return hr; }
EXPORT HRESULT WINAPI VarUI2FromR4(FLOAT in, USHORT *out) { long long v; HRESULT hr = cv_f2ll((double)in, 0.0, 65536.0, &v); if (hr == S_OK) *out = (USHORT)v; return hr; }
EXPORT HRESULT WINAPI VarUI2FromR8(DOUBLE in, USHORT *out) { long long v; HRESULT hr = cv_f2ll((double)in, 0.0, 65536.0, &v); if (hr == S_OK) *out = (USHORT)v; return hr; }
EXPORT HRESULT WINAPI VarUI2FromBool(VARIANT_BOOL in, USHORT *out) { *out = (USHORT)in; return S_OK; }
EXPORT HRESULT WINAPI VarUI2FromDate(DATE in, USHORT *out) { return VarUI2FromR8(in, out); }

EXPORT HRESULT WINAPI VarI4FromI1(CHAR in, LONG *out) { HRESULT hr = cv_chk_s((long long)in, (-2147483647LL - 1), 2147483647); if (hr == S_OK) *out = (LONG)in; return hr; }
EXPORT HRESULT WINAPI VarI4FromI2(SHORT in, LONG *out) { HRESULT hr = cv_chk_s((long long)in, (-2147483647LL - 1), 2147483647); if (hr == S_OK) *out = (LONG)in; return hr; }
EXPORT HRESULT WINAPI VarI4FromI8(LONG64 in, LONG *out) { HRESULT hr = cv_chk_s((long long)in, (-2147483647LL - 1), 2147483647); if (hr == S_OK) *out = (LONG)in; return hr; }
EXPORT HRESULT WINAPI VarI4FromUI1(BYTE in, LONG *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 2147483647); if (hr == S_OK) *out = (LONG)in; return hr; }
EXPORT HRESULT WINAPI VarI4FromUI2(USHORT in, LONG *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 2147483647); if (hr == S_OK) *out = (LONG)in; return hr; }
EXPORT HRESULT WINAPI VarI4FromUI4(ULONG in, LONG *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 2147483647); if (hr == S_OK) *out = (LONG)in; return hr; }
EXPORT HRESULT WINAPI VarI4FromUI8(ULONG64 in, LONG *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 2147483647); if (hr == S_OK) *out = (LONG)in; return hr; }
EXPORT HRESULT WINAPI VarI4FromR4(FLOAT in, LONG *out) { long long v; HRESULT hr = cv_f2ll((double)in, -2147483648.0, 2147483648.0, &v); if (hr == S_OK) *out = (LONG)v; return hr; }
EXPORT HRESULT WINAPI VarI4FromR8(DOUBLE in, LONG *out) { long long v; HRESULT hr = cv_f2ll((double)in, -2147483648.0, 2147483648.0, &v); if (hr == S_OK) *out = (LONG)v; return hr; }
EXPORT HRESULT WINAPI VarI4FromBool(VARIANT_BOOL in, LONG *out) { *out = (LONG)in; return S_OK; }
EXPORT HRESULT WINAPI VarI4FromDate(DATE in, LONG *out) { return VarI4FromR8(in, out); }

EXPORT HRESULT WINAPI VarUI4FromI1(CHAR in, ULONG *out) { HRESULT hr = cv_chk_s((long long)in, 0, 4294967295u); if (hr == S_OK) *out = (ULONG)in; return hr; }
EXPORT HRESULT WINAPI VarUI4FromI2(SHORT in, ULONG *out) { HRESULT hr = cv_chk_s((long long)in, 0, 4294967295u); if (hr == S_OK) *out = (ULONG)in; return hr; }
EXPORT HRESULT WINAPI VarUI4FromI4(LONG in, ULONG *out) { HRESULT hr = cv_chk_s((long long)in, 0, 4294967295u); if (hr == S_OK) *out = (ULONG)in; return hr; }
EXPORT HRESULT WINAPI VarUI4FromI8(LONG64 in, ULONG *out) { HRESULT hr = cv_chk_s((long long)in, 0, 4294967295u); if (hr == S_OK) *out = (ULONG)in; return hr; }
EXPORT HRESULT WINAPI VarUI4FromUI1(BYTE in, ULONG *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 4294967295u); if (hr == S_OK) *out = (ULONG)in; return hr; }
EXPORT HRESULT WINAPI VarUI4FromUI2(USHORT in, ULONG *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 4294967295u); if (hr == S_OK) *out = (ULONG)in; return hr; }
EXPORT HRESULT WINAPI VarUI4FromUI8(ULONG64 in, ULONG *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 4294967295u); if (hr == S_OK) *out = (ULONG)in; return hr; }
EXPORT HRESULT WINAPI VarUI4FromR4(FLOAT in, ULONG *out) { long long v; HRESULT hr = cv_f2ll((double)in, 0.0, 4294967296.0, &v); if (hr == S_OK) *out = (ULONG)v; return hr; }
EXPORT HRESULT WINAPI VarUI4FromR8(DOUBLE in, ULONG *out) { long long v; HRESULT hr = cv_f2ll((double)in, 0.0, 4294967296.0, &v); if (hr == S_OK) *out = (ULONG)v; return hr; }
EXPORT HRESULT WINAPI VarUI4FromBool(VARIANT_BOOL in, ULONG *out) { *out = (ULONG)in; return S_OK; }
EXPORT HRESULT WINAPI VarUI4FromDate(DATE in, ULONG *out) { return VarUI4FromR8(in, out); }

EXPORT HRESULT WINAPI VarI8FromI1(CHAR in, LONG64 *out) { HRESULT hr = cv_chk_s((long long)in, (-9223372036854775807LL - 1), 9223372036854775807ull); if (hr == S_OK) *out = (LONG64)in; return hr; }
EXPORT HRESULT WINAPI VarI8FromI2(SHORT in, LONG64 *out) { HRESULT hr = cv_chk_s((long long)in, (-9223372036854775807LL - 1), 9223372036854775807ull); if (hr == S_OK) *out = (LONG64)in; return hr; }
EXPORT HRESULT WINAPI VarI8FromUI1(BYTE in, LONG64 *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 9223372036854775807ull); if (hr == S_OK) *out = (LONG64)in; return hr; }
EXPORT HRESULT WINAPI VarI8FromUI2(USHORT in, LONG64 *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 9223372036854775807ull); if (hr == S_OK) *out = (LONG64)in; return hr; }
EXPORT HRESULT WINAPI VarI8FromUI4(ULONG in, LONG64 *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 9223372036854775807ull); if (hr == S_OK) *out = (LONG64)in; return hr; }
EXPORT HRESULT WINAPI VarI8FromUI8(ULONG64 in, LONG64 *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 9223372036854775807ull); if (hr == S_OK) *out = (LONG64)in; return hr; }
EXPORT HRESULT WINAPI VarI8FromR4(FLOAT in, LONG64 *out) { long long v; HRESULT hr = cv_f2ll((double)in, -CV_TWO63, CV_TWO63, &v); if (hr == S_OK) *out = (LONG64)v; return hr; }
EXPORT HRESULT WINAPI VarI8FromR8(DOUBLE in, LONG64 *out) { long long v; HRESULT hr = cv_f2ll((double)in, -CV_TWO63, CV_TWO63, &v); if (hr == S_OK) *out = (LONG64)v; return hr; }
EXPORT HRESULT WINAPI VarI8FromBool(VARIANT_BOOL in, LONG64 *out) { *out = (LONG64)in; return S_OK; }
EXPORT HRESULT WINAPI VarI8FromDate(DATE in, LONG64 *out) { return VarI8FromR8(in, out); }

EXPORT HRESULT WINAPI VarUI8FromI1(CHAR in, ULONG64 *out) { HRESULT hr = cv_chk_s((long long)in, 0, 18446744073709551615ull); if (hr == S_OK) *out = (ULONG64)in; return hr; }
EXPORT HRESULT WINAPI VarUI8FromI2(SHORT in, ULONG64 *out) { HRESULT hr = cv_chk_s((long long)in, 0, 18446744073709551615ull); if (hr == S_OK) *out = (ULONG64)in; return hr; }
EXPORT HRESULT WINAPI VarUI8FromI8(LONG64 in, ULONG64 *out) { HRESULT hr = cv_chk_s((long long)in, 0, 18446744073709551615ull); if (hr == S_OK) *out = (ULONG64)in; return hr; }
EXPORT HRESULT WINAPI VarUI8FromUI1(BYTE in, ULONG64 *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 18446744073709551615ull); if (hr == S_OK) *out = (ULONG64)in; return hr; }
EXPORT HRESULT WINAPI VarUI8FromUI2(USHORT in, ULONG64 *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 18446744073709551615ull); if (hr == S_OK) *out = (ULONG64)in; return hr; }
EXPORT HRESULT WINAPI VarUI8FromUI4(ULONG in, ULONG64 *out) { HRESULT hr = cv_chk_u((unsigned long long)in, 18446744073709551615ull); if (hr == S_OK) *out = (ULONG64)in; return hr; }
EXPORT HRESULT WINAPI VarUI8FromR4(FLOAT in, ULONG64 *out) { unsigned long long v; HRESULT hr = cv_f2ull((double)in, CV_TWO64, &v); if (hr == S_OK) *out = v; return hr; }
EXPORT HRESULT WINAPI VarUI8FromR8(DOUBLE in, ULONG64 *out) { unsigned long long v; HRESULT hr = cv_f2ull((double)in, CV_TWO64, &v); if (hr == S_OK) *out = v; return hr; }
EXPORT HRESULT WINAPI VarUI8FromBool(VARIANT_BOOL in, ULONG64 *out) { *out = (ULONG64)in; return S_OK; }
EXPORT HRESULT WINAPI VarUI8FromDate(DATE in, ULONG64 *out) { return VarUI8FromR8(in, out); }

/* @end ints */

/* (the conversions that no function of Windows is for) */
static HRESULT cv_i8_from_i4(LONG in, LONG64 *out) { *out = in; return S_OK; }
static HRESULT cv_ui8_from_i4(LONG in, ULONG64 *out) { if (in < 0) return DISP_E_OVERFLOW; *out = (ULONG64)in; return S_OK; }
#define VarI8FromI4 cv_i8_from_i4
#define VarUI8FromI4 cv_ui8_from_i4

/* R4, R8, DATE */
/* @begin floats */
EXPORT HRESULT WINAPI VarR4FromI1(CHAR in, FLOAT *out) { *out = (FLOAT)in; return S_OK; }
EXPORT HRESULT WINAPI VarR4FromUI1(BYTE in, FLOAT *out) { *out = (FLOAT)in; return S_OK; }
EXPORT HRESULT WINAPI VarR4FromI2(SHORT in, FLOAT *out) { *out = (FLOAT)in; return S_OK; }
EXPORT HRESULT WINAPI VarR4FromUI2(USHORT in, FLOAT *out) { *out = (FLOAT)in; return S_OK; }
EXPORT HRESULT WINAPI VarR4FromI4(LONG in, FLOAT *out) { *out = (FLOAT)in; return S_OK; }
EXPORT HRESULT WINAPI VarR4FromUI4(ULONG in, FLOAT *out) { *out = (FLOAT)in; return S_OK; }
EXPORT HRESULT WINAPI VarR4FromI8(LONG64 in, FLOAT *out) { *out = (FLOAT)in; return S_OK; }
EXPORT HRESULT WINAPI VarR4FromUI8(ULONG64 in, FLOAT *out) { *out = (FLOAT)in; return S_OK; }
EXPORT HRESULT WINAPI VarR4FromBool(VARIANT_BOOL in, FLOAT *out) { *out = (FLOAT)in; return S_OK; }
EXPORT HRESULT WINAPI VarR8FromI1(CHAR in, DOUBLE *out) { *out = (DOUBLE)in; return S_OK; }
EXPORT HRESULT WINAPI VarR8FromUI1(BYTE in, DOUBLE *out) { *out = (DOUBLE)in; return S_OK; }
EXPORT HRESULT WINAPI VarR8FromI2(SHORT in, DOUBLE *out) { *out = (DOUBLE)in; return S_OK; }
EXPORT HRESULT WINAPI VarR8FromUI2(USHORT in, DOUBLE *out) { *out = (DOUBLE)in; return S_OK; }
EXPORT HRESULT WINAPI VarR8FromI4(LONG in, DOUBLE *out) { *out = (DOUBLE)in; return S_OK; }
EXPORT HRESULT WINAPI VarR8FromUI4(ULONG in, DOUBLE *out) { *out = (DOUBLE)in; return S_OK; }
EXPORT HRESULT WINAPI VarR8FromI8(LONG64 in, DOUBLE *out) { *out = (DOUBLE)in; return S_OK; }
EXPORT HRESULT WINAPI VarR8FromUI8(ULONG64 in, DOUBLE *out) { *out = (DOUBLE)in; return S_OK; }
EXPORT HRESULT WINAPI VarR8FromBool(VARIANT_BOOL in, DOUBLE *out) { *out = (DOUBLE)in; return S_OK; }
/* @end floats */
EXPORT HRESULT WINAPI VarR4FromR8(DOUBLE in, FLOAT *out) { if (oa_fabs(in) > 3.4028234663852886e38) return DISP_E_OVERFLOW; *out = (FLOAT)in; return S_OK; }
EXPORT HRESULT WINAPI VarR8FromR4(FLOAT in, DOUBLE *out) { *out = (DOUBLE)in; return S_OK; }
EXPORT HRESULT WINAPI VarR4FromDate(DATE in, FLOAT *out) { return VarR4FromR8(in, out); }
EXPORT HRESULT WINAPI VarR8FromDate(DATE in, DOUBLE *out) { *out = in; return S_OK; }

EXPORT HRESULT WINAPI VarDateFromR8(DOUBLE in, DATE *out) { if (!(in > CV_DATE_MIN - 1.0 && in < CV_DATE_MAX + 1.0)) return DISP_E_OVERFLOW; *out = in; return S_OK; }
EXPORT HRESULT WINAPI VarDateFromR4(FLOAT in, DATE *out) { return VarDateFromR8((double)in, out); }
EXPORT HRESULT WINAPI VarDateFromI1(CHAR in, DATE *out) { *out = in; return S_OK; }
EXPORT HRESULT WINAPI VarDateFromUI1(BYTE in, DATE *out) { *out = in; return S_OK; }
EXPORT HRESULT WINAPI VarDateFromI2(SHORT in, DATE *out) { *out = in; return S_OK; }
EXPORT HRESULT WINAPI VarDateFromUI2(USHORT in, DATE *out) { *out = in; return S_OK; }
EXPORT HRESULT WINAPI VarDateFromI4(LONG in, DATE *out) { if (in < (LONG)CV_DATE_MIN || in > (LONG)CV_DATE_MAX) return DISP_E_OVERFLOW; *out = in; return S_OK; }
EXPORT HRESULT WINAPI VarDateFromUI4(ULONG in, DATE *out) { if (in > (ULONG)CV_DATE_MAX) return DISP_E_OVERFLOW; *out = in; return S_OK; }
EXPORT HRESULT WINAPI VarDateFromI8(LONG64 in, DATE *out) { if (in < (LONG64)CV_DATE_MIN || in > (LONG64)CV_DATE_MAX) return DISP_E_OVERFLOW; *out = (DATE)in; return S_OK; }
EXPORT HRESULT WINAPI VarDateFromUI8(ULONG64 in, DATE *out) { if (in > (ULONG64)CV_DATE_MAX) return DISP_E_OVERFLOW; *out = (DATE)in; return S_OK; }
EXPORT HRESULT WINAPI VarDateFromBool(VARIANT_BOOL in, DATE *out) { *out = in; return S_OK; }

/* BOOL: any non-zero value is true */
/* @begin bools */
EXPORT HRESULT WINAPI VarBoolFromI1(CHAR in, VARIANT_BOOL *out) { *out = in ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
EXPORT HRESULT WINAPI VarBoolFromUI1(BYTE in, VARIANT_BOOL *out) { *out = in ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
EXPORT HRESULT WINAPI VarBoolFromI2(SHORT in, VARIANT_BOOL *out) { *out = in ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
EXPORT HRESULT WINAPI VarBoolFromUI2(USHORT in, VARIANT_BOOL *out) { *out = in ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
EXPORT HRESULT WINAPI VarBoolFromI4(LONG in, VARIANT_BOOL *out) { *out = in ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
EXPORT HRESULT WINAPI VarBoolFromUI4(ULONG in, VARIANT_BOOL *out) { *out = in ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
EXPORT HRESULT WINAPI VarBoolFromI8(LONG64 in, VARIANT_BOOL *out) { *out = in ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
EXPORT HRESULT WINAPI VarBoolFromUI8(ULONG64 in, VARIANT_BOOL *out) { *out = in ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
EXPORT HRESULT WINAPI VarBoolFromR4(FLOAT in, VARIANT_BOOL *out) { *out = in ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
EXPORT HRESULT WINAPI VarBoolFromR8(DOUBLE in, VARIANT_BOOL *out) { *out = in ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
EXPORT HRESULT WINAPI VarBoolFromDate(DATE in, VARIANT_BOOL *out) { *out = in ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
/* @end bools */

/* the conversions of a type to itself, for the table of VariantChangeType (VarI4FromI4 and the like are macros in the headers) */
#define CV_SAME(TN, TT) static HRESULT cv_same_##TN(TT in, TT *out) { *out = in; return S_OK; }
CV_SAME(I1, CHAR) CV_SAME(UI1, BYTE) CV_SAME(I2, SHORT) CV_SAME(UI2, USHORT) CV_SAME(I4, LONG) CV_SAME(UI4, ULONG) CV_SAME(I8, LONG64) CV_SAME(UI8, ULONG64)
CV_SAME(R4, FLOAT) CV_SAME(R8, DOUBLE) CV_SAME(Date, DATE) CV_SAME(Bool, VARIANT_BOOL) CV_SAME(Cy, CY)
static HRESULT cv_same_Dec(DECIMAL *in, DECIMAL *out) { *out = *in; return S_OK; }
#undef VarI4FromI4
#undef VarUI4FromUI4
#undef VarI8FromI8
#undef VarUI8FromUI8
#define VarI1FromI1 cv_same_I1
#define VarUI1FromUI1 cv_same_UI1
#define VarI2FromI2 cv_same_I2
#define VarUI2FromUI2 cv_same_UI2
#define VarI4FromI4 cv_same_I4
#define VarUI4FromUI4 cv_same_UI4
#define VarI8FromI8 cv_same_I8
#define VarUI8FromUI8 cv_same_UI8
#define VarR4FromR4 cv_same_R4
#define VarR8FromR8 cv_same_R8
#define VarDateFromDate cv_same_Date
#define VarBoolFromBool cv_same_Bool
#define VarCyFromCy cv_same_Cy
#define VarDecFromDec cv_same_Dec

/* ---- strings to numbers ------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT cv_number_from_str(const OLECHAR *string, LCID lcid, ULONG flags, ULONG vtbits, VARIANT *result) {
  NUMPARSE parse;
  BYTE digits[128];
  parse.cDig = (int)sizeof(digits);
  parse.dwInFlags = NUMPRS_STD;
  HRESULT hr = VarParseNumFromStr(string, lcid, flags, &parse, digits);
  if (FAILED(hr)) return hr;
  return VarNumFromParseNum(&parse, digits, vtbits, result);
}

/* @begin strs */
EXPORT HRESULT WINAPI VarI1FromStr(LPCOLESTR in, LCID lcid, ULONG flags, CHAR *out) { VARIANT v; HRESULT hr = cv_number_from_str(in, lcid, flags, VTBIT_I1, &v); if (hr == S_OK) *out = V_I1(&v); return hr; }
EXPORT HRESULT WINAPI VarUI1FromStr(LPCOLESTR in, LCID lcid, ULONG flags, BYTE *out) { VARIANT v; HRESULT hr = cv_number_from_str(in, lcid, flags, VTBIT_UI1, &v); if (hr == S_OK) *out = V_UI1(&v); return hr; }
EXPORT HRESULT WINAPI VarI2FromStr(LPCOLESTR in, LCID lcid, ULONG flags, SHORT *out) { VARIANT v; HRESULT hr = cv_number_from_str(in, lcid, flags, VTBIT_I2, &v); if (hr == S_OK) *out = V_I2(&v); return hr; }
EXPORT HRESULT WINAPI VarUI2FromStr(LPCOLESTR in, LCID lcid, ULONG flags, USHORT *out) { VARIANT v; HRESULT hr = cv_number_from_str(in, lcid, flags, VTBIT_UI2, &v); if (hr == S_OK) *out = V_UI2(&v); return hr; }
EXPORT HRESULT WINAPI VarI4FromStr(LPCOLESTR in, LCID lcid, ULONG flags, LONG *out) { VARIANT v; HRESULT hr = cv_number_from_str(in, lcid, flags, VTBIT_I4, &v); if (hr == S_OK) *out = V_I4(&v); return hr; }
EXPORT HRESULT WINAPI VarUI4FromStr(LPCOLESTR in, LCID lcid, ULONG flags, ULONG *out) { VARIANT v; HRESULT hr = cv_number_from_str(in, lcid, flags, VTBIT_UI4, &v); if (hr == S_OK) *out = V_UI4(&v); return hr; }
EXPORT HRESULT WINAPI VarI8FromStr(LPCOLESTR in, LCID lcid, ULONG flags, LONG64 *out) { VARIANT v; HRESULT hr = cv_number_from_str(in, lcid, flags, VTBIT_I8, &v); if (hr == S_OK) *out = V_I8(&v); return hr; }
EXPORT HRESULT WINAPI VarUI8FromStr(LPCOLESTR in, LCID lcid, ULONG flags, ULONG64 *out) { VARIANT v; HRESULT hr = cv_number_from_str(in, lcid, flags, VTBIT_UI8, &v); if (hr == S_OK) *out = V_UI8(&v); return hr; }
EXPORT HRESULT WINAPI VarR4FromStr(LPCOLESTR in, LCID lcid, ULONG flags, FLOAT *out) { VARIANT v; HRESULT hr = cv_number_from_str(in, lcid, flags, VTBIT_R4, &v); if (hr == S_OK) *out = V_R4(&v); return hr; }
EXPORT HRESULT WINAPI VarR8FromStr(LPCOLESTR in, LCID lcid, ULONG flags, DOUBLE *out) { VARIANT v; HRESULT hr = cv_number_from_str(in, lcid, flags, VTBIT_R8, &v); if (hr == S_OK) *out = V_R8(&v); return hr; }
/* @end strs */

/* ---- the text of TRUE and FALSE -------------------------------------------------------------------------------------------------------------------- */

typedef struct { WORD language; const WCHAR *yes, *no; } CV_BOOL_TEXT;
static const CV_BOOL_TEXT cv_bool_texts[] = {
  { 0x09, L"True", L"False" },
  { 0x0C, L"Vrai", L"Faux" },
  { 0x07, L"Wahr", L"Falsch" },
  { 0x0A, L"Verdadero", L"Falso" },
  { 0x10, L"Vero", L"Falso" },
  { 0x16, L"Verdadeiro", L"Falso" },
  { 0x13, L"Waar", L"Onwaar" },
  { 0x1D, L"Sant", L"Falskt" },
  { 0x06, L"Sandt", L"Falsk" },
  { 0x14, L"Sann", L"Usann" },
  { 0x0B, L"Tosi", L"Epätosi" },
  { 0x15, L"Prawda", L"Fałsz" },
  { 0x05, L"Pravda", L"Nepravda" },
  { 0x0E, L"Igaz", L"Hamis" },
  { 0x19, L"Adevărat", L"Fals" },
};

/* The primary language of an LCID, where the default LCIDs mean the user's. */
static WORD cv_primary_language(LCID lcid) {
  WORD language = (WORD)(lcid & 0x3FF);
  if (language == 0 || (lcid & 0xFFFF) == 0x0800 || (lcid & 0xFFFF) == 0x0400) {
    const LCID user = GetUserDefaultLCID();
    language = (WORD)(user & 0x3FF);
  }
  return language;
}
static const CV_BOOL_TEXT *cv_bool_text(WORD language) {
  for (size_t i = 0; i < OA_ARRAY_SIZE(cv_bool_texts); i++) if (cv_bool_texts[i].language == language) return &cv_bool_texts[i];
  return &cv_bool_texts[0];
}

static BOOL cv_equal_nocase(const WCHAR *a, const WCHAR *b) {
  for (; *a && *b; a++, b++) if (oa_towlower(*a) != oa_towlower(*b)) return FALSE;
  return *a == *b;
}

EXPORT HRESULT WINAPI VarBoolFromStr(LPCOLESTR in, LCID lcid, ULONG flags, VARIANT_BOOL *out) {
  if (!in || !out) return DISP_E_TYPEMISMATCH;
  /* #TRUE# and #FALSE# in every locale, then the English words, then (on request) those of the locale, and at last any number */
  if (!oa_wcscmp(in, L"#FALSE#")) { *out = VARIANT_FALSE; return S_OK; }
  if (!oa_wcscmp(in, L"#TRUE#")) { *out = VARIANT_TRUE; return S_OK; }
  if (cv_equal_nocase(in, cv_bool_texts[0].no)) { *out = VARIANT_FALSE; return S_OK; }
  if (cv_equal_nocase(in, cv_bool_texts[0].yes)) { *out = VARIANT_TRUE; return S_OK; }
  if (flags & VAR_LOCALBOOL) {
    const CV_BOOL_TEXT *text = cv_bool_text(cv_primary_language(lcid));
    if (cv_equal_nocase(in, text->no)) { *out = VARIANT_FALSE; return S_OK; }
    if (cv_equal_nocase(in, text->yes)) { *out = VARIANT_TRUE; return S_OK; }
  }
  double d;
  HRESULT hr = VarR8FromStr(in, lcid, flags, &d);
  if (FAILED(hr)) return hr;
  *out = d != 0.0 ? VARIANT_TRUE : VARIANT_FALSE;
  return S_OK;
}

/* ---- numbers to strings ------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT cv_make_bstr(const WCHAR *text, int length, BSTR *out) {
  if (!out) return E_INVALIDARG;
  *out = SysAllocStringLen(text, (UINT)length);
  return *out ? S_OK : E_OUTOFMEMORY;
}

static int cv_format_integer(unsigned long long magnitude, BOOL negative, WCHAR *out) {
  WCHAR tmp[24];
  int n = 0, length = 0;
  do { tmp[n++] = (WCHAR)('0' + (int)(magnitude % 10)); magnitude /= 10; } while (magnitude);
  if (negative) out[length++] = '-';
  while (n) out[length++] = tmp[--n];
  out[length] = 0;
  return length;
}

static HRESULT cv_bstr_from_signed(long long v, BSTR *out) {
  WCHAR text[24];
  const int n = cv_format_integer(v < 0 ? 0 - (unsigned long long)v : (unsigned long long)v, v < 0, text);
  return cv_make_bstr(text, n, out);
}
static HRESULT cv_bstr_from_unsigned(unsigned long long v, BSTR *out) {
  WCHAR text[24];
  const int n = cv_format_integer(v, FALSE, text);
  return cv_make_bstr(text, n, out);
}

/* @begin bstrs */
EXPORT HRESULT WINAPI VarBstrFromI1(CHAR in, LCID lcid, ULONG flags, BSTR *out) { (void)lcid; (void)flags; return cv_bstr_from_signed(in, out); }
EXPORT HRESULT WINAPI VarBstrFromI2(SHORT in, LCID lcid, ULONG flags, BSTR *out) { (void)lcid; (void)flags; return cv_bstr_from_signed(in, out); }
EXPORT HRESULT WINAPI VarBstrFromI4(LONG in, LCID lcid, ULONG flags, BSTR *out) { (void)lcid; (void)flags; return cv_bstr_from_signed(in, out); }
EXPORT HRESULT WINAPI VarBstrFromI8(LONG64 in, LCID lcid, ULONG flags, BSTR *out) { (void)lcid; (void)flags; return cv_bstr_from_signed(in, out); }
EXPORT HRESULT WINAPI VarBstrFromUI1(BYTE in, LCID lcid, ULONG flags, BSTR *out) { (void)lcid; (void)flags; return cv_bstr_from_unsigned(in, out); }
EXPORT HRESULT WINAPI VarBstrFromUI2(USHORT in, LCID lcid, ULONG flags, BSTR *out) { (void)lcid; (void)flags; return cv_bstr_from_unsigned(in, out); }
EXPORT HRESULT WINAPI VarBstrFromUI4(ULONG in, LCID lcid, ULONG flags, BSTR *out) { (void)lcid; (void)flags; return cv_bstr_from_unsigned(in, out); }
EXPORT HRESULT WINAPI VarBstrFromUI8(ULONG64 in, LCID lcid, ULONG flags, BSTR *out) { (void)lcid; (void)flags; return cv_bstr_from_unsigned(in, out); }
/* @end bstrs */

/* The significant digits of a non-negative finite double, rounded (halves up) to `ndigits`: the decimal digits of its exact value are made with a small base 10^9 big number
 * (the value is mantissa * 2^e2, or mantissa * 5^k / 10^k when e2 is negative). Writes at most ndigits characters without trailing zeros, returns their number, and the
 * decimal exponent of the first digit in *exponent. */
#define CV_LIMBS 100
static void cv_limbs_multiply(unsigned *limbs, int *count, unsigned long long factor) {
  unsigned long long carry = 0;
  for (int i = 0; i < *count; i++) {
    const unsigned long long cur = (unsigned long long)limbs[i] * factor + carry;
    limbs[i] = (unsigned)(cur % 1000000000ull);
    carry = cur / 1000000000ull;
  }
  while (carry && *count < CV_LIMBS) { limbs[(*count)++] = (unsigned)(carry % 1000000000ull); carry /= 1000000000ull; }
}

static int cv_real_digits(double value, int ndigits, char *out, int *exponent) {
  unsigned long long bits, mantissa;
  memcpy(&bits, &value, sizeof(bits));
  const int biased = (int)((bits >> 52) & 0x7FF);
  mantissa = bits & ((1ull << 52) - 1);
  int e2;
  if (biased == 0) e2 = -1074; else { mantissa |= 1ull << 52; e2 = biased - 1075; }
  if (!mantissa) { out[0] = '0'; *exponent = 0; return 1; }

  unsigned limbs[CV_LIMBS];
  int count = 0, places = 0;
  for (unsigned long long m = mantissa; m; m /= 1000000000ull) limbs[count++] = (unsigned)(m % 1000000000ull);
  if (e2 >= 0) {
    for (; e2 >= 28; e2 -= 28) cv_limbs_multiply(limbs, &count, 1ull << 28);
    if (e2) cv_limbs_multiply(limbs, &count, 1ull << e2);
  } else {
    places = -e2;
    int k = places;
    for (; k >= 12; k -= 12) cv_limbs_multiply(limbs, &count, 244140625ull);        /* 5^12 */
    unsigned long long five = 1;
    for (; k > 0; k--) five *= 5;
    if (five > 1) cv_limbs_multiply(limbs, &count, five);
  }
  /* the decimal digits of the big number */
  char all[CV_LIMBS * 9 + 1];
  int length = 0;
  for (int i = count - 1; i >= 0; i--) {
    char part[9];
    unsigned v = limbs[i];
    for (int j = 8; j >= 0; j--) { part[j] = (char)('0' + v % 10); v /= 10; }
    int start = 0;
    if (i == count - 1) while (start < 8 && part[start] == '0') start++;
    for (int j = start; j < 9; j++) all[length++] = part[j];
  }
  int exp10 = length - 1 - places;
  int keep = length < ndigits ? length : ndigits;
  for (int i = 0; i < keep; i++) out[i] = all[i];
  if (length > ndigits && all[ndigits] >= '5') {
    int i = keep - 1;
    while (i >= 0 && out[i] == '9') { out[i] = '0'; i--; }
    if (i >= 0) out[i]++;
    else { out[0] = '1'; for (int j = 1; j < keep; j++) out[j] = '0'; exp10++; }
  }
  while (keep > 1 && out[keep - 1] == '0') keep--;
  *exponent = exp10;
  return keep;
}

/* VB's text of a real number with `ndigits` significant digits: plain when the digits fit before and after the decimal point, otherwise 1.5E+20. */
static int cv_format_real(double value, int ndigits, WCHAR decimal, WCHAR *out) {
  int length = 0;
  char d[24] = { 0 };
  int e = 0;
  if (value < 0) { out[length++] = '-'; value = -value; }
  const int n = cv_real_digits(value, ndigits, d, &e);
  if (n == 1 && d[0] == '0') { out[length++] = '0'; out[length] = 0; return length; }
  if (e >= ndigits || (e < 0 && (-e - 1) + n > ndigits)) {
    out[length++] = (WCHAR)d[0];
    if (n > 1) { out[length++] = decimal; for (int i = 1; i < n; i++) out[length++] = (WCHAR)d[i]; }
    out[length++] = 'E';
    out[length++] = e < 0 ? '-' : '+';
    const int a = e < 0 ? -e : e;
    if (a >= 100) out[length++] = (WCHAR)('0' + a / 100);
    out[length++] = (WCHAR)('0' + a / 10 % 10);
    out[length++] = (WCHAR)('0' + a % 10);
  } else if (e >= 0) {
    for (int i = 0; i <= e; i++) out[length++] = i < n ? (WCHAR)d[i] : '0';
    if (n > e + 1) { out[length++] = decimal; for (int i = e + 1; i < n; i++) out[length++] = (WCHAR)d[i]; }
  } else {
    out[length++] = '0';
    out[length++] = decimal;
    for (int i = 0; i < -e - 1; i++) out[length++] = '0';
    for (int i = 0; i < n; i++) out[length++] = (WCHAR)d[i];
  }
  out[length] = 0;
  return length;
}

static HRESULT cv_bstr_from_real(double value, LCID lcid, ULONG flags, int ndigits, BSTR *out) {
  if (!out) return E_INVALIDARG;
  if (value != value || oa_fabs(value) > 1.7976931348623157e308) return DISP_E_OVERFLOW;
  WCHAR decimal[16];
  WCHAR text[48];
  if (!parse_locale_text(lcid, flags, LOCALE_SDECIMAL, decimal, 16)) { decimal[0] = '.'; decimal[1] = 0; }
  return cv_make_bstr(text, cv_format_real(value, ndigits, decimal[0], text), out);
}
EXPORT HRESULT WINAPI VarBstrFromR8(DOUBLE in, LCID lcid, ULONG flags, BSTR *out) { return cv_bstr_from_real(in, lcid, flags, 15, out); }
EXPORT HRESULT WINAPI VarBstrFromR4(FLOAT in, LCID lcid, ULONG flags, BSTR *out) { return cv_bstr_from_real((double)in, lcid, flags, 7, out); }

EXPORT HRESULT WINAPI VarBstrFromBool(VARIANT_BOOL in, LCID lcid, ULONG flags, BSTR *out) {
  if (!out) return E_INVALIDARG;
  const CV_BOOL_TEXT *text = (flags & VAR_LOCALBOOL) ? cv_bool_text(cv_primary_language(lcid)) : &cv_bool_texts[0];
  const WCHAR *word = in ? text->yes : text->no;
  return cv_make_bstr(word, (int)oa_wcslen(word), out);
}

/* ---- IDispatch: the value is the default property ------------------------------------------------------------------------------------------------- */

static const GUID cv_null_iid = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };
static HRESULT cv_from_disp(IDispatch *disp, LCID lcid, VARTYPE vt, void *out) {
  if (!disp) return DISP_E_BADVARTYPE;
  DISPPARAMS none = { NULL, NULL, 0, 0 };
  VARIANT source, dest;
  VariantInit(&source);
  VariantInit(&dest);
  HRESULT hr = IDispatch_Invoke(disp, DISPID_VALUE, &cv_null_iid, lcid, DISPATCH_PROPERTYGET, &none, &source, NULL, NULL);
  if (FAILED(hr)) return DISP_E_TYPEMISMATCH;
  hr = VariantChangeTypeEx(&dest, &source, lcid, 0, vt);
  VariantClear(&source);
  if (FAILED(hr)) return hr;
  switch (vt) {
    case VT_I1: case VT_UI1: *(BYTE *)out = V_UI1(&dest); break;
    case VT_I2: case VT_UI2: case VT_BOOL: *(USHORT *)out = V_UI2(&dest); break;
    case VT_I4: case VT_UI4: case VT_R4: *(ULONG *)out = V_UI4(&dest); break;
    case VT_I8: case VT_UI8: case VT_R8: case VT_DATE: case VT_CY: *(ULONG64 *)out = V_UI8(&dest); break;
    case VT_DECIMAL: *(DECIMAL *)out = V_DECIMAL(&dest); break;
    case VT_BSTR: *(BSTR *)out = V_BSTR(&dest); return S_OK;           /* (the string now belongs to the caller) */
    default: break;
  }
  VariantClear(&dest);
  return S_OK;
}

/* @begin disps */
EXPORT HRESULT WINAPI VarI1FromDisp(IDispatch *disp, LCID lcid, CHAR *out) { return cv_from_disp(disp, lcid, VT_I1, out); }
EXPORT HRESULT WINAPI VarUI1FromDisp(IDispatch *disp, LCID lcid, BYTE *out) { return cv_from_disp(disp, lcid, VT_UI1, out); }
EXPORT HRESULT WINAPI VarI2FromDisp(IDispatch *disp, LCID lcid, SHORT *out) { return cv_from_disp(disp, lcid, VT_I2, out); }
EXPORT HRESULT WINAPI VarUI2FromDisp(IDispatch *disp, LCID lcid, USHORT *out) { return cv_from_disp(disp, lcid, VT_UI2, out); }
EXPORT HRESULT WINAPI VarI4FromDisp(IDispatch *disp, LCID lcid, LONG *out) { return cv_from_disp(disp, lcid, VT_I4, out); }
EXPORT HRESULT WINAPI VarUI4FromDisp(IDispatch *disp, LCID lcid, ULONG *out) { return cv_from_disp(disp, lcid, VT_UI4, out); }
EXPORT HRESULT WINAPI VarI8FromDisp(IDispatch *disp, LCID lcid, LONG64 *out) { return cv_from_disp(disp, lcid, VT_I8, out); }
EXPORT HRESULT WINAPI VarUI8FromDisp(IDispatch *disp, LCID lcid, ULONG64 *out) { return cv_from_disp(disp, lcid, VT_UI8, out); }
EXPORT HRESULT WINAPI VarR4FromDisp(IDispatch *disp, LCID lcid, FLOAT *out) { return cv_from_disp(disp, lcid, VT_R4, out); }
EXPORT HRESULT WINAPI VarR8FromDisp(IDispatch *disp, LCID lcid, DOUBLE *out) { return cv_from_disp(disp, lcid, VT_R8, out); }
EXPORT HRESULT WINAPI VarDateFromDisp(IDispatch *disp, LCID lcid, DATE *out) { return cv_from_disp(disp, lcid, VT_DATE, out); }
EXPORT HRESULT WINAPI VarBoolFromDisp(IDispatch *disp, LCID lcid, VARIANT_BOOL *out) { return cv_from_disp(disp, lcid, VT_BOOL, out); }
/* @end disps */
EXPORT HRESULT WINAPI VarBstrFromDisp(IDispatch *disp, LCID lcid, ULONG flags, BSTR *out) {
  (void)flags;
  return cv_from_disp(disp, lcid, VT_BSTR, out);
}

/* ---- VariantChangeType ------------------------------------------------------------------------------------------------------------------------------ */

static DWORD cv_locale_setting;
EXPORT HRESULT WINAPI GetVarConversionLocaleSetting(DWORD *value) {
  if (!value) return E_POINTER;
  *value = cv_locale_setting;
  return S_OK;
}
EXPORT HRESULT WINAPI SetVarConversionLocaleSetting(DWORD value) {
  if (value != 0 && value != LOCALE_USE_NLS) return E_INVALIDARG;
  cv_locale_setting = value;
  return S_OK;
}

#define CV_SOURCES_NUMERIC(TN, TF) \
  case VT_I1: return Var##TN##FromI1(V_I1(ps), &V_##TF(pd)); \
  case VT_UI1: return Var##TN##FromUI1(V_UI1(ps), &V_##TF(pd)); \
  case VT_I2: return Var##TN##FromI2(V_I2(ps), &V_##TF(pd)); \
  case VT_UI2: return Var##TN##FromUI2(V_UI2(ps), &V_##TF(pd)); \
  case VT_I4: return Var##TN##FromI4(V_I4(ps), &V_##TF(pd)); \
  case VT_UI4: return Var##TN##FromUI4(V_UI4(ps), &V_##TF(pd)); \
  case VT_I8: return Var##TN##FromI8(V_I8(ps), &V_##TF(pd)); \
  case VT_UI8: return Var##TN##FromUI8(V_UI8(ps), &V_##TF(pd)); \
  case VT_R4: return Var##TN##FromR4(V_R4(ps), &V_##TF(pd)); \
  case VT_R8: return Var##TN##FromR8(V_R8(ps), &V_##TF(pd)); \
  case VT_DATE: return Var##TN##FromDate(V_DATE(ps), &V_##TF(pd)); \
  case VT_BOOL: return Var##TN##FromBool(V_BOOL(ps), &V_##TF(pd)); \
  case VT_CY: return Var##TN##FromCy(V_CY(ps), &V_##TF(pd)); \
  case VT_DECIMAL: return Var##TN##FromDec(&V_DECIMAL(ps), &V_##TF(pd)); \
  case VT_BSTR: return Var##TN##FromStr(V_BSTR(ps), lcid, dw, &V_##TF(pd)); \
  case VT_DISPATCH: return Var##TN##FromDisp(V_DISPATCH(ps), lcid, &V_##TF(pd));

#define CV_TARGET(NAME, TN, TF) \
  static HRESULT NAME(VARIANT *pd, VARIANT *ps, VARTYPE from, LCID lcid, ULONG dw) { \
    (void)lcid; (void)dw; \
    switch (from) { CV_SOURCES_NUMERIC(TN, TF) default: return DISP_E_TYPEMISMATCH; } }
CV_TARGET(cv_to_i1, I1, I1) CV_TARGET(cv_to_ui1, UI1, UI1) CV_TARGET(cv_to_i2, I2, I2) CV_TARGET(cv_to_ui2, UI2, UI2) CV_TARGET(cv_to_i4, I4, I4) CV_TARGET(cv_to_ui4, UI4, UI4)
CV_TARGET(cv_to_i8, I8, I8) CV_TARGET(cv_to_ui8, UI8, UI8) CV_TARGET(cv_to_r4, R4, R4) CV_TARGET(cv_to_r8, R8, R8) CV_TARGET(cv_to_date, Date, DATE) CV_TARGET(cv_to_bool, Bool, BOOL)
CV_TARGET(cv_to_cy, Cy, CY) CV_TARGET(cv_to_dec, Dec, DECIMAL)

static HRESULT cv_to_bstr(VARIANT *pd, VARIANT *ps, VARTYPE from, LCID lcid, ULONG dw, USHORT flags) {
  BSTR *out = &V_BSTR(pd);
  switch (from) {
    case VT_I1: return VarBstrFromI1(V_I1(ps), lcid, dw, out);
    case VT_UI1: return VarBstrFromUI1(V_UI1(ps), lcid, dw, out);
    case VT_I2: return VarBstrFromI2(V_I2(ps), lcid, dw, out);
    case VT_UI2: return VarBstrFromUI2(V_UI2(ps), lcid, dw, out);
    case VT_I4: return VarBstrFromI4(V_I4(ps), lcid, dw, out);
    case VT_UI4: return VarBstrFromUI4(V_UI4(ps), lcid, dw, out);
    case VT_I8: return VarBstrFromI8(V_I8(ps), lcid, dw, out);
    case VT_UI8: return VarBstrFromUI8(V_UI8(ps), lcid, dw, out);
    case VT_R4: return VarBstrFromR4(V_R4(ps), lcid, dw, out);
    case VT_R8: return VarBstrFromR8(V_R8(ps), lcid, dw, out);
    case VT_DATE: return VarBstrFromDate(V_DATE(ps), lcid, dw, out);
    case VT_BOOL:
      /* "True" and "False" only on request; otherwise the number */
      if (flags & (VARIANT_ALPHABOOL | VARIANT_LOCALBOOL)) return VarBstrFromBool(V_BOOL(ps), lcid, (flags & VARIANT_LOCALBOOL) ? dw | VAR_LOCALBOOL : dw, out);
      return VarBstrFromI2(V_BOOL(ps), lcid, dw, out);
    case VT_CY: return VarBstrFromCy(V_CY(ps), lcid, dw, out);
    case VT_DECIMAL: return VarBstrFromDec(&V_DECIMAL(ps), lcid, dw, out);
    case VT_DISPATCH: return VarBstrFromDisp(V_DISPATCH(ps), lcid, dw, out);
    default: return DISP_E_TYPEMISMATCH;
  }
}

/* A VARTYPE that VariantChangeType accepts as a source or a target (CLSID is not one). */
static BOOL cv_valid_type(VARTYPE vt) { return variant_valid_type(vt) && (vt & VT_TYPEMASK) != VT_CLSID; }

/* A byte array and a string hold the same bytes. */
static HRESULT cv_bstr_from_bytes(SAFEARRAY *array, BSTR *out) {
  if (!array) { *out = NULL; return S_OK; }
  if (SafeArrayGetDim(array) != 1 || array->cbElements != 1) return DISP_E_TYPEMISMATCH;
  void *data;
  HRESULT hr = SafeArrayAccessData(array, &data);
  if (FAILED(hr)) return hr;
  *out = SysAllocStringByteLen((LPCSTR)data, array->rgsabound[0].cElements);
  SafeArrayUnaccessData(array);
  return *out ? S_OK : E_OUTOFMEMORY;
}
static HRESULT cv_bytes_from_bstr(BSTR string, SAFEARRAY **out) {
  const UINT length = SysStringByteLen(string);
  SAFEARRAY *array = SafeArrayCreateVector(VT_UI1, 0, length);
  if (!array) return E_OUTOFMEMORY;
  if (length) {
    void *data;
    HRESULT hr = SafeArrayAccessData(array, &data);
    if (FAILED(hr)) { SafeArrayDestroy(array); return hr; }
    memcpy(data, string, length);
    SafeArrayUnaccessData(array);
  }
  *out = array;
  return S_OK;
}

/* Converts `ps` (never by reference) into `pd` (empty): the result has type `vt` on success. */
static HRESULT cv_coerce(VARIANT *pd, LCID lcid, USHORT flags, VARIANT *ps, VARTYPE vt) {
  VARTYPE from = V_VT(ps);
  ULONG dw = 0;
  if (flags & VARIANT_NOUSEROVERRIDE) dw |= LOCALE_NOUSEROVERRIDE;
  if (flags & VARIANT_USE_NLS) dw |= LOCALE_USE_NLS;
  if (flags & VARIANT_LOCALBOOL) dw |= VAR_LOCALBOOL;

  if (from == vt) return VariantCopy(pd, ps);
  if ((from & VT_ARRAY) || (vt & VT_ARRAY)) {
    if (vt == VT_BSTR && from == (VT_ARRAY | VT_UI1)) { HRESULT hr = cv_bstr_from_bytes(V_ARRAY(ps), &V_BSTR(pd)); if (hr == S_OK) V_VT(pd) = vt; return hr; }
    if (vt == (VT_ARRAY | VT_UI1) && from == VT_BSTR) { HRESULT hr = cv_bytes_from_bstr(V_BSTR(ps), &V_ARRAY(pd)); if (hr == S_OK) V_VT(pd) = vt; return hr; }
    return DISP_E_TYPEMISMATCH;
  }
  if ((vt & VT_BYREF) || (from & VT_BYREF)) return DISP_E_TYPEMISMATCH;

  if (vt == VT_EMPTY || vt == VT_NULL) {
    if (from == VT_NULL || from == VT_ERROR) return DISP_E_TYPEMISMATCH;
    V_VT(pd) = vt;
    return S_OK;
  }
  if (from == VT_NULL || from == VT_ERROR || from == VT_VARIANT || from == VT_RECORD) return DISP_E_TYPEMISMATCH;
  if (vt == VT_ERROR || vt == VT_VARIANT || vt == VT_RECORD) return DISP_E_TYPEMISMATCH;

  if (vt == VT_UNKNOWN || vt == VT_DISPATCH) {
    if (from != VT_UNKNOWN && from != VT_DISPATCH) return DISP_E_TYPEMISMATCH;
    IUnknown *object = from == VT_UNKNOWN ? V_UNKNOWN(ps) : (IUnknown *)V_DISPATCH(ps);
    void *result = NULL;
    if (object) {
      HRESULT hr = IUnknown_QueryInterface(object, vt == VT_UNKNOWN ? &IID_IUnknown : &IID_IDispatch, &result);
      if (FAILED(hr)) return hr;
    }
    if (vt == VT_UNKNOWN) V_UNKNOWN(pd) = result; else V_DISPATCH(pd) = result;
    V_VT(pd) = vt;
    return S_OK;
  }
  if (from == VT_UNKNOWN) return DISP_E_TYPEMISMATCH;
  if (from == VT_DISPATCH && (flags & VARIANT_NOVALUEPROP)) return DISP_E_TYPEMISMATCH;

  /* a plain EMPTY is zero (or the empty string) */
  if (from == VT_EMPTY) {
    switch (vt) {
      case VT_BSTR: V_BSTR(pd) = SysAllocStringLen(NULL, 0); if (!V_BSTR(pd)) return E_OUTOFMEMORY; break;
      case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_I8: case VT_UI8:
      case VT_R4: case VT_R8: case VT_DATE: case VT_BOOL: case VT_CY: break;
      case VT_DECIMAL: memset(&V_DECIMAL(pd), 0, sizeof(DECIMAL)); break;
      default: return DISP_E_TYPEMISMATCH;
    }
    V_VT(pd) = vt;
    return S_OK;
  }

  /* INT and UINT are I4 and UI4; between the signed and the unsigned type of one size the bits are kept (-1 becomes 255 as a byte) */
  VARTYPE target = vt == VT_INT ? VT_I4 : vt == VT_UINT ? VT_UI4 : vt;
  if (from == VT_INT) from = VT_I4;
  if (from == VT_UINT) from = VT_UI4;
  if ((from == VT_I1 && target == VT_UI1) || (from == VT_UI1 && target == VT_I1)) { V_UI1(pd) = V_UI1(ps); V_VT(pd) = vt; return S_OK; }
  if ((from == VT_I2 && target == VT_UI2) || (from == VT_UI2 && target == VT_I2)) { V_UI2(pd) = V_UI2(ps); V_VT(pd) = vt; return S_OK; }
  if ((from == VT_I4 && target == VT_UI4) || (from == VT_UI4 && target == VT_I4)) { V_UI4(pd) = V_UI4(ps); V_VT(pd) = vt; return S_OK; }
  if (from == target) return VariantCopy(pd, ps) == S_OK ? (V_VT(pd) = vt, S_OK) : DISP_E_TYPEMISMATCH;

  HRESULT hr;
  /* a BOOL is the number it holds (not -1) for CY and DECIMAL */
  if (from == VT_BOOL && (target == VT_CY || target == VT_DECIMAL)) {
    VARIANT number;
    VariantInit(&number);
    V_VT(&number) = VT_I2;
    V_I2(&number) = V_BOOL(ps);
    hr = cv_coerce(pd, lcid, flags, &number, target);
    return hr;
  }
  switch (target) {
    case VT_I1: hr = cv_to_i1(pd, ps, from, lcid, dw); break;
    case VT_UI1: hr = cv_to_ui1(pd, ps, from, lcid, dw); break;
    case VT_I2: hr = cv_to_i2(pd, ps, from, lcid, dw); break;
    case VT_UI2: hr = cv_to_ui2(pd, ps, from, lcid, dw); break;
    case VT_I4: hr = cv_to_i4(pd, ps, from, lcid, dw); break;
    case VT_UI4: hr = cv_to_ui4(pd, ps, from, lcid, dw); break;
    case VT_I8: hr = cv_to_i8(pd, ps, from, lcid, dw); break;
    case VT_UI8: hr = cv_to_ui8(pd, ps, from, lcid, dw); break;
    case VT_R4: hr = cv_to_r4(pd, ps, from, lcid, dw); break;
    case VT_R8: hr = cv_to_r8(pd, ps, from, lcid, dw); break;
    case VT_DATE: hr = cv_to_date(pd, ps, from, lcid, dw); break;
    case VT_BOOL: hr = cv_to_bool(pd, ps, from, lcid, dw); break;
    case VT_CY: hr = cv_to_cy(pd, ps, from, lcid, dw); break;
    case VT_DECIMAL: hr = cv_to_dec(pd, ps, from, lcid, dw); break;
    case VT_BSTR: hr = cv_to_bstr(pd, ps, from, lcid, dw, flags); break;
    default: return DISP_E_TYPEMISMATCH;
  }
  if (SUCCEEDED(hr)) V_VT(pd) = vt;
  return hr;
}

EXPORT HRESULT WINAPI VariantChangeTypeEx(VARIANTARG *dest, VARIANTARG *src, LCID lcid, USHORT flags, VARTYPE vt) {
  if (!dest || !src) return E_INVALIDARG;
  if (!cv_valid_type(vt)) return DISP_E_BADVARTYPE;
  if ((V_VT(src) & VT_BYREF) && !V_BYREF(src)) return DISP_E_TYPEMISMATCH;
  if (!cv_valid_type(V_VT(src))) return DISP_E_BADVARTYPE;

  VARIANT value, result;
  VariantInit(&value);
  VariantInit(&result);
  VARIANT *ps = src;
  if (V_VT(src) & VT_BYREF) {
    HRESULT hr = VariantCopyInd(&value, src);
    if (FAILED(hr)) return hr;
    ps = &value;
  }
  HRESULT hr = cv_coerce(&result, lcid, flags, ps, vt);
  if (SUCCEEDED(hr)) {
    /* the destination is replaced only when the result is ready (it may be the source) */
    hr = VariantClear(dest);
    if (SUCCEEDED(hr)) *dest = result; else VariantClear(&result);
  } else VariantClear(&result);
  if (ps == &value) VariantClear(&value);
  return hr;
}

EXPORT HRESULT WINAPI VariantChangeType(VARIANTARG *dest, VARIANTARG *src, USHORT flags, VARTYPE vt) {
  return VariantChangeTypeEx(dest, src, GetUserDefaultLCID(), flags, vt);
}
