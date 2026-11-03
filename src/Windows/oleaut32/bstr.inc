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

/* The BSTR cache of Windows: freed strings are kept in rings of 6 per 16-byte size class and handed out again first-in first-out. A string freed twice, or read after
 * its free, therefore still works as long as it sits in the cache (programs depend on that, and Wine's tests check it). OANOCACHE in the environment, or SetOaNoCache,
 * turns it off; OACleanup empties it. */
#define BSTR_CLASS 16u
#define BSTR_CLASSES 4096u
#define BSTR_RING 6u
#define BSTR_SEARCH 2u
static struct { unsigned head, count; void *block[BSTR_RING]; } bstr_cache[BSTR_CLASSES];
static volatile LONG bstr_cache_lock;
static volatile LONG bstr_cache_state;      /* 0: not looked at yet, 1: on, 2: off */

static void bstr_lock(void) { while (__sync_lock_test_and_set(&bstr_cache_lock, 1)) Sleep(0); }
static void bstr_unlock(void) { __sync_lock_release(&bstr_cache_lock); }
static BOOL bstr_cache_on(void) {
  if (bstr_cache_state == 0) bstr_cache_state = GetEnvironmentVariableA("OANOCACHE", NULL, 0) ? 2 : 1;
  return bstr_cache_state == 1;
}
static void bstr_cache_flush(void) {
  bstr_lock();
  for (unsigned c = 0; c < BSTR_CLASSES; c++) {
    while (bstr_cache[c].count) { oa_free(bstr_cache[c].block[bstr_cache[c].head]); bstr_cache[c].head = (bstr_cache[c].head + 1) % BSTR_RING; bstr_cache[c].count--; }
  }
  bstr_unlock();
}
static void *bstr_cache_take(SIZE_T size) {
  if (!bstr_cache_on()) return NULL;
  void *found = NULL;
  bstr_lock();
  /* (a class holds blocks from its size up to the next class's, so the first class that can fit is the one of `size` itself, and the block is checked) */
  for (unsigned c = (unsigned)(size / BSTR_CLASS), end = c + BSTR_SEARCH + 1; c < end && c < BSTR_CLASSES && !found; c++) {
    if (!bstr_cache[c].count || HeapSize(GetProcessHeap(), 0, bstr_cache[c].block[bstr_cache[c].head]) < size) continue;
    found = bstr_cache[c].block[bstr_cache[c].head];
    bstr_cache[c].head = (bstr_cache[c].head + 1) % BSTR_RING;
    bstr_cache[c].count--;
  }
  bstr_unlock();
  return found;
}
static void bstr_release(char *block) {
  if (bstr_cache_on()) {
    const SIZE_T size = HeapSize(GetProcessHeap(), 0, block);
    const SIZE_T c = size == (SIZE_T)-1 ? 0 : size / BSTR_CLASS;
    if (c && c < BSTR_CLASSES) {
      bstr_lock();
      unsigned i;
      for (i = 0; i < bstr_cache[c].count; i++) if (bstr_cache[c].block[(bstr_cache[c].head + i) % BSTR_RING] == block) break;
      if (i < bstr_cache[c].count) { bstr_unlock(); return; }      /* already there: a second free does nothing */
      if (bstr_cache[c].count < BSTR_RING) {
        bstr_cache[c].block[(bstr_cache[c].head + bstr_cache[c].count++) % BSTR_RING] = block;
        bstr_unlock();
        return;
      }
      bstr_unlock();
    }
  }
  oa_free(block);
}

static BSTR bstr_alloc_bytes(const void *source, UINT bytes) {
  if (bytes > BSTR_MAX_BYTES - 1) return NULL;
  char *block = bstr_cache_take(bstr_block_size(bytes));
  if (!block) block = oa_alloc(bstr_block_size(bytes));
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
EXPORT void WINAPI SysFreeString(BSTR string) { if (string) bstr_release(BSTR_BLOCK(string)); }
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
  if (source) {
    /* (a source inside the old string is not read past the old string's end) */
    UINT n = bytes;
    if (aliased && n > SysStringByteLen(old)) n = SysStringByteLen(old);
    memcpy(fresh, source, n);
  }
  SysFreeString(old);
  *string = fresh;
  return TRUE;
}
EXPORT INT WINAPI SysReAllocString(BSTR *string, const OLECHAR *source) {
  if (!string || !source) return FALSE;
  return SysReAllocStringLen(string, source, (UINT)oa_wcslen(source));
}
