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

/* A 32-bit Windows program that uses the libraries beyond the C runtime, for tests/wow64_qemu.sh (docs/Wow64.md): what a setup program does when it
 * installs without showing itself - the registry (advapi32), the shell's folders and a shortcut through COM (shell32, ole32), common controls'
 * start-up (comctl32), strings (user32) - and then the window system through the 32-bit user32.dll, gdi32.dll and win32u.dll: a class, a window,
 * the messages of its creation with their structures, sent and posted messages, a timer, a device context. Ends with exit code 42 if every check
 * passed.
 *   i686-w64-mingw32-gcc -O1 -o gui32.exe tests/windows/gui32.c -ladvapi32 -luser32 -lgdi32 -lshell32 -lole32 -lcomctl32 -luuid */
#define COBJMACROS
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <objbase.h>

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? "gui32: ok: %s\n" : "gui32: FAILED: %s\n", what);
  fflush(stdout);
  if (ok) ++g_passed; else ++g_failed;
}

/* ---- the window ---- */
static int g_nccreate, g_create, g_create_good, g_pos_changed, g_pos_good, g_user_messages, g_timer_ticks, g_destroyed, g_size_messages;
static LPARAM g_last_lparam;
static WPARAM g_last_wparam;
static int g_marker;
static LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_NCCREATE:
      ++g_nccreate;
      break;
    case WM_CREATE: {
      const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
      ++g_create;
      g_create_good = create->lpCreateParams == &g_marker && create->cx == 320 && create->cy == 200 && create->x == 40 && create->y == 30 &&
                      create->hInstance == GetModuleHandleW(NULL) && create->lpszName && lstrcmpW(create->lpszName, L"gui32 window") == 0 &&
                      (create->style & WS_CAPTION) == WS_CAPTION;
      return 0;
    }
    case WM_WINDOWPOSCHANGED: {
      const WINDOWPOS *pos = (const WINDOWPOS *)lparam;
      ++g_pos_changed;
      g_pos_good = pos->hwnd == window;
      break;
    }
    case WM_SIZE: ++g_size_messages; break;
    case WM_USER + 1:
      ++g_user_messages;
      g_last_wparam = wparam;
      g_last_lparam = lparam;
      return 0x12345678;
    case WM_TIMER:
      ++g_timer_ticks;
      return 0;
    case WM_DESTROY:
      ++g_destroyed;
      PostQuitMessage(7);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

int main(void) {
  printf("gui32: the libraries of a 32-bit program beyond the C runtime\n");
  fflush(stdout);

  /* The registry. */
  HKEY key = NULL;
  DWORD disposition = 0;
  LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\TheoryCraftOS\\gui32", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, &disposition);
  check(status == ERROR_SUCCESS && key != NULL, "RegCreateKeyExW");
  const WCHAR text[] = L"a value of text";
  DWORD number = 0xC0FFEE, type = 0, size = 0;
  check(RegSetValueExW(key, L"Text", 0, REG_SZ, (const BYTE *)text, sizeof(text)) == ERROR_SUCCESS &&
        RegSetValueExW(key, L"Number", 0, REG_DWORD, (const BYTE *)&number, sizeof(number)) == ERROR_SUCCESS, "RegSetValueExW");
  WCHAR back[64] = {0};
  size = sizeof(back);
  status = RegQueryValueExW(key, L"Text", NULL, &type, (BYTE *)back, &size);
  check(status == ERROR_SUCCESS && type == REG_SZ && size == sizeof(text) && lstrcmpW(back, text) == 0, "RegQueryValueExW gives the text back");
  number = 0; size = sizeof(number);
  check(RegQueryValueExW(key, L"Number", NULL, &type, (BYTE *)&number, &size) == ERROR_SUCCESS && type == REG_DWORD && number == 0xC0FFEE, "and the number");
  WCHAR name[64];
  DWORD name_size = 64;
  check(RegEnumValueW(key, 0, name, &name_size, NULL, NULL, NULL, NULL) == ERROR_SUCCESS && name_size > 0, "RegEnumValueW");
  check(RegDeleteValueW(key, L"Number") == ERROR_SUCCESS && RegQueryValueExW(key, L"Number", NULL, NULL, NULL, NULL) == ERROR_FILE_NOT_FOUND, "RegDeleteValueW");
  RegCloseKey(key);
  check(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\TheoryCraftOS\\gui32", 0, KEY_READ, &key) == ERROR_SUCCESS, "RegOpenKeyExW");
  RegCloseKey(key);
  check(RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\TheoryCraftOS\\gui32") == ERROR_SUCCESS &&
        RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\TheoryCraftOS\\gui32", 0, KEY_READ, &key) == ERROR_FILE_NOT_FOUND, "RegDeleteKeyW");

  /* The process's token: what a setup program asks to see whether it runs as an administrator. */
  HANDLE token = NULL;
  DWORD elevation_type = 0, token_size = 0;
  check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) && token != NULL, "OpenProcessToken of the current process");
  check(GetTokenInformation(token, TokenElevationType, &elevation_type, sizeof(elevation_type), &token_size) && token_size == sizeof(elevation_type) &&
        elevation_type >= TokenElevationTypeDefault && elevation_type <= TokenElevationTypeLimited, "GetTokenInformation(TokenElevationType)");
  SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
  PSID administrators = NULL;
  BOOL member = FALSE;
  check(AllocateAndInitializeSid(&nt_authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administrators) &&
        CheckTokenMembership(NULL, administrators, &member) && member, "CheckTokenMembership: the user is an administrator");
  if (administrators) FreeSid(administrators);
  if (token) CloseHandle(token);

  /* Strings of user32. */
  WCHAR formatted[64];
  const int written = wsprintfW(formatted, L"%s-%d-%x", L"abc", 42, 0xBEEF);
  check(written == 11 && lstrcmpW(formatted, L"abc-42-beef") == 0 && CharNextW(formatted) == formatted + 1 && CharPrevW(formatted, formatted + 2) == formatted + 1,
        "wsprintfW, CharNextW, CharPrevW");

  /* Common controls, the shell and COM. */
  InitCommonControls();
  check(1, "InitCommonControls returns");
  const HRESULT initialized = OleInitialize(NULL);
  check(SUCCEEDED(initialized), "OleInitialize");
  WCHAR folder[MAX_PATH] = {0};
  LPITEMIDLIST list = NULL;
  const HRESULT located = SHGetSpecialFolderLocation(NULL, CSIDL_PROGRAMS, &list);
  check(SUCCEEDED(located) && list != NULL && SHGetPathFromIDListW(list, folder) && folder[0] != 0, "SHGetSpecialFolderLocation and SHGetPathFromIDListW (the Programs folder)");
  if (list) CoTaskMemFree(list);
  SHFILEINFOW file_info;
  memset(&file_info, 0, sizeof(file_info));
  SHGetFileInfoW(L"C:\\Windows", 0, &file_info, sizeof(file_info), SHGFI_DISPLAYNAME);
  check(1, "SHGetFileInfoW returns");
  IShellLinkW *link = NULL;
  HRESULT made = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
  check(SUCCEEDED(made) && link != NULL, "CoCreateInstance(CLSID_ShellLink)");
  if (link) {
    IPersistFile *file = NULL;
    WCHAR target[MAX_PATH] = {0};
    HRESULT saved = E_FAIL;
    IShellLinkW_SetPath(link, L"C:\\Windows\\gui32.exe");
    IShellLinkW_SetDescription(link, L"made by gui32");
    if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file)) && file) {
      saved = IPersistFile_Save(file, L"C:\\Windows\\Temp\\gui32.lnk", TRUE);
      IPersistFile_Release(file);
    }
    check(SUCCEEDED(saved) && GetFileAttributesW(L"C:\\Windows\\Temp\\gui32.lnk") != INVALID_FILE_ATTRIBUTES, "a shortcut saved through IPersistFile");
    check(SUCCEEDED(IShellLinkW_GetPath(link, target, MAX_PATH, NULL, 0)) && lstrcmpiW(target, L"C:\\Windows\\gui32.exe") == 0, "IShellLinkW keeps its path");
    IShellLinkW_Release(link);
    DeleteFileW(L"C:\\Windows\\Temp\\gui32.lnk");
  }
  if (SUCCEEDED(initialized)) OleUninitialize();

  /* The window system. */
  check(GetSystemMetrics(SM_CXSCREEN) > 0 && GetSystemMetrics(SM_CYSCREEN) > 0, "GetSystemMetrics gives a screen size");
  HDC screen = GetDC(NULL);
  check(screen != NULL && GetDeviceCaps(screen, BITSPIXEL) >= 8 && GetDeviceCaps(screen, LOGPIXELSY) > 0, "GetDC(NULL) and GetDeviceCaps");
  if (screen) ReleaseDC(NULL, screen);
  WNDCLASSW cls;
  memset(&cls, 0, sizeof(cls));
  cls.lpfnWndProc = window_procedure;
  cls.hInstance = GetModuleHandleW(NULL);
  cls.lpszClassName = L"Gui32Class";
  cls.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  const ATOM atom = RegisterClassW(&cls);
  WNDCLASSW found;
  check(atom != 0 && GetClassInfoW(cls.hInstance, L"Gui32Class", &found) && found.lpfnWndProc == window_procedure, "RegisterClassW and GetClassInfoW");
  HWND window = CreateWindowExW(0, L"Gui32Class", L"gui32 window", WS_OVERLAPPEDWINDOW, 40, 30, 320, 200, NULL, NULL, cls.hInstance, &g_marker);
  check(window != NULL && IsWindow(window), "CreateWindowExW");
  check(g_nccreate == 1 && g_create == 1 && g_create_good, "WM_NCCREATE and WM_CREATE arrive, with a CREATESTRUCT a 32-bit program can read");
  WCHAR title[64] = {0};
  check(GetWindowTextW(window, title, 64) == 12 && lstrcmpW(title, L"gui32 window") == 0, "GetWindowTextW");
  check(SetWindowTextW(window, L"renamed") && GetWindowTextW(window, title, 64) == 7 && lstrcmpW(title, L"renamed") == 0, "SetWindowTextW");
  RECT rect = {0, 0, 0, 0};
  check(GetWindowRect(window, &rect) && rect.left == 40 && rect.top == 30 && rect.right - rect.left == 320 && rect.bottom - rect.top == 200, "GetWindowRect");
  check(GetClientRect(window, &rect) && rect.left == 0 && rect.top == 0 && rect.right > 0 && rect.right <= 320 && rect.bottom > 0 && rect.bottom <= 200, "GetClientRect");
  check(SetWindowLongW(window, GWL_USERDATA, 0x1234ABCD) == 0 && GetWindowLongW(window, GWL_USERDATA) == 0x1234ABCD, "SetWindowLongW and GetWindowLongW");
  check((LONG_PTR)GetWindowLongW(window, GWL_WNDPROC) == (LONG_PTR)window_procedure, "the window procedure is the class's");
  g_pos_changed = 0;
  check(SetWindowPos(window, NULL, 60, 50, 400, 240, SWP_NOZORDER | SWP_NOACTIVATE) && g_pos_changed >= 1 && g_pos_good && g_size_messages >= 1 &&
        GetWindowRect(window, &rect) && rect.left == 60 && rect.right - rect.left == 400, "SetWindowPos moves it, with WM_WINDOWPOSCHANGED and WM_SIZE");
  check(SendMessageW(window, WM_USER + 1, 0x11112222, (LPARAM)-5) == 0x12345678 && g_user_messages == 1 && g_last_wparam == 0x11112222 && g_last_lparam == -5,
        "SendMessageW calls the procedure and gives its result");
  check(PostMessageW(window, WM_USER + 1, 3, 4), "PostMessageW");
  MSG message;
  int posted = 0;
  while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
    if (message.message == WM_USER + 1 && message.hwnd == window && message.wParam == 3 && message.lParam == 4) posted = 1;
    DispatchMessageW(&message);
  }
  check(posted && g_user_messages == 2, "PeekMessageW finds the posted message, with an MSG a 32-bit program can read, and DispatchMessageW delivers it");
  check(SetTimer(window, 1, 20, NULL) != 0, "SetTimer");
  const DWORD start = GetTickCount();
  while (g_timer_ticks < 2 && GetTickCount() - start < 3000) {
    if (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message); else Sleep(5);
  }
  KillTimer(window, 1);
  check(g_timer_ticks >= 2, "WM_TIMER arrives");
  HDC dc = GetDC(window);
  HBRUSH brush = CreateSolidBrush(RGB(10, 20, 30));
  RECT fill = {0, 0, 10, 10};
  check(dc != NULL && brush != NULL && FillRect(dc, &fill, brush) && SetBkMode(dc, TRANSPARENT) != 0 && SetTextColor(dc, RGB(1, 2, 3)) != CLR_INVALID, "a device context of the window: FillRect, SetBkMode, SetTextColor");
  if (brush) DeleteObject(brush);
  if (dc) ReleaseDC(window, dc);
  check(DestroyWindow(window) && g_destroyed == 1 && !IsWindow(window), "DestroyWindow, with WM_DESTROY");
  int quit = 0;
  while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) if (message.message == WM_QUIT && message.wParam == 7) quit = 1;
  check(quit, "PostQuitMessage's WM_QUIT is in the queue");
  check(UnregisterClassW(L"Gui32Class", cls.hInstance), "UnregisterClassW");

  printf(g_failed == 0 ? "gui32: all %d checks passed\n" : "gui32: %d checks passed, some FAILED\n", g_passed);
  fflush(stdout);
  return g_failed == 0 ? 42 : 1;
}
