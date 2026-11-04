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

/* shcore.dll for TheoryCraftOS: the "shell core" that shell32.dll and shlwapi.dll share on Windows 8 and later. The parts:
 *   DPI           GetDpiForMonitor, Get/SetProcessDpiAwareness, the scale factors: over the win32k services NtUserGetDpiForMonitor, NtUserGetSystemDpiForProcess and
 *                 NtUserGet/SetProcessDpiAwarenessContext (src/Process/Win32k/Win32kUserMiscMonitor.cpp). The scale-change events are signalled by a watcher thread that
 *                 polls the system DPI (the kernel has no change notification for it) while somebody is registered.
 *   strings       SHAnsiToUnicode and friends (the truncating copy functions of shlwapi)
 *   registry      SHRegGetValue, SHEnumKeyEx, SHCopyKey, SHRegGetPath ... and the registry streams (SHOpenRegStream*)
 *   streams       SHCreateMemStream, IStream_Copy / ReadStr / WriteStr. SHCreateStreamOnFile*, IStream_Read/Write/Size/Reset, SHGetValue and the other registry helpers that
 *                 shlwapi.dll has are forwarded to it (shcore.def); CommandLineToArgvW and the AppUserModelID functions to shell32.dll.
 *   COM helpers   IUnknown_Set / GetSite / SetSite / QueryService / AtomicRelease, the process reference (SetProcessReference ...), thread references and SHCreateThread
 *   process       IsOS, the explicit AppUserModelID (kept in the process parameters, as Windows does), feature-state queries (nothing is overridden: the defaults)
 * Not here: the WinRT stream functions (CreateRandomAccessStreamOnFile ... need HSTRING and the IRandomAccessStream interfaces: they report E_NOTIMPL) and the shell task
 * pool (SHTaskPool*, an undocumented interface): those stay stubs (src/Windows/stubs/shcore.inc). Behaviour checked with Wine's tests (tests/windows/wine/shcore).
 * Built freestanding over kernel32.dll with the OS's mingw compiler, against mingw-w64's headers. */
#define _SHLWAPI_
#define COBJMACROS
#define INITGUID
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winternl.h>
#include <objbase.h>
#include <ocidl.h>
#include <servprov.h>

#define EXPORT __declspec(dllexport)

/* (the generic names are macros for the W functions; regvalue.inc spells out both) */
#undef RegQueryValueEx
#undef RegOpenKeyEx
#undef ExpandEnvironmentStrings

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

/* The services of win32k (win32u.dll). Every parameter and result is one register. */
__declspec(dllimport) ULONG_PTR NtUserGetDpiForMonitor(ULONG_PTR monitor, ULONG_PTR type, ULONG_PTR x, ULONG_PTR y);
__declspec(dllimport) ULONG_PTR NtUserGetSystemDpiForProcess(ULONG_PTR process);
__declspec(dllimport) ULONG_PTR NtUserGetProcessDpiAwarenessContext(ULONG_PTR process);
__declspec(dllimport) ULONG_PTR NtUserSetProcessDpiAwarenessContext(ULONG_PTR context, ULONG_PTR reserved);
__declspec(dllimport) ULONG_PTR NtUserPostMessage(ULONG_PTR window, ULONG_PTR message, ULONG_PTR wparam, ULONG_PTR lparam);

#define LEN_OF(a) (sizeof(a) / sizeof((a)[0]))
#define CURRENT_PROCESS ((ULONG_PTR)-1)

/* ---- helpers ----------------------------------------------------------------------------------------------------------------------------------------------- */

static void *mem_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void mem_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static void mem_copy(void *d, const void *s, SIZE_T n) { BYTE *dd = d; const BYTE *ss = s; if (dd < ss) while (n--) *dd++ = *ss++; else { dd += n; ss += n; while (n--) *--dd = *--ss; } }
static void mem_zero(void *d, SIZE_T n) { BYTE *dd = d; while (n--) *dd++ = 0; }
static SIZE_T wlen(const WCHAR *s) { SIZE_T n = 0; while (s[n]) ++n; return n; }
static SIZE_T alen(const char *s) { SIZE_T n = 0; while (s[n]) ++n; return n; }

/* The wide copy of an ANSI string (heap, NULL for NULL or when out of memory). */
static WCHAR *to_wide(const char *s) {
  if (!s) return NULL;
  const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
  WCHAR *w = mem_alloc((SIZE_T)(n > 0 ? n : 1) * sizeof(WCHAR));
  if (w && n > 0) MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
  return w;
}

/* ---- DPI ----------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef enum { DPI_UNAWARE_ = 0, DPI_SYSTEM_ = 1, DPI_PER_MONITOR_ = 2 } DpiAwareness;

/* The awareness contexts that win32k stores: the awareness in the low nibble, the version of the model above it (src/Process/Win32k/Win32kUserMiscMonitor.cpp). */
#define CONTEXT_UNAWARE 0x10u
#define CONTEXT_SYSTEM 0x11u
#define CONTEXT_PER_MONITOR 0x12u

static HRESULT dpi_of_monitor(HMONITOR monitor, UINT *dpi) {
  UINT x = 0, y = 0;
  if (!monitor) return E_INVALIDARG;
  SetLastError(0);
  if (!NtUserGetDpiForMonitor((ULONG_PTR)monitor, 0, (ULONG_PTR)&x, (ULONG_PTR)&y)) return E_INVALIDARG;
  *dpi = x;
  return S_OK;
}

EXPORT HRESULT WINAPI GetDpiForMonitor(HMONITOR monitor, int type, UINT *x, UINT *y) {
  UINT dx = 0, dy = 0;
  if (!x || !y || (UINT)type > 2) return E_INVALIDARG;
  if (!monitor) return E_INVALIDARG;
  if (!NtUserGetDpiForMonitor((ULONG_PTR)monitor, (ULONG_PTR)type, (ULONG_PTR)&dx, (ULONG_PTR)&dy)) return E_INVALIDARG;
  *x = dx;
  *y = dy;
  return S_OK;
}

EXPORT HRESULT WINAPI GetProcessDpiAwareness(HANDLE process, DpiAwareness *awareness) {
  if (!awareness) return E_INVALIDARG;
  if (process && process != GetCurrentProcess() && GetProcessId(process) != GetCurrentProcessId()) return E_ACCESSDENIED;
  const ULONG_PTR context = NtUserGetProcessDpiAwarenessContext(CURRENT_PROCESS);
  const unsigned level = (unsigned)(context & 0xF);
  *awareness = level == 2 ? DPI_PER_MONITOR_ : level == 1 ? DPI_SYSTEM_ : DPI_UNAWARE_;
  return S_OK;
}

EXPORT HRESULT WINAPI SetProcessDpiAwareness(DpiAwareness awareness) {
  if ((unsigned)awareness > 2) return E_INVALIDARG;
  const ULONG_PTR context = awareness == DPI_PER_MONITOR_ ? CONTEXT_PER_MONITOR : awareness == DPI_SYSTEM_ ? CONTEXT_SYSTEM : CONTEXT_UNAWARE;
  SetLastError(0);
  if (NtUserSetProcessDpiAwarenessContext(context, 0)) return S_OK;
  return GetLastError() == ERROR_INVALID_PARAMETER ? E_INVALIDARG : E_ACCESSDENIED;      /* the awareness of a process is set once */
}

/* DEVICE_SCALE_FACTOR: the percentages of the display scale settings. */
static const int g_scale_factors[] = {100, 120, 125, 140, 150, 160, 175, 180, 200, 225, 250, 300, 350, 400, 450, 500};
static int scale_for_dpi(UINT dpi) {
  const unsigned percent = (dpi * 100u + 48u) / 96u;      /* rounded */
  int best = g_scale_factors[0];
  unsigned best_distance = 0xFFFFFFFFu;
  for (SIZE_T i = 0; i < LEN_OF(g_scale_factors); ++i) {
    const unsigned d = percent > (unsigned)g_scale_factors[i] ? percent - (unsigned)g_scale_factors[i] : (unsigned)g_scale_factors[i] - percent;
    if (d < best_distance) { best_distance = d; best = g_scale_factors[i]; }
  }
  return best;
}

EXPORT HRESULT WINAPI GetScaleFactorForMonitor(HMONITOR monitor, int *scale) {
  UINT dpi = 0;
  if (!scale) return E_INVALIDARG;
  const HRESULT hr = dpi_of_monitor(monitor, &dpi);
  if (FAILED(hr)) return hr;
  *scale = scale_for_dpi(dpi);
  return S_OK;
}

EXPORT int WINAPI GetScaleFactorForDevice(int device) {
  if (device != 0) return 100;      /* DEVICE_PRIMARY is the only one with a scale */
  return scale_for_dpi((UINT)NtUserGetSystemDpiForProcess(CURRENT_PROCESS));
}

EXPORT UINT WINAPI GetDpiForShellUIComponent(int component) {
  if ((unsigned)component > 2) return 0;      /* SHELL_UI_COMPONENT_TASKBARS, NOTIFICATIONAREA, DESKBAND */
  return (UINT)NtUserGetSystemDpiForProcess(CURRENT_PROCESS);
}

/* The scale-change registrations: an event to set or a window to post a message to, when the system DPI changes. */
#define MAX_SCALE_LISTENERS 32
typedef struct { DWORD cookie; HANDLE event; HWND window; UINT message; } ScaleListener;
static ScaleListener g_listeners[MAX_SCALE_LISTENERS];
static CRITICAL_SECTION g_listener_lock;
static LONG g_listener_lock_state;      /* 0 = not initialised, 1 = initialising, 2 = ready */
static DWORD g_next_cookie = 1;
static HANDLE g_watcher;
static UINT g_watched_dpi;

static void listener_lock(void) {
  if (InterlockedCompareExchange(&g_listener_lock_state, 1, 0) == 0) {
    InitializeCriticalSection(&g_listener_lock);
    InterlockedExchange(&g_listener_lock_state, 2);
  } else {
    while (g_listener_lock_state != 2) Sleep(0);
  }
  EnterCriticalSection(&g_listener_lock);
}
static void listener_unlock(void) { LeaveCriticalSection(&g_listener_lock); }

static BOOL any_listener(void) {
  for (SIZE_T i = 0; i < MAX_SCALE_LISTENERS; ++i) if (g_listeners[i].cookie) return TRUE;
  return FALSE;
}

static DWORD WINAPI scale_watcher(void *unused) {
  (void)unused;
  for (;;) {
    Sleep(250);
    const UINT dpi = (UINT)NtUserGetSystemDpiForProcess(CURRENT_PROCESS);
    listener_lock();
    if (!any_listener()) { g_watcher = NULL; listener_unlock(); return 0; }
    if (dpi != g_watched_dpi) {
      g_watched_dpi = dpi;
      for (SIZE_T i = 0; i < MAX_SCALE_LISTENERS; ++i) {
        if (!g_listeners[i].cookie) continue;
        if (g_listeners[i].event) SetEvent(g_listeners[i].event);
        if (g_listeners[i].window) NtUserPostMessage((ULONG_PTR)g_listeners[i].window, g_listeners[i].message, (ULONG_PTR)scale_for_dpi(dpi), 0);
      }
    }
    listener_unlock();
  }
}

static HRESULT add_listener(HANDLE event, HWND window, UINT message, DWORD *cookie) {
  HRESULT hr = E_OUTOFMEMORY;
  listener_lock();
  for (SIZE_T i = 0; i < MAX_SCALE_LISTENERS; ++i) {
    if (g_listeners[i].cookie) continue;
    g_listeners[i].cookie = g_next_cookie++;
    if (!g_next_cookie) g_next_cookie = 1;
    g_listeners[i].event = event;
    g_listeners[i].window = window;
    g_listeners[i].message = message;
    *cookie = g_listeners[i].cookie;
    hr = S_OK;
    if (!g_watcher) {
      g_watched_dpi = (UINT)NtUserGetSystemDpiForProcess(CURRENT_PROCESS);
      g_watcher = CreateThread(NULL, 0, scale_watcher, NULL, 0, NULL);
      if (!g_watcher) { g_listeners[i].cookie = 0; hr = E_OUTOFMEMORY; } else CloseHandle(g_watcher);
    }
    break;
  }
  listener_unlock();
  return hr;
}
static HRESULT remove_listener(DWORD cookie, BOOL want_event) {
  HRESULT hr = E_INVALIDARG;
  listener_lock();
  for (SIZE_T i = 0; cookie && i < MAX_SCALE_LISTENERS; ++i) {
    if (g_listeners[i].cookie != cookie || (want_event ? !g_listeners[i].event : !g_listeners[i].window)) continue;
    mem_zero(&g_listeners[i], sizeof(g_listeners[i]));
    hr = S_OK;
    break;
  }
  listener_unlock();
  return hr;
}

EXPORT HRESULT WINAPI RegisterScaleChangeEvent(HANDLE event, DWORD_PTR *cookie) {
  DWORD c = 0;
  if (!event || !cookie) return E_INVALIDARG;
  const HRESULT hr = add_listener(event, NULL, 0, &c);
  *cookie = SUCCEEDED(hr) ? c : 0;
  return hr;
}
EXPORT HRESULT WINAPI UnregisterScaleChangeEvent(DWORD_PTR cookie) { return remove_listener((DWORD)cookie, TRUE); }
EXPORT HRESULT WINAPI RegisterScaleChangeNotifications(int device, HWND window, UINT message, DWORD *cookie) {
  if (device != 0 || !window || !cookie) return E_INVALIDARG;
  return add_listener(NULL, window, message, cookie);
}
EXPORT HRESULT WINAPI RevokeScaleChangeNotifications(int device, DWORD cookie) { return device != 0 ? E_INVALIDARG : remove_listener(cookie, FALSE); }

/* ---- strings ------------------------------------------------------------------------------------------------------------------------------------------------- */

/* The conversions truncate: they return the characters written including the terminator, and for a destination too small they cut the text and terminate it. */
EXPORT int WINAPI SHUnicodeToAnsi(const WCHAR *source, char *dest, int capacity) {
  if (capacity <= 0 || !dest) return 0;
  if (!source) { dest[0] = 0; return 1; }
  int n = WideCharToMultiByte(CP_ACP, 0, source, -1, dest, capacity, NULL, NULL);
  if (n > 0) return n;
  const int cut = capacity - 1;
  n = cut > 0 ? WideCharToMultiByte(CP_ACP, 0, source, cut, dest, cut, NULL, NULL) : 0;
  if (cut > 0 && n == 0) n = 0;
  dest[n] = 0;
  return n + 1;
}
EXPORT int WINAPI SHAnsiToUnicode(const char *source, WCHAR *dest, int capacity) {
  if (capacity <= 0 || !dest) return 0;
  if (!source) { dest[0] = 0; return 1; }
  int n = MultiByteToWideChar(CP_ACP, 0, source, -1, dest, capacity);
  if (n > 0) return n;
  const int cut = capacity - 1;
  n = cut > 0 ? MultiByteToWideChar(CP_ACP, 0, source, cut, dest, cut) : 0;
  dest[n] = 0;
  return n + 1;
}
/* A string copy that reports 0 when it had to truncate. */
EXPORT int WINAPI SHAnsiToAnsi(const char *source, char *dest, int capacity) {
  if (capacity <= 0 || !dest) return 0;
  if (!source) { dest[0] = 0; return 1; }
  const SIZE_T n = alen(source);
  if (n + 1 <= (SIZE_T)capacity) { mem_copy(dest, source, n + 1); return (int)n + 1; }
  mem_copy(dest, source, (SIZE_T)capacity - 1);
  dest[capacity - 1] = 0;
  return 0;
}
EXPORT int WINAPI SHUnicodeToUnicode(const WCHAR *source, WCHAR *dest, int capacity) {
  if (capacity <= 0 || !dest) return 0;
  if (!source) { dest[0] = 0; return 1; }
  const SIZE_T n = wlen(source);
  if (n + 1 <= (SIZE_T)capacity) { mem_copy(dest, source, (n + 1) * sizeof(WCHAR)); return (int)n + 1; }
  mem_copy(dest, source, ((SIZE_T)capacity - 1) * sizeof(WCHAR));
  dest[capacity - 1] = 0;
  return 0;
}

/* ---- registry ------------------------------------------------------------------------------------------------------------------------------------------------ */

EXPORT HKEY WINAPI SHRegDuplicateHKey(HKEY key) {
  HKEY copy = NULL;
  if (RegOpenKeyExW(key, NULL, 0, MAXIMUM_ALLOWED, &copy) != ERROR_SUCCESS) return NULL;
  return copy;
}

/* SHQueryValueEx, SHGetValue, SHRegGetValue and SHRegGetPath, for A and W (regvalue.inc). */
#define SUF A
#define CH char
#include "regvalue.inc"
#undef SUF
#undef CH
#define SUF W
#define CH WCHAR
#include "regvalue.inc"
#undef SUF
#undef CH
/* HKCU first, then HKLM. */
EXPORT LSTATUS WINAPI SHRegGetValueFromHKCUHKLM(LPCWSTR subkey, LPCWSTR value, DWORD flags, DWORD *type, void *data, DWORD *size) {
  DWORD copy = size ? *size : 0;
  LSTATUS r = RegGetValueW(HKEY_CURRENT_USER, subkey, value, flags, type, data, size ? &copy : NULL);
  if (r == ERROR_SUCCESS || r == ERROR_MORE_DATA) { if (size) *size = copy; return r; }
  return RegGetValueW(HKEY_LOCAL_MACHINE, subkey, value, flags, type, data, size);
}
EXPORT int WINAPI SHRegGetIntW(HKEY key, LPCWSTR value, int default_value) {
  WCHAR text[32];
  DWORD size = sizeof(text), type = 0;
  mem_zero(text, sizeof(text));
  if (RegQueryValueExW(key, value, NULL, &type, (BYTE *)text, &size) != ERROR_SUCCESS || size == 0) return default_value;
  if (type == REG_DWORD || type == REG_BINARY) { DWORD v = 0; mem_copy(&v, text, size < 4 ? size : 4); return (int)v; }
  if (type != REG_SZ) return default_value;
  text[LEN_OF(text) - 1] = 0;
  int sign = 1, v = 0;
  const WCHAR *p = text;
  if (*p == '-') { sign = -1; ++p; }
  if (*p < '0' || *p > '9') return default_value;
  while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
  return sign * v;
}

static LSTATUS set_path(HKEY key, LPCWSTR value, LPCWSTR path, DWORD type) {
  return RegSetValueExW(key, value, 0, type, (const BYTE *)path, (DWORD)((wlen(path) + 1) * sizeof(WCHAR)));
}
EXPORT LSTATUS WINAPI SHRegSetPathW(HKEY root, LPCWSTR subkey, LPCWSTR value, LPCWSTR path, DWORD flags) {
  HKEY key;
  (void)flags;
  LSTATUS r = RegCreateKeyExW(root, subkey, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
  if (r != ERROR_SUCCESS) return r;
  /* a path under a well-known folder would be stored with an environment variable in Windows; the path is stored as given */
  r = set_path(key, value, path, REG_SZ);
  RegCloseKey(key);
  return r;
}
EXPORT LSTATUS WINAPI SHRegSetPathA(HKEY root, LPCSTR subkey, LPCSTR value, LPCSTR path, DWORD flags) {
  WCHAR *k = to_wide(subkey), *v = to_wide(value), *p = to_wide(path);
  LSTATUS r = ERROR_OUTOFMEMORY;
  if ((!subkey || k) && (!value || v) && p) r = SHRegSetPathW(root, k, v, p, flags);
  mem_free(k); mem_free(v); mem_free(p);
  return r;
}

EXPORT LSTATUS WINAPI SHDeleteEmptyKeyA(HKEY root, LPCSTR subkey) {
  HKEY key;
  DWORD subkeys = 0, values = 0;
  LSTATUS r = RegOpenKeyExA(root, subkey, 0, KEY_READ | KEY_WRITE, &key);
  if (r != ERROR_SUCCESS) return r;
  r = RegQueryInfoKeyW(key, NULL, NULL, NULL, &subkeys, NULL, NULL, &values, NULL, NULL, NULL, NULL);
  RegCloseKey(key);
  if (r != ERROR_SUCCESS) return r;
  if (subkeys || values) return ERROR_KEY_HAS_CHILDREN;
  return RegDeleteKeyA(root, subkey);
}
EXPORT LSTATUS WINAPI SHDeleteValueA(HKEY root, LPCSTR subkey, LPCSTR value) {
  HKEY key;
  LSTATUS r = RegOpenKeyExA(root, subkey, 0, KEY_SET_VALUE, &key);
  if (r != ERROR_SUCCESS) return r;
  r = RegDeleteValueA(key, value);
  RegCloseKey(key);
  return r;
}

EXPORT LSTATUS WINAPI SHEnumKeyExW(HKEY key, DWORD index, LPWSTR name, DWORD *size) { return RegEnumKeyExW(key, index, name, size, NULL, NULL, NULL, NULL); }
EXPORT LSTATUS WINAPI SHEnumKeyExA(HKEY key, DWORD index, LPSTR name, DWORD *size) { return RegEnumKeyExA(key, index, name, size, NULL, NULL, NULL, NULL); }
EXPORT LSTATUS WINAPI SHEnumValueW(HKEY key, DWORD index, LPWSTR name, DWORD *name_size, DWORD *type, void *data, DWORD *data_size) {
  return RegEnumValueW(key, index, name, name_size, NULL, type, data, data_size);
}
EXPORT LSTATUS WINAPI SHEnumValueA(HKEY key, DWORD index, LPSTR name, DWORD *name_size, DWORD *type, void *data, DWORD *data_size) {
  return RegEnumValueA(key, index, name, name_size, NULL, type, data, data_size);
}
EXPORT LSTATUS WINAPI SHQueryInfoKeyW(HKEY key, DWORD *subkeys, DWORD *max_subkey, DWORD *values, DWORD *max_value) {
  return RegQueryInfoKeyW(key, NULL, NULL, NULL, subkeys, max_subkey, NULL, values, max_value, NULL, NULL, NULL);
}
EXPORT LSTATUS WINAPI SHQueryInfoKeyA(HKEY key, DWORD *subkeys, DWORD *max_subkey, DWORD *values, DWORD *max_value) {
  return RegQueryInfoKeyA(key, NULL, NULL, NULL, subkeys, max_subkey, NULL, values, max_value, NULL, NULL, NULL);
}

/* Copies the values and (recursively) the subkeys of `source` (or of its subkey) into `dest`. */
static LSTATUS copy_tree(HKEY source, HKEY dest, int depth) {
  DWORD subkeys = 0, values = 0, max_name = 0, max_value_name = 0, max_data = 0;
  LSTATUS r = RegQueryInfoKeyW(source, NULL, NULL, NULL, &subkeys, &max_name, NULL, &values, &max_value_name, &max_data, NULL, NULL);
  if (r != ERROR_SUCCESS) return r;
  if (depth > 32) return ERROR_BAD_PATHNAME;
  WCHAR *name = mem_alloc(((SIZE_T)(max_name > max_value_name ? max_name : max_value_name) + 2) * sizeof(WCHAR));
  BYTE *data = mem_alloc((SIZE_T)max_data + 2);
  if (!name || !data) { mem_free(name); mem_free(data); return ERROR_NOT_ENOUGH_MEMORY; }
  for (DWORD i = 0; i < values && r == ERROR_SUCCESS; ++i) {
    DWORD name_size = max_value_name + 1, data_size = max_data, type = 0;
    r = RegEnumValueW(source, i, name, &name_size, NULL, &type, data, &data_size);
    if (r == ERROR_SUCCESS) r = RegSetValueExW(dest, name, 0, type, data, data_size);
  }
  /* (the subkeys are enumerated from the end: copying does not change the source, but a copy into a subkey of the source would) */
  for (DWORD i = 0; i < subkeys && r == ERROR_SUCCESS; ++i) {
    DWORD name_size = max_name + 1;
    HKEY from, to;
    r = RegEnumKeyExW(source, i, name, &name_size, NULL, NULL, NULL, NULL);
    if (r != ERROR_SUCCESS) break;
    r = RegOpenKeyExW(source, name, 0, KEY_READ, &from);
    if (r != ERROR_SUCCESS) break;
    r = RegCreateKeyExW(dest, name, 0, NULL, 0, KEY_WRITE, NULL, &to, NULL);
    if (r == ERROR_SUCCESS) { r = copy_tree(from, to, depth + 1); RegCloseKey(to); }
    RegCloseKey(from);
  }
  mem_free(name);
  mem_free(data);
  return r;
}
EXPORT LSTATUS WINAPI SHCopyKeyW(HKEY source, LPCWSTR subkey, HKEY dest, DWORD reserved) {
  HKEY from = source;
  (void)reserved;
  if (subkey && *subkey) {
    LSTATUS r = RegOpenKeyExW(source, subkey, 0, KEY_READ, &from);
    if (r != ERROR_SUCCESS) return r;
  }
  const LSTATUS r = copy_tree(from, dest, 0);
  if (from != source) RegCloseKey(from);
  return r;
}
EXPORT LSTATUS WINAPI SHCopyKeyA(HKEY source, LPCSTR subkey, HKEY dest, DWORD reserved) {
  WCHAR *w = to_wide(subkey);
  if (subkey && !w) return ERROR_NOT_ENOUGH_MEMORY;
  const LSTATUS r = SHCopyKeyW(source, w, dest, reserved);
  mem_free(w);
  return r;
}

/* ---- memory streams --------------------------------------------------------------------------------------------------------------------------------------- */

#define STG_E_INVALIDFUNCTION_ ((HRESULT)0x80030001)
#define STG_E_INVALIDPOINTER_ ((HRESULT)0x80030009)
#define STG_E_MEDIUMFULL_ ((HRESULT)0x80030070)
#define STG_E_ACCESSDENIED_ ((HRESULT)0x80030005)
#define MAX_STREAM_SIZE (0x7FFFFFFFull)

typedef struct {
  IStream iface;
  LONG ref;
  BYTE *data;
  SIZE_T size, capacity;
  ULONGLONG pos;
  HKEY key;                 /* a registry stream: the value to write back when the last reference goes */
  WCHAR *value;
  BOOL writable, dirty;
} MemStream;

static MemStream *mem_stream_of(IStream *s) { return (MemStream *)s; }

static BOOL stream_reserve(MemStream *s, SIZE_T need) {
  if (need <= s->capacity) return TRUE;
  SIZE_T cap = s->capacity ? s->capacity : 64;
  while (cap < need) { if (cap > MAX_STREAM_SIZE / 2) { cap = need; break; } cap *= 2; }
  BYTE *bigger = mem_alloc(cap);
  if (!bigger) return FALSE;
  if (s->size) mem_copy(bigger, s->data, s->size);
  mem_free(s->data);
  s->data = bigger;
  s->capacity = cap;
  return TRUE;
}

static BOOL is_iid(REFIID a, REFIID b) { return IsEqualGUID(a, b); }

static HRESULT STDMETHODCALLTYPE ms_query(IStream *self, REFIID iid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (is_iid(iid, &IID_IUnknown) || is_iid(iid, &IID_IStream) || is_iid(iid, &IID_ISequentialStream)) {
    *out = self;
    InterlockedIncrement(&mem_stream_of(self)->ref);
    return S_OK;
  }
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ms_addref(IStream *self) { return (ULONG)InterlockedIncrement(&mem_stream_of(self)->ref); }

static HRESULT stream_save(MemStream *s) {
  if (!s->key || !s->writable || !s->dirty) return S_OK;
  const LSTATUS r = RegSetValueExW(s->key, s->value, 0, REG_BINARY, s->data, (DWORD)s->size);
  if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
  s->dirty = FALSE;
  return S_OK;
}
static ULONG STDMETHODCALLTYPE ms_release(IStream *self) {
  MemStream *s = mem_stream_of(self);
  const LONG n = InterlockedDecrement(&s->ref);
  if (n == 0) {
    stream_save(s);
    if (s->key) RegCloseKey(s->key);
    mem_free(s->value);
    mem_free(s->data);
    mem_free(s);
  }
  return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE ms_read(IStream *self, void *buffer, ULONG count, ULONG *read) {
  MemStream *s = mem_stream_of(self);
  ULONG n = 0;
  if (!buffer) return STG_E_INVALIDPOINTER_;
  if (s->pos < s->size) {
    const ULONGLONG left = s->size - s->pos;
    n = left < count ? (ULONG)left : count;
    mem_copy(buffer, s->data + s->pos, n);
    s->pos += n;
  }
  if (read) *read = n;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE ms_write(IStream *self, const void *buffer, ULONG count, ULONG *written) {
  MemStream *s = mem_stream_of(self);
  if (written) *written = 0;
  if (!buffer) return STG_E_INVALIDPOINTER_;
  if (s->key && !s->writable) return STG_E_ACCESSDENIED_;
  if (s->pos > MAX_STREAM_SIZE || count > MAX_STREAM_SIZE - s->pos) return STG_E_MEDIUMFULL_;
  const SIZE_T end = (SIZE_T)s->pos + count;
  if (!stream_reserve(s, end)) return E_OUTOFMEMORY;
  if (s->pos > s->size) mem_zero(s->data + s->size, (SIZE_T)s->pos - s->size);      /* (a gap left by a seek past the end) */
  mem_copy(s->data + s->pos, buffer, count);
  s->pos = end;
  if (end > s->size) s->size = end;
  s->dirty = TRUE;
  if (written) *written = count;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE ms_seek(IStream *self, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *out) {
  MemStream *s = mem_stream_of(self);
  LONGLONG base;
  if (origin == STREAM_SEEK_SET) base = 0;
  else if (origin == STREAM_SEEK_CUR) base = (LONGLONG)s->pos;
  else if (origin == STREAM_SEEK_END) base = (LONGLONG)s->size;
  else return STG_E_INVALIDFUNCTION_;
  if (move.QuadPart > (LONGLONG)MAX_STREAM_SIZE || move.QuadPart < -(LONGLONG)MAX_STREAM_SIZE) return STG_E_INVALIDFUNCTION_;
  const LONGLONG target = base + move.QuadPart;
  if (target < 0 || target > (LONGLONG)MAX_STREAM_SIZE) return STG_E_INVALIDFUNCTION_;
  s->pos = (ULONGLONG)target;
  if (out) out->QuadPart = s->pos;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE ms_set_size(IStream *self, ULARGE_INTEGER size) {
  MemStream *s = mem_stream_of(self);
  if (s->key && !s->writable) return STG_E_ACCESSDENIED_;
  if (size.QuadPart > MAX_STREAM_SIZE) return STG_E_MEDIUMFULL_;
  if (!stream_reserve(s, (SIZE_T)size.QuadPart)) return E_OUTOFMEMORY;
  if (size.QuadPart > s->size) mem_zero(s->data + s->size, (SIZE_T)size.QuadPart - s->size);
  s->size = (SIZE_T)size.QuadPart;
  s->dirty = TRUE;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE ms_copy_to(IStream *self, IStream *dest, ULARGE_INTEGER count, ULARGE_INTEGER *read, ULARGE_INTEGER *written) {
  MemStream *s = mem_stream_of(self);
  ULONGLONG total_read = 0, total_written = 0;
  HRESULT hr = S_OK;
  if (!dest) return STG_E_INVALIDPOINTER_;
  if (s->pos < s->size) {
    ULONGLONG n = s->size - s->pos;
    if (n > count.QuadPart) n = count.QuadPart;
    while (n && SUCCEEDED(hr)) {
      const ULONG chunk = n > 0x10000000u ? 0x10000000u : (ULONG)n;
      ULONG put = 0;
      hr = IStream_Write(dest, s->data + s->pos, chunk, &put);
      s->pos += put;
      total_read += put;
      total_written += put;
      if (put != chunk) break;
      n -= chunk;
    }
  }
  if (read) read->QuadPart = total_read;
  if (written) written->QuadPart = total_written;
  return hr;
}
static HRESULT STDMETHODCALLTYPE ms_commit(IStream *self, DWORD flags) { (void)flags; return stream_save(mem_stream_of(self)); }
static HRESULT STDMETHODCALLTYPE ms_revert(IStream *self) { (void)self; return S_OK; }
static HRESULT STDMETHODCALLTYPE ms_lock(IStream *self, ULARGE_INTEGER offset, ULARGE_INTEGER length, DWORD type) { (void)self; (void)offset; (void)length; (void)type; return STG_E_INVALIDFUNCTION_; }
static HRESULT STDMETHODCALLTYPE ms_stat(IStream *self, STATSTG *out, DWORD flags) {
  MemStream *s = mem_stream_of(self);
  (void)flags;
  if (!out) return STG_E_INVALIDPOINTER_;
  mem_zero(out, sizeof(*out));
  out->type = STGTY_STREAM;
  out->cbSize.QuadPart = s->size;
  out->grfMode = s->key && !s->writable ? STGM_READ : STGM_READWRITE;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE ms_clone(IStream *self, IStream **out);

static const IStreamVtbl g_mem_stream_vtbl = {ms_query, ms_addref, ms_release, ms_read, ms_write, ms_seek, ms_set_size, ms_copy_to, ms_commit, ms_revert, ms_lock, ms_lock, ms_stat, ms_clone};

static MemStream *new_mem_stream(const void *init, SIZE_T size) {
  MemStream *s = mem_alloc(sizeof(MemStream));
  if (!s) return NULL;
  s->iface.lpVtbl = (IStreamVtbl *)&g_mem_stream_vtbl;
  s->ref = 1;
  if (size) {
    if (!stream_reserve(s, size)) { mem_free(s); return NULL; }
    if (init) mem_copy(s->data, init, size);
    s->size = size;
  }
  return s;
}
static HRESULT STDMETHODCALLTYPE ms_clone(IStream *self, IStream **out) {
  MemStream *s = mem_stream_of(self);
  if (!out) return STG_E_INVALIDPOINTER_;
  MemStream *copy = new_mem_stream(s->data, s->size);
  *out = copy ? &copy->iface : NULL;
  if (!copy) return E_OUTOFMEMORY;
  copy->pos = s->pos;
  return S_OK;
}

EXPORT IStream *WINAPI SHCreateMemStream(const BYTE *init, UINT size) {
  if (!init && size) return NULL;
  MemStream *s = new_mem_stream(init, size);
  return s ? &s->iface : NULL;
}

/* The registry streams: the value is read into memory; a stream opened for writing stores it back (as REG_BINARY) when it is committed or released. */
static IStream *open_reg_stream(HKEY root, LPCWSTR subkey, LPCWSTR value, DWORD mode) {
  const BOOL write = (mode & (STGM_WRITE | STGM_READWRITE)) != 0;
  HKEY key = NULL;
  DWORD type = 0, size = 0;
  BYTE *data = NULL;
  if (write ? RegCreateKeyExW(root, subkey, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS : RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) key = NULL;
  if (key && RegQueryValueExW(key, value, NULL, &type, NULL, &size) == ERROR_SUCCESS && size) {
    data = mem_alloc(size);
    if (data && RegQueryValueExW(key, value, NULL, &type, data, &size) != ERROR_SUCCESS) size = 0;
  } else size = 0;
  MemStream *s = new_mem_stream(data, size);
  mem_free(data);
  if (!s) { if (key) RegCloseKey(key); return NULL; }
  if (key) {
    s->key = key;
    s->writable = write;
    if (value) { const SIZE_T n = wlen(value); s->value = mem_alloc((n + 1) * sizeof(WCHAR)); if (s->value) mem_copy(s->value, value, (n + 1) * sizeof(WCHAR)); }
    if ((value && !s->value) || (write && !(mode & STGM_CREATE) && 0)) { ms_release(&s->iface); return NULL; }
  } else if (write) {
    s->writable = FALSE;      /* (the key could not be opened: the stream still reads as empty and drops writes) */
  }
  return &s->iface;
}
EXPORT IStream *WINAPI SHOpenRegStream2W(HKEY root, LPCWSTR subkey, LPCWSTR value, DWORD mode) { return open_reg_stream(root, subkey, value, mode); }
EXPORT IStream *WINAPI SHOpenRegStreamW(HKEY root, LPCWSTR subkey, LPCWSTR value, DWORD mode) { return open_reg_stream(root, subkey, value, mode); }
EXPORT IStream *WINAPI SHOpenRegStream2A(HKEY root, LPCSTR subkey, LPCSTR value, DWORD mode) {
  WCHAR *k = to_wide(subkey), *v = to_wide(value);
  IStream *s = NULL;
  if ((!subkey || k) && (!value || v)) s = open_reg_stream(root, k, v, mode);
  mem_free(k); mem_free(v);
  return s;
}
EXPORT IStream *WINAPI SHOpenRegStreamA(HKEY root, LPCSTR subkey, LPCSTR value, DWORD mode) { return SHOpenRegStream2A(root, subkey, value, mode); }

EXPORT HRESULT WINAPI IStream_Copy(IStream *from, IStream *to, DWORD count) {
  BYTE buffer[1024];
  if (!from || !to) return E_INVALIDARG;
  while (count) {
    ULONG got = 0, put = 0;
    const ULONG chunk = count < sizeof(buffer) ? count : (ULONG)sizeof(buffer);
    HRESULT hr = IStream_Read(from, buffer, chunk, &got);
    if (FAILED(hr)) return hr;
    if (!got) return S_OK;
    hr = IStream_Write(to, buffer, got, &put);
    if (FAILED(hr)) return hr;
    if (put != got) return E_FAIL;
    count -= got;
  }
  return S_OK;
}

/* A string in a stream: its length in characters (a DWORD) and the UTF-16 characters, without the terminator. */
EXPORT HRESULT WINAPI IStream_WriteStr(IStream *stream, const WCHAR *text) {
  ULONG put = 0;
  if (!stream || !text) return E_INVALIDARG;
  const DWORD n = (DWORD)wlen(text);
  HRESULT hr = IStream_Write(stream, &n, sizeof(n), &put);
  if (FAILED(hr)) return hr;
  if (put != sizeof(n)) return E_FAIL;
  if (!n) return S_OK;
  hr = IStream_Write(stream, text, n * sizeof(WCHAR), &put);
  if (FAILED(hr)) return hr;
  return put == n * sizeof(WCHAR) ? S_OK : E_FAIL;
}
EXPORT HRESULT WINAPI IStream_ReadStr(IStream *stream, WCHAR **text) {
  DWORD n = 0;
  ULONG got = 0;
  if (!text) return E_INVALIDARG;
  *text = NULL;
  if (!stream) return E_INVALIDARG;
  HRESULT hr = IStream_Read(stream, &n, sizeof(n), &got);
  if (FAILED(hr)) return hr;
  if (got != sizeof(n)) return E_FAIL;
  if (n > 0x1000000u) return E_FAIL;
  WCHAR *out = CoTaskMemAlloc(((SIZE_T)n + 1) * sizeof(WCHAR));
  if (!out) return E_OUTOFMEMORY;
  hr = n ? IStream_Read(stream, out, n * sizeof(WCHAR), &got) : S_OK;
  if (FAILED(hr) || (n && got != n * sizeof(WCHAR))) { CoTaskMemFree(out); return FAILED(hr) ? hr : E_FAIL; }
  out[n] = 0;
  *text = out;
  return S_OK;
}

/* ---- COM helpers --------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT void WINAPI IUnknown_AtomicRelease(void **object) {
  if (!object || !*object) return;
  IUnknown *u = *object;
  *object = NULL;
  IUnknown_Release(u);
}
/* Stores `source` (with a reference) in *dest and releases what was there. */
EXPORT void WINAPI IUnknown_Set(IUnknown **dest, IUnknown *source) {
  if (!dest) return;
  if (*dest == source) return;
  if (source) IUnknown_AddRef(source);
  IUnknown *old = *dest;
  *dest = source;
  if (old) IUnknown_Release(old);
}
EXPORT HRESULT WINAPI IUnknown_GetSite(IUnknown *object, REFIID iid, void **site) {
  IObjectWithSite *with_site = NULL;
  if (site) *site = NULL;
  if (!object || !site) return E_FAIL;
  HRESULT hr = IUnknown_QueryInterface(object, &IID_IObjectWithSite, (void **)&with_site);
  if (FAILED(hr)) return hr;
  hr = IObjectWithSite_GetSite(with_site, iid, site);
  IObjectWithSite_Release(with_site);
  return hr;
}
EXPORT HRESULT WINAPI IUnknown_SetSite(IUnknown *object, IUnknown *site) {
  IObjectWithSite *with_site = NULL;
  if (!object) return E_FAIL;
  HRESULT hr = IUnknown_QueryInterface(object, &IID_IObjectWithSite, (void **)&with_site);
  if (FAILED(hr)) return hr;
  hr = IObjectWithSite_SetSite(with_site, site);
  IObjectWithSite_Release(with_site);
  return hr;
}
EXPORT HRESULT WINAPI IUnknown_QueryService(IUnknown *object, REFGUID service, REFIID iid, void **out) {
  IServiceProvider *provider = NULL;
  if (out) *out = NULL;
  if (!object) return E_FAIL;
  if (!out) return E_INVALIDARG;
  HRESULT hr = IUnknown_QueryInterface(object, &IID_IServiceProvider, (void **)&provider);
  if (FAILED(hr)) return hr;
  hr = IServiceProvider_QueryService(provider, service, iid, out);
  IServiceProvider_Release(provider);
  return hr;
}

/* ---- the process reference, thread references, threads --------------------------------------------------------------------------------------------------- */

static IUnknown *volatile g_process_ref;
EXPORT void WINAPI SetProcessReference(IUnknown *object) { g_process_ref = object; }
EXPORT HRESULT WINAPI GetProcessReference(IUnknown **object) {
  if (!object) return E_INVALIDARG;
  IUnknown *ref = g_process_ref;
  *object = ref;
  if (!ref) return E_FAIL;
  IUnknown_AddRef(ref);
  return S_OK;
}
EXPORT HRESULT WINAPI SHGetInstanceExplorer(IUnknown **object) { return GetProcessReference(object); }

/* The reference of the thread (SHSetThreadRef): an object that keeps the thread's owner alive, kept in a TLS slot. */
static volatile LONG g_thread_ref_slot = -1;      /* the TLS index + 1, 0 while it is being allocated */
static DWORD thread_ref_slot(void) {
  LONG slot = g_thread_ref_slot;
  if (slot > 0) return (DWORD)(slot - 1);
  DWORD index = TlsAlloc();
  if (index == TLS_OUT_OF_INDEXES) return TLS_OUT_OF_INDEXES;
  if (InterlockedCompareExchange(&g_thread_ref_slot, (LONG)index + 1, -1) != -1) { TlsFree(index); return (DWORD)(g_thread_ref_slot - 1); }
  return index;
}
EXPORT HRESULT WINAPI SHSetThreadRef(IUnknown *object) {
  const DWORD slot = thread_ref_slot();
  if (slot == TLS_OUT_OF_INDEXES) return E_OUTOFMEMORY;
  return TlsSetValue(slot, object) ? S_OK : E_FAIL;
}
EXPORT HRESULT WINAPI SHGetThreadRef(IUnknown **object) {
  if (!object) return E_INVALIDARG;
  *object = NULL;
  const DWORD slot = thread_ref_slot();
  IUnknown *ref = slot == TLS_OUT_OF_INDEXES ? NULL : (IUnknown *)TlsGetValue(slot);
  if (!ref) return E_NOINTERFACE;
  IUnknown_AddRef(ref);
  *object = ref;
  return S_OK;
}
EXPORT HRESULT WINAPI SHReleaseThreadRef(void) { return S_OK; }

/* SHCreateThreadRef: an IUnknown whose reference count lives in the caller's LONG. */
typedef struct { IUnknown iface; LONG *count; } CountedRef;
static HRESULT STDMETHODCALLTYPE cr_query(IUnknown *self, REFIID iid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!is_iid(iid, &IID_IUnknown)) return E_NOINTERFACE;
  *out = self;
  InterlockedIncrement(((CountedRef *)self)->count);
  return S_OK;
}
static ULONG STDMETHODCALLTYPE cr_addref(IUnknown *self) { return (ULONG)InterlockedIncrement(((CountedRef *)self)->count); }
static ULONG STDMETHODCALLTYPE cr_release(IUnknown *self) {
  const LONG n = InterlockedDecrement(((CountedRef *)self)->count);
  if (n <= 0) { mem_free(self); return 0; }
  return (ULONG)n;
}
static const IUnknownVtbl g_counted_ref_vtbl = {cr_query, cr_addref, cr_release};
EXPORT HRESULT WINAPI SHCreateThreadRef(LONG *count, IUnknown **out) {
  if (!count || !out) return E_INVALIDARG;
  *out = NULL;
  CountedRef *r = mem_alloc(sizeof(CountedRef));
  if (!r) return E_OUTOFMEMORY;
  r->iface.lpVtbl = (IUnknownVtbl *)&g_counted_ref_vtbl;
  r->count = count;
  *count = 1;
  *out = &r->iface;
  return S_OK;
}

#define CTF_INSIST 0x00000001
#define CTF_THREAD_REF 0x00000002
#define CTF_PROCESS_REF 0x00000004
#define CTF_COINIT_STA 0x00000008
#define CTF_COINIT 0x00000008
#define CTF_FREELIBANDEXIT 0x00000010
#define CTF_REF_COUNTED 0x00000020
#define CTF_WAIT_ALLOWCOM 0x00000040
#define CTF_UNUSED 0x00000080
#define CTF_INHERITWOW64 0x00000100
#define CTF_OLEINITIALIZE 0x00000200
#define CTF_NOADDREFLIB 0x00001000

typedef struct {
  LPTHREAD_START_ROUTINE proc, callback;
  void *data;
  DWORD flags;
  IUnknown *process_ref, *thread_ref;
  HANDLE started;
  HMODULE library;
} ShThread;

static DWORD WINAPI sh_thread_entry(void *arg) {
  ShThread t = *(ShThread *)arg;
  BOOL com = FALSE;
  DWORD result = 0;
  SetEvent(((ShThread *)arg)->started);      /* (the creator may free the block from now on) */
  if (t.flags & CTF_COINIT) com = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
  else if (t.flags & CTF_OLEINITIALIZE) com = SUCCEEDED(OleInitialize(NULL));
  if (t.thread_ref) SHSetThreadRef(t.thread_ref);
  if (t.callback) t.callback(t.data);
  result = t.proc ? t.proc(t.data) : 0;
  if (t.thread_ref) { SHSetThreadRef(NULL); IUnknown_Release(t.thread_ref); }
  if (t.process_ref) IUnknown_Release(t.process_ref);
  if (com) { if (t.flags & CTF_OLEINITIALIZE) OleUninitialize(); else CoUninitialize(); }
  if ((t.flags & CTF_FREELIBANDEXIT) && t.library) FreeLibraryAndExitThread(t.library, result);
  return result;
}

static BOOL create_sh_thread(LPTHREAD_START_ROUTINE proc, void *data, DWORD flags, LPTHREAD_START_ROUTINE callback, HANDLE *handle) {
  ShThread t;
  mem_zero(&t, sizeof(t));
  t.proc = proc;
  t.callback = callback;
  t.data = data;
  t.flags = flags;
  if (handle) *handle = NULL;
  if (!proc) return FALSE;
  if (flags & CTF_PROCESS_REF) GetProcessReference(&t.process_ref);
  if (flags & CTF_THREAD_REF) SHGetThreadRef(&t.thread_ref);
  if (flags & CTF_FREELIBANDEXIT) GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)proc, &t.library);
  t.started = CreateEventW(NULL, FALSE, FALSE, NULL);
  HANDLE thread = t.started ? CreateThread(NULL, 0, sh_thread_entry, &t, 0, NULL) : NULL;
  if (thread) {
    WaitForSingleObject(t.started, INFINITE);      /* (the block is on this stack: the thread copies it first) */
    CloseHandle(t.started);
    if (handle) *handle = thread; else CloseHandle(thread);
    return TRUE;
  }
  if (t.started) CloseHandle(t.started);
  if (t.process_ref) IUnknown_Release(t.process_ref);
  if (t.thread_ref) IUnknown_Release(t.thread_ref);
  if (t.library) FreeLibrary(t.library);
  if (flags & CTF_INSIST) {      /* the caller insists: run it here */
    if (callback) callback(data);
    proc(data);
    return TRUE;
  }
  return FALSE;
}
EXPORT BOOL WINAPI SHCreateThread(LPTHREAD_START_ROUTINE proc, void *data, DWORD flags, LPTHREAD_START_ROUTINE callback) { return create_sh_thread(proc, data, flags, callback, NULL); }
EXPORT BOOL WINAPI SHCreateThreadWithHandle(LPTHREAD_START_ROUTINE proc, void *data, DWORD flags, LPTHREAD_START_ROUTINE callback, HANDLE *handle) {
  return create_sh_thread(proc, data, flags, callback, handle);
}

/* ---- the process ------------------------------------------------------------------------------------------------------------------------------------------ */

/* IsOS: this is a Windows 10 workstation, not a domain member, with the new logon UI and fast user switching. */
EXPORT BOOL WINAPI IsOS(DWORD feature) {
  switch (feature) {
    case 1:       /* OS_NT */
    case 2:       /* OS_WIN95ORGREATER */
    case 3:       /* OS_NT4ORGREATER */
    case 5:       /* OS_WIN98ORGREATER */
    case 7:       /* OS_WIN2000ORGREATER */
    case 8:       /* OS_WIN2000PRO */
    case 17:      /* OS_MEORGREATER */
    case 18:      /* OS_XPORGREATER */
    case 20:      /* OS_PROFESSIONAL */
    case 26:      /* OS_FASTUSERSWITCHING */
    case 27:      /* OS_WELCOMELOGONUI */
      return TRUE;
    default:
      return FALSE;
  }
}

/* The explicit AppUserModelID lives in the process parameters: the window title with STARTF_TITLEISAPPID (and Windows' 0x4000 in the flags). The parameters block is
 * read at its documented x64 offsets: WindowFlags 0xA4, WindowTitle 0xB0. */
#define APPID_MAX_CHARS 128
#define STARTF_APPID_SET_ 0x4000
typedef struct { USHORT Length, MaximumLength; WCHAR *Buffer; } TitleString;
static BYTE *process_parameters(void) { return (BYTE *)NtCurrentTeb()->ProcessEnvironmentBlock->ProcessParameters; }
#define PARAMS_FLAGS(p) (*(DWORD *)((p) + 0xA4))
#define PARAMS_TITLE(p) ((TitleString *)((p) + 0xB0))
EXPORT HRESULT WINAPI SetCurrentProcessExplicitAppUserModelID(const WCHAR *id) {
  BYTE *params = process_parameters();
  if (!id || !*id || wlen(id) >= APPID_MAX_CHARS) return E_INVALIDARG;
  const SIZE_T bytes = (wlen(id) + 1) * sizeof(WCHAR);
  WCHAR *copy = mem_alloc(bytes);
  if (!copy) return E_OUTOFMEMORY;
  mem_copy(copy, id, bytes);
  TitleString *title = PARAMS_TITLE(params);      /* (the old title belongs to the parameters block: it is not freed) */
  title->Buffer = copy;
  title->Length = (USHORT)(bytes - sizeof(WCHAR));
  title->MaximumLength = (USHORT)bytes;
  PARAMS_FLAGS(params) = STARTF_TITLEISAPPID | STARTF_APPID_SET_;
  return S_OK;
}
EXPORT HRESULT WINAPI GetCurrentProcessExplicitAppUserModelID(WCHAR **id) {
  BYTE *params = process_parameters();
  if (!id) return E_INVALIDARG;
  *id = NULL;
  const TitleString *title = PARAMS_TITLE(params);
  if (!(PARAMS_FLAGS(params) & STARTF_TITLEISAPPID) || !title->Buffer) return E_FAIL;
  const SIZE_T n = title->Length / sizeof(WCHAR);
  WCHAR *out = CoTaskMemAlloc((n + 1) * sizeof(WCHAR));
  if (!out) return E_OUTOFMEMORY;
  mem_copy(out, title->Buffer, n * sizeof(WCHAR));
  out[n] = 0;
  *id = out;
  return S_OK;
}

EXPORT HRESULT WINAPI IsProcessInIsolatedContainer(BOOL *isolated) {
  if (!isolated) return E_INVALIDARG;
  *isolated = FALSE;
  return S_OK;
}
EXPORT HRESULT WINAPI IsProcessInWDAGContainer(void *reserved, BOOL *in_container) {
  (void)reserved;
  if (!in_container) return E_INVALIDARG;
  *in_container = FALSE;
  return S_OK;
}

/* Feature management: nothing is configured here, so every feature has its default state and no variant. */
typedef struct { DWORD id; } FeatureSubscription;
EXPORT int WINAPI GetFeatureEnabledState(UINT feature, int change_time) { (void)feature; (void)change_time; return 0; }      /* FEATURE_ENABLED_STATE_DEFAULT */
EXPORT UINT WINAPI GetFeatureVariant(UINT feature, int change_time, UINT *payload_id, BOOL *has_notification) {
  (void)feature; (void)change_time;
  if (payload_id) *payload_id = 0;
  if (has_notification) *has_notification = FALSE;
  return 0;
}
EXPORT void WINAPI RecordFeatureUsage(UINT feature, UINT kind, UINT count, const char *name) { (void)feature; (void)kind; (void)count; (void)name; }
EXPORT void WINAPI RecordFeatureError(UINT feature, const void *error) { (void)feature; (void)error; }
EXPORT void WINAPI SubscribeFeatureStateChangeNotification(void **subscription, void *callback, void *context) {
  (void)callback; (void)context;
  if (subscription) *subscription = mem_alloc(sizeof(FeatureSubscription));      /* (the state never changes: the callback is never called) */
}
EXPORT void WINAPI UnsubscribeFeatureStateChangeNotification(void *subscription) { mem_free(subscription); }

/* ---- WinRT and the COM entry points ------------------------------------------------------------------------------------------------------------------- */

EXPORT HRESULT WINAPI CreateRandomAccessStreamOnFile(const WCHAR *path, DWORD mode, REFIID iid, void **out) { (void)path; (void)mode; (void)iid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CreateRandomAccessStreamOverStream(IStream *stream, int options, REFIID iid, void **out) { (void)stream; (void)options; (void)iid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CreateStreamOverRandomAccessStream(IUnknown *stream, REFIID iid, void **out) { (void)stream; (void)iid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI DllGetActivationFactory(void *class_id, void **factory) { (void)class_id; if (factory) *factory = NULL; return CLASS_E_CLASSNOTAVAILABLE; }
EXPORT HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out) { (void)clsid; (void)iid; if (out) *out = NULL; return CLASS_E_CLASSNOTAVAILABLE; }
EXPORT HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }
