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

/* kernel32.dll: GlobalAlloc / LocalAlloc and their relatives. A fixed block is a heap block and its own handle. A moveable block is a handle table entry (flags and the address
 * of the data); the handle is the address of the entry's pointer, which is 8 more than a multiple of 16, so it can be told from a pointer. The data is a heap block that
 * remembers its handle (the heap's user value) so that GlobalHandle can find it. The table holds 0x10000 entries. The two families differ in a few places (the error codes
 * for bad pointers, what a fixed block can be turned into), as they do on Windows. */

IMPORT BOOLEAN NTAPI RtlGetUserInfoHeap(PVOID heap, ULONG flags, PVOID memory, PVOID *value, PULONG user_flags);
IMPORT BOOLEAN NTAPI RtlSetUserValueHeap(PVOID heap, ULONG flags, PVOID memory, PVOID value);
IMPORT BOOLEAN NTAPI RtlSetUserFlagsHeap(PVOID heap, ULONG flags, PVOID memory, ULONG clear, ULONG set);

IMPORT BOOL WINAPI SwitchToThread(void);
#define MEM_MAX_ENTRIES 0x10000
#define MEM_FLAG_USED 1
#define MEM_FLAG_MOVEABLE 2
#define MEM_FLAG_DISCARDABLE 4
#define MEM_FLAG_DISCARDED 8
#define MEM_FLAG_DDESHARE 0x8000
#define MEM_FLAG_LOCKS 0xFF0000
#define MEM_HEAP_USER_VALUE 0x100      /* HEAP_SETTABLE_USER_VALUE */
#define MEM_HEAP_USER_FLAG1 0x200      /* HEAP_SETTABLE_USER_FLAG1: this block is the data of a moveable handle */
#define MEM_MODIFY 0x80
#define MEM_DISCARDABLE_BIT 0x100
#define MEM_DDESHARE_BIT 0x2000
#define MEM_INVALID_HANDLE_FLAGS 0x8000
#define MEM_BIG_BLOCK 0x4000      /* from this size on a block may be resized in place */

typedef struct { UINT_PTR flags; void *ptr; } MemEntry;
static MemEntry g_mem_entries[MEM_MAX_ENTRIES] __attribute__((aligned(16)));
static MemEntry *g_mem_free;      /* freed entries, linked through their flags field */
static ULONG g_mem_unused;        /* entries before this one have been used */
static volatile LONG g_mem_lock;
static void mem_lock(void) { while (__sync_lock_test_and_set(&g_mem_lock, 1)) SwitchToThread(); }
static void mem_unlock(void) { __sync_lock_release(&g_mem_lock); }

#define MEM_END (&g_mem_entries[MEM_MAX_ENTRIES])

/* Does the value have the shape of a handle (8 more than a multiple of 16)? And is it an entry of the table? */
static int mem_is_handle_shaped(const void *h) { return ((UINT_PTR)h & 15) == 8; }
static MemEntry *mem_entry_in_table(const void *h) {
  MemEntry *e = (MemEntry *)((const BYTE *)h - __builtin_offsetof(MemEntry, ptr));
  return (e >= g_mem_entries && e < MEM_END) ? e : NULL;
}
static int mem_owned(const void *p) { return p && RtlSizeHeap(heap(), 0, (PVOID)p) != (SIZE_T)-1; }

static MemEntry *mem_take_entry(void) {
  MemEntry *e = NULL;
  mem_lock();
  if (g_mem_free && g_mem_free != MEM_END) { e = g_mem_free; g_mem_free = (MemEntry *)e->flags; }
  else if (g_mem_unused < MEM_MAX_ENTRIES) e = &g_mem_entries[g_mem_unused++];
  mem_unlock();
  return e;
}
static void mem_give_entry(MemEntry *e) {
  mem_lock();
  e->ptr = NULL;
  e->flags = (UINT_PTR)(g_mem_free ? g_mem_free : MEM_END);      /* (never 0: a freed entry is told by its flags not being a flag set) */
  g_mem_free = e;
  mem_unlock();
}

static void *mem_data_alloc(MemEntry *e, SIZE_T size, int zero) {
  void *p = RtlAllocateHeap(heap(), MEM_HEAP_USER_VALUE | MEM_HEAP_USER_FLAG1 | (zero ? HEAP_ZERO_MEMORY : 0), size);
  if (p) RtlSetUserValueHeap(heap(), 0, p, &e->ptr);
  return p;
}

static HANDLE mem_alloc(int local, UINT flags, SIZE_T size) {
  const int zero = (flags & 0x40) != 0;
  if (!(flags & GMEM_MOVEABLE)) {
    void *p = RtlAllocateHeap(heap(), zero ? HEAP_ZERO_MEMORY : 0, size == 0 && !local ? 1 : size);
    if (!p) set_error(ERROR_NOT_ENOUGH_MEMORY);
    return p;
  }
  MemEntry *e = mem_take_entry();
  if (!e) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  e->flags = MEM_FLAG_USED | MEM_FLAG_MOVEABLE | ((flags & MEM_DISCARDABLE_BIT) ? MEM_FLAG_DISCARDABLE : 0) | ((flags & MEM_DDESHARE_BIT) ? MEM_FLAG_DDESHARE : 0);
  e->ptr = NULL;
  if (size == 0) e->flags |= MEM_FLAG_DISCARDED;
  else if (!(e->ptr = mem_data_alloc(e, size, zero))) { mem_give_entry(e); set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  return &e->ptr;
}

/* The error for a pointer that is not a block of the heap. */
static DWORD mem_bad_pointer_error(int local) { return local ? ERROR_NOACCESS : ERROR_INVALID_HANDLE; }

static HANDLE mem_free(int local, HANDLE h) {
  if (!h) return NULL;
  if (mem_is_handle_shaped(h)) {
    MemEntry *e = mem_entry_in_table(h);
    if (!e || !(e->flags & MEM_FLAG_USED)) { set_error(ERROR_INVALID_HANDLE); return h; }
    if (e->ptr) RtlFreeHeap(heap(), 0, e->ptr);
    mem_give_entry(e);
    return NULL;
  }
  if (!RtlFreeHeap(heap(), 0, h)) { set_error(ERROR_NOACCESS); return h; }
  return NULL;
}

static SIZE_T mem_size(HANDLE h) {
  if (mem_is_handle_shaped(h)) {
    MemEntry *e = mem_entry_in_table(h);
    if (e && (e->flags & MEM_FLAG_USED)) return e->ptr ? RtlSizeHeap(heap(), 0, e->ptr) : 0;
  } else if (mem_owned(h)) {
    return RtlSizeHeap(heap(), 0, h);
  }
  set_error(ERROR_INVALID_HANDLE);
  return 0;
}

static UINT mem_flags(int local, HANDLE h) {
  if (mem_is_handle_shaped(h)) {
    MemEntry *e = mem_entry_in_table(h);
    if (!e || !(e->flags & MEM_FLAG_USED)) { set_error(ERROR_INVALID_HANDLE); return MEM_INVALID_HANDLE_FLAGS; }
    return (UINT)((e->flags & MEM_FLAG_LOCKS) >> 16) | ((e->flags & MEM_FLAG_DISCARDABLE) ? MEM_DISCARDABLE_BIT : 0) | ((e->flags & MEM_FLAG_DISCARDED) ? 0x4000 : 0) |
           ((e->flags & MEM_FLAG_DDESHARE) ? MEM_DDESHARE_BIT : 0);
  }
  if (mem_owned(h)) return 0;
  set_error(mem_bad_pointer_error(local));
  return MEM_INVALID_HANDLE_FLAGS;
}

static LPVOID mem_lock_block(int local, HANDLE h) {
  if (mem_is_handle_shaped(h)) {
    MemEntry *e = mem_entry_in_table(h);
    if (!e || !(e->flags & MEM_FLAG_USED)) { set_error(ERROR_INVALID_HANDLE); return NULL; }
    if (!e->ptr) { set_error(ERROR_DISCARDED); return NULL; }
    if ((e->flags & MEM_FLAG_LOCKS) != MEM_FLAG_LOCKS) e->flags += 0x10000;
    return e->ptr;
  }
  if (mem_owned(h)) return h;
  if (!local) set_error(ERROR_INVALID_HANDLE);
  return NULL;
}

static BOOL mem_unlock_block(int local, HANDLE h) {
  if (mem_is_handle_shaped(h)) {
    MemEntry *e = mem_entry_in_table(h);
    if (!e || !(e->flags & MEM_FLAG_USED)) { set_error(ERROR_INVALID_HANDLE); return !local; }
    if (!(e->flags & MEM_FLAG_LOCKS)) { set_error(ERROR_NOT_LOCKED); return FALSE; }
    e->flags -= 0x10000;
    if (e->flags & MEM_FLAG_LOCKS) return TRUE;
    set_error(NO_ERROR);
    return FALSE;
  }
  if (local) { set_error(ERROR_NOT_LOCKED); return FALSE; }
  return TRUE;
}

static HANDLE mem_handle_of(int local, LPCVOID p) {
  PVOID value = NULL;
  ULONG user_flags = 0;
  if (!mem_owned(p) || !RtlGetUserInfoHeap(heap(), 0, (PVOID)p, &value, &user_flags)) { set_error(mem_bad_pointer_error(local)); return NULL; }
  return (user_flags & MEM_HEAP_USER_FLAG1) ? value : (HANDLE)p;
}

/* A fixed block moved to new memory of `size` bytes (the contents that fit are kept). */
static void *mem_move_block(void *old, SIZE_T size, int zero) {
  const SIZE_T have = RtlSizeHeap(heap(), 0, old);
  void *p = RtlAllocateHeap(heap(), zero ? HEAP_ZERO_MEMORY : 0, size);
  if (!p) return NULL;
  memcpy(p, old, have < size ? have : size);
  RtlFreeHeap(heap(), 0, old);
  return p;
}

static HANDLE mem_realloc(int local, HANDLE h, SIZE_T size, UINT flags) {
  if (!h) return NULL;
  const int zero = (flags & 0x40) != 0, moveable = (flags & GMEM_MOVEABLE) != 0, modify = (flags & MEM_MODIFY) != 0, discardable = (flags & MEM_DISCARDABLE_BIT) != 0;
  if (mem_is_handle_shaped(h)) {
    MemEntry *e = mem_entry_in_table(h);
    if (!e || !(e->flags & MEM_FLAG_USED)) { set_error(ERROR_INVALID_HANDLE); return NULL; }
    if (modify) { if (discardable) e->flags |= MEM_FLAG_DISCARDABLE; return h; }
    const int locked = (e->flags & MEM_FLAG_LOCKS) != 0;
    if (discardable) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (size == 0) {
      if (!moveable || locked) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
      if (e->ptr) RtlFreeHeap(heap(), 0, e->ptr);
      e->ptr = NULL;
      e->flags |= MEM_FLAG_DISCARDED;
      return h;
    }
    if (!e->ptr) {
      if (!(e->ptr = mem_data_alloc(e, size, zero))) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
      e->flags &= ~(UINT_PTR)MEM_FLAG_DISCARDED;
      return h;
    }
    const SIZE_T have = RtlSizeHeap(heap(), 0, e->ptr);
    const int in_place = locked && !moveable && size > have;      /* (a locked block that may not move grows where it is, or not at all) */
    void *p = RtlReAllocateHeap(heap(), (zero ? HEAP_ZERO_MEMORY : 0) | (in_place ? 0x10 /* HEAP_REALLOC_IN_PLACE_ONLY */ : 0), e->ptr, size);
    if (!p) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    e->ptr = p;
    return h;
  }
  if (!mem_owned(h)) { set_error(ERROR_NOACCESS); return NULL; }
  const SIZE_T have = RtlSizeHeap(heap(), 0, h);
  if (modify) {
    if (moveable && !local) {      /* a fixed block becomes moveable: its contents go to a block of their own, which the new entry points to */
      MemEntry *e = mem_take_entry();
      if (!e) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
      e->flags = MEM_FLAG_USED | MEM_FLAG_MOVEABLE | (discardable ? MEM_FLAG_DISCARDABLE : 0);
      e->ptr = NULL;
      if (have == 0) e->flags |= MEM_FLAG_DISCARDED;
      else {
        void *p = mem_data_alloc(e, have, 0);
        if (!p) { mem_give_entry(e); set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
        memcpy(p, h, have);
        RtlFreeHeap(heap(), 0, h);
        e->ptr = p;
      }
      return &e->ptr;
    }
    return h;
  }
  if (discardable) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  if (size == have && moveable) return h;      /* nothing to change */
  if (have >= MEM_BIG_BLOCK) {      /* a big block is resized where it is, if it can be */
    void *p = RtlReAllocateHeap(heap(), (zero ? HEAP_ZERO_MEMORY : 0) | 0x10 /* HEAP_REALLOC_IN_PLACE_ONLY */, h, size);
    if (p) return p;
    if (!moveable) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  } else if (!moveable) {
    set_error(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
  }
  void *p = mem_move_block(h, size, zero);
  if (!p) set_error(ERROR_NOT_ENOUGH_MEMORY);
  return p;
}

EXPORT HLOCAL WINAPI LocalAlloc(UINT flags, SIZE_T bytes) { return mem_alloc(1, flags, bytes); }
EXPORT HLOCAL WINAPI LocalReAlloc(HLOCAL m, SIZE_T bytes, UINT flags) { return mem_realloc(1, m, bytes, flags); }
EXPORT HLOCAL WINAPI LocalFree(HLOCAL m) { return mem_free(1, m); }
EXPORT SIZE_T WINAPI LocalSize(HLOCAL m) { return mem_size(m); }
EXPORT LPVOID WINAPI LocalLock(HLOCAL m) { return mem_lock_block(1, m); }
EXPORT BOOL WINAPI LocalUnlock(HLOCAL m) { return mem_unlock_block(1, m); }
EXPORT UINT WINAPI LocalFlags(HLOCAL m) { return mem_flags(1, m); }
EXPORT HLOCAL WINAPI LocalHandle(LPCVOID p) { return mem_handle_of(1, p); }
EXPORT HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T bytes) { return mem_alloc(0, flags, bytes); }
EXPORT HGLOBAL WINAPI GlobalReAlloc(HGLOBAL m, SIZE_T bytes, UINT flags) { return mem_realloc(0, m, bytes, flags); }
EXPORT HGLOBAL WINAPI GlobalFree(HGLOBAL m) { return mem_free(0, m); }
EXPORT SIZE_T WINAPI GlobalSize(HGLOBAL m) { return mem_size(m); }
EXPORT LPVOID WINAPI GlobalLock(HGLOBAL m) { return mem_lock_block(0, m); }
EXPORT BOOL WINAPI GlobalUnlock(HGLOBAL m) { return mem_unlock_block(0, m); }
EXPORT UINT WINAPI GlobalFlags(HGLOBAL m) { return mem_flags(0, m); }
EXPORT HGLOBAL WINAPI GlobalHandle(LPCVOID p) { return mem_handle_of(0, p); }
