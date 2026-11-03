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

/* What programs that use the shell lean on besides the path and string functions: STRRET conversion, SHStrDup, the registry helpers (SHGetValue ...), streams on files,
 * the stream helpers, searching the PATH, and AssocQueryString over the file associations in the registry. Included by shlwapi.c. */

typedef long LSTATUS_;
typedef ULONG REGSAM_;
typedef struct { UINT uType; union { LPWSTR pOleStr; UINT uOffset; char cStr[MAX_PATH]; } u; } STRRET_;
#define STRRET_WSTR_ 0
#define STRRET_OFFSET_ 1
#define STRRET_CSTR_ 2
#define HKEY_CLASSES_ROOT_ ((HKEY)(ULONG_PTR)(LONG)0x80000000)
#define HKEY_CURRENT_USER_ ((HKEY)(ULONG_PTR)(LONG)0x80000001)
#define HKEY_LOCAL_MACHINE_ ((HKEY)(ULONG_PTR)(LONG)0x80000002)
#define KEY_READ_ 0x20019
#define KEY_WRITE_ 0x20006
#define KEY_ALL_ACCESS_ 0xF003F
#define REG_SZ_ 1
#define REG_EXPAND_SZ_ 2
#define E_FAIL_ ((LONG)0x80004005)
#define E_POINTER_ ((LONG)0x80004003)
#define E_NOINTERFACE_ ((LONG)0x80004002)
#define E_NOTIMPL_ ((LONG)0x80004001)
#define ERROR_NO_ASSOCIATION_ 1155

IMPORT LSTATUS_ WINAPI RegOpenKeyExW(HKEY key, LPCWSTR subkey, DWORD options, REGSAM_ access, HKEY *result);
IMPORT LSTATUS_ WINAPI RegCreateKeyExW(HKEY key, LPCWSTR subkey, DWORD reserved, LPWSTR cls, DWORD options, REGSAM_ access, LPVOID sa, HKEY *result, LPDWORD disposition);
IMPORT LSTATUS_ WINAPI RegCloseKey(HKEY key);
IMPORT LSTATUS_ WINAPI RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size);
IMPORT LSTATUS_ WINAPI RegSetValueExW(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD size);
IMPORT LSTATUS_ WINAPI RegDeleteKeyW(HKEY key, LPCWSTR subkey);
IMPORT LSTATUS_ WINAPI RegDeleteValueW(HKEY key, LPCWSTR name);
IMPORT LSTATUS_ WINAPI RegEnumKeyExW(HKEY key, DWORD index, LPWSTR name, LPDWORD size, LPDWORD reserved, LPWSTR cls, LPDWORD cls_size, LPFILETIME time);
IMPORT DWORD WINAPI ExpandEnvironmentStringsW(LPCWSTR source, LPWSTR dest, DWORD capacity);
IMPORT HANDLE WINAPI CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE template_file);
IMPORT BOOL WINAPI ReadFile(HANDLE h, LPVOID buffer, DWORD n, LPDWORD done, LPOVERLAPPED o);
IMPORT BOOL WINAPI WriteFile(HANDLE h, LPCVOID buffer, DWORD n, LPDWORD done, LPOVERLAPPED o);
IMPORT BOOL WINAPI SetFilePointerEx(HANDLE h, LARGE_INTEGER distance, PLARGE_INTEGER result, DWORD method);
IMPORT BOOL WINAPI SetEndOfFile(HANDLE h);
IMPORT BOOL WINAPI GetFileSizeEx(HANDLE h, PLARGE_INTEGER size);
IMPORT BOOL WINAPI FlushFileBuffers(HANDLE h);
IMPORT BOOL WINAPI CloseHandle(HANDLE h);
IMPORT BOOL WINAPI GetFileTime(HANDLE h, LPFILETIME created, LPFILETIME accessed, LPFILETIME written);
IMPORT LONG WINAPI InterlockedIncrement(volatile LONG *p);
IMPORT LONG WINAPI InterlockedDecrement(volatile LONG *p);
IMPORT DWORD WINAPI SearchPathW(LPCWSTR path, LPCWSTR file, LPCWSTR ext, DWORD capacity, LPWSTR buffer, LPWSTR *file_part);
IMPORT UINT WINAPI GetDriveTypeW(LPCWSTR root);
IMPORT HANDLE WINAPI FindFirstFileW(LPCWSTR name, void *data);
IMPORT BOOL WINAPI FindNextFileW(HANDLE h, void *data);
IMPORT BOOL WINAPI FindClose(HANDLE h);
IMPORT HANDLE WINAPI GetProcessHeap(void);
IMPORT LPVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes);
IMPORT BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, LPVOID memory);
IMPORT LPVOID WINAPI HeapReAlloc(HANDLE heap, DWORD flags, LPVOID memory, SIZE_T bytes);

static void *task_alloc(SIZE_T bytes) { return HeapAlloc(GetProcessHeap(), 0, bytes ? bytes : 1); }
static void task_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static WCHAR *to_wide_dup(LPCSTR s) {
  if (!s) return NULL;
  const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
  WCHAR *w = task_alloc((SIZE_T)(n > 0 ? n : 1) * sizeof(WCHAR));
  if (w) { if (n > 0) MultiByteToWideChar(CP_ACP, 0, s, -1, w, n); else w[0] = 0; }
  return w;
}

/* ---- STRRET and SHStrDup ----------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HRESULT_ WINAPI StrRetToBufW(STRRET_ *s, const void *pidl, LPWSTR buffer, UINT capacity) {
  if (!s || !buffer || !capacity) { if (buffer && capacity) buffer[0] = 0; return E_FAIL_; }
  HRESULT_ hr = 0;
  switch (s->uType) {
    case STRRET_WSTR_: {
      if (!s->u.pOleStr) { buffer[0] = 0; return E_FAIL_; }
      UINT i = 0;
      for (; s->u.pOleStr[i] && i + 1 < capacity; ++i) buffer[i] = s->u.pOleStr[i];
      buffer[i] = 0;
      task_free(s->u.pOleStr);
      s->u.pOleStr = NULL;
      break;
    }
    case STRRET_CSTR_: if (!MultiByteToWideChar(CP_ACP, 0, s->u.cStr, -1, buffer, (int)capacity)) { buffer[0] = 0; hr = E_FAIL_; } break;
    case STRRET_OFFSET_:
      if (!pidl || !MultiByteToWideChar(CP_ACP, 0, (const char *)pidl + s->u.uOffset, -1, buffer, (int)capacity)) { buffer[0] = 0; hr = E_FAIL_; }
      break;
    default: buffer[0] = 0; hr = E_FAIL_; break;
  }
  return hr;
}
EXPORT HRESULT_ WINAPI StrRetToBufA(STRRET_ *s, const void *pidl, LPSTR buffer, UINT capacity) {
  WCHAR wide[MAX_PATH_ * 4];
  if (!s || !buffer || !capacity) return E_FAIL_;
  buffer[0] = 0;
  if (s->uType == STRRET_CSTR_) {
    UINT i = 0;
    for (; s->u.cStr[i] && i + 1 < capacity; ++i) buffer[i] = s->u.cStr[i];
    buffer[i] = 0;
    return 0;
  }
  const HRESULT_ hr = StrRetToBufW(s, pidl, wide, (UINT)(sizeof(wide) / sizeof(wide[0])));
  if (hr) return hr;
  if (!WideCharToMultiByte(CP_ACP, 0, wide, -1, buffer, (int)capacity, NULL, NULL)) { buffer[capacity - 1] = 0; }
  return 0;
}
EXPORT HRESULT_ WINAPI StrRetToStrW(STRRET_ *s, const void *pidl, LPWSTR *out) {
  WCHAR buffer[MAX_PATH_ * 4];
  if (!s || !out) return E_FAIL_;
  *out = NULL;
  if (s->uType == STRRET_WSTR_) { *out = s->u.pOleStr; s->u.pOleStr = NULL; return *out ? 0 : E_FAIL_; }
  const HRESULT_ hr = StrRetToBufW(s, pidl, buffer, (UINT)(sizeof(buffer) / sizeof(buffer[0])));
  if (hr) return hr;
  const size_t_ n = wcslen(buffer);
  *out = task_alloc((n + 1) * sizeof(WCHAR));
  if (!*out) return E_OUTOFMEMORY_;
  memcpy(*out, buffer, (n + 1) * sizeof(WCHAR));
  return 0;
}
EXPORT HRESULT_ WINAPI StrRetToStrA(STRRET_ *s, const void *pidl, LPSTR *out) {
  WCHAR buffer[MAX_PATH_ * 4];
  if (!s || !out) return E_FAIL_;
  *out = NULL;
  const HRESULT_ hr = StrRetToBufW(s, pidl, buffer, (UINT)(sizeof(buffer) / sizeof(buffer[0])));
  if (hr) return hr;
  const int n = WideCharToMultiByte(CP_ACP, 0, buffer, -1, NULL, 0, NULL, NULL);
  *out = task_alloc((SIZE_T)(n > 0 ? n : 1));
  if (!*out) return E_OUTOFMEMORY_;
  if (n > 0) WideCharToMultiByte(CP_ACP, 0, buffer, -1, *out, n, NULL, NULL); else (*out)[0] = 0;
  return 0;
}
EXPORT HRESULT_ WINAPI SHStrDupW(LPCWSTR s, LPWSTR *out) {
  if (!out) return E_POINTER_;
  *out = NULL;
  if (!s) return E_INVALIDARG_;
  const size_t_ n = wcslen(s);
  *out = task_alloc((n + 1) * sizeof(WCHAR));
  if (!*out) return E_OUTOFMEMORY_;
  memcpy(*out, s, (n + 1) * sizeof(WCHAR));
  return 0;
}
EXPORT HRESULT_ WINAPI SHStrDupA(LPCSTR s, LPWSTR *out) {
  if (!out) return E_POINTER_;
  *out = to_wide_dup(s);
  if (!s) return E_INVALIDARG_;
  return *out ? 0 : E_OUTOFMEMORY_;
}

/* ---- registry helpers --------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT LSTATUS_ WINAPI SHGetValueW(HKEY root, LPCWSTR subkey, LPCWSTR value, LPDWORD type, LPVOID data, LPDWORD size) {
  HKEY key;
  LSTATUS_ r = RegOpenKeyExW(root, subkey, 0, KEY_READ_, &key);
  if (r) return r;
  r = RegQueryValueExW(key, value, NULL, type, data, size);
  RegCloseKey(key);
  return r;
}
EXPORT LSTATUS_ WINAPI SHGetValueA(HKEY root, LPCSTR subkey, LPCSTR value, LPDWORD type, LPVOID data, LPDWORD size) {
  WCHAR *k = to_wide_dup(subkey), *v = to_wide_dup(value);
  LSTATUS_ r = ERROR_OUTOFMEMORY;
  if ((!subkey || k) && (!value || v)) {
    DWORD t = 0;
    if (!data) r = SHGetValueW(root, k, v, type, NULL, size);
    else {
      /* strings come back as ANSI: read wide, convert */
      DWORD wide_size = size ? *size * 2 : 0;
      BYTE *wide = task_alloc(wide_size ? wide_size : 2);
      if (wide) {
        r = SHGetValueW(root, k, v, &t, wide, &wide_size);
        if (type) *type = t;
        if (!r && (t == REG_SZ_ || t == REG_EXPAND_SZ_)) {
          const int n = WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)wide, -1, data, size ? (int)*size : 0, NULL, NULL);
          if (!n) r = ERROR_MORE_DATA;
          else if (size) *size = (DWORD)n;
        } else if (!r) {
          if (size && wide_size > *size) r = ERROR_MORE_DATA; else { memcpy(data, wide, wide_size); if (size) *size = wide_size; }
        } else if (size && r == ERROR_MORE_DATA) *size = wide_size;
        task_free(wide);
      }
    }
  }
  task_free(k); task_free(v);
  return r;
}
EXPORT LSTATUS_ WINAPI SHSetValueW(HKEY root, LPCWSTR subkey, LPCWSTR value, DWORD type, LPCVOID data, DWORD size) {
  HKEY key;
  LSTATUS_ r = RegCreateKeyExW(root, subkey, 0, NULL, 0, KEY_WRITE_, NULL, &key, NULL);
  if (r) return r;
  r = RegSetValueExW(key, value, 0, type, data, size);
  RegCloseKey(key);
  return r;
}
EXPORT LSTATUS_ WINAPI SHSetValueA(HKEY root, LPCSTR subkey, LPCSTR value, DWORD type, LPCVOID data, DWORD size) {
  WCHAR *k = to_wide_dup(subkey), *v = to_wide_dup(value);
  LSTATUS_ r = ERROR_OUTOFMEMORY;
  if ((!subkey || k) && (!value || v)) {
    if (type == REG_SZ_ || type == REG_EXPAND_SZ_) {
      WCHAR *d = to_wide_dup((LPCSTR)data);
      if (d) { r = SHSetValueW(root, k, v, type, d, (DWORD)((wcslen(d) + 1) * sizeof(WCHAR))); task_free(d); }
    } else r = SHSetValueW(root, k, v, type, data, size);
  }
  task_free(k); task_free(v);
  return r;
}
EXPORT LSTATUS_ WINAPI SHQueryValueExW(HKEY key, LPCWSTR value, LPDWORD reserved, LPDWORD type, LPVOID data, LPDWORD size) {
  DWORD t = 0, need = 0;
  (void)reserved;
  LSTATUS_ r = RegQueryValueExW(key, value, NULL, &t, NULL, &need);
  if (r) return r;
  if (type) *type = t == REG_EXPAND_SZ_ ? REG_SZ_ : t;
  if (t != REG_EXPAND_SZ_) return RegQueryValueExW(key, value, NULL, NULL, data, size);
  /* an expandable string comes back expanded */
  WCHAR *raw = task_alloc(need + 2);
  if (!raw) return ERROR_OUTOFMEMORY;
  DWORD got = need;
  r = RegQueryValueExW(key, value, NULL, NULL, (LPBYTE)raw, &got);
  if (!r) {
    raw[got / sizeof(WCHAR)] = 0;
    const DWORD n = ExpandEnvironmentStringsW(raw, NULL, 0);
    if (size) {
      if (!data || *size < n * sizeof(WCHAR)) { r = data ? ERROR_MORE_DATA : 0; *size = n * sizeof(WCHAR); }
      else { ExpandEnvironmentStringsW(raw, data, *size / sizeof(WCHAR)); *size = n * sizeof(WCHAR); }
    }
  }
  task_free(raw);
  return r;
}
EXPORT LSTATUS_ WINAPI SHQueryValueExA(HKEY key, LPCSTR value, LPDWORD reserved, LPDWORD type, LPVOID data, LPDWORD size) {
  WCHAR *v = to_wide_dup(value);
  DWORD t = 0, wide_size = size ? *size * 2 : 0;
  BYTE *wide = task_alloc(wide_size ? wide_size : 2);
  LSTATUS_ r = ERROR_OUTOFMEMORY;
  if ((!value || v) && wide) {
    r = SHQueryValueExW(key, v, reserved, &t, data ? wide : NULL, &wide_size);
    if (type) *type = t;
    if (!r && data && t == REG_SZ_) {
      const int n = WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)wide, -1, data, size ? (int)*size : 0, NULL, NULL);
      if (!n) r = ERROR_MORE_DATA; else if (size) *size = (DWORD)n;
    } else if (!r && data) {
      if (size && wide_size > *size) r = ERROR_MORE_DATA; else { memcpy(data, wide, wide_size); if (size) *size = wide_size; }
    } else if (!r || r == ERROR_MORE_DATA) { if (size) *size = t == REG_SZ_ ? wide_size / 2 : wide_size; }
  }
  task_free(v); task_free(wide);
  return r;
}
EXPORT LSTATUS_ WINAPI SHDeleteKeyW(HKEY root, LPCWSTR subkey) {
  HKEY key;
  LSTATUS_ r = RegOpenKeyExW(root, subkey, 0, KEY_ALL_ACCESS_, &key);
  if (r) return r;
  for (;;) {
    WCHAR name[260];
    DWORD n = 260;
    if (RegEnumKeyExW(key, 0, name, &n, NULL, NULL, NULL, NULL)) break;
    if (SHDeleteKeyW(key, name)) break;
  }
  RegCloseKey(key);
  return RegDeleteKeyW(root, subkey);
}
EXPORT LSTATUS_ WINAPI SHDeleteKeyA(HKEY root, LPCSTR subkey) {
  WCHAR *k = to_wide_dup(subkey);
  const LSTATUS_ r = k || !subkey ? SHDeleteKeyW(root, k) : ERROR_OUTOFMEMORY;
  task_free(k);
  return r;
}
EXPORT LSTATUS_ WINAPI SHDeleteEmptyKeyW(HKEY root, LPCWSTR subkey) {
  HKEY key;
  LSTATUS_ r = RegOpenKeyExW(root, subkey, 0, KEY_READ_, &key);
  if (r) return r;
  WCHAR name[260];
  DWORD n = 260;
  const BOOL has_children = RegEnumKeyExW(key, 0, name, &n, NULL, NULL, NULL, NULL) == 0;
  RegCloseKey(key);
  return has_children ? ERROR_KEY_HAS_CHILDREN : RegDeleteKeyW(root, subkey);
}
EXPORT LSTATUS_ WINAPI SHDeleteValueW(HKEY root, LPCWSTR subkey, LPCWSTR value) {
  HKEY key;
  LSTATUS_ r = RegOpenKeyExW(root, subkey, 0, KEY_WRITE_, &key);
  if (r) return r;
  r = RegDeleteValueW(key, value);
  RegCloseKey(key);
  return r;
}
EXPORT LSTATUS_ WINAPI SHRegGetPathW(HKEY root, LPCWSTR subkey, LPCWSTR value, LPWSTR path, DWORD flags) {
  DWORD size = MAX_PATH_ * sizeof(WCHAR), type = 0;
  (void)flags;
  HKEY key;
  LSTATUS_ r = RegOpenKeyExW(root, subkey, 0, KEY_READ_, &key);
  if (r) return r;
  r = SHQueryValueExW(key, value, NULL, &type, path, &size);
  RegCloseKey(key);
  return r;
}

/* ---- streams on files -------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct IStreamLike {
  const struct IStreamLikeVtbl {
    HRESULT_ (WINAPI *QueryInterface)(struct IStreamLike *, const void *, void **);
    ULONG (WINAPI *AddRef)(struct IStreamLike *);
    ULONG (WINAPI *Release)(struct IStreamLike *);
    HRESULT_ (WINAPI *Read)(struct IStreamLike *, void *, ULONG, ULONG *);
    HRESULT_ (WINAPI *Write)(struct IStreamLike *, const void *, ULONG, ULONG *);
    HRESULT_ (WINAPI *Seek)(struct IStreamLike *, LARGE_INTEGER, DWORD, ULARGE_INTEGER *);
    HRESULT_ (WINAPI *SetSize)(struct IStreamLike *, ULARGE_INTEGER);
    HRESULT_ (WINAPI *CopyTo)(struct IStreamLike *, struct IStreamLike *, ULARGE_INTEGER, ULARGE_INTEGER *, ULARGE_INTEGER *);
    HRESULT_ (WINAPI *Commit)(struct IStreamLike *, DWORD);
    HRESULT_ (WINAPI *Revert)(struct IStreamLike *);
    HRESULT_ (WINAPI *LockRegion)(struct IStreamLike *, ULARGE_INTEGER, ULARGE_INTEGER, DWORD);
    HRESULT_ (WINAPI *UnlockRegion)(struct IStreamLike *, ULARGE_INTEGER, ULARGE_INTEGER, DWORD);
    HRESULT_ (WINAPI *Stat)(struct IStreamLike *, void *, DWORD);
    HRESULT_ (WINAPI *Clone)(struct IStreamLike *, struct IStreamLike **);
  } *vtbl;
} IStreamLike;

static const BYTE kIidUnknown[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
static const BYTE kIidStream[16] = {0x0C, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
static const BYTE kIidSequential[16] = {0x0A, 0, 0x01, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
#define STG_E_ACCESSDENIED_ ((LONG)0x80030005)
#define STG_E_INVALIDFUNCTION_ ((LONG)0x80030001)
#define STG_E_INVALIDPOINTER_ ((LONG)0x80030009)
#define STG_E_INVALIDPARAMETER_ ((LONG)0x80030057)
#define STG_E_REVERTED_ ((LONG)0x80030102)

typedef struct { const struct IStreamLikeVtbl *vtbl; LONG ref; HANDLE file; DWORD mode; } FileStream;
static BOOL same_iid(const void *a, const BYTE *b) { return !memcmp(a, b, 16); }
/* The GUIDs written as bytes: Data1..Data3 little-endian. */
static HRESULT_ WINAPI fs_query(IStreamLike *self, const void *iid, void **out) {
  static const struct { DWORD d1; WORD d2, d3; BYTE d4[8]; } stream_iid = {0x0000000C, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}}, seq_iid = {0x0C733A30, 0x2A1C, 0x11CE, {0xAD, 0xE5, 0x00, 0xAA, 0x00, 0x44, 0x77, 0x3D}},
      unknown_iid = {0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
  (void)kIidUnknown; (void)kIidStream; (void)kIidSequential; (void)same_iid;
  if (!out) return E_POINTER_;
  *out = NULL;
  if (!memcmp(iid, &stream_iid, 16) || !memcmp(iid, &seq_iid, 16) || !memcmp(iid, &unknown_iid, 16)) { *out = self; InterlockedIncrement(&((FileStream *)self)->ref); return 0; }
  return E_NOINTERFACE_;
}
static ULONG WINAPI fs_addref(IStreamLike *self) { return (ULONG)InterlockedIncrement(&((FileStream *)self)->ref); }
static ULONG WINAPI fs_release(IStreamLike *self) {
  FileStream *s = (FileStream *)self;
  const LONG n = InterlockedDecrement(&s->ref);
  if (!n) { if (s->file != INVALID_HANDLE_VALUE) CloseHandle(s->file); task_free(s); }
  return (ULONG)n;
}
static HRESULT_ WINAPI fs_read(IStreamLike *self, void *buffer, ULONG count, ULONG *read) {
  FileStream *s = (FileStream *)self;
  DWORD got = 0;
  if (!buffer) return STG_E_INVALIDPOINTER_;
  if (!ReadFile(s->file, buffer, count, &got, NULL)) { if (read) *read = 0; return HRESULT_FROM_WIN32_(GetLastError()); }
  if (read) *read = got;
  return got == count ? 0 : 1;
}
static HRESULT_ WINAPI fs_write(IStreamLike *self, const void *buffer, ULONG count, ULONG *written) {
  FileStream *s = (FileStream *)self;
  DWORD done = 0;
  if (!buffer) return STG_E_INVALIDPOINTER_;
  if ((s->mode & 3) == 0) { if (written) *written = 0; return STG_E_ACCESSDENIED_; }
  if (!WriteFile(s->file, buffer, count, &done, NULL)) { if (written) *written = 0; return HRESULT_FROM_WIN32_(GetLastError()); }
  if (written) *written = done;
  return 0;
}
static HRESULT_ WINAPI fs_seek(IStreamLike *self, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *out) {
  FileStream *s = (FileStream *)self;
  LARGE_INTEGER result;
  if (origin > 2) return STG_E_INVALIDFUNCTION_;
  if (!SetFilePointerEx(s->file, move, &result, origin)) return STG_E_INVALIDFUNCTION_;
  if (out) out->QuadPart = (ULONGLONG)result.QuadPart;
  return 0;
}
static HRESULT_ WINAPI fs_set_size(IStreamLike *self, ULARGE_INTEGER size) {
  FileStream *s = (FileStream *)self;
  LARGE_INTEGER here, zero, target;
  zero.QuadPart = 0;
  if ((s->mode & 3) == 0) return STG_E_ACCESSDENIED_;
  if (!SetFilePointerEx(s->file, zero, &here, 1)) return E_FAIL_;
  target.QuadPart = (LONGLONG)size.QuadPart;
  if (!SetFilePointerEx(s->file, target, NULL, 0) || !SetEndOfFile(s->file)) { SetFilePointerEx(s->file, here, NULL, 0); return E_FAIL_; }
  SetFilePointerEx(s->file, here, NULL, 0);
  return 0;
}
static HRESULT_ WINAPI fs_copy_to(IStreamLike *self, IStreamLike *dest, ULARGE_INTEGER count, ULARGE_INTEGER *read, ULARGE_INTEGER *written) {
  BYTE buffer[1024];
  ULONGLONG left = count.QuadPart, total_read = 0, total_written = 0;
  HRESULT_ hr = 0;
  if (!dest) return STG_E_INVALIDPOINTER_;
  while (left) {
    ULONG got = 0, put = 0;
    hr = fs_read(self, buffer, left < sizeof(buffer) ? (ULONG)left : (ULONG)sizeof(buffer), &got);
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
static HRESULT_ WINAPI fs_commit(IStreamLike *self, DWORD flags) { (void)flags; FlushFileBuffers(((FileStream *)self)->file); return 0; }
static HRESULT_ WINAPI fs_revert(IStreamLike *self) { (void)self; return 0; }
static HRESULT_ WINAPI fs_lock(IStreamLike *self, ULARGE_INTEGER a, ULARGE_INTEGER b, DWORD c) { (void)self; (void)a; (void)b; (void)c; return STG_E_INVALIDFUNCTION_; }
static HRESULT_ WINAPI fs_stat(IStreamLike *self, void *out, DWORD flags) {
  FileStream *s = (FileStream *)self;
  struct { LPWSTR name; DWORD type; ULARGE_INTEGER size; FILETIME modified, created, accessed; DWORD mode, locks; BYTE clsid[16]; DWORD state, reserved; } *st = out;
  LARGE_INTEGER size;
  (void)flags;
  if (!out) return STG_E_INVALIDPOINTER_;
  memset(out, 0, sizeof(*st));
  st->type = 2;      /* STGTY_STREAM */
  if (GetFileSizeEx(s->file, &size)) st->size.QuadPart = (ULONGLONG)size.QuadPart;
  GetFileTime(s->file, &st->created, &st->accessed, &st->modified);
  st->mode = s->mode;
  return 0;
}
static HRESULT_ WINAPI fs_clone(IStreamLike *self, IStreamLike **out) { (void)self; if (out) *out = NULL; return E_NOTIMPL_; }
static const struct IStreamLikeVtbl g_file_stream_vtbl = {fs_query, fs_addref, fs_release, fs_read, fs_write, fs_seek, fs_set_size, fs_copy_to, fs_commit, fs_revert, fs_lock, fs_lock, fs_stat, fs_clone};

EXPORT HRESULT_ WINAPI SHCreateStreamOnFileEx(LPCWSTR path, DWORD mode, DWORD attributes, BOOL create, IStreamLike *template_stream, IStreamLike **out) {
  if (!out) return E_INVALIDARG_;
  *out = NULL;
  if (!path || template_stream) return E_INVALIDARG_;
  DWORD access = (mode & 3) == 0 ? 0x80000000u : ((mode & 3) == 1 ? 0x40000000u : 0xC0000000u);
  DWORD share = 0;
  switch (mode & 0x70) { case 0x40: share = 3; break; case 0x20: share = 1; break; case 0x30: share = 2; break; case 0x10: share = 0; break; default: share = 1; break; }
  DWORD disposition = (mode & 0x1000) ? 2 /* CREATE_ALWAYS */ : (create ? 4 /* OPEN_ALWAYS */ : 3 /* OPEN_EXISTING */);
  if (create && (mode & 0x1000)) disposition = 2;
  HANDLE f = CreateFileW(path, access, share, NULL, disposition, attributes ? attributes : 0x80, NULL);
  if (f == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32_(GetLastError());
  FileStream *s = task_alloc(sizeof(*s));
  if (!s) { CloseHandle(f); return E_OUTOFMEMORY_; }
  s->vtbl = &g_file_stream_vtbl;
  s->ref = 1;
  s->file = f;
  s->mode = mode;
  *out = (IStreamLike *)s;
  return 0;
}
EXPORT HRESULT_ WINAPI SHCreateStreamOnFileW(LPCWSTR path, DWORD mode, IStreamLike **out) {
  if (!out) return E_INVALIDARG_;
  if (mode & 0x1000) {}
  return SHCreateStreamOnFileEx(path, mode, 0, FALSE, NULL, out);
}
EXPORT HRESULT_ WINAPI SHCreateStreamOnFileA(LPCSTR path, DWORD mode, IStreamLike **out) {
  WCHAR *w = to_wide_dup(path);
  const HRESULT_ hr = w ? SHCreateStreamOnFileW(w, mode, out) : E_INVALIDARG_;
  task_free(w);
  return hr;
}
EXPORT HRESULT_ WINAPI IStream_Read(IStreamLike *s, void *buffer, ULONG count) {
  ULONG got = 0;
  if (!s) return E_INVALIDARG_;
  const HRESULT_ hr = s->vtbl->Read(s, buffer, count, &got);
  return hr >= 0 && got != count ? E_FAIL_ : (hr > 0 ? 0 : hr);
}
EXPORT HRESULT_ WINAPI IStream_Write(IStreamLike *s, const void *buffer, ULONG count) {
  ULONG done = 0;
  if (!s) return E_INVALIDARG_;
  const HRESULT_ hr = s->vtbl->Write(s, buffer, count, &done);
  return hr >= 0 && done != count ? E_FAIL_ : hr;
}
EXPORT HRESULT_ WINAPI IStream_Reset(IStreamLike *s) {
  LARGE_INTEGER zero;
  zero.QuadPart = 0;
  if (!s) return E_INVALIDARG_;
  return s->vtbl->Seek(s, zero, 0, NULL);
}
EXPORT HRESULT_ WINAPI IStream_Size(IStreamLike *s, ULARGE_INTEGER *size) {
  struct { LPWSTR name; DWORD type; ULARGE_INTEGER size; FILETIME m, c, a; DWORD mode, locks; BYTE clsid[16]; DWORD state, reserved; } st;
  if (!s || !size) return E_INVALIDARG_;
  const HRESULT_ hr = s->vtbl->Stat(s, &st, 1);
  if (hr >= 0) size->QuadPart = st.size.QuadPart;
  return hr;
}

/* ---- paths -------------------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT BOOL WINAPI PathFindOnPathW(LPWSTR file, LPCWSTR *dirs) {
  WCHAR found[MAX_PATH_ * 2];
  if (!file) return FALSE;
  if (dirs) {
    for (; *dirs; ++dirs) {
      WCHAR candidate[MAX_PATH_ * 2];
      size_t_ n = wcslen(*dirs);
      if (n + wcslen(file) + 2 >= sizeof(candidate) / sizeof(candidate[0])) continue;
      memcpy(candidate, *dirs, n * sizeof(WCHAR));
      if (n && candidate[n - 1] != '\\') candidate[n++] = '\\';
      memcpy(candidate + n, file, (wcslen(file) + 1) * sizeof(WCHAR));
      if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES) {
        if (wcslen(candidate) >= MAX_PATH_) return FALSE;
        memcpy(file, candidate, (wcslen(candidate) + 1) * sizeof(WCHAR));
        return TRUE;
      }
    }
    return FALSE;
  }
  if (!SearchPathW(NULL, file, L".exe", sizeof(found) / sizeof(found[0]), found, NULL) || wcslen(found) >= MAX_PATH_) return FALSE;
  memcpy(file, found, (wcslen(found) + 1) * sizeof(WCHAR));
  return TRUE;
}
EXPORT BOOL WINAPI PathFindOnPathA(LPSTR file, LPCSTR *dirs) {
  WCHAR *w = to_wide_dup(file);
  WCHAR buffer[MAX_PATH_ * 2];
  BOOL ok = FALSE;
  if (w && !dirs && wcslen(w) < MAX_PATH_) {
    memcpy(buffer, w, (wcslen(w) + 1) * sizeof(WCHAR));
    if (PathFindOnPathW(buffer, NULL)) ok = WideCharToMultiByte(CP_ACP, 0, buffer, -1, file, MAX_PATH_, NULL, NULL) != 0;
  }
  task_free(w);
  return ok;
}
EXPORT BOOL WINAPI PathIsDirectoryEmptyW(LPCWSTR path) {
  WCHAR pattern[MAX_PATH_ * 2];
  struct { DWORD attributes; FILETIME c, a, w; DWORD high, low, r0, r1; WCHAR name[MAX_PATH_]; WCHAR alt[14]; } fd;
  if (!path || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES || !(GetFileAttributesW(path) & FILE_ATTRIBUTE_DIRECTORY_)) return FALSE;
  size_t_ n = wcslen(path);
  if (n + 3 >= sizeof(pattern) / sizeof(pattern[0])) return FALSE;
  memcpy(pattern, path, n * sizeof(WCHAR));
  if (n && pattern[n - 1] != '\\') pattern[n++] = '\\';
  pattern[n++] = '*';
  pattern[n] = 0;
  HANDLE h = FindFirstFileW(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) return TRUE;
  BOOL empty = TRUE;
  do {
    if (!(fd.name[0] == '.' && (!fd.name[1] || (fd.name[1] == '.' && !fd.name[2])))) { empty = FALSE; break; }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return empty;
}
EXPORT BOOL WINAPI PathIsDirectoryEmptyA(LPCSTR path) {
  WCHAR *w = to_wide_dup(path);
  const BOOL r = w && PathIsDirectoryEmptyW(w);
  task_free(w);
  return r;
}
EXPORT BOOL WINAPI PathIsNetworkPathW(LPCWSTR path) {
  if (!path) return FALSE;
  if (path[0] == '\\' && path[1] == '\\') return TRUE;
  if (((path[0] | 0x20) >= 'a' && (path[0] | 0x20) <= 'z') && path[1] == ':') { WCHAR root[4] = {path[0], ':', '\\', 0}; return GetDriveTypeW(root) == 4; }
  return FALSE;
}
EXPORT BOOL WINAPI PathIsNetworkPathA(LPCSTR path) {
  WCHAR *w = to_wide_dup(path);
  const BOOL r = w && PathIsNetworkPathW(w);
  task_free(w);
  return r;
}
EXPORT BOOL WINAPI PathIsURLW(LPCWSTR path) {
  if (!path) return FALSE;
  size_t_ i = 0;
  while (path[i] && ((path[i] | 0x20) >= 'a' && (path[i] | 0x20) <= 'z')) ++i;
  return i >= 2 && path[i] == ':' && path[i + 1] == '/' && path[i + 2] == '/';
}
EXPORT BOOL WINAPI PathIsURLA(LPCSTR path) {
  WCHAR *w = to_wide_dup(path);
  const BOOL r = w && PathIsURLW(w);
  task_free(w);
  return r;
}

/* ---- file associations (AssocQueryString) ------------------------------------------------------------------------------------------------------------------------- */

#define ASSOCSTR_COMMAND_ 1
#define ASSOCSTR_EXECUTABLE_ 2
#define ASSOCSTR_FRIENDLYDOCNAME_ 3
#define ASSOCSTR_FRIENDLYAPPNAME_ 4
#define ASSOCSTR_NOOPEN_ 5
#define ASSOCSTR_DEFAULTICON_ 15
#define ASSOCF_NOTRUNCATE_ 0x20
#define E_ASSOC_NOT_FOUND_ ((LONG)0x80070483)      /* HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION) */

static BOOL assoc_value(const WCHAR *key, const WCHAR *value, WCHAR *out, DWORD capacity) {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT_, key, 0, KEY_READ_, &k)) return FALSE;
  WCHAR raw[MAX_PATH_ * 2];
  DWORD size = sizeof(raw) - 2, type = 0;
  const LSTATUS_ r = RegQueryValueExW(k, value, NULL, &type, (LPBYTE)raw, &size);
  RegCloseKey(k);
  if (r || (type != REG_SZ_ && type != REG_EXPAND_SZ_)) return FALSE;
  raw[size / 2] = 0;
  if (type == REG_EXPAND_SZ_) { const DWORD n = ExpandEnvironmentStringsW(raw, out, capacity); return n && n <= capacity && out[0]; }
  if (wcslen(raw) >= capacity) return FALSE;
  memcpy(out, raw, (wcslen(raw) + 1) * sizeof(WCHAR));
  return out[0] != 0;
}
EXPORT HRESULT_ WINAPI AssocQueryStringW(DWORD flags, DWORD what, LPCWSTR assoc, LPCWSTR extra, LPWSTR out, DWORD *size) {
  WCHAR class_name[MAX_PATH_], key[MAX_PATH_ * 2], value[MAX_PATH_ * 2];
  (void)flags;
  if (!assoc || !size || (*size && !out && *size)) return E_INVALIDARG_;
  /* the class: the extension's, or the name given */
  if (assoc[0] == '.') { if (!assoc_value(assoc, NULL, class_name, MAX_PATH_)) { memcpy(class_name, assoc, (wcslen(assoc) + 1) * sizeof(WCHAR)); } }
  else { if (wcslen(assoc) >= MAX_PATH_) return E_INVALIDARG_; memcpy(class_name, assoc, (wcslen(assoc) + 1) * sizeof(WCHAR)); }
  const WCHAR *verb = extra && *extra ? extra : L"open";
  value[0] = 0;
  BOOL found = FALSE;
  switch (what) {
    case ASSOCSTR_COMMAND_: case ASSOCSTR_EXECUTABLE_:
      swprintf_(key, class_name, L"\\shell\\", verb, L"\\command");
      found = assoc_value(key, NULL, value, MAX_PATH_ * 2);
      if (found && what == ASSOCSTR_EXECUTABLE_) {
        /* the program: the first word of the command, quotes taken off */
        WCHAR *p = value, *start = value;
        if (*p == '"') { start = ++p; while (*p && *p != '"') ++p; *p = 0; }
        else { while (*p && *p != ' ') ++p; *p = 0; }
        memmove(value, start, (wcslen(start) + 1) * sizeof(WCHAR));
      }
      break;
    case ASSOCSTR_FRIENDLYDOCNAME_: found = assoc_value(class_name, NULL, value, MAX_PATH_ * 2); break;
    case ASSOCSTR_FRIENDLYAPPNAME_: {
      WCHAR program[MAX_PATH_ * 2];
      swprintf_(key, class_name, L"\\shell\\", verb, L"\\command");
      if (assoc_value(key, NULL, program, MAX_PATH_ * 2)) {
        WCHAR *p = program, *start = program;
        if (*p == '"') { start = ++p; while (*p && *p != '"') ++p; *p = 0; } else { while (*p && *p != ' ') ++p; *p = 0; }
        const WCHAR *name = start, *s = start;
        for (; *s; ++s) if (*s == '\\') name = s + 1;
        size_t_ n = wcslen(name);
        if (n > 4 && name[n - 4] == '.') n -= 4;
        memcpy(value, name, n * sizeof(WCHAR));
        value[n] = 0;
        found = TRUE;
      }
      break;
    }
    case ASSOCSTR_DEFAULTICON_: swprintf_(key, class_name, L"\\DefaultIcon", L"", L""); found = assoc_value(key, NULL, value, MAX_PATH_ * 2); break;
    default: return E_INVALIDARG_;
  }
  if (!found) return E_ASSOC_NOT_FOUND_;
  const DWORD need = (DWORD)wcslen(value) + 1;
  if (!out || *size < need) { *size = need; return out ? E_POINTER_ : 1; }
  memcpy(out, value, need * sizeof(WCHAR));
  *size = need;
  return 0;
}
EXPORT HRESULT_ WINAPI AssocQueryStringA(DWORD flags, DWORD what, LPCSTR assoc, LPCSTR extra, LPSTR out, DWORD *size) {
  WCHAR *a = to_wide_dup(assoc), *e = to_wide_dup(extra);
  WCHAR wide[MAX_PATH_ * 2];
  HRESULT_ hr = E_INVALIDARG_;
  if (a && size) {
    DWORD n = sizeof(wide) / sizeof(wide[0]);
    hr = AssocQueryStringW(flags, what, a, e, wide, &n);
    if (hr == 0) {
      const int bytes = WideCharToMultiByte(CP_ACP, 0, wide, -1, NULL, 0, NULL, NULL);
      if (!out || *size < (DWORD)bytes) { *size = (DWORD)bytes; hr = out ? E_POINTER_ : 1; }
      else { WideCharToMultiByte(CP_ACP, 0, wide, -1, out, bytes, NULL, NULL); *size = (DWORD)bytes; }
    }
  }
  task_free(a); task_free(e);
  return hr;
}
