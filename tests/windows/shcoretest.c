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

/* shcore.dll: DPI, the truncating string copies, registry helpers, memory and registry streams, COM helpers, thread references, IsOS, the forwarders to shlwapi and shell32
 * (mingw gcc; run it as shcore_own.exe through `scripts/winetest.sh shcore own`). Prints "shcoretest: N checks, M failed"; the exit code is the number of failures. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;
static void check_(int ok, const char *what, int line) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL line %d: %s\n", line, what);
}
#define check(ok, what) check_((ok) ? 1 : 0, what, __LINE__)

static HMODULE g_core;
static HRESULT (WINAPI *pGetDpiForMonitor)(void *, int, UINT *, UINT *);
static HRESULT (WINAPI *pGetProcessDpiAwareness)(HANDLE, int *);
static HRESULT (WINAPI *pSetProcessDpiAwareness)(int);
static HRESULT (WINAPI *pGetScaleFactorForMonitor)(void *, int *);
static int (WINAPI *pGetScaleFactorForDevice)(int);
static UINT (WINAPI *pGetDpiForShellUIComponent)(int);
static IStream *(WINAPI *pSHCreateMemStream)(const BYTE *, UINT);
static HRESULT (WINAPI *pIStream_Copy)(IStream *, IStream *, DWORD);
static HRESULT (WINAPI *pIStream_WriteStr)(IStream *, const WCHAR *);
static HRESULT (WINAPI *pIStream_ReadStr)(IStream *, WCHAR **);
static void (WINAPI *pIUnknown_Set)(IUnknown **, IUnknown *);
static void (WINAPI *pIUnknown_AtomicRelease)(void **);
static BOOL (WINAPI *pIsOS)(DWORD);
static HRESULT (WINAPI *pSHGetThreadRef)(IUnknown **);
static HRESULT (WINAPI *pSHSetThreadRef)(IUnknown *);
static HRESULT (WINAPI *pSHCreateThreadRef)(LONG *, IUnknown **);
static BOOL (WINAPI *pSHCreateThread)(LPTHREAD_START_ROUTINE, void *, DWORD, LPTHREAD_START_ROUTINE);
static LSTATUS (WINAPI *pSHRegGetValueA)(HKEY, const char *, const char *, DWORD, DWORD *, void *, DWORD *);
static int (WINAPI *pSHRegGetIntW)(HKEY, const WCHAR *, int);
static LSTATUS (WINAPI *pSHEnumKeyExA)(HKEY, DWORD, char *, DWORD *);
static LSTATUS (WINAPI *pSHEnumValueA)(HKEY, DWORD, char *, DWORD *, DWORD *, void *, DWORD *);
static LSTATUS (WINAPI *pSHQueryInfoKeyA)(HKEY, DWORD *, DWORD *, DWORD *, DWORD *);
static LSTATUS (WINAPI *pSHCopyKeyA)(HKEY, const char *, HKEY, DWORD);
static IStream *(WINAPI *pSHOpenRegStreamA)(HKEY, const char *, const char *, DWORD);
static LSTATUS (WINAPI *pSHRegSetPathA)(HKEY, const char *, const char *, const char *, DWORD);
static LSTATUS (WINAPI *pSHRegGetPathA)(HKEY, const char *, const char *, char *, DWORD);
static LSTATUS (WINAPI *pSHDeleteKeyA)(HKEY, const char *);
static LSTATUS (WINAPI *pSHDeleteEmptyKeyA)(HKEY, const char *);
static LSTATUS (WINAPI *pSHDeleteValueA)(HKEY, const char *, const char *);
static HRESULT (WINAPI *pSetCurrentProcessExplicitAppUserModelID)(const WCHAR *);
static HRESULT (WINAPI *pGetCurrentProcessExplicitAppUserModelID)(WCHAR **);
static WCHAR **(WINAPI *pCommandLineToArgvW)(const WCHAR *, int *);
static LSTATUS (WINAPI *pSHGetValueA)(HKEY, const char *, const char *, DWORD *, void *, DWORD *);
static HRESULT (WINAPI *pRegisterScaleChangeEvent)(void *, void *);
static HRESULT (WINAPI *pUnregisterScaleChangeEvent)(DWORD_PTR);

static void load(void) {
  g_core = LoadLibraryA("shcore.dll");
  if (!g_core) { printf("no shcore.dll\n"); exit(1); }
#define X(n) do { p##n = (void *)GetProcAddress(g_core, #n); if (!p##n) { printf("FAIL: shcore has no %s\n", #n); ++g_failed; } } while (0)
  X(GetDpiForMonitor); X(GetProcessDpiAwareness); X(SetProcessDpiAwareness); X(GetScaleFactorForMonitor); X(GetScaleFactorForDevice); X(GetDpiForShellUIComponent);
  X(SHCreateMemStream); X(IStream_Copy); X(IStream_WriteStr); X(IStream_ReadStr); X(IUnknown_Set); X(IUnknown_AtomicRelease); X(IsOS); X(SHGetThreadRef); X(SHSetThreadRef);
  X(SHCreateThreadRef); X(SHCreateThread); X(SHRegGetValueA); X(SHRegGetIntW); X(SHEnumKeyExA); X(SHEnumValueA); X(SHQueryInfoKeyA); X(SHCopyKeyA); X(SHOpenRegStreamA);
  X(SHRegSetPathA); X(SHRegGetPathA); X(SHDeleteEmptyKeyA); X(SHDeleteValueA); X(SetCurrentProcessExplicitAppUserModelID); X(GetCurrentProcessExplicitAppUserModelID);
  X(CommandLineToArgvW); X(SHDeleteKeyA); X(SHGetValueA); X(RegisterScaleChangeEvent); X(UnregisterScaleChangeEvent);
#undef X
}

static void test_dpi(void) {
  UINT x = 0, y = 0;
  int awareness = -1, scale = 0;
  void *monitor = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
  check(pGetDpiForMonitor(NULL, 0, &x, &y) == E_INVALIDARG, "null monitor");
  if (monitor) {
    check(pGetDpiForMonitor(monitor, 0, &x, &y) == S_OK && x >= 96 && x == y, "dpi of the primary monitor");
    check(pGetDpiForMonitor(monitor, 3, &x, &y) == E_INVALIDARG, "bad type");
    check(pGetScaleFactorForMonitor(monitor, &scale) == S_OK && scale >= 100, "scale of the monitor");
  }
  check(pGetScaleFactorForMonitor(monitor, NULL) == E_INVALIDARG, "no scale out");
  check(pGetScaleFactorForDevice(0) >= 100, "device scale");
  check(pGetDpiForShellUIComponent(0) >= 96 && pGetDpiForShellUIComponent(9) == 0, "shell component dpi");
  check(pGetProcessDpiAwareness(NULL, &awareness) == S_OK && awareness >= 0 && awareness <= 2, "awareness");
  check(pSetProcessDpiAwareness(7) == E_INVALIDARG, "bad awareness");
  check(pSetProcessDpiAwareness(2) == S_OK, "set awareness");
  check(pGetProcessDpiAwareness(GetCurrentProcess(), &awareness) == S_OK && awareness == 2, "awareness is per-monitor");
  check(pSetProcessDpiAwareness(1) == E_ACCESSDENIED, "set only once");
  HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
  DWORD_PTR cookie = 0;
  check(pRegisterScaleChangeEvent(event, &cookie) == S_OK && cookie, "register scale event");
  check(pUnregisterScaleChangeEvent(cookie) == S_OK && pUnregisterScaleChangeEvent(cookie) == E_INVALIDARG, "unregister scale event");
  CloseHandle(event);
}

static void test_strings(void) {
  char a[16];
  WCHAR w[16];
  int n;
  HMODULE core = g_core;
  int (WINAPI *to_ansi)(const WCHAR *, char *, int) = (void *)GetProcAddress(core, "SHUnicodeToAnsi");
  int (WINAPI *to_wide)(const char *, WCHAR *, int) = (void *)GetProcAddress(core, "SHAnsiToUnicode");
  int (WINAPI *ansi)(const char *, char *, int) = (void *)GetProcAddress(core, "SHAnsiToAnsi");
  int (WINAPI *wide)(const WCHAR *, WCHAR *, int) = (void *)GetProcAddress(core, "SHUnicodeToUnicode");
  check(to_ansi && to_wide && ansi && wide, "string exports");
  if (!(to_ansi && to_wide && ansi && wide)) return;
  n = to_ansi(L"test", a, 16);
  check(n == 5 && !strcmp(a, "test"), "SHUnicodeToAnsi");
  n = to_ansi(L"test", a, 2);
  check(n == 2 && !strcmp(a, "t"), "SHUnicodeToAnsi truncates");
  n = to_wide("test", w, 16);
  check(n == 5 && w[3] == 't' && !w[4], "SHAnsiToUnicode");
  n = ansi("test", a, 4);
  check(n == 0 && !strcmp(a, "tes"), "SHAnsiToAnsi truncated is 0");
  check(ansi("test", a, 5) == 5, "SHAnsiToAnsi fits");
  n = wide(L"test", w, 3);
  check(n == 0 && w[0] == 't' && w[1] == 'e' && !w[2], "SHUnicodeToUnicode truncated");
  check(wide(L"", w, 3) == 1, "SHUnicodeToUnicode empty");
}

static void test_streams(void) {
  BYTE data[8] = {1, 2, 3, 4, 5, 6, 7, 8}, back[16];
  ULONG n = 0;
  LARGE_INTEGER zero = {{0, 0}}, move;
  ULARGE_INTEGER pos;
  IStream *s = pSHCreateMemStream(data, 8);
  check(s != NULL, "mem stream");
  if (!s) return;
  check(IStream_Read(s, back, 16, &n) == S_OK && n == 8 && !memcmp(back, data, 8), "read all");
  check(IStream_Read(s, back, 16, &n) == S_OK && n == 0, "read at the end");
  move.QuadPart = 12;
  check(IStream_Seek(s, move, STREAM_SEEK_SET, &pos) == S_OK && pos.QuadPart == 12, "seek past the end");
  check(IStream_Write(s, "xy", 2, &n) == S_OK && n == 2, "write past the end");
  STATSTG st;
  check(IStream_Stat(s, &st, STATFLAG_NONAME) == S_OK && st.cbSize.QuadPart == 14, "size grew");
  IStream_Seek(s, zero, STREAM_SEEK_SET, NULL);
  IStream_Read(s, back, 14, &n);
  check(n == 14 && back[8] == 0 && back[11] == 0 && back[12] == 'x', "the gap reads as zeros");
  move.QuadPart = -1;
  check(IStream_Seek(s, move, STREAM_SEEK_SET, NULL) != S_OK, "negative seek fails");
  IStream *t = pSHCreateMemStream(NULL, 0);
  IStream_Seek(s, zero, STREAM_SEEK_SET, NULL);
  check(pIStream_Copy(s, t, 14) == S_OK, "IStream_Copy");
  IStream_Stat(t, &st, STATFLAG_NONAME);
  check(st.cbSize.QuadPart == 14, "copied size");
  IStream_Release(s);
  IStream_Release(t);
  s = pSHCreateMemStream(NULL, 0);
  WCHAR *text = NULL;
  check(pIStream_WriteStr(s, L"hello") == S_OK, "WriteStr");
  IStream_Seek(s, zero, STREAM_SEEK_SET, NULL);
  check(pIStream_ReadStr(s, &text) == S_OK && text && !lstrcmpW(text, L"hello"), "ReadStr");
  CoTaskMemFree(text);
  check(pIStream_ReadStr(s, &text) != S_OK && !text, "ReadStr at the end fails");
  IStream_Release(s);
  check(pSHCreateMemStream(NULL, 4) == NULL, "no data, a size");
}

static void test_registry(void) {
  HKEY key, sub;
  DWORD type, size, subkeys, values, max_key, max_value;
  char name[64], buffer[64];
  RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\TcosShcoreTest\\Copy\\Inner");
  RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\TcosShcoreTest\\Copy");
  RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\TcosShcoreTest\\Source\\Inner");
  RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\TcosShcoreTest\\Source");
  check(RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\TcosShcoreTest\\Source", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL) == 0, "create key");
  RegSetValueExA(key, "Text", 0, REG_SZ, (const BYTE *)"value", 6);
  DWORD number = 42;
  RegSetValueExA(key, "Number", 0, REG_DWORD, (const BYTE *)&number, 4);
  RegSetValueExA(key, "NumberText", 0, REG_SZ, (const BYTE *)"-17", 4);
  RegCreateKeyExA(key, "Inner", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &sub, NULL);
  RegSetValueExA(sub, "Deep", 0, REG_SZ, (const BYTE *)"deeper", 7);
  RegCloseKey(sub);

  size = sizeof(buffer);
  check(pSHRegGetValueA(key, NULL, "Text", RRF_RT_REG_SZ, &type, buffer, &size) == 0 && !strcmp(buffer, "value") && type == REG_SZ, "SHRegGetValue");
  size = sizeof(buffer);
  check(pSHRegGetValueA(key, NULL, "Text", RRF_RT_REG_DWORD, &type, buffer, &size) == ERROR_UNSUPPORTED_TYPE, "SHRegGetValue type filter");
  check(pSHRegGetIntW(key, L"Number", 7) == 42 && pSHRegGetIntW(key, L"NumberText", 7) == -17 && pSHRegGetIntW(key, L"Missing", 7) == 7, "SHRegGetInt");
  check(pSHQueryInfoKeyA(key, &subkeys, &max_key, &values, &max_value) == 0 && subkeys == 1 && values == 3, "SHQueryInfoKey");
  size = sizeof(name);
  check(pSHEnumKeyExA(key, 0, name, &size) == 0 && !strcmp(name, "Inner"), "SHEnumKeyEx");
  size = sizeof(name);
  DWORD data_size = sizeof(buffer);
  check(pSHEnumValueA(key, 0, name, &size, &type, buffer, &data_size) == 0, "SHEnumValue");

  HKEY copy;
  RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\TcosShcoreTest\\Copy", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &copy, NULL);
  check(pSHCopyKeyA(key, NULL, copy, 0) == 0, "SHCopyKey");
  size = sizeof(buffer);
  check(pSHGetValueA(copy, "Inner", "Deep", &type, buffer, &size) == 0 && !strcmp(buffer, "deeper"), "the copy has the subkey and its value (and SHGetValueA forwards)");
  size = sizeof(buffer);
  check(pSHGetValueA(copy, NULL, "Text", &type, buffer, &size) == 0 && !strcmp(buffer, "value"), "the copy has the values");

  check(pSHRegSetPathA(copy, NULL, "Dir", "C:\\Windows", 0) == 0, "SHRegSetPath");
  check(pSHRegGetPathA(copy, NULL, "Dir", buffer, 0) == 0 && !strcmp(buffer, "C:\\Windows"), "SHRegGetPath");
  check(pSHDeleteValueA(copy, NULL, "Dir") == 0, "SHDeleteValue");
  check(pSHDeleteEmptyKeyA(HKEY_CURRENT_USER, "Software\\TcosShcoreTest\\Copy") == ERROR_KEY_HAS_CHILDREN, "SHDeleteEmptyKey refuses a key with values");

  /* a registry stream: read, write back */
  IStream *rs = pSHOpenRegStreamA(key, NULL, "Text", STGM_READ);
  ULONG n = 0;
  check(rs != NULL, "SHOpenRegStream");
  if (rs) { IStream_Read(rs, buffer, 32, &n); check(n == 12 && !lstrcmpW((const WCHAR *)buffer, L"value"), "the stream holds the value (stored wide)"); IStream_Release(rs); }
  rs = pSHOpenRegStreamA(key, NULL, "Blob", STGM_WRITE);
  if (rs) { IStream_Write(rs, "abc", 3, &n); IStream_Release(rs); }
  size = sizeof(buffer);
  check(RegQueryValueExA(key, "Blob", NULL, &type, (BYTE *)buffer, &size) == 0 && type == REG_BINARY && size == 3 && !memcmp(buffer, "abc", 3), "a write stream stores on release");

  RegCloseKey(copy);
  RegCloseKey(key);
  pSHDeleteKeyA(HKEY_CURRENT_USER, "Software\\TcosShcoreTest");
}

static IUnknown *g_dummy_ref;
static DWORD WINAPI thread_proc(void *arg) {
  IUnknown *ref = NULL;
  *(HRESULT *)arg = pSHGetThreadRef(&ref);
  if (ref) IUnknown_Release(ref);
  return 0;
}
static void test_com_and_threads(void) {
  LONG count = 0;
  IUnknown *ref = NULL, *got = NULL;
  check(pSHCreateThreadRef(&count, &ref) == S_OK && count == 1, "SHCreateThreadRef");
  check(pSHGetThreadRef(&got) == E_NOINTERFACE, "no thread reference yet");
  check(pSHSetThreadRef(ref) == S_OK && pSHGetThreadRef(&got) == S_OK && got == ref && count == 2, "SHSetThreadRef / GetThreadRef");
  IUnknown_Release(got);
  pSHSetThreadRef(NULL);
  /* IUnknown_Set */
  pIUnknown_Set(&g_dummy_ref, ref);
  check(g_dummy_ref == ref && count == 2, "IUnknown_Set takes a reference");
  pIUnknown_Set(&g_dummy_ref, NULL);
  check(g_dummy_ref == NULL && count == 1, "IUnknown_Set releases");
  pIUnknown_Set(&g_dummy_ref, ref);
  pIUnknown_AtomicRelease((void **)&g_dummy_ref);
  check(g_dummy_ref == NULL && count == 1, "IUnknown_AtomicRelease");
  IUnknown_Release(ref);

  HRESULT hr = 1;
  HANDLE done = CreateEventW(NULL, FALSE, FALSE, NULL);
  (void)done;
  check(pSHCreateThread(thread_proc, &hr, 0, NULL), "SHCreateThread");
  for (int i = 0; i < 200 && hr == 1; ++i) Sleep(10);
  check(hr == E_NOINTERFACE, "the new thread has no thread reference");
  check(pIsOS(1) && !pIsOS(0) && pIsOS(20), "IsOS");
}

static void test_app_id(void) {
  WCHAR *id = NULL;
  check(pGetCurrentProcessExplicitAppUserModelID(&id) == E_FAIL && !id, "no id yet");
  check(pSetCurrentProcessExplicitAppUserModelID(L"Tcos.Shcore.Test") == S_OK, "set id");
  check(pGetCurrentProcessExplicitAppUserModelID(&id) == S_OK && id && !lstrcmpW(id, L"Tcos.Shcore.Test"), "get id");
  CoTaskMemFree(id);
  check(pSetCurrentProcessExplicitAppUserModelID(NULL) == E_INVALIDARG, "no id");
  /* shell32's forwarders reach the same state */
  HMODULE shell = LoadLibraryA("shell32.dll");
  HRESULT (WINAPI *get)(WCHAR **) = (void *)GetProcAddress(shell, "GetCurrentProcessExplicitAppUserModelID");
  check(get && get(&id) == S_OK && !lstrcmpW(id, L"Tcos.Shcore.Test"), "shell32 forwards");
  CoTaskMemFree(id);
  int count = 0;
  WCHAR **argv = pCommandLineToArgvW(L"a \"b c\" d", &count);
  check(argv && count == 3 && !lstrcmpW(argv[1], L"b c"), "CommandLineToArgvW forwards");
  LocalFree(argv);
}

int main(void) {
  load();
  if (g_failed) { printf("shcoretest: missing exports\n"); return g_failed; }
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  test_dpi();
  test_strings();
  test_streams();
  test_registry();
  test_com_and_threads();
  test_app_id();
  printf("shcoretest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
