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

/* BSTRs: a DWORD byte count, the characters, and a NUL character that the count leaves out. The pointer is to the first character. */

#define BSTR_MAX_BYTES (0xFFFFFFFFu - sizeof(DWORD) - sizeof(WCHAR))

static BSTR bstr_alloc_bytes(const void *source, UINT bytes) {
  if (bytes > BSTR_MAX_BYTES) return NULL;
  char *block = oa_alloc(sizeof(DWORD) + (SIZE_T)bytes + sizeof(WCHAR));
  if (!block) return NULL;
  *(DWORD *)block = bytes;
  if (source) memcpy(block + sizeof(DWORD), source, bytes);
  else memset(block + sizeof(DWORD), 0, bytes);
  *(WCHAR *)(block + sizeof(DWORD) + bytes) = 0;
  return (BSTR)(block + sizeof(DWORD));
}

EXPORT BSTR WINAPI SysAllocString(const OLECHAR *string) {
  if (!string) return NULL;
  return bstr_alloc_bytes(string, (UINT)(oa_wcslen(string) * sizeof(WCHAR)));
}
EXPORT BSTR WINAPI SysAllocStringLen(const OLECHAR *string, UINT length) {
  if (length > BSTR_MAX_BYTES / sizeof(WCHAR)) return NULL;
  return bstr_alloc_bytes(string, length * (UINT)sizeof(WCHAR));
}
EXPORT BSTR WINAPI SysAllocStringByteLen(LPCSTR string, UINT length) { return bstr_alloc_bytes(string, length); }
EXPORT void WINAPI SysFreeString(BSTR string) { if (string) oa_free((char *)string - sizeof(DWORD)); }
EXPORT UINT WINAPI SysStringByteLen(BSTR string) { return string ? *(DWORD *)((char *)string - sizeof(DWORD)) : 0; }
EXPORT UINT WINAPI SysStringLen(BSTR string) { return SysStringByteLen(string) / sizeof(WCHAR); }
EXPORT INT WINAPI SysReAllocStringLen(BSTR *string, const OLECHAR *source, UINT length) {
  if (!string) return FALSE;
  if (length > BSTR_MAX_BYTES / sizeof(WCHAR)) return FALSE;
  BSTR old = *string;
  /* The source may point into the old string, so the new one is made first. */
  BSTR fresh = bstr_alloc_bytes(NULL, length * (UINT)sizeof(WCHAR));
  if (!fresh) return FALSE;
  if (source) {
    UINT keep = length;
    memcpy(fresh, source, keep * sizeof(WCHAR));
  } else if (old) {
    UINT keep = SysStringLen(old);
    if (keep > length) keep = length;
    memcpy(fresh, old, keep * sizeof(WCHAR));
  }
  SysFreeString(old);
  *string = fresh;
  return TRUE;
}
EXPORT INT WINAPI SysReAllocString(BSTR *string, const OLECHAR *source) {
  if (!string || !source) return FALSE;
  return SysReAllocStringLen(string, source, (UINT)oa_wcslen(source));
}
