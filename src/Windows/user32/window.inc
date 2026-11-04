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

/* The window system of user32.dll: classes, windows, messages and the message loop. Everything is the kernel's window manager (src/Process/Win32k/Win32kUser*.cpp, docs/win32k/User.md)
 * reached through win32u.dll, so a window made here is the same object a kernel-mode window is: the same tree, styles, messages, focus and painting. The window procedures run in this
 * process; win32u.dll calls them when the kernel asks (the upcalls). The calls follow Wine's ntuser interface, which the kernel half implements.
 * ANSI: the A functions convert their strings and set the "ansi" flag of the services; window procedures registered with RegisterClassA are called with the Unicode messages as
 * they are stored (the kernel does not convert messages for them). */

typedef struct { LONG x, y; } POINT_;
typedef struct { HANDLE hwnd; UINT message; ULONG_PTR wparam; LPARAM lparam; DWORD time; POINT_ pt; DWORD private_; } MSG_;
typedef LONG_PTR (WINAPI *WNDPROC_)(HANDLE, UINT, ULONG_PTR, LONG_PTR);
typedef struct {
  UINT cbSize, style;
  WNDPROC_ lpfnWndProc;
  int cbClsExtra, cbWndExtra;
  HANDLE hInstance, hIcon, hCursor, hbrBackground;
  const void *lpszMenuName, *lpszClassName;
  HANDLE hIconSm;
} WNDCLASSEX_;
typedef struct {
  UINT style;
  WNDPROC_ lpfnWndProc;
  int cbClsExtra, cbWndExtra;
  HANDLE hInstance, hIcon, hCursor, hbrBackground;
  const void *lpszMenuName, *lpszClassName;
} WNDCLASS_;

/* ---- the services (win32u.dll) ------------------------------------------------------------------------------------------------------------------------------------ */
#define SVC IMPORT ULONG_PTR WINAPI
SVC NtUserCreateWindowEx(ULONG_PTR ex_style, ULONG_PTR class_name, ULONG_PTR version, ULONG_PTR window_name, ULONG_PTR style, ULONG_PTR x, ULONG_PTR y, ULONG_PTR cx, ULONG_PTR cy,
                         ULONG_PTR parent, ULONG_PTR menu, ULONG_PTR instance, ULONG_PTR params, ULONG_PTR flags, ULONG_PTR client_instance, ULONG_PTR cls, ULONG_PTR ansi);
SVC NtUserDestroyWindow(ULONG_PTR hwnd);
SVC NtUserSetParent(ULONG_PTR hwnd, ULONG_PTR parent);
SVC NtUserEnableWindow(ULONG_PTR hwnd, ULONG_PTR enable);
SVC NtUserSetWindowLong(ULONG_PTR hwnd, ULONG_PTR index, ULONG_PTR value, ULONG_PTR ansi);
SVC NtUserSetWindowLongPtr(ULONG_PTR hwnd, ULONG_PTR index, ULONG_PTR value, ULONG_PTR ansi);
SVC NtUserSetWindowWord(ULONG_PTR hwnd, ULONG_PTR index, ULONG_PTR value);
SVC NtUserSetWindowPos(ULONG_PTR hwnd, ULONG_PTR after, ULONG_PTR x, ULONG_PTR y, ULONG_PTR cx, ULONG_PTR cy, ULONG_PTR flags);
SVC NtUserMoveWindow(ULONG_PTR hwnd, ULONG_PTR x, ULONG_PTR y, ULONG_PTR cx, ULONG_PTR cy, ULONG_PTR repaint);
SVC NtUserShowWindow(ULONG_PTR hwnd, ULONG_PTR cmd);
SVC NtUserShowWindowAsync(ULONG_PTR hwnd, ULONG_PTR cmd);
SVC NtUserBeginPaint(ULONG_PTR hwnd, ULONG_PTR paint);
SVC NtUserEndPaint(ULONG_PTR hwnd, ULONG_PTR paint);
SVC NtUserGetUpdateRect(ULONG_PTR hwnd, ULONG_PTR rect, ULONG_PTR erase);
SVC NtUserInvalidateRect(ULONG_PTR hwnd, ULONG_PTR rect, ULONG_PTR erase);
SVC NtUserValidateRect(ULONG_PTR hwnd, ULONG_PTR rect);
SVC NtUserUpdateWindow(ULONG_PTR hwnd);
SVC NtUserSetFocus(ULONG_PTR hwnd);
SVC NtUserSetActiveWindow(ULONG_PTR hwnd);
SVC NtUserSetForegroundWindow(ULONG_PTR hwnd);
SVC NtUserGetForegroundWindow(void);
SVC NtUserSetCapture(ULONG_PTR hwnd);
SVC NtUserReleaseCapture(void);
SVC NtUserMessageCall(ULONG_PTR hwnd, ULONG_PTR message, ULONG_PTR wparam, ULONG_PTR lparam, ULONG_PTR info, ULONG_PTR type, ULONG_PTR ansi);
SVC NtUserDispatchMessage(ULONG_PTR msg);
SVC NtUserTranslateMessage(ULONG_PTR msg, ULONG_PTR flags);
SVC NtUserGetMessage(ULONG_PTR msg, ULONG_PTR hwnd, ULONG_PTR first, ULONG_PTR last);
SVC NtUserPeekMessage(ULONG_PTR msg, ULONG_PTR hwnd, ULONG_PTR first, ULONG_PTR last, ULONG_PTR flags);
SVC NtUserWaitMessage(void);
SVC NtUserPostMessage(ULONG_PTR hwnd, ULONG_PTR message, ULONG_PTR wparam, ULONG_PTR lparam);
SVC NtUserPostThreadMessage(ULONG_PTR thread, ULONG_PTR message, ULONG_PTR wparam, ULONG_PTR lparam);
SVC NtUserPostQuitMessage(ULONG_PTR code);
SVC NtUserReplyMessage(ULONG_PTR result);
SVC NtUserGetMessagePos(void);
SVC NtUserGetQueueStatus(ULONG_PTR flags);
SVC NtUserMsgWaitForMultipleObjectsEx(ULONG_PTR count, ULONG_PTR handles, ULONG_PTR timeout, ULONG_PTR mask, ULONG_PTR flags);
SVC NtUserSetTimer(ULONG_PTR hwnd, ULONG_PTR id, ULONG_PTR elapse, ULONG_PTR proc, ULONG_PTR tolerance);
SVC NtUserKillTimer(ULONG_PTR hwnd, ULONG_PTR id);
SVC NtUserCallHwnd(ULONG_PTR hwnd, ULONG_PTR code);
SVC NtUserCallHwndParam(ULONG_PTR hwnd, ULONG_PTR param, ULONG_PTR code);
SVC NtUserCallOneParam(ULONG_PTR param, ULONG_PTR code);
SVC NtUserCallTwoParam(ULONG_PTR a, ULONG_PTR b, ULONG_PTR code);
SVC NtUserGetThreadState(ULONG_PTR what);
SVC NtUserGetAncestor(ULONG_PTR hwnd, ULONG_PTR type);
SVC NtUserFindWindowEx(ULONG_PTR parent, ULONG_PTR child, ULONG_PTR class_name, ULONG_PTR title, ULONG_PTR unknown);
SVC NtUserInternalGetWindowText(ULONG_PTR hwnd, ULONG_PTR buffer, ULONG_PTR count);
SVC NtUserRegisterClassExWOW(ULONG_PTR wc, ULONG_PTR name, ULONG_PTR version, ULONG_PTR menu, ULONG_PTR fnid, ULONG_PTR flags, ULONG_PTR wow);
SVC NtUserUnregisterClass(ULONG_PTR name, ULONG_PTR instance, ULONG_PTR menu);
SVC NtUserGetClassInfoEx(ULONG_PTR instance, ULONG_PTR name, ULONG_PTR wc, ULONG_PTR menu, ULONG_PTR ansi);
SVC NtUserGetClassName(ULONG_PTR hwnd, ULONG_PTR real, ULONG_PTR name);
SVC NtUserSetClassLong(ULONG_PTR hwnd, ULONG_PTR index, ULONG_PTR value, ULONG_PTR ansi);
SVC NtUserSetClassLongPtr(ULONG_PTR hwnd, ULONG_PTR index, ULONG_PTR value, ULONG_PTR ansi);
SVC NtUserSetClassWord(ULONG_PTR hwnd, ULONG_PTR index, ULONG_PTR value);
SVC NtUserRegisterWindowMessage(ULONG_PTR name);
SVC NtUserWindowFromPoint(ULONG_PTR x, ULONG_PTR y);
SVC NtUserSetProp(ULONG_PTR hwnd, ULONG_PTR name, ULONG_PTR data);
SVC NtUserGetProp(ULONG_PTR hwnd, ULONG_PTR name);
SVC NtUserRemoveProp(ULONG_PTR hwnd, ULONG_PTR name);
IMPORT int WINAPI MultiByteToWideChar(UINT code_page, DWORD flags, LPCSTR source, int source_length, LPWSTR destination, int destination_length);
IMPORT int WINAPI WideCharToMultiByte(UINT code_page, DWORD flags, LPCWSTR source, int source_length, LPSTR destination, int destination_length, LPCSTR default_char, LPBOOL used_default);
IMPORT HANDLE WINAPI GetModuleHandleW(LPCWSTR name);
IMPORT void *WINAPI GetProcessHeap(void);
IMPORT void *WINAPI HeapAlloc(void *heap, DWORD flags, SIZE_T bytes);
IMPORT BOOL WINAPI HeapFree(void *heap, DWORD flags, void *memory);

/* codes of NtUserCallHwnd, NtUserCallHwndParam, NtUserCallOneParam, NtUserCallTwoParam and NtUserMessageCall (the order of Wine's ntuser.h, which the kernel half follows) */
enum { CH_GET_PARENT = 5, CH_GET_TEXT_LENGTH = 9, CH_IS_WINDOW = 10, CH_IS_ENABLED = 11, CH_IS_UNICODE = 12, CH_IS_VISIBLE = 13, CH_GET_FULL_HANDLE = 15 };
enum { CHP_CLIENT_TO_SCREEN = 0, CHP_GET_CLASS_LONG_A = 2, CHP_GET_CLASS_LONG_W, CHP_GET_CLASS_LONG_PTR_A, CHP_GET_CLASS_LONG_PTR_W, CHP_GET_CLASS_WORD, CHP_GET_WINDOW_INFO = 8,
       CHP_GET_WINDOW_LONG_A, CHP_GET_WINDOW_LONG_W, CHP_GET_WINDOW_LONG_PTR_A, CHP_GET_WINDOW_LONG_PTR_W, CHP_GET_WINDOW_RECT, CHP_GET_CLIENT_RECT, CHP_GET_WINDOW_RELATIVE = 16,
       CHP_GET_WINDOW_THREAD, CHP_GET_WINDOW_WORD, CHP_IS_CHILD, CHP_MAP_WINDOW_POINTS, CHP_SCREEN_TO_CLIENT = 23 };
#define MC_DEF_WINDOW_PROC 0x29e
#define MC_SEND_MESSAGE 0x2b1
#define MC_SEND_MESSAGE_TIMEOUT 0x2b3
#define MC_SEND_NOTIFY 0x2b7

#define ERROR_INVALID_WINDOW_HANDLE_ 1400
#define ERROR_CLASS_DOES_NOT_EXIST_ 1411

/* ---- strings ------------------------------------------------------------------------------------------------------------------------------------------------------ */
static size_t_ ws_len(const WCHAR *s) { size_t_ n = 0; while (s[n]) ++n; return n; }
static void *u32_alloc(SIZE_T bytes) { return HeapAlloc(GetProcessHeap(), 0, bytes); }
static void u32_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* A name that is a string or an atom (a pointer below 0x10000), as the UNICODE_STRING the services take: an atom has length 0 and the atom as its buffer. */
static void us_name(UNICODE_STRING *us, const WCHAR *name) {
  if (((ULONG_PTR)name >> 16) == 0) { us->Length = us->MaximumLength = 0; us->Buffer = (PWSTR)name; return; }
  const size_t_ n = ws_len(name);
  us->Length = (USHORT)(n * 2); us->MaximumLength = (USHORT)(n * 2 + 2); us->Buffer = (PWSTR)name;
}
/* The wide copy of an ANSI string (or the atom itself); free with u32_free unless it is an atom or NULL. */
static WCHAR *wide_of(const char *s) {
  if (!s || ((ULONG_PTR)s >> 16) == 0) return (WCHAR *)s;
  const int n = MultiByteToWideChar(65001 /* CP_UTF8 */, 0, s, -1, NULL, 0);
  WCHAR *w = u32_alloc((SIZE_T)n * 2 + 2);
  if (!w) return NULL;
  MultiByteToWideChar(65001, 0, s, -1, w, n);
  return w;
}
static void wide_free(WCHAR *w, const char *original) { if (w && original && ((ULONG_PTR)original >> 16) != 0) u32_free(w); }

/* ---- classes ------------------------------------------------------------------------------------------------------------------------------------------------------ */
static LONG_PTR WINAPI builtin_proc(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return (LONG_PTR)NtUserMessageCall((ULONG_PTR)hwnd, message, wparam, (ULONG_PTR)lparam, 0, MC_DEF_WINDOW_PROC, 0); }

static ATOM register_class(const WNDCLASSEX_ *wc, const WCHAR *name, DWORD flags) {
  UNICODE_STRING us;
  ULONG_PTR menu[3] = {0, 0, 0};
  us_name(&us, name);
  return (ATOM)NtUserRegisterClassExWOW((ULONG_PTR)wc, (ULONG_PTR)&us, 0, (ULONG_PTR)menu, 0, flags, 0);
}

/* The classes of the controls and the dialog: global classes whose procedure is the default one (the control behaviour of Windows is not done: a Button, Static or Edit window is a plain
 * window with its text, style and messages kept by the window manager). Made when the process first registers, creates or looks for a class. */
static void builtin_classes(void) {
  static volatile LONG done;
  static const WCHAR *const names[] = {L"Button", L"Static", L"Edit", L"ListBox", L"ComboBox", L"ComboLBox", L"ScrollBar", L"#32770", L"MDIClient", L"IME", L"SysLink", L"tooltips_class32"};
  if (__sync_lock_test_and_set(&done, 1)) return;
  for (size_t_ i = 0; i < sizeof names / sizeof names[0]; ++i) {
    WNDCLASSEX_ wc = {sizeof wc, 0x4000 /* CS_GLOBALCLASS */ | 8 /* CS_DBLCLKS */, builtin_proc, 0, names[i][0] == L'#' ? 30 : 0, GetModuleHandleW(L"user32.dll"), NULL, NULL, NULL, NULL, names[i], NULL};
    register_class(&wc, names[i], 0);
  }
}

EXPORT ATOM WINAPI RegisterClassExW(const WNDCLASSEX_ *wc) {
  builtin_classes();
  if (!wc || wc->cbSize != sizeof *wc || !wc->lpszClassName) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  return register_class(wc, wc->lpszClassName, 0);
}
EXPORT ATOM WINAPI RegisterClassExA(const WNDCLASSEX_ *wc) {
  builtin_classes();
  if (!wc || wc->cbSize != sizeof *wc || !wc->lpszClassName) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  WNDCLASSEX_ copy = *wc;
  WCHAR *name = wide_of(wc->lpszClassName);
  copy.lpszClassName = name;
  const ATOM atom = register_class(&copy, name, 1);
  wide_free(name, wc->lpszClassName);
  return atom;
}
static void class_ex_of(WNDCLASSEX_ *out, const WNDCLASS_ *in) {
  out->cbSize = sizeof *out; out->style = in->style; out->lpfnWndProc = in->lpfnWndProc; out->cbClsExtra = in->cbClsExtra; out->cbWndExtra = in->cbWndExtra; out->hInstance = in->hInstance;
  out->hIcon = in->hIcon; out->hCursor = in->hCursor; out->hbrBackground = in->hbrBackground; out->lpszMenuName = in->lpszMenuName; out->lpszClassName = in->lpszClassName; out->hIconSm = NULL;
}
EXPORT ATOM WINAPI RegisterClassW(const WNDCLASS_ *wc) { WNDCLASSEX_ ex; if (!wc) { SetLastError(ERROR_INVALID_PARAMETER); return 0; } class_ex_of(&ex, wc); return RegisterClassExW(&ex); }
EXPORT ATOM WINAPI RegisterClassA(const WNDCLASS_ *wc) { WNDCLASSEX_ ex; if (!wc) { SetLastError(ERROR_INVALID_PARAMETER); return 0; } class_ex_of(&ex, wc); return RegisterClassExA(&ex); }

EXPORT BOOL WINAPI UnregisterClassW(const WCHAR *name, HANDLE instance) {
  UNICODE_STRING us;
  builtin_classes();
  if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  us_name(&us, name);
  return (NtUserUnregisterClass((ULONG_PTR)&us, (ULONG_PTR)instance, 0) & 0xFFFFFFFFu) != 0;
}
EXPORT BOOL WINAPI UnregisterClassA(const char *name, HANDLE instance) {
  WCHAR *w = wide_of(name);
  const BOOL r = name ? UnregisterClassW(w, instance) : (SetLastError(ERROR_INVALID_PARAMETER), FALSE);
  wide_free(w, name);
  return r;
}
static BOOL class_info(HANDLE instance, const WCHAR *name, WNDCLASSEX_ *wc, BOOL ansi) {
  UNICODE_STRING us;
  ULONG_PTR menu = 0;
  builtin_classes();
  if (!name || !wc) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  us_name(&us, name);
  wc->lpszClassName = name;
  wc->lpszMenuName = NULL;
  const ATOM atom = (ATOM)NtUserGetClassInfoEx((ULONG_PTR)instance, (ULONG_PTR)&us, (ULONG_PTR)wc, (ULONG_PTR)&menu, ansi);
  if (!atom) return FALSE;
  wc->lpszMenuName = (const void *)menu;
  return TRUE;
}
EXPORT BOOL WINAPI GetClassInfoExW(HANDLE instance, const WCHAR *name, WNDCLASSEX_ *wc) { if (wc && wc->cbSize != sizeof *wc) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; } return class_info(instance, name, wc, FALSE); }
EXPORT BOOL WINAPI GetClassInfoExA(HANDLE instance, const char *name, WNDCLASSEX_ *wc) {
  if (wc && wc->cbSize != sizeof *wc) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR *w = wide_of(name);
  const BOOL r = name ? class_info(instance, w, wc, TRUE) : (SetLastError(ERROR_INVALID_PARAMETER), FALSE);
  if (r) wc->lpszClassName = name;
  wide_free(w, name);
  return r;
}
EXPORT BOOL WINAPI GetClassInfoW(HANDLE instance, const WCHAR *name, WNDCLASS_ *wc) {
  WNDCLASSEX_ ex;
  ex.cbSize = sizeof ex;
  if (!wc || !class_info(instance, name, &ex, FALSE)) return FALSE;
  wc->style = ex.style; wc->lpfnWndProc = ex.lpfnWndProc; wc->cbClsExtra = ex.cbClsExtra; wc->cbWndExtra = ex.cbWndExtra; wc->hInstance = ex.hInstance; wc->hIcon = ex.hIcon;
  wc->hCursor = ex.hCursor; wc->hbrBackground = ex.hbrBackground; wc->lpszMenuName = ex.lpszMenuName; wc->lpszClassName = ex.lpszClassName;
  return TRUE;
}
EXPORT BOOL WINAPI GetClassInfoA(HANDLE instance, const char *name, WNDCLASS_ *wc) {
  WNDCLASSEX_ ex;
  ex.cbSize = sizeof ex;
  if (!wc || !GetClassInfoExA(instance, name, &ex)) return FALSE;
  wc->style = ex.style; wc->lpfnWndProc = ex.lpfnWndProc; wc->cbClsExtra = ex.cbClsExtra; wc->cbWndExtra = ex.cbWndExtra; wc->hInstance = ex.hInstance; wc->hIcon = ex.hIcon;
  wc->hCursor = ex.hCursor; wc->hbrBackground = ex.hbrBackground; wc->lpszMenuName = ex.lpszMenuName; wc->lpszClassName = ex.lpszClassName;
  return TRUE;
}
EXPORT int WINAPI GetClassNameW(HANDLE hwnd, LPWSTR buffer, int count) {
  UNICODE_STRING us;
  if (count <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  us.Length = 0; us.MaximumLength = (USHORT)(count > 32767 ? 65534 : count * 2); us.Buffer = buffer;
  return (int)(NtUserGetClassName((ULONG_PTR)hwnd, 0, (ULONG_PTR)&us) & 0xFFFFFFFFu);
}
EXPORT int WINAPI GetClassNameA(HANDLE hwnd, LPSTR buffer, int count) {
  WCHAR wide[260];
  if (count <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  const int n = GetClassNameW(hwnd, wide, 260);
  if (!n) return 0;
  const int bytes = WideCharToMultiByte(65001, 0, wide, n, buffer, count - 1, NULL, NULL);
  buffer[bytes] = 0;
  return bytes;
}
static ULONG_PTR class_get(HANDLE hwnd, int index, int code) { return NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)code); }
EXPORT DWORD WINAPI GetClassLongW(HANDLE hwnd, int index) { return (DWORD)class_get(hwnd, index, CHP_GET_CLASS_LONG_W); }
EXPORT DWORD WINAPI GetClassLongA(HANDLE hwnd, int index) { return (DWORD)class_get(hwnd, index, CHP_GET_CLASS_LONG_A); }
EXPORT ULONG_PTR WINAPI GetClassLongPtrW(HANDLE hwnd, int index) { return class_get(hwnd, index, CHP_GET_CLASS_LONG_PTR_W); }
EXPORT ULONG_PTR WINAPI GetClassLongPtrA(HANDLE hwnd, int index) { return class_get(hwnd, index, CHP_GET_CLASS_LONG_PTR_A); }
EXPORT WORD WINAPI GetClassWord(HANDLE hwnd, int index) { return (WORD)class_get(hwnd, index, CHP_GET_CLASS_WORD); }
EXPORT DWORD WINAPI SetClassLongW(HANDLE hwnd, int index, LONG value) { return (DWORD)NtUserSetClassLong((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)(DWORD)value, 0); }
EXPORT DWORD WINAPI SetClassLongA(HANDLE hwnd, int index, LONG value) { return (DWORD)NtUserSetClassLong((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)(DWORD)value, 1); }
EXPORT ULONG_PTR WINAPI SetClassLongPtrW(HANDLE hwnd, int index, LONG_PTR value) { return NtUserSetClassLongPtr((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)value, 0); }
EXPORT ULONG_PTR WINAPI SetClassLongPtrA(HANDLE hwnd, int index, LONG_PTR value) { return NtUserSetClassLongPtr((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)value, 1); }
EXPORT WORD WINAPI SetClassWord(HANDLE hwnd, int index, WORD value) { return (WORD)NtUserSetClassWord((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, value); }

EXPORT UINT WINAPI RegisterWindowMessageW(const WCHAR *name) {
  UNICODE_STRING us;
  if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  us_name(&us, name);
  return (UINT)NtUserRegisterWindowMessage((ULONG_PTR)&us);
}
EXPORT UINT WINAPI RegisterWindowMessageA(const char *name) {
  if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR *w = wide_of(name);
  const UINT r = RegisterWindowMessageW(w);
  wide_free(w, name);
  return r;
}

/* ---- windows ------------------------------------------------------------------------------------------------------------------------------------------------------ */
#define CW_USEDEFAULT_ ((int)0x80000000)
static HANDLE create_window(DWORD ex_style, const WCHAR *class_name, const WCHAR *title, DWORD style, int x, int y, int cx, int cy, HANDLE parent, HANDLE menu, HANDLE instance, void *params, BOOL ansi) {
  UNICODE_STRING class_us, title_us, version_us = {0, 0, NULL};
  builtin_classes();
  if (!class_name) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  us_name(&class_us, class_name);
  if (title) us_name(&title_us, title);
  if (!instance) instance = GetModuleHandleW(NULL);
  return (HANDLE)NtUserCreateWindowEx(ex_style, (ULONG_PTR)&class_us, (ULONG_PTR)&version_us, title ? (ULONG_PTR)&title_us : 0, style, (ULONG_PTR)(LONG_PTR)x, (ULONG_PTR)(LONG_PTR)y,
                                       (ULONG_PTR)(LONG_PTR)cx, (ULONG_PTR)(LONG_PTR)cy, (ULONG_PTR)parent, (ULONG_PTR)menu, (ULONG_PTR)instance, (ULONG_PTR)params, 0, (ULONG_PTR)instance,
                                       (ULONG_PTR)class_name, ansi);
}
EXPORT HANDLE WINAPI CreateWindowExW(DWORD ex_style, const WCHAR *class_name, const WCHAR *title, DWORD style, int x, int y, int cx, int cy, HANDLE parent, HANDLE menu, HANDLE instance, void *params) {
  return create_window(ex_style, class_name, title, style, x, y, cx, cy, parent, menu, instance, params, FALSE);
}
EXPORT HANDLE WINAPI CreateWindowExA(DWORD ex_style, const char *class_name, const char *title, DWORD style, int x, int y, int cx, int cy, HANDLE parent, HANDLE menu, HANDLE instance, void *params) {
  WCHAR *c = wide_of(class_name), *t = wide_of(title);
  const HANDLE h = create_window(ex_style, c, t, style, x, y, cx, cy, parent, menu, instance, params, TRUE);
  wide_free(c, class_name); wide_free(t, title);
  return h;
}
EXPORT BOOL WINAPI DestroyWindow(HANDLE hwnd) { return (NtUserDestroyWindow((ULONG_PTR)hwnd) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI IsWindowVisible(HANDLE hwnd) { return (NtUserCallHwnd((ULONG_PTR)hwnd, CH_IS_VISIBLE) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI IsWindowEnabled(HANDLE hwnd) { return (NtUserCallHwnd((ULONG_PTR)hwnd, CH_IS_ENABLED) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI IsWindowUnicode(HANDLE hwnd) { return (NtUserCallHwnd((ULONG_PTR)hwnd, CH_IS_UNICODE) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI EnableWindow(HANDLE hwnd, BOOL enable) { return (NtUserEnableWindow((ULONG_PTR)hwnd, enable) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI ShowWindow(HANDLE hwnd, int cmd) { return (NtUserShowWindow((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)cmd) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI ShowWindowAsync(HANDLE hwnd, int cmd) { return (NtUserShowWindowAsync((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)cmd) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI SetWindowPos(HANDLE hwnd, HANDLE after, int x, int y, int cx, int cy, UINT flags) {
  return (NtUserSetWindowPos((ULONG_PTR)hwnd, (ULONG_PTR)after, (ULONG_PTR)(LONG_PTR)x, (ULONG_PTR)(LONG_PTR)y, (ULONG_PTR)(LONG_PTR)cx, (ULONG_PTR)(LONG_PTR)cy, flags) & 0xFFFFFFFFu) != 0;
}
EXPORT BOOL WINAPI MoveWindow(HANDLE hwnd, int x, int y, int cx, int cy, BOOL repaint) {
  return (NtUserMoveWindow((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)x, (ULONG_PTR)(LONG_PTR)y, (ULONG_PTR)(LONG_PTR)cx, (ULONG_PTR)(LONG_PTR)cy, repaint) & 0xFFFFFFFFu) != 0;
}
EXPORT HANDLE WINAPI GetParent(HANDLE hwnd) { return (HANDLE)NtUserCallHwnd((ULONG_PTR)hwnd, CH_GET_PARENT); }
EXPORT HANDLE WINAPI SetParent(HANDLE hwnd, HANDLE parent) { return (HANDLE)NtUserSetParent((ULONG_PTR)hwnd, (ULONG_PTR)parent); }
EXPORT HANDLE WINAPI GetAncestor(HANDLE hwnd, UINT type) { return (HANDLE)NtUserGetAncestor((ULONG_PTR)hwnd, type); }
EXPORT HANDLE WINAPI GetWindow(HANDLE hwnd, UINT relation) { return (HANDLE)NtUserCallHwndParam((ULONG_PTR)hwnd, relation, CHP_GET_WINDOW_RELATIVE); }
EXPORT HANDLE WINAPI GetTopWindow(HANDLE hwnd) { return GetWindow(hwnd ? hwnd : GetDesktopWindow(), 5 /* GW_CHILD */); }
EXPORT BOOL WINAPI IsChild(HANDLE parent, HANDLE hwnd) { return (NtUserCallHwndParam((ULONG_PTR)parent, (ULONG_PTR)hwnd, CHP_IS_CHILD) & 0xFFFFFFFFu) != 0; }
EXPORT DWORD WINAPI GetWindowThreadProcessId(HANDLE hwnd, LPDWORD process) { return (DWORD)NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)process, CHP_GET_WINDOW_THREAD); }
EXPORT LONG WINAPI GetWindowLongW(HANDLE hwnd, int index) { return (LONG)NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, CHP_GET_WINDOW_LONG_W); }
EXPORT LONG WINAPI GetWindowLongA(HANDLE hwnd, int index) { return (LONG)NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, CHP_GET_WINDOW_LONG_A); }
EXPORT LONG_PTR WINAPI GetWindowLongPtrW(HANDLE hwnd, int index) { return (LONG_PTR)NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, CHP_GET_WINDOW_LONG_PTR_W); }
EXPORT LONG_PTR WINAPI GetWindowLongPtrA(HANDLE hwnd, int index) { return (LONG_PTR)NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, CHP_GET_WINDOW_LONG_PTR_A); }
EXPORT WORD WINAPI GetWindowWord(HANDLE hwnd, int index) { return (WORD)NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, CHP_GET_WINDOW_WORD); }
EXPORT LONG WINAPI SetWindowLongW(HANDLE hwnd, int index, LONG value) { return (LONG)NtUserSetWindowLong((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)(DWORD)value, 0); }
EXPORT LONG WINAPI SetWindowLongA(HANDLE hwnd, int index, LONG value) { return (LONG)NtUserSetWindowLong((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)(DWORD)value, 1); }
EXPORT LONG_PTR WINAPI SetWindowLongPtrW(HANDLE hwnd, int index, LONG_PTR value) { return (LONG_PTR)NtUserSetWindowLongPtr((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)value, 0); }
EXPORT LONG_PTR WINAPI SetWindowLongPtrA(HANDLE hwnd, int index, LONG_PTR value) { return (LONG_PTR)NtUserSetWindowLongPtr((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)value, 1); }
EXPORT WORD WINAPI SetWindowWord(HANDLE hwnd, int index, WORD value) { return (WORD)NtUserSetWindowWord((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, value); }
EXPORT BOOL WINAPI ClientToScreen(HANDLE hwnd, POINT_ *p) { return p && (NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)p, CHP_CLIENT_TO_SCREEN) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI ScreenToClient(HANDLE hwnd, POINT_ *p) { return p && (NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)p, CHP_SCREEN_TO_CLIENT) & 0xFFFFFFFFu) != 0; }
EXPORT int WINAPI MapWindowPoints(HANDLE from, HANDLE to, POINT_ *points, UINT count) {
  struct { HANDLE to; POINT_ *points; UINT count, dpi; } params = {to, points, count, 0};
  return (int)NtUserCallHwndParam((ULONG_PTR)from, (ULONG_PTR)&params, CHP_MAP_WINDOW_POINTS);
}
EXPORT HANDLE WINAPI WindowFromPoint(POINT_ p) { return (HANDLE)NtUserWindowFromPoint((ULONG_PTR)(LONG_PTR)p.x, (ULONG_PTR)(LONG_PTR)p.y); }
EXPORT BOOL WINAPI GetWindowInfo(HANDLE hwnd, void *info) { return (NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)info, CHP_GET_WINDOW_INFO) & 0xFFFFFFFFu) != 0; }

static HANDLE find_window(HANDLE parent, HANDLE child, const WCHAR *class_name, const WCHAR *title) {
  UNICODE_STRING class_us, title_us;
  builtin_classes();
  if (class_name) us_name(&class_us, class_name);
  if (title) us_name(&title_us, title);
  return (HANDLE)NtUserFindWindowEx((ULONG_PTR)parent, (ULONG_PTR)child, class_name ? (ULONG_PTR)&class_us : 0, title ? (ULONG_PTR)&title_us : 0, 0);
}
EXPORT HANDLE WINAPI FindWindowExW(HANDLE parent, HANDLE child, const WCHAR *class_name, const WCHAR *title) { return find_window(parent, child, class_name, title); }
EXPORT HANDLE WINAPI FindWindowW(const WCHAR *class_name, const WCHAR *title) { return find_window(NULL, NULL, class_name, title); }
EXPORT HANDLE WINAPI FindWindowExA(HANDLE parent, HANDLE child, const char *class_name, const char *title) {
  WCHAR *c = wide_of(class_name), *t = wide_of(title);
  const HANDLE h = find_window(parent, child, c, t);
  wide_free(c, class_name); wide_free(t, title);
  return h;
}
EXPORT HANDLE WINAPI FindWindowA(const char *class_name, const char *title) { return FindWindowExA(NULL, NULL, class_name, title); }

/* The text of a window: kept by the window manager; the A functions convert. */
EXPORT int WINAPI GetWindowTextLengthW(HANDLE hwnd) { return (int)NtUserMessageCall((ULONG_PTR)hwnd, 0x000E /* WM_GETTEXTLENGTH */, 0, 0, 0, MC_SEND_MESSAGE, 0); }
EXPORT int WINAPI GetWindowTextLengthA(HANDLE hwnd) { return (int)NtUserMessageCall((ULONG_PTR)hwnd, 0x000E, 0, 0, 0, MC_SEND_MESSAGE, 1); }
EXPORT int WINAPI GetWindowTextW(HANDLE hwnd, LPWSTR buffer, int count) {
  if (!buffer || count <= 0) return 0;
  buffer[0] = 0;
  return (int)NtUserMessageCall((ULONG_PTR)hwnd, 0x000D /* WM_GETTEXT */, (ULONG_PTR)count, (ULONG_PTR)buffer, 0, MC_SEND_MESSAGE, 0);
}
EXPORT int WINAPI GetWindowTextA(HANDLE hwnd, LPSTR buffer, int count) {
  if (!buffer || count <= 0) return 0;
  buffer[0] = 0;
  return (int)NtUserMessageCall((ULONG_PTR)hwnd, 0x000D, (ULONG_PTR)count, (ULONG_PTR)buffer, 0, MC_SEND_MESSAGE, 1);
}
EXPORT BOOL WINAPI SetWindowTextW(HANDLE hwnd, LPCWSTR text) { return (NtUserMessageCall((ULONG_PTR)hwnd, 0x000C /* WM_SETTEXT */, 0, (ULONG_PTR)text, 0, MC_SEND_MESSAGE, 0) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI SetWindowTextA(HANDLE hwnd, LPCSTR text) { return (NtUserMessageCall((ULONG_PTR)hwnd, 0x000C, 0, (ULONG_PTR)text, 0, MC_SEND_MESSAGE, 1) & 0xFFFFFFFFu) != 0; }

/* Properties (SetProp ...): the name is a string or an atom. */
EXPORT BOOL WINAPI SetPropW(HANDLE hwnd, LPCWSTR name, HANDLE data) { return (NtUserSetProp((ULONG_PTR)hwnd, (ULONG_PTR)name, (ULONG_PTR)data) & 0xFFFFFFFFu) != 0; }
EXPORT HANDLE WINAPI GetPropW(HANDLE hwnd, LPCWSTR name) { return (HANDLE)NtUserGetProp((ULONG_PTR)hwnd, (ULONG_PTR)name); }
EXPORT HANDLE WINAPI RemovePropW(HANDLE hwnd, LPCWSTR name) { return (HANDLE)NtUserRemoveProp((ULONG_PTR)hwnd, (ULONG_PTR)name); }
EXPORT BOOL WINAPI SetPropA(HANDLE hwnd, LPCSTR name, HANDLE data) { WCHAR *w = wide_of(name); const BOOL r = SetPropW(hwnd, w, data); wide_free(w, name); return r; }
EXPORT HANDLE WINAPI GetPropA(HANDLE hwnd, LPCSTR name) { WCHAR *w = wide_of(name); const HANDLE r = GetPropW(hwnd, w); wide_free(w, name); return r; }
EXPORT HANDLE WINAPI RemovePropA(HANDLE hwnd, LPCSTR name) { WCHAR *w = wide_of(name); const HANDLE r = RemovePropW(hwnd, w); wide_free(w, name); return r; }

/* Focus, activation, capture */
EXPORT HANDLE WINAPI GetFocus(void) { return (HANDLE)NtUserGetThreadState(0); }
EXPORT HANDLE WINAPI GetActiveWindow(void) { return (HANDLE)NtUserGetThreadState(1); }
EXPORT HANDLE WINAPI GetCapture(void) { return (HANDLE)NtUserGetThreadState(2); }
EXPORT HANDLE WINAPI GetForegroundWindow(void) { return (HANDLE)NtUserGetForegroundWindow(); }
EXPORT HANDLE WINAPI SetFocus(HANDLE hwnd) { return (HANDLE)NtUserSetFocus((ULONG_PTR)hwnd); }
EXPORT HANDLE WINAPI SetActiveWindow(HANDLE hwnd) { return (HANDLE)NtUserSetActiveWindow((ULONG_PTR)hwnd); }
EXPORT BOOL WINAPI SetForegroundWindow(HANDLE hwnd) { return (NtUserSetForegroundWindow((ULONG_PTR)hwnd) & 0xFFFFFFFFu) != 0; }
EXPORT HANDLE WINAPI SetCapture(HANDLE hwnd) { return (HANDLE)NtUserSetCapture((ULONG_PTR)hwnd); }
EXPORT BOOL WINAPI ReleaseCapture(void) { return (NtUserReleaseCapture() & 0xFFFFFFFFu) != 0; }

/* Painting */
EXPORT HANDLE WINAPI BeginPaint(HANDLE hwnd, void *paint) { return (HANDLE)NtUserBeginPaint((ULONG_PTR)hwnd, (ULONG_PTR)paint); }
EXPORT BOOL WINAPI EndPaint(HANDLE hwnd, const void *paint) { return (NtUserEndPaint((ULONG_PTR)hwnd, (ULONG_PTR)paint) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI InvalidateRect(HANDLE hwnd, const void *rect, BOOL erase) { return (NtUserInvalidateRect((ULONG_PTR)hwnd, (ULONG_PTR)rect, erase) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI ValidateRect(HANDLE hwnd, const void *rect) { return (NtUserValidateRect((ULONG_PTR)hwnd, (ULONG_PTR)rect) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI UpdateWindow(HANDLE hwnd) { return (NtUserUpdateWindow((ULONG_PTR)hwnd) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI GetUpdateRect(HANDLE hwnd, void *rect, BOOL erase) { return (NtUserGetUpdateRect((ULONG_PTR)hwnd, (ULONG_PTR)rect, erase) & 0xFFFFFFFFu) != 0; }

/* ---- messages ------------------------------------------------------------------------------------------------------------------------------------------------------ */
EXPORT LONG_PTR WINAPI DefWindowProcW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return (LONG_PTR)NtUserMessageCall((ULONG_PTR)hwnd, message, wparam, (ULONG_PTR)lparam, 0, MC_DEF_WINDOW_PROC, 0); }
EXPORT LONG_PTR WINAPI DefWindowProcA(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return (LONG_PTR)NtUserMessageCall((ULONG_PTR)hwnd, message, wparam, (ULONG_PTR)lparam, 0, MC_DEF_WINDOW_PROC, 1); }
/* The procedure is called directly (Wine's WINPROC handles of ANSI/Unicode conversion are not done). */
EXPORT LONG_PTR WINAPI CallWindowProcW(WNDPROC_ proc, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return proc(hwnd, message, wparam, lparam); }
EXPORT LONG_PTR WINAPI CallWindowProcA(WNDPROC_ proc, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return proc(hwnd, message, wparam, lparam); }
EXPORT LONG_PTR WINAPI SendMessageW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return (LONG_PTR)NtUserMessageCall((ULONG_PTR)hwnd, message, wparam, (ULONG_PTR)lparam, 0, MC_SEND_MESSAGE, 0); }
EXPORT LONG_PTR WINAPI SendMessageA(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return (LONG_PTR)NtUserMessageCall((ULONG_PTR)hwnd, message, wparam, (ULONG_PTR)lparam, 0, MC_SEND_MESSAGE, 1); }
static LONG_PTR send_timeout(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam, UINT flags, UINT timeout, ULONG_PTR *result, BOOL ansi) {
  struct { UINT flags, timeout; ULONG_PTR result; } info = {flags, timeout, 0};
  if (result) *result = 0;
  const ULONG_PTR ok = NtUserMessageCall((ULONG_PTR)hwnd, message, wparam, (ULONG_PTR)lparam, (ULONG_PTR)&info, MC_SEND_MESSAGE_TIMEOUT, ansi);
  if (result) *result = info.result;
  return (LONG_PTR)ok;
}
EXPORT LONG_PTR WINAPI SendMessageTimeoutW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam, UINT flags, UINT timeout, ULONG_PTR *result) { return send_timeout(hwnd, message, wparam, lparam, flags, timeout, result, FALSE); }
EXPORT LONG_PTR WINAPI SendMessageTimeoutA(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam, UINT flags, UINT timeout, ULONG_PTR *result) { return send_timeout(hwnd, message, wparam, lparam, flags, timeout, result, TRUE); }
EXPORT BOOL WINAPI SendNotifyMessageW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return (NtUserMessageCall((ULONG_PTR)hwnd, message, wparam, (ULONG_PTR)lparam, 0, MC_SEND_NOTIFY, 0) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI SendNotifyMessageA(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return (NtUserMessageCall((ULONG_PTR)hwnd, message, wparam, (ULONG_PTR)lparam, 0, MC_SEND_NOTIFY, 1) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI PostMessageW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return (NtUserPostMessage((ULONG_PTR)hwnd, message, wparam, (ULONG_PTR)lparam) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI PostMessageA(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return PostMessageW(hwnd, message, wparam, lparam); }
EXPORT BOOL WINAPI PostThreadMessageW(DWORD thread, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return (NtUserPostThreadMessage(thread, message, wparam, (ULONG_PTR)lparam) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI PostThreadMessageA(DWORD thread, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return PostThreadMessageW(thread, message, wparam, lparam); }
EXPORT void WINAPI PostQuitMessage(int code) { NtUserPostQuitMessage((ULONG_PTR)(LONG_PTR)code); }
EXPORT BOOL WINAPI ReplyMessage(LONG_PTR result) { return (NtUserReplyMessage((ULONG_PTR)result) & 0xFFFFFFFFu) != 0; }
EXPORT int WINAPI GetMessageW(MSG_ *msg, HANDLE hwnd, UINT first, UINT last) {
  if (!msg) { SetLastError(ERROR_INVALID_PARAMETER); return -1; }
  return (int)(LONG)NtUserGetMessage((ULONG_PTR)msg, (ULONG_PTR)hwnd, first, last);
}
EXPORT int WINAPI GetMessageA(MSG_ *msg, HANDLE hwnd, UINT first, UINT last) { return GetMessageW(msg, hwnd, first, last); }
EXPORT BOOL WINAPI PeekMessageW(MSG_ *msg, HANDLE hwnd, UINT first, UINT last, UINT flags) {
  if (!msg) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return (NtUserPeekMessage((ULONG_PTR)msg, (ULONG_PTR)hwnd, first, last, flags) & 0xFFFFFFFFu) != 0;
}
EXPORT BOOL WINAPI PeekMessageA(MSG_ *msg, HANDLE hwnd, UINT first, UINT last, UINT flags) { return PeekMessageW(msg, hwnd, first, last, flags); }
EXPORT BOOL WINAPI WaitMessage(void) { return (NtUserWaitMessage() & 0xFFFFFFFFu) != 0; }
EXPORT LONG_PTR WINAPI DispatchMessageW(const MSG_ *msg) { return msg ? (LONG_PTR)NtUserDispatchMessage((ULONG_PTR)msg) : 0; }
EXPORT LONG_PTR WINAPI DispatchMessageA(const MSG_ *msg) { return DispatchMessageW(msg); }
EXPORT BOOL WINAPI TranslateMessage(const MSG_ *msg) { return msg && (NtUserTranslateMessage((ULONG_PTR)msg, 0) & 0xFFFFFFFFu) != 0; }
EXPORT DWORD WINAPI GetMessagePos(void) { return (DWORD)NtUserGetMessagePos(); }
EXPORT DWORD WINAPI GetQueueStatus(UINT flags) { return (DWORD)NtUserGetQueueStatus(flags); }
EXPORT DWORD WINAPI MsgWaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, DWORD timeout, DWORD mask, DWORD flags) { return (DWORD)NtUserMsgWaitForMultipleObjectsEx(count, (ULONG_PTR)handles, timeout, mask, flags); }
EXPORT DWORD WINAPI MsgWaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL wait_all, DWORD timeout, DWORD mask) { return MsgWaitForMultipleObjectsEx(count, handles, timeout, mask, wait_all ? 1 : 0); }
EXPORT ULONG_PTR WINAPI SetTimer(HANDLE hwnd, ULONG_PTR id, UINT elapse, void *proc) { return NtUserSetTimer((ULONG_PTR)hwnd, id, elapse, (ULONG_PTR)proc, 0); }
EXPORT BOOL WINAPI KillTimer(HANDLE hwnd, ULONG_PTR id) { return (NtUserKillTimer((ULONG_PTR)hwnd, id) & 0xFFFFFFFFu) != 0; }

/* The window rectangle for a client rectangle (AdjustWindowRectEx): the metrics are the window manager's. */
EXPORT BOOL WINAPI AdjustWindowRectEx(RECT_ *rect, DWORD style, BOOL menu, DWORD ex_style) {
  struct { DWORD style, ex_style, menu, dpi; } params = {style, ex_style, menu, 0};
  if (!rect) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return (NtUserCallTwoParam((ULONG_PTR)rect, (ULONG_PTR)&params, 7 /* AdjustWindowRect */) & 0xFFFFFFFFu) != 0;
}
EXPORT BOOL WINAPI AdjustWindowRect(RECT_ *rect, DWORD style, BOOL menu) { return AdjustWindowRectEx(rect, style, menu, 0); }
