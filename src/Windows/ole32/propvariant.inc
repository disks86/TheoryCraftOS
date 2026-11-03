/* ole32: PROPVARIANT (clear, copy, change type, arrays). */

/* ---- which types are valid ------------------------------------------------------------------------------------------------------------------------------------ */

enum { PV_SIMPLE = 1, PV_ARRAY = 2, PV_VECTOR = 4, PV_BYREF = 8 };

/* what Windows accepts, per base type: simple, with VT_ARRAY, with VT_VECTOR, with VT_BYREF (tests/windows/wine/ole32/propvariant.c valid_types) */
static BYTE pv_validity(unsigned base) {
  switch (base) {
    case VT_EMPTY: case VT_NULL: return PV_SIMPLE;
    case VT_I2: case VT_I4: case VT_R4: case VT_R8: case VT_CY: case VT_DATE: case VT_BSTR: case VT_ERROR: case VT_BOOL: case VT_VARIANT:
    case VT_I1: case VT_UI1: case VT_UI2: case VT_UI4: case VT_I8: case VT_UI8: return PV_SIMPLE | PV_ARRAY | PV_VECTOR | PV_BYREF;
    case VT_DISPATCH: case VT_UNKNOWN: case VT_DECIMAL: case VT_INT: case VT_UINT: case VT_RECORD: return PV_SIMPLE | PV_ARRAY | PV_BYREF;
    case VT_LPSTR: case VT_LPWSTR: case VT_FILETIME: case VT_CF: case VT_CLSID: return PV_SIMPLE | PV_VECTOR;
    case VT_BLOB: case VT_STREAM: case VT_STORAGE: case VT_STREAMED_OBJECT: case VT_STORED_OBJECT: case VT_BLOB_OBJECT: return PV_SIMPLE;
    default: return 0;
  }
}
static BOOL pv_valid_type(VARTYPE vt) {
  unsigned base = vt & VT_TYPEMASK;
  BYTE ok = pv_validity(base);
  if (vt & VT_BYREF) return (ok & PV_BYREF) != 0;
  if (vt & VT_ARRAY) return (ok & PV_ARRAY) != 0;
  if (vt & VT_VECTOR) return (ok & PV_VECTOR) != 0;
  return (ok & PV_SIMPLE) != 0;
}

/* SAFEARRAYs belong to oleaut32.dll; they are used through it when it provides them */
static HMODULE g_oleaut;
static FARPROC oleaut_proc(const char *name) {
  if (!g_oleaut) g_oleaut = LoadLibraryW(W('o', 'l', 'e', 'a', 'u', 't', '3', '2', '.', 'd', 'l', 'l'));
  return g_oleaut ? GetProcAddress(g_oleaut, name) : NULL;
}

/* ---- clear --------------------------------------------------------------------------------------------------------------------------------------------------------- */

static void pv_clear_element(VARTYPE base, const void *element);
static void pv_free_vector(PROPVARIANT *pv) {
  VARTYPE base = pv->vt & VT_TYPEMASK;
  BYTE *items = (BYTE *)pv->caub.pElems;
  size_t size = 0;
  switch (base) {
    case VT_BSTR: case VT_LPSTR: case VT_LPWSTR: case VT_CF: case VT_VARIANT: {
      size = base == VT_VARIANT ? sizeof(PROPVARIANT) : sizeof(void *);
      if (items) for (ULONG i = 0; i < pv->caub.cElems; i++) pv_clear_element(base, items + (size_t)i * size);
      break;
    }
    default: break;
  }
  CoTaskMemFree(items);
}
static void pv_clear_element(VARTYPE base, const void *element) {
  switch (base) {
    case VT_BSTR: bstr_free(*(BSTR *)element); break;
    case VT_LPSTR: case VT_LPWSTR: CoTaskMemFree(*(void **)element); break;
    case VT_CF: {
      CLIPDATA *cd = *(CLIPDATA **)element;
      if (cd) { CoTaskMemFree(cd->pClipData); CoTaskMemFree(cd); }
      break;
    }
    case VT_VARIANT: PropVariantClear((PROPVARIANT *)(void *)element); break;
    default: break;
  }
}

EXPORT HRESULT WINAPI PropVariantClear(PROPVARIANT *pv) {
  if (!pv) return S_OK;
  if (!pv_valid_type(pv->vt)) {
    mem_zero(pv, sizeof(*pv));
    return STG_E_INVALIDPARAMETER;
  }
  VARTYPE vt = pv->vt;
  if (vt & VT_BYREF) {
    /* the value is not owned */
  } else if (vt & VT_ARRAY) {
    if (pv->parray) {
      typedef HRESULT (WINAPI *Destroy)(SAFEARRAY *);
      Destroy destroy = (Destroy)(void *)oleaut_proc("SafeArrayDestroy");
      if (destroy) destroy(pv->parray);
    }
  } else if (vt & VT_VECTOR) {
    pv_free_vector(pv);
  } else {
    switch (vt) {
      case VT_BSTR: bstr_free(pv->bstrVal); break;
      case VT_BLOB: case VT_BLOB_OBJECT: CoTaskMemFree(pv->blob.pBlobData); break;
      case VT_STREAM: case VT_STREAMED_OBJECT: if (pv->pStream) IStream_Release(pv->pStream); break;
      case VT_STORAGE: case VT_STORED_OBJECT: if (pv->pStorage) IStorage_Release(pv->pStorage); break;
      case VT_UNKNOWN: if (pv->punkVal) IUnknown_Release(pv->punkVal); break;
      case VT_DISPATCH: if (pv->pdispVal) IDispatch_Release(pv->pdispVal); break;
      case VT_CLSID: case VT_LPSTR: case VT_LPWSTR: CoTaskMemFree(pv->pszVal); break;
      case VT_CF: pv_clear_element(VT_CF, &pv->pclipdata); break;
      case VT_VARIANT: break;
      default: break;
    }
  }
  mem_zero(pv, sizeof(*pv));
  return S_OK;
}

EXPORT HRESULT WINAPI FreePropVariantArray(ULONG count, PROPVARIANT *array) {
  if (!array) return E_INVALIDARG;
  for (ULONG i = 0; i < count; i++) PropVariantClear(&array[i]);
  return S_OK;
}

/* ---- copy ---------------------------------------------------------------------------------------------------------------------------------------------------------- */

static size_t pv_element_size(VARTYPE base) {
  switch (base) {
    case VT_I1: case VT_UI1: return 1;
    case VT_I2: case VT_UI2: case VT_BOOL: return 2;
    case VT_I4: case VT_UI4: case VT_R4: case VT_ERROR: return 4;
    case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE: case VT_FILETIME: return 8;
    case VT_CLSID: return sizeof(GUID);
    case VT_BSTR: case VT_LPSTR: case VT_LPWSTR: case VT_CF: return sizeof(void *);
    case VT_VARIANT: return sizeof(PROPVARIANT);
    default: return 0;
  }
}

static HRESULT pv_copy_clipdata(const CLIPDATA *src, CLIPDATA **out) {
  CLIPDATA *copy;
  if (!src) { *out = NULL; return S_OK; }
  if (src->cbSize < sizeof(ULONG) || src->cbSize > 0x40000000) return STG_E_INVALIDPARAMETER;
  copy = CoTaskMemAlloc(sizeof(*copy));
  if (!copy) return E_OUTOFMEMORY;
  copy->cbSize = src->cbSize;
  copy->ulClipFmt = src->ulClipFmt;
  copy->pClipData = CoTaskMemAlloc(src->cbSize - sizeof(ULONG) ? src->cbSize - sizeof(ULONG) : 1);
  if (!copy->pClipData) { CoTaskMemFree(copy); return E_OUTOFMEMORY; }
  mem_copy(copy->pClipData, src->pClipData, src->cbSize - sizeof(ULONG));
  *out = copy;
  return S_OK;
}

static HRESULT pv_copy_string(const void *src, BOOL wide, void **out) {
  if (!src) { *out = NULL; return S_OK; }
  SIZE_T n = wide ? (wlen(src) + 1) * sizeof(WCHAR) : 0;
  if (!wide) { const char *p = src; while (p[n]) n++; n++; }
  void *copy = CoTaskMemAlloc(n);
  if (!copy) return E_OUTOFMEMORY;
  mem_copy(copy, src, n);
  *out = copy;
  return S_OK;
}

EXPORT HRESULT WINAPI PropVariantCopy(PROPVARIANT *dest, const PROPVARIANT *src) {
  PROPVARIANT tmp;
  HRESULT hr = S_OK;
  if (!dest || !src) return E_INVALIDARG;
  if (!pv_valid_type(src->vt)) return DISP_E_BADVARTYPE;
  tmp = *src;
  VARTYPE vt = src->vt;
  if (vt & VT_BYREF) {
    /* the pointer is copied as it is */
  } else if (vt & VT_ARRAY) {
    if (src->parray) {
      typedef HRESULT (WINAPI *Copy)(SAFEARRAY *, SAFEARRAY **);
      Copy copy = (Copy)(void *)oleaut_proc("SafeArrayCopy");
      tmp.parray = NULL;
      if (!copy) return E_NOTIMPL;
      hr = copy(src->parray, &tmp.parray);
      if (SUCCEEDED(hr) && !tmp.parray) hr = E_FAIL;
    }
  } else if (vt & VT_VECTOR) {
    VARTYPE base = vt & VT_TYPEMASK;
    size_t size = pv_element_size(base);
    ULONG n = src->caub.cElems;
    tmp.caub.pElems = NULL;
    if (!size) return STG_E_INVALIDPARAMETER;
    if (n && src->caub.pElems) {
      if (n > 0x7FFFFFFFu / size) return E_OUTOFMEMORY;
      BYTE *items = CoTaskMemAlloc((SIZE_T)n * size);
      if (!items) return E_OUTOFMEMORY;
      mem_zero(items, (SIZE_T)n * size);
      const BYTE *from = (const BYTE *)src->caub.pElems;
      ULONG i;
      for (i = 0; i < n && SUCCEEDED(hr); i++) {
        BYTE *to = items + (size_t)i * size;
        const BYTE *s = from + (size_t)i * size;
        switch (base) {
          case VT_BSTR: { const WCHAR *w = *(BSTR const *)s; *(BSTR *)to = bstr_dup(w); if (w && !*(BSTR *)to) hr = E_OUTOFMEMORY; break; }
          case VT_LPSTR: hr = pv_copy_string(*(void *const *)s, FALSE, (void **)to); break;
          case VT_LPWSTR: hr = pv_copy_string(*(void *const *)s, TRUE, (void **)to); break;
          case VT_CF: hr = pv_copy_clipdata(*(CLIPDATA *const *)s, (CLIPDATA **)to); break;
          case VT_VARIANT: hr = PropVariantCopy((PROPVARIANT *)to, (const PROPVARIANT *)s); break;
          default: mem_copy(to, s, size); break;
        }
      }
      if (FAILED(hr)) {
        PROPVARIANT undo;
        mem_zero(&undo, sizeof(undo));
        undo.vt = vt;
        undo.caub.cElems = i;
        undo.caub.pElems = (void *)items;
        pv_free_vector(&undo);
        return hr;
      }
      tmp.caub.pElems = (void *)items;
    }
  } else {
    switch (vt) {
      case VT_BSTR:
        tmp.bstrVal = bstr_dup(src->bstrVal);
        if (src->bstrVal && !tmp.bstrVal) return E_OUTOFMEMORY;
        break;
      case VT_BLOB: case VT_BLOB_OBJECT:
        tmp.blob.pBlobData = NULL;
        if (src->blob.cbSize && src->blob.pBlobData) {
          tmp.blob.pBlobData = CoTaskMemAlloc(src->blob.cbSize);
          if (!tmp.blob.pBlobData) return E_OUTOFMEMORY;
          mem_copy(tmp.blob.pBlobData, src->blob.pBlobData, src->blob.cbSize);
        }
        break;
      case VT_STREAM: case VT_STREAMED_OBJECT: if (src->pStream) IStream_AddRef(src->pStream); break;
      case VT_STORAGE: case VT_STORED_OBJECT: if (src->pStorage) IStorage_AddRef(src->pStorage); break;
      case VT_UNKNOWN: if (src->punkVal) IUnknown_AddRef(src->punkVal); break;
      case VT_DISPATCH: if (src->pdispVal) IDispatch_AddRef(src->pdispVal); break;
      case VT_CLSID:
        tmp.puuid = NULL;
        if (src->puuid) {
          tmp.puuid = CoTaskMemAlloc(sizeof(GUID));
          if (!tmp.puuid) return E_OUTOFMEMORY;
          *tmp.puuid = *src->puuid;
        }
        break;
      case VT_CF: hr = pv_copy_clipdata(src->pclipdata, &tmp.pclipdata); break;
      case VT_LPSTR: hr = pv_copy_string(src->pszVal, FALSE, (void **)&tmp.pszVal); break;
      case VT_LPWSTR: hr = pv_copy_string(src->pwszVal, TRUE, (void **)&tmp.pwszVal); break;
      default: break;
    }
  }
  if (FAILED(hr)) return hr;
  *dest = tmp;
  return S_OK;
}

/* ---- change of type -------------------------------------------------------------------------------------------------------------------------------------------- */

/* A scalar of a PROPVARIANT as a signed or unsigned 64-bit value or a double. */
typedef struct PvScalar { int kind; /* 0 none, 1 signed, 2 unsigned, 3 double */ LONGLONG s; ULONGLONG u; double d; } PvScalar;

static BOOL pv_get_scalar(const PROPVARIANT *pv, PvScalar *out) {
  mem_zero(out, sizeof(*out));
  switch (pv->vt) {
    case VT_I1: out->kind = 1; out->s = pv->cVal; return TRUE;
    case VT_UI1: out->kind = 2; out->u = pv->bVal; return TRUE;
    case VT_I2: out->kind = 1; out->s = pv->iVal; return TRUE;
    case VT_UI2: out->kind = 2; out->u = pv->uiVal; return TRUE;
    case VT_I4: case VT_INT: out->kind = 1; out->s = pv->lVal; return TRUE;
    case VT_UI4: case VT_UINT: out->kind = 2; out->u = pv->ulVal; return TRUE;
    case VT_I8: out->kind = 1; out->s = pv->hVal.QuadPart; return TRUE;
    case VT_UI8: out->kind = 2; out->u = pv->uhVal.QuadPart; return TRUE;
    case VT_BOOL: out->kind = 1; out->s = pv->boolVal ? -1 : 0; return TRUE;
    case VT_R4: out->kind = 3; out->d = pv->fltVal; return TRUE;
    case VT_R8: out->kind = 3; out->d = pv->dblVal; return TRUE;
    case VT_ERROR: out->kind = 2; out->u = (ULONG)pv->scode; return TRUE;
    default: return FALSE;
  }
}

static int pv_format_u64(WCHAR *out, ULONGLONG v, BOOL negative) {
  WCHAR tmp[24];
  int n = 0, len = 0;
  do { tmp[n++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
  if (negative) out[len++] = '-';
  while (n) out[len++] = tmp[--n];
  out[len] = 0;
  return len;
}

/* Parses an integer ("-12", "0x1F" is not accepted) from wide text; the whole string must be used. */
static BOOL pv_parse_int(const WCHAR *s, BOOL *neg, ULONGLONG *value) {
  *neg = FALSE;
  *value = 0;
  while (*s == ' ') s++;
  if (*s == '-') { *neg = TRUE; s++; } else if (*s == '+') s++;
  if (*s < '0' || *s > '9') return FALSE;
  while (*s >= '0' && *s <= '9') {
    ULONGLONG d = (ULONGLONG)(*s - '0');
    if (*value > (0xFFFFFFFFFFFFFFFFull - d) / 10) return FALSE;
    *value = *value * 10 + d;
    s++;
  }
  while (*s == ' ') s++;
  return *s == 0;
}

static HRESULT pv_store_int(PROPVARIANT *out, VARTYPE vt, BOOL neg, ULONGLONG mag) {
  /* neg: the value is -mag */
  LONGLONG smax, smin;
  ULONGLONG umax;
  switch (vt) {
    case VT_I1: smax = 127; smin = -128; umax = 127; break;
    case VT_UI1: smax = 255; smin = 0; umax = 255; break;
    case VT_I2: smax = 32767; smin = -32768; umax = 32767; break;
    case VT_UI2: smax = 65535; smin = 0; umax = 65535; break;
    case VT_I4: case VT_INT: smax = 2147483647ll; smin = -2147483648ll; umax = 2147483647ull; break;
    case VT_UI4: case VT_UINT: smax = 4294967295ll; smin = 0; umax = 4294967295ull; break;
    case VT_I8: smax = 0x7FFFFFFFFFFFFFFFll; smin = (-0x7FFFFFFFFFFFFFFFll - 1); umax = 0x7FFFFFFFFFFFFFFFull; break;
    case VT_UI8: smax = 0x7FFFFFFFFFFFFFFFll; smin = 0; umax = 0xFFFFFFFFFFFFFFFFull; break;
    default: return DISP_E_TYPEMISMATCH;
  }
  if (neg && mag) {
    if (smin == 0) return DISP_E_OVERFLOW;
    if (mag > (ULONGLONG)(-(smin + 1)) + 1) return DISP_E_OVERFLOW;
  } else if (mag > umax) {
    return DISP_E_OVERFLOW;
  }
  (void)smax;
  LONGLONG s = neg ? (LONGLONG)(0 - mag) : (LONGLONG)mag;
  out->vt = vt;
  mem_zero(&out->hVal, sizeof(out->hVal));
  switch (vt) {
    case VT_I1: out->cVal = (CHAR)s; break;
    case VT_UI1: out->bVal = (BYTE)mag; break;
    case VT_I2: out->iVal = (SHORT)s; break;
    case VT_UI2: out->uiVal = (USHORT)mag; break;
    case VT_I4: case VT_INT: out->lVal = (LONG)s; break;
    case VT_UI4: case VT_UINT: out->ulVal = (ULONG)mag; break;
    case VT_I8: out->hVal.QuadPart = s; break;
    default: out->uhVal.QuadPart = mag; break;
  }
  return S_OK;
}

EXPORT HRESULT WINAPI PropVariantChangeType(PROPVARIANT *dest, REFPROPVARIANT src, PROPVAR_CHANGE_FLAGS flags, VARTYPE vt) {
  PvScalar sc;
  WCHAR text[32];
  const WCHAR *string = NULL;
  (void)flags;
  if (!dest || !src) return E_INVALIDARG;
  if (src->vt == vt) {
    PROPVARIANT copy;
    HRESULT hr = PropVariantCopy(&copy, src);
    if (FAILED(hr)) return hr;
    *dest = copy;
    return S_OK;
  }
  if (!pv_valid_type(src->vt) || !pv_valid_type(vt)) return DISP_E_BADVARTYPE;
  if (src->vt & (VT_ARRAY | VT_VECTOR | VT_BYREF)) return DISP_E_TYPEMISMATCH;
  if (src->vt == VT_BSTR) string = src->bstrVal;
  else if (src->vt == VT_LPWSTR) string = src->pwszVal;
  PROPVARIANT out;
  mem_zero(&out, sizeof(out));
  /* to a string */
  if (vt == VT_BSTR || vt == VT_LPWSTR || vt == VT_LPSTR) {
    const WCHAR *w = string;
    if (!w && src->vt == VT_LPSTR && src->pszVal) {
      int n = MultiByteToWideChar(CP_ACP, 0, src->pszVal, -1, NULL, 0);
      WCHAR *tmpw = n > 0 ? mem_alloc((SIZE_T)n * sizeof(WCHAR)) : NULL;
      if (!tmpw) return E_OUTOFMEMORY;
      MultiByteToWideChar(CP_ACP, 0, src->pszVal, -1, tmpw, n);
      HRESULT hr = PropVariantChangeType(dest, &(PROPVARIANT){.vt = VT_LPWSTR, .pwszVal = tmpw}, flags, vt);
      mem_free(tmpw);
      return hr;
    }
    if (!w) {
      if (src->vt == VT_EMPTY) { text[0] = 0; w = text; }
      else if (pv_get_scalar(src, &sc)) {
        if (sc.kind == 1) pv_format_u64(text, sc.s < 0 ? (ULONGLONG)0 - (ULONGLONG)sc.s : (ULONGLONG)sc.s, sc.s < 0);
        else if (sc.kind == 2) pv_format_u64(text, sc.u, FALSE);
        else { LONGLONG whole = (LONGLONG)sc.d; pv_format_u64(text, whole < 0 ? (ULONGLONG)0 - (ULONGLONG)whole : (ULONGLONG)whole, whole < 0); }
        if (src->vt == VT_BOOL) ascii_to_wide(text, src->boolVal ? "True" : "False");
        w = text;
      } else if (src->vt == VT_CLSID && src->puuid) { guid_to_text(src->puuid, text); w = text; }
      else return DISP_E_TYPEMISMATCH;
    }
    if (vt == VT_BSTR) { out.bstrVal = bstr_dup(w); if (!out.bstrVal) return E_OUTOFMEMORY; }
    else if (vt == VT_LPWSTR) { HRESULT hr = pv_copy_string(w, TRUE, (void **)&out.pwszVal); if (FAILED(hr)) return hr; }
    else {
      int n = WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL);
      out.pszVal = CoTaskMemAlloc(n > 0 ? (SIZE_T)n : 1);
      if (!out.pszVal) return E_OUTOFMEMORY;
      WideCharToMultiByte(CP_ACP, 0, w, -1, out.pszVal, n, NULL, NULL);
    }
    out.vt = vt;
    *dest = out;
    return S_OK;
  }
  /* to a number from a string */
  if (string || (src->vt == VT_LPSTR && src->pszVal)) {
    WCHAR conv[40];
    if (!string) {
      if (MultiByteToWideChar(CP_ACP, 0, src->pszVal, -1, conv, 40) <= 0) return DISP_E_OVERFLOW;
      string = conv;
    }
    if (vt == VT_BOOL) {
      if (!wicmp(string, W('t', 'r', 'u', 'e')) || !wicmp(string, W('#', 'T', 'R', 'U', 'E', '#')) || !wcmp(string, W('1'))) { out.vt = VT_BOOL; out.boolVal = VARIANT_TRUE; *dest = out; return S_OK; }
      if (!wicmp(string, W('f', 'a', 'l', 's', 'e')) || !wicmp(string, W('#', 'F', 'A', 'L', 'S', 'E', '#')) || !wcmp(string, W('0'))) { out.vt = VT_BOOL; out.boolVal = VARIANT_FALSE; *dest = out; return S_OK; }
      return DISP_E_TYPEMISMATCH;
    }
    BOOL neg;
    ULONGLONG mag;
    if (!pv_parse_int(string, &neg, &mag)) return DISP_E_TYPEMISMATCH;
    HRESULT hr = pv_store_int(&out, vt, neg, mag);
    if (FAILED(hr)) return hr;
    *dest = out;
    return S_OK;
  }
  /* number to number */
  if (!pv_get_scalar(src, &sc)) {
    if (src->vt == VT_EMPTY && (vt == VT_BOOL || (pv_get_scalar(&(PROPVARIANT){.vt = vt}, &sc)))) { out.vt = vt; *dest = out; return S_OK; }
    return DISP_E_TYPEMISMATCH;
  }
  if (vt == VT_BOOL) {
    BOOL nonzero = sc.kind == 3 ? sc.d != 0 : (sc.kind == 1 ? sc.s != 0 : sc.u != 0);
    out.vt = VT_BOOL;
    out.boolVal = nonzero ? VARIANT_TRUE : VARIANT_FALSE;
    *dest = out;
    return S_OK;
  }
  if (vt == VT_R4 || vt == VT_R8) {
    double d = sc.kind == 3 ? sc.d : (sc.kind == 1 ? (double)sc.s : (double)sc.u);
    out.vt = vt;
    if (vt == VT_R4) out.fltVal = (float)d; else out.dblVal = d;
    *dest = out;
    return S_OK;
  }
  BOOL neg = FALSE;
  ULONGLONG mag;
  if (sc.kind == 3) {
    double d = sc.d;
    d = d < 0 ? -(double)(ULONGLONG)(-d + 0.5) : (double)(ULONGLONG)(d + 0.5);      /* (rounds to nearest) */
    if (sc.d >= 18446744073709551615.0 || sc.d <= -9223372036854775808.0) return DISP_E_OVERFLOW;
    neg = sc.d < 0;
    mag = (ULONGLONG)(neg ? -d : d);
    if (mag == 0) neg = FALSE;
  } else if (sc.kind == 1) {
    neg = sc.s < 0;
    mag = neg ? (ULONGLONG)0 - (ULONGLONG)sc.s : (ULONGLONG)sc.s;
  } else {
    mag = sc.u;
  }
  HRESULT hr = pv_store_int(&out, vt, neg, mag);
  if (FAILED(hr)) return hr;
  *dest = out;
  return S_OK;
}
