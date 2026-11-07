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

/* The smaller parts of user32.dll that programs such as Chromium call on their normal paths (included by user32.c, after window.inc):
 *   rectangles      SetRect, CopyRect, EqualRect, InflateRect, OffsetRect, IntersectRect, UnionRect, SubtractRect, IsRectEmpty, PtInRect (pure arithmetic, as Windows)
 *   strings         LoadStringW/A from the RT_STRING bundles of a module (also the "pointer to the resource" mode of a zero length), OEM <-> ANSI conversions
 *   message boxes   MessageBox*: there is no dialog yet; the text goes to the debugger output and the default button is the answer
 *   stations        window stations and desktops over win32k's station services (src/Process/Win32k/Win32kUserStation.cpp)
 *   input           hot keys, TrackMouseEvent, GetLastInputInfo over win32k; pointer (pen/touch) devices: none
 *   notifications   power setting, suspend/resume and device notifications: handles that register nothing (no such broadcasts are sent here)
 *   painting        DrawEdge, DrawFrameControl (plain 3D frames), InvertRect, window regions, WindowFromDC
 *   DDE             not available: every call fails as DDEML does before DdeInitialize succeeds. */

IMPORT void WINAPI OutputDebugStringW(LPCWSTR text);
IMPORT void WINAPI AcquireSRWLockExclusive(SRWLOCK *lock);
IMPORT void WINAPI ReleaseSRWLockExclusive(SRWLOCK *lock);
typedef unsigned int UINT32;

/* ---- rectangles ------------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT BOOL WINAPI SetRect(RECT_ *r, int left, int top, int right, int bottom) {
  if (!r) return FALSE;
  r->left = left; r->top = top; r->right = right; r->bottom = bottom;
  return TRUE;
}
EXPORT BOOL WINAPI SetRectEmpty(RECT_ *r) { return SetRect(r, 0, 0, 0, 0); }
EXPORT BOOL WINAPI CopyRect(RECT_ *dst, const RECT_ *src) {
  if (!dst || !src) return FALSE;
  *dst = *src;
  return TRUE;
}
EXPORT BOOL WINAPI IsRectEmpty(const RECT_ *r) { return !r || r->right <= r->left || r->bottom <= r->top; }
EXPORT BOOL WINAPI EqualRect(const RECT_ *a, const RECT_ *b) {
  if (!a || !b) return FALSE;
  return a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom;
}
EXPORT BOOL WINAPI InflateRect(RECT_ *r, int dx, int dy) {
  if (!r) return FALSE;
  r->left -= dx; r->right += dx; r->top -= dy; r->bottom += dy;
  return TRUE;
}
EXPORT BOOL WINAPI OffsetRect(RECT_ *r, int dx, int dy) {
  if (!r) return FALSE;
  r->left += dx; r->right += dx; r->top += dy; r->bottom += dy;
  return TRUE;
}
EXPORT BOOL WINAPI PtInRect(const RECT_ *r, POINT_ pt) { return r && pt.x >= r->left && pt.x < r->right && pt.y >= r->top && pt.y < r->bottom; }
/* The overlap of two rectangles; an empty overlap (or an empty input) gives the empty rectangle 0,0,0,0 and FALSE. */
EXPORT BOOL WINAPI IntersectRect(RECT_ *dst, const RECT_ *a, const RECT_ *b) {
  if (!dst || !a || !b) return FALSE;
  if (IsRectEmpty(a) || IsRectEmpty(b) || a->left >= b->right || b->left >= a->right || a->top >= b->bottom || b->top >= a->bottom) { SetRectEmpty(dst); return FALSE; }
  RECT_ r;
  r.left = a->left > b->left ? a->left : b->left;
  r.right = a->right < b->right ? a->right : b->right;
  r.top = a->top > b->top ? a->top : b->top;
  r.bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
  *dst = r;
  return TRUE;
}
/* The smallest rectangle around both (an empty one does not count); both empty: the empty rectangle and FALSE. */
EXPORT BOOL WINAPI UnionRect(RECT_ *dst, const RECT_ *a, const RECT_ *b) {
  if (!dst || !a || !b) return FALSE;
  const BOOL ea = IsRectEmpty(a), eb = IsRectEmpty(b);
  if (ea && eb) { SetRectEmpty(dst); return FALSE; }
  if (ea) { *dst = *b; return TRUE; }
  if (eb) { *dst = *a; return TRUE; }
  RECT_ r;
  r.left = a->left < b->left ? a->left : b->left;
  r.right = a->right > b->right ? a->right : b->right;
  r.top = a->top < b->top ? a->top : b->top;
  r.bottom = a->bottom > b->bottom ? a->bottom : b->bottom;
  *dst = r;
  return TRUE;
}
/* a minus b, when the result is still a rectangle (b covers a whole side of a); otherwise a itself. FALSE if the result is empty. */
EXPORT BOOL WINAPI SubtractRect(RECT_ *dst, const RECT_ *a, const RECT_ *b) {
  if (!dst || !a || !b) return FALSE;
  RECT_ r = *a, cut;
  if (IsRectEmpty(a)) { SetRectEmpty(dst); return FALSE; }
  if (IntersectRect(&cut, a, b)) {
    if (EqualRect(&cut, a)) { SetRectEmpty(dst); return FALSE; }
    if (cut.top == r.top && cut.bottom == r.bottom) { if (cut.left == r.left) r.left = cut.right; else if (cut.right == r.right) r.right = cut.left; }
    else if (cut.left == r.left && cut.right == r.right) { if (cut.top == r.top) r.top = cut.bottom; else if (cut.bottom == r.bottom) r.bottom = cut.top; }
  }
  *dst = r;
  return TRUE;
}

/* ---- string resources ------------------------------------------------------------------------------------------------------------------------------------------- */

/* The string `id` of a module: strings are kept 16 to a bundle (RT_STRING resource id / 16 + 1), each a WORD length and that many characters, not terminated. */
static BOOL find_string_resource(HANDLE module, UINT id, const WCHAR **text, UINT *length) {
  *text = NULL;
  *length = 0;
  if (!module) module = GetModuleHandleW(NULL);
  HANDLE info = FindResourceW(module, (LPCWSTR)(ULONG_PTR)((id >> 4) + 1), (LPCWSTR)(ULONG_PTR)6 /* RT_STRING */);
  if (!info) return FALSE;
  HANDLE data = LoadResource(module, info);
  const WORD *p = data ? LockResource(data) : NULL;
  const DWORD words = SizeofResource(module, info) / 2;
  if (!p) return FALSE;
  DWORD at = 0;
  for (UINT i = 0; i < (id & 15); ++i) {      /* skip the strings before it, never past the end of the bundle */
    if (at >= words) return FALSE;
    const DWORD n = p[at];
    if (n > words - at - 1) return FALSE;
    at += 1 + n;
  }
  if (at >= words) return FALSE;
  const DWORD n = p[at];
  if (n > words - at - 1) return FALSE;
  *text = (const WCHAR *)&p[at + 1];
  *length = n;
  return n != 0;
}
/* The string into `buffer` (cut to max - 1 characters and terminated) and its length; max 0: *buffer becomes a pointer to the string in the resource (not terminated). */
EXPORT int WINAPI LoadStringW(HANDLE instance, UINT id, LPWSTR buffer, int max) {
  const WCHAR *text;
  UINT length;
  if (!buffer) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  const BOOL found = find_string_resource(instance, id, &text, &length);
  if (max == 0) { *(const WCHAR **)buffer = found ? text : NULL; return found ? (int)length : 0; }
  if (max < 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  if (!found) { buffer[0] = 0; SetLastError(1814 /* ERROR_RESOURCE_NAME_NOT_FOUND */); return 0; }
  const UINT n = length < (UINT)(max - 1) ? length : (UINT)(max - 1);
  for (UINT i = 0; i < n; ++i) buffer[i] = text[i];
  buffer[n] = 0;
  return (int)n;
}
EXPORT int WINAPI LoadStringA(HANDLE instance, UINT id, LPSTR buffer, int max) {
  const WCHAR *text;
  UINT length;
  if (!buffer || max <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  if (!find_string_resource(instance, id, &text, &length)) { buffer[0] = 0; return 0; }
  int n = WideCharToMultiByte(0 /* CP_ACP */, 0, text, (int)length, buffer, max - 1, NULL, NULL);
  if (n < 0) n = 0;
  if (n == 0 && length) {      /* it did not fit: as much as fits, character by character */
    while (n < max - 1 && (UINT)n < length && WideCharToMultiByte(0, 0, text, n + 1, buffer, max - 1, NULL, NULL) == n + 1) ++n;
    if (n) WideCharToMultiByte(0, 0, text, n, buffer, max - 1, NULL, NULL);
  }
  buffer[n] = 0;
  return n;
}

/* ---- OEM characters --------------------------------------------------------------------------------------------------------------------------------------------- */

#define CP_OEMCP_ 1
/* `length` bytes of one code page into another through UTF-16, a few hundred characters at a time (the buffers may be the same: Windows allows it for these). */
static BOOL convert_bytes(UINT from, UINT to, LPCSTR src, LPSTR dst, DWORD length) {
  if (!src || !dst) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR wide[256];
  while (length) {
    const int n = length > 256 ? 256 : (int)length;
    const int w = MultiByteToWideChar(from, 0, src, n, wide, 256);
    if (w <= 0) return FALSE;
    char narrow[512];
    const int b = WideCharToMultiByte(to, 0, wide, w, narrow, (int)sizeof narrow, NULL, NULL);
    if (b <= 0) return FALSE;
    for (int i = 0; i < b && i < n; ++i) dst[i] = narrow[i];
    src += n; dst += n; length -= (DWORD)n;
  }
  return TRUE;
}
static DWORD narrow_length(LPCSTR s) { DWORD n = 0; while (s[n]) ++n; return n; }
static DWORD wide_length(LPCWSTR s) { DWORD n = 0; while (s[n]) ++n; return n; }
EXPORT BOOL WINAPI OemToCharBuffA(LPCSTR src, LPSTR dst, DWORD length) { return convert_bytes(CP_OEMCP_, 0, src, dst, length); }
EXPORT BOOL WINAPI CharToOemBuffA(LPCSTR src, LPSTR dst, DWORD length) { return convert_bytes(0, CP_OEMCP_, src, dst, length); }
EXPORT BOOL WINAPI OemToCharA(LPCSTR src, LPSTR dst) {
  if (!src || !dst) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return convert_bytes(CP_OEMCP_, 0, src, dst, narrow_length(src) + 1);
}
EXPORT BOOL WINAPI CharToOemA(LPCSTR src, LPSTR dst) {
  if (!src || !dst) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return convert_bytes(0, CP_OEMCP_, src, dst, narrow_length(src) + 1);
}
EXPORT BOOL WINAPI CharToOemBuffW(LPCWSTR src, LPSTR dst, DWORD length) {
  if (!src || !dst) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (length == 0) return TRUE;
  if (length > 0x7FFFFFFF) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return WideCharToMultiByte(CP_OEMCP_, 0, src, (int)length, dst, (int)length, NULL, NULL) > 0;
}
EXPORT BOOL WINAPI CharToOemW(LPCWSTR src, LPSTR dst) {
  if (!src || !dst) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return CharToOemBuffW(src, dst, wide_length(src) + 1);
}
EXPORT BOOL WINAPI OemToCharBuffW(LPCSTR src, LPWSTR dst, DWORD length) {
  if (!src || !dst) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (length == 0) return TRUE;
  if (length > 0x7FFFFFFF) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return MultiByteToWideChar(CP_OEMCP_, 0, src, (int)length, dst, (int)length) > 0;
}
EXPORT BOOL WINAPI OemToCharW(LPCSTR src, LPWSTR dst) {
  if (!src || !dst) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return OemToCharBuffW(src, dst, narrow_length(src) + 1);
}

/* ---- message boxes ---------------------------------------------------------------------------------------------------------------------------------------------- */

/* No dialog is shown: caption and text go to the debugger output (OutputDebugString) and the answer is the default button, as if Enter had been pressed. */
static int message_box_answer(UINT type) {
  static const BYTE buttons[7][3] = {{1, 0, 0} /* OK */, {1, 2, 0} /* OK Cancel */, {3, 4, 5} /* Abort Retry Ignore */, {6, 7, 2} /* Yes No Cancel */, {6, 7, 0} /* Yes No */,
                                     {4, 2, 0} /* Retry Cancel */, {2, 10, 11} /* Cancel TryAgain Continue */};
  const UINT kind = type & 0xF, def = (type >> 8) & 0xF;
  if (kind > 6) return 1;
  const int answer = def < 3 ? buttons[kind][def] : 0;
  return answer ? answer : buttons[kind][0];
}
EXPORT int WINAPI MessageBoxExW(HANDLE hwnd, LPCWSTR text, LPCWSTR caption, UINT type, WORD language) {
  (void)hwnd; (void)language;
  OutputDebugStringW(caption ? caption : L"Error");
  OutputDebugStringW(L": ");
  OutputDebugStringW(text ? text : L"");
  OutputDebugStringW(L"\n");
  return message_box_answer(type);
}
EXPORT int WINAPI MessageBoxW(HANDLE hwnd, LPCWSTR text, LPCWSTR caption, UINT type) { return MessageBoxExW(hwnd, text, caption, type, 0); }
static WCHAR *message_box_wide(LPCSTR s) {
  if (!s) return NULL;
  const int n = MultiByteToWideChar(0, 0, s, -1, NULL, 0);
  WCHAR *w = n > 0 ? HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n * 2) : NULL;
  if (w) MultiByteToWideChar(0, 0, s, -1, w, n);
  return w;
}
EXPORT int WINAPI MessageBoxExA(HANDLE hwnd, LPCSTR text, LPCSTR caption, UINT type, WORD language) {
  WCHAR *t = message_box_wide(text), *c = message_box_wide(caption);
  const int answer = MessageBoxExW(hwnd, t, c, type, language);
  if (t) HeapFree(GetProcessHeap(), 0, t);
  if (c) HeapFree(GetProcessHeap(), 0, c);
  return answer;
}
EXPORT int WINAPI MessageBoxA(HANDLE hwnd, LPCSTR text, LPCSTR caption, UINT type) { return MessageBoxExA(hwnd, text, caption, type, 0); }

/* ---- window stations and desktops ------------------------------------------------------------------------------------------------------------------------------- */

SVC NtUserCreateWindowStation(ULONG_PTR attributes, ULONG_PTR access, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7);
SVC NtUserOpenWindowStation(ULONG_PTR attributes, ULONG_PTR access);
SVC NtUserCloseWindowStation(ULONG_PTR handle);
SVC NtUserGetProcessWindowStation(void);
SVC NtUserSetProcessWindowStation(ULONG_PTR handle);
SVC NtUserCreateDesktopEx(ULONG_PTR attributes, ULONG_PTR device, ULONG_PTR devmode, ULONG_PTR flags, ULONG_PTR access, ULONG_PTR heap);
SVC NtUserOpenDesktop(ULONG_PTR attributes, ULONG_PTR flags, ULONG_PTR access);
SVC NtUserOpenInputDesktop(ULONG_PTR flags, ULONG_PTR inherit, ULONG_PTR access);
SVC NtUserCloseDesktop(ULONG_PTR handle);
SVC NtUserSwitchDesktop(ULONG_PTR handle);
SVC NtUserGetThreadDesktop(ULONG_PTR thread);
SVC NtUserSetThreadDesktop(ULONG_PTR handle);
SVC NtUserGetObjectInformation(ULONG_PTR handle, ULONG_PTR index, ULONG_PTR info, ULONG_PTR length, ULONG_PTR needed);
SVC NtUserSetObjectInformation(ULONG_PTR handle, ULONG_PTR index, ULONG_PTR info, ULONG_PTR length);

#define OBJ_INHERIT_ 0x02
#define OBJ_OPENIF_ 0x80
#define OBJ_CASE_INSENSITIVE_ 0x40
#define CWF_CREATE_ONLY_ 1
typedef struct { DWORD nLength; void *lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES_;

/* OBJECT_ATTRIBUTES naming an object of win32k (the kernel looks at the last path component of the name only). */
static void station_attributes(OBJECT_ATTRIBUTES *oa, UNICODE_STRING *us, LPCWSTR name, HANDLE root, ULONG attributes, const SECURITY_ATTRIBUTES_ *sa) {
  const DWORD n = name ? wide_length(name) : 0;
  us->Length = (USHORT)(n > 0x7FFF ? 0xFFFE : n * 2);
  us->MaximumLength = us->Length;
  us->Buffer = (PWSTR)name;
  oa->Length = sizeof *oa;
  oa->RootDirectory = root;
  oa->ObjectName = us;
  oa->Attributes = attributes | OBJ_CASE_INSENSITIVE_ | (sa && sa->bInheritHandle ? OBJ_INHERIT_ : 0);
  oa->SecurityDescriptor = sa ? sa->lpSecurityDescriptor : NULL;
  oa->SecurityQualityOfService = NULL;
}
static BOOL name_too_long(LPCWSTR name) {
  if (name && wide_length(name) >= 260) { SetLastError(206 /* ERROR_FILENAME_EXCED_RANGE */); return TRUE; }
  return FALSE;
}
/* An A name as a W one (NULL stays NULL; FALSE: no memory). */
static BOOL station_name_w(LPCSTR name, WCHAR *out, int capacity) {
  if (!name) return TRUE;
  if (MultiByteToWideChar(0, 0, name, -1, out, capacity) > 0) return TRUE;
  SetLastError(206 /* ERROR_FILENAME_EXCED_RANGE */);
  return FALSE;
}

EXPORT HANDLE WINAPI CreateWindowStationW(LPCWSTR name, DWORD flags, DWORD access, const SECURITY_ATTRIBUTES_ *sa) {
  if (name_too_long(name)) return NULL;
  OBJECT_ATTRIBUTES oa;
  UNICODE_STRING us;
  station_attributes(&oa, &us, name, NULL, flags & CWF_CREATE_ONLY_ ? 0 : OBJ_OPENIF_, sa);
  return (HANDLE)NtUserCreateWindowStation((ULONG_PTR)&oa, access, 0, 0, 0, 0, 0);
}
EXPORT HANDLE WINAPI CreateWindowStationA(LPCSTR name, DWORD flags, DWORD access, const SECURITY_ATTRIBUTES_ *sa) {
  WCHAR wide[260];
  if (!station_name_w(name, wide, 260)) return NULL;
  return CreateWindowStationW(name ? wide : NULL, flags, access, sa);
}
EXPORT HANDLE WINAPI OpenWindowStationW(LPCWSTR name, BOOL inherit, DWORD access) {
  if (name_too_long(name)) return NULL;
  OBJECT_ATTRIBUTES oa;
  UNICODE_STRING us;
  station_attributes(&oa, &us, name, NULL, inherit ? OBJ_INHERIT_ : 0, NULL);
  return (HANDLE)NtUserOpenWindowStation((ULONG_PTR)&oa, access);
}
EXPORT HANDLE WINAPI OpenWindowStationA(LPCSTR name, BOOL inherit, DWORD access) {
  WCHAR wide[260];
  if (!station_name_w(name, wide, 260)) return NULL;
  return OpenWindowStationW(name ? wide : NULL, inherit, access);
}
EXPORT BOOL WINAPI CloseWindowStation(HANDLE station) { return (NtUserCloseWindowStation((ULONG_PTR)station) & 0xFFFFFFFFu) != 0; }
EXPORT HANDLE WINAPI GetProcessWindowStation(void) { return (HANDLE)NtUserGetProcessWindowStation(); }
EXPORT BOOL WINAPI SetProcessWindowStation(HANDLE station) { return (NtUserSetProcessWindowStation((ULONG_PTR)station) & 0xFFFFFFFFu) != 0; }

/* A desktop is made in the process's window station (the root directory of the name). */
EXPORT HANDLE WINAPI CreateDesktopW(LPCWSTR name, LPCWSTR device, void *devmode, DWORD flags, DWORD access, const SECURITY_ATTRIBUTES_ *sa) {
  (void)device;
  if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (name_too_long(name)) return NULL;
  OBJECT_ATTRIBUTES oa;
  UNICODE_STRING us;
  station_attributes(&oa, &us, name, GetProcessWindowStation(), OBJ_OPENIF_, sa);
  return (HANDLE)NtUserCreateDesktopEx((ULONG_PTR)&oa, 0, (ULONG_PTR)devmode, flags, access, 0);
}
EXPORT HANDLE WINAPI CreateDesktopA(LPCSTR name, LPCSTR device, void *devmode, DWORD flags, DWORD access, const SECURITY_ATTRIBUTES_ *sa) {
  (void)device;
  WCHAR wide[260];
  if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (!station_name_w(name, wide, 260)) return NULL;
  return CreateDesktopW(wide, NULL, devmode, flags, access, sa);
}
EXPORT HANDLE WINAPI OpenDesktopW(LPCWSTR name, DWORD flags, BOOL inherit, DWORD access) {
  if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (name_too_long(name)) return NULL;
  OBJECT_ATTRIBUTES oa;
  UNICODE_STRING us;
  station_attributes(&oa, &us, name, GetProcessWindowStation(), inherit ? OBJ_INHERIT_ : 0, NULL);
  return (HANDLE)NtUserOpenDesktop((ULONG_PTR)&oa, flags, access);
}
EXPORT HANDLE WINAPI OpenDesktopA(LPCSTR name, DWORD flags, BOOL inherit, DWORD access) {
  WCHAR wide[260];
  if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (!station_name_w(name, wide, 260)) return NULL;
  return OpenDesktopW(wide, flags, inherit, access);
}
EXPORT HANDLE WINAPI OpenInputDesktop(DWORD flags, BOOL inherit, DWORD access) { return (HANDLE)NtUserOpenInputDesktop(flags, inherit, access); }
EXPORT BOOL WINAPI CloseDesktop(HANDLE desktop) { return (NtUserCloseDesktop((ULONG_PTR)desktop) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI SwitchDesktop(HANDLE desktop) { return (NtUserSwitchDesktop((ULONG_PTR)desktop) & 0xFFFFFFFFu) != 0; }
EXPORT HANDLE WINAPI GetThreadDesktop(DWORD thread) { return (HANDLE)NtUserGetThreadDesktop(thread); }
EXPORT BOOL WINAPI SetThreadDesktop(HANDLE desktop) { return (NtUserSetThreadDesktop((ULONG_PTR)desktop) & 0xFFFFFFFFu) != 0; }

/* UOI_FLAGS (a USEROBJECTFLAGS), UOI_NAME, UOI_TYPE ("WindowStation", "Desktop"): a buffer too small fails with ERROR_INSUFFICIENT_BUFFER and *needed set. */
EXPORT BOOL WINAPI GetUserObjectInformationW(HANDLE object, int index, void *info, DWORD length, DWORD *needed) {
  return (NtUserGetObjectInformation((ULONG_PTR)object, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)info, length, (ULONG_PTR)needed) & 0xFFFFFFFFu) != 0;
}
/* The names are converted; *needed is the size of the A string. */
EXPORT BOOL WINAPI GetUserObjectInformationA(HANDLE object, int index, void *info, DWORD length, DWORD *needed) {
  if (index != 2 && index != 3) return GetUserObjectInformationW(object, index, info, length, needed);
  WCHAR wide[300];
  DWORD wide_needed = 0;
  if (!GetUserObjectInformationW(object, index, wide, sizeof wide, &wide_needed)) return FALSE;
  const int n = WideCharToMultiByte(0, 0, wide, -1, NULL, 0, NULL, NULL);
  if (n <= 0) return FALSE;
  if (needed) *needed = (DWORD)n;
  if (!info || length < (DWORD)n) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  return WideCharToMultiByte(0, 0, wide, -1, info, (int)length, NULL, NULL) > 0;
}
EXPORT BOOL WINAPI SetUserObjectInformationW(HANDLE object, int index, void *info, DWORD length) {
  return (NtUserSetObjectInformation((ULONG_PTR)object, (ULONG_PTR)(LONG_PTR)index, (ULONG_PTR)info, length) & 0xFFFFFFFFu) != 0;
}

/* ---- input ------------------------------------------------------------------------------------------------------------------------------------------------------ */

SVC NtUserRegisterHotKey(ULONG_PTR hwnd, ULONG_PTR id, ULONG_PTR modifiers, ULONG_PTR vk);
SVC NtUserUnregisterHotKey(ULONG_PTR hwnd, ULONG_PTR id);
SVC NtUserTrackMouseEvent(ULONG_PTR info);
SVC NtUserAllowSetForegroundWindow(ULONG_PTR process);
SVC NtUserLockSetForegroundWindow(ULONG_PTR code);

EXPORT BOOL WINAPI RegisterHotKey(HANDLE hwnd, int id, UINT modifiers, UINT vk) { return (NtUserRegisterHotKey((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)id, modifiers, vk) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI UnregisterHotKey(HANDLE hwnd, int id) { return (NtUserUnregisterHotKey((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)id) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI AllowSetForegroundWindow(DWORD process) { return (NtUserAllowSetForegroundWindow(process) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI LockSetForegroundWindow(UINT code) { return (NtUserLockSetForegroundWindow(code) & 0xFFFFFFFFu) != 0; }

typedef struct { DWORD cbSize, dwFlags; HANDLE hwndTrack; DWORD dwHoverTime; } TRACKMOUSEEVENT_;
/* TME_LEAVE (and TME_CANCEL) are win32k's: WM_MOUSELEAVE is posted when the pointer leaves the window. TME_HOVER is accepted but no WM_MOUSEHOVER is sent;
 * TME_QUERY answers what this process last asked for. */
static TRACKMOUSEEVENT_ g_tracking;
EXPORT BOOL WINAPI TrackMouseEvent(TRACKMOUSEEVENT_ *tme) {
  if (!tme || tme->cbSize != sizeof *tme) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (tme->dwFlags & 0x40000000u) {      /* TME_QUERY */
    *tme = g_tracking;
    tme->cbSize = sizeof *tme;
    return TRUE;
  }
  if (tme->dwFlags & ~(0x80000000u | 0x10u | 0x2u | 0x1u)) { SetLastError(ERROR_INVALID_FLAGS); return FALSE; }
  if (!(NtUserTrackMouseEvent((ULONG_PTR)tme) & 0xFFFFFFFFu)) return FALSE;
  if (tme->dwFlags & 0x80000000u) { if (g_tracking.hwndTrack == tme->hwndTrack) { g_tracking.dwFlags = 0; g_tracking.hwndTrack = NULL; } }
  else g_tracking = *tme;
  return TRUE;
}

typedef struct { UINT cbSize; DWORD dwTime; } LASTINPUTINFO_;
/* The tick count of the last keyboard or mouse input (win32k's input time). */
EXPORT BOOL WINAPI GetLastInputInfo(LASTINPUTINFO_ *info) {
  if (!info || info->cbSize != sizeof *info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  info->dwTime = (DWORD)NtUserCallNoParam(2 /* GetLastInputTime */);
  return TRUE;
}

/* Pointer (pen and touch) devices: there are none, and no pointer ids exist. */
#define ERROR_INVALID_POINTER_ ERROR_INVALID_PARAMETER
EXPORT BOOL WINAPI GetPointerDevices(UINT32 *count, void *devices) {
  (void)devices;
  if (!count) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  *count = 0;
  return TRUE;
}
EXPORT BOOL WINAPI GetPointerDevice(HANDLE device, void *info) { (void)device; (void)info; SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
EXPORT BOOL WINAPI GetPointerDeviceRects(HANDLE device, RECT_ *pointer, RECT_ *display) { (void)device; (void)pointer; (void)display; SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
EXPORT BOOL WINAPI GetPointerType(UINT32 id, DWORD *type) { (void)id; if (type) *type = 0; SetLastError(ERROR_INVALID_POINTER_); return FALSE; }
EXPORT BOOL WINAPI GetPointerInfo(UINT32 id, void *info) { (void)id; (void)info; SetLastError(ERROR_INVALID_POINTER_); return FALSE; }
EXPORT BOOL WINAPI GetPointerPenInfo(UINT32 id, void *info) { (void)id; (void)info; SetLastError(ERROR_INVALID_POINTER_); return FALSE; }
EXPORT BOOL WINAPI GetPointerTouchInfo(UINT32 id, void *info) { (void)id; (void)info; SetLastError(ERROR_INVALID_POINTER_); return FALSE; }
EXPORT BOOL WINAPI RegisterPointerDeviceNotifications(HANDLE hwnd, BOOL range) { (void)range; if (!IsWindow(hwnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE_); return FALSE; } return TRUE; }
EXPORT BOOL WINAPI CloseTouchInputHandle(HANDLE input) { (void)input; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }

/* ---- notifications ---------------------------------------------------------------------------------------------------------------------------------------------- */

/* Power setting, suspend/resume and device-change notifications: nothing sends them here, so a registration is a handle that is only remembered to be closed. */
#define NOTIFY_BASE 0x4E000000u
#define NOTIFY_SLOTS 256
static BYTE g_notify_used[NOTIFY_SLOTS];
static SRWLOCK g_notify_lock;
static HANDLE notify_register(BYTE kind, const void *recipient) {
  if (!recipient) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  AcquireSRWLockExclusive(&g_notify_lock);
  for (UINT i = 0; i < NOTIFY_SLOTS; ++i) {
    if (!g_notify_used[i]) { g_notify_used[i] = kind; ReleaseSRWLockExclusive(&g_notify_lock); return (HANDLE)(ULONG_PTR)(NOTIFY_BASE + i * 4); }
  }
  ReleaseSRWLockExclusive(&g_notify_lock);
  SetLastError(ERROR_NOT_ENOUGH_MEMORY);
  return NULL;
}
static BOOL notify_unregister(BYTE kind, HANDLE handle) {
  const ULONG_PTR h = (ULONG_PTR)handle;
  const ULONG_PTR slot = (h - NOTIFY_BASE) / 4;
  if (h < NOTIFY_BASE || (h & 3) || slot >= NOTIFY_SLOTS) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  AcquireSRWLockExclusive(&g_notify_lock);
  const BOOL ok = g_notify_used[slot] == kind;
  if (ok) g_notify_used[slot] = 0;
  ReleaseSRWLockExclusive(&g_notify_lock);
  if (!ok) SetLastError(ERROR_INVALID_HANDLE);
  return ok;
}
EXPORT HANDLE WINAPI RegisterPowerSettingNotification(HANDLE recipient, const void *setting, DWORD flags) { (void)flags; return setting ? notify_register(1, recipient) : (SetLastError(ERROR_INVALID_PARAMETER), NULL); }
EXPORT BOOL WINAPI UnregisterPowerSettingNotification(HANDLE handle) { return notify_unregister(1, handle); }
EXPORT HANDLE WINAPI RegisterSuspendResumeNotification(HANDLE recipient, DWORD flags) { (void)flags; return notify_register(2, recipient); }
EXPORT BOOL WINAPI UnregisterSuspendResumeNotification(HANDLE handle) { return notify_unregister(2, handle); }
EXPORT HANDLE WINAPI RegisterDeviceNotificationW(HANDLE recipient, void *filter, DWORD flags) { (void)flags; return filter ? notify_register(3, recipient) : (SetLastError(ERROR_INVALID_PARAMETER), NULL); }
EXPORT HANDLE WINAPI RegisterDeviceNotificationA(HANDLE recipient, void *filter, DWORD flags) { return RegisterDeviceNotificationW(recipient, filter, flags); }
EXPORT BOOL WINAPI UnregisterDeviceNotification(HANDLE handle) { return notify_unregister(3, handle); }

/* ---- windows ---------------------------------------------------------------------------------------------------------------------------------------------------- */

SVC NtUserSetWindowRgn(ULONG_PTR hwnd, ULONG_PTR region, ULONG_PTR redraw);
SVC NtUserGetWindowRgnEx(ULONG_PTR hwnd, ULONG_PTR region, ULONG_PTR unknown);
SVC NtUserWindowFromDC(ULONG_PTR hdc);
SVC NtUserSetWindowDisplayAffinity(ULONG_PTR hwnd, ULONG_PTR affinity);
SVC NtUserGetWindowDisplayAffinity(ULONG_PTR hwnd, ULONG_PTR affinity);

/* The window region belongs to the system once set (the caller must not delete it); GetWindowRgn copies it into the caller's region. */
EXPORT int WINAPI SetWindowRgn(HANDLE hwnd, HANDLE region, BOOL redraw) { return (int)(NtUserSetWindowRgn((ULONG_PTR)hwnd, (ULONG_PTR)region, redraw) & 0xFFFFFFFFu); }
EXPORT int WINAPI GetWindowRgn(HANDLE hwnd, HANDLE region) { return (int)(NtUserGetWindowRgnEx((ULONG_PTR)hwnd, (ULONG_PTR)region, 0) & 0xFFFFFFFFu); }
EXPORT int WINAPI GetWindowRgnBox(HANDLE hwnd, RECT_ *box) {
  if (!box) return 0;
  HANDLE region = CreateRectRgn(0, 0, 0, 0);
  if (!region) return 0;
  int kind = GetWindowRgn(hwnd, region);
  if (kind) kind = GetRgnBox(region, box);
  DeleteObject(region);
  return kind;
}
EXPORT HANDLE WINAPI WindowFromDC(HANDLE hdc) { return (HANDLE)NtUserWindowFromDC((ULONG_PTR)hdc); }
EXPORT BOOL WINAPI SetWindowDisplayAffinity(HANDLE hwnd, DWORD affinity) { return (NtUserSetWindowDisplayAffinity((ULONG_PTR)hwnd, affinity) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI GetWindowDisplayAffinity(HANDLE hwnd, DWORD *affinity) {
  if (!affinity) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  *affinity = 0;      /* WDA_NONE */
  return (NtUserGetWindowDisplayAffinity((ULONG_PTR)hwnd, (ULONG_PTR)affinity) & 0xFFFFFFFFu) != 0;
}
EXPORT HANDLE WINAPI GetShellWindow(void) { return (HANDLE)NtUserCallNoParam(4 /* GetShellWindow */); }
/* Snap layouts do not exist here: no window is arranged. */
EXPORT BOOL WINAPI IsWindowArranged(HANDLE hwnd) { (void)hwnd; return FALSE; }
/* No flashing caption bars: the result is the window's previous state, "not active" (FALSE), as for a window that was not flashing. */
EXPORT BOOL WINAPI FlashWindowEx(const void *info) { (void)info; return FALSE; }
EXPORT BOOL WINAPI FlashWindow(HANDLE hwnd, BOOL invert) { (void)hwnd; (void)invert; return FALSE; }
/* PrintWindow (a copy of a window into a DC through WM_PRINT) is not supported. */
EXPORT BOOL WINAPI PrintWindow(HANDLE hwnd, HANDLE hdc, UINT flags) { (void)hwnd; (void)hdc; (void)flags; SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
/* The number of GDI or USER objects of a process (GR_GDIOBJECTS 0, GR_USEROBJECTS 1, their peaks 2 and 4): not counted per process here; a small constant
 * that stands for "some objects" (0 would read as a failure). */
EXPORT DWORD WINAPI GetGuiResources(HANDLE process, DWORD flags) {
  (void)process;
  if (flags > 4 || flags == 3) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  return 16;
}

/* ---- 3D frames -------------------------------------------------------------------------------------------------------------------------------------------------- */

#define COLOR_3DLIGHT_ 22
#define DSTINVERT_ 0x00550009u
/* A line of the colour `color` (a system colour index). */
static void edge_bar(HANDLE hdc, int color, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  HANDLE old = SelectObject(hdc, GetSysColorBrush(color));
  PatBlt(hdc, x, y, w, h, PATCOPY_);
  SelectObject(hdc, old);
}
/* One ring of a frame: top-left and bottom-right colours, on the sides `sides` (BF_LEFT 1, BF_TOP 2, BF_RIGHT 4, BF_BOTTOM 8); the rectangle shrinks by it. */
static void edge_ring(HANDLE hdc, RECT_ *r, int top_left, int bottom_right, UINT sides) {
  const int w = r->right - r->left, h = r->bottom - r->top;
  if (sides & 2) edge_bar(hdc, top_left, r->left, r->top, w, 1);
  if (sides & 1) edge_bar(hdc, top_left, r->left, r->top, 1, h);
  if (sides & 8) edge_bar(hdc, bottom_right, r->left, r->bottom - 1, w, 1);
  if (sides & 4) edge_bar(hdc, bottom_right, r->right - 1, r->top, 1, h);
  if (sides & 1) ++r->left;
  if (sides & 2) ++r->top;
  if (sides & 4) --r->right;
  if (sides & 8) --r->bottom;
}
/* The rectangular edges of Windows (BDR_RAISEDOUTER 1, SUNKENOUTER 2, RAISEDINNER 4, SUNKENINNER 8; BF_MIDDLE fills, BF_ADJUST returns the inner rectangle,
 * BF_FLAT/BF_MONO draw both rings in one colour). Diagonal edges (BF_DIAGONAL) are not drawn. */
EXPORT BOOL WINAPI DrawEdge(HANDLE hdc, RECT_ *rect, UINT edge, UINT flags) {
  if (!hdc || !rect) return FALSE;
  if (flags & 0x10) return TRUE;      /* BF_DIAGONAL */
  RECT_ r = *rect;
  const UINT sides = flags & 0xF;
  const BOOL mono = (flags & 0x8000) != 0, flat = (flags & 0x4000) != 0;
  if (edge & 3) {
    if (mono) edge_ring(hdc, &r, COLOR_WINDOWFRAME_, COLOR_WINDOWFRAME_, sides);
    else if (flat) edge_ring(hdc, &r, COLOR_BTNSHADOW_, COLOR_BTNSHADOW_, sides);
    else if (edge & 1) edge_ring(hdc, &r, COLOR_3DLIGHT_, COLOR_3DDKSHADOW_, sides);
    else edge_ring(hdc, &r, COLOR_BTNSHADOW_, COLOR_BTNHILIGHT_, sides);
  }
  if (edge & 12) {
    if (mono) edge_ring(hdc, &r, COLOR_WINDOW_, COLOR_WINDOW_, sides);
    else if (flat) edge_ring(hdc, &r, COLOR_BTNFACE_, COLOR_BTNFACE_, sides);
    else if (edge & 4) edge_ring(hdc, &r, COLOR_BTNHILIGHT_, COLOR_BTNSHADOW_, sides);
    else edge_ring(hdc, &r, COLOR_3DDKSHADOW_, COLOR_3DLIGHT_, sides);
  }
  if ((flags & 0x800) && r.right > r.left && r.bottom > r.top) edge_bar(hdc, mono ? COLOR_WINDOW_ : COLOR_BTNFACE_, r.left, r.top, r.right - r.left, r.bottom - r.top);
  if (flags & 0x2000) *rect = r;      /* BF_ADJUST */
  return TRUE;
}
/* The frame controls as plain buttons: a raised (pushed: sunken) frame with the face filled. The glyphs (check marks, arrows, caption buttons) are not drawn. */
EXPORT BOOL WINAPI DrawFrameControl(HANDLE hdc, RECT_ *rect, UINT type, UINT state) {
  if (!hdc || !rect || type < 1 || type > 4) return FALSE;
  RECT_ r = *rect;
  const BOOL pushed = (state & 0x0200 /* DFCS_PUSHED */) || (state & 0x0400 /* DFCS_CHECKED */);
  const UINT edge = state & 0x4000 /* DFCS_FLAT */ ? 0 : pushed ? 0x2 | 0x8 /* EDGE_SUNKEN */ : 0x1 | 0x4 /* EDGE_RAISED */;
  return DrawEdge(hdc, &r, edge, 0xF | 0x800 | (state & 0x4000 ? 0x4000 : 0));
}
EXPORT BOOL WINAPI InvertRect(HANDLE hdc, const RECT_ *r) {
  if (!hdc || !r) return FALSE;
  return PatBlt(hdc, r->left, r->top, r->right - r->left, r->bottom - r->top, DSTINVERT_);
}

/* ---- DDE -------------------------------------------------------------------------------------------------------------------------------------------------------- */

/* DDEML is not available: initialisation fails (DMLERR_SYS_ERROR) and every other call fails as for an instance that was never initialised. */
#define DMLERR_DLL_NOT_INITIALIZED_ 0x4003u
#define DMLERR_SYS_ERROR_ 0x400Fu
EXPORT UINT WINAPI DdeInitializeW(DWORD *instance, void *callback, DWORD command, DWORD reserved) {
  (void)callback; (void)command; (void)reserved;
  if (!instance) return 0x4006u;      /* DMLERR_INVALIDPARAMETER */
  *instance = 0;
  return DMLERR_SYS_ERROR_;
}
EXPORT UINT WINAPI DdeInitializeA(DWORD *instance, void *callback, DWORD command, DWORD reserved) { return DdeInitializeW(instance, callback, command, reserved); }
EXPORT BOOL WINAPI DdeUninitialize(DWORD instance) { (void)instance; return FALSE; }
EXPORT UINT WINAPI DdeGetLastError(DWORD instance) { (void)instance; return DMLERR_DLL_NOT_INITIALIZED_; }
EXPORT HANDLE WINAPI DdeConnect(DWORD instance, HANDLE service, HANDLE topic, void *context) { (void)instance; (void)service; (void)topic; (void)context; return NULL; }
EXPORT BOOL WINAPI DdeDisconnect(HANDLE conversation) { (void)conversation; return FALSE; }
EXPORT HANDLE WINAPI DdeCreateStringHandleW(DWORD instance, LPCWSTR text, int code_page) { (void)instance; (void)text; (void)code_page; return NULL; }
EXPORT HANDLE WINAPI DdeCreateStringHandleA(DWORD instance, LPCSTR text, int code_page) { (void)instance; (void)text; (void)code_page; return NULL; }
EXPORT BOOL WINAPI DdeFreeStringHandle(DWORD instance, HANDLE string) { (void)instance; (void)string; return FALSE; }
EXPORT HANDLE WINAPI DdeClientTransaction(BYTE *data, DWORD size, HANDLE conversation, HANDLE item, UINT format, UINT type, DWORD timeout, DWORD *result) {
  (void)data; (void)size; (void)conversation; (void)item; (void)format; (void)type; (void)timeout;
  if (result) *result = 0;
  return NULL;
}
