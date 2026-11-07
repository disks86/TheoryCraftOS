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
SVC NtUserCallNoParam(ULONG_PTR code);
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
SVC NtUserGetAtomName(ULONG_PTR atom, ULONG_PTR name);
SVC NtUserWindowFromPoint(ULONG_PTR x, ULONG_PTR y);
SVC NtUserSetProp(ULONG_PTR hwnd, ULONG_PTR name, ULONG_PTR data);
SVC NtUserGetProp(ULONG_PTR hwnd, ULONG_PTR name);
SVC NtUserRemoveProp(ULONG_PTR hwnd, ULONG_PTR name);
IMPORT int WINAPI MultiByteToWideChar(UINT code_page, DWORD flags, LPCSTR source, int source_length, LPWSTR destination, int destination_length);
IMPORT int WINAPI WideCharToMultiByte(UINT code_page, DWORD flags, LPCWSTR source, int source_length, LPSTR destination, int destination_length, LPCSTR default_char, LPBOOL used_default);
IMPORT HANDLE WINAPI GetModuleHandleW(LPCWSTR name);
IMPORT BOOL WINAPI IsBadWritePtr(void *p, SIZE_T length);
IMPORT HANDLE WINAPI GlobalAlloc(UINT flags, SIZE_T bytes);
IMPORT HANDLE WINAPI GlobalFree(HANDLE memory);
IMPORT void *WINAPI GlobalLock(HANDLE memory);
IMPORT BOOL WINAPI GlobalUnlock(HANDLE memory);
IMPORT SIZE_T WINAPI GlobalSize(HANDLE memory);
IMPORT HANDLE WINAPI FindResourceW(HANDLE module, LPCWSTR name, LPCWSTR type);
IMPORT HANDLE WINAPI LoadResource(HANDLE module, HANDLE resource);
IMPORT void *WINAPI LockResource(HANDLE resource);
IMPORT DWORD WINAPI SizeofResource(HANDLE module, HANDLE resource);
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

EXPORT LONG WINAPI GetWindowLongW(HANDLE hwnd, int index);

/* ---- ANSI and Unicode ------------------------------------------------------------------------------------------------------------------------------------------------ *
 * The window manager only knows Unicode: every window is a Unicode window and the messages carry Unicode text. The A functions convert what they send (a_to_w) and a window procedure
 * registered with RegisterClassA / SetWindowLongA is held behind a thunk (one of a fixed set of functions, each bound to the program's procedure) that converts what it receives (w_to_a).
 * Only the messages that carry text or characters are converted: WM_SETTEXT, WM_GETTEXT, WM_GETTEXTLENGTH, WM_CREATE / WM_NCCREATE (CREATESTRUCT) and the character messages. The A
 * getters answer the program's own procedure, never the thunk. */
typedef LONG_PTR (*MsgFn)(void *ctx, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam);
static int is_char_message(UINT m) { return m == 0x102 || m == 0x103 || m == 0x106 || m == 0x107 || m == 0x120 || m == 0x286; }
static ULONG_PTR char_to_wide(ULONG_PTR w) {
  char bytes[2];
  WCHAR wide[2];
  int n = 1;
  if (w > 0xFF) { bytes[0] = (char)(w >> 8); bytes[1] = (char)w; n = 2; } else bytes[0] = (char)w;
  return MultiByteToWideChar(65001, 0, bytes, n, wide, 2) >= 1 ? (w & ~(ULONG_PTR)0xFFFF) | wide[0] : w;
}
static ULONG_PTR char_to_ansi(ULONG_PTR w) {
  WCHAR wide = (WCHAR)w;
  char bytes[8];
  const int n = WideCharToMultiByte(65001, 0, &wide, 1, bytes, 8, NULL, NULL);
  if (n == 1) return (w & ~(ULONG_PTR)0xFFFF) | (BYTE)bytes[0];
  if (n == 2) return (w & ~(ULONG_PTR)0xFFFF) | ((BYTE)bytes[0] << 8) | (BYTE)bytes[1];
  return (w & ~(ULONG_PTR)0xFFFF) | '?';
}
static char *narrow_of(const WCHAR *s) {
  if (!s || ((ULONG_PTR)s >> 16) == 0) return (char *)s;
  const int n = WideCharToMultiByte(65001, 0, s, -1, NULL, 0, NULL, NULL);
  char *a = u32_alloc((SIZE_T)n + 1);
  if (a) WideCharToMultiByte(65001, 0, s, -1, a, n, NULL, NULL);
  return a;
}
static void narrow_free(char *a, const WCHAR *original) { if (a && original && ((ULONG_PTR)original >> 16) != 0) u32_free(a); }
typedef struct { ULONG_PTR create_params, instance, menu, parent; int cy, cx, y, x; LONG style; const void *name, *cls; DWORD ex_style; } CreateStruct_;

/* The messages of list and combo boxes that carry a string the caller gives (an owner-drawn box without strings takes the pointer as item data and is left alone). */
static int a_string_in_message(HANDLE hwnd, UINT message) {
  const int list = message == 0x0180 || message == 0x0181 || message == 0x018C || message == 0x018F || message == 0x01A2;      /* LB_ADDSTRING, INSERT, SELECTSTRING, FINDSTRING, FINDSTRINGEXACT */
  const int combo = message == 0x0143 || message == 0x014A || message == 0x014D || message == 0x014C || message == 0x0158;      /* CB_ADDSTRING, INSERT, SELECTSTRING, FINDSTRING, FINDSTRINGEXACT */
  if (!list && !combo) return 0;
  const LONG style = GetWindowLongW(hwnd, -16);
  if (list && (style & 0x30) && !(style & 0x40)) return 0;
  if (combo && (style & 0x10 /* CBS_OWNERDRAWFIXED */ || style & 0x20) && !(style & 0x200 /* CBS_HASSTRINGS */)) return 0;
  return 1;
}

static void *g_mdi_params;      /* WM_MDICREATE from an A caller: the structure the new child gets as its creation parameter */
/* A caller, W callee. */
static LONG_PTR a_to_w(MsgFn fn, void *ctx, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  LONG_PTR result;
  if ((message == 0x000C /* WM_SETTEXT */ || message == 0x00C2 /* EM_REPLACESEL */) && lparam) {
    WCHAR *w = wide_of((const char *)lparam);
    result = fn(ctx, hwnd, message, wparam, (LONG_PTR)w);
    wide_free(w, (const char *)lparam);
    return result;
  }
  if (message == 0x000D /* WM_GETTEXT */ && lparam && wparam) {
    WCHAR *w = u32_alloc((SIZE_T)wparam * 2);
    if (!w) return 0;
    w[0] = 0;
    result = fn(ctx, hwnd, message, wparam, (LONG_PTR)w);
    char *out = (char *)lparam;
    int bytes = 0;
    if (result > 0) bytes = WideCharToMultiByte(65001, 0, w, (int)result, out, (int)wparam - 1, NULL, NULL);
    out[bytes] = 0;
    u32_free(w);
    return bytes;
  }
  if (message == 0x00C4 /* EM_GETLINE: the first word of the buffer is its size */ && lparam) {
    const UINT room = *(WORD *)lparam;
    WCHAR *w = u32_alloc(((SIZE_T)room + 1) * 2);
    if (!w) return 0;
    *(WORD *)w = (WORD)room;
    result = fn(ctx, hwnd, message, wparam, (LONG_PTR)w);
    int bytes = 0;
    if (result > 0) bytes = WideCharToMultiByte(65001, 0, w, (int)result, (char *)lparam, (int)room, NULL, NULL);
    u32_free(w);
    return bytes;
  }
  if (a_string_in_message(hwnd, message) && lparam) {      /* list and combo box strings in */
    WCHAR *w = wide_of((const char *)lparam);
    result = fn(ctx, hwnd, message, wparam, (LONG_PTR)w);
    wide_free(w, (const char *)lparam);
    return result;
  }
  if ((message == 0x0189 /* LB_GETTEXT */ || message == 0x0148 /* CB_GETLBTEXT */) && lparam) {
    const LONG_PTR chars = fn(ctx, hwnd, message == 0x0189 ? 0x018A : 0x0149, wparam, 0);
    if (chars < 0) return chars;
    WCHAR *w = u32_alloc(((SIZE_T)chars + 2) * 2);
    if (!w) return -1;
    result = fn(ctx, hwnd, message, wparam, (LONG_PTR)w);
    int bytes = 0;
    if (result >= 0) { bytes = WideCharToMultiByte(65001, 0, w, (int)result, (char *)lparam, 0x7FFFFFFF, NULL, NULL); ((char *)lparam)[bytes] = 0; }
    u32_free(w);
    return result < 0 ? result : bytes;
  }
  if (message == 0x018A /* LB_GETTEXTLEN */ || message == 0x0149 /* CB_GETLBTEXTLEN */) {
    const LONG_PTR chars = fn(ctx, hwnd, message, wparam, 0);
    if (chars <= 0) return chars;
    WCHAR *w = u32_alloc(((SIZE_T)chars + 2) * 2);
    if (!w) return chars;
    const LONG_PTR got = fn(ctx, hwnd, message == 0x018A ? 0x0189 : 0x0148, wparam, (LONG_PTR)w);
    const int bytes = got > 0 ? WideCharToMultiByte(65001, 0, w, (int)got, NULL, 0, NULL, NULL) : (int)chars;
    u32_free(w);
    return bytes;
  }
  if (message == 0x00CC /* EM_SETPASSWORDCHAR */) wparam = char_to_wide(wparam);
  if (message == 0x000E /* WM_GETTEXTLENGTH */) {
    const LONG_PTR chars = fn(ctx, hwnd, message, 0, 0);
    if (chars <= 0) return chars;
    WCHAR *w = u32_alloc(((SIZE_T)chars + 1) * 2);
    if (!w) return chars;
    w[0] = 0;
    const LONG_PTR got = fn(ctx, hwnd, 0x000D, (ULONG_PTR)chars + 1, (LONG_PTR)w);
    const int bytes = got > 0 ? WideCharToMultiByte(65001, 0, w, (int)got, NULL, 0, NULL, NULL) : 0;
    u32_free(w);
    return bytes;
  }
  if ((message == 0x0001 /* WM_CREATE */ || message == 0x0081 /* WM_NCCREATE */) && lparam) {
    CreateStruct_ *a = (CreateStruct_ *)lparam, w = *a;
    WCHAR *name = wide_of(a->name), *cls = wide_of(a->cls);
    w.name = name; w.cls = cls;
    result = fn(ctx, hwnd, message, wparam, (LONG_PTR)&w);
    w.name = a->name; w.cls = a->cls;
    *a = w;
    wide_free(name, a->name); wide_free(cls, a->cls);
    return result;
  }
  if (message == 0x0220 /* WM_MDICREATE: the MDICREATESTRUCTA's strings; the child still gets the program's structure as its creation parameter */ && lparam) {
    typedef struct { const void *cls, *title; HANDLE owner; int x, y, cx, cy; DWORD style; LONG_PTR lparam; } MdiCs_;
    const MdiCs_ *a = (const MdiCs_ *)lparam;
    MdiCs_ w = *a;
    WCHAR *cls = wide_of((const char *)a->cls), *title = wide_of((const char *)a->title);
    w.cls = cls; w.title = title;
    g_mdi_params = (void *)lparam;
    result = fn(ctx, hwnd, message, wparam, (LONG_PTR)&w);
    g_mdi_params = NULL;
    wide_free(cls, (const char *)a->cls); wide_free(title, (const char *)a->title);
    return result;
  }
  if (is_char_message(message)) wparam = char_to_wide(wparam);
  return fn(ctx, hwnd, message, wparam, lparam);
}

/* W caller, A callee. */
static LONG_PTR w_to_a(MsgFn fn, void *ctx, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  LONG_PTR result;
  if (message == 0x000C && lparam) {
    char *a = narrow_of((const WCHAR *)lparam);
    result = fn(ctx, hwnd, message, wparam, (LONG_PTR)a);
    narrow_free(a, (const WCHAR *)lparam);
    return result;
  }
  if (message == 0x000D && lparam && wparam) {
    char *a = u32_alloc((SIZE_T)wparam * 4 + 4);
    if (!a) return 0;
    a[0] = 0;
    result = fn(ctx, hwnd, message, wparam * 4, (LONG_PTR)a);
    int chars = 0;
    if (result > 0) chars = MultiByteToWideChar(65001, 0, a, (int)result, (WCHAR *)lparam, (int)wparam - 1);
    ((WCHAR *)lparam)[chars] = 0;
    u32_free(a);
    return chars;
  }
  if ((message == 0x0001 || message == 0x0081) && lparam) {
    CreateStruct_ *w = (CreateStruct_ *)lparam, a = *w;
    char *name = narrow_of(w->name), *cls = narrow_of(w->cls);
    a.name = name; a.cls = cls;
    result = fn(ctx, hwnd, message, wparam, (LONG_PTR)&a);
    a.name = w->name; a.cls = w->cls;
    *w = a;
    narrow_free(name, w->name); narrow_free(cls, w->cls);
    return result;
  }
  if (is_char_message(message)) wparam = char_to_ansi(wparam);
  return fn(ctx, hwnd, message, wparam, lparam);
}

#define THUNK_LIST(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) \
  X(30) X(31) X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47) X(48) X(49) X(50) X(51) X(52) X(53) X(54) X(55) X(56) X(57) X(58) X(59) X(60) X(61) X(62) X(63)
#define THUNK_COUNT 64
static WNDPROC_ g_ansi_procs[THUNK_COUNT];      /* the programs' A procedures, one per thunk (0 = free) */
static LONG_PTR call_ansi_proc(void *ctx, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return ((WNDPROC_)ctx)(hwnd, message, wparam, lparam); }
#define DEFINE_THUNK(n) static LONG_PTR WINAPI ansi_thunk_##n(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return w_to_a(call_ansi_proc, (void *)g_ansi_procs[n], hwnd, message, wparam, lparam); }
THUNK_LIST(DEFINE_THUNK)
#define THUNK_ENTRY(n) ansi_thunk_##n,
static WNDPROC_ const g_thunks[THUNK_COUNT] = {THUNK_LIST(THUNK_ENTRY)};
static int thunk_slot(WNDPROC_ proc) { for (int i = 0; i < THUNK_COUNT; ++i) if (g_thunks[i] == proc) return i; return -1; }
/* The thunk for an A procedure (made on first use; a procedure that is already a thunk stays as it is; with no thunk left the procedure itself is used). */
static WNDPROC_ ansi_wrap(WNDPROC_ proc) {
  if (!proc || thunk_slot(proc) >= 0) return proc;
  for (int i = 0; i < THUNK_COUNT; ++i) if (g_ansi_procs[i] == proc) return g_thunks[i];
  for (int i = 0; i < THUNK_COUNT; ++i) {
    WNDPROC_ expected = NULL;
    if (__atomic_compare_exchange_n(&g_ansi_procs[i], &expected, proc, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return g_thunks[i];
  }
  return proc;
}
static ULONG_PTR ansi_unwrap(ULONG_PTR value) { const int slot = thunk_slot((WNDPROC_)value); return slot >= 0 ? (ULONG_PTR)g_ansi_procs[slot] : value; }

EXPORT LONG_PTR WINAPI SendMessageW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam);
EXPORT LONG_PTR WINAPI SendMessageA(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam);
static ATOM register_class(const WNDCLASSEX_ *wc, const WCHAR *name, DWORD flags) {
  UNICODE_STRING us;
  ULONG_PTR menu[3] = {0, 0, 0};
  us_name(&us, name);
  return (ATOM)NtUserRegisterClassExWOW((ULONG_PTR)wc, (ULONG_PTR)&us, 0, (ULONG_PTR)menu, 0, flags, 0);
}

/* The classes of the controls and the dialog: global classes whose procedure is the default one (the control behaviour of Windows is not done: a Button, Static or Edit window is a plain
 * window with its text, style and messages kept by the window manager). Made when the process first registers, creates or looks for a class. */
/* (the controls below use these before they are defined) */
#define BOOL_OF(x) (((x) & 0xFFFFFFFFu) != 0)
EXPORT BOOL WINAPI ShowWindow(HANDLE hwnd, int cmd);
EXPORT BOOL WINAPI OpenClipboard(HANDLE hwnd);
EXPORT BOOL WINAPI CloseClipboard(void);
EXPORT BOOL WINAPI EmptyClipboard(void);
EXPORT HANDLE WINAPI SetClipboardData(UINT format, HANDLE handle);
EXPORT HANDLE WINAPI GetClipboardData(UINT format);
EXPORT LONG_PTR WINAPI DispatchMessageW(const MSG_ *msg);
EXPORT int WINAPI GetMessageW(MSG_ *msg, HANDLE hwnd, UINT first, UINT last);
EXPORT BOOL WINAPI TranslateMessage(const MSG_ *msg);
EXPORT BOOL WINAPI PostMessageW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam);
EXPORT void WINAPI PostQuitMessage(int code);
EXPORT HANDLE WINAPI GetActiveWindow(void);
EXPORT HANDLE WINAPI SetActiveWindow(HANDLE hwnd);
EXPORT BOOL WINAPI IsWindow(HANDLE window);
EXPORT int WINAPI GetWindowTextA(HANDLE hwnd, LPSTR buffer, int count);
EXPORT int WINAPI GetSystemMetrics(int index);
EXPORT BOOL WINAPI GetWindowRect(HANDLE window, RECT_ *rect);
EXPORT BOOL WINAPI EnableWindow(HANDLE hwnd, BOOL enable);
EXPORT BOOL WINAPI PeekMessageW(MSG_ *msg, HANDLE hwnd, UINT first, UINT last, UINT flags);
EXPORT BOOL WINAPI DestroyWindow(HANDLE hwnd);
EXPORT HANDLE WINAPI CreateWindowExW(DWORD ex_style, const WCHAR *class_name, const WCHAR *title, DWORD style, int x, int y, int cx, int cy, HANDLE parent, HANDLE menu, HANDLE instance, void *params);
EXPORT BOOL WINAPI SetWindowPos(HANDLE hwnd, HANDLE after, int x, int y, int cx, int cy, UINT flags);
EXPORT LONG_PTR WINAPI DefWindowProcW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam);
EXPORT SHORT WINAPI GetKeyState(int vk);
EXPORT LONG WINAPI GetWindowLongW(HANDLE hwnd, int index);
EXPORT LONG WINAPI SetWindowLongW(HANDLE hwnd, int index, LONG value);
EXPORT LONG_PTR WINAPI GetWindowLongPtrW(HANDLE hwnd, int index);
EXPORT LONG_PTR WINAPI SetWindowLongPtrW(HANDLE hwnd, int index, LONG_PTR value);
EXPORT HANDLE WINAPI GetParent(HANDLE hwnd);
EXPORT HANDLE WINAPI GetWindow(HANDLE hwnd, UINT relation);
EXPORT HANDLE WINAPI GetFocus(void);
EXPORT HANDLE WINAPI SetFocus(HANDLE hwnd);
EXPORT HANDLE WINAPI SetCapture(HANDLE hwnd);
EXPORT HANDLE WINAPI GetCapture(void);
EXPORT BOOL WINAPI ReleaseCapture(void);
EXPORT HANDLE WINAPI BeginPaint(HANDLE hwnd, void *paint);
EXPORT BOOL WINAPI EndPaint(HANDLE hwnd, const void *paint);
EXPORT BOOL WINAPI InvalidateRect(HANDLE hwnd, const void *rect, BOOL erase);
EXPORT BOOL WINAPI IsWindowVisible(HANDLE hwnd);
EXPORT int WINAPI GetWindowTextW(HANDLE hwnd, LPWSTR buffer, int count);
EXPORT BOOL WINAPI GetClientRect(HANDLE window, RECT_ *rect);
#include "hooks.inc"
#include "controls.inc"
#include "scroll.inc"
#include "scrollwin.inc"
#include "display.inc"
#include "listbox.inc"
#include "combo.inc"
#include "dialog.inc"
#include "menu.inc"
#include "menuui.inc"
static LONG_PTR def_proc_w(void *ctx, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam);
#include "mdi.inc"
static void builtin_classes(void) {
  static volatile LONG done;
  static const struct { const WCHAR *name; UINT style; int wnd_extra; WNDPROC_ proc; } classes[] = {
      {L"Button", 0x8b, 8, button_proc}, {L"ComboBox", 0x8b, 8, combo_proc}, {L"Edit", 0x88, 8, edit_proc}, {L"ListBox", 0x88, 8, listbox_proc}, {L"MDIClient", 0, 0, mdiclient_proc},
      {L"ScrollBar", 0x8b, 8, scrollbar_proc}, {L"Static", 0x88, 8, static_proc}, {L"ComboLBox", 0x808, 8, listbox_proc}, {L"#32768", 0x20808, 0, menu_proc}, {L"#32770", 0x808, 30, DefDlgProcW},
      {L"#32771", 0x803, 0, builtin_proc}, {L"#32772", 0, 0, builtin_proc}, {L"IME", 0, 0, builtin_proc}, {L"SysLink", 0x8, 0, builtin_proc}, {L"tooltips_class32", 0x8, 0, builtin_proc}};
  if (__sync_lock_test_and_set(&done, 1)) return;
  for (size_t_ i = 0; i < sizeof classes / sizeof classes[0]; ++i) {
    WNDCLASSEX_ wc = {sizeof wc, 0x4000 /* CS_GLOBALCLASS */ | classes[i].style, classes[i].proc, 0, classes[i].wnd_extra, GetModuleHandleW(L"user32.dll"), NULL, NULL, NULL, NULL, classes[i].name, NULL};
    register_class(&wc, classes[i].name, 0);
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
  copy.lpfnWndProc = (WNDPROC_)ansi_wrap(wc->lpfnWndProc);
  const ATOM atom = register_class(&copy, name, 0);
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
/* Answers the class atom (nonzero on success, like Windows). The hInstance of the result is the one asked for, except for the classes of user32 itself (0). */
static DWORD class_info(HANDLE instance, const WCHAR *name, WNDCLASSEX_ *wc, BOOL ansi) {
  UNICODE_STRING us;
  ULONG_PTR menu = 0;
  builtin_classes();
  if (!name || !wc) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  us_name(&us, name);
  wc->lpszClassName = name;
  wc->lpszMenuName = NULL;
  const ATOM atom = (ATOM)NtUserGetClassInfoEx((ULONG_PTR)instance, (ULONG_PTR)&us, (ULONG_PTR)wc, (ULONG_PTR)&menu, 0);
  if (atom && ansi) wc->lpfnWndProc = (WNDPROC_)ansi_unwrap((ULONG_PTR)wc->lpfnWndProc);
  if (!atom) return 0;
  wc->lpszMenuName = (const void *)menu;
  wc->hInstance = wc->hInstance == GetModuleHandleW(L"user32.dll") ? NULL : instance;
  return atom;
}
EXPORT DWORD WINAPI GetClassInfoExW(HANDLE instance, const WCHAR *name, WNDCLASSEX_ *wc) { if (wc && wc->cbSize != sizeof *wc) { SetLastError(ERROR_INVALID_PARAMETER); return 0; } return class_info(instance, name, wc, FALSE); }
EXPORT DWORD WINAPI GetClassInfoExA(HANDLE instance, const char *name, WNDCLASSEX_ *wc) {
  if (wc && wc->cbSize != sizeof *wc) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR *w = wide_of(name);
  const DWORD r = name ? class_info(instance, w, wc, TRUE) : (SetLastError(ERROR_INVALID_PARAMETER), 0);
  if (r) wc->lpszClassName = name;
  wide_free(w, name);
  return r;
}
EXPORT DWORD WINAPI GetClassInfoW(HANDLE instance, const WCHAR *name, WNDCLASS_ *wc) {
  WNDCLASSEX_ ex;
  ex.cbSize = sizeof ex;
  const DWORD atom = wc ? class_info(instance, name, &ex, FALSE) : 0;
  if (!atom) return 0;
  wc->style = ex.style; wc->lpfnWndProc = ex.lpfnWndProc; wc->cbClsExtra = ex.cbClsExtra; wc->cbWndExtra = ex.cbWndExtra; wc->hInstance = ex.hInstance; wc->hIcon = ex.hIcon;
  wc->hCursor = ex.hCursor; wc->hbrBackground = ex.hbrBackground; wc->lpszMenuName = ex.lpszMenuName; wc->lpszClassName = ex.lpszClassName;
  return atom;
}
EXPORT DWORD WINAPI GetClassInfoA(HANDLE instance, const char *name, WNDCLASS_ *wc) {
  WNDCLASSEX_ ex;
  ex.cbSize = sizeof ex;
  const DWORD atom = wc ? GetClassInfoExA(instance, name, &ex) : 0;
  if (!atom) return 0;
  wc->style = ex.style; wc->lpfnWndProc = ex.lpfnWndProc; wc->cbClsExtra = ex.cbClsExtra; wc->cbWndExtra = ex.cbWndExtra; wc->hInstance = ex.hInstance; wc->hIcon = ex.hIcon;
  wc->hCursor = ex.hCursor; wc->hbrBackground = ex.hbrBackground; wc->lpszMenuName = ex.lpszMenuName; wc->lpszClassName = ex.lpszClassName;
  return atom;
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
static ULONG_PTR class_get(HANDLE hwnd, int index, int code) {
  const ULONG_PTR v = NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)code);
  const int ansi = code == CHP_GET_CLASS_LONG_A || code == CHP_GET_CLASS_LONG_PTR_A;
  return ansi && index == -24 /* GCL_WNDPROC */ ? ansi_unwrap(v) : v;
}
EXPORT DWORD WINAPI GetClassLongW(HANDLE hwnd, int index) { return (DWORD)class_get(hwnd, index, CHP_GET_CLASS_LONG_W); }
EXPORT DWORD WINAPI GetClassLongA(HANDLE hwnd, int index) { return (DWORD)class_get(hwnd, index, CHP_GET_CLASS_LONG_A); }
EXPORT ULONG_PTR WINAPI GetClassLongPtrW(HANDLE hwnd, int index) { return class_get(hwnd, index, CHP_GET_CLASS_LONG_PTR_W); }
EXPORT ULONG_PTR WINAPI GetClassLongPtrA(HANDLE hwnd, int index) { return class_get(hwnd, index, CHP_GET_CLASS_LONG_PTR_A); }
EXPORT WORD WINAPI GetClassWord(HANDLE hwnd, int index) { return (WORD)class_get(hwnd, index, CHP_GET_CLASS_WORD); }
EXPORT DWORD WINAPI SetClassLongW(HANDLE hwnd, int index, LONG value) { return (DWORD)NtUserSetClassLong((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)(DWORD)value, 0); }
EXPORT DWORD WINAPI SetClassLongA(HANDLE hwnd, int index, LONG value) {
  const ULONG_PTR in = index == -24 ? (ULONG_PTR)ansi_wrap((WNDPROC_)(ULONG_PTR)(DWORD)value) : (ULONG_PTR)(DWORD)value;
  return (DWORD)NtUserSetClassLong((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, in, 0);
}
EXPORT ULONG_PTR WINAPI SetClassLongPtrW(HANDLE hwnd, int index, LONG_PTR value) { return NtUserSetClassLongPtr((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)value, 0); }
EXPORT ULONG_PTR WINAPI SetClassLongPtrA(HANDLE hwnd, int index, LONG_PTR value) {
  const ULONG_PTR in = index == -24 ? (ULONG_PTR)ansi_wrap((WNDPROC_)value) : (ULONG_PTR)value;
  return ansi_unwrap(NtUserSetClassLongPtr((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, in, 0));
}
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
  if ((ex_style & 0x40 /* WS_EX_MDICHILD */) && parent && !g_mdi_creating && is_mdi_client(parent)) return mdi_create_through_client(parent, class_name, title, style, x, y, cx, cy, instance, params, ansi);
  us_name(&class_us, class_name);
  if (title) us_name(&title_us, title);
  /* (an MDI child keeps the owner it was given in its CREATESTRUCT, even none; and no instance finds the process's class of that name whatever module registered it,
     as on Windows: a browser registers its classes from its DLL and creates the windows with no instance) */
  const HANDLE given_instance = instance;
  if (!instance) instance = GetModuleHandleW(NULL);
  return (HANDLE)NtUserCreateWindowEx(ex_style, (ULONG_PTR)&class_us, (ULONG_PTR)&version_us, title ? (ULONG_PTR)&title_us : 0, style, (ULONG_PTR)(LONG_PTR)x, (ULONG_PTR)(LONG_PTR)y,
                                       (ULONG_PTR)(LONG_PTR)cx, (ULONG_PTR)(LONG_PTR)cy, (ULONG_PTR)parent, (ULONG_PTR)menu, (ULONG_PTR)(g_mdi_creating || !given_instance ? given_instance : instance), (ULONG_PTR)params, 0, (ULONG_PTR)instance,
                                       (ULONG_PTR)class_name, 0);
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
EXPORT BOOL WINAPI DestroyWindow(HANDLE hwnd) {
  if (HOOK_ACTIVE(WH_CBT) && IsWindow(hwnd) && hook_cbt(HCBT_DESTROYWND, (ULONG_PTR)hwnd, 0)) return FALSE;
  return (NtUserDestroyWindow((ULONG_PTR)hwnd) & 0xFFFFFFFFu) != 0;
}
EXPORT BOOL WINAPI IsWindowVisible(HANDLE hwnd) { return (NtUserCallHwnd((ULONG_PTR)hwnd, CH_IS_VISIBLE) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI IsWindowEnabled(HANDLE hwnd) { return (NtUserCallHwnd((ULONG_PTR)hwnd, CH_IS_ENABLED) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI IsWindowUnicode(HANDLE hwnd) {      /* (an A window is one whose procedure is a thunk) */
  if (!NtUserCallHwnd((ULONG_PTR)hwnd, CH_IS_WINDOW)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE_); return FALSE; }
  return thunk_slot((WNDPROC_)NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)-4, CHP_GET_WINDOW_LONG_PTR_W)) < 0;
}
EXPORT BOOL WINAPI EnableWindow(HANDLE hwnd, BOOL enable) { return (NtUserEnableWindow((ULONG_PTR)hwnd, enable) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI ShowWindow(HANDLE hwnd, int cmd) {
  if (HOOK_ACTIVE(WH_CBT) && (cmd == 2 || cmd == 3 || cmd == 6 || cmd == 9 || cmd == 7) && hook_cbt(HCBT_MINMAX, (ULONG_PTR)hwnd, cmd)) return FALSE;
  return (NtUserShowWindow((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)cmd) & 0xFFFFFFFFu) != 0; }
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
/* On 64 bits the 32-bit functions cannot hold a pointer: the window procedure, the instance and the parent are refused */
static BOOL long_index_is_pointer(int index) { if (index == -4 || index == -6 || index == -8) { SetLastError(1413 /* ERROR_INVALID_INDEX */); return TRUE; } return FALSE; }
EXPORT LONG WINAPI GetWindowLongW(HANDLE hwnd, int index) { if (long_index_is_pointer(index)) return 0; return (LONG)NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, CHP_GET_WINDOW_LONG_W); }
EXPORT LONG WINAPI GetWindowLongA(HANDLE hwnd, int index) {
  if (long_index_is_pointer(index)) return 0;
  const ULONG_PTR v = NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, CHP_GET_WINDOW_LONG_A);
  return (LONG)(index == -4 /* GWL_WNDPROC */ ? ansi_unwrap(v) : v);
}
EXPORT LONG_PTR WINAPI GetWindowLongPtrW(HANDLE hwnd, int index) { return (LONG_PTR)NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, CHP_GET_WINDOW_LONG_PTR_W); }
EXPORT LONG_PTR WINAPI GetWindowLongPtrA(HANDLE hwnd, int index) {
  const ULONG_PTR v = NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, CHP_GET_WINDOW_LONG_PTR_A);
  return (LONG_PTR)((index == -4 || index == 8 /* DWLP_DLGPROC */) ? ansi_unwrap(v) : v);
}
EXPORT WORD WINAPI GetWindowWord(HANDLE hwnd, int index) { if (long_index_is_pointer(index)) return 0; return (WORD)NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, CHP_GET_WINDOW_WORD); }
/* A window that becomes or stops being layered is drawn again, children included */
static void layered_redraw(HANDLE hwnd, int index, LONG old_style, LONG value) {
  if (index != -20 /* GWL_EXSTYLE */ || !((old_style ^ value) & 0x80000)) return;
  InvalidateRect(hwnd, NULL, TRUE);
  for (HANDLE c = GetWindow(hwnd, 5); c; c = GetWindow(c, 2)) InvalidateRect(c, NULL, TRUE);
}
EXPORT LONG WINAPI SetWindowLongW(HANDLE hwnd, int index, LONG value) {
  if (long_index_is_pointer(index)) return 0;
  const LONG old_style = index == -20 ? GetWindowLongW(hwnd, -20) : 0;
  const LONG r = (LONG)NtUserSetWindowLong((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)(DWORD)value, 0);
  layered_redraw(hwnd, index, old_style, (LONG)value);
  return r;
}
EXPORT LONG WINAPI SetWindowLongA(HANDLE hwnd, int index, LONG value) {
  if (long_index_is_pointer(index)) return 0;
  const LONG old_style = index == -20 ? GetWindowLongW(hwnd, -20) : 0;
  const LONG r = (LONG)NtUserSetWindowLong((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)(DWORD)value, 0);
  layered_redraw(hwnd, index, old_style, (LONG)value);
  return r;
}
EXPORT LONG_PTR WINAPI SetWindowLongPtrW(HANDLE hwnd, int index, LONG_PTR value) { if (index == -4 && value == 0 && IsWindow(hwnd)) return GetWindowLongPtrW(hwnd, index); return (LONG_PTR)NtUserSetWindowLongPtr((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)value, 0); }
EXPORT LONG_PTR WINAPI SetWindowLongPtrA(HANDLE hwnd, int index, LONG_PTR value) {
  if (index == -4 && value == 0 && IsWindow(hwnd)) return GetWindowLongPtrA(hwnd, index);
  const ULONG_PTR in = index == -4 ? (ULONG_PTR)ansi_wrap((WNDPROC_)value) : (ULONG_PTR)value;
  const ULONG_PTR old = NtUserSetWindowLongPtr((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, in, 0);
  return (LONG_PTR)(index == -4 ? ansi_unwrap(old) : old);
}
EXPORT WORD WINAPI SetWindowWord(HANDLE hwnd, int index, WORD value) { if (long_index_is_pointer(index)) return 0; return (WORD)NtUserSetWindowWord((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)index, value); }
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
EXPORT int WINAPI GetWindowTextLengthW(HANDLE hwnd) { return (int)SendMessageW(hwnd, 0x000E /* WM_GETTEXTLENGTH */, 0, 0); }
EXPORT int WINAPI GetWindowTextLengthA(HANDLE hwnd) { return (int)SendMessageA(hwnd, 0x000E, 0, 0); }
EXPORT int WINAPI GetWindowTextW(HANDLE hwnd, LPWSTR buffer, int count) {
  if (!buffer || count <= 0 || IsBadWritePtr(buffer, 2)) return 0;
  buffer[0] = 0;
  return (int)SendMessageW(hwnd, 0x000D /* WM_GETTEXT */, (ULONG_PTR)count, (LONG_PTR)buffer);
}
EXPORT int WINAPI GetWindowTextA(HANDLE hwnd, LPSTR buffer, int count) {
  if (!buffer || count <= 0 || IsBadWritePtr(buffer, 1)) return 0;
  buffer[0] = 0;
  return (int)SendMessageA(hwnd, 0x000D, (ULONG_PTR)count, (LONG_PTR)buffer);
}
EXPORT BOOL WINAPI SetWindowTextW(HANDLE hwnd, LPCWSTR text) { return SendMessageW(hwnd, 0x000C /* WM_SETTEXT */, 0, (LONG_PTR)text) != 0; }
EXPORT BOOL WINAPI SetWindowTextA(HANDLE hwnd, LPCSTR text) { return SendMessageA(hwnd, 0x000C, 0, (LONG_PTR)text) != 0; }

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
EXPORT HANDLE WINAPI SetFocus(HANDLE hwnd) {
  if (HOOK_ACTIVE(WH_CBT) && hwnd && hook_cbt(HCBT_SETFOCUS, (ULONG_PTR)hwnd, (LONG_PTR)GetFocus())) return NULL;
  return (HANDLE)NtUserSetFocus((ULONG_PTR)hwnd);
}
EXPORT HANDLE WINAPI SetActiveWindow(HANDLE hwnd) {
  if (HOOK_ACTIVE(WH_CBT) && hwnd) {
    struct { BOOL mouse; HANDLE active; } cbt = {FALSE, GetActiveWindow()};
    if (hook_cbt(HCBT_ACTIVATE, (ULONG_PTR)hwnd, (LONG_PTR)&cbt)) return NULL;
  }
  return (HANDLE)NtUserSetActiveWindow((ULONG_PTR)hwnd);
}
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
static LONG_PTR def_proc_w(void *ctx, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  (void)ctx;
  LONG_PTR result;
  if (menu_def_before(hwnd, message, wparam, lparam, &result)) return result;
  if (message == 0x0014 /* WM_ERASEBKGND */) {
    /* the class background brush (a COLOR_ index plus one when small) fills the client area */
    ULONG_PTR brush = GetClassLongPtrW(hwnd, -10 /* GCLP_HBRBACKGROUND */);
    if (!brush || !wparam) return 0;
    RECT_ rc;
    GetClientRect(hwnd, &rc);
    FillRect((HANDLE)wparam, &rc, (HANDLE)brush);
    return 1;
  }
  if (message == 0x000F /* WM_PAINT */) {
    /* painting nothing still erases the background and validates */
    ULONG_PTR ps[9] = {0};
    if (BeginPaint(hwnd, ps)) EndPaint(hwnd, ps);
    return 0;
  }
  result = (LONG_PTR)NtUserMessageCall((ULONG_PTR)hwnd, message, wparam, (ULONG_PTR)lparam, 0, MC_DEF_WINDOW_PROC, 0);
  menu_def_after(hwnd, message, wparam, lparam, &result);
  return result;
}
EXPORT LONG_PTR WINAPI DefWindowProcW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return def_proc_w(NULL, hwnd, message, wparam, lparam); }
EXPORT LONG_PTR WINAPI DefWindowProcA(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return a_to_w(def_proc_w, NULL, hwnd, message, wparam, lparam); }
/* The procedure is called directly: a W caller of a thunk gives it Unicode (what it takes), an A caller of a thunk gives the program's procedure its own A messages; an A caller of a window
 * procedure of the system (Unicode) converts. */
static LONG_PTR call_w_proc(void *ctx, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return ((WNDPROC_)ctx)(hwnd, message, wparam, lparam); }
EXPORT LONG_PTR WINAPI CallWindowProcW(WNDPROC_ proc, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return proc(hwnd, message, wparam, lparam); }
EXPORT LONG_PTR WINAPI CallWindowProcA(WNDPROC_ proc, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  const int slot = thunk_slot(proc);
  if (slot >= 0) return g_ansi_procs[slot](hwnd, message, wparam, lparam);
  if (proc == builtin_proc || proc == static_proc || proc == button_proc || proc == edit_proc) return a_to_w(call_w_proc, (void *)proc, hwnd, message, wparam, lparam);
  return proc(hwnd, message, wparam, lparam);
}
static LONG_PTR send_w(void *ctx, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  (void)ctx;
  if (g_hook_count[WH_CALLWNDPROC + 1] == 0 && g_hook_count[WH_CALLWNDPROCRET + 1] == 0) return (LONG_PTR)NtUserMessageCall((ULONG_PTR)hwnd, message, wparam, (ULONG_PTR)lparam, 0, MC_SEND_MESSAGE, 0);
  hook_call_wnd_proc(hwnd, message, wparam, lparam);
  const LONG_PTR result = (LONG_PTR)NtUserMessageCall((ULONG_PTR)hwnd, message, wparam, (ULONG_PTR)lparam, 0, MC_SEND_MESSAGE, 0);
  hook_call_wnd_proc_ret(hwnd, message, wparam, lparam, result);
  return result;
}
EXPORT LONG_PTR WINAPI SendMessageW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return send_w(NULL, hwnd, message, wparam, lparam); }
EXPORT LONG_PTR WINAPI SendMessageA(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return a_to_w(send_w, NULL, hwnd, message, wparam, lparam); }
typedef struct { UINT flags, timeout; ULONG_PTR *result; } TimeoutArgs;
static LONG_PTR send_timeout_w(void *ctx, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  TimeoutArgs *a = ctx;
  struct { UINT flags, timeout; ULONG_PTR result; } info = {a->flags, a->timeout, 0};
  if (a->result) *a->result = 0;
  const ULONG_PTR ok = NtUserMessageCall((ULONG_PTR)hwnd, message, wparam, (ULONG_PTR)lparam, (ULONG_PTR)&info, MC_SEND_MESSAGE_TIMEOUT, 0);
  if (a->result) *a->result = info.result;
  return (LONG_PTR)ok;
}
EXPORT LONG_PTR WINAPI SendMessageTimeoutW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam, UINT flags, UINT timeout, ULONG_PTR *result) {
  TimeoutArgs a = {flags, timeout, result};
  return send_timeout_w(&a, hwnd, message, wparam, lparam);
}
EXPORT LONG_PTR WINAPI SendMessageTimeoutA(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam, UINT flags, UINT timeout, ULONG_PTR *result) {
  TimeoutArgs a = {flags, timeout, result};
  return a_to_w(send_timeout_w, &a, hwnd, message, wparam, lparam);
}
static LONG_PTR notify_w(void *ctx, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { (void)ctx; return (LONG_PTR)NtUserMessageCall((ULONG_PTR)hwnd, message, wparam, (ULONG_PTR)lparam, 0, MC_SEND_NOTIFY, 0); }
EXPORT BOOL WINAPI SendNotifyMessageW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return (notify_w(NULL, hwnd, message, wparam, lparam) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI SendNotifyMessageA(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return (a_to_w(notify_w, NULL, hwnd, message, wparam, lparam) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI PostMessageW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return (NtUserPostMessage((ULONG_PTR)hwnd, message, wparam, (ULONG_PTR)lparam) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI PostMessageA(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return PostMessageW(hwnd, message, is_char_message(message) ? char_to_wide(wparam) : wparam, lparam); }
EXPORT BOOL WINAPI PostThreadMessageW(DWORD thread, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return (NtUserPostThreadMessage(thread, message, wparam, (ULONG_PTR)lparam) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI PostThreadMessageA(DWORD thread, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return PostThreadMessageW(thread, message, is_char_message(message) ? char_to_wide(wparam) : wparam, lparam); }
EXPORT void WINAPI PostQuitMessage(int code) { NtUserPostQuitMessage((ULONG_PTR)(LONG_PTR)code); }
EXPORT BOOL WINAPI ReplyMessage(LONG_PTR result) { return (NtUserReplyMessage((ULONG_PTR)result) & 0xFFFFFFFFu) != 0; }
EXPORT int WINAPI GetMessageW(MSG_ *msg, HANDLE hwnd, UINT first, UINT last) {
  if (!msg) { SetLastError(ERROR_INVALID_PARAMETER); return -1; }
  for (;;) {
    const int r = (int)(LONG)NtUserGetMessage((ULONG_PTR)msg, (ULONG_PTR)hwnd, first, last);
    if (r <= 0 || !(g_hook_count[WH_KEYBOARD + 1] | g_hook_count[WH_MOUSE + 1] | g_hook_count[WH_GETMESSAGE + 1]) || hook_filter_message(msg, TRUE)) return r;
  }
}
EXPORT int WINAPI GetMessageA(MSG_ *msg, HANDLE hwnd, UINT first, UINT last) { return GetMessageW(msg, hwnd, first, last); }
EXPORT BOOL WINAPI PeekMessageW(MSG_ *msg, HANDLE hwnd, UINT first, UINT last, UINT flags) {
  if (!msg) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  for (;;) {
    if (!(NtUserPeekMessage((ULONG_PTR)msg, (ULONG_PTR)hwnd, first, last, flags) & 0xFFFFFFFFu)) return FALSE;
    if (!(g_hook_count[WH_KEYBOARD + 1] | g_hook_count[WH_MOUSE + 1] | g_hook_count[WH_GETMESSAGE + 1]) || hook_filter_message(msg, (flags & 1 /* PM_REMOVE */) != 0)) return TRUE;
  }
}
EXPORT BOOL WINAPI PeekMessageA(MSG_ *msg, HANDLE hwnd, UINT first, UINT last, UINT flags) { return PeekMessageW(msg, hwnd, first, last, flags); }
EXPORT BOOL WINAPI WaitMessage(void) { return (NtUserWaitMessage() & 0xFFFFFFFFu) != 0; }
EXPORT LONG_PTR WINAPI DispatchMessageW(const MSG_ *msg) {
  if (!msg) return 0;
  if (g_hook_count[WH_CALLWNDPROC + 1] == 0 && g_hook_count[WH_CALLWNDPROCRET + 1] == 0) return (LONG_PTR)NtUserDispatchMessage((ULONG_PTR)msg);
  hook_call_wnd_proc(msg->hwnd, msg->message, msg->wparam, msg->lparam);
  const LONG_PTR result = (LONG_PTR)NtUserDispatchMessage((ULONG_PTR)msg);
  hook_call_wnd_proc_ret(msg->hwnd, msg->message, msg->wparam, msg->lparam, result);
  return result;
}
EXPORT LONG_PTR WINAPI DispatchMessageA(const MSG_ *msg) { return DispatchMessageW(msg); }
EXPORT BOOL WINAPI TranslateMessage(const MSG_ *msg) { return msg && (NtUserTranslateMessage((ULONG_PTR)msg, 0) & 0xFFFFFFFFu) != 0; }
EXPORT DWORD WINAPI GetMessagePos(void) { return (DWORD)NtUserGetMessagePos(); }
EXPORT DWORD WINAPI GetQueueStatus(UINT flags) { return (DWORD)NtUserGetQueueStatus(flags); }
EXPORT DWORD WINAPI MsgWaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, DWORD timeout, DWORD mask, DWORD flags) { return (DWORD)NtUserMsgWaitForMultipleObjectsEx(count, (ULONG_PTR)handles, timeout, mask, flags); }
EXPORT DWORD WINAPI MsgWaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL wait_all, DWORD timeout, DWORD mask) { return MsgWaitForMultipleObjectsEx(count, handles, timeout, mask, wait_all ? 1 : 0); }
EXPORT ULONG_PTR WINAPI SetTimer(HANDLE hwnd, ULONG_PTR id, UINT elapse, void *proc) { return NtUserSetTimer((ULONG_PTR)hwnd, id, elapse, (ULONG_PTR)proc, 0); }
EXPORT BOOL WINAPI KillTimer(HANDLE hwnd, ULONG_PTR id) { return (NtUserKillTimer((ULONG_PTR)hwnd, id) & 0xFFFFFFFFu) != 0; }

/* The window rectangle for a client rectangle (AdjustWindowRectEx): the metrics are the window manager's. */
EXPORT BOOL WINAPI AdjustWindowRectExForDpi(RECT_ *rect, DWORD style, BOOL menu, DWORD ex_style, UINT dpi) {
  struct { DWORD style, ex_style, menu, dpi; } params = {style, ex_style, menu, dpi};
  if (!rect) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return (NtUserCallTwoParam((ULONG_PTR)rect, (ULONG_PTR)&params, 7 /* AdjustWindowRect */) & 0xFFFFFFFFu) != 0;
}
EXPORT BOOL WINAPI AdjustWindowRectEx(RECT_ *rect, DWORD style, BOOL menu, DWORD ex_style) { return AdjustWindowRectExForDpi(rect, style, menu, ex_style, 0); }
EXPORT BOOL WINAPI AdjustWindowRect(RECT_ *rect, DWORD style, BOOL menu) { return AdjustWindowRectEx(rect, style, menu, 0); }

/* Clipboard format names are atoms of the window manager's table (the same table the class names use). */
EXPORT UINT WINAPI RegisterClipboardFormatW(const WCHAR *name) { return RegisterWindowMessageW(name); }
EXPORT UINT WINAPI RegisterClipboardFormatA(const char *name) { return RegisterWindowMessageA(name); }
EXPORT int WINAPI GetClipboardFormatNameW(UINT format, LPWSTR buffer, int count) {
  UNICODE_STRING us;
  if (format < 0xC000 || !buffer || count <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  us.Length = 0; us.MaximumLength = (USHORT)(count > 32767 ? 65534 : count * 2); us.Buffer = buffer;
  return (int)(NtUserGetAtomName(format, (ULONG_PTR)&us) & 0xFFFFFFFFu);
}
EXPORT int WINAPI GetClipboardFormatNameA(UINT format, LPSTR buffer, int count) {
  WCHAR wide[260];
  if (!buffer || count <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  const int n = GetClipboardFormatNameW(format, wide, 260);
  if (!n) return 0;
  const int bytes = WideCharToMultiByte(65001, 0, wide, n, buffer, count - 1, NULL, NULL);
  buffer[bytes] = 0;
  return bytes;
}

/* ---- more of the window manager's services -------------------------------------------------------------------------------------------------------------------------- */
SVC NtUserGetWindowPlacement(ULONG_PTR hwnd, ULONG_PTR placement);
SVC NtUserSetWindowPlacement(ULONG_PTR hwnd, ULONG_PTR placement);
SVC NtUserBeginDeferWindowPos(ULONG_PTR count);
SVC NtUserDeferWindowPosAndBand(ULONG_PTR dwp, ULONG_PTR hwnd, ULONG_PTR after, ULONG_PTR x, ULONG_PTR y, ULONG_PTR cx, ULONG_PTR cy, ULONG_PTR flags, ULONG_PTR unknown1, ULONG_PTR unknown2);
SVC NtUserEndDeferWindowPosEx(ULONG_PTR dwp, ULONG_PTR async);
SVC NtUserRedrawWindow(ULONG_PTR hwnd, ULONG_PTR rect, ULONG_PTR region, ULONG_PTR flags);
SVC NtUserInvalidateRgn(ULONG_PTR hwnd, ULONG_PTR region, ULONG_PTR erase);
SVC NtUserValidateRgn(ULONG_PTR hwnd, ULONG_PTR region);
SVC NtUserGetUpdateRgn(ULONG_PTR hwnd, ULONG_PTR region, ULONG_PTR erase);
SVC NtUserLockWindowUpdate(ULONG_PTR hwnd);
SVC NtUserChildWindowFromPointEx(ULONG_PTR parent, ULONG_PTR x, ULONG_PTR y, ULONG_PTR flags);
SVC NtUserGetGUIThreadInfo(ULONG_PTR thread, ULONG_PTR info);
SVC NtUserGetKeyState(ULONG_PTR vk);
SVC NtUserGetAsyncKeyState(ULONG_PTR vk);
SVC NtUserGetKeyboardState(ULONG_PTR buffer);
SVC NtUserSetKeyboardState(ULONG_PTR buffer);
SVC NtUserGetCursorPos(ULONG_PTR point);
SVC NtUserSetCursorPos(ULONG_PTR x, ULONG_PTR y);
SVC NtUserAttachThreadInput(ULONG_PTR from, ULONG_PTR to, ULONG_PTR attach);
SVC NtUserBuildHwndList(ULONG_PTR desktop, ULONG_PTR parent, ULONG_PTR children, ULONG_PTR non_immersive, ULONG_PTR thread, ULONG_PTR count, ULONG_PTR buffer, ULONG_PTR needed);

EXPORT BOOL WINAPI IsWindow(HANDLE window);
#define WS_MINIMIZE_ 0x20000000u
#define WS_MAXIMIZE_ 0x01000000u
EXPORT BOOL WINAPI GetWindowPlacement(HANDLE hwnd, void *placement) { return BOOL_OF(NtUserGetWindowPlacement((ULONG_PTR)hwnd, (ULONG_PTR)placement)); }
EXPORT BOOL WINAPI SetWindowPlacement(HANDLE hwnd, const void *placement) { return BOOL_OF(NtUserSetWindowPlacement((ULONG_PTR)hwnd, (ULONG_PTR)placement)); }
EXPORT BOOL WINAPI IsIconic(HANDLE hwnd) { return (GetWindowLongW(hwnd, -16) & WS_MINIMIZE_) != 0; }
EXPORT BOOL WINAPI IsZoomed(HANDLE hwnd) { return (GetWindowLongW(hwnd, -16) & WS_MAXIMIZE_) != 0; }
EXPORT BOOL WINAPI CloseWindow(HANDLE hwnd) { ShowWindow(hwnd, 6 /* SW_MINIMIZE */); return TRUE; }
EXPORT BOOL WINAPI OpenIcon(HANDLE hwnd) { if (!IsIconic(hwnd)) return FALSE; ShowWindow(hwnd, 9 /* SW_RESTORE */); return TRUE; }
EXPORT BOOL WINAPI BringWindowToTop(HANDLE hwnd) { return SetWindowPos(hwnd, NULL /* HWND_TOP */, 0, 0, 0, 0, 0x3 /* SWP_NOSIZE | SWP_NOMOVE */); }
EXPORT HANDLE WINAPI GetLastActivePopup(HANDLE hwnd) { return (HANDLE)NtUserCallHwnd((ULONG_PTR)hwnd, 3); }
EXPORT HANDLE WINAPI ChildWindowFromPointEx(HANDLE parent, POINT_ p, UINT flags) { return (HANDLE)NtUserChildWindowFromPointEx((ULONG_PTR)parent, (ULONG_PTR)(LONG_PTR)p.x, (ULONG_PTR)(LONG_PTR)p.y, flags); }
EXPORT HANDLE WINAPI ChildWindowFromPoint(HANDLE parent, POINT_ p) { return ChildWindowFromPointEx(parent, p, 0); }

EXPORT HANDLE WINAPI BeginDeferWindowPos(int count) { return (HANDLE)NtUserBeginDeferWindowPos((ULONG_PTR)(LONG_PTR)count); }
EXPORT HANDLE WINAPI DeferWindowPos(HANDLE dwp, HANDLE hwnd, HANDLE after, int x, int y, int cx, int cy, UINT flags) {
  return (HANDLE)NtUserDeferWindowPosAndBand((ULONG_PTR)dwp, (ULONG_PTR)hwnd, (ULONG_PTR)after, (ULONG_PTR)(LONG_PTR)x, (ULONG_PTR)(LONG_PTR)y, (ULONG_PTR)(LONG_PTR)cx, (ULONG_PTR)(LONG_PTR)cy, flags, 0, 0);
}
EXPORT BOOL WINAPI EndDeferWindowPos(HANDLE dwp) { return BOOL_OF(NtUserEndDeferWindowPosEx((ULONG_PTR)dwp, 0)); }
EXPORT BOOL WINAPI RedrawWindow(HANDLE hwnd, const void *rect, HANDLE region, UINT flags) { return BOOL_OF(NtUserRedrawWindow((ULONG_PTR)hwnd, (ULONG_PTR)rect, (ULONG_PTR)region, flags)); }
EXPORT BOOL WINAPI InvalidateRgn(HANDLE hwnd, HANDLE region, BOOL erase) { return BOOL_OF(NtUserInvalidateRgn((ULONG_PTR)hwnd, (ULONG_PTR)region, erase)); }
EXPORT BOOL WINAPI ValidateRgn(HANDLE hwnd, HANDLE region) { return BOOL_OF(NtUserValidateRgn((ULONG_PTR)hwnd, (ULONG_PTR)region)); }
EXPORT int WINAPI GetUpdateRgn(HANDLE hwnd, HANDLE region, BOOL erase) { return (int)NtUserGetUpdateRgn((ULONG_PTR)hwnd, (ULONG_PTR)region, erase); }
EXPORT BOOL WINAPI LockWindowUpdate(HANDLE hwnd) { return BOOL_OF(NtUserLockWindowUpdate((ULONG_PTR)hwnd)); }

/* Windows of a desktop, a parent or a thread: the window manager lists the handles, the callback is called for those that still exist. */
static HANDLE *list_windows(HANDLE parent, BOOL recursive, DWORD thread, ULONG *count) {
  ULONG capacity = 64, needed = 0;
  for (;;) {
    HANDLE *list = u32_alloc((SIZE_T)capacity * sizeof(HANDLE));
    if (!list) return NULL;
    const LONG status = (LONG)NtUserBuildHwndList(0, (ULONG_PTR)parent, recursive, 0, thread, capacity, (ULONG_PTR)list, (ULONG_PTR)&needed);
    if (status >= 0) { *count = needed; return list; }
    u32_free(list);
    if ((ULONG)status != 0x80000005u && (ULONG)status != 0xC0000023u) return NULL;      /* (anything but "buffer too small") */
    if (needed <= capacity) needed = capacity * 2;
    capacity = needed;
  }
}
typedef BOOL (WINAPI *WNDENUMPROC_)(HANDLE, LONG_PTR);
static BOOL enum_list(HANDLE *list, ULONG count, WNDENUMPROC_ proc, LONG_PTR lparam) {
  BOOL result = TRUE;
  if (!list) return FALSE;
  for (ULONG i = 0; i < count && result; ++i) if (IsWindow(list[i])) result = proc(list[i], lparam);
  u32_free(list);
  return result;
}
EXPORT BOOL WINAPI EnumWindows(WNDENUMPROC_ proc, LONG_PTR lparam) { ULONG n = 0; HANDLE *l = list_windows(NULL, FALSE, 0, &n); return enum_list(l, n, proc, lparam); }
EXPORT BOOL WINAPI EnumThreadWindows(DWORD thread, WNDENUMPROC_ proc, LONG_PTR lparam) { ULONG n = 0; HANDLE *l = list_windows(NULL, FALSE, thread, &n); return enum_list(l, n, proc, lparam); }
EXPORT BOOL WINAPI EnumChildWindows(HANDLE parent, WNDENUMPROC_ proc, LONG_PTR lparam) {
  ULONG n = 0;
  if (!parent) parent = GetDesktopWindow();
  HANDLE *l = list_windows(parent, TRUE, 0, &n);
  return enum_list(l, n, proc, lparam);
}

/* The state of the calling thread (NtUserGetThreadState) and of the keyboard and the cursor. */
EXPORT BOOL WINAPI InSendMessage(void) { return (NtUserGetThreadState(8) & 1) != 0; }
EXPORT DWORD WINAPI InSendMessageEx(void *reserved) { (void)reserved; return (DWORD)NtUserGetThreadState(8); }
EXPORT DWORD WINAPI GetMessageTime(void) { return (DWORD)NtUserGetThreadState(9); }
EXPORT LONG_PTR WINAPI GetMessageExtraInfo(void) { return (LONG_PTR)NtUserGetThreadState(7); }
EXPORT BOOL WINAPI GetGUIThreadInfo(DWORD thread, void *info) { return BOOL_OF(NtUserGetGUIThreadInfo(thread, (ULONG_PTR)info)); }
EXPORT SHORT WINAPI GetKeyState(int vk) { return (SHORT)NtUserGetKeyState((ULONG_PTR)(LONG_PTR)vk); }
EXPORT SHORT WINAPI GetAsyncKeyState(int vk) { return (SHORT)NtUserGetAsyncKeyState((ULONG_PTR)(LONG_PTR)vk); }
EXPORT BOOL WINAPI GetKeyboardState(BYTE *state) { return BOOL_OF(NtUserGetKeyboardState((ULONG_PTR)state)); }
EXPORT BOOL WINAPI SetKeyboardState(BYTE *state) { return BOOL_OF(NtUserSetKeyboardState((ULONG_PTR)state)); }
EXPORT BOOL WINAPI GetCursorPos(POINT_ *p) { return p && BOOL_OF(NtUserGetCursorPos((ULONG_PTR)p)); }
EXPORT BOOL WINAPI SetCursorPos(int x, int y) { return BOOL_OF(NtUserSetCursorPos((ULONG_PTR)(LONG_PTR)x, (ULONG_PTR)(LONG_PTR)y)); }
EXPORT BOOL WINAPI AttachThreadInput(DWORD from, DWORD to, BOOL attach) { return BOOL_OF(NtUserAttachThreadInput(from, to, attach)); }

/* ---- synthesized input ---------------------------------------------------------------------------------------------------------------------------------------------- */
SVC NtUserSendInput(ULONG_PTR count, ULONG_PTR inputs, ULONG_PTR size);
typedef struct { DWORD type; DWORD pad_; union { struct { LONG dx, dy; DWORD data, flags, time; ULONG_PTR extra; } mouse; struct { WORD vk, scan; DWORD flags, time; ULONG_PTR extra; } key; BYTE raw[32]; } u; } Input_;
EXPORT UINT WINAPI SendInput(UINT count, Input_ *inputs, int size) {
  if (!count || !inputs || size != (int)sizeof(Input_)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  return (UINT)NtUserSendInput(count, (ULONG_PTR)inputs, (ULONG_PTR)(LONG_PTR)size);
}
EXPORT void WINAPI mouse_event(DWORD flags, DWORD dx, DWORD dy, DWORD data, ULONG_PTR extra) {
  Input_ in = {0};
  in.type = 0;
  in.u.mouse.dx = (LONG)dx; in.u.mouse.dy = (LONG)dy; in.u.mouse.data = data; in.u.mouse.flags = flags; in.u.mouse.extra = extra;
  SendInput(1, &in, sizeof in);
}
EXPORT void WINAPI keybd_event(BYTE vk, BYTE scan, DWORD flags, ULONG_PTR extra) {
  Input_ in = {0};
  in.type = 1;
  in.u.key.vk = vk; in.u.key.scan = scan; in.u.key.flags = flags; in.u.key.extra = extra;
  SendInput(1, &in, sizeof in);
}
