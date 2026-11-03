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

/* VARIANT basics: VariantInit, VariantClear, VariantCopy, VariantCopyInd. (VariantChangeType and the conversions are in conv.inc.) */

/* Is `vt` a type a VARIANT can hold (Windows' rule: no vector or reserved flag, an array or reference of EMPTY/NULL is not one, 15 and the types from VT_VOID on are not)? */
static BOOL variant_valid_type(VARTYPE vt) {
  const VARTYPE extra = vt & (VT_BYREF | VT_ARRAY | VT_VECTOR | VT_RESERVED), base = vt & VT_TYPEMASK;
  if (extra & (VT_VECTOR | VT_RESERVED)) return FALSE;
  if (base < VT_VOID || base == VT_RECORD || base == VT_CLSID) {
    if ((extra & (VT_BYREF | VT_ARRAY)) && base <= VT_NULL) return FALSE;
    return base != 15;
  }
  return FALSE;
}

EXPORT void WINAPI VariantInit(VARIANTARG *variant) { if (variant) memset(variant, 0, sizeof(*variant)); }

EXPORT HRESULT WINAPI VariantClear(VARIANTARG *variant) {
  if (!variant) return DISP_E_BADVARTYPE;
  const VARTYPE vt = V_VT(variant);
  if (!variant_valid_type(vt)) return DISP_E_BADVARTYPE;
  if (!(vt & VT_BYREF)) {
    if (vt & VT_ARRAY) {
      if (V_ARRAY(variant)) { HRESULT hr = SafeArrayDestroy(V_ARRAY(variant)); if (FAILED(hr)) return hr; }
    } else switch (vt) {
      case VT_BSTR: SysFreeString(V_BSTR(variant)); break;
      case VT_UNKNOWN: if (V_UNKNOWN(variant)) IUnknown_Release(V_UNKNOWN(variant)); break;
      case VT_DISPATCH: if (V_DISPATCH(variant)) IDispatch_Release(V_DISPATCH(variant)); break;
      case VT_RECORD: {
        if (V_RECORDINFO(variant)) {
          if (V_RECORD(variant)) IRecordInfo_RecordClear(V_RECORDINFO(variant), V_RECORD(variant));
          IRecordInfo_Release(V_RECORDINFO(variant));
        }
        break;
      }
      default: break;
    }
  }
  V_VT(variant) = VT_EMPTY;
  return S_OK;
}

EXPORT HRESULT WINAPI VariantCopy(VARIANTARG *dest, VARIANTARG *src) {
  if (!dest || !src) return DISP_E_BADVARTYPE;
  const VARTYPE vt = V_VT(src);
  if (!variant_valid_type(vt)) return DISP_E_BADVARTYPE;
  if (dest == src) return S_OK;
  HRESULT hr = VariantClear(dest);
  if (FAILED(hr)) return hr;
  if ((vt & VT_BYREF) || (!(vt & VT_ARRAY) && vt != VT_BSTR && vt != VT_UNKNOWN && vt != VT_DISPATCH && vt != VT_RECORD)) {
    *dest = *src;
    return S_OK;
  }
  if (vt & VT_ARRAY) {
    V_VT(dest) = VT_EMPTY;
    if (V_ARRAY(src)) {
      hr = SafeArrayCopy(V_ARRAY(src), &V_ARRAY(dest));
      if (FAILED(hr)) return hr;
    } else V_ARRAY(dest) = NULL;
    V_VT(dest) = vt;
    return S_OK;
  }
  switch (vt) {
    case VT_BSTR:
      if (V_BSTR(src)) {
        BSTR copy = SysAllocStringByteLen((LPCSTR)V_BSTR(src), SysStringByteLen(V_BSTR(src)));
        if (!copy) return E_OUTOFMEMORY;
        V_BSTR(dest) = copy;
      } else V_BSTR(dest) = NULL;
      break;
    case VT_UNKNOWN: V_UNKNOWN(dest) = V_UNKNOWN(src); if (V_UNKNOWN(src)) IUnknown_AddRef(V_UNKNOWN(src)); break;
    case VT_DISPATCH: V_DISPATCH(dest) = V_DISPATCH(src); if (V_DISPATCH(src)) IDispatch_AddRef(V_DISPATCH(src)); break;
    case VT_RECORD: {
      IRecordInfo *info = V_RECORDINFO(src);
      void *record = V_RECORD(src);
      V_RECORDINFO(dest) = info;
      V_RECORD(dest) = NULL;
      if (info) {
        ULONG size = 0;
        IRecordInfo_AddRef(info);
        hr = IRecordInfo_GetSize(info, &size);
        if (SUCCEEDED(hr) && record) {
          void *copy = oa_calloc(size);
          if (!copy) { IRecordInfo_Release(info); V_RECORDINFO(dest) = NULL; return E_OUTOFMEMORY; }
          hr = IRecordInfo_RecordCopy(info, record, copy);
          if (FAILED(hr)) { oa_free(copy); IRecordInfo_Release(info); V_RECORDINFO(dest) = NULL; return hr; }
          V_RECORD(dest) = copy;
        }
      } else V_RECORD(dest) = record;
      break;
    }
  }
  V_VT(dest) = vt;
  return S_OK;
}

/* Copies what a by-reference VARIANT points to (one level), or a plain copy. */
EXPORT HRESULT WINAPI VariantCopyInd(VARIANT *dest, VARIANTARG *src) {
  if (!dest || !src) return E_INVALIDARG;
  const VARTYPE vt = V_VT(src);
  if (!(vt & VT_BYREF)) return VariantCopy(dest, src);
  if (!variant_valid_type(vt)) return DISP_E_BADVARTYPE;
  if (vt == (VT_VARIANT | VT_BYREF)) {
    VARIANT *inner = V_VARIANTREF(src);
    if (!inner) return E_INVALIDARG;
    const VARTYPE ivt = V_VT(inner);
    if (ivt == (VT_VARIANT | VT_BYREF) || ivt == VT_VARIANT || ivt == (VT_VARIANT | VT_ARRAY)) return E_INVALIDARG;
    if (dest == src) {      /* the destination holds the source: copy first, then free the reference */
      VARIANT temp;
      VariantInit(&temp);
      HRESULT hr = VariantCopyInd(&temp, inner);
      if (FAILED(hr)) return hr;
      *dest = temp;
      return S_OK;
    }
    HRESULT hr = VariantClear(dest);
    if (FAILED(hr)) return hr;
    return VariantCopyInd(dest, inner);
  }
  if (vt & VT_ARRAY) {
    SAFEARRAY **array = V_ARRAYREF(src);
    if (dest != src) { HRESULT hr = VariantClear(dest); if (FAILED(hr)) return hr; }
    if (!array) return E_INVALIDARG;
    VARIANT temp;
    VariantInit(&temp);
    if (*array) { HRESULT hr = SafeArrayCopy(*array, &V_ARRAY(&temp)); if (FAILED(hr)) return hr; } else V_ARRAY(&temp) = NULL;
    V_VT(&temp) = (VARTYPE)(vt & ~VT_BYREF);
    *dest = temp;
    return S_OK;
  }
  VARIANT temp;
  VariantInit(&temp);
  const VARTYPE base = (VARTYPE)(vt & ~VT_BYREF);
  void *ref = V_BYREF(src);
  if (!ref) return E_INVALIDARG;
  V_VT(&temp) = base;
  switch (base) {
    case VT_I1: case VT_UI1: V_UI1(&temp) = *(BYTE *)ref; break;
    case VT_I2: case VT_UI2: case VT_BOOL: V_UI2(&temp) = *(USHORT *)ref; break;
    case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_R4: case VT_ERROR: V_UI4(&temp) = *(ULONG *)ref; break;
    case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE: V_UI8(&temp) = *(ULONGLONG *)ref; break;
    case VT_DECIMAL: V_DECIMAL(&temp) = *(DECIMAL *)ref; V_VT(&temp) = VT_DECIMAL; break;
    case VT_BSTR: {
      BSTR s = *(BSTR *)ref;
      if (s) { V_BSTR(&temp) = SysAllocStringByteLen((LPCSTR)s, SysStringByteLen(s)); if (!V_BSTR(&temp)) return E_OUTOFMEMORY; } else V_BSTR(&temp) = NULL;
      break;
    }
    case VT_UNKNOWN: V_UNKNOWN(&temp) = *(IUnknown **)ref; if (V_UNKNOWN(&temp)) IUnknown_AddRef(V_UNKNOWN(&temp)); break;
    case VT_DISPATCH: V_DISPATCH(&temp) = *(IDispatch **)ref; if (V_DISPATCH(&temp)) IDispatch_AddRef(V_DISPATCH(&temp)); break;
    case VT_RECORD: return E_INVALIDARG;
    default: return DISP_E_BADVARTYPE;
  }
  if (dest != src) { HRESULT hr = VariantClear(dest); if (FAILED(hr)) { VariantClear(&temp); return hr; } }
  *dest = temp;
  return S_OK;
}
