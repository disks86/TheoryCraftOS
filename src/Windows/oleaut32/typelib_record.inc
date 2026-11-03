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

/* IRecordInfo over the typeinfo of a record: its size, GUID and name, the fields (found through the typeinfo's variables) and the operations on the records that
 * follow a field's type: strings are copied and freed, interfaces counted, safe arrays copied, nested records handled through their own record info. */

typedef struct {
  VARTYPE vt;                   /* the VARIANT type of the field, VT_RECORD for a nested record */
  BYTE ptr_levels;
  ULONG offset;
  BSTR name;
  ITypeInfo *record;            /* nested record: its typeinfo */
  ULONG size;
} TlRecField;

typedef struct {
  IRecordInfo IRecordInfo_iface;
  LONG ref;
  ITypeInfo *info;
  GUID guid;
  BSTR name;
  ULONG size;
  TlRecField *fields; UINT nfields;
} TlRecord;

static TlRecord *tl_rec_from(IRecordInfo *i) { return CONTAINING_RECORD(i, TlRecord, IRecordInfo_iface); }

static void tl_rec_free(TlRecord *r) {
  for (UINT i = 0; i < r->nfields; i++) { SysFreeString(r->fields[i].name); if (r->fields[i].record) ITypeInfo_Release(r->fields[i].record); }
  oa_free(r->fields);
  SysFreeString(r->name);
  if (r->info) ITypeInfo_Release(r->info);
  oa_free(r);
}
static HRESULT WINAPI tl_rec_QueryInterface(IRecordInfo *iface, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IRecordInfo)) { *out = iface; IRecordInfo_AddRef(iface); return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI tl_rec_AddRef(IRecordInfo *iface) { return (ULONG)InterlockedIncrement(&tl_rec_from(iface)->ref); }
static ULONG WINAPI tl_rec_Release(IRecordInfo *iface) {
  TlRecord *r = tl_rec_from(iface);
  const LONG ref = InterlockedDecrement(&r->ref);
  if (!ref) tl_rec_free(r);
  return (ULONG)ref;
}

/* The VARIANT type (and nested record) of a field type */
static HRESULT tl_rec_field_type(ITypeInfo *ti, const TYPEDESC *td, TlRecField *f, UINT depth) {
  if (depth > 16) return E_FAIL;
  switch (td->vt) {
    case VT_PTR:
      if (!td->lptdesc) return E_FAIL;
      f->ptr_levels++;
      return tl_rec_field_type(ti, td->lptdesc, f, depth + 1);
    case VT_SAFEARRAY: f->vt = VT_ARRAY; return S_OK;
    case VT_CARRAY: f->vt = VT_BYREF; return S_OK;
    case VT_INT: f->vt = VT_I4; return S_OK;
    case VT_UINT: f->vt = VT_UI4; return S_OK;
    case VT_HRESULT: f->vt = VT_ERROR; return S_OK;
    case VT_USERDEFINED: {
      ITypeInfo *ref;
      TYPEATTR *ta;
      HRESULT hr = ITypeInfo_GetRefTypeInfo(ti, td->hreftype, &ref);
      if (FAILED(hr)) return hr;
      hr = ITypeInfo_GetTypeAttr(ref, &ta);
      if (SUCCEEDED(hr)) {
        switch (ta->typekind) {
          case TKIND_ALIAS: hr = tl_rec_field_type(ref, &ta->tdescAlias, f, depth + 1); break;
          case TKIND_ENUM: f->vt = VT_I4; break;
          case TKIND_RECORD: case TKIND_UNION: f->vt = VT_RECORD; f->size = ta->cbSizeInstance; f->record = ref; ITypeInfo_AddRef(ref); break;
          case TKIND_INTERFACE: case TKIND_COCLASS: f->vt = VT_UNKNOWN; break;
          case TKIND_DISPATCH: f->vt = VT_DISPATCH; break;
          default: hr = E_FAIL; break;
        }
        ITypeInfo_ReleaseTypeAttr(ref, ta);
      }
      ITypeInfo_Release(ref);
      return hr;
    }
    default: f->vt = td->vt; return S_OK;
  }
}
static ULONG tl_rec_vt_size(VARTYPE vt) {
  switch (vt & VT_TYPEMASK) {
    case VT_I1: case VT_UI1: return 1;
    case VT_I2: case VT_UI2: case VT_BOOL: return 2;
    case VT_I4: case VT_UI4: case VT_R4: case VT_ERROR: return 4;
    case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE: return 8;
    case VT_VARIANT: return sizeof(VARIANT);
    case VT_DECIMAL: return sizeof(DECIMAL);
    default: return sizeof(void *);
  }
}
static HRESULT tl_rec_load_fields(TlRecord *r) {
  TYPEATTR *ta;
  HRESULT hr = ITypeInfo_GetTypeAttr(r->info, &ta);
  if (FAILED(hr)) return hr;
  const UINT n = ta->cVars;
  r->fields = n ? oa_calloc((SIZE_T)n * sizeof(TlRecField)) : NULL;
  if (n && !r->fields) { ITypeInfo_ReleaseTypeAttr(r->info, ta); return E_OUTOFMEMORY; }
  for (UINT i = 0; i < n && SUCCEEDED(hr); i++) {
    VARDESC *vd;
    hr = ITypeInfo_GetVarDesc(r->info, i, &vd);
    if (FAILED(hr)) break;
    TlRecField *f = &r->fields[r->nfields];
    f->offset = vd->oInst;
    UINT cn = 0;
    ITypeInfo_GetNames(r->info, vd->memid, &f->name, 1, &cn);
    hr = tl_rec_field_type(r->info, &vd->elemdescVar.tdesc, f, 0);
    if (SUCCEEDED(hr)) r->nfields++;
    ITypeInfo_ReleaseVarDesc(r->info, vd);
  }
  ITypeInfo_ReleaseTypeAttr(r->info, ta);
  return hr;
}

static HRESULT WINAPI tl_rec_RecordInit(IRecordInfo *iface, PVOID data) {
  TlRecord *r = tl_rec_from(iface);
  if (!data) return E_INVALIDARG;
  memset(data, 0, r->size);
  return S_OK;
}
static void tl_rec_clear_field(TlRecField *f, BYTE *p) {
  if (f->ptr_levels) { memset(p, 0, sizeof(void *)); return; }          /* (a pointer is not owned) */
  if (f->vt == VT_BSTR) { SysFreeString(*(BSTR *)p); *(BSTR *)p = NULL; }
  else if (f->vt == VT_UNKNOWN || f->vt == VT_DISPATCH) { IUnknown *u = *(IUnknown **)p; if (u) IUnknown_Release(u); *(IUnknown **)p = NULL; }
  else if (f->vt == VT_VARIANT) VariantClear((VARIANT *)p);
  else if (f->vt == VT_ARRAY) { SAFEARRAY *a = *(SAFEARRAY **)p; if (a) SafeArrayDestroy(a); *(SAFEARRAY **)p = NULL; }
  else if (f->vt == VT_RECORD && f->record) {
    IRecordInfo *ri;
    if (SUCCEEDED(GetRecordInfoFromTypeInfo(f->record, &ri))) { IRecordInfo_RecordClear(ri, p); IRecordInfo_Release(ri); }
  }
}
static HRESULT WINAPI tl_rec_RecordClear(IRecordInfo *iface, PVOID data) {
  TlRecord *r = tl_rec_from(iface);
  if (!data) return E_INVALIDARG;
  for (UINT i = 0; i < r->nfields; i++) tl_rec_clear_field(&r->fields[i], (BYTE *)data + r->fields[i].offset);
  return S_OK;
}
static HRESULT WINAPI tl_rec_RecordCopy(IRecordInfo *iface, PVOID src, PVOID dest) {
  TlRecord *r = tl_rec_from(iface);
  if (!src || !dest) return E_INVALIDARG;
  if (src == dest) return S_OK;
  tl_rec_RecordClear(iface, dest);
  memcpy(dest, src, r->size);
  HRESULT hr = S_OK;
  for (UINT i = 0; i < r->nfields && SUCCEEDED(hr); i++) {
    TlRecField *f = &r->fields[i];
    BYTE *s = (BYTE *)src + f->offset, *d = (BYTE *)dest + f->offset;
    if (f->ptr_levels) continue;
    if (f->vt == VT_BSTR) { *(BSTR *)d = *(BSTR *)s ? SysAllocStringLen(*(BSTR *)s, SysStringLen(*(BSTR *)s)) : NULL; if (*(BSTR *)s && !*(BSTR *)d) hr = E_OUTOFMEMORY; }
    else if (f->vt == VT_UNKNOWN || f->vt == VT_DISPATCH) { if (*(IUnknown **)d) IUnknown_AddRef(*(IUnknown **)d); }
    else if (f->vt == VT_VARIANT) { memset(d, 0, sizeof(VARIANT)); hr = VariantCopy((VARIANT *)d, (VARIANT *)s); }
    else if (f->vt == VT_ARRAY) { *(SAFEARRAY **)d = NULL; if (*(SAFEARRAY **)s) hr = SafeArrayCopy(*(SAFEARRAY **)s, (SAFEARRAY **)d); }
    else if (f->vt == VT_RECORD && f->record) {
      IRecordInfo *ri;
      memset(d, 0, f->size);
      if (SUCCEEDED(GetRecordInfoFromTypeInfo(f->record, &ri))) { hr = IRecordInfo_RecordCopy(ri, s, d); IRecordInfo_Release(ri); }
    }
  }
  return hr;
}
static HRESULT WINAPI tl_rec_GetGuid(IRecordInfo *iface, GUID *guid) { if (!guid) return E_INVALIDARG; *guid = tl_rec_from(iface)->guid; return S_OK; }
static HRESULT WINAPI tl_rec_GetName(IRecordInfo *iface, BSTR *name) {
  TlRecord *r = tl_rec_from(iface);
  if (!name) return E_INVALIDARG;
  *name = r->name ? SysAllocStringLen(r->name, SysStringLen(r->name)) : NULL;
  return r->name && !*name ? E_OUTOFMEMORY : S_OK;
}
static HRESULT WINAPI tl_rec_GetSize(IRecordInfo *iface, ULONG *size) { if (!size) return E_INVALIDARG; *size = tl_rec_from(iface)->size; return S_OK; }
static HRESULT WINAPI tl_rec_GetTypeInfo(IRecordInfo *iface, ITypeInfo **info) {
  if (!info) return E_INVALIDARG;
  *info = tl_rec_from(iface)->info;
  ITypeInfo_AddRef(*info);
  return S_OK;
}
static TlRecField *tl_rec_find(TlRecord *r, LPCOLESTR name) {
  if (!name) return NULL;
  for (UINT i = 0; i < r->nfields; i++) if (r->fields[i].name && !tl_wcsicmp(r->fields[i].name, name)) return &r->fields[i];
  return NULL;
}
/* The value of a field as a VARIANT: a copy, or (nocopy) a reference to the field */
static HRESULT tl_rec_get(TlRecord *r, PVOID data, LPCOLESTR name, VARIANT *v, BOOL nocopy, PVOID *carray) {
  if (!data || !v) return E_INVALIDARG;
  TlRecField *f = tl_rec_find(r, name);
  if (!f) return TYPE_E_FIELDNOTFOUND;
  BYTE *p = (BYTE *)data + f->offset;
  if (carray) *carray = NULL;
  VariantInit(v);
  if (f->ptr_levels || f->vt == VT_BYREF) { V_VT(v) = (VARTYPE)(VT_BYREF | (f->vt == VT_BYREF ? VT_UI1 : f->vt)); V_BYREF(v) = f->vt == VT_BYREF ? (PVOID)p : *(PVOID *)p; if (carray && f->vt == VT_BYREF) *carray = p; return S_OK; }
  if (f->vt == VT_RECORD && f->record) {
    IRecordInfo *ri;
    HRESULT hr = GetRecordInfoFromTypeInfo(f->record, &ri);
    if (FAILED(hr)) return hr;
    PVOID copy = NULL;
    if (nocopy) copy = p; else { hr = IRecordInfo_RecordCreateCopy(ri, p, &copy); if (FAILED(hr)) { IRecordInfo_Release(ri); return hr; } }
    V_VT(v) = VT_RECORD;
    V_RECORD(v) = copy;
    V_RECORDINFO(v) = ri;
    return S_OK;
  }
  if (nocopy) {
    V_VT(v) = (VARTYPE)(VT_BYREF | f->vt);
    V_BYREF(v) = p;
    return S_OK;
  }
  V_VT(v) = f->vt;
  switch (f->vt) {
    case VT_BSTR: V_BSTR(v) = *(BSTR *)p ? SysAllocStringLen(*(BSTR *)p, SysStringLen(*(BSTR *)p)) : NULL; return *(BSTR *)p && !V_BSTR(v) ? E_OUTOFMEMORY : S_OK;
    case VT_UNKNOWN: case VT_DISPATCH: V_UNKNOWN(v) = *(IUnknown **)p; if (V_UNKNOWN(v)) IUnknown_AddRef(V_UNKNOWN(v)); return S_OK;
    case VT_VARIANT: V_VT(v) = VT_EMPTY; return VariantCopy(v, (VARIANT *)p);
    case VT_ARRAY: V_VT(v) = VT_ARRAY; V_ARRAY(v) = NULL; return *(SAFEARRAY **)p ? SafeArrayCopy(*(SAFEARRAY **)p, &V_ARRAY(v)) : S_OK;
    default: memcpy(&V_UI8(v), p, tl_rec_vt_size(f->vt)); return S_OK;
  }
}
static HRESULT WINAPI tl_rec_GetField(IRecordInfo *iface, PVOID data, LPCOLESTR name, VARIANT *v) { return tl_rec_get(tl_rec_from(iface), data, name, v, FALSE, NULL); }
static HRESULT WINAPI tl_rec_GetFieldNoCopy(IRecordInfo *iface, PVOID data, LPCOLESTR name, VARIANT *v, PVOID *carray) { return tl_rec_get(tl_rec_from(iface), data, name, v, TRUE, carray); }
static HRESULT tl_rec_put(TlRecord *r, ULONG flags, PVOID data, LPCOLESTR name, VARIANT *v, BOOL nocopy) {
  if (!data || !v) return E_INVALIDARG;
  if (flags != INVOKE_PROPERTYPUT && flags != INVOKE_PROPERTYPUTREF) return E_INVALIDARG;
  TlRecField *f = tl_rec_find(r, name);
  if (!f) return TYPE_E_FIELDNOTFOUND;
  BYTE *p = (BYTE *)data + f->offset;
  if (f->ptr_levels || f->vt == VT_BYREF) {
    if (!(V_VT(v) & VT_BYREF)) return E_INVALIDARG;
    *(PVOID *)p = V_BYREF(v);
    return S_OK;
  }
  if (f->vt == VT_RECORD) {
    if (V_VT(v) != VT_RECORD || !V_RECORD(v) || !f->record) return DISP_E_TYPEMISMATCH;
    IRecordInfo *ri;
    HRESULT hr = GetRecordInfoFromTypeInfo(f->record, &ri);
    if (FAILED(hr)) return hr;
    hr = IRecordInfo_RecordCopy(ri, V_RECORD(v), p);
    IRecordInfo_Release(ri);
    return hr;
  }
  VARIANT conv;
  VariantInit(&conv);
  HRESULT hr;
  if (f->vt == VT_VARIANT) { tl_rec_clear_field(f, p); memset(p, 0, sizeof(VARIANT)); return VariantCopy((VARIANT *)p, v); }
  hr = VariantChangeType(&conv, v, 0, f->vt);
  if (FAILED(hr)) return hr;
  (void)nocopy;
  tl_rec_clear_field(f, p);
  switch (f->vt) {
    case VT_BSTR: *(BSTR *)p = V_BSTR(&conv); break;                 /* (the converted copy is taken over) */
    case VT_UNKNOWN: case VT_DISPATCH: *(IUnknown **)p = V_UNKNOWN(&conv); break;
    case VT_ARRAY: *(SAFEARRAY **)p = V_ARRAY(&conv); break;
    default: memcpy(p, &V_UI8(&conv), tl_rec_vt_size(f->vt)); break;
  }
  return S_OK;
}
static HRESULT WINAPI tl_rec_PutField(IRecordInfo *iface, ULONG flags, PVOID data, LPCOLESTR name, VARIANT *v) { return tl_rec_put(tl_rec_from(iface), flags, data, name, v, FALSE); }
static HRESULT WINAPI tl_rec_PutFieldNoCopy(IRecordInfo *iface, ULONG flags, PVOID data, LPCOLESTR name, VARIANT *v) { return tl_rec_put(tl_rec_from(iface), flags, data, name, v, TRUE); }
static HRESULT WINAPI tl_rec_GetFieldNames(IRecordInfo *iface, ULONG *count, BSTR *names) {
  TlRecord *r = tl_rec_from(iface);
  if (!count) return E_INVALIDARG;
  if (!names) { *count = r->nfields; return S_OK; }
  ULONG n = 0;
  for (; n < *count && n < r->nfields; n++) {
    names[n] = r->fields[n].name ? SysAllocStringLen(r->fields[n].name, SysStringLen(r->fields[n].name)) : NULL;
    if (r->fields[n].name && !names[n]) { while (n--) SysFreeString(names[n]); return E_OUTOFMEMORY; }
  }
  *count = n;
  return S_OK;
}
static BOOL WINAPI tl_rec_IsMatchingType(IRecordInfo *iface, IRecordInfo *other) {
  GUID g;
  if (!other || FAILED(IRecordInfo_GetGuid(other, &g))) return FALSE;
  return IsEqualGUID(&tl_rec_from(iface)->guid, &g);
}
static PVOID WINAPI tl_rec_RecordCreate(IRecordInfo *iface) {
  TlRecord *r = tl_rec_from(iface);
  return oa_calloc(r->size);
}
static HRESULT WINAPI tl_rec_RecordCreateCopy(IRecordInfo *iface, PVOID src, PVOID *dest) {
  TlRecord *r = tl_rec_from(iface);
  if (!src || !dest) return E_INVALIDARG;
  *dest = oa_calloc(r->size);
  if (!*dest) return E_OUTOFMEMORY;
  HRESULT hr = tl_rec_RecordCopy(iface, src, *dest);
  if (FAILED(hr)) { oa_free(*dest); *dest = NULL; }
  return hr;
}
static HRESULT WINAPI tl_rec_RecordDestroy(IRecordInfo *iface, PVOID data) {
  if (!data) return E_INVALIDARG;
  tl_rec_RecordClear(iface, data);
  oa_free(data);
  return S_OK;
}
static IRecordInfoVtbl tl_rec_vtbl = {
  tl_rec_QueryInterface, tl_rec_AddRef, tl_rec_Release, tl_rec_RecordInit, tl_rec_RecordClear, tl_rec_RecordCopy, tl_rec_GetGuid, tl_rec_GetName, tl_rec_GetSize,
  tl_rec_GetTypeInfo, tl_rec_GetField, tl_rec_GetFieldNoCopy, tl_rec_PutField, tl_rec_PutFieldNoCopy, tl_rec_GetFieldNames, tl_rec_IsMatchingType, tl_rec_RecordCreate,
  tl_rec_RecordCreateCopy, tl_rec_RecordDestroy,
};

EXPORT HRESULT WINAPI GetRecordInfoFromTypeInfo(ITypeInfo *info, IRecordInfo **out) {
  if (!info || !out) return E_INVALIDARG;
  *out = NULL;
  TYPEATTR *ta;
  HRESULT hr = ITypeInfo_GetTypeAttr(info, &ta);
  if (FAILED(hr)) return hr;
  if (ta->typekind != TKIND_RECORD && ta->typekind != TKIND_UNION) { ITypeInfo_ReleaseTypeAttr(info, ta); return TYPE_E_WRONGTYPEKIND; }
  TlRecord *r = oa_calloc(sizeof(TlRecord));
  if (!r) { ITypeInfo_ReleaseTypeAttr(info, ta); return E_OUTOFMEMORY; }
  r->IRecordInfo_iface.lpVtbl = &tl_rec_vtbl;
  r->ref = 1;
  r->info = info;
  ITypeInfo_AddRef(info);
  r->guid = ta->guid;
  r->size = ta->cbSizeInstance;
  ITypeInfo_ReleaseTypeAttr(info, ta);
  ITypeInfo_GetDocumentation(info, MEMBERID_NIL, &r->name, NULL, NULL, NULL);
  hr = tl_rec_load_fields(r);
  if (FAILED(hr)) { tl_rec_free(r); return hr; }
  *out = &r->IRecordInfo_iface;
  return S_OK;
}
EXPORT HRESULT WINAPI GetRecordInfoFromGuids(REFGUID libid, ULONG major, ULONG minor, LCID lcid, REFGUID guid, IRecordInfo **out) {
  if (!libid || !guid || !out) return E_INVALIDARG;
  ITypeLib *lib;
  HRESULT hr = LoadRegTypeLib(libid, (WORD)major, (WORD)minor, lcid, &lib);
  if (FAILED(hr)) return hr;
  ITypeInfo *info;
  hr = ITypeLib_GetTypeInfoOfGuid(lib, guid, &info);
  ITypeLib_Release(lib);
  if (FAILED(hr)) return hr;
  hr = GetRecordInfoFromTypeInfo(info, out);
  ITypeInfo_Release(info);
  return hr;
}
