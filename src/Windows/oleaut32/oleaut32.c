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

/* oleaut32.dll for TheoryCraftOS: the BSTR functions (a length in bytes before the characters, allocated from the process heap like the BSTRs of ole32.dll) and the error
 * object functions, which call ole32.dll's implementation. The rest of the library (VARIANT, SAFEARRAY, type libraries) is not implemented yet. */
#include "../tcos_win.h"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");
typedef WCHAR OLECHAR;
typedef OLECHAR *BSTR;
typedef const OLECHAR *LPCOLESTR;

IMPORT HANDLE WINAPI GetProcessHeap(void);
IMPORT LPVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes);
IMPORT BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, LPVOID memory);

static void copy_bytes(void *to, const void *from, size_t_ n) {
  BYTE *d = to;
  const BYTE *s = from;
  while (n--) *d++ = *s++;
}

/* A string of `bytes` bytes (a multiple of 2 for wide text) with the end mark, and the length in front. */
static BSTR alloc_bytes(const void *source, UINT bytes) {
  if (bytes > 0xFFFFFFFFu - sizeof(DWORD) - sizeof(OLECHAR) - 3) return NULL;
  DWORD *block = HeapAlloc(GetProcessHeap(), 0x8 /* HEAP_ZERO_MEMORY */, sizeof(DWORD) + bytes + sizeof(OLECHAR));
  if (!block) return NULL;
  block[0] = bytes;
  if (source) copy_bytes(block + 1, source, bytes);
  return (BSTR)(block + 1);
}

EXPORT BSTR WINAPI SysAllocStringLen(const OLECHAR *s, UINT chars) {
  if (chars > 0x7FFFFFF0u) return NULL;
  return alloc_bytes(s, chars * (UINT)sizeof(OLECHAR));
}
EXPORT BSTR WINAPI SysAllocString(const OLECHAR *s) {
  size_t_ n = 0;
  if (!s) return NULL;
  while (s[n]) n++;
  return SysAllocStringLen(s, (UINT)n);
}
EXPORT BSTR WINAPI SysAllocStringByteLen(const char *s, UINT bytes) { return alloc_bytes(s, bytes); }
EXPORT void WINAPI SysFreeString(BSTR b) { if (b) HeapFree(GetProcessHeap(), 0, (DWORD *)b - 1); }
EXPORT UINT WINAPI SysStringLen(BSTR b) { return b ? ((DWORD *)b)[-1] / sizeof(OLECHAR) : 0; }
EXPORT UINT WINAPI SysStringByteLen(BSTR b) { return b ? ((DWORD *)b)[-1] : 0; }
EXPORT BOOL WINAPI SysReAllocStringLen(BSTR *b, const OLECHAR *s, UINT chars) {
  BSTR fresh = SysAllocStringLen(s, chars);
  if (!fresh) return FALSE;
  if (!s && *b) copy_bytes(fresh, *b, (((DWORD *)*b)[-1] < chars * sizeof(OLECHAR) ? ((DWORD *)*b)[-1] : chars * sizeof(OLECHAR)));
  SysFreeString(*b);
  *b = fresh;
  return TRUE;
}
EXPORT BOOL WINAPI SysReAllocString(BSTR *b, const OLECHAR *s) {
  size_t_ n = 0;
  if (!s) return FALSE;
  while (s[n]) n++;
  return SysReAllocStringLen(b, s, (UINT)n);
}

/* ---- the error object (ole32.dll holds the per-thread object) ---------------------------------------------------------------------------------------------------- */

IMPORT HMODULE WINAPI LoadLibraryA(LPCSTR name);
IMPORT FARPROC WINAPI GetProcAddress(HMODULE module, LPCSTR name);

static FARPROC ole32_proc(const char *name) {
  HMODULE m = LoadLibraryA("ole32.dll");
  return m ? GetProcAddress(m, name) : NULL;
}
EXPORT LONG WINAPI CreateErrorInfo(void **out) {
  LONG (WINAPI *f)(void **) = (void *)ole32_proc("CreateErrorInfo");
  return f ? f(out) : (LONG)0x80004001;
}
EXPORT LONG WINAPI SetErrorInfo(ULONG reserved, void *info) {
  LONG (WINAPI *f)(ULONG, void *) = (void *)ole32_proc("SetErrorInfo");
  return f ? f(reserved, info) : (LONG)0x80004001;
}
EXPORT LONG WINAPI GetErrorInfo(ULONG reserved, void **out) {
  LONG (WINAPI *f)(ULONG, void **) = (void *)ole32_proc("GetErrorInfo");
  return f ? f(reserved, out) : (LONG)0x80004001;
}
