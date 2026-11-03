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

/* Loading type libraries: LoadTypeLib/LoadTypeLibEx find the file (a path, or "module\N" for the TYPELIB resource N of a module), read the typelib out of a raw file or the
 * resources of an executable (the PE image is walked here, no module is loaded), build the model with the reader that matches the magic number, and cache the
 * result by full path. */

#define TL_MAX_FILE (256u * 1024 * 1024)

static BYTE *tl_read_file(const WCHAR *path, SIZE_T *size) {
  HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return NULL;
  LARGE_INTEGER li;
  BYTE *buf = NULL;
  if (GetFileSizeEx(f, &li) && li.QuadPart > 0 && li.QuadPart <= TL_MAX_FILE) {
    buf = oa_alloc((SIZE_T)li.QuadPart);
    SIZE_T done = 0;
    while (buf && done < (SIZE_T)li.QuadPart) {
      DWORD got = 0;
      if (!ReadFile(f, buf + done, (DWORD)((SIZE_T)li.QuadPart - done > 0x100000 ? 0x100000 : (SIZE_T)li.QuadPart - done), &got, NULL) || !got) { oa_free(buf); buf = NULL; }
      else done += got;
    }
    if (buf) *size = (SIZE_T)li.QuadPart;
  }
  CloseHandle(f);
  return buf;
}

/* ---- a resource of a PE image ------------------------------------------------------------------------------------------------------------------------------ */
static BOOL pe_u32(const BYTE *d, SIZE_T n, SIZE_T off, DWORD *v) {
  *v = 0;
  if (off > n || n - off < 4) return FALSE;
  *v = (DWORD)d[off] | (DWORD)d[off + 1] << 8 | (DWORD)d[off + 2] << 16 | (DWORD)d[off + 3] << 24;
  return TRUE;
}
static BOOL pe_u16(const BYTE *d, SIZE_T n, SIZE_T off, WORD *v) {
  *v = 0;
  if (off > n || n - off < 2) return FALSE;
  *v = (WORD)(d[off] | d[off + 1] << 8);
  return TRUE;
}
/* RVA to file offset */
static BOOL pe_rva(const BYTE *d, SIZE_T n, SIZE_T sections, UINT nsections, DWORD rva, SIZE_T *off) {
  for (UINT i = 0; i < nsections; i++) {
    DWORD va, vsize, raw, rawsize;
    const SIZE_T at = sections + (SIZE_T)i * 40;
    if (!pe_u32(d, n, at + 8, &vsize) || !pe_u32(d, n, at + 12, &va) || !pe_u32(d, n, at + 16, &rawsize) || !pe_u32(d, n, at + 20, &raw)) return FALSE;
    const DWORD span = vsize > rawsize ? vsize : rawsize;
    if (rva >= va && rva - va < span) {
      const ULONGLONG o = (ULONGLONG)raw + (rva - va);
      if (o >= n) return FALSE;
      *off = (SIZE_T)o;
      return TRUE;
    }
  }
  return FALSE;
}
/* Finds the first leaf below directory `dir` (offset in the resource section) for entry `name` (an id, or 0xffffffff for any); returns the offset of the next level. */
static BOOL pe_dir_entry(const BYTE *d, SIZE_T n, SIZE_T rsrc, SIZE_T dir, DWORD id, const WCHAR *name, DWORD *next) {
  WORD named, ids;
  if (!pe_u16(d, n, rsrc + dir + 12, &named) || !pe_u16(d, n, rsrc + dir + 14, &ids)) return FALSE;
  const UINT total = (UINT)named + ids;
  for (UINT i = 0; i < total && i < 4096; i++) {
    DWORD key, data;
    const SIZE_T at = rsrc + dir + 16 + (SIZE_T)i * 8;
    if (!pe_u32(d, n, at, &key) || !pe_u32(d, n, at + 4, &data)) return FALSE;
    BOOL match;
    if (key & 0x80000000u) {
      match = FALSE;
      WORD len;
      const SIZE_T s = rsrc + (key & 0x7fffffffu);
      if (name && pe_u16(d, n, s, &len) && n - s >= 2 + (SIZE_T)len * 2 && len == oa_wcslen(name)) {
        match = TRUE;
        for (UINT k = 0; k < len; k++) { WORD c; pe_u16(d, n, s + 2 + k * 2, &c); if (oa_towupper(c) != oa_towupper(name[k])) { match = FALSE; break; } }
      }
    } else {
      match = !name && (id == 0xffffffffu || key == id);
    }
    if (match) { *next = data; return TRUE; }
  }
  return FALSE;
}
/* The TYPELIB resource `id` of a PE image in memory; *found = FALSE if `d` is not a PE image. */
static BOOL pe_typelib(const BYTE *d, SIZE_T n, DWORD id, const BYTE **data, SIZE_T *len, BOOL *is_pe) {
  *is_pe = FALSE;
  WORD mz;
  DWORD lfanew, sig;
  if (!pe_u16(d, n, 0, &mz) || mz != 0x5a4d || !pe_u32(d, n, 0x3c, &lfanew) || !pe_u32(d, n, lfanew, &sig) || sig != 0x4550) return FALSE;
  *is_pe = TRUE;
  WORD nsec, optsize, magic;
  const SIZE_T fh = (SIZE_T)lfanew + 4;
  if (!pe_u16(d, n, fh + 2, &nsec) || !pe_u16(d, n, fh + 16, &optsize) || !pe_u16(d, n, fh + 20, &magic)) return FALSE;
  const SIZE_T opt = fh + 20, sections = opt + optsize;
  const SIZE_T dd = opt + (magic == 0x20b ? 112 : 96) + 2 * 8;
  DWORD rva, rsize;
  if (!pe_u32(d, n, dd, &rva) || !pe_u32(d, n, dd + 4, &rsize) || !rva) return FALSE;
  SIZE_T rsrc;
  if (!pe_rva(d, n, sections, nsec, rva, &rsrc)) return FALSE;
  DWORD e1, e2, e3;
  if (!pe_dir_entry(d, n, rsrc, 0, 0, L"TYPELIB", &e1) || !(e1 & 0x80000000u)) return FALSE;
  if (!pe_dir_entry(d, n, rsrc, e1 & 0x7fffffffu, id, NULL, &e2) || !(e2 & 0x80000000u)) return FALSE;
  if (!pe_dir_entry(d, n, rsrc, e2 & 0x7fffffffu, 0xffffffffu, NULL, &e3) || (e3 & 0x80000000u)) return FALSE;
  DWORD drva, dsize, off;
  SIZE_T doff;
  if (!pe_u32(d, n, rsrc + e3, &drva) || !pe_u32(d, n, rsrc + e3 + 4, &dsize) || !pe_rva(d, n, sections, nsec, drva, &doff) || doff >= n || n - doff < dsize) return FALSE;
  (void)off;
  *data = d + doff;
  *len = dsize;
  return TRUE;
}

/* ---- the cache ----------------------------------------------------------------------------------------------------------------------------------------- */
static volatile LONG tl_cache_lock;
static TlLib **tl_cache;
static UINT tl_cache_count, tl_cache_cap;
static void tl_cache_enter(void) { while (InterlockedCompareExchange(&tl_cache_lock, 1, 0)) Sleep(0); }
static void tl_cache_leave(void) { InterlockedExchange(&tl_cache_lock, 0); }

static void tl_cache_remove(TlLib *lib) {
  tl_cache_enter();
  if (lib->ref != 0) { tl_cache_leave(); return; }          /* revived by a lookup in the meantime: the lookup's reference keeps it */
  for (UINT i = 0; i < tl_cache_count; i++)
    if (tl_cache[i] == lib) { tl_cache[i] = tl_cache[--tl_cache_count]; break; }
  lib->in_cache = FALSE;
  tl_cache_leave();
}
static TlLib *tl_cache_find(const WCHAR *path) {
  TlLib *found = NULL;
  tl_cache_enter();
  for (UINT i = 0; i < tl_cache_count && !found; i++)
    if (tl_cache[i]->path && !tl_wcsicmp(tl_cache[i]->path, path)) {
      found = tl_cache[i];
      InterlockedIncrement(&found->ref);
    }
  tl_cache_leave();
  return found;
}
static BOOL tl_cache_find_guid(const GUID *guid, WORD major, WORD minor, ITypeLib **out) {
  BOOL found = FALSE;
  tl_cache_enter();
  for (UINT i = 0; i < tl_cache_count && !found; i++) {
    TlLib *l = tl_cache[i];
    if (l->ref > 0 && IsEqualGUID(&l->guid, guid) && l->major == major && l->minor >= minor) {
      InterlockedIncrement(&l->ref);
      *out = (ITypeLib *)&l->ITypeLib2_iface;
      found = TRUE;
    }
  }
  tl_cache_leave();
  return found;
}
/* Adds a library; if another thread was quicker, returns that one (referenced) instead. */
static TlLib *tl_cache_add(TlLib *lib) {
  TlLib *other = NULL;
  tl_cache_enter();
  for (UINT i = 0; i < tl_cache_count && !other; i++)
    if (tl_cache[i]->path && !tl_wcsicmp(tl_cache[i]->path, lib->path)) { other = tl_cache[i]; InterlockedIncrement(&other->ref); }
  if (!other) {
    if (tl_cache_count >= tl_cache_cap) {
      const UINT ncap = tl_cache_cap ? tl_cache_cap * 2 : 16;
      TlLib **n = oa_realloc(tl_cache, (SIZE_T)ncap * sizeof(*n));
      if (n) { tl_cache = n; tl_cache_cap = ncap; }
    }
    if (tl_cache_count < tl_cache_cap) { tl_cache[tl_cache_count++] = lib; lib->in_cache = TRUE; }
  }
  tl_cache_leave();
  return other;
}

/* ---- from memory --------------------------------------------------------------------------------------------------------------------------------------- */
static HRESULT tl_load_memory(const BYTE *data, SIZE_T size, TlLib **out) {
  if (size >= 4 && data[0] == 'M' && data[1] == 'S' && data[2] == 'F' && data[3] == 'T') return msft_load(data, size, out);
  if (size >= 4 && data[0] == 'S' && data[1] == 'L' && data[2] == 'T' && data[3] == 'G') return sltg_load(data, size, out);
  return TYPE_E_CANTLOADLIBRARY;
}

/* "name\N": the trailing resource index */
static BOOL tl_split_index(const WCHAR *name, WCHAR *base, DWORD *index) {
  const SIZE_T len = oa_wcslen(name);
  SIZE_T i = len;
  while (i > 0 && name[i - 1] >= '0' && name[i - 1] <= '9') i--;
  if (i == len || i == 0 || name[i - 1] != '\\' || i - 1 >= MAX_PATH) return FALSE;
  DWORD v = 0;
  for (SIZE_T k = i; k < len; k++) { if (v > 0xffffffffu / 10 - 1) return FALSE; v = v * 10 + (name[k] - '0'); }
  memcpy(base, name, (i - 1) * sizeof(WCHAR));
  base[i - 1] = 0;
  *index = v;
  return TRUE;
}

static HRESULT tl_load_path(const WCHAR *path, DWORD index, TlLib **out) {
  SIZE_T size = 0;
  BYTE *file = tl_read_file(path, &size);
  if (!file) return TYPE_E_CANTLOADLIBRARY;
  HRESULT hr;
  const BYTE *data = file;
  SIZE_T len = size;
  BOOL is_pe;
  if (size >= 2 && file[0] == 'M' && file[1] == 'Z') {
    if (!pe_typelib(file, size, index ? index : 1, &data, &len, &is_pe)) { oa_free(file); return TYPE_E_CANTLOADLIBRARY; }
  }
  hr = tl_load_memory(data, len, out);
  oa_free(file);
  return hr;
}

EXPORT HRESULT WINAPI LoadTypeLibEx(LPCOLESTR file, REGKIND regkind, ITypeLib **out) {
  if (!file) return E_INVALIDARG;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  WCHAR found[MAX_PATH], full[MAX_PATH], base[MAX_PATH];
  DWORD index = 0;
  const WCHAR *name = file;
  if (!SearchPathW(NULL, name, NULL, MAX_PATH, found, NULL)) {
    if (!tl_split_index(file, base, &index) || !SearchPathW(NULL, base, NULL, MAX_PATH, found, NULL)) {
      /* a name with an extension missing is a registered type library's file name; nothing else can be done with it */
      return TYPE_E_CANTLOADLIBRARY;
    }
  }
  const DWORD fl = GetFullPathNameW(found, MAX_PATH, full, NULL);
  if (!fl || fl >= MAX_PATH) return TYPE_E_CANTLOADLIBRARY;
  /* the cache key includes the resource index */
  WCHAR key[MAX_PATH + 16];
  SIZE_T k = oa_wcslen(full);
  if (k + 12 >= sizeof(key) / sizeof(key[0])) return TYPE_E_CANTLOADLIBRARY;
  memcpy(key, full, (k + 1) * sizeof(WCHAR));
  if (index) {
    key[k++] = '\\';
    WCHAR digits[12];
    int nd = 0;
    for (DWORD v = index; v; v /= 10) digits[nd++] = (WCHAR)('0' + v % 10);
    while (nd) key[k++] = digits[--nd];
    key[k] = 0;
  }
  TlLib *lib = tl_cache_find(key);
  if (!lib) {
    HRESULT hr = tl_load_path(full, index, &lib);
    if (FAILED(hr)) return hr;
    lib->path = tl_wcsdup(key);
    TlLib *other = lib->path ? tl_cache_add(lib) : NULL;
    if (other) { tl_lib_free(lib); lib = other; }
  }
  *out = (ITypeLib *)&lib->ITypeLib2_iface;
  switch (regkind) {
    case REGKIND_DEFAULT:
      /* a library found by a full path is not registered */
      if ((file[0] == '\\' && file[1] == '\\') || (file[0] && file[1] == ':')) break;
      /* fall through */
    case REGKIND_REGISTER: {
      HRESULT hr = RegisterTypeLib(*out, (LPOLESTR)file, NULL);
      if (FAILED(hr)) { ITypeLib_Release(*out); *out = NULL; return hr; }
      break;
    }
    default: break;
  }
  return S_OK;
}

EXPORT HRESULT WINAPI LoadTypeLib(LPCOLESTR file, ITypeLib **out) { return LoadTypeLibEx(file, REGKIND_DEFAULT, out); }
