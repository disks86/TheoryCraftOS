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

/* BSTRs: a DWORD byte count (in the last four of eight header bytes, so that the characters are aligned to 8), the characters, and a NUL character that the count leaves out. The pointer is to the first character. */

#define BSTR_HEADER 8u
#define BSTR_MAX_BYTES (0xFFFFFFFFu - BSTR_HEADER - sizeof(WCHAR))
#define BSTR_BLOCK(b) ((char *)(b) - BSTR_HEADER)

/* The terminator is the whole WCHAR after the last byte, so an odd byte count has a zero padding byte as well. */
static SIZE_T bstr_block_size(UINT bytes) { return BSTR_HEADER + (((SIZE_T)bytes + 1) & ~(SIZE_T)1) + sizeof(WCHAR); }
static void bstr_terminate(char *block, UINT bytes) {
  *(DWORD *)(block + BSTR_HEADER - sizeof(DWORD)) = bytes;
  memset(block + BSTR_HEADER + bytes, 0, bstr_block_size(bytes) - BSTR_HEADER - bytes);
}

static BSTR bstr_alloc_bytes(const void *source, UINT bytes) {
  if (bytes > BSTR_MAX_BYTES - 1) return NULL;
  char *block = oa_alloc(bstr_block_size(bytes));
  if (!block) return NULL;
  if (source) memcpy(block + BSTR_HEADER, source, bytes);
  else memset(block + BSTR_HEADER, 0, bytes);
  bstr_terminate(block, bytes);
  return (BSTR)(block + BSTR_HEADER);
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
EXPORT void WINAPI SysFreeString(BSTR string) { if (string) oa_free(BSTR_BLOCK(string)); }
EXPORT UINT WINAPI SysStringByteLen(BSTR string) { return string ? *(DWORD *)((char *)string - sizeof(DWORD)) : 0; }
EXPORT UINT WINAPI SysStringLen(BSTR string) { return SysStringByteLen(string) / sizeof(WCHAR); }
EXPORT INT WINAPI SysReAllocStringLen(BSTR *string, const OLECHAR *source, UINT length) {
  if (!string) return FALSE;
  if (length > BSTR_MAX_BYTES / sizeof(WCHAR) - 1) return FALSE;
  BSTR old = *string;
  const UINT bytes = length * (UINT)sizeof(WCHAR);
  const BOOL aliased = old && source && (const char *)source >= (const char *)old - BSTR_HEADER && (const char *)source <= (const char *)old + SysStringByteLen(old) + sizeof(WCHAR);
  if (old && !aliased) {
    /* in place: the block is resized (a smaller one stays where it is) */
    char *block = oa_realloc(BSTR_BLOCK(old), bstr_block_size(bytes));
    if (!block) return FALSE;
    if (source) memcpy(block + BSTR_HEADER, source, bytes);
    bstr_terminate(block, bytes);
    *string = (BSTR)(block + BSTR_HEADER);
    return TRUE;
  }
  /* The source points into the old string (or there is none): the new one is made first. */
  BSTR fresh = bstr_alloc_bytes(NULL, bytes);
  if (!fresh) return FALSE;
  if (source) memcpy(fresh, source, bytes);
  SysFreeString(old);
  *string = fresh;
  return TRUE;
}
EXPORT INT WINAPI SysReAllocString(BSTR *string, const OLECHAR *source) {
  if (!string || !source) return FALSE;
  return SysReAllocStringLen(string, source, (UINT)oa_wcslen(source));
}
