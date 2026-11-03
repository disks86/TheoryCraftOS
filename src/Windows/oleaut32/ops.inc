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

/* Variant arithmetic and comparison: VarAdd VarSub VarMul VarDiv VarIdiv VarMod VarPow VarAnd VarOr VarXor VarEqv VarImp VarNot VarNeg VarAbs VarInt VarFix VarRound
 * VarCmp VarCat VarBstrCat VarBstrCmp. The result type of each operation follows the tables of Windows' oleaut32 (see Wine's tests/vartest.c): the operands are
 * promoted to a common type, integer results grow to the next larger type instead of wrapping, strings are converted with the VarXFromStr functions, NULL propagates.
 * A failing operation leaves V_VT(result) at VT_EMPTY. Operands by reference (VT_BYREF) and arrays are not accepted, as in Windows. */

#define OPS_LCID LOCALE_USER_DEFAULT

static const GUID ops_iid_null = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

/* The value of an IDispatch's default property (what Visual Basic does with an object used as a value). */
static HRESULT ops_dispatch_value(IDispatch *dispatch, VARIANT *out) {
  DISPPARAMS params = { NULL, NULL, 0, 0 };
  VariantInit(out);
  if (!dispatch) return DISP_E_TYPEMISMATCH;
  HRESULT hr = IDispatch_Invoke(dispatch, DISPID_VALUE, &ops_iid_null, OPS_LCID, DISPATCH_PROPERTYGET, &params, out, NULL, NULL);
  if (FAILED(hr)) { VariantClear(out); return hr; }
  if (V_VT(out) & VT_BYREF) {
    VARIANT copy;
    VariantInit(&copy);
    hr = VariantCopyInd(&copy, out);
    VariantClear(out);
    if (FAILED(hr)) return hr;
    *out = copy;
  }
  return S_OK;
}

/* ---- operand values: a plain VARIANT (no flags, no VT_DISPATCH) converted to a C number -------------------------------------------------------------------- */

static HRESULT ops_get_r8(const VARIANT *v, BOOL exact_r4, double *out) {
  switch (V_VT(v)) {
    case VT_EMPTY: *out = 0.0; return S_OK;
    case VT_I1: *out = (double)(signed char)V_I1(v); return S_OK;
    case VT_UI1: *out = (double)V_UI1(v); return S_OK;
    case VT_I2: case VT_BOOL: *out = (double)V_I2(v); return S_OK;
    case VT_UI2: *out = (double)V_UI2(v); return S_OK;
    case VT_I4: case VT_INT: *out = (double)V_I4(v); return S_OK;
    case VT_UI4: case VT_UINT: *out = (double)V_UI4(v); return S_OK;
    case VT_I8: return VarR8FromI8(V_I8(v), out);
    case VT_UI8: return VarR8FromUI8(V_UI8(v), out);
    case VT_R4: if (exact_r4) { *out = (double)V_R4(v); return S_OK; } return VarR8FromR4(V_R4(v), out);
    case VT_R8: case VT_DATE: *out = V_R8(v); return S_OK;
    case VT_CY: return VarR8FromCy(V_CY(v), out);
    case VT_DECIMAL: { DECIMAL d = V_DECIMAL(v); return VarR8FromDec(&d, out); }
    case VT_BSTR: return V_BSTR(v) ? VarR8FromStr(V_BSTR(v), OPS_LCID, 0, out) : DISP_E_TYPEMISMATCH;
    default: return DISP_E_TYPEMISMATCH;
  }
}

/* wrap: an unsigned 32 bit value is taken bit for bit (the logical operators); otherwise it must fit. */
static HRESULT ops_get_i4(const VARIANT *v, BOOL wrap, LONG *out) {
  switch (V_VT(v)) {
    case VT_EMPTY: *out = 0; return S_OK;
    case VT_I1: *out = (signed char)V_I1(v); return S_OK;
    case VT_UI1: *out = V_UI1(v); return S_OK;
    case VT_I2: case VT_BOOL: *out = V_I2(v); return S_OK;
    case VT_UI2: *out = V_UI2(v); return S_OK;
    case VT_I4: case VT_INT: *out = V_I4(v); return S_OK;
    case VT_UI4: case VT_UINT: if (wrap) { *out = (LONG)V_UI4(v); return S_OK; } return VarI4FromUI4(V_UI4(v), out);
    case VT_I8: return VarI4FromI8(V_I8(v), out);
    case VT_UI8: return VarI4FromUI8(V_UI8(v), out);
    case VT_R4: return VarI4FromR4(V_R4(v), out);
    case VT_R8: return VarI4FromR8(V_R8(v), out);
    case VT_DATE: return VarI4FromDate(V_DATE(v), out);
    case VT_CY: return VarI4FromCy(V_CY(v), out);
    case VT_DECIMAL: { DECIMAL d = V_DECIMAL(v); return VarI4FromDec(&d, out); }
    case VT_BSTR: return V_BSTR(v) ? VarI4FromStr(V_BSTR(v), OPS_LCID, 0, out) : DISP_E_TYPEMISMATCH;
    default: return DISP_E_TYPEMISMATCH;
  }
}

static HRESULT ops_get_i8(const VARIANT *v, LONG64 *out) {
  switch (V_VT(v)) {
    case VT_EMPTY: *out = 0; return S_OK;
    case VT_I1: *out = (signed char)V_I1(v); return S_OK;
    case VT_UI1: *out = V_UI1(v); return S_OK;
    case VT_I2: case VT_BOOL: *out = V_I2(v); return S_OK;
    case VT_UI2: *out = V_UI2(v); return S_OK;
    case VT_I4: case VT_INT: *out = V_I4(v); return S_OK;
    case VT_UI4: case VT_UINT: *out = V_UI4(v); return S_OK;
    case VT_I8: *out = V_I8(v); return S_OK;
    case VT_UI8: return VarI8FromUI8(V_UI8(v), out);
    case VT_R4: return VarI8FromR4(V_R4(v), out);
    case VT_R8: return VarI8FromR8(V_R8(v), out);
    case VT_DATE: return VarI8FromDate(V_DATE(v), out);
    case VT_CY: return VarI8FromCy(V_CY(v), out);
    case VT_DECIMAL: { DECIMAL d = V_DECIMAL(v); return VarI8FromDec(&d, out); }
    case VT_BSTR: return V_BSTR(v) ? VarI8FromStr(V_BSTR(v), OPS_LCID, 0, out) : DISP_E_TYPEMISMATCH;
    default: return DISP_E_TYPEMISMATCH;
  }
}

static HRESULT ops_get_cy(const VARIANT *v, CY *out) {
  switch (V_VT(v)) {
    case VT_EMPTY: out->int64 = 0; return S_OK;
    case VT_UI1: return VarCyFromUI1(V_UI1(v), out);
    case VT_I2: return VarCyFromI2(V_I2(v), out);
    case VT_BOOL: return VarCyFromBool(V_BOOL(v), out);
    case VT_I4: case VT_INT: return VarCyFromI4(V_I4(v), out);
    case VT_I8: return VarCyFromI8(V_I8(v), out);
    case VT_R4: return VarCyFromR4(V_R4(v), out);
    case VT_R8: return VarCyFromR8(V_R8(v), out);
    case VT_DATE: return VarCyFromDate(V_DATE(v), out);
    case VT_CY: *out = V_CY(v); return S_OK;
    case VT_DECIMAL: { DECIMAL d = V_DECIMAL(v); return VarCyFromDec(&d, out); }
    case VT_BSTR: return V_BSTR(v) ? VarCyFromStr(V_BSTR(v), OPS_LCID, 0, out) : DISP_E_TYPEMISMATCH;
    default: return DISP_E_TYPEMISMATCH;
  }
}

static HRESULT ops_get_dec(const VARIANT *v, DECIMAL *out) {
  switch (V_VT(v)) {
    case VT_EMPTY: return VarDecFromI4(0, out);
    case VT_UI1: return VarDecFromUI1(V_UI1(v), out);
    case VT_I2: return VarDecFromI2(V_I2(v), out);
    case VT_BOOL: return VarDecFromBool(V_BOOL(v), out);
    case VT_I4: case VT_INT: return VarDecFromI4(V_I4(v), out);
    case VT_I8: return VarDecFromI8(V_I8(v), out);
    case VT_R4: return VarDecFromR4(V_R4(v), out);
    case VT_R8: return VarDecFromR8(V_R8(v), out);
    case VT_DATE: return VarDecFromDate(V_DATE(v), out);
    case VT_CY: return VarDecFromCy(V_CY(v), out);
    case VT_DECIMAL: *out = V_DECIMAL(v); return S_OK;
    case VT_BSTR: return V_BSTR(v) ? VarDecFromStr(V_BSTR(v), OPS_LCID, 0, out) : DISP_E_TYPEMISMATCH;
    default: return DISP_E_TYPEMISMATCH;
  }
}

/* A string used as a number: its value, else (as the logical operators do) "#TRUE#" and "#FALSE#" as a VT_BOOL. */
static HRESULT ops_bstr_number(BSTR s, VARIANT *out) {
  double d;
  VARIANT_BOOL b;
  if (!s) return DISP_E_TYPEMISMATCH;
  HRESULT hr = VarR8FromStr(s, OPS_LCID, 0, &d);
  if (SUCCEEDED(hr)) { V_VT(out) = VT_R8; V_R8(out) = d; return S_OK; }
  if (hr == DISP_E_OVERFLOW) return hr;
  hr = VarBoolFromStr(s, OPS_LCID, 0, &b);
  if (SUCCEEDED(hr)) { V_VT(out) = VT_BOOL; V_BOOL(out) = b; return S_OK; }
  return DISP_E_TYPEMISMATCH;
}

/* Is the integer v a value of the (signed) type vt? Then store it. */
static BOOL ops_store_int(VARIANT *out, LONG64 v, VARTYPE vt) {
  switch (vt) {
    case VT_UI1: if (v >= 0 && v <= 255) { V_UI1(out) = (BYTE)v; V_VT(out) = VT_UI1; return TRUE; } return FALSE;
    case VT_I2: if (v >= -32768 && v <= 32767) { V_I2(out) = (SHORT)v; V_VT(out) = VT_I2; return TRUE; } return FALSE;
    case VT_I4: if (v >= -2147483647 - 1 && v <= 2147483647) { V_I4(out) = (LONG)v; V_VT(out) = VT_I4; return TRUE; } return FALSE;
    default: return FALSE;
  }
}

/* An integer result that did not fit its type: the next larger one (UI1 -> I2 -> I4 -> R8). */
static void ops_store_grow(VARIANT *out, LONG64 v, VARTYPE vt) {
  if (vt == VT_UI1 && ops_store_int(out, v, VT_UI1)) return;
  if ((vt == VT_UI1 || vt == VT_I2) && ops_store_int(out, v, VT_I2)) return;
  if (ops_store_int(out, v, VT_I4)) return;
  V_R8(out) = (double)v;
  V_VT(out) = VT_R8;
}

static BOOL ops_is_inf_or_nan(double d) { return d != d || d == __builtin_huge_val() || d == -__builtin_huge_val(); }

/* ---- + - * / ------------------------------------------------------------------------------------------------------------------------------------------- */

enum { OPS_ADD, OPS_SUB, OPS_MUL, OPS_DIV };

static BOOL ops_arith_vt_ok(VARTYPE t) {
  switch (t) {
    case VT_EMPTY: case VT_NULL: case VT_I2: case VT_I4: case VT_R4: case VT_R8: case VT_CY: case VT_DATE: case VT_BSTR: case VT_DISPATCH: case VT_ERROR: case VT_BOOL:
    case VT_DECIMAL: case VT_UI1: case VT_I8: return TRUE;
    default: return FALSE;
  }
}

/* Errors by type: flags (an array is a type mismatch), then the types Windows cannot add (I1, UI2, UI4, INT, UINT, UI8, UNKNOWN, VARIANT, ...). */
static HRESULT ops_arith_validate(VARTYPE lf, VARTYPE rf) {
  const VARTYPE l = lf & VT_TYPEMASK, r = rf & VT_TYPEMASK;
  if ((lf | rf) & ~VT_TYPEMASK) return ((lf | rf) & VT_ARRAY) ? DISP_E_TYPEMISMATCH : DISP_E_BADVARTYPE;
  if (ops_arith_vt_ok(l) && ops_arith_vt_ok(r)) return S_OK;
  if (r == VT_RECORD) return DISP_E_TYPEMISMATCH;
  if (l == VT_RECORD) return (r <= VT_UI1 || r == VT_I8) ? DISP_E_TYPEMISMATCH : DISP_E_BADVARTYPE;
  return DISP_E_BADVARTYPE;
}

#define OPS_HAS(t) (l == (t) || r == (t))

/* The result type for operand types that are valid, not NULL and not ERROR. */
static VARTYPE ops_arith_type(int op, VARTYPE l, VARTYPE r) {
  switch (op) {
    case OPS_ADD:
      if (OPS_HAS(VT_DECIMAL)) return VT_DECIMAL;
      if (OPS_HAS(VT_DATE)) return VT_DATE;
      if (OPS_HAS(VT_CY)) return VT_CY;
      if (OPS_HAS(VT_R8)) return VT_R8;
      if (OPS_HAS(VT_BSTR)) return ((l == VT_BSTR && r == VT_BSTR) || OPS_HAS(VT_EMPTY)) ? VT_BSTR : VT_R8;
      if (OPS_HAS(VT_R4)) return (OPS_HAS(VT_I4) || OPS_HAS(VT_I8)) ? VT_R8 : VT_R4;
      break;
    case OPS_SUB:
      if ((l == VT_EMPTY && r == VT_BSTR) || (l == VT_DATE && r == VT_DATE) || (l == VT_BSTR && r == VT_EMPTY) || (l == VT_BSTR && r == VT_BSTR)) return VT_R8;
      if (OPS_HAS(VT_DECIMAL)) return VT_DECIMAL;
      if (OPS_HAS(VT_DATE)) return VT_DATE;
      if (OPS_HAS(VT_CY)) return VT_CY;
      if (OPS_HAS(VT_R8) || OPS_HAS(VT_BSTR)) return VT_R8;
      if (OPS_HAS(VT_R4)) return (OPS_HAS(VT_I4) || OPS_HAS(VT_I8)) ? VT_R8 : VT_R4;
      break;
    case OPS_MUL:
      if (OPS_HAS(VT_DECIMAL)) return VT_DECIMAL;
      if (OPS_HAS(VT_R8) || OPS_HAS(VT_BSTR) || OPS_HAS(VT_DATE)) return VT_R8;
      if (OPS_HAS(VT_R4)) return (OPS_HAS(VT_I4) || OPS_HAS(VT_I8) || OPS_HAS(VT_CY)) ? VT_R8 : VT_R4;
      if (OPS_HAS(VT_CY)) return VT_CY;
      break;
    default:      /* OPS_DIV */
      if (OPS_HAS(VT_DECIMAL)) return VT_DECIMAL;
      if (OPS_HAS(VT_I8) || OPS_HAS(VT_CY) || OPS_HAS(VT_DATE) || OPS_HAS(VT_I4) || OPS_HAS(VT_BSTR) || OPS_HAS(VT_I2) || OPS_HAS(VT_BOOL) || OPS_HAS(VT_R8) || OPS_HAS(VT_UI1)) {
        if ((l == VT_UI1 && r == VT_R4) || (l == VT_R4 && r == VT_UI1)) return VT_R4;
        if ((l == VT_R4 && (r == VT_BOOL || r == VT_I2)) || (r == VT_R4 && (l == VT_BOOL || l == VT_I2))) return VT_R4;
        return VT_R8;
      }
      return OPS_HAS(VT_R4) ? VT_R4 : VT_R8;
  }
  if (OPS_HAS(VT_I8)) return VT_I8;
  if (OPS_HAS(VT_I4)) return VT_I4;
  if (OPS_HAS(VT_I2) || OPS_HAS(VT_BOOL) || (l == VT_EMPTY && r == VT_EMPTY)) return VT_I2;
  if (OPS_HAS(VT_UI1)) return VT_UI1;
  return VT_I2;
}

static HRESULT ops_dbl_op(int op, double a, double b, double *out) {
  switch (op) {
    case OPS_ADD: *out = a + b; break;
    case OPS_SUB: *out = a - b; break;
    case OPS_MUL: *out = a * b; break;
    default:
      if (b == 0.0) return a == 0.0 ? DISP_E_OVERFLOW : DISP_E_DIVBYZERO;
      *out = a / b;
      break;
  }
  return ops_is_inf_or_nan(*out) ? DISP_E_OVERFLOW : S_OK;
}

/* Operands are plain, valid, and neither NULL nor ERROR nor DISPATCH. */
static HRESULT ops_arith_core(int op, const VARIANT *left, const VARIANT *right, VARIANT *out) {
  const VARTYPE l = V_VT(left), r = V_VT(right), rt = ops_arith_type(op, l, r);
  HRESULT hr;
  VariantInit(out);
  if (op == OPS_DIV && r == VT_EMPTY) return DISP_E_DIVBYZERO;
  switch (rt) {
    case VT_DECIMAL: {
      DECIMAL a, b, res;
      hr = ops_get_dec(left, &a);
      if (SUCCEEDED(hr)) hr = ops_get_dec(right, &b);
      if (FAILED(hr)) return hr;
      hr = op == OPS_ADD ? VarDecAdd(&a, &b, &res) : op == OPS_SUB ? VarDecSub(&a, &b, &res) : op == OPS_MUL ? VarDecMul(&a, &b, &res) : VarDecDiv(&a, &b, &res);
      if (FAILED(hr)) return hr;
      V_DECIMAL(out) = res;
      V_VT(out) = VT_DECIMAL;      /* (the DECIMAL overlays the type field: set it last) */
      return S_OK;
    }
    case VT_CY: {
      CY a, b, res;
      hr = ops_get_cy(left, &a);
      if (SUCCEEDED(hr)) hr = ops_get_cy(right, &b);
      if (FAILED(hr)) return hr;
      hr = op == OPS_ADD ? VarCyAdd(a, b, &res) : op == OPS_SUB ? VarCySub(a, b, &res) : VarCyMul(a, b, &res);
      if (FAILED(hr)) return hr;
      V_CY(out) = res;
      V_VT(out) = VT_CY;
      return S_OK;
    }
    case VT_BSTR: {
      BSTR joined;
      hr = VarBstrCat(l == VT_BSTR ? V_BSTR(left) : NULL, r == VT_BSTR ? V_BSTR(right) : NULL, &joined);
      if (FAILED(hr)) return hr;
      V_BSTR(out) = joined;
      V_VT(out) = VT_BSTR;
      return S_OK;
    }
    case VT_R4: case VT_R8: case VT_DATE: {
      double a, b, res;
      hr = ops_get_r8(left, rt == VT_R4, &a);
      if (SUCCEEDED(hr)) hr = ops_get_r8(right, rt == VT_R4, &b);
      if (SUCCEEDED(hr)) hr = ops_dbl_op(op, a, b, &res);
      if (FAILED(hr)) return hr;
      if (rt == VT_R4) {
        const float f = (float)res;
        if (!ops_is_inf_or_nan((double)f)) { V_R4(out) = f; V_VT(out) = VT_R4; return S_OK; }      /* too large for a single: a double */
        V_R8(out) = res;
        V_VT(out) = VT_R8;
        return S_OK;
      }
      V_R8(out) = res;
      V_VT(out) = rt;
      return S_OK;
    }
    case VT_I8: {
      LONG64 a, b, res;
      hr = ops_get_i8(left, &a);
      if (SUCCEEDED(hr)) hr = ops_get_i8(right, &b);
      if (FAILED(hr)) return hr;
      if (op == OPS_ADD ? __builtin_add_overflow(a, b, &res) : op == OPS_SUB ? __builtin_sub_overflow(a, b, &res) : __builtin_mul_overflow(a, b, &res)) return DISP_E_OVERFLOW;
      V_I8(out) = res;
      V_VT(out) = VT_I8;
      return S_OK;
    }
    default: {      /* UI1, I2, I4 */
      LONG a, b;
      LONG64 res;
      hr = ops_get_i4(left, TRUE, &a);
      if (SUCCEEDED(hr)) hr = ops_get_i4(right, TRUE, &b);
      if (FAILED(hr)) return hr;
      res = op == OPS_ADD ? (LONG64)a + b : op == OPS_SUB ? (LONG64)a - b : (LONG64)a * b;
      ops_store_grow(out, res, rt);
      return S_OK;
    }
  }
}

static HRESULT ops_arith(int op, VARIANT *left, VARIANT *right, VARIANT *result) {
  VARIANT lt, rt, out;
  const VARIANT *l = left, *r = right;
  HRESULT hr;
  if (!left || !right || !result) return E_INVALIDARG;
  VariantInit(&lt);
  VariantInit(&rt);
  hr = ops_arith_validate(V_VT(left), V_VT(right));
  if (SUCCEEDED(hr)) {
    const VARTYPE lvt = V_VT(left), rvt = V_VT(right);
    if (lvt == VT_NULL || rvt == VT_NULL) {
      if (lvt == VT_ERROR || rvt == VT_ERROR) hr = DISP_E_TYPEMISMATCH;
      else { VariantInit(&out); V_VT(&out) = VT_NULL; *result = out; return S_OK; }
    } else if (lvt == VT_ERROR || rvt == VT_ERROR) hr = DISP_E_TYPEMISMATCH;
    else {
      if (lvt == VT_DISPATCH) { hr = ops_dispatch_value(V_DISPATCH(left), &lt); l = &lt; }
      if (SUCCEEDED(hr) && rvt == VT_DISPATCH) { hr = ops_dispatch_value(V_DISPATCH(right), &rt); r = &rt; }
      if (SUCCEEDED(hr)) {
        if (l != left || r != right) {      /* the values of objects: a NULL (or a type that cannot be used) now */
          hr = ops_arith_validate(V_VT(l), V_VT(r));
          if (SUCCEEDED(hr) && (V_VT(l) == VT_DISPATCH || V_VT(r) == VT_DISPATCH || V_VT(l) == VT_ERROR || V_VT(r) == VT_ERROR)) hr = DISP_E_TYPEMISMATCH;
          if (SUCCEEDED(hr) && (V_VT(l) == VT_NULL || V_VT(r) == VT_NULL)) { VariantClear(&lt); VariantClear(&rt); VariantInit(&out); V_VT(&out) = VT_NULL; *result = out; return S_OK; }
        }
        if (SUCCEEDED(hr)) hr = ops_arith_core(op, l, r, &out);
      }
    }
  }
  VariantClear(&lt);
  VariantClear(&rt);
  if (FAILED(hr)) { V_VT(result) = VT_EMPTY; return hr; }
  *result = out;
  return S_OK;
}

EXPORT HRESULT WINAPI VarAdd(LPVARIANT left, LPVARIANT right, LPVARIANT result) { return ops_arith(OPS_ADD, left, right, result); }
EXPORT HRESULT WINAPI VarSub(LPVARIANT left, LPVARIANT right, LPVARIANT result) { return ops_arith(OPS_SUB, left, right, result); }
EXPORT HRESULT WINAPI VarMul(LPVARIANT left, LPVARIANT right, LPVARIANT result) { return ops_arith(OPS_MUL, left, right, result); }
EXPORT HRESULT WINAPI VarDiv(LPVARIANT left, LPVARIANT right, LPVARIANT result) { return ops_arith(OPS_DIV, left, right, result); }

/* ---- \ Mod ^ -------------------------------------------------------------------------------------------------------------------------------------------- */

/* Windows' order of checks for integer division and Mod. kind: 0 not a type, 1 a number/string/object, 2 a type that is a mismatch (ERROR, VARIANT, UNKNOWN, RECORD). */
static int ops_int_kind(VARTYPE t) {
  if (t & ~VT_TYPEMASK) return 0;
  switch (t) {
    case VT_EMPTY: case VT_NULL: case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_I4: case VT_I8: case VT_UI4: case VT_UI8: case VT_INT: case VT_UINT: case VT_R4: case VT_R8:
    case VT_BOOL: case VT_DATE: case VT_CY: case VT_DECIMAL: case VT_BSTR: case VT_DISPATCH: return 1;
    case VT_ERROR: case VT_VARIANT: case VT_UNKNOWN: case VT_RECORD: return 2;
    default: return 0;
  }
}

static HRESULT ops_int_validate(VARTYPE lf, VARTYPE rf) {
  const int lk = ops_int_kind(lf), rk = ops_int_kind(rf);
  const BOOL lfound = lk != 0, rfound = rk != 0, lvalid = lk == 1;
  const VARTYPE l = lfound ? lf : 0xffff, r = rfound ? rf : 0xffff;
  if ((l == VT_I8 && r == VT_INT) || (l == VT_INT && r == VT_I8)) return DISP_E_TYPEMISMATCH;
  if ((l == VT_EMPTY && r == VT_NULL) || (l == VT_NULL && r == VT_EMPTY) || (l == VT_EMPTY && r == VT_CY)) return S_OK;
  if (l == VT_EMPTY && r == VT_RECORD) return DISP_E_TYPEMISMATCH;
  if (r == VT_EMPTY && lfound && lvalid) return DISP_E_DIVBYZERO;
  if (l == VT_ERROR || (r == VT_ERROR && lfound && lvalid)) return DISP_E_TYPEMISMATCH;
  if (l == VT_NULL && r == VT_NULL) return S_OK;
  if (l == VT_VARIANT || (r == VT_VARIANT && lfound && lvalid)) return DISP_E_TYPEMISMATCH;
  if (l == VT_NULL && r == VT_RECORD) return DISP_E_TYPEMISMATCH;
  if ((l == VT_I8 && r == VT_DECIMAL) || (l == VT_DECIMAL && r == VT_I8)) return S_OK;
  if (l == VT_UNKNOWN || (r == VT_UNKNOWN && lfound && lvalid)) return DISP_E_TYPEMISMATCH;
  if (l == VT_NULL && rfound) return S_OK;
  if (l == VT_RECORD) return DISP_E_TYPEMISMATCH;
  if (r == VT_RECORD && lvalid && lfound) return DISP_E_TYPEMISMATCH;
  if (l == VT_EMPTY && r == VT_EMPTY) return DISP_E_DIVBYZERO;
  if (!lfound || !rfound) return DISP_E_BADVARTYPE;
  if (r == VT_NULL && lvalid) return S_OK;
  return S_OK;
}

static VARTYPE ops_idiv_type(VARTYPE l, VARTYPE r) {
  if (OPS_HAS(VT_I8)) return VT_I8;
  if (OPS_HAS(VT_I4) || OPS_HAS(VT_INT) || OPS_HAS(VT_UINT) || OPS_HAS(VT_UI8) || OPS_HAS(VT_UI4) || OPS_HAS(VT_UI2) || OPS_HAS(VT_I1) || OPS_HAS(VT_BSTR) || OPS_HAS(VT_DATE) ||
      OPS_HAS(VT_CY) || OPS_HAS(VT_DECIMAL) || OPS_HAS(VT_R8) || OPS_HAS(VT_R4)) return VT_I4;
  if (OPS_HAS(VT_I2) || OPS_HAS(VT_BOOL) || l == VT_EMPTY) return VT_I2;
  return VT_UI1;
}

/* Plain operands, neither NULL nor DISPATCH nor one of the mismatch types. */
static HRESULT ops_intdiv_core(BOOL mod, const VARIANT *left, const VARIANT *right, VARIANT *out) {
  const VARTYPE l = V_VT(left), r = V_VT(right), rt = ops_idiv_type(l, r);
  LONG64 a, b, res;
  HRESULT hr;
  VariantInit(out);
  if (rt == VT_I8) {
    hr = ops_get_i8(left, &a);
    if (SUCCEEDED(hr)) hr = ops_get_i8(right, &b);
  } else {
    LONG x, y;
    hr = ops_get_i4(left, FALSE, &x);
    if (SUCCEEDED(hr)) hr = ops_get_i4(right, FALSE, &y);
    a = x;
    b = y;
  }
  if (FAILED(hr)) return hr;
  if (b == 0) return DISP_E_DIVBYZERO;
  if (b == -1) {      /* (and a division of the smallest number by -1 would trap) */
    if (mod) res = 0;
    else if (a == (-9223372036854775807LL - 1)) return DISP_E_OVERFLOW;
    else res = -a;
  } else res = mod ? a % b : a / b;
  switch (rt) {
    case VT_I8: V_I8(out) = res; V_VT(out) = VT_I8; return S_OK;
    case VT_I4: if (!ops_store_int(out, res, VT_I4)) return DISP_E_OVERFLOW; return S_OK;
    case VT_I2: if (!ops_store_int(out, res, VT_I2)) return DISP_E_OVERFLOW; return S_OK;
    default: if (!ops_store_int(out, res, VT_UI1)) return DISP_E_OVERFLOW; return S_OK;
  }
}

static HRESULT ops_intdiv(BOOL mod, VARIANT *left, VARIANT *right, VARIANT *result) {
  VARIANT lt, rt, out;
  const VARIANT *l = left, *r = right;
  HRESULT hr;
  if (!left || !right || !result) return E_INVALIDARG;
  VariantInit(&lt);
  VariantInit(&rt);
  VariantInit(&out);
  hr = ops_int_validate(V_VT(left), V_VT(right));
  if (SUCCEEDED(hr)) {
    if (V_VT(left) == VT_DISPATCH) { hr = ops_dispatch_value(V_DISPATCH(left), &lt); l = &lt; }
    if (SUCCEEDED(hr) && V_VT(right) == VT_DISPATCH) { hr = ops_dispatch_value(V_DISPATCH(right), &rt); r = &rt; }
    if (SUCCEEDED(hr) && (l != left || r != right)) hr = ops_int_validate(V_VT(l), V_VT(r));
    if (SUCCEEDED(hr) && (V_VT(l) == VT_DISPATCH || V_VT(r) == VT_DISPATCH)) hr = DISP_E_TYPEMISMATCH;
    if (SUCCEEDED(hr)) {
      if (V_VT(l) == VT_NULL || V_VT(r) == VT_NULL) V_VT(&out) = VT_NULL;
      else hr = ops_intdiv_core(mod, l, r, &out);
    }
  }
  VariantClear(&lt);
  VariantClear(&rt);
  if (FAILED(hr)) { V_VT(result) = VT_EMPTY; return hr; }
  *result = out;
  return S_OK;
}

EXPORT HRESULT WINAPI VarIdiv(LPVARIANT left, LPVARIANT right, LPVARIANT result) { return ops_intdiv(FALSE, left, right, result); }
EXPORT HRESULT WINAPI VarMod(LPVARIANT left, LPVARIANT right, LPVARIANT result) { return ops_intdiv(TRUE, left, right, result); }

static BOOL ops_pow_vt_ok(VARTYPE t) {
  switch (t) {
    case VT_EMPTY: case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_I4: case VT_UI4: case VT_I8: case VT_UI8: case VT_INT: case VT_UINT: case VT_R4: case VT_R8: case VT_CY:
    case VT_DATE: case VT_BSTR: case VT_BOOL: case VT_DECIMAL: return TRUE;
    default: return FALSE;
  }
}

EXPORT HRESULT WINAPI VarPow(LPVARIANT left, LPVARIANT right, LPVARIANT result) {
  VARIANT lt, rt, out;
  const VARIANT *l = left, *r = right;
  HRESULT hr = S_OK;
  double a, b;
  if (!left || !right || !result) return E_INVALIDARG;
  VariantInit(&lt);
  VariantInit(&rt);
  VariantInit(&out);
  if ((V_VT(left) | V_VT(right)) & ~VT_TYPEMASK) hr = ((V_VT(left) | V_VT(right)) & VT_ARRAY) ? DISP_E_TYPEMISMATCH : DISP_E_BADVARTYPE;
  else {
    VARTYPE lvt = V_VT(left), rvt = V_VT(right);
    if (lvt == VT_DISPATCH && rvt != VT_NULL) { hr = ops_dispatch_value(V_DISPATCH(left), &lt); l = &lt; lvt = V_VT(l); }
    if (SUCCEEDED(hr) && rvt == VT_DISPATCH && lvt != VT_NULL) { hr = ops_dispatch_value(V_DISPATCH(right), &rt); r = &rt; rvt = V_VT(r); }
    if (SUCCEEDED(hr)) {
      const BOOL lok = lvt == VT_NULL || ops_pow_vt_ok(lvt), rok = rvt == VT_NULL || ops_pow_vt_ok(rvt);
      if (!lok || !rok) hr = (lvt == VT_ERROR || lvt == VT_VARIANT || lvt == VT_UNKNOWN || lvt == VT_RECORD || lvt == VT_DISPATCH || rvt == VT_ERROR || rvt == VT_VARIANT || rvt == VT_UNKNOWN ||
                              rvt == VT_RECORD || rvt == VT_DISPATCH) ? DISP_E_TYPEMISMATCH : DISP_E_BADVARTYPE;
      else if (lvt == VT_NULL || rvt == VT_NULL) V_VT(&out) = VT_NULL;
      else {
        hr = ops_get_r8(l, FALSE, &a);
        if (SUCCEEDED(hr)) hr = ops_get_r8(r, FALSE, &b);
        if (SUCCEEDED(hr)) {
          const double p = oa_pow(a, b);
          if (ops_is_inf_or_nan(p)) hr = DISP_E_OVERFLOW;
          else { V_R8(&out) = p; V_VT(&out) = VT_R8; }
        }
      }
    }
  }
  VariantClear(&lt);
  VariantClear(&rt);
  if (FAILED(hr)) { V_VT(result) = VT_EMPTY; return hr; }
  *result = out;
  return S_OK;
}

/* ---- And Or Xor Eqv Imp Not ----------------------------------------------------------------------------------------------------------------------------- */

enum { OPS_AND, OPS_OR, OPS_XOR, OPS_EQV, OPS_IMP };

static int ops_bit_kind(VARTYPE t) {      /* 1 usable, 2 a type mismatch, 0 not a type */
  switch (t) {
    case VT_EMPTY: case VT_NULL: case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_I4: case VT_UI4: case VT_I8: case VT_UI8: case VT_INT: case VT_UINT: case VT_R4: case VT_R8:
    case VT_CY: case VT_DATE: case VT_DECIMAL: case VT_BOOL: case VT_BSTR: case VT_DISPATCH: return 1;
    case VT_ERROR: case VT_VARIANT: case VT_UNKNOWN: case VT_RECORD: return 2;
    default: return 0;
  }
}

/* The type of the result of a logical operator on two values that are not NULL (VT_ERROR: Windows fails the combination). */
static VARTYPE ops_bit_type(int op, VARTYPE l, VARTYPE r) {
  if (l == VT_EMPTY || r == VT_EMPTY) {
    if (l == r || OPS_HAS(VT_I2) || OPS_HAS(VT_UI1) || OPS_HAS(VT_BOOL)) return VT_I2;
    return OPS_HAS(VT_I8) ? VT_I8 : VT_I4;
  }
  if (OPS_HAS(VT_UI1)) {
    if (l == r) return VT_UI1;
    if (OPS_HAS(VT_I2) || OPS_HAS(VT_BOOL)) return VT_I2;
    return OPS_HAS(VT_I8) ? VT_I8 : VT_I4;
  }
  if (OPS_HAS(VT_I2)) {
    if (l == r || OPS_HAS(VT_BOOL)) return VT_I2;
    return OPS_HAS(VT_I8) ? VT_I8 : VT_I4;
  }
  if (l == VT_BOOL && r == VT_BOOL) return VT_BOOL;
  if (OPS_HAS(VT_I8)) {
    if (op == OPS_IMP ? (l == VT_I8 && r == VT_INT) : OPS_HAS(VT_INT)) return VT_ERROR;
    return VT_I8;
  }
  return VT_I4;
}

/* The type a lone value (the other being NULL) gets. */
static VARTYPE ops_bit_promote(VARTYPE t) {
  switch (t) {
    case VT_I8: case VT_UI1: case VT_I2: case VT_BOOL: return t;
    case VT_EMPTY: return VT_I2;
    default: return VT_I4;
  }
}

/* The value of v as the type t chose (the bits, as a LONG64). */
static HRESULT ops_bit_value(const VARIANT *v, VARTYPE t, LONG64 *out) {
  LONG x;
  HRESULT hr;
  switch (t) {
    case VT_I8: return ops_get_i8(v, out);
    case VT_I4: hr = ops_get_i4(v, TRUE, &x); *out = x; return hr;
    case VT_UI1: *out = V_VT(v) == VT_EMPTY ? 0 : V_UI1(v); return S_OK;
    default: *out = V_VT(v) == VT_EMPTY ? 0 : V_VT(v) == VT_UI1 ? V_UI1(v) : V_I2(v); return S_OK;      /* I2, BOOL (from a byte too) */
  }
}

static LONG64 ops_bit_trunc(LONG64 v, VARTYPE t) {
  switch (t) {
    case VT_I8: return v;
    case VT_I4: return (LONG)v;
    case VT_UI1: return (BYTE)v;
    default: return (SHORT)v;
  }
}

static void ops_bit_store(VARIANT *out, LONG64 v, VARTYPE t) {
  switch (t) {
    case VT_I8: V_I8(out) = v; break;
    case VT_I4: V_I4(out) = (LONG)v; break;
    case VT_UI1: V_UI1(out) = (BYTE)v; break;
    case VT_BOOL: V_BOOL(out) = (VARIANT_BOOL)v; break;
    default: V_I2(out) = (SHORT)v; break;
  }
  V_VT(out) = t;
}

/* A logical operand resolved: objects to their value, strings to a number (or a boolean). May leave a temporary in tmp. */
static HRESULT ops_bit_operand(const VARIANT *in, VARIANT *tmp, const VARIANT **use) {
  HRESULT hr = S_OK;
  *use = in;
  if (V_VT(in) == VT_DISPATCH) {
    hr = ops_dispatch_value(V_DISPATCH(in), tmp);
    if (FAILED(hr)) return hr;
    *use = tmp;
    if (V_VT(tmp) == VT_DISPATCH || ops_bit_kind(V_VT(tmp)) != 1) return DISP_E_TYPEMISMATCH;
  }
  if (V_VT(*use) == VT_BSTR) {
    VARIANT num;
    VariantInit(&num);
    hr = ops_bstr_number(V_BSTR(*use), &num);
    if (FAILED(hr)) return hr;
    VariantClear(tmp);
    *tmp = num;
    *use = tmp;
  }
  return S_OK;
}

static HRESULT ops_bit(int op, VARIANT *left, VARIANT *right, VARIANT *result) {
  VARIANT lt, rt, out;
  const VARIANT *l, *r;
  HRESULT hr = S_OK;
  if (!left || !right || !result) return E_INVALIDARG;
  VariantInit(&lt);
  VariantInit(&rt);
  VariantInit(&out);
  if ((V_VT(left) | V_VT(right)) & ~VT_TYPEMASK) hr = ((V_VT(left) | V_VT(right)) & VT_ARRAY) ? DISP_E_TYPEMISMATCH : DISP_E_BADVARTYPE;
  else {
    const int lk = ops_bit_kind(V_VT(left)), rk = ops_bit_kind(V_VT(right));
    if (lk != 1 || rk != 1) hr = (lk == 2 || rk == 2) ? DISP_E_TYPEMISMATCH : DISP_E_BADVARTYPE;
    else {
      hr = ops_bit_operand(left, &lt, &l);
      if (SUCCEEDED(hr)) hr = ops_bit_operand(right, &rt, &r);
      if (SUCCEEDED(hr)) {
        const VARTYPE lvt = V_VT(l), rvt = V_VT(r);
        if (lvt == VT_NULL && rvt == VT_NULL) V_VT(&out) = VT_NULL;
        else if (lvt == VT_NULL || rvt == VT_NULL) {
          const VARIANT *x = lvt == VT_NULL ? r : l;      /* the value that is not NULL */
          const VARTYPE t = ops_bit_promote(V_VT(x));
          LONG64 v;
          hr = ops_bit_value(x, t, &v);
          if (SUCCEEDED(hr)) {
            BOOL keep = FALSE;
            if (op == OPS_AND) keep = v == 0;
            else if (op == OPS_OR || (op == OPS_IMP && lvt == VT_NULL)) keep = v != 0;
            else if (op == OPS_IMP) { v = ops_bit_trunc(~v, t); keep = v != 0; }
            if (keep) ops_bit_store(&out, v, t);
            else V_VT(&out) = VT_NULL;
          }
        } else {
          const VARTYPE t = ops_bit_type(op, lvt, rvt);
          LONG64 a, b, v;
          if (t == VT_ERROR) hr = DISP_E_TYPEMISMATCH;
          else {
            hr = ops_bit_value(l, t, &a);
            if (SUCCEEDED(hr)) hr = ops_bit_value(r, t, &b);
            if (SUCCEEDED(hr)) {
              switch (op) {
                case OPS_AND: v = a & b; break;
                case OPS_OR: v = a | b; break;
                case OPS_XOR: v = a ^ b; break;
                case OPS_EQV: v = ~(a ^ b); break;
                default: v = ~a | b; break;
              }
              ops_bit_store(&out, ops_bit_trunc(v, t), t);
            }
          }
        }
      }
    }
  }
  VariantClear(&lt);
  VariantClear(&rt);
  if (FAILED(hr)) { V_VT(result) = VT_EMPTY; return hr; }
  *result = out;
  return S_OK;
}

EXPORT HRESULT WINAPI VarAnd(LPVARIANT left, LPVARIANT right, LPVARIANT result) { return ops_bit(OPS_AND, left, right, result); }
EXPORT HRESULT WINAPI VarOr(LPVARIANT left, LPVARIANT right, LPVARIANT result) { return ops_bit(OPS_OR, left, right, result); }
EXPORT HRESULT WINAPI VarXor(LPVARIANT left, LPVARIANT right, LPVARIANT result) { return ops_bit(OPS_XOR, left, right, result); }
EXPORT HRESULT WINAPI VarEqv(LPVARIANT left, LPVARIANT right, LPVARIANT result) { return ops_bit(OPS_EQV, left, right, result); }
EXPORT HRESULT WINAPI VarImp(LPVARIANT left, LPVARIANT right, LPVARIANT result) { return ops_bit(OPS_IMP, left, right, result); }

/* ---- unary: Not Neg Abs Int Fix Round -------------------------------------------------------------------------------------------------------------------- */

/* The operand of a unary operator as a plain value: flags and objects handled. not_style selects VarNot's error codes for flags. */
static HRESULT ops_unary_operand(const VARIANT *in, VARIANT *tmp, const VARIANT **use, BOOL not_style) {
  const VARTYPE vt = V_VT(in);
  *use = in;
  VariantInit(tmp);
  if (vt & ~VT_TYPEMASK) {
    if (not_style) return (variant_valid_type(vt) && (vt & VT_TYPEMASK) != VT_CLSID) ? DISP_E_TYPEMISMATCH : DISP_E_BADVARTYPE;
    return (vt & VT_ARRAY) ? DISP_E_TYPEMISMATCH : DISP_E_BADVARTYPE;
  }
  if (vt == VT_DISPATCH) {
    HRESULT hr = ops_dispatch_value(V_DISPATCH(in), tmp);
    if (FAILED(hr)) return hr;
    *use = tmp;
    if ((V_VT(tmp) & ~VT_TYPEMASK) || V_VT(tmp) == VT_DISPATCH) { VariantClear(tmp); return DISP_E_TYPEMISMATCH; }
  }
  return S_OK;
}

/* The error for a type a unary operator does not take. */
static HRESULT ops_unary_bad(VARTYPE vt, BOOL not_style) {
  switch (vt) {
    case VT_ERROR: case VT_UNKNOWN: case VT_RECORD: case VT_DISPATCH: case VT_BSTR: return DISP_E_TYPEMISMATCH;
    case VT_VARIANT: return not_style ? DISP_E_TYPEMISMATCH : DISP_E_BADVARTYPE;
    default: return (not_style && variant_valid_type(vt) && vt != VT_CLSID) ? DISP_E_TYPEMISMATCH : DISP_E_BADVARTYPE;
  }
}

static HRESULT ops_not_core(const VARIANT *v, VARIANT *out) {
  VariantInit(out);
  switch (V_VT(v)) {
    case VT_EMPTY: V_I2(out) = -1; V_VT(out) = VT_I2; return S_OK;
    case VT_NULL: V_VT(out) = VT_NULL; return S_OK;
    case VT_BOOL: V_BOOL(out) = (VARIANT_BOOL)~V_BOOL(v); V_VT(out) = VT_BOOL; return S_OK;
    case VT_UI1: V_UI1(out) = (BYTE)~V_UI1(v); V_VT(out) = VT_UI1; return S_OK;
    case VT_I2: V_I2(out) = (SHORT)~V_I2(v); V_VT(out) = VT_I2; return S_OK;
    case VT_I8: V_I8(out) = ~V_I8(v); V_VT(out) = VT_I8; return S_OK;
    case VT_BSTR: {
      VARIANT num, again;
      HRESULT hr = ops_bstr_number(V_BSTR(v), &num);
      if (FAILED(hr)) return hr;
      hr = ops_not_core(&num, &again);
      if (SUCCEEDED(hr)) *out = again;
      return hr;
    }
    case VT_I1: case VT_UI2: case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_UI8: case VT_R4: case VT_R8: case VT_DATE: case VT_CY: case VT_DECIMAL: {
      LONG x;
      HRESULT hr = ops_get_i4(v, TRUE, &x);
      if (FAILED(hr)) return hr;
      V_I4(out) = ~x;
      V_VT(out) = VT_I4;
      return S_OK;
    }
    default: return ops_unary_bad(V_VT(v), TRUE);
  }
}

EXPORT HRESULT WINAPI VarNot(LPVARIANT in, LPVARIANT result) {
  VARIANT tmp, out;
  const VARIANT *v;
  if (!in || !result) return E_INVALIDARG;
  HRESULT hr = ops_unary_operand(in, &tmp, &v, TRUE);
  if (SUCCEEDED(hr)) hr = ops_not_core(v, &out);
  VariantClear(&tmp);
  if (FAILED(hr)) { V_VT(result) = VT_EMPTY; return hr; }
  *result = out;
  return S_OK;
}

enum { OPS_NEG, OPS_ABS, OPS_INT, OPS_FIX, OPS_ROUND };

/* The value of an integer type with the sign of the operation done (an overflow goes to the larger type). */
static void ops_signed_result(VARIANT *out, LONG64 v, VARTYPE vt) {
  if (vt == VT_I8) { V_I8(out) = v; V_VT(out) = VT_I8; return; }
  if (!ops_store_int(out, v, vt == VT_I4 ? VT_I4 : VT_I2)) {
    if (!ops_store_int(out, v, VT_I4)) { V_R8(out) = (double)v; V_VT(out) = VT_R8; }
  }
}

static HRESULT ops_unary_core(int op, int digits, const VARIANT *v, VARIANT *out) {
  VariantInit(out);
  switch (V_VT(v)) {
    case VT_EMPTY: V_I2(out) = 0; V_VT(out) = VT_I2; return S_OK;
    case VT_NULL: V_VT(out) = VT_NULL; return S_OK;
    case VT_BOOL: {
      const LONG64 b = V_I2(v);
      ops_signed_result(out, op == OPS_NEG ? -b : op == OPS_ABS ? (b < 0 ? -b : b) : b, VT_I2);
      return S_OK;
    }
    case VT_UI1: {
      const LONG64 b = V_UI1(v);
      if (op == OPS_NEG) ops_signed_result(out, -b, VT_I2);
      else { V_UI1(out) = V_UI1(v); V_VT(out) = VT_UI1; }
      return S_OK;
    }
    case VT_I2: case VT_I4: case VT_I8: {
      const VARTYPE vt = V_VT(v);
      const LONG64 n = vt == VT_I2 ? V_I2(v) : vt == VT_I4 ? V_I4(v) : V_I8(v);
      if (op == OPS_NEG || op == OPS_ABS) {
        if (n == (-9223372036854775807LL - 1) && vt == VT_I8) { V_R8(out) = 9223372036854775808.0; V_VT(out) = VT_R8; return S_OK; }
        ops_signed_result(out, (op == OPS_NEG || n < 0) ? -n : n, vt);
      } else ops_signed_result(out, n, vt);
      return S_OK;
    }
    case VT_R4: case VT_R8: case VT_DATE: case VT_BSTR: {
      double d;
      VARTYPE vt = V_VT(v);
      if (vt == VT_BSTR) {
        HRESULT hr = V_BSTR(v) ? VarR8FromStr(V_BSTR(v), OPS_LCID, 0, &d) : DISP_E_TYPEMISMATCH;
        if (FAILED(hr)) return hr;
        vt = VT_R8;
      } else d = vt == VT_R4 ? (double)V_R4(v) : V_R8(v);
      switch (op) {
        case OPS_NEG: d = -d; break;
        case OPS_ABS: d = oa_fabs(d); break;
        case OPS_INT: d = oa_floor(d); break;
        case OPS_FIX: d = d >= 0.0 ? oa_floor(d) : oa_ceil(d); break;
        default: {
          HRESULT hr;
          if (digits < 0) return E_INVALIDARG;
          hr = VarR8Round(d, digits, &d);
          if (FAILED(hr)) return hr;
          break;
        }
      }
      if (vt == VT_R4) { V_R4(out) = (float)d; V_VT(out) = VT_R4; }
      else { V_R8(out) = d; V_VT(out) = vt; }
      return S_OK;
    }
    case VT_CY: {
      CY c = V_CY(v), res;
      HRESULT hr = op == OPS_NEG ? VarCyNeg(c, &res) : op == OPS_ABS ? VarCyAbs(c, &res) : op == OPS_INT ? VarCyInt(c, &res) : op == OPS_FIX ? VarCyFix(c, &res) :
                   digits < 0 ? E_INVALIDARG : VarCyRound(c, digits, &res);
      if (FAILED(hr)) return hr;
      V_CY(out) = res;
      V_VT(out) = VT_CY;
      return S_OK;
    }
    case VT_DECIMAL: {
      DECIMAL d = V_DECIMAL(v), res;
      HRESULT hr = op == OPS_NEG ? VarDecNeg(&d, &res) : op == OPS_ABS ? VarDecAbs(&d, &res) : op == OPS_INT ? VarDecInt(&d, &res) : op == OPS_FIX ? VarDecFix(&d, &res) :
                   digits < 0 ? E_INVALIDARG : VarDecRound(&d, digits, &res);
      if (FAILED(hr)) return hr;
      V_DECIMAL(out) = res;
      V_VT(out) = VT_DECIMAL;
      return S_OK;
    }
    default: return ops_unary_bad(V_VT(v), FALSE);
  }
}

static HRESULT ops_unary(int op, int digits, VARIANT *in, VARIANT *result) {
  VARIANT tmp, out;
  const VARIANT *v;
  if (!in || !result) return E_INVALIDARG;
  HRESULT hr = ops_unary_operand(in, &tmp, &v, FALSE);
  if (SUCCEEDED(hr)) hr = ops_unary_core(op, digits, v, &out);
  VariantClear(&tmp);
  if (FAILED(hr)) { V_VT(result) = VT_EMPTY; return hr; }
  *result = out;
  return S_OK;
}

EXPORT HRESULT WINAPI VarNeg(LPVARIANT in, LPVARIANT result) { return ops_unary(OPS_NEG, 0, in, result); }
EXPORT HRESULT WINAPI VarAbs(LPVARIANT in, LPVARIANT result) { return ops_unary(OPS_ABS, 0, in, result); }
EXPORT HRESULT WINAPI VarInt(LPVARIANT in, LPVARIANT result) { return ops_unary(OPS_INT, 0, in, result); }
EXPORT HRESULT WINAPI VarFix(LPVARIANT in, LPVARIANT result) { return ops_unary(OPS_FIX, 0, in, result); }
EXPORT HRESULT WINAPI VarRound(LPVARIANT in, int decimals, LPVARIANT result) { return ops_unary(OPS_ROUND, decimals, in, result); }

/* ---- strings: VarBstrCat VarBstrCmp VarCat; comparison: VarCmp ---------------------------------------------------------------------------------------------- */

EXPORT HRESULT WINAPI VarBstrCat(BSTR left, BSTR right, LPBSTR out) {
  if (!out) return E_POINTER;
  const UINT ll = SysStringLen(left), rl = SysStringLen(right);
  if (ll > 0x7fffffffu - rl) return E_OUTOFMEMORY;
  BSTR joined = SysAllocStringLen(NULL, ll + rl);
  if (!joined) return E_OUTOFMEMORY;
  if (ll) memcpy(joined, left, ll * sizeof(WCHAR));
  if (rl) memcpy(joined + ll, right, rl * sizeof(WCHAR));
  *out = joined;
  return S_OK;
}

/* A NULL string is an empty one. */
EXPORT HRESULT WINAPI VarBstrCmp(BSTR left, BSTR right, LCID lcid, ULONG flags) {
  const UINT ll = SysStringLen(left), rl = SysStringLen(right);
  if (!lcid) {      /* LCID 0: a binary comparison of the bytes (an odd byte count counts), then of the byte counts */
    const UINT lb = SysStringByteLen(left), rb = SysStringByteLen(right), n = lb < rb ? lb : rb;
    const unsigned char *x = (const unsigned char *)left, *y = (const unsigned char *)right;
    for (UINT i = 0; i < n; i++)
      if (x[i] != y[i]) return x[i] < y[i] ? VARCMP_LT : VARCMP_GT;
    return lb == rb ? VARCMP_EQ : lb < rb ? VARCMP_LT : VARCMP_GT;
  }
  if (!ll || !rl) return ll == rl ? VARCMP_EQ : ll ? VARCMP_GT : VARCMP_LT;
  switch (CompareStringW(lcid, flags, left, (int)ll, right, (int)rl)) {
    case CSTR_LESS_THAN: return VARCMP_LT;
    case CSTR_GREATER_THAN: return VARCMP_GT;
    case CSTR_EQUAL: return VARCMP_EQ;
    default: return VARCMP_NULL;
  }
}

/* One side of a concatenation as a string: *text is a new BSTR or NULL for NULL/empty. */
static HRESULT ops_cat_operand(const VARIANT *in, BSTR *text) {
  VARIANT value, str;
  const VARIANT *v = in;
  HRESULT hr = S_OK;
  VariantInit(&value);
  VariantInit(&str);
  *text = NULL;
  if (V_VT(in) & ~VT_TYPEMASK) return (V_VT(in) & VT_ARRAY) ? DISP_E_TYPEMISMATCH : DISP_E_BADVARTYPE;
  if (V_VT(in) == VT_DISPATCH) {
    hr = ops_dispatch_value(V_DISPATCH(in), &value);
    if (FAILED(hr)) return hr;
    v = &value;
    if ((V_VT(v) & ~VT_TYPEMASK) || V_VT(v) == VT_DISPATCH) { VariantClear(&value); return DISP_E_TYPEMISMATCH; }
  }
  switch (V_VT(v)) {
    case VT_EMPTY: case VT_NULL: break;
    case VT_BSTR: if (SysStringLen(V_BSTR(v))) { *text = SysAllocStringLen(V_BSTR(v), SysStringLen(V_BSTR(v))); if (!*text) hr = E_OUTOFMEMORY; } break;
    case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_I4: case VT_UI4: case VT_I8: case VT_UI8: case VT_INT: case VT_UINT: case VT_R4: case VT_R8: case VT_BOOL:
    case VT_CY: case VT_DATE: case VT_DECIMAL:
      hr = VariantChangeType(&str, (VARIANT *)v, VARIANT_LOCALBOOL, VT_BSTR);
      if (SUCCEEDED(hr)) { *text = V_BSTR(&str); V_VT(&str) = VT_EMPTY; }
      break;
    case VT_ERROR: case VT_VARIANT: case VT_UNKNOWN: case VT_RECORD: hr = DISP_E_TYPEMISMATCH; break;
    default: hr = DISP_E_BADVARTYPE; break;
  }
  VariantClear(&value);
  return hr;
}

EXPORT HRESULT WINAPI VarCat(LPVARIANT left, LPVARIANT right, LPVARIANT result) {
  BSTR ls = NULL, rs = NULL, joined;
  HRESULT hr;
  if (!left || !right || !result) return E_INVALIDARG;
  hr = ops_cat_operand(left, &ls);
  if (SUCCEEDED(hr)) hr = ops_cat_operand(right, &rs);
  if (SUCCEEDED(hr)) {
    VARIANT out;
    VariantInit(&out);
    if (V_VT(left) == VT_NULL && V_VT(right) == VT_NULL) V_VT(&out) = VT_NULL;
    else {
      hr = VarBstrCat(ls, rs, &joined);
      if (SUCCEEDED(hr)) { V_BSTR(&out) = joined; V_VT(&out) = VT_BSTR; }
    }
    if (SUCCEEDED(hr)) *result = out;
  }
  SysFreeString(ls);
  SysFreeString(rs);
  if (FAILED(hr)) V_VT(result) = VT_EMPTY;
  return hr;
}

/* VarCmp's types: the two sides must be numbers, strings, EMPTY or NULL; INT only on the left; two ERRORs are equal. */
static BOOL ops_cmp_vt_ok(VARTYPE t, BOOL left) {
  switch (t) {
    case VT_EMPTY: case VT_NULL: case VT_I2: case VT_I4: case VT_R4: case VT_R8: case VT_CY: case VT_DATE: case VT_BSTR: case VT_BOOL: case VT_DECIMAL: case VT_UI1: case VT_I8: return TRUE;
    case VT_INT: return left;
    default: return FALSE;
  }
}

/* A decimal against a double: the double as a decimal (one too big is beyond any decimal). */
static HRESULT ops_cmp_dec_r8(DECIMAL *dec, double d) {
  DECIMAL other;
  HRESULT hr = VarDecFromR8(d, &other);
  if (hr == DISP_E_OVERFLOW) return d > 0.0 ? VARCMP_LT : VARCMP_GT;
  if (FAILED(hr)) return hr;
  return VarDecCmp(dec, &other);
}

static HRESULT ops_cmp_float(float a, float b) { return a < b ? VARCMP_LT : a > b ? VARCMP_GT : VARCMP_EQ; }

static HRESULT ops_cmp_numbers(const VARIANT *l, const VARIANT *r) {
  const VARTYPE lt = V_VT(l), rt = V_VT(r);
  if (lt == VT_DECIMAL || rt == VT_DECIMAL) {
    DECIMAL a, b;
    HRESULT hr;
    if ((lt == VT_R8 || lt == VT_DATE || lt == VT_R4) || (rt == VT_R8 || rt == VT_DATE || rt == VT_R4)) {
      const BOOL dec_left = lt == VT_DECIMAL;
      const VARIANT *n = dec_left ? r : l;
      double d = V_VT(n) == VT_R4 ? (double)V_R4(n) : V_R8(n);
      a = V_DECIMAL(dec_left ? l : r);
      hr = ops_cmp_dec_r8(&a, d);
      return dec_left ? hr : (hr == VARCMP_LT ? VARCMP_GT : hr == VARCMP_GT ? VARCMP_LT : hr);
    }
    hr = ops_get_dec(l, &a);
    if (SUCCEEDED(hr)) hr = ops_get_dec(r, &b);
    if (FAILED(hr)) return hr;
    return VarDecCmp(&a, &b);
  }
  if (lt == VT_CY || rt == VT_CY) {
    CY a, b;
    HRESULT hr;
    if ((lt == VT_R8 || lt == VT_DATE || lt == VT_R4) || (rt == VT_R8 || rt == VT_DATE || rt == VT_R4)) {
      const BOOL cy_left = lt == VT_CY;
      const VARIANT *n = cy_left ? r : l;
      const double d = V_VT(n) == VT_R4 ? (double)V_R4(n) : V_R8(n);
      hr = VarCyCmpR8(cy_left ? V_CY(l) : V_CY(r), d);
      return cy_left ? hr : (hr == VARCMP_LT ? VARCMP_GT : hr == VARCMP_GT ? VARCMP_LT : hr);
    }
    hr = ops_get_cy(l, &a);
    if (SUCCEEDED(hr)) hr = ops_get_cy(r, &b);
    if (FAILED(hr)) return hr;
    return a.int64 < b.int64 ? VARCMP_LT : a.int64 > b.int64 ? VARCMP_GT : VARCMP_EQ;
  }
  if (lt == VT_R4 && (rt == VT_R8 || rt == VT_DATE)) return ops_cmp_float(V_R4(l), (float)V_R8(r));      /* (a single against a double: the double is narrowed) */
  if (rt == VT_R4 && (lt == VT_R8 || lt == VT_DATE)) return ops_cmp_float((float)V_R8(l), V_R4(r));
  if (lt == VT_R4 || rt == VT_R4 || lt == VT_R8 || rt == VT_R8 || lt == VT_DATE || rt == VT_DATE) {
    double a, b;
    HRESULT hr = ops_get_r8(l, TRUE, &a);
    if (SUCCEEDED(hr)) hr = ops_get_r8(r, TRUE, &b);
    if (FAILED(hr)) return hr;
    return a < b ? VARCMP_LT : a > b ? VARCMP_GT : VARCMP_EQ;
  } else {
    LONG64 a, b;
    HRESULT hr = ops_get_i8(l, &a);
    if (SUCCEEDED(hr)) hr = ops_get_i8(r, &b);
    if (FAILED(hr)) return hr;
    return a < b ? VARCMP_LT : a > b ? VARCMP_GT : VARCMP_EQ;
  }
}

/* A string against a number. Neither side marked VT_RESERVED (or both): the string is greater, whatever it holds - Windows: the string "wins"; both marked: it is smaller
 * if it is a negative number. Only the string marked: compared as text with the number written out. Only the number marked: compared as numbers if the string is one. */
static HRESULT ops_cmp_string_number(const VARIANT *s, BOOL s_marked, const VARIANT *n, BOOL n_marked, LCID lcid, ULONG flags) {
  if (s_marked == n_marked) {
    double d;
    if (s_marked && V_BSTR(s) && SUCCEEDED(VarR8FromStr(V_BSTR(s), lcid, 0, &d)) && d < 0.0) return VARCMP_LT;
    return VARCMP_GT;
  }
  if (s_marked) {
    VARIANT text;
    VariantInit(&text);
    HRESULT hr = VariantChangeType(&text, (VARIANT *)n, VARIANT_LOCALBOOL, VT_BSTR);
    if (FAILED(hr)) return hr;
    hr = VarBstrCmp(V_BSTR(s), V_BSTR(&text), lcid, flags);
    VariantClear(&text);
    return hr;
  } else {
    VARIANT num;
    double d;
    if (!V_BSTR(s) || FAILED(VarR8FromStr(V_BSTR(s), lcid, 0, &d))) return VARCMP_GT;
    VariantInit(&num);
    V_VT(&num) = VT_R8;
    V_R8(&num) = d;
    if (V_VT(n) == VT_R4) {      /* (not narrowed to a single: the string has more digits) */
      VARIANT wide = *n;
      V_R8(&wide) = (double)V_R4(n);
      V_VT(&wide) = VT_R8;
      return ops_cmp_numbers(&num, &wide);
    }
    return ops_cmp_numbers(&num, n);
  }
}

EXPORT HRESULT WINAPI VarCmp(LPVARIANT left, LPVARIANT right, LCID lcid, ULONG flags) {
  VARTYPE lf, rf;
  BOOL lmark, rmark;
  VARIANT lv, rv;
  if (!left || !right) return E_INVALIDARG;
  lf = V_VT(left);
  rf = V_VT(right);
  lmark = (lf & VT_RESERVED) != 0;      /* (a mark Windows' callers use to say "compare this string as text") */
  rmark = (rf & VT_RESERVED) != 0;
  lf &= ~VT_RESERVED;
  rf &= ~VT_RESERVED;
  if ((lf | rf) & ~VT_TYPEMASK) return ((lf | rf) & VT_ARRAY) ? DISP_E_TYPEMISMATCH : DISP_E_BADVARTYPE;
  if (lf == VT_ERROR && rf == VT_ERROR) return VARCMP_EQ;
  if (!ops_cmp_vt_ok(lf, TRUE) || !ops_cmp_vt_ok(rf, FALSE)) {
    switch (lf) { case VT_ERROR: case VT_VARIANT: case VT_UNKNOWN: case VT_DISPATCH: case VT_RECORD: return DISP_E_TYPEMISMATCH; default: break; }
    switch (rf) { case VT_ERROR: case VT_VARIANT: case VT_UNKNOWN: case VT_DISPATCH: case VT_RECORD: return DISP_E_TYPEMISMATCH; default: break; }
    return DISP_E_BADVARTYPE;
  }
  if (lf == VT_NULL || rf == VT_NULL) return VARCMP_NULL;
  lv = *left;      /* (copies without the mark; EMPTY has no value to read) */
  rv = *right;
  V_VT(&lv) = lf;
  V_VT(&rv) = rf;
  if (lf == VT_BSTR && rf == VT_BSTR) return VarBstrCmp(V_BSTR(left), V_BSTR(right), lcid, flags);
  if (lf == VT_BSTR && rf == VT_EMPTY) return VarBstrCmp(V_BSTR(left), NULL, lcid, flags);
  if (lf == VT_EMPTY && rf == VT_BSTR) return VarBstrCmp(NULL, V_BSTR(right), lcid, flags);
  if (lf == VT_EMPTY && rf == VT_EMPTY) return VARCMP_EQ;
  if (lf == VT_BSTR || rf == VT_BSTR) {
    HRESULT hr;
    if (lf == VT_BSTR) return ops_cmp_string_number(&lv, lmark, &rv, rmark, lcid, flags);
    hr = ops_cmp_string_number(&rv, rmark, &lv, lmark, lcid, flags);
    return hr == VARCMP_LT ? VARCMP_GT : hr == VARCMP_GT ? VARCMP_LT : hr;
  }
  return ops_cmp_numbers(&lv, &rv);
}
