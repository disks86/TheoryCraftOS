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

/* SAFEARRAYs. The descriptor is allocated with 16 hidden bytes in front of it, as on Windows: the VARTYPE (a DWORD just before the descriptor), the IID of an
 * interface array (all 16 bytes), or the IRecordInfo pointer (just before the descriptor). Bounds are stored rightmost dimension first: rgsabound[0] is the last
 * (slowest varying) dimension, and indices[0] belongs to rgsabound[cDims - 1] (the fastest varying one). */

#ifndef FADF_CREATEVECTOR
#define FADF_CREATEVECTOR 0x2000
#endif
#define SA_HIDDEN_SIZE sizeof(GUID)
#define SA_MAX_LOCKS 0xFFFFu
#define SA_MAX_BYTES 0xFFFFFFF0ull

/* The hidden fields are the only place where the descriptor pointer is used as a pointer to something else: the block was allocated with SA_HIDDEN_SIZE spare
 * bytes in front of it, aligned for all of these. */
static DWORD *sa_hidden_vt(SAFEARRAY *psa) { return (DWORD *)psa - 1; }
static GUID *sa_hidden_iid(SAFEARRAY *psa) { return (GUID *)psa - 1; }
static IRecordInfo **sa_hidden_record(SAFEARRAY *psa) { return (IRecordInfo **)psa - 1; }
static void *sa_block(SAFEARRAY *psa) { return (char *)psa - SA_HIDDEN_SIZE; }

static DWORD sa_vt_size(VARTYPE vt) {
  switch (vt) {
    case VT_I1: case VT_UI1: return 1;
    case VT_BOOL: case VT_I2: case VT_UI2: return 2;
    case VT_I4: case VT_UI4: case VT_R4: case VT_ERROR: case VT_INT: case VT_UINT: return 4;
    case VT_R8: case VT_I8: case VT_UI8: case VT_CY: case VT_DATE: return 8;
    case VT_BSTR: return sizeof(BSTR);
    case VT_DISPATCH: return sizeof(IDispatch *);
    case VT_UNKNOWN: return sizeof(IUnknown *);
    case VT_VARIANT: return sizeof(VARIANT);
    case VT_DECIMAL: return sizeof(DECIMAL);
    default: return 0;
  }
}

/* The number of elements; FALSE when the count does not fit in 32 bits. */
static BOOL sa_cell_count(const SAFEARRAY *psa, ULONG *count) {
  unsigned long long n = 1;
  for (USHORT i = 0; i < psa->cDims; i++) {
    n *= psa->rgsabound[i].cElements;
    if (n > 0xFFFFFFFFull) return FALSE;
  }
  *count = (ULONG)n;
  return TRUE;
}
/* The size of the data in bytes. */
static BOOL sa_data_size(const SAFEARRAY *psa, ULONG *bytes) {
  ULONG cells;
  if (!sa_cell_count(psa, &cells)) return FALSE;
  unsigned long long n = (unsigned long long)cells * psa->cbElements;
  if (n > SA_MAX_BYTES) return FALSE;
  *bytes = (ULONG)n;
  return TRUE;
}

static void sa_set_iid(SAFEARRAY *psa, const GUID *iid) { *sa_hidden_iid(psa) = *iid; }
/* The features that come from the element type; vector and BSTR/VARIANT/... flags are added by the creators. */
static void sa_set_features(SAFEARRAY *psa, VARTYPE vt) {
  if (vt == VT_DISPATCH) { psa->fFeatures |= FADF_HAVEIID; sa_set_iid(psa, &IID_IDispatch); }
  else if (vt == VT_UNKNOWN) { psa->fFeatures |= FADF_HAVEIID; sa_set_iid(psa, &IID_IUnknown); }
  else if (vt == VT_RECORD) psa->fFeatures |= FADF_RECORD;
  else { psa->fFeatures |= FADF_HAVEVARTYPE; *sa_hidden_vt(psa) = (DWORD)vt; }
}
static USHORT sa_type_flags(VARTYPE vt) {
  switch (vt) {
    case VT_BSTR: return FADF_BSTR;
    case VT_DISPATCH: return FADF_DISPATCH;
    case VT_UNKNOWN: return FADF_UNKNOWN;
    case VT_VARIANT: return FADF_VARIANT;
    default: return 0;
  }
}

#define SA_OWNING_FEATURES (FADF_BSTR | FADF_UNKNOWN | FADF_DISPATCH | FADF_VARIANT | FADF_RECORD)

/* Releases what the elements [first, first + count) own and zeroes them (the data is then valid for reuse or for freeing). */
static HRESULT sa_clear_elements(SAFEARRAY *psa, ULONG first, ULONG count, BOOL clear_records) {
  HRESULT result = S_OK;
  if (!psa->pvData || !count) return S_OK;
  unsigned char *base = (unsigned char *)psa->pvData + (SIZE_T)first * psa->cbElements;
  for (ULONG i = 0; i < count; i++) {
    unsigned char *element = base + (SIZE_T)i * psa->cbElements;
    if (psa->fFeatures & FADF_BSTR) { BSTR *s = (BSTR *)element; SysFreeString(*s); *s = NULL; }
    else if (psa->fFeatures & FADF_VARIANT) { HRESULT hr = VariantClear((VARIANT *)element); if (FAILED(hr)) result = hr; }
    else if (psa->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) {
      IUnknown **u = (IUnknown **)element;
      if (*u) { IUnknown *old = *u; *u = NULL; IUnknown_Release(old); }
    } else if ((psa->fFeatures & FADF_RECORD) && clear_records) {
      IRecordInfo *info = *sa_hidden_record(psa);
      if (info) IRecordInfo_RecordClear(info, element);
    }
  }
  return result;
}

EXPORT HRESULT WINAPI SafeArrayAllocDescriptor(UINT dims, SAFEARRAY **out) {
  if (!dims || dims >= 0x10000) return E_INVALIDARG;
  if (!out) return E_POINTER;
  char *block = oa_calloc(SA_HIDDEN_SIZE + sizeof(SAFEARRAY) + (SIZE_T)(dims - 1) * sizeof(SAFEARRAYBOUND));
  if (!block) return E_OUTOFMEMORY;
  SAFEARRAY *psa = (SAFEARRAY *)(block + SA_HIDDEN_SIZE);
  psa->cDims = (USHORT)dims;
  *out = psa;
  return S_OK;
}
EXPORT HRESULT WINAPI SafeArrayAllocDescriptorEx(VARTYPE vt, UINT dims, SAFEARRAY **out) {
  if (!dims || dims >= 0x10000) return E_INVALIDARG;
  if (!out) return E_POINTER;
  HRESULT hr = SafeArrayAllocDescriptor(dims, out);
  if (FAILED(hr)) return hr;
  sa_set_features(*out, vt);
  (*out)->cbElements = sa_vt_size(vt);
  return S_OK;
}
EXPORT HRESULT WINAPI SafeArrayAllocData(SAFEARRAY *psa) {
  if (!psa) return E_INVALIDARG;
  ULONG bytes;
  if (!sa_data_size(psa, &bytes)) return E_OUTOFMEMORY;
  psa->pvData = oa_calloc(bytes);
  return psa->pvData ? S_OK : E_OUTOFMEMORY;
}

EXPORT HRESULT WINAPI SafeArrayLock(SAFEARRAY *psa) {
  if (!psa) return E_INVALIDARG;
  if (psa->cLocks >= SA_MAX_LOCKS) return E_UNEXPECTED;
  psa->cLocks++;
  return S_OK;
}
EXPORT HRESULT WINAPI SafeArrayUnlock(SAFEARRAY *psa) {
  if (!psa) return E_INVALIDARG;
  if (!psa->cLocks) return E_UNEXPECTED;
  psa->cLocks--;
  return S_OK;
}
EXPORT HRESULT WINAPI SafeArrayAccessData(SAFEARRAY *psa, void **data) {
  if (!psa || !data) return E_INVALIDARG;
  HRESULT hr = SafeArrayLock(psa);
  if (FAILED(hr)) return hr;
  *data = psa->pvData;
  return S_OK;
}
EXPORT HRESULT WINAPI SafeArrayUnaccessData(SAFEARRAY *psa) { return SafeArrayUnlock(psa); }

EXPORT UINT WINAPI SafeArrayGetDim(SAFEARRAY *psa) { return psa ? psa->cDims : 0; }
EXPORT UINT WINAPI SafeArrayGetElemsize(SAFEARRAY *psa) { return psa ? psa->cbElements : 0; }
EXPORT HRESULT WINAPI SafeArrayGetLBound(SAFEARRAY *psa, UINT dim, LONG *bound) {
  if (!psa || !bound) return E_INVALIDARG;
  if (!dim || dim > psa->cDims) return DISP_E_BADINDEX;
  *bound = psa->rgsabound[psa->cDims - dim].lLbound;
  return S_OK;
}
EXPORT HRESULT WINAPI SafeArrayGetUBound(SAFEARRAY *psa, UINT dim, LONG *bound) {
  if (!psa || !bound) return E_INVALIDARG;
  if (!dim || dim > psa->cDims) return DISP_E_BADINDEX;
  const SAFEARRAYBOUND *b = &psa->rgsabound[psa->cDims - dim];
  *bound = (LONG)((ULONG)b->lLbound + b->cElements - 1);
  return S_OK;
}

EXPORT HRESULT WINAPI SafeArrayGetVartype(SAFEARRAY *psa, VARTYPE *vt) {
  if (!psa || !vt) return E_INVALIDARG;
  if (psa->fFeatures & FADF_RECORD) *vt = VT_RECORD;
  else if (psa->fFeatures & FADF_HAVEIID) *vt = (psa->fFeatures & FADF_DISPATCH) ? VT_DISPATCH : VT_UNKNOWN;
  else if (psa->fFeatures & FADF_HAVEVARTYPE) *vt = (VARTYPE)*sa_hidden_vt(psa);
  else return E_INVALIDARG;
  return S_OK;
}
EXPORT HRESULT WINAPI SafeArraySetIID(SAFEARRAY *psa, REFGUID iid) {
  if (!psa || !iid || !(psa->fFeatures & FADF_HAVEIID)) return E_INVALIDARG;
  sa_set_iid(psa, iid);
  return S_OK;
}
EXPORT HRESULT WINAPI SafeArrayGetIID(SAFEARRAY *psa, GUID *iid) {
  if (!psa || !iid || !(psa->fFeatures & FADF_HAVEIID)) return E_INVALIDARG;
  *iid = *sa_hidden_iid(psa);
  return S_OK;
}
EXPORT HRESULT WINAPI SafeArraySetRecordInfo(SAFEARRAY *psa, IRecordInfo *info) {
  if (!psa || !(psa->fFeatures & FADF_RECORD)) return E_INVALIDARG;
  IRecordInfo **slot = sa_hidden_record(psa);
  IRecordInfo *old = *slot;
  if (info) IRecordInfo_AddRef(info);
  *slot = info;
  if (old) IRecordInfo_Release(old);
  return S_OK;
}
EXPORT HRESULT WINAPI SafeArrayGetRecordInfo(SAFEARRAY *psa, IRecordInfo **info) {
  if (!psa || !info || !(psa->fFeatures & FADF_RECORD)) return E_INVALIDARG;
  *info = *sa_hidden_record(psa);
  if (*info) IRecordInfo_AddRef(*info);
  return S_OK;
}

EXPORT HRESULT WINAPI SafeArrayPtrOfIndex(SAFEARRAY *psa, LONG *indices, void **out) {
  if (!psa || !indices || !out) return E_INVALIDARG;
  if (!psa->cDims) return E_INVALIDARG;
  unsigned long long cell = 0, stride = 1;
  for (USHORT i = 0; i < psa->cDims; i++) {
    const SAFEARRAYBOUND *b = &psa->rgsabound[psa->cDims - 1 - i];
    long long offset = (long long)indices[i] - b->lLbound;
    if (!b->cElements || offset < 0 || offset >= (long long)b->cElements) return DISP_E_BADINDEX;
    cell += (unsigned long long)offset * stride;
    stride *= b->cElements;
    if (stride > 0xFFFFFFFFull) return DISP_E_BADINDEX;
  }
  if (!psa->pvData) return E_INVALIDARG;
  *out = (char *)psa->pvData + cell * psa->cbElements;
  return S_OK;
}

/* Stores one element in the way the array's features say; dest is the slot, src the value (the BSTR or the interface pointer itself, else a pointer to the value). */
static HRESULT sa_store_element(SAFEARRAY *psa, void *dest, const void *src) {
  if (psa->fFeatures & FADF_VARIANT) {
    HRESULT hr = VariantClear((VARIANT *)dest);
    return FAILED(hr) ? hr : VariantCopy((VARIANT *)dest, (VARIANT *)src);
  }
  if (psa->fFeatures & FADF_BSTR) {
    BSTR fresh = NULL;
    if (src) { fresh = SysAllocStringByteLen((const char *)src, SysStringByteLen((BSTR)src)); if (!fresh) return E_OUTOFMEMORY; }
    SysFreeString(*(BSTR *)dest);
    *(BSTR *)dest = fresh;
    return S_OK;
  }
  if (psa->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) {
    IUnknown *fresh = (IUnknown *)src, *old = *(IUnknown **)dest;
    if (fresh) IUnknown_AddRef(fresh);
    *(IUnknown **)dest = fresh;
    if (old) IUnknown_Release(old);
    return S_OK;
  }
  if (psa->fFeatures & FADF_RECORD) {
    IRecordInfo *info = *sa_hidden_record(psa);
    if (!info) return E_INVALIDARG;
    return IRecordInfo_RecordCopy(info, (PVOID)src, dest);
  }
  memcpy(dest, src, psa->cbElements);
  return S_OK;
}

EXPORT HRESULT WINAPI SafeArrayPutElement(SAFEARRAY *psa, LONG *indices, void *value) {
  if (!psa || !indices) return E_INVALIDARG;
  if (!value && !(psa->fFeatures & (FADF_BSTR | FADF_UNKNOWN | FADF_DISPATCH))) return E_INVALIDARG;
  void *slot;
  HRESULT hr = SafeArrayPtrOfIndex(psa, indices, &slot);
  if (FAILED(hr)) return hr;
  return sa_store_element(psa, slot, value);
}
EXPORT HRESULT WINAPI SafeArrayGetElement(SAFEARRAY *psa, LONG *indices, void *value) {
  if (!psa || !indices || !value) return E_INVALIDARG;
  void *slot;
  HRESULT hr = SafeArrayPtrOfIndex(psa, indices, &slot);
  if (FAILED(hr)) return hr;
  if (psa->fFeatures & FADF_VARIANT) { VariantInit((VARIANT *)value); return VariantCopy((VARIANT *)value, (VARIANT *)slot); }
  if (psa->fFeatures & FADF_BSTR) {
    BSTR s = *(BSTR *)slot, copy = NULL;
    if (s) { copy = SysAllocStringByteLen((const char *)s, SysStringByteLen(s)); if (!copy) return E_OUTOFMEMORY; }
    *(BSTR *)value = copy;
    return S_OK;
  }
  if (psa->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) {
    IUnknown *u = *(IUnknown **)slot;
    if (u) IUnknown_AddRef(u);
    *(IUnknown **)value = u;
    return S_OK;
  }
  if (psa->fFeatures & FADF_RECORD) {
    IRecordInfo *info = *sa_hidden_record(psa);
    if (!info) return E_INVALIDARG;
    return IRecordInfo_RecordCopy(info, slot, value);
  }
  memcpy(value, slot, psa->cbElements);
  return S_OK;
}

EXPORT HRESULT WINAPI SafeArrayDestroyData(SAFEARRAY *psa) {
  if (!psa) return E_INVALIDARG;
  if (psa->cLocks) return DISP_E_ARRAYISLOCKED;
  if (!psa->pvData) return S_OK;
  ULONG cells, bytes;
  if (!sa_cell_count(psa, &cells) || !sa_data_size(psa, &bytes)) return E_INVALIDARG;
  sa_clear_elements(psa, 0, cells, TRUE);
  if (psa->fFeatures & (FADF_STATIC | FADF_FIXEDSIZE | FADF_CREATEVECTOR)) memset(psa->pvData, 0, bytes);   /* the data is not ours to free */
  else { oa_free(psa->pvData); psa->pvData = NULL; }
  return S_OK;
}
EXPORT HRESULT WINAPI SafeArrayDestroyDescriptor(SAFEARRAY *psa) {
  if (!psa) return S_OK;
  if (psa->cLocks) return DISP_E_ARRAYISLOCKED;
  if (psa->fFeatures & FADF_RECORD) {
    IRecordInfo *info = *sa_hidden_record(psa);
    *sa_hidden_record(psa) = NULL;
    if (info) IRecordInfo_Release(info);
  }
  oa_free(sa_block(psa));
  return S_OK;
}
EXPORT HRESULT WINAPI SafeArrayDestroy(SAFEARRAY *psa) {
  if (!psa) return S_OK;
  if (psa->cLocks) return DISP_E_ARRAYISLOCKED;
  HRESULT hr = SafeArrayDestroyData(psa);
  if (FAILED(hr)) return hr;
  return SafeArrayDestroyDescriptor(psa);
}

EXPORT SAFEARRAY *WINAPI SafeArrayCreateEx(VARTYPE vt, UINT dims, SAFEARRAYBOUND *bounds, PVOID extra) {
  if (!bounds || !dims || dims >= 0x10000) return NULL;
  if (vt == VT_RECORD && !extra) return NULL;
  SAFEARRAY *psa;
  if (FAILED(SafeArrayAllocDescriptorEx(vt, dims, &psa))) return NULL;
  psa->fFeatures |= sa_type_flags(vt);
  if (vt == VT_RECORD) {
    IRecordInfo *info = (IRecordInfo *)extra;
    ULONG size = 0;
    SafeArraySetRecordInfo(psa, info);
    IRecordInfo_GetSize(info, &size);      /* (the result is not checked on Windows either) */
    psa->cbElements = size;
  } else if (extra && (vt == VT_UNKNOWN || vt == VT_DISPATCH)) sa_set_iid(psa, (const GUID *)extra);
  if (!psa->cbElements && vt != VT_RECORD) { SafeArrayDestroyDescriptor(psa); return NULL; }
  for (UINT i = 0; i < dims; i++) psa->rgsabound[i] = bounds[dims - 1 - i];
  if (FAILED(SafeArrayAllocData(psa))) { SafeArrayDestroyDescriptor(psa); return NULL; }
  return psa;
}
EXPORT SAFEARRAY *WINAPI SafeArrayCreate(VARTYPE vt, UINT dims, SAFEARRAYBOUND *bounds) { return SafeArrayCreateEx(vt, dims, bounds, NULL); }

EXPORT SAFEARRAY *WINAPI SafeArrayCreateVectorEx(VARTYPE vt, LONG lower, ULONG count, PVOID extra) {
  ULONG size = sa_vt_size(vt);
  IRecordInfo *info = NULL;
  if (vt == VT_RECORD) {
    if (!extra) return NULL;
    info = (IRecordInfo *)extra;
    IRecordInfo_GetSize(info, &size);
  }
  if (!size) return NULL;
  unsigned long long data = (unsigned long long)count * size;
  if (data > SA_MAX_BYTES - sizeof(SAFEARRAY) - SA_HIDDEN_SIZE) return NULL;
  char *block = oa_calloc(SA_HIDDEN_SIZE + sizeof(SAFEARRAY) + (SIZE_T)data);
  if (!block) return NULL;
  SAFEARRAY *psa = (SAFEARRAY *)(block + SA_HIDDEN_SIZE);
  psa->cDims = 1;
  psa->fFeatures = FADF_CREATEVECTOR | sa_type_flags(vt);
  sa_set_features(psa, vt);
  psa->cbElements = size;
  psa->pvData = psa + 1;
  psa->rgsabound[0].cElements = count;
  psa->rgsabound[0].lLbound = lower;
  if (vt == VT_RECORD) SafeArraySetRecordInfo(psa, info);
  else if (extra && (vt == VT_UNKNOWN || vt == VT_DISPATCH)) sa_set_iid(psa, (const GUID *)extra);
  return psa;
}
EXPORT SAFEARRAY *WINAPI SafeArrayCreateVector(VARTYPE vt, LONG lower, ULONG count) { return SafeArrayCreateVectorEx(vt, lower, count, NULL); }

EXPORT HRESULT WINAPI SafeArrayCopyData(SAFEARRAY *source, SAFEARRAY *dest) {
  if (!source || !dest) return E_INVALIDARG;
  if (source->cDims != dest->cDims) return E_INVALIDARG;
  for (USHORT i = 0; i < source->cDims; i++)
    if (source->rgsabound[i].cElements != dest->rgsabound[i].cElements) return E_INVALIDARG;
  ULONG cells;
  if (!sa_cell_count(source, &cells)) return E_INVALIDARG;
  if (!cells) return S_OK;
  if (!source->pvData || !dest->pvData) return E_INVALIDARG;
  if (source->cbElements != dest->cbElements) return E_INVALIDARG;
  if (source == dest) return S_OK;
  sa_clear_elements(dest, 0, cells, FALSE);     /* (records in the destination are not cleared, as on Windows) */
  for (ULONG i = 0; i < cells; i++) {
    unsigned char *from = (unsigned char *)source->pvData + (SIZE_T)i * source->cbElements, *to = (unsigned char *)dest->pvData + (SIZE_T)i * dest->cbElements;
    HRESULT hr = S_OK;
    if (source->fFeatures & FADF_VARIANT) { VariantInit((VARIANT *)to); hr = VariantCopy((VARIANT *)to, (VARIANT *)from); }
    else if (source->fFeatures & FADF_BSTR) {
      BSTR s = *(BSTR *)from;
      *(BSTR *)to = NULL;
      if (s) { *(BSTR *)to = SysAllocStringByteLen((const char *)s, SysStringByteLen(s)); if (!*(BSTR *)to) hr = E_OUTOFMEMORY; }
    } else if (source->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) {
      IUnknown *u = *(IUnknown **)from;
      if (u) IUnknown_AddRef(u);
      *(IUnknown **)to = u;
    } else if (source->fFeatures & FADF_RECORD) {
      IRecordInfo *info = *sa_hidden_record(source);
      if (info) hr = IRecordInfo_RecordCopy(info, from, to);
    } else memcpy(to, from, source->cbElements);
    if (FAILED(hr)) return hr;
  }
  return S_OK;
}

EXPORT HRESULT WINAPI SafeArrayCopy(SAFEARRAY *psa, SAFEARRAY **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!psa) return S_OK;
  if (!psa->cbElements || !psa->cDims) return E_INVALIDARG;
  SAFEARRAY *copy;
  VARTYPE vt;
  HRESULT hr;
  if (SUCCEEDED(SafeArrayGetVartype(psa, &vt))) hr = SafeArrayAllocDescriptorEx(vt, psa->cDims, &copy);
  else hr = SafeArrayAllocDescriptor(psa->cDims, &copy);
  if (FAILED(hr)) return hr;
  if (psa->fFeatures & FADF_RECORD) {
    IRecordInfo *info = *sa_hidden_record(psa);
    if (info) SafeArraySetRecordInfo(copy, info);
  }
  if (psa->fFeatures & FADF_HAVEIID) sa_set_iid(copy, sa_hidden_iid(psa));
  copy->fFeatures |= psa->fFeatures & (FADF_BSTR | FADF_UNKNOWN | FADF_DISPATCH | FADF_VARIANT);
  copy->cbElements = psa->cbElements;
  for (USHORT i = 0; i < psa->cDims; i++) copy->rgsabound[i] = psa->rgsabound[i];
  hr = SafeArrayAllocData(copy);
  if (SUCCEEDED(hr)) hr = SafeArrayCopyData(psa, copy);
  if (FAILED(hr)) { SafeArrayDestroy(copy); return hr; }
  *out = copy;
  return S_OK;
}

EXPORT HRESULT WINAPI SafeArrayRedim(SAFEARRAY *psa, SAFEARRAYBOUND *bound) {
  if (!psa || !bound || !psa->cDims) return E_INVALIDARG;
  if (psa->cLocks || (psa->fFeatures & FADF_FIXEDSIZE)) return DISP_E_ARRAYISLOCKED;
  SAFEARRAYBOUND *last = &psa->rgsabound[0];
  unsigned long long stride = 1;
  for (USHORT i = 1; i < psa->cDims; i++) {
    stride *= psa->rgsabound[i].cElements;
    if (stride > 0xFFFFFFFFull) return E_OUTOFMEMORY;
  }
  unsigned long long old_cells = stride * last->cElements, new_cells = stride * bound->cElements;
  if (new_cells > 0xFFFFFFFFull || new_cells * psa->cbElements > SA_MAX_BYTES) return E_OUTOFMEMORY;
  if (psa->pvData && bound->cElements < last->cElements) sa_clear_elements(psa, (ULONG)new_cells, (ULONG)(old_cells - new_cells), TRUE);
  if (bound->cElements != last->cElements || !psa->pvData) {
    void *data = oa_realloc(psa->pvData, (SIZE_T)(new_cells * psa->cbElements));
    if (!data) return E_OUTOFMEMORY;
    if (psa->pvData && new_cells > old_cells) memset((char *)data + old_cells * psa->cbElements, 0, (SIZE_T)((new_cells - old_cells) * psa->cbElements));
    else if (!psa->pvData) memset(data, 0, (SIZE_T)(new_cells * psa->cbElements));
    psa->pvData = data;
  }
  last->lLbound = bound->lLbound;
  last->cElements = bound->cElements;
  return S_OK;
}

EXPORT HRESULT WINAPI BstrFromVector(SAFEARRAY *psa, BSTR *out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!psa || psa->cbElements != 1 || psa->cDims != 1) return E_INVALIDARG;
  *out = SysAllocStringByteLen((const char *)psa->pvData, psa->rgsabound[0].cElements);
  return *out ? S_OK : E_OUTOFMEMORY;
}
EXPORT HRESULT WINAPI VectorFromBstr(BSTR bstr, SAFEARRAY **out) {
  if (!out) return E_INVALIDARG;
  SAFEARRAYBOUND bound = { SysStringByteLen(bstr), 0 };
  *out = SafeArrayCreate(VT_UI1, 1, &bound);
  if (!*out) return E_OUTOFMEMORY;
  if (bound.cElements) memcpy((*out)->pvData, bstr, bound.cElements);
  return S_OK;
}
