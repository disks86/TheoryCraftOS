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

/* Keyboard layouts of user32.dll (included by user32.c): the layout handles and the translations that depend on the layout (MapVirtualKey, VkKeyScan, ToUnicode, GetKeyNameText) are
 * win32k's (src/Process/Win32k/Win32kUserMiscKeyboard.cpp, where the US English layout 00000409 has the tables). The A functions convert in the ANSI code page of user32 (UTF-8). */

SVC NtUserGetKeyboardLayout(ULONG_PTR tid);
SVC NtUserGetKeyboardLayoutList(ULONG_PTR size, ULONG_PTR list);
SVC NtUserGetKeyboardLayoutName(ULONG_PTR name);
SVC NtUserGetKeyboardType(ULONG_PTR type);
SVC NtUserActivateKeyboardLayout(ULONG_PTR hkl, ULONG_PTR flags);
SVC NtUserLoadKeyboardLayoutEx(ULONG_PTR file, ULONG_PTR offset, ULONG_PTR old, ULONG_PTR klid_name, ULONG_PTR klid, ULONG_PTR flags);
SVC NtUserUnloadKeyboardLayout(ULONG_PTR hkl);
SVC NtUserMapVirtualKeyEx(ULONG_PTR code, ULONG_PTR type, ULONG_PTR tid, ULONG_PTR hkl);
SVC NtUserVkKeyScanEx(ULONG_PTR ch, ULONG_PTR hkl);
SVC NtUserToUnicodeEx(ULONG_PTR vk, ULONG_PTR scan, ULONG_PTR state, ULONG_PTR buffer, ULONG_PTR size, ULONG_PTR flags, ULONG_PTR hkl);
SVC NtUserGetKeyNameText(ULONG_PTR lparam, ULONG_PTR buffer, ULONG_PTR size);

EXPORT HANDLE WINAPI GetKeyboardLayout(DWORD tid) { return (HANDLE)NtUserGetKeyboardLayout(tid); }
EXPORT int WINAPI GetKeyboardLayoutList(int size, HANDLE *list) { return (int)NtUserGetKeyboardLayoutList((ULONG_PTR)(LONG_PTR)size, (ULONG_PTR)list); }
EXPORT BOOL WINAPI GetKeyboardLayoutNameW(LPWSTR name) {
  if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return BOOL_OF(NtUserGetKeyboardLayoutName((ULONG_PTR)name));
}
EXPORT BOOL WINAPI GetKeyboardLayoutNameA(LPSTR name) {
  WCHAR wide[9];      /* KL_NAMELENGTH */
  if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!GetKeyboardLayoutNameW(wide)) return FALSE;
  return WideCharToMultiByte(65001, 0, wide, -1, name, 9, NULL, NULL) > 0;
}
EXPORT int WINAPI GetKeyboardType(int type) { return (int)NtUserGetKeyboardType((ULONG_PTR)(LONG_PTR)type); }
EXPORT HANDLE WINAPI ActivateKeyboardLayout(HANDLE hkl, UINT flags) { return (HANDLE)NtUserActivateKeyboardLayout((ULONG_PTR)hkl, flags); }
EXPORT HANDLE WINAPI LoadKeyboardLayoutW(LPCWSTR klid, UINT flags) {
  UNICODE_STRING us;
  if (!klid) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  us_name(&us, klid);
  return (HANDLE)NtUserLoadKeyboardLayoutEx(0, 0, 0, (ULONG_PTR)&us, 0, flags);
}
EXPORT HANDLE WINAPI LoadKeyboardLayoutA(LPCSTR klid, UINT flags) {
  WCHAR *wide = wide_of(klid);
  if (!wide) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  HANDLE hkl = LoadKeyboardLayoutW(wide, flags);
  wide_free(wide, klid);
  return hkl;
}
EXPORT HANDLE WINAPI LoadKeyboardLayoutEx(HANDLE old, LPCWSTR klid, UINT flags) {
  UNICODE_STRING us;
  if (!klid) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  us_name(&us, klid);
  return (HANDLE)NtUserLoadKeyboardLayoutEx(0, 0, (ULONG_PTR)old, (ULONG_PTR)&us, 0, flags);
}
EXPORT BOOL WINAPI UnloadKeyboardLayout(HANDLE hkl) { return BOOL_OF(NtUserUnloadKeyboardLayout((ULONG_PTR)hkl)); }

EXPORT UINT WINAPI MapVirtualKeyExW(UINT code, UINT type, HANDLE hkl) { return (UINT)NtUserMapVirtualKeyEx(code, type, 0, (ULONG_PTR)hkl); }
EXPORT UINT WINAPI MapVirtualKeyW(UINT code, UINT type) { return MapVirtualKeyExW(code, type, GetKeyboardLayout(0)); }
/* MAPVK_VK_TO_CHAR answers a character in the low word: the A form gives its ANSI byte (the US layout only makes ASCII, which is the same in both). */
EXPORT UINT WINAPI MapVirtualKeyExA(UINT code, UINT type, HANDLE hkl) {
  UINT result = MapVirtualKeyExW(code, type, hkl);
  if (type == 2 /* MAPVK_VK_TO_CHAR */ && (result & 0xFFFF) > 0x7F) {
    WCHAR wide = (WCHAR)result;
    char narrow = '?';
    WideCharToMultiByte(65001, 0, &wide, 1, &narrow, 1, NULL, NULL);
    result = (result & 0xFFFF0000u) | (BYTE)narrow;
  }
  return result;
}
EXPORT UINT WINAPI MapVirtualKeyA(UINT code, UINT type) { return MapVirtualKeyExA(code, type, GetKeyboardLayout(0)); }

EXPORT SHORT WINAPI VkKeyScanExW(WCHAR ch, HANDLE hkl) { return (SHORT)NtUserVkKeyScanEx(ch, (ULONG_PTR)hkl); }
EXPORT SHORT WINAPI VkKeyScanW(WCHAR ch) { return VkKeyScanExW(ch, GetKeyboardLayout(0)); }
EXPORT SHORT WINAPI VkKeyScanExA(CHAR ch, HANDLE hkl) {
  WCHAR wide = 0;
  if ((BYTE)ch < 0x80) wide = (BYTE)ch;
  else if (MultiByteToWideChar(65001, 0, &ch, 1, &wide, 1) != 1) return -1;
  return VkKeyScanExW(wide, hkl);
}
EXPORT SHORT WINAPI VkKeyScanA(CHAR ch) { return VkKeyScanExA(ch, GetKeyboardLayout(0)); }
/* OemKeyScan: the scan code of an OEM ASCII character in the low word and the shift state in the high word; -1 for a character the keyboard cannot type. */
EXPORT DWORD WINAPI OemKeyScan(WORD oem) {
  const SHORT vk = VkKeyScanW(oem);
  if (vk == -1) return 0xFFFFFFFFu;
  const UINT scan = MapVirtualKeyW((BYTE)vk, 0 /* MAPVK_VK_TO_VSC */);
  return scan ? ((DWORD)(vk >> 8 & 0xFF) << 16 | scan) : 0xFFFFFFFFu;
}

EXPORT int WINAPI ToUnicodeEx(UINT vk, UINT scan, const BYTE *state, LPWSTR buffer, int size, UINT flags, HANDLE hkl) {
  if (!state) return 0;
  return (int)NtUserToUnicodeEx(vk, scan, (ULONG_PTR)state, (ULONG_PTR)buffer, (ULONG_PTR)(LONG_PTR)size, flags, (ULONG_PTR)hkl);
}
EXPORT int WINAPI ToUnicode(UINT vk, UINT scan, const BYTE *state, LPWSTR buffer, int size, UINT flags) { return ToUnicodeEx(vk, scan, state, buffer, size, flags, GetKeyboardLayout(0)); }
/* ToAscii: the characters as bytes in the low bytes of a WORD (one character per byte, at most two). */
EXPORT int WINAPI ToAsciiEx(UINT vk, UINT scan, const BYTE *state, WORD *out, UINT flags, HANDLE hkl) {
  WCHAR wide[2];
  char narrow[2];
  if (!out) return 0;
  const int n = ToUnicodeEx(vk, scan, state, wide, 2, flags, hkl);
  if (n <= 0) return n;
  if (WideCharToMultiByte(65001, 0, wide, 1, narrow, 1, NULL, NULL) != 1) return 0;
  *out = (BYTE)narrow[0];
  return 1;
}
EXPORT int WINAPI ToAscii(UINT vk, UINT scan, const BYTE *state, WORD *out, UINT flags) { return ToAsciiEx(vk, scan, state, out, flags, GetKeyboardLayout(0)); }

EXPORT int WINAPI GetKeyNameTextW(LONG lparam, LPWSTR buffer, int size) {
  if (!buffer || size <= 0) return 0;
  return (int)NtUserGetKeyNameText((ULONG_PTR)(ULONG)lparam, (ULONG_PTR)buffer, (ULONG_PTR)(LONG_PTR)size);
}
EXPORT int WINAPI GetKeyNameTextA(LONG lparam, LPSTR buffer, int size) {
  WCHAR wide[64];
  if (!buffer || size <= 0) return 0;
  int n = GetKeyNameTextW(lparam, wide, 64);
  if (n > size - 1) n = size - 1;      /* (the name is cut to fit, as the W function does) */
  int written = 0;
  while (n > 0 && (written = WideCharToMultiByte(65001, 0, wide, n, buffer, size - 1, NULL, NULL)) <= 0) --n;      /* (a character that does not fit whole is left out) */
  buffer[written > 0 ? written : 0] = 0;
  return written > 0 ? written : 0;
}

/* Mouse settings and the beep that the window manager keeps (src/Process/Win32k/Win32kUserMiscSettings.cpp, Win32kUserMiscShell.cpp). */
SVC NtUserGetDoubleClickTime(void);
SVC NtUserSetDoubleClickTime(ULONG_PTR ms);
SVC NtUserSwapMouseButton(ULONG_PTR swap);
SVC NtUserMessageBeep(ULONG_PTR type);
EXPORT UINT WINAPI GetDoubleClickTime(void) { return (UINT)NtUserGetDoubleClickTime(); }
EXPORT BOOL WINAPI SetDoubleClickTime(UINT ms) { return BOOL_OF(NtUserSetDoubleClickTime(ms)); }
EXPORT BOOL WINAPI SwapMouseButton(BOOL swap) { return BOOL_OF(NtUserSwapMouseButton((ULONG_PTR)swap)); }
EXPORT BOOL WINAPI MessageBeep(UINT type) { return BOOL_OF(NtUserMessageBeep(type)); }
