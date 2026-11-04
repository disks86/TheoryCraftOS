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

/* Shared memory between processes (SHAllocShared and friends), the fixed-size-item arrays (FDSA_*), SHSearchMapInt and GUID parsing. Their ordinals are in shlwapi.def.
 * Included by shlwapi.c after reg.inc. */

IMPORT HANDLE WINAPI CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD high, DWORD low, LPCWSTR name);
IMPORT LPVOID WINAPI MapViewOfFile(HANDLE mapping, DWORD access, DWORD high, DWORD low, SIZE_T bytes);
IMPORT BOOL WINAPI UnmapViewOfFile(LPCVOID base);
IMPORT BOOL WINAPI DuplicateHandle(HANDLE source_process, HANDLE source, HANDLE target_process, PHANDLE target, DWORD access, BOOL inherit, DWORD options);
IMPORT HANDLE WINAPI OpenProcess(DWORD access, BOOL inherit, DWORD pid);
IMPORT HANDLE WINAPI GetCurrentProcess(void);
IMPORT DWORD WINAPI GetCurrentProcessId(void);

#define FILE_MAP_ALL_ACCESS_ 0xF001F
#define PROCESS_DUP_HANDLE_ 0x40
#define DUPLICATE_CLOSE_SOURCE_ 1
#define DUPLICATE_SAME_ACCESS_ 2

typedef struct { WORD vt, reserved[3]; union { LONGLONG i8; double r8; PVOID ptr; PVOID record[2]; } u; } PackVariant;      /* (24 bytes: a VARIANT is that large because of VT_RECORD) */
typedef struct { PackVariant *rgvarg; LONG *rgdispidNamedArgs; UINT cArgs, cNamedArgs; } PackDispParams;


/* ---- shared memory -------------------------------------------------------------------------------------------------------------------------------------------- */

/* A block is a file mapping whose first DWORD is the size; the handle given out is valid in the process the block was allocated for. */

EXPORT HANDLE WINAPI SHMapHandle(HANDLE handle, DWORD source_pid, DWORD target_pid, DWORD access, DWORD options) {
  HANDLE source_process = NULL, target_process = NULL, result = NULL;
  if (!handle) return NULL;
  const DWORD self = GetCurrentProcessId();
  source_process = source_pid == self ? GetCurrentProcess() : OpenProcess(PROCESS_DUP_HANDLE_, FALSE, source_pid);
  target_process = target_pid == self ? GetCurrentProcess() : OpenProcess(PROCESS_DUP_HANDLE_, FALSE, target_pid);
  if (source_process && target_process) {
    if (!access) options |= DUPLICATE_SAME_ACCESS_;
    if (!DuplicateHandle(source_process, handle, target_process, &result, access, FALSE, options)) result = NULL;
  }
  if (source_process && source_pid != self) CloseHandle(source_process);
  if (target_process && target_pid != self) CloseHandle(target_process);
  return result;
}
EXPORT HANDLE WINAPI SHAllocShared(const void *data, DWORD size, DWORD pid) {
  if (size > 0xFFFFFFFFu - sizeof(DWORD)) return NULL;
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, size + sizeof(DWORD), NULL);
  if (!mapping) return NULL;
  BYTE *view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS_, 0, 0, 0);
  if (!view) { CloseHandle(mapping); return NULL; }
  *(DWORD *)view = size;
  if (data) memcpy(view + sizeof(DWORD), data, size);
  UnmapViewOfFile(view);
  HANDLE result = SHMapHandle(mapping, GetCurrentProcessId(), pid, FILE_MAP_ALL_ACCESS_, DUPLICATE_SAME_ACCESS_);
  CloseHandle(mapping);
  return result;
}
EXPORT void *WINAPI SHLockShared(HANDLE handle, DWORD pid) {
  HANDLE local = SHMapHandle(handle, pid, GetCurrentProcessId(), FILE_MAP_ALL_ACCESS_, 0);
  if (!local) return NULL;
  BYTE *view = MapViewOfFile(local, FILE_MAP_ALL_ACCESS_, 0, 0, 0);
  CloseHandle(local);
  return view ? view + sizeof(DWORD) : NULL;
}
EXPORT BOOL WINAPI SHUnlockShared(void *block) { return block ? UnmapViewOfFile((BYTE *)block - sizeof(DWORD)) : FALSE; }
EXPORT BOOL WINAPI SHFreeShared(HANDLE handle, DWORD pid) {
  if (!handle) return TRUE;
  HANDLE process = pid == GetCurrentProcessId() ? GetCurrentProcess() : OpenProcess(PROCESS_DUP_HANDLE_, FALSE, pid);
  if (!process) return FALSE;
  const BOOL ok = DuplicateHandle(process, handle, GetCurrentProcess(), NULL, 0, FALSE, DUPLICATE_CLOSE_SOURCE_);
  if (pid != GetCurrentProcessId()) CloseHandle(process);
  return ok;
}

/* ---- FDSA: an array of fixed-size items in caller memory that moves to its own memory when it outgrows it -------------------------------------------------------------- */

typedef struct { DWORD num_items; void *mem; DWORD blocks_alloced; BYTE inc; BYTE block_size; BYTE flags; } FDSAInfo;
#define FDSA_OWNS_MEMORY 1

EXPORT BOOL WINAPI FDSA_Initialize(DWORD block_size, DWORD inc, FDSAInfo *info, void *mem, DWORD init_blocks) {
  info->num_items = 0;
  info->inc = (BYTE)(inc ? inc : 1);
  info->mem = mem;
  info->blocks_alloced = init_blocks;
  info->block_size = (BYTE)block_size;
  info->flags = 0;
  return TRUE;
}
EXPORT BOOL WINAPI FDSA_Destroy(FDSAInfo *info) {
  if (info->flags & FDSA_OWNS_MEMORY) { task_free(info->mem); info->mem = NULL; return FALSE; }
  return TRUE;
}
EXPORT DWORD WINAPI FDSA_InsertItem(FDSAInfo *info, DWORD where, const void *item) {
  if (where > info->num_items) where = info->num_items;
  if (info->num_items >= info->blocks_alloced) {
    const DWORD blocks = info->blocks_alloced + info->inc;
    BYTE *grown = task_alloc((size_t_)blocks * info->block_size);
    if (!grown) return (DWORD)-1;
    memcpy(grown, info->mem, (size_t_)info->num_items * info->block_size);
    if (info->flags & FDSA_OWNS_MEMORY) task_free(info->mem);
    info->mem = grown;
    info->blocks_alloced = blocks;
    info->flags |= FDSA_OWNS_MEMORY;
  }
  BYTE *base = info->mem;
  memmove(base + (size_t_)(where + 1) * info->block_size, base + (size_t_)where * info->block_size, (size_t_)(info->num_items - where) * info->block_size);
  memcpy(base + (size_t_)where * info->block_size, item, info->block_size);
  ++info->num_items;
  return where;
}
EXPORT BOOL WINAPI FDSA_DeleteItem(FDSAInfo *info, DWORD where) {
  if (where >= info->num_items) return FALSE;
  BYTE *base = info->mem;
  memmove(base + (size_t_)where * info->block_size, base + (size_t_)(where + 1) * info->block_size, (size_t_)(info->num_items - where - 1) * info->block_size);
  --info->num_items;
  return TRUE;
}

/* ---- small ones --------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT int WINAPI SHSearchMapInt(const int *keys, const int *values, int count, int key) {
  for (int i = 0; i < count; ++i) if (keys[i] == key) return values[i];
  return -1;
}

/* "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" to a GUID */
static BOOL guid_from_text(const WCHAR *s, void *guid) {
  static const int groups[] = {8, 4, 4, 4, 12};
  BYTE digits[32];
  int n = 0;
  if (!s || s[0] != '{') return FALSE;
  size_t_ p = 1;
  for (int g = 0; g < 5; ++g) {
    for (int i = 0; i < groups[g]; ++i, ++p) {
      const WCHAR c = s[p];
      int v;
      if (c >= '0' && c <= '9') v = c - '0'; else if (c >= 'a' && c <= 'f') v = c - 'a' + 10; else if (c >= 'A' && c <= 'F') v = c - 'A' + 10; else return FALSE;
      digits[n++] = (BYTE)v;
    }
    if (g < 4) { if (s[p] != '-') return FALSE; ++p; }
  }
  if (s[p] != '}') return FALSE;
  BYTE bytes[16];
  for (int i = 0; i < 16; ++i) bytes[i] = (BYTE)(digits[2 * i] << 4 | digits[2 * i + 1]);
  BYTE *out = guid;
  /* Data1 (4 bytes), Data2 and Data3 (2 each) are little-endian in memory; Data4 is as written */
  out[0] = bytes[3]; out[1] = bytes[2]; out[2] = bytes[1]; out[3] = bytes[0];
  out[4] = bytes[5]; out[5] = bytes[4];
  out[6] = bytes[7]; out[7] = bytes[6];
  memcpy(out + 8, bytes + 8, 8);
  return TRUE;
}
EXPORT BOOL WINAPI GUIDFromStringW(LPCWSTR s, void *guid) { return guid_from_text(s, guid); }
EXPORT BOOL WINAPI GUIDFromStringA(LPCSTR s, void *guid) {
  WCHAR wide[40];
  if (!s || !MultiByteToWideChar(CP_ACP, 0, s, -1, wide, 40)) return FALSE;
  return guid_from_text(wide, guid);
}

/* ---- SHPackDispParams: fill a DISPPARAMS from (type, value) pairs ------------------------------------------------------------------------------------------------- */

EXPORT HRESULT_ WINAPI SHPackDispParamsV(PackDispParams *params, PackVariant *args, UINT count, __builtin_va_list list) {
  if (!params) return E_INVALIDARG_;
  params->rgvarg = args;
  params->rgdispidNamedArgs = NULL;
  params->cArgs = count;
  params->cNamedArgs = 0;
  for (UINT i = 0; i < count; ++i) {
    PackVariant *v = &args[count - 1 - i];      /* (the pairs are stored last to first, as a call's arguments are) */
    WORD vt = (WORD)__builtin_va_arg(list, int);
    if (vt == 0) vt = 3;      /* (an empty one is packed as a 32-bit integer) */
    memset(v, 0, sizeof(*v));
    v->vt = vt;
    switch (vt) {
      case 2: case 11: case 16: case 17: case 18: case 3: case 10: case 19: case 22: case 23: v->u.i8 = (LONGLONG)__builtin_va_arg(list, int); break;
      case 20: case 21: v->u.i8 = __builtin_va_arg(list, LONGLONG); break;
      case 4: { const float f = (float)__builtin_va_arg(list, double); memcpy(&v->u.i8, &f, sizeof(f)); break; }
      case 5: case 7: v->u.r8 = __builtin_va_arg(list, double); break;
      case 8: case 9: case 13: v->u.ptr = __builtin_va_arg(list, PVOID); break;
      default: return E_INVALIDARG_;
    }
  }
  return 0;
}
EXPORT HRESULT_ __cdecl SHPackDispParams(PackDispParams *params, PackVariant *args, UINT count, ...) {
  __builtin_va_list list;
  __builtin_va_start(list, count);
  const HRESULT_ hr = SHPackDispParamsV(params, args, count, list);
  __builtin_va_end(list);
  return hr;
}

/* ---- connection points (the COM calls are made through the vtables: IUnknown 0-2, then each interface's own methods) ---------------------------------------------------- */

typedef struct ComObject { const void *const *vtbl; } ComObject;
#define COM_CALL(obj, slot, type, ...) (((type)((ComObject *)(obj))->vtbl[slot])((obj), ##__VA_ARGS__))
typedef HRESULT_ (WINAPI *ComQuery)(void *, const void *, void **);
typedef ULONG (WINAPI *ComRelease)(void *);

static const struct { DWORD d1; WORD d2, d3; BYTE d4[8]; } kIidConnectionPointContainer = {0xB196B284, 0xBAB4, 0x101A, {0xB6, 0x9C, 0x00, 0xAA, 0x00, 0x34, 0x1D, 0x07}},
                                                           kIidDispatch = {0x00020400, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

EXPORT HRESULT_ WINAPI ConnectToConnectionPoint(void *sink, const void *iid, BOOL connect, void *object, DWORD *cookie, void **point_out) {
  void *container = NULL, *point = NULL;
  if (!object || (connect && !sink)) return E_FAIL_;
  if (point_out) *point_out = NULL;
  HRESULT_ hr = COM_CALL(object, 0, ComQuery, &kIidConnectionPointContainer, &container);
  if (hr < 0) return hr;
  hr = COM_CALL(container, 4, HRESULT_ (WINAPI *)(void *, const void *, void **), iid, &point);
  if (hr >= 0) {
    hr = connect ? COM_CALL(point, 5, HRESULT_ (WINAPI *)(void *, void *, DWORD *), sink, cookie) : COM_CALL(point, 6, HRESULT_ (WINAPI *)(void *, DWORD), *cookie);
    if (hr < 0 && cookie) *cookie = 0;
    if (point_out && hr >= 0) *point_out = point; else COM_CALL(point, 2, ComRelease);
  }
  COM_CALL(container, 2, ComRelease);
  return hr;
}
EXPORT HRESULT_ WINAPI IConnectionPoint_SimpleInvoke(void *point, LONG dispid, void *params) {
  void *enumerator = NULL;
  struct { void *unknown; DWORD cookie; } data;
  ULONG fetched;
  PackDispParams none = {0};
  HRESULT_ hr = COM_CALL(point, 7, HRESULT_ (WINAPI *)(void *, void **), &enumerator);
  if (hr < 0) return hr;
  if (!params) params = &none;
  while (COM_CALL(enumerator, 3, HRESULT_ (WINAPI *)(void *, ULONG, void *, ULONG *), 1, &data, &fetched) == 0) {
    void *dispatch = NULL;
    if (COM_CALL(data.unknown, 0, ComQuery, &kIidDispatch, &dispatch) >= 0) {
      static const struct { DWORD d1; WORD d2, d3; BYTE d4[8]; } null_iid;
      COM_CALL(dispatch, 6, HRESULT_ (WINAPI *)(void *, LONG, const void *, DWORD, WORD, void *, void *, void *, UINT *), dispid, &null_iid, 0, 1, params, NULL, NULL, NULL);
      COM_CALL(dispatch, 2, ComRelease);
    }
    COM_CALL(data.unknown, 2, ComRelease);
  }
  COM_CALL(enumerator, 2, ComRelease);
  return 0;
}
EXPORT HRESULT_ WINAPI IConnectionPoint_InvokeWithCancel(void *point, LONG dispid, void *params, DWORD a, DWORD b) { (void)point; (void)dispid; (void)params; (void)a; (void)b; return E_NOTIMPL_; }

/* The ordinals of exports that are not implemented yet point here (shlwapi.def): a program (or Wine's test) that looks one up by number must get a function that does nothing,
 * not whatever export the linker happened to put on that number. */
EXPORT ULONG_PTR WINAPI ShlwapiOrdinalNotImplemented(void) { return 0; }

/* ---- AssocCreate: an IQueryAssociations over AssocQueryStringW -------------------------------------------------------------------------------------------------------- */

typedef struct { DWORD d1; WORD d2, d3; BYTE d4[8]; } GuidValue;
typedef struct QueryAssoc { const void *const *vtbl; LONG ref; DWORD flags; WCHAR assoc[MAX_PATH_]; BOOL initialized; } QueryAssoc;

static const GuidValue kClsidQueryAssociations = {0xA07034FD, 0x6CAA, 0x4954, {0xAC, 0x3F, 0x97, 0xA2, 0x72, 0x16, 0xF9, 0x8A}},
                       kIidQueryAssociations = {0xC46CA590, 0x3C3F, 0x11D2, {0xBE, 0xE6, 0x00, 0x00, 0xF8, 0x05, 0xCA, 0x57}},
                       kIidUnknownValue = {0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
#define CLASS_E_CLASSNOTAVAILABLE_ ((LONG)0x80040111)

static HRESULT_ WINAPI qa_query(QueryAssoc *self, const GuidValue *iid, void **out) {
  if (!out) return E_POINTER_;
  *out = NULL;
  if (!memcmp(iid, &kIidQueryAssociations, 16) || !memcmp(iid, &kIidUnknownValue, 16)) { *out = self; InterlockedIncrement(&self->ref); return 0; }
  return E_NOINTERFACE_;
}
static ULONG WINAPI qa_addref(QueryAssoc *self) { return (ULONG)InterlockedIncrement(&self->ref); }
static ULONG WINAPI qa_release(QueryAssoc *self) {
  const LONG n = InterlockedDecrement(&self->ref);
  if (!n) task_free(self);
  return (ULONG)n;
}
static HRESULT_ WINAPI qa_init(QueryAssoc *self, DWORD flags, LPCWSTR assoc, HKEY progid, void *window) {
  (void)window;
  if (!assoc && !progid) return E_INVALIDARG_;
  if (!assoc || wcslen(assoc) >= MAX_PATH_) return E_NOTIMPL_;
  memcpy(self->assoc, assoc, (wcslen(assoc) + 1) * sizeof(WCHAR));
  self->flags = flags;
  self->initialized = TRUE;
  return 0;
}
static HRESULT_ WINAPI qa_get_string(QueryAssoc *self, DWORD flags, DWORD what, LPCWSTR extra, LPWSTR out, DWORD *size) {
  if (!self->initialized) return E_FAIL_;
  return AssocQueryStringW(flags | self->flags, what, self->assoc, extra, out, size);
}
static HRESULT_ WINAPI qa_not_implemented(void) { return E_NOTIMPL_; }
static const void *const kQueryAssocVtbl[] = {qa_query, qa_addref, qa_release, qa_init, qa_get_string, qa_not_implemented, qa_not_implemented, qa_not_implemented};

EXPORT HRESULT_ WINAPI AssocCreate(GuidValue clsid, const GuidValue *iid, void **out) {
  if (!iid || !out) return E_INVALIDARG_;
  *out = NULL;
  if (memcmp(&clsid, &kClsidQueryAssociations, 16)) return CLASS_E_CLASSNOTAVAILABLE_;
  QueryAssoc *self = task_alloc(sizeof(QueryAssoc));
  if (!self) return E_OUTOFMEMORY_;
  memset(self, 0, sizeof(*self));
  self->vtbl = kQueryAssocVtbl;
  self->ref = 1;
  const HRESULT_ hr = qa_query(self, iid, out);
  qa_release(self);
  return hr;
}

/* ---- SHCreateMemStream: a growable stream in memory; a clone shares the bytes and has its own position ----------------------------------------------------------------- */

typedef struct { LONG ref; BYTE *data; size_t_ size, capacity; } MemData;
typedef struct { const struct IStreamLikeVtbl *vtbl; LONG ref; MemData *shared; ULONGLONG pos; } MemStream;

static void mem_data_release(MemData *d) { if (!InterlockedDecrement(&d->ref)) { task_free(d->data); task_free(d); } }
static BOOL mem_reserve(MemData *d, size_t_ need) {
  if (need <= d->capacity) return TRUE;
  size_t_ cap = d->capacity ? d->capacity : 64;
  while (cap < need) { if (cap > ((size_t_)-1) / 2) return FALSE; cap *= 2; }
  BYTE *grown = task_alloc(cap);
  if (!grown) return FALSE;
  if (d->size) memcpy(grown, d->data, d->size);
  task_free(d->data);
  d->data = grown; d->capacity = cap;
  return TRUE;
}
static HRESULT_ WINAPI ms_query(IStreamLike *self, const void *iid, void **out) {
  static const GuidValue stream_iid = {0x0000000C, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}}, seq_iid = {0x0C733A30, 0x2A1C, 0x11CE, {0xAD, 0xE5, 0x00, 0xAA, 0x00, 0x44, 0x77, 0x3D}},
                          unknown_iid = {0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
  if (!out) return E_POINTER_;
  *out = NULL;
  if (!memcmp(iid, &stream_iid, 16) || !memcmp(iid, &seq_iid, 16) || !memcmp(iid, &unknown_iid, 16)) { *out = self; InterlockedIncrement(&((MemStream *)self)->ref); return 0; }
  return E_NOINTERFACE_;
}
static ULONG WINAPI ms_addref(IStreamLike *self) { return (ULONG)InterlockedIncrement(&((MemStream *)self)->ref); }
static ULONG WINAPI ms_release(IStreamLike *self) {
  MemStream *s = (MemStream *)self;
  const LONG n = InterlockedDecrement(&s->ref);
  if (!n) { mem_data_release(s->shared); task_free(s); }
  return (ULONG)n;
}
static HRESULT_ WINAPI ms_read(IStreamLike *self, void *buffer, ULONG count, ULONG *read) {
  MemStream *s = (MemStream *)self;
  const MemData *d = s->shared;
  ULONG got = 0;
  if (!buffer && count) return STG_E_INVALIDPOINTER_;
  if (s->pos < d->size) { const size_t_ left = d->size - (size_t_)s->pos; got = count < left ? count : (ULONG)left; if (got) memcpy(buffer, d->data + s->pos, got); s->pos += got; }
  if (read) *read = got;
  return got == count ? 0 : 1;
}
static HRESULT_ WINAPI ms_write(IStreamLike *self, const void *buffer, ULONG count, ULONG *written) {
  MemStream *s = (MemStream *)self;
  MemData *d = s->shared;
  if (!buffer && count) return STG_E_INVALIDPOINTER_;
  if (s->pos > (size_t_)-1 - count) return STG_E_MEDIUMFULL_;
  const size_t_ end = (size_t_)s->pos + count;
  if (!mem_reserve(d, end)) return E_OUTOFMEMORY_;
  if (s->pos > d->size) memset(d->data + d->size, 0, (size_t_)s->pos - d->size);
  if (count) memcpy(d->data + s->pos, buffer, count);
  if (end > d->size) d->size = end;
  s->pos = end;
  if (written) *written = count;
  return 0;
}
static HRESULT_ WINAPI ms_seek(IStreamLike *self, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *out) {
  MemStream *s = (MemStream *)self;
  LONGLONG base;
  switch (origin) { case 0: base = 0; break; case 1: base = (LONGLONG)s->pos; break; case 2: base = (LONGLONG)s->shared->size; break; default: return STG_E_INVALIDFUNCTION_; }
  const LONGLONG target = base + move.QuadPart;
  if (target < 0) return STG_E_INVALIDFUNCTION_;
  s->pos = (ULONGLONG)target;
  if (out) out->QuadPart = s->pos;
  return 0;
}
static HRESULT_ WINAPI ms_set_size(IStreamLike *self, ULARGE_INTEGER size) {
  MemData *d = ((MemStream *)self)->shared;
  if (size.QuadPart > (ULONGLONG)((size_t_)-1) / 2 || !mem_reserve(d, (size_t_)size.QuadPart)) return E_OUTOFMEMORY_;
  if (size.QuadPart > d->size) memset(d->data + d->size, 0, (size_t_)size.QuadPart - d->size);
  d->size = (size_t_)size.QuadPart;
  return 0;
}
static HRESULT_ WINAPI ms_copy_to(IStreamLike *self, IStreamLike *dest, ULARGE_INTEGER count, ULARGE_INTEGER *read, ULARGE_INTEGER *written) {
  BYTE buffer[1024];
  ULONGLONG left = count.QuadPart, total_read = 0, total_written = 0;
  HRESULT_ hr = 0;
  if (!dest) return STG_E_INVALIDPOINTER_;
  while (left) {
    ULONG got = 0, put = 0;
    hr = ms_read(self, buffer, left < sizeof(buffer) ? (ULONG)left : (ULONG)sizeof(buffer), &got);
    if (hr < 0 || !got) { if (hr > 0) hr = 0; break; }
    total_read += got;
    hr = dest->vtbl->Write(dest, buffer, got, &put);
    total_written += put;
    if (hr < 0 || put != got) break;
    left -= got;
  }
  if (read) read->QuadPart = total_read;
  if (written) written->QuadPart = total_written;
  return hr;
}
static HRESULT_ WINAPI ms_ok(IStreamLike *self, DWORD flags) { (void)self; (void)flags; return 0; }
static HRESULT_ WINAPI ms_revert(IStreamLike *self) { (void)self; return 0; }
static HRESULT_ WINAPI ms_lock(IStreamLike *self, ULARGE_INTEGER a, ULARGE_INTEGER b, DWORD c) { (void)self; (void)a; (void)b; (void)c; return STG_E_INVALIDFUNCTION_; }
static HRESULT_ WINAPI ms_stat(IStreamLike *self, void *out, DWORD flags) {
  struct { LPWSTR name; DWORD type; ULARGE_INTEGER size; FILETIME modified, created, accessed; DWORD mode, locks; BYTE clsid[16]; DWORD state, reserved; } *st = out;
  (void)flags;
  if (!out) return STG_E_INVALIDPOINTER_;
  memset(out, 0, sizeof(*st));
  st->type = 2;
  st->size.QuadPart = ((MemStream *)self)->shared->size;
  st->mode = 2;      /* STGM_READWRITE */
  return 0;
}
static HRESULT_ WINAPI ms_clone(IStreamLike *self, IStreamLike **out) {
  MemStream *s = (MemStream *)self;
  if (!out) return E_POINTER_;
  *out = NULL;
  MemStream *c = task_alloc(sizeof(*c));
  if (!c) return E_OUTOFMEMORY_;
  *c = *s;
  c->ref = 1;
  InterlockedIncrement(&c->shared->ref);
  *out = (IStreamLike *)c;
  return 0;
}
static const struct IStreamLikeVtbl g_mem_stream_vtbl = {ms_query, ms_addref, ms_release, ms_read, ms_write, ms_seek, ms_set_size, ms_copy_to, ms_ok, ms_revert, ms_lock, ms_lock, ms_stat, ms_clone};

EXPORT IStreamLike *WINAPI SHCreateMemStream(const BYTE *init, UINT size) {
  MemStream *s = task_alloc(sizeof(*s));
  MemData *d = task_alloc(sizeof(*d));
  if (!s || !d) { task_free(s); task_free(d); return NULL; }
  memset(d, 0, sizeof(*d));
  d->ref = 1;
  if (init && size) {
    if (!mem_reserve(d, size)) { task_free(s); task_free(d); return NULL; }
    memcpy(d->data, init, size);
    d->size = size;
  }
  s->vtbl = &g_mem_stream_vtbl;
  s->ref = 1;
  s->shared = d;
  s->pos = 0;
  return (IStreamLike *)s;
}

/* ---- ini files (ordinals 294 and 295) ---------------------------------------------------------------------------------------------------------------------------------- */

IMPORT DWORD WINAPI GetPrivateProfileStringW(LPCWSTR section, LPCWSTR entry, LPCWSTR def_val, LPWSTR buffer, DWORD len, LPCWSTR filename);
IMPORT BOOL WINAPI WritePrivateProfileStringW(LPCWSTR section, LPCWSTR entry, LPCWSTR value, LPCWSTR filename);

/* With no key name it gives the first key of the section only (the length of that name). */
EXPORT DWORD WINAPI SHGetIniStringW(LPCWSTR section, LPCWSTR key, LPWSTR out, DWORD size, LPCWSTR path) {
  if (!size) return 0;
  const DWORD n = GetPrivateProfileStringW(section, key, L"", out, size, path);
  return key ? n : (DWORD)wcslen(out);
}
EXPORT BOOL WINAPI SHSetIniStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR path) { return WritePrivateProfileStringW(section, key, value, path); }

/* ---- shell keys (ordinals 491 and 516-519) ---------------------------------------------------------------------------------------------------------------------------- */

IMPORT HANDLE WINAPI LocalAlloc(DWORD flags, SIZE_T bytes);
IMPORT HANDLE WINAPI LocalFree(HANDLE memory);
#define HANDLE_FREE_LOCAL(p) LocalFree((HANDLE)(p))

/* flags: 1 = HKEY_CURRENT_USER, 2 = HKEY_LOCAL_MACHINE; bits 4-5 pick the folder (Explorer, Shell, ShellNoRoam, Classes). The folder itself is always there; the sub key only when asked to create. */
EXPORT HKEY WINAPI SHGetShellKey(DWORD flags, LPCWSTR subkey, BOOL create) {
  static const WCHAR *const folders[4] = {L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer", L"Software\\Microsoft\\Windows\\Shell", L"Software\\Microsoft\\Windows\\ShellNoRoam", L"Software\\Classes"};
  WCHAR path[MAX_PATH_];
  const WCHAR *folder = folders[(flags >> 4) & 3];
  HKEY root = (flags & 2) ? HKEY_LOCAL_MACHINE_ : HKEY_CURRENT_USER_, folder_key, key = NULL;
  if (RegCreateKeyExW(root, folder, 0, NULL, 0, KEY_ALL_ACCESS_, NULL, &folder_key, NULL)) return NULL;
  if (!subkey || !*subkey) return folder_key;
  (void)path;
  const LSTATUS_ r = create ? RegCreateKeyExW(folder_key, subkey, 0, NULL, 0, KEY_ALL_ACCESS_, NULL, &key, NULL) : RegOpenKeyExW(folder_key, subkey, 0, KEY_ALL_ACCESS_, &key);
  RegCloseKey(folder_key);
  return r ? NULL : key;
}
static HRESULT_ sk_open(DWORD flags, LPCWSTR subkey, BOOL create, HKEY *key) {
  *key = SHGetShellKey(flags, subkey, create);
  return *key ? 0 : HRESULT_FROM_WIN32_(ERROR_FILE_NOT_FOUND);
}
EXPORT HRESULT_ WINAPI SKGetValueW(DWORD flags, LPCWSTR subkey, LPCWSTR value, LPDWORD type, LPVOID data, LPDWORD size) {
  HKEY key;
  HRESULT_ hr = sk_open(flags, subkey, FALSE, &key);
  if (hr) return hr;
  hr = HRESULT_FROM_WIN32_(RegQueryValueExW(key, value, NULL, type, data, size));
  RegCloseKey(key);
  return hr;
}
EXPORT HRESULT_ WINAPI SKSetValueW(DWORD flags, LPCWSTR subkey, LPCWSTR value, DWORD type, LPCVOID data, DWORD size) {
  HKEY key;
  HRESULT_ hr = sk_open(flags, subkey, TRUE, &key);
  if (hr) return hr;
  hr = HRESULT_FROM_WIN32_(RegSetValueExW(key, value, 0, type, data, size));
  RegCloseKey(key);
  return hr;
}
EXPORT HRESULT_ WINAPI SKDeleteValueW(DWORD flags, LPCWSTR subkey, LPCWSTR value) {
  HKEY key;
  HRESULT_ hr = sk_open(flags, subkey, FALSE, &key);
  if (hr) return hr;
  hr = HRESULT_FROM_WIN32_(RegDeleteValueW(key, value));
  RegCloseKey(key);
  return hr;
}
EXPORT HRESULT_ WINAPI SKAllocValueW(DWORD flags, LPCWSTR subkey, LPCWSTR value, LPDWORD type, LPVOID *data, LPDWORD size) {
  HKEY key;
  DWORD need = 0, t = 0;
  HRESULT_ hr = sk_open(flags, subkey, FALSE, &key);
  if (hr) return hr;
  hr = HRESULT_FROM_WIN32_(RegQueryValueExW(key, value, NULL, &t, NULL, &need));
  if (!hr) {
    BYTE *buffer = (BYTE *)LocalAlloc(0x40 /* LPTR */, need + 2);
    if (!buffer) hr = E_OUTOFMEMORY_;
    else {
      DWORD got = need;
      hr = HRESULT_FROM_WIN32_(RegQueryValueExW(key, value, NULL, &t, buffer, &got));
      if (hr) { HANDLE_FREE_LOCAL(buffer); }
      else { *data = buffer; if (size) *size = got; if (type) *type = t; }
    }
  }
  RegCloseKey(key);
  return hr;
}

/* ---- GetShellSecurityDescriptor (ordinal 475) ------------------------------------------------------------------------------------------------------------------------- */

typedef struct { BYTE authority[6]; DWORD rid0, rid1; } ShellUserSid;
typedef struct { ShellUserSid sid; DWORD access_type; BOOL inherit; DWORD access_mask, inherit_mask, inherit_access_mask; } ShellUserPermission;

IMPORT BOOL WINAPI AllocateAndInitializeSid(void *authority, BYTE count, DWORD r0, DWORD r1, DWORD r2, DWORD r3, DWORD r4, DWORD r5, DWORD r6, DWORD r7, void **sid);
IMPORT void *WINAPI FreeSid(void *sid);
IMPORT DWORD WINAPI GetLengthSid(void *sid);
IMPORT BOOL WINAPI InitializeAcl(void *acl, DWORD length, DWORD revision);
IMPORT BOOL WINAPI AddAce(void *acl, DWORD revision, DWORD start, void *ace, DWORD length);
IMPORT BOOL WINAPI InitializeSecurityDescriptor(void *sd, DWORD revision);
IMPORT BOOL WINAPI SetSecurityDescriptorDacl(void *sd, BOOL present, void *acl, BOOL defaulted);
IMPORT BOOL WINAPI OpenProcessToken(HANDLE process, DWORD access, HANDLE *token);
IMPORT BOOL WINAPI GetTokenInformation(HANDLE token, DWORD cls, void *info, DWORD length, DWORD *needed);
IMPORT BOOL WINAPI CopySid(DWORD length, void *dst, void *src);

/* The SID of a permission: all-zero authority and rids means the user the process runs as. The result must be released with FreeSid or LocalFree (current_user). */
static void *shell_sid(const ShellUserSid *s, BOOL *current_user) {
  static const BYTE zero[6] = {0};
  void *sid = NULL;
  *current_user = FALSE;
  if (!memcmp(s->authority, zero, 6) && !s->rid0 && !s->rid1) {
    HANDLE token;
    BYTE buffer[128];
    DWORD needed = 0;
    if (!OpenProcessToken(GetCurrentProcess(), 8 /* TOKEN_QUERY */, &token)) return NULL;
    if (GetTokenInformation(token, 1 /* TokenUser */, buffer, sizeof(buffer), &needed)) {
      void *user = *(void **)buffer;
      const DWORD n = GetLengthSid(user);
      sid = LocalAlloc(0, n);
      if (sid) { CopySid(n, sid, user); *current_user = TRUE; }
    }
    CloseHandle(token);
    return sid;
  }
  BYTE auth[6];
  memcpy(auth, s->authority, 6);
  const BYTE count = (BYTE)((s->rid0 != 0) + (s->rid1 != 0));
  return AllocateAndInitializeSid(auth, count, s->rid0, s->rid1, 0, 0, 0, 0, 0, 0, &sid) ? sid : NULL;
}
static void shell_sid_free(void *sid, BOOL current_user) { if (current_user) LocalFree((HANDLE)sid); else FreeSid(sid); }

EXPORT void *WINAPI GetShellSecurityDescriptor(const ShellUserPermission *const *permissions, int count) {
  if (!permissions || count <= 0) return NULL;
  enum { kSdSize = 64, kAceHeader = 8 };
  void *sids[16];
  BOOL current[16];
  if (count > 16) return NULL;
  DWORD acl_size = 8 /* ACL */;
  int made = 0;
  for (; made < count; ++made) {
    sids[made] = shell_sid(&permissions[made]->sid, &current[made]);
    if (!sids[made]) break;
    acl_size += (kAceHeader + GetLengthSid(sids[made])) * (permissions[made]->inherit ? 2 : 1);
  }
  BYTE *block = NULL;
  if (made == count) block = (BYTE *)LocalAlloc(0x40, kSdSize + acl_size);
  if (block) {
    BYTE *acl = block + kSdSize;
    BOOL ok = InitializeSecurityDescriptor(block, 1) && InitializeAcl(acl, acl_size, 2);
    for (int i = 0; ok && i < count; ++i) {
      const ShellUserPermission *p = permissions[i];
      const DWORD sid_length = GetLengthSid(sids[i]);
      BYTE ace[kAceHeader + 68];
      if (sid_length > 68) { ok = FALSE; break; }
      for (int pass = 0; ok && pass < (p->inherit ? 2 : 1); ++pass) {
        ace[0] = (BYTE)p->access_type;
        ace[1] = pass ? (BYTE)p->inherit_mask : 0;
        *(WORD *)(ace + 2) = (WORD)(kAceHeader + sid_length);
        *(DWORD *)(ace + 4) = pass ? p->inherit_access_mask : p->access_mask;
        memcpy(ace + kAceHeader, sids[i], sid_length);
        ok = AddAce(acl, 2, 0xFFFFFFFFu, ace, kAceHeader + sid_length);
      }
    }
    if (ok) ok = SetSecurityDescriptorDacl(block, TRUE, acl, FALSE);
    if (!ok) { LocalFree((HANDLE)block); block = NULL; }
  }
  for (int i = 0; i < made; ++i) shell_sid_free(sids[i], current[i]);
  return block;
}

/* ---- SHFormatDateTime (ordinals 353 and 354) ------------------------------------------------------------------------------------------------------------------------- */

IMPORT BOOL WINAPI FileTimeToLocalFileTime(const FILETIME *utc, LPFILETIME local);
IMPORT BOOL WINAPI FileTimeToSystemTime(const FILETIME *file_time, LPSYSTEMTIME st);
IMPORT int WINAPI GetDateFormatW(DWORD locale, DWORD flags, const SYSTEMTIME *st, LPCWSTR format, LPWSTR out, int capacity);
IMPORT int WINAPI GetTimeFormatW(DWORD locale, DWORD flags, const SYSTEMTIME *st, LPCWSTR format, LPWSTR out, int capacity);

#define FDTF_SHORTTIME_ 0x1
#define FDTF_LONGTIME_ 0x8
#define FDTF_SHORTDATE_ 0x2
#define FDTF_LONGDATE_ 0x4

/* The time is UTC; the result is the date (long beats short) then a space then the time (long beats short), in the user's locale. Returns the characters written including the NUL. */
EXPORT int WINAPI SHFormatDateTimeW(const FILETIME *utc, DWORD *flags_in, LPWSTR out, UINT capacity) {
  WCHAR date[128], time_text[128];
  FILETIME local;
  SYSTEMTIME st;
  const DWORD flags = flags_in ? *flags_in : (FDTF_SHORTDATE_ | FDTF_SHORTTIME_);
  size_t_ n = 0;
  if (!out || !capacity || !utc) return 0;
  if (!FileTimeToLocalFileTime(utc, &local) || !FileTimeToSystemTime(&local, &st)) return 0;
  date[0] = time_text[0] = 0;
  if (flags & (FDTF_LONGDATE_ | FDTF_SHORTDATE_)) GetDateFormatW(0x0400 /* LOCALE_USER_DEFAULT */, (flags & FDTF_LONGDATE_) ? 2 /* DATE_LONGDATE */ : 1 /* DATE_SHORTDATE */, &st, NULL, date, 128);
  if (flags & (FDTF_LONGTIME_ | FDTF_SHORTTIME_)) GetTimeFormatW(0x0400, (flags & FDTF_LONGTIME_) ? 0 : 2 /* TIME_NOSECONDS */, &st, NULL, time_text, 128);
  const size_t_ date_length = wcslen(date), time_length = wcslen(time_text), total = date_length + time_length + (date_length && time_length ? 1 : 0);
  if (total + 1 > capacity) return 0;
  memcpy(out, date, date_length * sizeof(WCHAR));
  n = date_length;
  if (date_length && time_length) out[n++] = ' ';
  memcpy(out + n, time_text, time_length * sizeof(WCHAR));
  out[total] = 0;
  return (int)total + 1;
}
EXPORT int WINAPI SHFormatDateTimeA(const FILETIME *utc, DWORD *flags, LPSTR out, UINT capacity) {
  WCHAR wide[300];
  if (!out || !capacity) return 0;
  if (!SHFormatDateTimeW(utc, flags, wide, 300)) return 0;
  return WideCharToMultiByte(CP_ACP, 0, wide, -1, out, (int)capacity, NULL, NULL);
}
