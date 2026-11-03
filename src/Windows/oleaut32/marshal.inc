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

/* User marshalling of the automation types (BSTR, VARIANT, SAFEARRAY) in the NDR wire format of Windows (MS-OAUT: FLAGGED_WORD_BLOB, wireVARIANT, wireSAFEARRAY).
 * The stubs call <Type>_UserSize for the buffer size, <Type>_UserMarshal to fill it, <Type>_UserUnmarshal to read it back and <Type>_UserFree to release what
 * the unmarshalling allocated. The 64 variants are the same functions (the wire format is the same on x64).
 *
 * The functions get a buffer pointer only (no length), so what is read from the wire is checked for consistency (counts agree with each other and with the
 * bounds, element sizes fit the type, nesting is limited) but not against the end of the buffer; a wire form that does not hold together raises
 * RPC_X_BAD_STUB_DATA, a failing interface marshalling raises the HRESULT, as Windows does. Alignment is relative to the (8 aligned) buffer pointer. The padding
 * bytes are never written, as on Windows.
 *
 * Interface pointers (VT_UNKNOWN, VT_DISPATCH, interface arrays, record info) are marshalled with ole32's CoMarshalInterface, found by name when needed
 * (through a stream that lives here), so that this works as soon as ole32 implements it; without it the call raises E_NOTIMPL. */

#define MA_MAX_DEPTH 24                 /* nesting of VARIANT/SAFEARRAY inside each other */
#define MA_MAX_BYTES 0x7ffffff0u        /* the largest size accepted from the wire or produced by the size functions */
#define MA_MAX_IFACE 0x4000000u         /* the largest marshalled interface accepted from the wire */
#define MA_MAX_DIMS 64                  /* the most dimensions of an array on the wire */
#define MA_BAD_DATA 1783                /* RPC_X_BAD_STUB_DATA */

/* ---- failure, sizes and the wire cursor -------------------------------------------------------------------------------------------------------------------- */
static void ma_raise(DWORD code) __attribute__((noreturn));
static void ma_raise(DWORD code) { for (;;) RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, NULL); }
static void ma_bad(void) __attribute__((noreturn));
static void ma_bad(void) { ma_raise(MA_BAD_DATA); }

/* size + add, raising when the result leaves the supported range (the size functions return 32 bits) */
static ULONG ma_len(ULONG size, unsigned long long add) {
  const unsigned long long r = (unsigned long long)size + add;
  if (r > MA_MAX_BYTES) ma_bad();
  return (ULONG)r;
}
static ULONG ma_alen(ULONG size, ULONG a) { return ma_len(size, (a - (size & (a - 1))) & (a - 1)); }

/* The one pointer to integer conversion: aligning the cursor in the wire buffer (RPC buffers are 8 aligned, so this equals aligning the offset). */
static unsigned char *ma_align(unsigned char *p, UINT_PTR a) { return (unsigned char *)(((UINT_PTR)p + (a - 1)) & ~(a - 1)); }
static void ma_w16(unsigned char **p, unsigned v) { const unsigned short s = (unsigned short)v; memcpy(*p, &s, 2); *p += 2; }
static void ma_w32(unsigned char **p, DWORD v) { memcpy(*p, &v, 4); *p += 4; }
static void ma_wn(unsigned char **p, const void *src, SIZE_T n) { memcpy(*p, src, n); *p += n; }
static unsigned ma_r16(unsigned char **p) { unsigned short s; memcpy(&s, *p, 2); *p += 2; return s; }
static DWORD ma_r32(unsigned char **p) { DWORD v; memcpy(&v, *p, 4); *p += 4; return v; }
/* The id that stands for a non-null pointer on the wire (any non-zero value does; Windows sends the pointer plus one). */
static DWORD ma_id(const void *ptr) { return (DWORD)(UINT_PTR)ptr + 1; }
static DWORD ma_id_nn(const void *ptr) { return ptr ? ma_id(ptr) : 0; }
static DWORD ma_ctx(const ULONG *flags) { return flags ? LOWORD(*flags) : MSHCTX_LOCAL; }

/* ---- an in-memory stream, so that ole32's CoMarshalInterface needs no more than the interface pointer functions -------------------------------------------- */
typedef struct { IStream iface; LONG ref; BYTE *data; ULONG size, cap, pos; } MaStream;
/* `iface` is the first member, so the stream pointer ole32 hands back is the MaStream. */
static MaStream *ma_st(IStream *s) { return (MaStream *)s; }

static HRESULT WINAPI ma_st_QueryInterface(IStream *s, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ISequentialStream) || IsEqualGUID(riid, &IID_IStream)) {
    InterlockedIncrement(&ma_st(s)->ref);
    *out = s;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI ma_st_AddRef(IStream *s) { return (ULONG)InterlockedIncrement(&ma_st(s)->ref); }
static ULONG WINAPI ma_st_Release(IStream *s) {
  MaStream *st = ma_st(s);
  const LONG n = InterlockedDecrement(&st->ref);
  if (n == 0) { oa_free(st->data); oa_free(st); }
  return (ULONG)n;
}
static HRESULT WINAPI ma_st_Read(IStream *s, void *buffer, ULONG cb, ULONG *read) {
  MaStream *st = ma_st(s);
  const ULONG n = st->pos < st->size ? (cb < st->size - st->pos ? cb : st->size - st->pos) : 0;
  if (n) memcpy(buffer, st->data + st->pos, n);
  st->pos += n;
  if (read) *read = n;
  return n == cb ? S_OK : S_FALSE;
}
/* make the stream `size` bytes long, at least (zero filled) */
static HRESULT ma_st_grow(MaStream *st, ULONG size) {
  if (size > MA_MAX_IFACE) return STG_E_MEDIUMFULL;
  if (size > st->cap) {
    ULONG cap = st->cap ? st->cap : 256;
    while (cap < size) cap *= 2;
    BYTE *data = oa_realloc(st->data, cap);
    if (!data) return E_OUTOFMEMORY;
    memset(data + st->cap, 0, cap - st->cap);
    st->data = data;
    st->cap = cap;
  }
  if (size > st->size) st->size = size;
  return S_OK;
}
static HRESULT WINAPI ma_st_Write(IStream *s, const void *buffer, ULONG cb, ULONG *written) {
  MaStream *st = ma_st(s);
  if (cb > MA_MAX_IFACE || st->pos > MA_MAX_IFACE - cb) return STG_E_MEDIUMFULL;
  HRESULT hr = ma_st_grow(st, st->pos + cb);
  if (FAILED(hr)) return hr;
  memcpy(st->data + st->pos, buffer, cb);
  st->pos += cb;
  if (written) *written = cb;
  return S_OK;
}
static HRESULT WINAPI ma_st_Seek(IStream *s, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *newpos) {
  MaStream *st = ma_st(s);
  long long base;
  switch (origin) {
    case STREAM_SEEK_SET: base = 0; break;
    case STREAM_SEEK_CUR: base = st->pos; break;
    case STREAM_SEEK_END: base = st->size; break;
    default: return STG_E_INVALIDFUNCTION;
  }
  if ((move.QuadPart > 0 && move.QuadPart > (long long)MA_MAX_IFACE) || (move.QuadPart < 0 && move.QuadPart < -(long long)MA_MAX_IFACE)) return STG_E_INVALIDFUNCTION;
  base += move.QuadPart;
  if (base < 0 || base > (long long)MA_MAX_IFACE) return STG_E_INVALIDFUNCTION;
  st->pos = (ULONG)base;
  if (newpos) newpos->QuadPart = st->pos;
  return S_OK;
}
static HRESULT WINAPI ma_st_SetSize(IStream *s, ULARGE_INTEGER size) {
  MaStream *st = ma_st(s);
  if (size.QuadPart > MA_MAX_IFACE) return STG_E_MEDIUMFULL;
  if (size.QuadPart < st->size) { st->size = (ULONG)size.QuadPart; return S_OK; }
  return ma_st_grow(st, (ULONG)size.QuadPart);
}
static HRESULT WINAPI ma_st_CopyTo(IStream *s, IStream *dest, ULARGE_INTEGER cb, ULARGE_INTEGER *read, ULARGE_INTEGER *written) {
  MaStream *st = ma_st(s);
  const ULONG left = st->pos < st->size ? st->size - st->pos : 0;
  const ULONG n = cb.QuadPart < left ? (ULONG)cb.QuadPart : left;
  ULONG w = 0;
  HRESULT hr = n ? IStream_Write(dest, st->data + st->pos, n, &w) : S_OK;
  st->pos += n;
  if (read) read->QuadPart = n;
  if (written) written->QuadPart = w;
  return hr;
}
static HRESULT WINAPI ma_st_Commit(IStream *s, DWORD flags) { (void)s; (void)flags; return S_OK; }
static HRESULT WINAPI ma_st_Revert(IStream *s) { (void)s; return S_OK; }
static HRESULT WINAPI ma_st_LockRegion(IStream *s, ULARGE_INTEGER offset, ULARGE_INTEGER length, DWORD type) { (void)s; (void)offset; (void)length; (void)type; return STG_E_INVALIDFUNCTION; }
static HRESULT WINAPI ma_st_Stat(IStream *s, STATSTG *stat, DWORD flags) {
  (void)flags;
  if (!stat) return STG_E_INVALIDPOINTER;
  memset(stat, 0, sizeof(*stat));
  stat->type = STGTY_STREAM;
  stat->cbSize.QuadPart = ma_st(s)->size;
  stat->grfMode = STGM_READWRITE;
  return S_OK;
}
static HRESULT WINAPI ma_st_Clone(IStream *s, IStream **out) { (void)s; if (out) *out = NULL; return E_NOTIMPL; }
static IStreamVtbl ma_stream_vtbl = {
  ma_st_QueryInterface, ma_st_AddRef, ma_st_Release, ma_st_Read, ma_st_Write, ma_st_Seek, ma_st_SetSize, ma_st_CopyTo, ma_st_Commit, ma_st_Revert,
  ma_st_LockRegion, ma_st_LockRegion, ma_st_Stat, ma_st_Clone,
};
/* a stream with one reference, holding a copy of the `n` bytes at `init` (none: empty) */
static MaStream *ma_stream_new(const void *init, ULONG n) {
  MaStream *st = oa_calloc(sizeof(*st));
  if (!st) return NULL;
  st->iface.lpVtbl = &ma_stream_vtbl;
  st->ref = 1;
  if (n) {
    if (FAILED(ma_st_grow(st, n))) { oa_free(st->data); oa_free(st); return NULL; }
    memcpy(st->data, init, n);
  }
  return st;
}

/* ---- interface pointers through ole32 ------------------------------------------------------------------------------------------------------------------ */
typedef HRESULT (WINAPI *ma_pfn_size)(ULONG *, REFIID, LPUNKNOWN, DWORD, LPVOID, DWORD);
typedef HRESULT (WINAPI *ma_pfn_marshal)(LPSTREAM, REFIID, LPUNKNOWN, DWORD, LPVOID, DWORD);
typedef HRESULT (WINAPI *ma_pfn_unmarshal)(LPSTREAM, REFIID, LPVOID *);
typedef HRESULT (WINAPI *ma_pfn_release)(LPSTREAM);
static void *ma_ole32(const char *name) {
  HMODULE m = GetModuleHandleW(L"ole32.dll");
  if (!m) m = LoadLibraryW(L"ole32.dll");
  return m ? (void *)GetProcAddress(m, name) : NULL;
}

/* An upper bound for the marshalled form of the interface pointer. */
static ULONG ma_marshal_bound(const ULONG *flags, REFIID iid, IUnknown *unk) {
  ma_pfn_size fn = (ma_pfn_size)ma_ole32("CoGetMarshalSizeMax");
  ULONG n = 0;
  if (!fn) ma_raise((DWORD)E_NOTIMPL);
  HRESULT hr = fn(&n, iid, unk, ma_ctx(flags), NULL, MSHLFLAGS_NORMAL);
  if (FAILED(hr)) ma_raise((DWORD)hr);
  if (!n) ma_raise((DWORD)E_NOTIMPL);     /* (an ole32 that does nothing) */
  if (n > MA_MAX_IFACE) ma_bad();
  return n;
}
/* The size of the body of an interface pointer: ulCntData, the conformance count and the OBJREF bytes. */
static ULONG ma_iface_body_size(const ULONG *flags, ULONG size, REFIID iid, IUnknown *unk) {
  size = ma_alen(size, 4);
  return ma_len(ma_len(size, 8), ma_marshal_bound(flags, iid, unk));
}
static unsigned char *ma_iface_body_marshal(const ULONG *flags, unsigned char *p, REFIID iid, IUnknown *unk) {
  const ULONG bound = ma_marshal_bound(flags, iid, unk);
  ma_pfn_marshal marshal = (ma_pfn_marshal)ma_ole32("CoMarshalInterface");
  if (!marshal) ma_raise((DWORD)E_NOTIMPL);
  MaStream *st = ma_stream_new(NULL, 0);
  if (!st) ma_raise((DWORD)E_OUTOFMEMORY);
  HRESULT hr = marshal(&st->iface, iid, unk, ma_ctx(flags), NULL, MSHLFLAGS_NORMAL);
  if (SUCCEEDED(hr) && (!st->size || st->size > bound)) {
    /* nothing written (an ole32 that does nothing) or more than the size function allowed for: give the marshalled reference back */
    ma_pfn_release release = (ma_pfn_release)ma_ole32("CoReleaseMarshalData");
    if (st->size && release) { st->pos = 0; release(&st->iface); }
    hr = st->size ? MA_BAD_DATA : E_NOTIMPL;
  }
  if (FAILED(hr) || hr == MA_BAD_DATA) { ma_st_Release(&st->iface); ma_raise((DWORD)hr); }
  const ULONG n = st->size;
  p = ma_align(p, 4);
  ma_w32(&p, n);
  ma_w32(&p, n);
  ma_wn(&p, st->data, n);
  ma_st_Release(&st->iface);
  return p;
}
/* Reads the body written by the function above and unmarshals the interface (*out gets one reference). */
static unsigned char *ma_iface_body_unmarshal(unsigned char *p, REFIID iid, void **out) {
  p = ma_align(p, 4);
  const DWORD n = ma_r32(&p), n2 = ma_r32(&p);
  if (n != n2 || !n || n > MA_MAX_IFACE) ma_bad();
  ma_pfn_unmarshal unmarshal = (ma_pfn_unmarshal)ma_ole32("CoUnmarshalInterface");
  if (!unmarshal) ma_raise((DWORD)E_NOTIMPL);
  MaStream *st = ma_stream_new(p, n);
  if (!st) ma_raise((DWORD)E_OUTOFMEMORY);
  void *itf = NULL;
  HRESULT hr = unmarshal(&st->iface, iid, &itf);
  ma_st_Release(&st->iface);
  if (FAILED(hr)) ma_raise((DWORD)hr);
  if (!itf) ma_raise((DWORD)E_NOTIMPL);
  *out = itf;
  return p + n;
}

/* ---- BSTR: FLAGGED_WORD_BLOB { clSize (words), fFlags (byte length, 0xffffffff for NULL), clSize, data } ---------------------------------------------------- */
static ULONG ma_bstr_size(ULONG size, BSTR b) {
  size = ma_alen(size, 4);
  size = ma_len(size, 12);
  return b ? ma_len(size, ((unsigned long long)SysStringByteLen(b) + 1) & ~1ull) : size;
}
static unsigned char *ma_bstr_marshal(unsigned char *p, BSTR b) {
  p = ma_align(p, 4);
  const DWORD len = SysStringByteLen(b), words = (DWORD)(((unsigned long long)len + 1) >> 1);
  ma_w32(&p, b ? words : 0);
  ma_w32(&p, b ? len : 0xffffffffu);
  ma_w32(&p, b ? words : 0);
  /* (an odd length copies the byte after the string, which the allocation always has) */
  if (b) ma_wn(&p, b, (SIZE_T)words * 2);
  return p;
}
static unsigned char *ma_bstr_unmarshal(unsigned char *p, BSTR *out) {
  p = ma_align(p, 4);
  const DWORD words = ma_r32(&p), len = ma_r32(&p), words2 = ma_r32(&p);
  BSTR b = NULL;
  if (len == 0xffffffffu) {
    if (words || words2) ma_bad();
  } else {
    if (len > 0x7ffffff0u || words != words2 || words != (len + 1) / 2) ma_bad();
    b = SysAllocStringByteLen((LPCSTR)p, len);
    if (!b) ma_raise((DWORD)E_OUTOFMEMORY);
    p += (SIZE_T)words * 2;
  }
  SysFreeString(*out);
  *out = b;
  return p;
}

EXPORT ULONG WINAPI BSTR_UserSize(ULONG *flags, ULONG start, BSTR *b) { (void)flags; return ma_bstr_size(start, *b); }
EXPORT unsigned char * WINAPI BSTR_UserMarshal(ULONG *flags, unsigned char *buffer, BSTR *b) { (void)flags; return ma_bstr_marshal(buffer, *b); }
EXPORT unsigned char * WINAPI BSTR_UserUnmarshal(ULONG *flags, unsigned char *buffer, BSTR *b) { (void)flags; return ma_bstr_unmarshal(buffer, b); }
EXPORT void WINAPI BSTR_UserFree(ULONG *flags, BSTR *b) { (void)flags; if (*b) { SysFreeString(*b); *b = NULL; } }
EXPORT ULONG WINAPI BSTR_UserSize64(ULONG *flags, ULONG start, BSTR *b) { return BSTR_UserSize(flags, start, b); }
EXPORT unsigned char * WINAPI BSTR_UserMarshal64(ULONG *flags, unsigned char *buffer, BSTR *b) { return BSTR_UserMarshal(flags, buffer, b); }
EXPORT unsigned char * WINAPI BSTR_UserUnmarshal64(ULONG *flags, unsigned char *buffer, BSTR *b) { return BSTR_UserUnmarshal(flags, buffer, b); }
EXPORT void WINAPI BSTR_UserFree64(ULONG *flags, BSTR *b) { BSTR_UserFree(flags, b); }

/* ---- records: _wireBRECORD { fFlags, clSize, pRecInfo (unique), pRecord (unique, clSize bytes) } -------------------------------------------------------- */
static ULONG ma_rec_size(const ULONG *flags, ULONG size, IRecordInfo *info, ULONG cb) {
  size = ma_len(ma_alen(size, 4), 16);
  size = ma_iface_body_size(flags, size, &IID_IRecordInfo, (IUnknown *)info);
  return ma_len(ma_alen(size, 4), 4ull + cb);
}
static unsigned char *ma_rec_marshal(const ULONG *flags, unsigned char *p, IRecordInfo *info, const void *rec, ULONG cb) {
  p = ma_align(p, 4);
  ma_w32(&p, 0);
  ma_w32(&p, cb);
  ma_w32(&p, ma_id(info));
  ma_w32(&p, ma_id(rec));
  p = ma_iface_body_marshal(flags, p, &IID_IRecordInfo, (IUnknown *)info);
  p = ma_align(p, 4);
  ma_w32(&p, cb);
  ma_wn(&p, rec, cb);
  return p;
}
/* Reads a record body; the record info (a reference for the caller) is returned, and the bytes are copied to `dest` (cb of them: they must be what the wire says). */
static unsigned char *ma_rec_unmarshal(unsigned char *p, IRecordInfo **info_out, void *dest, ULONG cb) {
  p = ma_align(p, 4);
  ma_r32(&p);
  const DWORD size = ma_r32(&p), info_id = ma_r32(&p), rec_id = ma_r32(&p);
  if (size != cb || !info_id || !rec_id) ma_bad();
  void *info = NULL;
  p = ma_iface_body_unmarshal(p, &IID_IRecordInfo, &info);
  p = ma_align(p, 4);
  if (ma_r32(&p) != cb) { IRecordInfo_Release((IRecordInfo *)info); ma_bad(); }
  memcpy(dest, p, cb);
  *info_out = (IRecordInfo *)info;
  return p + cb;
}

/* ---- SAFEARRAY ---------------------------------------------------------------------------------------------------------------------------------------- */
/* The descriptor is allocated with the VARTYPE in the DWORD just before it (safearray.inc); the one place that is needed here. */
static DWORD *ma_hidden_vt(SAFEARRAY *psa) { return (DWORD *)psa - 1; }
#define MA_FADF_LOCAL (FADF_AUTO | FADF_STATIC | FADF_EMBEDDED | FADF_FIXEDSIZE)      /* features that describe where the descriptor lives, not what it is */

static SF_TYPE ma_sf_from(BOOL have_vt, VARTYPE vt, unsigned features, ULONG cb) {
  if (!have_vt) {
    if (features & FADF_VARIANT) return SF_VARIANT;
    switch (cb) {
      case 1: vt = VT_I1; break;
      case 2: vt = VT_I2; break;
      case 4: vt = VT_I4; break;
      case 8: vt = VT_I8; break;
      default: return SF_ERROR;
    }
  }
  if (features & FADF_HAVEIID) return SF_HAVEIID;
  switch (vt) {
    case VT_I1: case VT_UI1: return SF_I1;
    case VT_BOOL: case VT_I2: case VT_UI2: return SF_I2;
    case VT_INT: case VT_UINT: case VT_I4: case VT_UI4: case VT_R4: case VT_ERROR: return SF_I4;
    case VT_DATE: case VT_CY: case VT_R8: case VT_I8: case VT_UI8: return SF_I8;
    case VT_BSTR: return SF_BSTR;
    case VT_DISPATCH: return SF_DISPATCH;
    case VT_VARIANT: return SF_VARIANT;
    case VT_UNKNOWN: return SF_UNKNOWN;
    case VT_RECORD: return SF_RECORD;
    default: return SF_ERROR;
  }
}
static unsigned ma_sf_align(SF_TYPE sf) { return sf == SF_I1 ? 1 : sf == SF_I2 ? 2 : sf == SF_I8 ? 8 : 4; }
static BOOL ma_sf_is_ptr(SF_TYPE sf) { return sf == SF_BSTR || sf == SF_UNKNOWN || sf == SF_DISPATCH || sf == SF_HAVEIID; }

/* The wire type of a local array, with the checks that its descriptor can be walked: element size fits, data present, count fits. */
static void ma_sa_prepare(LPSAFEARRAY psa, SF_TYPE *sf, ULONG *cells) {
  VARTYPE vt = 0;
  const BOOL have_vt = SUCCEEDED(SafeArrayGetVartype(psa, &vt));
  *sf = ma_sf_from(have_vt, vt, psa->fFeatures, psa->cbElements);
  if (!psa->cDims || *sf == SF_ERROR) ma_raise((DWORD)E_INVALIDARG);
  unsigned long long n = 1;
  for (USHORT i = 0; i < psa->cDims; i++) {
    n *= psa->rgsabound[i].cElements;
    if (n > MA_MAX_BYTES) ma_bad();
  }
  if (*sf == SF_VARIANT) { if (psa->cbElements != sizeof(VARIANT)) ma_raise((DWORD)E_INVALIDARG); }
  else if (ma_sf_is_ptr(*sf)) { if (psa->cbElements != sizeof(void *)) ma_raise((DWORD)E_INVALIDARG); }
  else if (*sf == SF_RECORD) { if (!psa->cbElements) ma_raise((DWORD)E_INVALIDARG); }
  else if (psa->cbElements != ma_sf_align(*sf)) ma_raise((DWORD)E_INVALIDARG);
  if (n * psa->cbElements > MA_MAX_BYTES || (n && !psa->pvData)) ma_bad();
  *cells = (ULONG)n;
}
static BYTE *ma_elem(LPSAFEARRAY psa, ULONG i) { return (BYTE *)psa->pvData + (SIZE_T)i * psa->cbElements; }
static void *ma_elem_ptr(LPSAFEARRAY psa, ULONG i) { void *p; memcpy(&p, ma_elem(psa, i), sizeof(p)); return p; }
/* the element of a VARIANT array (the data holds VARIANTs; alignment of the allocation) */
static VARIANT *ma_elem_var(LPSAFEARRAY psa, ULONG i) { return (VARIANT *)ma_elem(psa, i); }
static const IID *ma_sa_iid(LPSAFEARRAY psa, SF_TYPE sf, GUID *buf) {
  if (sf == SF_HAVEIID) { if (FAILED(SafeArrayGetIID(psa, buf))) ma_raise((DWORD)E_INVALIDARG); return buf; }
  return sf == SF_DISPATCH ? &IID_IDispatch : &IID_IUnknown;
}
/* the record info of a record array (a reference for the caller) */
static IRecordInfo *ma_sa_record_info(LPSAFEARRAY psa) {
  IRecordInfo *info = NULL;
  if (FAILED(SafeArrayGetRecordInfo(psa, &info)) || !info) ma_raise((DWORD)E_INVALIDARG);
  return info;
}

static ULONG ma_var_size(ULONG *flags, ULONG size, VARIANT *v, int depth);
static unsigned char *ma_var_marshal(ULONG *flags, unsigned char *p, VARIANT *v, int depth);
static unsigned char *ma_var_unmarshal(ULONG *flags, unsigned char *p, VARIANT *v, int depth);

static ULONG ma_sa_size(ULONG *flags, ULONG size, LPSAFEARRAY psa, int depth) {
  if (depth > MA_MAX_DEPTH) ma_bad();
  size = ma_len(ma_alen(size, 4), 4);
  if (!psa) return size;
  SF_TYPE sf;
  ULONG cells;
  ma_sa_prepare(psa, &sf, &cells);
  size = ma_len(size, 28 + (sf == SF_HAVEIID ? 16 : 0) + 8ull * psa->cDims + 4);
  if (ma_sf_is_ptr(sf)) {
    GUID guid;
    const IID *iid = sf == SF_BSTR ? NULL : ma_sa_iid(psa, sf, &guid);
    size = ma_len(size, 4ull * cells);
    for (ULONG i = 0; i < cells; i++) {
      void *ptr = ma_elem_ptr(psa, i);
      if (sf == SF_BSTR) size = ma_bstr_size(size, (BSTR)ptr);
      else if (ptr) size = ma_iface_body_size(flags, size, iid, (IUnknown *)ptr);
    }
  } else if (sf == SF_VARIANT) {
    size = ma_len(size, 4ull * cells);
    for (ULONG i = 0; i < cells; i++) size = ma_var_size(flags, size, ma_elem_var(psa, i), depth + 1);
  } else if (sf == SF_RECORD) {
    IRecordInfo *info = cells ? ma_sa_record_info(psa) : NULL;
    size = ma_len(size, 4ull * cells);
    for (ULONG i = 0; i < cells; i++) size = ma_rec_size(flags, size, info, psa->cbElements);
    if (info) IRecordInfo_Release(info);
  } else {
    size = ma_len(ma_alen(size, ma_sf_align(sf)), (unsigned long long)cells * psa->cbElements);
  }
  return size;
}

static unsigned char *ma_sa_marshal(ULONG *flags, unsigned char *p, LPSAFEARRAY psa, int depth) {
  if (depth > MA_MAX_DEPTH) ma_bad();
  p = ma_align(p, 4);
  if (!psa) { ma_w32(&p, 0); return p; }
  SF_TYPE sf;
  ULONG cells;
  ma_sa_prepare(psa, &sf, &cells);
  VARTYPE vt = 0;
  if ((psa->fFeatures & FADF_HAVEIID) || FAILED(SafeArrayGetVartype(psa, &vt))) vt = 0;
  ma_w32(&p, ma_id(psa));
  ma_w32(&p, psa->cDims);
  ma_w16(&p, psa->cDims);
  ma_w16(&p, psa->fFeatures);
  ma_w32(&p, ma_sf_is_ptr(sf) ? 4 : sf == SF_VARIANT ? 16 : psa->cbElements);
  ma_w16(&p, psa->cLocks);
  ma_w16(&p, vt);
  ma_w32(&p, sf);
  ma_w32(&p, cells);
  ma_w32(&p, ma_id(psa->pvData));
  if (sf == SF_HAVEIID) {
    GUID guid;
    ma_sa_iid(psa, sf, &guid);
    ma_wn(&p, &guid, sizeof(guid));
  }
  /* the bounds go in natural order: the descriptor has the last dimension first */
  for (USHORT i = 0; i < psa->cDims; i++) ma_wn(&p, &psa->rgsabound[psa->cDims - 1 - i], sizeof(SAFEARRAYBOUND));
  ma_w32(&p, cells);
  if (ma_sf_is_ptr(sf)) {
    GUID guid;
    const IID *iid = sf == SF_BSTR ? NULL : ma_sa_iid(psa, sf, &guid);
    for (ULONG i = 0; i < cells; i++) ma_w32(&p, sf == SF_BSTR ? ma_id(ma_elem_ptr(psa, i)) : ma_id_nn(ma_elem_ptr(psa, i)));
    for (ULONG i = 0; i < cells; i++) {
      void *ptr = ma_elem_ptr(psa, i);
      if (sf == SF_BSTR) p = ma_bstr_marshal(p, (BSTR)ptr);
      else if (ptr) p = ma_iface_body_marshal(flags, p, iid, (IUnknown *)ptr);
    }
  } else if (sf == SF_VARIANT) {
    for (ULONG i = 0; i < cells; i++) ma_w32(&p, ma_id(ma_elem_var(psa, i)));
    for (ULONG i = 0; i < cells; i++) p = ma_var_marshal(flags, p, ma_elem_var(psa, i), depth + 1);
  } else if (sf == SF_RECORD) {
    IRecordInfo *info = cells ? ma_sa_record_info(psa) : NULL;
    for (ULONG i = 0; i < cells; i++) ma_w32(&p, ma_id(ma_elem(psa, i)));
    for (ULONG i = 0; i < cells; i++) p = ma_rec_marshal(flags, p, info, ma_elem(psa, i), psa->cbElements);
    if (info) IRecordInfo_Release(info);
  } else {
    p = ma_align(p, ma_sf_align(sf));
    ma_wn(&p, psa->pvData, (SIZE_T)cells * psa->cbElements);
  }
  return p;
}

/* releases what the elements hold and zeroes them */
static void ma_sa_clear_elements(LPSAFEARRAY psa, SF_TYPE sf, ULONG cells) {
  if (!psa->pvData) return;
  IRecordInfo *info = NULL;
  if (sf == SF_RECORD && cells) SafeArrayGetRecordInfo(psa, &info);
  for (ULONG i = 0; i < cells; i++) {
    if (sf == SF_BSTR) { SysFreeString((BSTR)ma_elem_ptr(psa, i)); memset(ma_elem(psa, i), 0, psa->cbElements); }
    else if (sf == SF_UNKNOWN || sf == SF_DISPATCH || sf == SF_HAVEIID) {
      IUnknown *unk = ma_elem_ptr(psa, i);
      if (unk) IUnknown_Release(unk);
      memset(ma_elem(psa, i), 0, psa->cbElements);
    } else if (sf == SF_VARIANT) { VariantClear(ma_elem_var(psa, i)); memset(ma_elem(psa, i), 0, psa->cbElements); }
    else if (sf == SF_RECORD) { if (info) IRecordInfo_RecordClear(info, ma_elem(psa, i)); memset(ma_elem(psa, i), 0, psa->cbElements); }
  }
  if (info) IRecordInfo_Release(info);
}

static unsigned char *ma_sa_unmarshal(ULONG *flags, unsigned char *p, LPSAFEARRAY *ppsa, int depth) {
  if (depth > MA_MAX_DEPTH) ma_bad();
  p = ma_align(p, 4);
  if (!ma_r32(&p)) {
    if (*ppsa) { SafeArrayDestroy(*ppsa); *ppsa = NULL; }
    return p;
  }
  const DWORD dims = ma_r32(&p);
  const unsigned dims16 = ma_r16(&p), wire_features = ma_r16(&p);
  const DWORD wire_cb = ma_r32(&p);
  ma_r16(&p);                                   /* cLocks: a new array starts with none */
  const VARTYPE vt = (VARTYPE)ma_r16(&p);
  const DWORD wire_sf = ma_r32(&p), cells = ma_r32(&p), data_id = ma_r32(&p);
  if (!dims || dims > MA_MAX_DIMS || dims != dims16 || cells > MA_MAX_BYTES) ma_bad();
  const SF_TYPE sf = ma_sf_from(vt != 0, vt, wire_features, wire_cb);
  if (sf == SF_ERROR || wire_sf != (DWORD)sf) ma_bad();
  GUID guid;
  if (sf == SF_HAVEIID) { memcpy(&guid, p, sizeof(guid)); p += sizeof(guid); }
  SAFEARRAYBOUND bounds[MA_MAX_DIMS];       /* (descriptor order: the wire has the first dimension first) */
  unsigned long long n = 1;
  for (DWORD i = 0; i < dims; i++) {
    SAFEARRAYBOUND b;
    memcpy(&b, p, sizeof(b));
    p += sizeof(b);
    n *= b.cElements;
    if (n > MA_MAX_BYTES || (b.cElements && (long long)b.lLbound + b.cElements - 1 > 0x7fffffffll)) ma_bad();
    bounds[dims - 1 - i] = b;
  }
  if (ma_r32(&p) != cells || n != cells || (cells && !data_id)) ma_bad();
  /* the element size here */
  ULONG cb;
  if (ma_sf_is_ptr(sf)) { if (wire_cb != 4) ma_bad(); cb = sizeof(void *); }
  else if (sf == SF_VARIANT) { if (wire_cb != 16 && wire_cb != sizeof(VARIANT)) ma_bad(); cb = sizeof(VARIANT); }
  else if (sf == SF_RECORD) { if (!wire_cb || wire_cb > MA_MAX_BYTES) ma_bad(); cb = wire_cb; }
  else { if (wire_cb != ma_sf_align(sf)) ma_bad(); cb = wire_cb; }
  if ((unsigned long long)cells * cb > MA_MAX_BYTES) ma_bad();

  /* the features of the new array are built from its type, not taken from the wire (a flag that does not belong to the type would make the descriptor's
   * hidden fields be read as something they are not): the record, interface or type flag of the type, and the one that tells what the elements hold */
  unsigned features = 0;
  if (sf == SF_HAVEIID) features = FADF_HAVEIID | ((wire_features & FADF_DISPATCH) ? FADF_DISPATCH : FADF_UNKNOWN);
  else if (sf == SF_RECORD) features = FADF_RECORD;
  else {
    if (vt) features |= FADF_HAVEVARTYPE;
    if (sf == SF_BSTR) features |= FADF_BSTR;
    else if (sf == SF_UNKNOWN) features |= FADF_UNKNOWN;
    else if (sf == SF_DISPATCH) features |= FADF_DISPATCH;
    else if (sf == SF_VARIANT) features |= FADF_VARIANT;
  }

  /* an array that is already there is reused when it has the same shape */
  LPSAFEARRAY psa = *ppsa;
  BOOL reuse = FALSE;
  if (psa && psa->cDims == dims && psa->cbElements == cb && psa->cLocks == 0 && (psa->fFeatures & ~MA_FADF_LOCAL) == features) {
    unsigned long long old = 1;
    for (USHORT i = 0; i < psa->cDims; i++) old *= psa->rgsabound[i].cElements;
    reuse = old == cells && (!cells || psa->pvData);
  }
  if (reuse) {
    ma_sa_clear_elements(psa, sf, cells);
    memcpy(psa->rgsabound, bounds, sizeof(SAFEARRAYBOUND) * dims);
  } else {
    if (psa) SafeArrayDestroy(psa);
    *ppsa = NULL;
    if (FAILED(SafeArrayAllocDescriptor(dims, &psa))) ma_raise((DWORD)E_OUTOFMEMORY);
    psa->fFeatures = (USHORT)features;
    psa->cbElements = cb;
    memcpy(psa->rgsabound, bounds, sizeof(SAFEARRAYBOUND) * dims);
    if (FAILED(SafeArrayAllocData(psa))) { SafeArrayDestroyDescriptor(psa); ma_raise((DWORD)E_OUTOFMEMORY); }
    *ppsa = psa;
  }
  if (features & FADF_HAVEVARTYPE) *ma_hidden_vt(psa) = vt;
  if (sf == SF_HAVEIID) SafeArraySetIID(psa, &guid);

  if (ma_sf_is_ptr(sf) || sf == SF_VARIANT || sf == SF_RECORD) {
    const unsigned char *ids = p;
    p += 4ull * cells;
    for (ULONG i = 0; i < cells; i++) {
      DWORD id;
      memcpy(&id, ids + 4ull * i, 4);
      if (!id) continue;
      if (sf == SF_BSTR) { p = ma_bstr_unmarshal(p, (BSTR *)ma_elem(psa, i)); }
      else if (sf == SF_VARIANT) { p = ma_var_unmarshal(flags, p, ma_elem_var(psa, i), depth + 1); }
      else if (sf == SF_RECORD) {
        IRecordInfo *info = NULL;
        p = ma_rec_unmarshal(p, &info, ma_elem(psa, i), cb);
        IRecordInfo *have = NULL;
        if (SUCCEEDED(SafeArrayGetRecordInfo(psa, &have)) && have) IRecordInfo_Release(have);
        else SafeArraySetRecordInfo(psa, info);
        IRecordInfo_Release(info);
      } else {
        void *itf = NULL;
        p = ma_iface_body_unmarshal(p, sf == SF_HAVEIID ? &guid : sf == SF_DISPATCH ? &IID_IDispatch : &IID_IUnknown, &itf);
        memcpy(ma_elem(psa, i), &itf, sizeof(itf));
      }
    }
  } else {
    p = ma_align(p, ma_sf_align(sf));
    memcpy(psa->pvData, p, (SIZE_T)cells * cb);
    p += (SIZE_T)cells * cb;
  }
  return p;
}

EXPORT ULONG WINAPI LPSAFEARRAY_UserSize(ULONG *flags, ULONG start, LPSAFEARRAY *psa) { return ma_sa_size(flags, start, *psa, 0); }
EXPORT unsigned char * WINAPI LPSAFEARRAY_UserMarshal(ULONG *flags, unsigned char *buffer, LPSAFEARRAY *psa) { return ma_sa_marshal(flags, buffer, *psa, 0); }
EXPORT unsigned char * WINAPI LPSAFEARRAY_UserUnmarshal(ULONG *flags, unsigned char *buffer, LPSAFEARRAY *psa) { return ma_sa_unmarshal(flags, buffer, psa, 0); }
EXPORT void WINAPI LPSAFEARRAY_UserFree(ULONG *flags, LPSAFEARRAY *psa) { (void)flags; if (*psa) { SafeArrayDestroy(*psa); *psa = NULL; } }
EXPORT ULONG WINAPI LPSAFEARRAY_UserSize64(ULONG *flags, ULONG start, LPSAFEARRAY *psa) { return LPSAFEARRAY_UserSize(flags, start, psa); }
EXPORT unsigned char * WINAPI LPSAFEARRAY_UserMarshal64(ULONG *flags, unsigned char *buffer, LPSAFEARRAY *psa) { return LPSAFEARRAY_UserMarshal(flags, buffer, psa); }
EXPORT unsigned char * WINAPI LPSAFEARRAY_UserUnmarshal64(ULONG *flags, unsigned char *buffer, LPSAFEARRAY *psa) { return LPSAFEARRAY_UserUnmarshal(flags, buffer, psa); }
EXPORT void WINAPI LPSAFEARRAY_UserFree64(ULONG *flags, LPSAFEARRAY *psa) { LPSAFEARRAY_UserFree(flags, psa); }
/* the forms the type library marshaller uses (the same wire format) */
EXPORT ULONG WINAPI LPSAFEARRAY_Size(ULONG *flags, ULONG start, LPSAFEARRAY *psa) { return LPSAFEARRAY_UserSize(flags, start, psa); }
EXPORT unsigned char * WINAPI LPSAFEARRAY_Marshal(ULONG *flags, unsigned char *buffer, LPSAFEARRAY *psa) { return LPSAFEARRAY_UserMarshal(flags, buffer, psa); }
EXPORT unsigned char * WINAPI LPSAFEARRAY_Unmarshal(ULONG *flags, unsigned char *buffer, LPSAFEARRAY *psa) { return LPSAFEARRAY_UserUnmarshal(flags, buffer, psa); }

/* ---- VARIANT: wireVARIANT { clSize (8 byte units of the whole), rpcReserved, vt, wReserved1-3, switch_is, union } ----------------------------------------- */
enum { MA_NONE, MA_SCALAR, MA_DECIMAL, MA_BSTR, MA_UNKNOWN, MA_DISPATCH, MA_ARRAY, MA_RECORD, MA_VARIANT };
/* The union arm of a type (and the size of a scalar), -1 for a type that is not marshalled. */
static int ma_var_kind(VARTYPE vt, ULONG *scalar) {
  const VARTYPE base = vt & VT_TYPEMASK;
  const BOOL byref = (vt & VT_BYREF) != 0;
  *scalar = 0;
  if (vt & (VT_VECTOR | VT_RESERVED)) return -1;
  if (vt & VT_ARRAY) return MA_ARRAY;
  switch (base) {
    case VT_EMPTY: case VT_NULL: return byref ? -1 : MA_NONE;
    case VT_I1: case VT_UI1: *scalar = 1; return MA_SCALAR;
    case VT_I2: case VT_UI2: case VT_BOOL: *scalar = 2; return MA_SCALAR;
    case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_R4: case VT_ERROR: *scalar = 4; return MA_SCALAR;
    case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE: *scalar = 8; return MA_SCALAR;
    case VT_DECIMAL: return MA_DECIMAL;
    case VT_BSTR: return MA_BSTR;
    case VT_UNKNOWN: return MA_UNKNOWN;
    case VT_DISPATCH: return MA_DISPATCH;
    case VT_VARIANT: return byref ? MA_VARIANT : -1;
    case VT_RECORD: return byref ? -1 : MA_RECORD;
    default: return -1;
  }
}
/* the value that stands for the non-null pointer of a by-reference arm (the size of what it points to on the 32 bit wire, at least a pointer) */
static DWORD ma_ref_id(int kind, ULONG scalar) {
  if (kind == MA_SCALAR) return scalar < 4 ? 4 : scalar;
  if (kind == MA_DECIMAL) return sizeof(DECIMAL);
  if (kind == MA_VARIANT) return sizeof(VARIANT);
  return 4;
}
static ULONG ma_ref_bytes(int kind, ULONG scalar) {
  switch (kind) {
    case MA_SCALAR: return scalar;
    case MA_DECIMAL: return sizeof(DECIMAL);
    case MA_VARIANT: return sizeof(VARIANT);
    default: return sizeof(void *);       /* BSTR, interfaces, arrays */
  }
}
static const IID *ma_kind_iid(int kind) { return kind == MA_DISPATCH ? &IID_IDispatch : &IID_IUnknown; }

static ULONG ma_var_size(ULONG *flags, ULONG size, VARIANT *v, int depth) {
  if (depth > MA_MAX_DEPTH) ma_bad();
  const VARTYPE vt = V_VT(v);
  ULONG scalar;
  const int kind = ma_var_kind(vt, &scalar);
  if (kind < 0) ma_bad();
  size = ma_len(ma_alen(size, 8), 20);
  const BOOL byref = (vt & VT_BYREF) != 0;
  if (byref) {
    size = ma_len(ma_alen(size, 4), 4);
    if (!V_BYREF(v)) return size;
  }
  switch (kind) {
    case MA_NONE: break;
    case MA_SCALAR: size = ma_len(ma_alen(size, scalar), scalar); break;
    case MA_DECIMAL: size = ma_len(ma_alen(size, 8), 16); break;
    case MA_BSTR: size = ma_bstr_size(ma_len(ma_alen(size, 4), 4), byref ? *V_BSTRREF(v) : V_BSTR(v)); break;
    case MA_UNKNOWN: case MA_DISPATCH: {
      IUnknown *unk = byref ? *V_UNKNOWNREF(v) : V_UNKNOWN(v);
      size = ma_len(ma_alen(size, 4), 4);
      if (unk) size = ma_iface_body_size(flags, size, ma_kind_iid(kind), unk);
      break;
    }
    case MA_ARRAY: {
      LPSAFEARRAY psa = byref ? *V_ARRAYREF(v) : V_ARRAY(v);
      size = ma_len(ma_alen(size, 4), 4);
      if (psa) size = ma_sa_size(flags, size, psa, depth + 1);
      break;
    }
    case MA_VARIANT: size = ma_var_size(flags, ma_len(size, 4), V_VARIANTREF(v), depth + 1); break;       /* (the 'User' marker, then the variant) */
    case MA_RECORD: {
      size = ma_len(ma_alen(size, 4), 4);
      IRecordInfo *info = V_RECORDINFO(v);
      ULONG cb = 0;
      if (V_RECORD(v) && info) {
        if (FAILED(IRecordInfo_GetSize(info, &cb))) ma_raise((DWORD)E_INVALIDARG);
        size = ma_rec_size(flags, size, info, cb);
      }
      break;
    }
  }
  return size;
}

static unsigned char *ma_var_marshal(ULONG *flags, unsigned char *p, VARIANT *v, int depth) {
  const ULONG total = ma_var_size(flags, 0, v, depth);
  const VARTYPE vt = V_VT(v);
  ULONG scalar;
  const int kind = ma_var_kind(vt, &scalar);
  const BOOL byref = (vt & VT_BYREF) != 0;
  p = ma_align(p, 8);
  ma_w32(&p, (total + 7) >> 3);
  ma_w32(&p, 0);
  ma_w16(&p, vt);
  ma_w16(&p, v->wReserved1);
  ma_w16(&p, v->wReserved2);
  ma_w16(&p, v->wReserved3);
  ma_w32(&p, (vt & VT_ARRAY) ? (DWORD)(vt & ~VT_TYPEMASK) : vt);
  if (byref) {
    p = ma_align(p, 4);
    if (!V_BYREF(v)) { ma_w32(&p, 0); return p; }
    ma_w32(&p, ma_ref_id(kind, scalar));
  }
  switch (kind) {
    case MA_NONE: break;
    case MA_SCALAR:
      p = ma_align(p, scalar);
      ma_wn(&p, byref ? V_BYREF(v) : (const void *)&V_I8(v), scalar);
      break;
    case MA_DECIMAL: {
      p = ma_align(p, 8);
      DECIMAL dec = byref ? *V_DECIMALREF(v) : V_DECIMAL(v);
      if (!byref) dec.wReserved = VT_DECIMAL;
      ma_wn(&p, &dec, sizeof(dec));
      break;
    }
    case MA_BSTR: {
      BSTR b = byref ? *V_BSTRREF(v) : V_BSTR(v);
      p = ma_align(p, 4);
      ma_w32(&p, ma_id(b));
      p = ma_bstr_marshal(p, b);
      break;
    }
    case MA_UNKNOWN: case MA_DISPATCH: {
      IUnknown *unk = byref ? *V_UNKNOWNREF(v) : V_UNKNOWN(v);
      p = ma_align(p, 4);
      ma_w32(&p, ma_id_nn(unk));
      if (unk) p = ma_iface_body_marshal(flags, p, ma_kind_iid(kind), unk);
      break;
    }
    case MA_ARRAY: {
      LPSAFEARRAY psa = byref ? *V_ARRAYREF(v) : V_ARRAY(v);
      p = ma_align(p, 4);
      ma_w32(&p, ma_id_nn(psa));
      if (psa) p = ma_sa_marshal(flags, p, psa, depth + 1);
      break;
    }
    case MA_VARIANT:
      ma_w32(&p, 'U' | 's' << 8 | 'e' << 16 | 'r' << 24);
      p = ma_var_marshal(flags, p, V_VARIANTREF(v), depth + 1);
      break;
    case MA_RECORD: {
      IRecordInfo *info = V_RECORDINFO(v);
      void *rec = V_RECORD(v);
      ULONG cb = 0;
      p = ma_align(p, 4);
      if (rec && info) {
        if (FAILED(IRecordInfo_GetSize(info, &cb))) ma_raise((DWORD)E_INVALIDARG);
        ma_w32(&p, ma_id(rec));
        p = ma_rec_marshal(flags, p, info, rec, cb);
      } else ma_w32(&p, 0);
      break;
    }
  }
  return p;
}

/* releases what a wire unmarshalling allocated, including the memory of a by-reference arm; the variant is empty afterwards */
static void ma_var_free(VARIANT *v, int depth) {
  const VARTYPE vt = V_VT(v);
  ULONG scalar;
  const int kind = ma_var_kind(vt, &scalar);
  if (kind < 0 || depth > MA_MAX_DEPTH) { V_VT(v) = VT_EMPTY; return; }
  if (vt & VT_BYREF) {
    void *ref = V_BYREF(v);
    if (ref) {
      if (kind == MA_BSTR) SysFreeString(*(BSTR *)ref);
      else if (kind == MA_UNKNOWN || kind == MA_DISPATCH) { if (*(IUnknown **)ref) IUnknown_Release(*(IUnknown **)ref); }
      else if (kind == MA_ARRAY) { if (*(LPSAFEARRAY *)ref) SafeArrayDestroy(*(LPSAFEARRAY *)ref); }
      else if (kind == MA_VARIANT) ma_var_free((VARIANT *)ref, depth + 1);
      oa_free(ref);
    }
  } else if (kind == MA_RECORD) {
    IRecordInfo *info = V_RECORDINFO(v);
    if (info) {
      if (V_RECORD(v)) IRecordInfo_RecordDestroy(info, V_RECORD(v));
      IRecordInfo_Release(info);
    }
  } else if (kind == MA_BSTR || kind == MA_UNKNOWN || kind == MA_DISPATCH || kind == MA_ARRAY) VariantClear(v);
  V_VT(v) = VT_EMPTY;
}

static unsigned char *ma_var_unmarshal(ULONG *flags, unsigned char *p, VARIANT *v, int depth) {
  if (depth > MA_MAX_DEPTH) ma_bad();
  p = ma_align(p, 8);
  ma_r32(&p);                                       /* clSize: only the whole size of the variant, which the reading derives itself */
  if (ma_r32(&p) != 0) ma_bad();
  const VARTYPE vt = (VARTYPE)ma_r16(&p);
  const unsigned res1 = ma_r16(&p), res2 = ma_r16(&p), res3 = ma_r16(&p);
  const DWORD switch_is = ma_r32(&p);
  ULONG scalar;
  const int kind = ma_var_kind(vt, &scalar);
  if (kind < 0 || switch_is != ((vt & VT_ARRAY) ? (DWORD)(vt & ~VT_TYPEMASK) : vt)) ma_bad();
  const BOOL byref = (vt & VT_BYREF) != 0;

  /* a by-reference variant of the same type keeps its memory; anything else goes */
  void *keep = (V_VT(v) == vt && byref) ? V_BYREF(v) : NULL;
  if (!keep) {
    if (!(V_VT(v) & VT_BYREF)) VariantClear(v);
    memset((char *)v + offsetof(VARIANT, llVal), 0, sizeof(VARIANT) - offsetof(VARIANT, llVal));
  }
  V_VT(v) = VT_EMPTY;
  v->wReserved1 = (WORD)res1;
  v->wReserved2 = (WORD)res2;
  v->wReserved3 = (WORD)res3;

  void *ref = NULL;                                 /* the memory a by-reference arm points to */
  if (byref) {
    p = ma_align(p, 4);
    if (!ma_r32(&p)) { V_BYREF(v) = NULL; if (keep) oa_free(keep); V_VT(v) = vt; return p; }
    ref = keep ? keep : oa_calloc(ma_ref_bytes(kind, scalar));
    if (!ref) ma_raise((DWORD)E_OUTOFMEMORY);
    V_BYREF(v) = ref;
    V_VT(v) = vt;                                   /* (from here the variant owns the memory: ma_var_free finds it if something below raises) */
  }
  switch (kind) {
    case MA_NONE: break;
    case MA_SCALAR:
      p = ma_align(p, scalar);
      memcpy(byref ? ref : (void *)&V_I8(v), p, scalar);
      p += scalar;
      break;
    case MA_DECIMAL:
      p = ma_align(p, 8);
      memcpy(byref ? ref : (void *)&V_DECIMAL(v), p, sizeof(DECIMAL));
      p += sizeof(DECIMAL);
      break;
    case MA_BSTR: {
      p = ma_align(p, 4);
      BSTR *dest = byref ? (BSTR *)ref : &V_BSTR(v);
      if (ma_r32(&p)) p = ma_bstr_unmarshal(p, dest);
      else { SysFreeString(*dest); *dest = NULL; }
      break;
    }
    case MA_UNKNOWN: case MA_DISPATCH: {
      p = ma_align(p, 4);
      IUnknown **dest = byref ? (IUnknown **)ref : &V_UNKNOWN(v);
      if (keep && *dest) { IUnknown_Release(*dest); *dest = NULL; }
      void *itf = NULL;
      if (ma_r32(&p)) p = ma_iface_body_unmarshal(p, ma_kind_iid(kind), &itf);
      *dest = itf;
      break;
    }
    case MA_ARRAY: {
      p = ma_align(p, 4);
      LPSAFEARRAY *dest = byref ? (LPSAFEARRAY *)ref : &V_ARRAY(v);
      if (ma_r32(&p)) p = ma_sa_unmarshal(flags, p, dest, depth + 1);
      else { if (*dest) SafeArrayDestroy(*dest); *dest = NULL; }
      break;
    }
    case MA_VARIANT:
      if (ma_r32(&p) != ('U' | 's' << 8 | 'e' << 16 | 'r' << 24)) ma_bad();
      p = ma_var_unmarshal(flags, p, (VARIANT *)ref, depth + 1);
      break;
    case MA_RECORD: {
      p = ma_align(p, 4);
      if (ma_r32(&p)) {
        /* (the record is as large as its record info says: read the body into a scratch buffer of the size the wire states, then create the record) */
        unsigned char *body = p;
        body = ma_align(body, 4);
        ma_r32(&body);
        const DWORD cb = ma_r32(&body);
        if (!cb || cb > MA_MAX_BYTES) ma_bad();
        BYTE *copy = oa_alloc(cb);
        if (!copy) ma_raise((DWORD)E_OUTOFMEMORY);
        IRecordInfo *info = NULL;
        p = ma_rec_unmarshal(p, &info, copy, cb);
        ULONG size = 0;
        void *rec = NULL;
        if (SUCCEEDED(IRecordInfo_GetSize(info, &size)) && size == cb) rec = IRecordInfo_RecordCreate(info);
        if (!rec) { oa_free(copy); IRecordInfo_Release(info); ma_bad(); }
        memcpy(rec, copy, cb);
        oa_free(copy);
        V_RECORD(v) = rec;
        V_RECORDINFO(v) = info;
      }
      break;
    }
  }
  V_VT(v) = vt;
  return p;
}

EXPORT ULONG WINAPI VARIANT_UserSize(ULONG *flags, ULONG start, VARIANT *v) { return ma_var_size(flags, start, v, 0); }
EXPORT unsigned char * WINAPI VARIANT_UserMarshal(ULONG *flags, unsigned char *buffer, VARIANT *v) { return ma_var_marshal(flags, buffer, v, 0); }
EXPORT unsigned char * WINAPI VARIANT_UserUnmarshal(ULONG *flags, unsigned char *buffer, VARIANT *v) { return ma_var_unmarshal(flags, buffer, v, 0); }
EXPORT void WINAPI VARIANT_UserFree(ULONG *flags, VARIANT *v) { (void)flags; ma_var_free(v, 0); }
EXPORT ULONG WINAPI VARIANT_UserSize64(ULONG *flags, ULONG start, VARIANT *v) { return VARIANT_UserSize(flags, start, v); }
EXPORT unsigned char * WINAPI VARIANT_UserMarshal64(ULONG *flags, unsigned char *buffer, VARIANT *v) { return VARIANT_UserMarshal(flags, buffer, v); }
EXPORT unsigned char * WINAPI VARIANT_UserUnmarshal64(ULONG *flags, unsigned char *buffer, VARIANT *v) { return VARIANT_UserUnmarshal(flags, buffer, v); }
EXPORT void WINAPI VARIANT_UserFree64(ULONG *flags, VARIANT *v) { VARIANT_UserFree(flags, v); }
