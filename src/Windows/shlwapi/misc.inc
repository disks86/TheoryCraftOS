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
