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

/* Part of d3d12.c: pipeline libraries (ID3D12PipelineLibrary1) and shader cache sessions (ID3D12ShaderCacheSession).
 *
 * A pipeline is made from its description each time here (the kernel keeps each shader once: pso.inc SharedShader), so a library keeps what Direct3D 12 needs
 * of it: the names of the pipelines stored, and the fingerprint of each one's description (pso.inc desc_hash_*). A pipeline is loaded when its name is there and
 * the description given is the one stored, as Direct3D 12 requires; it is made then. Serialize writes the names and fingerprints, and a library made from that
 * blob has them again. A blob not written by this driver is refused with D3D12_ERROR_DRIVER_VERSION_MISMATCH.
 *
 * A shader cache session is a store of values by key for the program (its own compiled shaders, for instance): in memory, or in a file of its own (disk mode,
 * under %LOCALAPPDATA%\D3DSCache\<program>, or the working directory with D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR), kept across runs by its identifier and version. */

/* ---- pipeline libraries ---------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct PlibEntry { WCHAR *name; UINT kind; UINT64 hash; } PlibEntry;      /* kind: 0 graphics, 1 compute */
typedef struct Plib {
  DObj d;
  PlibEntry *entries;
  UINT count, cap;
  CRITICAL_SECTION lock;
} Plib;
static const GUID *const g_plib_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12PipelineLibrary, &IID_ID3D12PipelineLibrary1, NULL};
static const char g_plib_magic[8] = {'T', 'C', 'O', 'S', 'P', 'L', 'B', '1'};

static void plib_destroy(DxObj *o) {
  Plib *l = (Plib *)o;
  for (UINT i = 0; i < l->count; ++i) mem_free(l->entries[i].name);
  mem_free(l->entries);
  DeleteCriticalSection(&l->lock);
  dobj_child_destroy(o);
}
static PlibEntry *plib_find(Plib *l, const WCHAR *name) {
  for (UINT i = 0; i < l->count; ++i) {
    const WCHAR *a = l->entries[i].name, *b = name;
    while (*a && *a == *b) { ++a; ++b; }
    if (*a == *b) return &l->entries[i];
  }
  return NULL;
}
static HRESULT plib_add(Plib *l, const WCHAR *name, SIZE_T chars, UINT kind, UINT64 hash) {
  if (l->count == l->cap) {
    const UINT cap = l->cap ? l->cap * 2 : 16;
    PlibEntry *n = mem_grow(l->entries, sizeof *n * cap);
    if (!n) return E_OUTOFMEMORY;
    l->entries = n;
    l->cap = cap;
  }
  WCHAR *copy = mem_alloc((chars + 1) * sizeof(WCHAR));
  if (!copy) return E_OUTOFMEMORY;
  memcpy(copy, name, chars * sizeof(WCHAR));
  copy[chars] = 0;
  l->entries[l->count++] = (PlibEntry){copy, kind, hash};
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreatePipelineLibrary(Dev *This, const void *blob, SIZE_T size, REFIID riid, void **out) {
  extern const ID3D12PipelineLibrary1Vtbl plib_vtbl;
  if (out) *out = NULL;
  if (size && !blob) return E_INVALIDARG;
  Plib *l = dobj_new(sizeof *l, This, &plib_vtbl, g_plib_iids, plib_destroy);
  if (!l) return E_OUTOFMEMORY;
  InitializeCriticalSection(&l->lock);
  if (size) {
    /* magic, count, then each entry: the name's length in characters, the kind, the fingerprint, the name */
    const BYTE *b = blob;
    if (size < 12 || memcmp(b, g_plib_magic, 8) != 0) { dx_Release(OBJ(l)); return D3D12_ERROR_DRIVER_VERSION_MISMATCH; }
    UINT count;
    memcpy(&count, b + 8, 4);
    SIZE_T at = 12;
    for (UINT i = 0; i < count; ++i) {
      UINT chars, kind;
      UINT64 hash;
      if (size - at < 16) { dx_Release(OBJ(l)); return E_INVALIDARG; }
      memcpy(&chars, b + at, 4);
      memcpy(&kind, b + at + 4, 4);
      memcpy(&hash, b + at + 8, 8);
      at += 16;
      if (chars > 4096 || (SIZE_T)chars * sizeof(WCHAR) > size - at || kind > 1) { dx_Release(OBJ(l)); return E_INVALIDARG; }
      WCHAR name[4097];
      memcpy(name, b + at, (SIZE_T)chars * sizeof(WCHAR));
      name[chars] = 0;
      at += (SIZE_T)chars * sizeof(WCHAR);
      if (plib_find(l, name) || FAILED(plib_add(l, name, chars, kind, hash))) { dx_Release(OBJ(l)); return E_INVALIDARG; }
    }
  }
  return query_out((IUnknown *)l, riid, out);
}
static HRESULT STDMETHODCALLTYPE plib_StorePipeline(Plib *l, const WCHAR *name, ID3D12PipelineState *state) {
  if (!name || !state) return E_INVALIDARG;
  const Pso *p = (const Pso *)state;
  EnterCriticalSection(&l->lock);
  HRESULT hr = E_INVALIDARG;      /* (a name already there) */
  if (!plib_find(l, name)) hr = plib_add(l, name, wlen(name), p->compute ? 1 : 0, p->hash);
  LeaveCriticalSection(&l->lock);
  return hr;
}
/* A stored pipeline's entry when it is there with this kind and fingerprint: E_INVALIDARG when the name is not there or the description is another. */
static HRESULT plib_check(Plib *l, const WCHAR *name, UINT kind, UINT64 hash) {
  if (!name) return E_INVALIDARG;
  EnterCriticalSection(&l->lock);
  const PlibEntry *e = plib_find(l, name);
  const HRESULT hr = e && e->kind == kind && e->hash == hash ? S_OK : E_INVALIDARG;
  LeaveCriticalSection(&l->lock);
  return hr;
}
static HRESULT STDMETHODCALLTYPE plib_LoadGraphicsPipeline(Plib *l, const WCHAR *name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **out) {
  if (out) *out = NULL;
  if (!desc) return E_INVALIDARG;
  const HRESULT hr = plib_check(l, name, 0, desc_hash_graphics(desc));
  return FAILED(hr) ? hr : dev_CreateGraphicsPipelineState(l->d.dev, desc, riid, out);
}
static HRESULT STDMETHODCALLTYPE plib_LoadComputePipeline(Plib *l, const WCHAR *name, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **out) {
  if (out) *out = NULL;
  if (!desc) return E_INVALIDARG;
  const HRESULT hr = plib_check(l, name, 1, desc_hash_compute(desc));
  return FAILED(hr) ? hr : dev_CreateComputePipelineState(l->d.dev, desc, riid, out);
}
static HRESULT STDMETHODCALLTYPE plib_LoadPipeline(Plib *l, const WCHAR *name, const D3D12_PIPELINE_STATE_STREAM_DESC *stream, REFIID riid, void **out) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC g;
  D3D12_COMPUTE_PIPELINE_STATE_DESC c;
  BOOL compute;
  if (out) *out = NULL;
  HRESULT hr = stream_parse(stream, &g, &c, &compute);
  if (FAILED(hr)) return hr;
  hr = plib_check(l, name, compute ? 1 : 0, compute ? desc_hash_compute(&c) : desc_hash_graphics(&g));
  if (FAILED(hr)) return hr;
  return compute ? dev_CreateComputePipelineState(l->d.dev, &c, riid, out) : dev_CreateGraphicsPipelineState(l->d.dev, &g, riid, out);
}
static SIZE_T plib_size(Plib *l) {
  SIZE_T n = 12;
  for (UINT i = 0; i < l->count; ++i) n += 16 + wlen(l->entries[i].name) * sizeof(WCHAR);
  return n;
}
static SIZE_T STDMETHODCALLTYPE plib_GetSerializedSize(Plib *l) {
  EnterCriticalSection(&l->lock);
  const SIZE_T n = plib_size(l);
  LeaveCriticalSection(&l->lock);
  return n;
}
static HRESULT STDMETHODCALLTYPE plib_Serialize(Plib *l, void *data, SIZE_T size) {
  if (!data) return E_INVALIDARG;
  EnterCriticalSection(&l->lock);
  if (size < plib_size(l)) { LeaveCriticalSection(&l->lock); return E_INVALIDARG; }
  BYTE *b = data;
  memcpy(b, g_plib_magic, 8);
  memcpy(b + 8, &l->count, 4);
  SIZE_T at = 12;
  for (UINT i = 0; i < l->count; ++i) {
    const UINT chars = (UINT)wlen(l->entries[i].name);
    memcpy(b + at, &chars, 4);
    memcpy(b + at + 4, &l->entries[i].kind, 4);
    memcpy(b + at + 8, &l->entries[i].hash, 8);
    memcpy(b + at + 16, l->entries[i].name, (SIZE_T)chars * sizeof(WCHAR));
    at += 16 + (SIZE_T)chars * sizeof(WCHAR);
  }
  LeaveCriticalSection(&l->lock);
  return S_OK;
}

/* ---- shader cache sessions -------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct CacheEntry { BYTE *key; UINT key_size; BYTE *value; UINT value_size; } CacheEntry;
typedef struct ShaderCache {
  DObj d;
  D3D12_SHADER_CACHE_SESSION_DESC desc;
  CacheEntry *entries;
  UINT count, cap;
  UINT64 bytes;
  BOOL delete_on_destroy;
  BOOL rewrite;                /* the file is not what the store holds (a clear it did not take): the next store writes all of it */
  WCHAR path[MAX_PATH];        /* disk mode: the file */
  CRITICAL_SECTION lock;
  struct ShaderCache *next;    /* the device's sessions (Dev.caches: ShaderCacheControl clears them) */
} ShaderCache;
static const GUID *const g_scache_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12ShaderCacheSession, NULL};
static const char g_scache_magic[8] = {'T', 'C', 'O', 'S', 'S', 'C', 'C', '1'};

static void scache_clear(ShaderCache *c) {
  for (UINT i = 0; i < c->count; ++i) { mem_free(c->entries[i].key); mem_free(c->entries[i].value); }
  c->count = 0;
  c->bytes = 0;
}
/* Disk mode: the whole store written to its file (magic, version, then key and value sizes and bytes). */
static BOOL write_all(HANDLE f, const void *data, DWORD n) { DWORD w = 0; return WriteFile(f, data, n, &w, NULL) && w == n; }
static BOOL scache_write_entry(HANDLE f, const CacheEntry *e) {
  return write_all(f, &e->key_size, 4) && write_all(f, &e->value_size, 4) && write_all(f, e->key, e->key_size) && write_all(f, e->value, e->value_size);
}
/* Returns the error of a write that failed as an HRESULT (S_OK: written; memory mode: nothing to write). */
static HRESULT scache_io_error(void) { const DWORD e = GetLastError(); return e ? HRESULT_FROM_WIN32(e) : E_FAIL; }
static HRESULT scache_save(ShaderCache *c) {
  if (!c->path[0]) return S_OK;
  HANDLE f = CreateFileW(c->path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return scache_io_error();
  BOOL ok = write_all(f, g_scache_magic, 8) && write_all(f, &c->desc.Version, 8);
  for (UINT i = 0; ok && i < c->count; ++i) ok = scache_write_entry(f, &c->entries[i]);
  const HRESULT hr = ok ? S_OK : scache_io_error();
  CloseHandle(f);
  return hr;
}
/* Disk mode: one new entry added at the end of the file (the whole file is written only when it is new or cleared). */
static HRESULT scache_append(ShaderCache *c, const CacheEntry *e) {
  if (!c->path[0]) return S_OK;
  if (c->rewrite) {
    const HRESULT hr = scache_save(c);
    if (SUCCEEDED(hr)) c->rewrite = FALSE;
    return hr;
  }
  HANDLE f = CreateFileW(c->path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return scache_save(c);      /* (no file yet: all of it, this entry included) */
  LARGE_INTEGER zero = {{0, 0}};
  BOOL ok = SetFilePointerEx(f, zero, NULL, FILE_END) && scache_write_entry(f, e);
  const HRESULT hr = ok ? S_OK : scache_io_error();
  CloseHandle(f);
  return hr;
}
static HRESULT scache_put(ShaderCache *c, const void *key, UINT key_size, const void *value, UINT value_size) {
  if (c->count == c->cap) {
    const UINT cap = c->cap ? c->cap * 2 : 32;
    CacheEntry *n = mem_grow(c->entries, sizeof *n * cap);
    if (!n) return E_OUTOFMEMORY;
    c->entries = n;
    c->cap = cap;
  }
  CacheEntry e = {mem_alloc(key_size ? key_size : 1), key_size, mem_alloc(value_size ? value_size : 1), value_size};
  if (!e.key || !e.value) { mem_free(e.key); mem_free(e.value); return E_OUTOFMEMORY; }
  memcpy(e.key, key, key_size);
  memcpy(e.value, value, value_size);
  c->entries[c->count++] = e;
  c->bytes += (UINT64)key_size + value_size;
  return S_OK;
}
/* Disk mode: what the file holds, when it is of this version (a file of another version is started over, as Direct3D 12 does). */
static void scache_load(ShaderCache *c) {
  HANDLE f = CreateFileW(c->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return;
  char magic[8];
  UINT64 version = 0;
  DWORD r;
  if (ReadFile(f, magic, 8, &r, NULL) && r == 8 && memcmp(magic, g_scache_magic, 8) == 0 && ReadFile(f, &version, 8, &r, NULL) && r == 8 && version == c->desc.Version) {
    for (;;) {
      UINT sizes[2];
      /* (a record is refused - and the rest of the file with it - when it is bigger than a value may be, or would take the store over its limits) */
      if (!ReadFile(f, sizes, 8, &r, NULL) || r != 8 || !sizes[0] || !sizes[1] || sizes[0] > (1u << 20) || sizes[1] > (128u << 20)) break;
      if (c->bytes + (UINT64)sizes[0] + sizes[1] > c->desc.MaximumValueFileSizeBytes) break;
      BYTE *key = mem_alloc((SIZE_T)sizes[0]), *value = mem_alloc((SIZE_T)sizes[1]);
      BOOL ok = key && value && ReadFile(f, key, sizes[0], &r, NULL) && r == sizes[0] && ReadFile(f, value, sizes[1], &r, NULL) && r == sizes[1];
      if (ok) ok = SUCCEEDED(scache_put(c, key, sizes[0], value, sizes[1]));
      mem_free(key);
      mem_free(value);
      if (!ok) break;
    }
  }
  CloseHandle(f);
}
static void scache_destroy(DxObj *o) {
  ShaderCache *c = (ShaderCache *)o;
  Dev *d = c->d.dev;
  AcquireSRWLockExclusive(&d->cache_lock);
  for (ShaderCache **at = &d->caches; *at; at = &(*at)->next) if (*at == c) { *at = c->next; break; }
  ReleaseSRWLockExclusive(&d->cache_lock);
  if (c->path[0] && c->delete_on_destroy) DeleteFileW(c->path);
  scache_clear(c);
  mem_free(c->entries);
  DeleteCriticalSection(&c->lock);
  dobj_child_destroy(o);
}
/* %LOCALAPPDATA%\D3DSCache\<program>\<identifier>.tcos (or the working directory's <identifier>.tcos), the folders made. */
static BOOL scache_path(ShaderCache *c) {
  WCHAR dir[MAX_PATH];
  DWORD n;
  if (c->desc.Flags & D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR) {
    n = GetCurrentDirectoryW(MAX_PATH, dir);
    if (!n || n >= MAX_PATH - 48) return FALSE;
  } else {
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (!n || n >= MAX_PATH - 120) return FALSE;
    static const WCHAR sub[] = L"\\D3DSCache";
    memcpy(dir + n, sub, sizeof sub);
    n += (DWORD)(sizeof sub / sizeof sub[0]) - 1;
    CreateDirectoryW(dir, NULL);
    WCHAR exe[MAX_PATH];
    const DWORD e = GetModuleFileNameW(NULL, exe, MAX_PATH);
    DWORD start = e;
    while (start && exe[start - 1] != L'\\' && exe[start - 1] != L'/') --start;
    dir[n++] = L'\\';
    for (DWORD i = start; i < e && n < MAX_PATH - 48; ++i) dir[n++] = exe[i];
    dir[n] = 0;
    CreateDirectoryW(dir, NULL);
  }
  /* the identifier as 32 hex digits */
  static const WCHAR hex[] = L"0123456789abcdef";
  const BYTE *g = (const BYTE *)&c->desc.Identifier;
  dir[n++] = L'\\';
  for (UINT i = 0; i < 16; ++i) { dir[n++] = hex[g[i] >> 4]; dir[n++] = hex[g[i] & 15]; }
  static const WCHAR ext[] = L".tcos";
  memcpy(dir + n, ext, sizeof ext);
  memcpy(c->path, dir, sizeof c->path);
  return TRUE;
}
static HRESULT STDMETHODCALLTYPE dev_CreateShaderCacheSession(Dev *This, const D3D12_SHADER_CACHE_SESSION_DESC *desc, REFIID riid, void **out) {
  extern const ID3D12ShaderCacheSessionVtbl scache_vtbl;
  if (out) *out = NULL;
  if (!desc || desc->Mode > D3D12_SHADER_CACHE_MODE_DISK || (desc->Flags & ~(UINT)(D3D12_SHADER_CACHE_FLAG_DRIVER_VERSIONED | D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR))) return E_INVALIDARG;
  if (desc->Mode == D3D12_SHADER_CACHE_MODE_MEMORY && desc->Flags) return E_INVALIDARG;      /* (the flags are of disk caches) */
  if (desc->Mode == D3D12_SHADER_CACHE_MODE_MEMORY && desc->MaximumValueFileSizeBytes) return E_INVALIDARG;
  if (!out) return S_FALSE;
  ShaderCache *c = dobj_new(sizeof *c, This, &scache_vtbl, g_scache_iids, scache_destroy);
  if (!c) return E_OUTOFMEMORY;
  InitializeCriticalSection(&c->lock);
  c->desc = *desc;
  /* (the defaults Direct3D 12 gives what is left 0) */
  if (!c->desc.MaximumInMemoryCacheSizeBytes) c->desc.MaximumInMemoryCacheSizeBytes = 1u << 20;
  if (!c->desc.MaximumInMemoryCacheEntries) c->desc.MaximumInMemoryCacheEntries = 128;
  if (desc->Mode == D3D12_SHADER_CACHE_MODE_DISK && !c->desc.MaximumValueFileSizeBytes) c->desc.MaximumValueFileSizeBytes = 128u << 20;
  if (desc->Mode == D3D12_SHADER_CACHE_MODE_DISK) {
    if (!scache_path(c)) { dx_Release(OBJ(c)); return E_FAIL; }
    scache_load(c);
    /* (the file as this session has it: a file of another version, or cut short, is started over, so later entries can be added at its end) */
    const HRESULT saved = scache_save(c);
    if (FAILED(saved)) { dx_Release(OBJ(c)); return saved; }
  }
  AcquireSRWLockExclusive(&This->cache_lock);
  c->next = This->caches;
  This->caches = c;
  ReleaseSRWLockExclusive(&This->cache_lock);
  return query_out((IUnknown *)c, riid, out);
}
static CacheEntry *scache_find(ShaderCache *c, const void *key, UINT key_size) {
  for (UINT i = 0; i < c->count; ++i)
    if (c->entries[i].key_size == key_size && memcmp(c->entries[i].key, key, key_size) == 0) return &c->entries[i];
  return NULL;
}
static HRESULT STDMETHODCALLTYPE scache_FindValue(ShaderCache *c, const void *key, UINT key_size, void *value, UINT *value_size) {
  if (!key || !key_size || !value_size) return E_INVALIDARG;
  EnterCriticalSection(&c->lock);
  const CacheEntry *e = scache_find(c, key, key_size);
  HRESULT hr = DXGI_ERROR_NOT_FOUND;
  if (e) {
    if (!value) hr = S_OK;
    else if (*value_size < e->value_size) hr = DXGI_ERROR_MORE_DATA;
    else { memcpy(value, e->value, e->value_size); hr = S_OK; }
    *value_size = e->value_size;
  }
  LeaveCriticalSection(&c->lock);
  return hr;
}
static HRESULT STDMETHODCALLTYPE scache_StoreValue(ShaderCache *c, const void *key, UINT key_size, const void *value, UINT value_size) {
  if (!key || !key_size || !value || !value_size) return E_INVALIDARG;
  EnterCriticalSection(&c->lock);
  HRESULT hr;
  /* (a memory session holds what its in-memory limits allow; a disk session what its file may hold - the in-memory limits bound only what is kept in memory on Windows,
   * here the whole store is) */
  const UINT64 adding = (UINT64)key_size + value_size;
  const BOOL full = c->path[0] ? c->bytes + adding > c->desc.MaximumValueFileSizeBytes
                               : c->count >= c->desc.MaximumInMemoryCacheEntries || c->bytes + adding > c->desc.MaximumInMemoryCacheSizeBytes;
  if (scache_find(c, key, key_size)) hr = DXGI_ERROR_ALREADY_EXISTS;
  else if (full) hr = DXGI_ERROR_CACHE_FULL;
  else {
    hr = scache_put(c, key, key_size, value, value_size);
    if (SUCCEEDED(hr)) {
      hr = scache_append(c, &c->entries[c->count - 1]);
      if (FAILED(hr)) {      /* (not stored: the file has not got it) */
        CacheEntry *e = &c->entries[--c->count];
        c->bytes -= adding;
        mem_free(e->key);
        mem_free(e->value);
      }
    }
  }
  LeaveCriticalSection(&c->lock);
  return hr;
}
static void STDMETHODCALLTYPE scache_SetDeleteOnDestroy(ShaderCache *c) { c->delete_on_destroy = TRUE; }
static D3D12_SHADER_CACHE_SESSION_DESC *STDMETHODCALLTYPE scache_GetDesc(ShaderCache *c, D3D12_SHADER_CACHE_SESSION_DESC *ret) { *ret = c->desc; return ret; }

/* The caches of the system and the driver: there are none here (pipelines are made from their descriptions), so disabling, enabling or clearing them has nothing
 * to do; clearing the application's empties the device's sessions. */
static HRESULT STDMETHODCALLTYPE dev_ShaderCacheControl(Dev *This, D3D12_SHADER_CACHE_KIND_FLAGS kinds, D3D12_SHADER_CACHE_CONTROL_FLAGS control) {
  const UINT all_kinds = D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_D3D_CACHE_FOR_DRIVER | D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_D3D_CONVERSIONS |
                         D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_DRIVER_MANAGED | D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED;
  const UINT all_controls = D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE | D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE | D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR;
  if (!kinds || (kinds & ~all_kinds) || !control || (control & ~all_controls)) return E_INVALIDARG;
  if ((control & D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE) && (control & D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE)) return E_INVALIDARG;
  if ((kinds & D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED) && (control & D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR)) {
    AcquireSRWLockShared(&This->cache_lock);
    for (ShaderCache *c = This->caches; c; c = c->next) {
      EnterCriticalSection(&c->lock);
      scache_clear(c);
      c->rewrite = FAILED(scache_save(c));      /* (a clear the file could not take: the next StoreValue writes it again) */
      LeaveCriticalSection(&c->lock);
    }
    ReleaseSRWLockShared(&This->cache_lock);
  }
  return S_OK;
}
