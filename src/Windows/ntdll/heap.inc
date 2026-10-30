/* TheoryCraftOS - a minimal UEFI x86_64 operating system.
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


/* The heaps of ntdll.dll (included by ntdll.c): RtlCreateHeap and friends, laid out like the heap of Windows where programs can see it. A heap is reserved at a 64 KiB boundary and
 * starts with its header (the flags are at the offsets Windows has them); the first region is the heap itself, further regions are reserved when the heap grows. A region commits
 * its pages as they are needed and keeps its last page unused. Blocks are chunks [prev_size][size and flags][payload...] (the user pointer is the payload, 16-byte aligned; the
 * last 16 bytes of a block hold the user value and flags, and the size that was asked for sits in the 8 bytes after them). Big blocks of a growable heap are made of their own
 * pages (a header of 0x40 bytes first). A heap made with a maximum size cannot grow: it is one region of that size. */

#define CHUNK_USED 1ull
#define CHUNK_PREV_USED 2ull
#define CHUNK_SIZE_MASK 0x00000FFFFFFFFFF0ull
#define CHUNK_SIZE(c) ((c)->size_flags & CHUNK_SIZE_MASK)
#define CHUNK_BIG 8ull      /* a header of 16 bytes (a heap with global flags, or a call with checking flags), not 8 */
#define CHUNK_INFO 4ull      /* the block keeps user info (a value and flags) after its tail */
#define CHUNK_UNUSED_SHIFT 44      /* 8 bits: how many bytes of the payload are not asked for */
#define CHUNK_UFLAGS_SHIFT 52      /* 12 bits: the user flags (HEAP_SETTABLE_USER_FLAG1..3 are 0x200..0x800) */
typedef struct Chunk { ULONGLONG prev_size, size_flags; struct Chunk *next, *prev; } Chunk;

#define REGION_ALIGN 0x10000ull
#define INITIAL_COMMIT (0x400ull * sizeof(void *))
#define LARGE_MIN 0xFF000ull        /* a block of this size or more is a block of its own (growable heaps) */
#define LARGE_HEADER 0x40ull
#define LARGE_MAGIC 0x4C415247454B4C42ull
#define MIN_CHUNK 32ull
#define PAGE 0x1000ull
#define HEAP_MAGIC 0x50414548454B4F54ull
#define HEAP_FLAG_NO_SERIALIZE 1
#define HEAP_FLAG_GENERATE_EXCEPTIONS 4
#define HEAP_FLAG_ZERO_MEMORY 8
#define HEAP_FLAG_REALLOC_IN_PLACE 0x10

typedef struct Subheap { struct Subheap *next; BYTE *base; SIZE_T reserve, committed, limit; ULONG index; ULONG header; } Subheap;      /* (limit: how much of the reserve may be committed) */
typedef struct Large { struct Large *next; SIZE_T reserved, committed, asked; ULONGLONG user_value, user_flags, magic; ULONGLONG pad; } Large;      /* (0x40 bytes) */
typedef struct Heap {
  BYTE reserved0[0x10];
  ULONG ffeeffee, auto_flags;      /* 0x10: Windows keeps 0xFFEEFFEE here */
  BYTE reserved1[0x58];
  ULONG flags, force_flags;        /* 0x70 */
  ULONGLONG magic;
  SIZE_T maximum;                  /* a fixed heap's size; 0 for a growable one */
  Subheap *subheaps;
  Large *large;
  SIZE_T next_region;               /* the size of the next region reserved */
  ULONG compat, allocations;       /* HeapCompatibilityInformation (2: low fragmentation, switched on by use) */
  volatile LONG inner;             /* guards the structures */
  volatile LONG user_lock;         /* what HeapLock holds (and what serializes the calls) */
  ULONG_PTR user_owner;
  LONG user_depth;
  Subheap first;
  Chunk free_list;
  BYTE pad[0x800 - 0x100];      /* (the header is 0x800 bytes in all: the first chunk follows it) */
} Heap;

static Chunk *chunk_next(Chunk *c) { return (Chunk *)((BYTE *)c + CHUNK_SIZE(c)); }
static void free_unlink(Chunk *c) { c->prev->next = c->next; c->next->prev = c->prev; }
static void free_link(Heap *h, Chunk *c) { c->next = h->free_list.next; c->prev = &h->free_list; h->free_list.next->prev = c; h->free_list.next = c; }
static ULONGLONG round_up(ULONGLONG v, ULONGLONG to) { return (v + to - 1) & ~(to - 1); }
#define HF_TAIL 0x20u
#define HF_FREE 0x40u
#define HF_NO_COALESCE 0x80u
#define HF_SHARED 0x04000000u
#define HF_PAGE_ALLOCS 0x01000000u
#define TAIL_BYTES 16ull
static void fill_dwords(BYTE *at, SIZE_T bytes, ULONG pattern) { for (SIZE_T i = 0; i + 4 <= bytes; i += 4) *(ULONG *)(at + i) = pattern; }
static int check_dwords(const BYTE *at, SIZE_T bytes, ULONG pattern) { for (SIZE_T i = 0; i + 4 <= bytes; i += 4) if (*(const ULONG *)(at + i) != pattern) return 0; return 1; }

/* Makes `c` (size bytes, not in any list) a free chunk and puts the bookkeeping of its neighbour right. */
static void make_free(Heap *h, Chunk *c, ULONGLONG size, int prev_used) {
  c->size_flags = size | (prev_used ? CHUNK_PREV_USED : 0);
  Chunk *after = (Chunk *)((BYTE *)c + size);
  after->prev_size = size;
  after->size_flags &= ~CHUNK_PREV_USED;
  free_link(h, c);
  if (h->flags & HF_FREE) fill_dwords((BYTE *)c + 32, (SIZE_T)(size - 32), 0xFEEEFEEEu);      /* (free checking: a free chunk is filled, and the fill is checked) */
}

static void heaps_add(Heap *h);
#define MAX_HEAPS 128
static Heap *g_heaps[MAX_HEAPS];
/* A heap handle is good if it is one of the heaps made (a destroyed heap's memory is gone: its header is not looked at). */
static int heap_ok(Heap *h) {
  if (!h) return 0;
  for (int i = 0; i < MAX_HEAPS; ++i) if (g_heaps[i] == h) return h->magic == HEAP_MAGIC;
  return 0;
}

static void inner_lock(Heap *h) { while (__sync_lock_test_and_set(&h->inner, 1)) NtYieldExecution(); }
static void inner_unlock(Heap *h) { __sync_lock_release(&h->inner); }
/* The lock of HeapLock: recursive, held by a thread across calls. */
static void user_lock(Heap *h) {
  const ULONG_PTR me = TEB_CLIENT_THREAD(NtCurrentTeb());
  if (h->user_lock && h->user_owner == me) { ++h->user_depth; return; }
  while (__sync_lock_test_and_set(&h->user_lock, 1)) NtYieldExecution();
  h->user_owner = me;
  h->user_depth = 1;
}
static void user_unlock(Heap *h) {
  if (!h->user_lock || h->user_owner != TEB_CLIENT_THREAD(NtCurrentTeb())) return;
  if (--h->user_depth > 0) return;
  h->user_owner = 0;
  __sync_lock_release(&h->user_lock);
}

/* Starts the chunks of a region whose header ends at `start`: one free chunk up to a sentinel (a zero-size chunk in use) at the end of the committed part. */
static void region_init(Heap *h, Subheap *s) {
  BYTE *start = s->base + s->header, *end = s->base + s->committed - 16;
  Chunk *sentinel = (Chunk *)end;
  sentinel->size_flags = CHUNK_USED;
  make_free(h, (Chunk *)start, (ULONGLONG)(end - start), 1);
}
static Chunk *region_first(Subheap *s) { return (Chunk *)(s->base + s->header); }

static int commit_pages(BYTE *at, SIZE_T bytes) {
  PVOID base = at;
  SIZE_T size = bytes;
  return NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size, MEM_COMMIT, PAGE_READWRITE));
}

static Heap *heap_new(ULONG flags, SIZE_T initial, SIZE_T maximum, int process_heap) {
  const SIZE_T header = (SIZE_T)round_up(sizeof(Heap), 32);
  SIZE_T commit = (SIZE_T)round_up(initial > INITIAL_COMMIT ? initial : INITIAL_COMMIT, INITIAL_COMMIT);
  SIZE_T reserve = maximum ? (SIZE_T)round_up(maximum < INITIAL_COMMIT ? INITIAL_COMMIT : maximum, INITIAL_COMMIT) : (SIZE_T)round_up((ULONGLONG)commit + 1, REGION_ALIGN);
  PVOID base = NULL;
  SIZE_T size = reserve;
  if (!NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size, MEM_RESERVE, PAGE_READWRITE))) return NULL;
  const SIZE_T limit = maximum ? reserve : reserve - PAGE;      /* (a growable heap keeps the last page of a region unused) */
  if (commit > limit) commit = limit;
  if (!commit_pages(base, commit)) { SIZE_T zero = 0; NtFreeVirtualMemory(NtCurrentProcess(), &base, &zero, MEM_RELEASE); return NULL; }
  Heap *h = (Heap *)base;
  h->ffeeffee = 0xFFEEFFEEu;
  const ULONG kept = flags & (HEAP_FLAG_NO_SERIALIZE | HEAP_FLAG_GENERATE_EXCEPTIONS);      /* (the others - tail and free checking, shared - come from the global flags only) */
  const ULONG global = *(ULONG *)((BYTE *)NtCurrentPeb() + 0xBC);
  ULONG derived = 0;
  if (global & 0x10) derived |= HF_TAIL;
  if (global & 0x20) derived |= HF_FREE;
  if (global & 0x40) derived |= 0x40000000u | HF_TAIL | HF_FREE;
  if (global & 0x80) derived |= 0x20000000u | HF_TAIL | HF_FREE;
  if (global & 0x200000) derived |= HF_NO_COALESCE;
  if (global & 0x2000000) derived |= HF_PAGE_ALLOCS;
  if (global & 0x800) derived |= HF_SHARED;
  h->flags = derived | kept | (maximum ? 0 : 2 /* HEAP_GROWABLE */) | (process_heap ? 0 : 0x1000 /* HEAP_PRIVATE */);
  h->force_flags = h->flags & ~(HF_SHARED | HF_NO_COALESCE | ((h->flags & HF_PAGE_ALLOCS) ? 0 : 0x1002u));
  h->auto_flags = maximum ? 0 : 2;
  h->magic = HEAP_MAGIC;
  h->maximum = maximum;
  h->next_region = 0x100000;
  h->free_list.next = h->free_list.prev = &h->free_list;
  h->first.base = (BYTE *)base;
  h->first.reserve = reserve;
  h->first.committed = commit;
  h->first.limit = limit;
  h->first.header = (ULONG)header;
  h->subheaps = &h->first;
  region_init(h, &h->first);
  if (h->flags & HF_PAGE_ALLOCS) for (BYTE *b = (BYTE *)h; b < (BYTE *)h + 0x70; ++b) *b = 0xEE;      /* (such a heap has a header of that fill) */
  heaps_add(h);
  return h;
}

/* The committed part of a region grows by `add` bytes, which become free space at its end. */
static int region_extend(Heap *h, Subheap *s, SIZE_T add) {
  add = (SIZE_T)round_up(add, PAGE);
  if (s->committed + add > s->limit) return 0;
  if (!commit_pages(s->base + s->committed, add)) return 0;
  Chunk *old_sentinel = (Chunk *)(s->base + s->committed - 16);
  const int prev_used = (old_sentinel->size_flags & CHUNK_PREV_USED) != 0;
  s->committed += add;
  Chunk *sentinel = (Chunk *)(s->base + s->committed - 16);
  sentinel->size_flags = CHUNK_USED;
  if (!prev_used) {      /* the chunk before the old sentinel is free: it grows */
    Chunk *last = (Chunk *)((BYTE *)old_sentinel - old_sentinel->prev_size);
    const ULONGLONG size = CHUNK_SIZE(last) + add;
    const int last_prev_used = (last->size_flags & CHUNK_PREV_USED) != 0;
    free_unlink(last);
    make_free(h, last, size, last_prev_used);
  } else {
    make_free(h, old_sentinel, add, 1);
  }
  return 1;
}

static Subheap *region_add(Heap *h, SIZE_T need) {
  const SIZE_T header = PAGE;      /* (the header takes a page of its own) */
  SIZE_T reserve = (SIZE_T)round_up(need + header + 2 * PAGE, REGION_ALIGN);
  if (reserve < h->next_region) reserve = h->next_region;
  h->next_region = h->next_region * 2 > 0xFD0000 ? 0xFD0000 : h->next_region * 2;      /* (the regions that follow are twice as big, up to a limit) */
  PVOID base = NULL;
  SIZE_T size = reserve;
  if (!NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size, MEM_RESERVE, PAGE_READWRITE))) return NULL;
  SIZE_T commit = (SIZE_T)round_up(need + 16, PAGE) + header;
  if (commit > reserve - PAGE) commit = reserve - PAGE;
  if (!commit_pages(base, commit)) { SIZE_T zero = 0; NtFreeVirtualMemory(NtCurrentProcess(), &base, &zero, MEM_RELEASE); return NULL; }
  Subheap *s = (Subheap *)base;
  s->next = NULL;
  s->base = (BYTE *)base;
  s->reserve = reserve;
  s->committed = commit;
  s->limit = reserve - PAGE;
  s->header = (ULONG)header;
  Subheap *tail = h->subheaps;
  while (tail->next) tail = tail->next;
  s->index = tail->index + 1;
  tail->next = s;
  region_init(h, s);
  return s;
}

#define MAX_HEAP_BLOCK (1ull << 46)
static ULONGLONG chunk_tail(Heap *h) { return (h->flags & HF_TAIL) ? TAIL_BYTES : 0; }
static ULONGLONG chunk_pad(Heap *h) { return (h->flags & 0x70000000u) ? 16 : 0; }      /* (the validating heaps keep 16 bytes after the tail) */
static ULONGLONG chunk_base(Chunk *c) { return (c->size_flags & CHUNK_BIG) ? 16 : 8; }
/* The size of the chunk for a block: what the chunk owns of the header (8 bytes, or 16), the payload, a tail, padding and user info, up to 16 (the next chunk's prev_size field is payload). */
static ULONGLONG chunk_need(Heap *h, SIZE_T bytes, int info, int big) { ULONGLONG need = ((ULONGLONG)bytes + chunk_tail(h) + chunk_pad(h) + (info ? 16 : 0) + (big ? 16 : 8) + 15) & ~15ull; return need < MIN_CHUNK ? MIN_CHUNK : need; }
static ULONGLONG chunk_asked(Heap *h, Chunk *c) {
  const ULONGLONG unused = (c->size_flags >> CHUNK_UNUSED_SHIFT) & 0xFF;
  return CHUNK_SIZE(c) - chunk_base(c) - unused - ((c->size_flags & CHUNK_INFO) ? 16 : 0) - chunk_tail(h) - chunk_pad(h);
}
static void chunk_set_asked(Heap *h, Chunk *c, SIZE_T bytes) {
  const ULONGLONG used = bytes + chunk_tail(h) + chunk_pad(h) + ((c->size_flags & CHUNK_INFO) ? 16 : 0);
  c->size_flags = (c->size_flags & ~(0xFFull << CHUNK_UNUSED_SHIFT)) | ((CHUNK_SIZE(c) - chunk_base(c) - used) << CHUNK_UNUSED_SHIFT);
}
static BYTE *chunk_info(Heap *h, Chunk *c) { return (BYTE *)c + 16 + chunk_asked(h, c) + chunk_tail(h) + chunk_pad(h); }
static int heap_is_fixed(Heap *h) { return h->maximum != 0; }

static PVOID large_alloc(Heap *h, ULONG flags, SIZE_T bytes) {
  const SIZE_T total = (SIZE_T)round_up((ULONGLONG)bytes + LARGE_HEADER, PAGE);
  PVOID base = NULL;
  SIZE_T size = total;
  if (!NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))) return NULL;
  Large *l = (Large *)base;
  l->reserved = l->committed = total;
  l->asked = bytes;
  l->user_value = 0;
  l->user_flags = flags & 0xE00;
  l->magic = LARGE_MAGIC;
  l->next = NULL;
  Large **tail = &h->large;
  while (*tail) tail = &(*tail)->next;
  *tail = l;      /* (the list is in the order the blocks were made: HeapWalk reports them so) */
  return (BYTE *)base + LARGE_HEADER;      /* (fresh pages are zero) */
}

static PVOID heap_alloc_locked(Heap *h, ULONG flags, SIZE_T bytes) {
  if (bytes > MAX_HEAP_BLOCK) return NULL;
  if (heap_is_fixed(h) && bytes >= 0xFF000) return NULL;      /* (a fixed heap holds no very big blocks) */
  if (bytes >= LARGE_MIN && !heap_is_fixed(h)) return large_alloc(h, flags, bytes);
  const int info = (flags & 0x100) != 0;
  const int big = *(ULONG *)((BYTE *)NtCurrentPeb() + 0xBC) != 0 || (flags & (HF_TAIL | HF_FREE));
  const ULONGLONG need = chunk_need(h, bytes, info, big);
  for (int attempt = 0; attempt < 3; ++attempt) {
    Chunk *chosen = NULL;
    for (Chunk *c = h->free_list.next; c != &h->free_list; c = c->next) {
      const ULONGLONG size = CHUNK_SIZE(c);
      if (size < need) continue;
      if (!chosen || (need >= 0x4000 && size > CHUNK_SIZE(chosen))) chosen = c;      /* (a big block is cut from the biggest free chunk: it can grow in place after that) */
      if (need < 0x4000) break;
    }
    if (chosen) {
      Chunk *c = chosen;
      const ULONGLONG size = CHUNK_SIZE(c);
      free_unlink(c);
      const ULONGLONG extra_bits = (info ? CHUNK_INFO : 0) | (big ? CHUNK_BIG : 0) | ((ULONGLONG)(flags & 0xE00) << CHUNK_UFLAGS_SHIFT);
      if (size - need >= MIN_CHUNK) {      /* split: the rest stays free */
        c->size_flags = need | CHUNK_USED | (c->size_flags & CHUNK_PREV_USED) | extra_bits;
        make_free(h, (Chunk *)((BYTE *)c + need), size - need, 1);
      } else {
        c->size_flags = size | CHUNK_USED | (c->size_flags & CHUNK_PREV_USED) | extra_bits;
        chunk_next(c)->size_flags |= CHUNK_PREV_USED;
      }
      chunk_set_asked(h, c, bytes);
      BYTE *user = (BYTE *)c + 16;
      if (flags & HEAP_FLAG_ZERO_MEMORY) memset(user, 0, (size_t_)((h->flags & (HF_TAIL | 0x70000000u)) || info ? bytes : CHUNK_SIZE(c) - chunk_base(c)));      /* (all of the payload: the bytes after the size are zero too, unless a tail follows) */
      else if (h->flags & HF_FREE) fill_dwords(user, bytes, 0xBAADF00Du);
      if (h->flags & HF_TAIL) memset(user + bytes, 0xAB, (size_t_)TAIL_BYTES);
      if (info) memset(chunk_info(h, c), 0, 16);
      return user;
    }
    if (attempt == 0) {      /* commit more of the newest region */
      int grown = 0;
      for (Subheap *s = h->subheaps; s && !grown; s = s->next) {
        grown = region_extend(h, s, (SIZE_T)need);
      }
      if (grown) continue;
    }
    if (attempt == 1) {
      if (heap_is_fixed(h) || !region_add(h, (SIZE_T)need)) return NULL;
    }
  }
  return NULL;
}

/* The address of a chunk of the heap that `memory` names, or NULL if it is not a block of a region (nothing is followed before the address is known to be in one). */
static Chunk *heap_chunk_of(Heap *h, PVOID memory) {
  const ULONG_PTR p = (ULONG_PTR)memory;
  if (p & 15) return NULL;
  for (Subheap *s = h->subheaps; s; s = s->next) {
    const ULONG_PTR first = (ULONG_PTR)s->base + s->header + 16, last = (ULONG_PTR)s->base + s->committed - 16;
    if (p < first || p >= last) continue;
    Chunk *c = (Chunk *)((BYTE *)memory - 16);
    if (!(c->size_flags & CHUNK_USED) || CHUNK_SIZE(c) < MIN_CHUNK || (ULONG_PTR)c + CHUNK_SIZE(c) >= last + 16) return NULL;
    return c;
  }
  return NULL;
}
static Large *heap_large_of(Heap *h, PVOID memory) {
  for (Large *l = h->large; l; l = l->next) if ((BYTE *)l + LARGE_HEADER == (BYTE *)memory) return l;
  return NULL;
}

static BOOLEAN heap_free_locked(Heap *h, PVOID memory) {
  if (Large *l = heap_large_of(h, memory)) {
    Large **link = &h->large;
    while (*link != l) link = &(*link)->next;
    *link = l->next;
    PVOID base = l;
    SIZE_T zero = 0;
    NtFreeVirtualMemory(NtCurrentProcess(), &base, &zero, MEM_RELEASE);
    return TRUE;
  }
  Chunk *c = heap_chunk_of(h, memory);
  if (!c) return FALSE;
  ULONGLONG size = CHUNK_SIZE(c);
  int prev_used = (c->size_flags & CHUNK_PREV_USED) != 0;
  Chunk *after = chunk_next(c);
  if (h->flags & HF_NO_COALESCE) { make_free(h, c, size, prev_used); return TRUE; }
  if (!(after->size_flags & CHUNK_USED)) {      /* merge with the free chunk after */
    size += CHUNK_SIZE(after);
    free_unlink(after);
  }
  if (!prev_used) {      /* merge with the free chunk before */
    Chunk *before = (Chunk *)((BYTE *)c - c->prev_size);
    size += CHUNK_SIZE(before);
    prev_used = (before->size_flags & CHUNK_PREV_USED) != 0;
    free_unlink(before);
    c = before;
  }
  make_free(h, c, size, prev_used);
  return TRUE;
}

/* How many bytes were asked for, or (SIZE_T)-1 if `memory` is not a block in use. */
static SIZE_T heap_size_locked(Heap *h, PVOID memory) {
  if (Large *l = heap_large_of(h, memory)) return l->asked;
  Chunk *c = heap_chunk_of(h, memory);
  return c ? (SIZE_T)chunk_asked(h, c) : (SIZE_T)-1;
}
/* The user info of a block: a value and flags, there if the block was made with HEAP_ADD_USER_INFO (blocks of their own pages always have them). */
static int block_has_info(Heap *h, PVOID memory) { if (heap_large_of(h, memory)) return 1; return ((((Chunk *)((BYTE *)memory - 16))->size_flags) & CHUNK_INFO) != 0; }
static ULONGLONG *block_value_slot(Heap *h, PVOID memory) {
  if (Large *l = heap_large_of(h, memory)) return &l->user_value;
  return (ULONGLONG *)(chunk_info(h, (Chunk *)((BYTE *)memory - 16)) + 8);
}
static ULONG block_uflags(Heap *h, PVOID memory) {
  if (Large *l = heap_large_of(h, memory)) return (ULONG)l->user_flags & 0xE00;
  return (ULONG)((((Chunk *)((BYTE *)memory - 16))->size_flags >> CHUNK_UFLAGS_SHIFT) & 0xE00);
}
static void block_set_uflags(Heap *h, PVOID memory, ULONG value) {
  if (Large *l = heap_large_of(h, memory)) { l->user_flags = value & 0xE00; return; }
  Chunk *c = (Chunk *)((BYTE *)memory - 16);
  c->size_flags = (c->size_flags & ~(0xFFFull << CHUNK_UFLAGS_SHIFT)) | ((ULONGLONG)(value & 0xE00) << CHUNK_UFLAGS_SHIFT);
}

static PVOID heap_realloc_locked(Heap *h, ULONG flags, PVOID memory, SIZE_T bytes) {
  const SIZE_T have = heap_size_locked(h, memory);
  if (have == (SIZE_T)-1) return NULL;
  if (bytes > MAX_HEAP_BLOCK) return NULL;
  if (Large *l = heap_large_of(h, memory)) {      /* a block of its own pages: it changes size inside what it has reserved */
    const SIZE_T total = (SIZE_T)round_up((ULONGLONG)bytes + LARGE_HEADER, PAGE);
    if (total <= l->reserved && (bytes >= LARGE_MIN || (flags & HEAP_FLAG_REALLOC_IN_PLACE))) {
      if (total > l->committed) { if (!commit_pages((BYTE *)l + l->committed, total - l->committed)) return NULL; l->committed = total; }
      else if (total < l->committed) {
        PVOID tail = (BYTE *)l + total;
        SIZE_T cut = l->committed - total;
        NtFreeVirtualMemory(NtCurrentProcess(), &tail, &cut, MEM_DECOMMIT);
        l->committed = total;
        if (l->reserved - total >= 0x80000) {      /* a big shrink gives the range back: the block cannot grow in place again */
          PVOID rest = (BYTE *)l + total;
          SIZE_T zero = 0;
          NtFreeVirtualMemory(NtCurrentProcess(), &rest, &zero, MEM_RELEASE);
          l->reserved = total;
        }
      }
      if ((flags & HEAP_FLAG_ZERO_MEMORY) && bytes > have) memset((BYTE *)memory + have, 0, bytes - have);
      l->asked = bytes;
      return memory;
    }
    if (flags & HEAP_FLAG_REALLOC_IN_PLACE) return NULL;
  } else {
    Chunk *c = (Chunk *)((BYTE *)memory - 16);
    const int info = (c->size_flags & CHUNK_INFO) != 0;
    const SIZE_T tail = (SIZE_T)chunk_tail(h);
    const ULONGLONG need = chunk_need(h, bytes, info, (c->size_flags & CHUNK_BIG) != 0);
    ULONGLONG size = CHUNK_SIZE(c);
    const int small_enough = bytes < LARGE_MIN || heap_is_fixed(h);
    BYTE saved_info[16];
    if (info) for (int i = 0; i < 16; ++i) saved_info[i] = chunk_info(h, c)[i];
    int resized = 0;
    if (need <= size && small_enough) {      /* fits in the chunk: stays where it is (and gives back what is far too much) */
      if (size - need >= 64) {
        Chunk *rest = (Chunk *)((BYTE *)c + need);
        ULONGLONG rest_size = size - need;
        Chunk *after = chunk_next(c);
        if (!(after->size_flags & CHUNK_USED)) { rest_size += CHUNK_SIZE(after); free_unlink(after); }
        c->size_flags = (c->size_flags & ~CHUNK_SIZE_MASK) | need;
        make_free(h, rest, rest_size, 1);
      }
      resized = 1;
    } else if (flags & HEAP_FLAG_REALLOC_IN_PLACE) {
      /* A big block may grow into the free chunk after it (a small one is never grown in place: it would be a low-fragmentation block on Windows). */
      Chunk *after = chunk_next(c);
      if (size < 0x4000 || !small_enough) return NULL;
      if (size + ((after->size_flags & CHUNK_USED) ? 0 : CHUNK_SIZE(after)) < need) {      /* at the end of a region, which can commit more */
        Chunk *end = (after->size_flags & CHUNK_USED) ? after : chunk_next(after);
        if (CHUNK_SIZE(end) != 0) return NULL;
        for (Subheap *s = h->subheaps; s; s = s->next) {
          if ((BYTE *)c < s->base || (BYTE *)c >= s->base + s->committed) continue;
          const ULONGLONG got = size + ((after->size_flags & CHUNK_USED) ? 0 : CHUNK_SIZE(after));
          if (!region_extend(h, s, (SIZE_T)(need - got + 16))) return NULL;
          break;
        }
        after = chunk_next(c);
      }
      if ((after->size_flags & CHUNK_USED) || size + CHUNK_SIZE(after) < need) return NULL;
      const ULONGLONG total = size + CHUNK_SIZE(after);
      free_unlink(after);
      if (total - need >= MIN_CHUNK) {
        c->size_flags = (c->size_flags & ~CHUNK_SIZE_MASK) | need;
        make_free(h, (Chunk *)((BYTE *)c + need), total - need, 1);
      } else {
        c->size_flags = (c->size_flags & ~CHUNK_SIZE_MASK) | total;
        chunk_next(c)->size_flags |= CHUNK_PREV_USED;
      }
      resized = 1;
    }
    if (resized) {
      chunk_set_asked(h, c, bytes);
      if ((flags & HEAP_FLAG_ZERO_MEMORY) && bytes > have) memset((BYTE *)memory + have, 0, bytes - have);
      else if ((h->flags & HF_FREE) && bytes > have) fill_dwords((BYTE *)memory + ((have + 3) & ~(SIZE_T)3), bytes - ((have + 3) & ~(SIZE_T)3), 0xBAADF00Du);
      if (tail) memset((BYTE *)memory + bytes, 0xAB, tail);
      if (info) for (int i = 0; i < 16; ++i) chunk_info(h, c)[i] = saved_info[i];
      return memory;
    }
  }
  PVOID fresh = heap_alloc_locked(h, (flags & ~(ULONG)HEAP_FLAG_ZERO_MEMORY) | (block_has_info(h, memory) ? 0x100u : 0u), bytes);
  if (!fresh) return NULL;
  memcpy(fresh, memory, have < bytes ? have : bytes);
  if ((flags & HEAP_FLAG_ZERO_MEMORY) && bytes > have) memset((BYTE *)fresh + have, 0, bytes - have);
  if (block_has_info(h, fresh) && block_has_info(h, memory)) { *block_value_slot(h, fresh) = *block_value_slot(h, memory); block_set_uflags(h, fresh, block_uflags(h, memory)); }
  heap_free_locked(h, memory);
  return fresh;
}

/* Calls of a heap: serialized (HeapLock's lock) unless the heap or the call says not to be, and a heap of the low-fragmentation kind does not serialize small allocations. */
static int serialized(Heap *h, ULONG flags) { return !((h->flags | flags) & HEAP_FLAG_NO_SERIALIZE); }
static void *heap_lock_for(Heap *h, ULONG flags, int small) {
  if (serialized(h, flags) && !(small && h->compat == 2)) { user_lock(h); inner_lock(h); return h; }
  inner_lock(h);
  return NULL;
}
static void heap_unlock_for(Heap *h, void *held) { inner_unlock(h); if (held) user_unlock(h); }

static void heap_error(ULONG error) { TEB_LAST_ERROR(NtCurrentTeb()) = error; }
EXPORT void NTAPI RtlRaiseStatus(NTSTATUS status);

EXPORT ULONG NTAPI RtlGetNtGlobalFlags(void) { return *(ULONG *)((BYTE *)NtCurrentPeb() + 0xBC); }

EXPORT PVOID NTAPI RtlCreateHeap(ULONG flags, PVOID base, SIZE_T reserve, SIZE_T commit, PVOID lock, PVOID parameters) {
  (void)base; (void)lock; (void)parameters;
  if (reserve && commit > reserve) { return NULL; }
  return heap_new(flags, commit, reserve, 0);
}

EXPORT PVOID NTAPI RtlAllocateHeap(PVOID heap, ULONG flags, SIZE_T bytes) {
  Heap *h = heap;
  if (!heap_ok(h)) { heap_error(6); return NULL; }
  void *held = heap_lock_for(h, flags, bytes < 0x4000);
  PVOID r = heap_alloc_locked(h, flags, bytes);
  if (r && ++h->allocations >= 0x12 && h->compat == 0 && !(h->flags & HEAP_FLAG_NO_SERIALIZE) && !heap_is_fixed(h)) h->compat = 2;
  heap_unlock_for(h, held);
  if (!r) {
    heap_error(8 /* ERROR_NOT_ENOUGH_MEMORY */);
    if ((flags | h->flags) & HEAP_FLAG_GENERATE_EXCEPTIONS) RtlRaiseStatus((NTSTATUS)0xC0000017u);
  }
  return r;
}
EXPORT BOOLEAN NTAPI RtlFreeHeap(PVOID heap, ULONG flags, PVOID memory) {
  Heap *h = heap;
  if (!memory) return TRUE;
  if (!heap_ok(h)) { heap_error(6); return FALSE; }
  void *held = heap_lock_for(h, flags, 1);
  const BOOLEAN r = heap_free_locked(h, memory);
  heap_unlock_for(h, held);
  if (!r) heap_error(87 /* ERROR_INVALID_PARAMETER */);
  return r;
}
EXPORT SIZE_T NTAPI RtlSizeHeap(PVOID heap, ULONG flags, PVOID memory) {
  Heap *h = heap;
  if (!heap_ok(h) || !memory) return (SIZE_T)-1;
  void *held = heap_lock_for(h, flags, 1);
  const SIZE_T r = heap_size_locked(h, memory);
  heap_unlock_for(h, held);
  return r;
}
EXPORT PVOID NTAPI RtlReAllocateHeap(PVOID heap, ULONG flags, PVOID memory, SIZE_T bytes) {
  Heap *h = heap;
  if (!heap_ok(h)) { heap_error(6); return NULL; }
  if (!memory) { heap_error(0); return NULL; }
  void *held = heap_lock_for(h, flags, 0);
  PVOID r = heap_realloc_locked(h, flags, memory, bytes);
  heap_unlock_for(h, held);
  if (!r) {
    heap_error(8);
    if ((flags | h->flags) & HEAP_FLAG_GENERATE_EXCEPTIONS) RtlRaiseStatus((NTSTATUS)0xC0000017u);
  }
  return r;
}
/* Is the tail of the block (the asked size is `bytes`) still what was put there? */
static int tail_intact(Heap *h, PVOID memory, SIZE_T bytes) {
  if (!(h->flags & HF_TAIL) || heap_large_of(h, memory)) return 1;
  for (SIZE_T i = 0; i < TAIL_BYTES; ++i) if (((BYTE *)memory)[bytes + i] != 0xAB) return 0;
  return 1;
}
/* Every chunk of every region: the sizes agree, free chunks keep their fill, blocks keep their tails. */
static int heap_validate_all(Heap *h) {
  for (Subheap *s = h->subheaps; s; s = s->next) {
    Chunk *c = region_first(s);
    BYTE *end = s->base + s->committed - 16;
    while ((BYTE *)c < end) {
      const ULONGLONG size = CHUNK_SIZE(c);
      if (size < MIN_CHUNK || (BYTE *)c + size > end) return 0;
      if (c->size_flags & CHUNK_USED) {
        const ULONGLONG asked = chunk_asked(h, c);
        if ((h->flags & HF_TAIL) && asked + TAIL_BYTES + chunk_base(c) <= size && !tail_intact(h, (BYTE *)c + 16, (SIZE_T)asked)) return 0;
      } else if ((h->flags & HF_FREE) && !check_dwords((BYTE *)c + 32, (SIZE_T)(size - 32), 0xFEEEFEEEu)) {
        return 0;
      }
      c = chunk_next(c);
    }
  }
  return 1;
}
EXPORT BOOLEAN NTAPI RtlValidateHeap(PVOID heap, ULONG flags, PVOID memory) {
  Heap *h = heap;
  if (!heap_ok(h)) return FALSE;
  void *held = heap_lock_for(h, flags, 1);
  BOOLEAN ok;
  if (!memory) ok = heap_validate_all(h) ? TRUE : FALSE;
  else { const SIZE_T r = heap_size_locked(h, memory); ok = r != (SIZE_T)-1 && tail_intact(h, memory, r); }
  heap_unlock_for(h, held);
  return ok;
}

/* The value and the flags (HEAP_SETTABLE_USER_FLAG1..3) a program can keep with a block (a block made with HEAP_ADD_USER_INFO has room for them). */
EXPORT BOOLEAN NTAPI RtlGetUserInfoHeap(PVOID heap, ULONG flags, PVOID memory, PVOID *value, PULONG user_flags) {
  Heap *h = heap;
  if (!heap_ok(h) || !memory) return FALSE;
  void *held = heap_lock_for(h, flags, 1);
  const int ok = heap_size_locked(h, memory) != (SIZE_T)-1;
  if (ok) {
    const int info = block_has_info(h, memory);
    if (value && info) *value = (PVOID)(ULONG_PTR)*block_value_slot(h, memory);
    if (user_flags) *user_flags = info ? block_uflags(h, memory) : 0;
  }
  heap_unlock_for(h, held);
  return ok ? TRUE : FALSE;
}
EXPORT BOOLEAN NTAPI RtlSetUserValueHeap(PVOID heap, ULONG flags, PVOID memory, PVOID value) {
  Heap *h = heap;
  if (!heap_ok(h) || !memory) return FALSE;
  void *held = heap_lock_for(h, flags, 1);
  const int ok = heap_size_locked(h, memory) != (SIZE_T)-1 && block_has_info(h, memory);
  if (ok) *block_value_slot(h, memory) = (ULONGLONG)(ULONG_PTR)value;
  heap_unlock_for(h, held);
  return ok ? TRUE : FALSE;
}
EXPORT BOOLEAN NTAPI RtlSetUserFlagsHeap(PVOID heap, ULONG flags, PVOID memory, ULONG clear, ULONG set) {
  Heap *h = heap;
  if (!heap_ok(h) || !memory) return FALSE;
  if ((clear | set) & ~(ULONG)0xE00) { heap_error(87 /* ERROR_INVALID_PARAMETER */); return FALSE; }
  void *held = heap_lock_for(h, flags, 1);
  const int ok = heap_size_locked(h, memory) != (SIZE_T)-1 && block_has_info(h, memory);
  if (ok) block_set_uflags(h, memory, (block_uflags(h, memory) & ~clear) | set);
  heap_unlock_for(h, held);
  return ok ? TRUE : FALSE;
}

/* Every heap of the process, in the order they were made (the process heap first): RtlGetProcessHeaps and the Toolhelp heap lists. */
static volatile LONG g_heaps_lock;
static void heaps_lock(void) { while (__sync_lock_test_and_set(&g_heaps_lock, 1)) NtYieldExecution(); }
static void heaps_unlock(void) { __sync_lock_release(&g_heaps_lock); }
static void heaps_add(Heap *h) { heaps_lock(); for (int i = 0; i < MAX_HEAPS; ++i) if (!g_heaps[i]) { g_heaps[i] = h; break; } heaps_unlock(); }
static void heaps_remove(Heap *h) { heaps_lock(); for (int i = 0; i < MAX_HEAPS; ++i) if (g_heaps[i] == h) g_heaps[i] = NULL; heaps_unlock(); }

NTSTATUS DbgPrint(const char *fmt, ...);
static void soft_break(void);
EXPORT PVOID NTAPI RtlDestroyHeap(PVOID heap) {
  Heap *h = heap;
  if (!heap_ok(h)) {
    /* a pointer that is not a heap: with the validation flag in the header it is reported to a debugger, which is told with a message and a breakpoint */
    if (NtCurrentPeb()->BeingDebugged && heap && (*(ULONG *)((BYTE *)heap + 0x70) & 0x40000000u)) {
      DbgPrint("Invalid heap %p\n", heap);
      soft_break();
    }
    return heap;
  }
  if (h == NtCurrentPeb()->ProcessHeap) return heap;
  inner_lock(h);
  h->magic = 0;
  heaps_remove(h);
  for (Large *l = h->large; l;) {
    Large *next = l->next;
    PVOID base = l;
    SIZE_T zero = 0;
    NtFreeVirtualMemory(NtCurrentProcess(), &base, &zero, MEM_RELEASE);
    l = next;
  }
  Subheap *s = h->subheaps;
  while (s) {
    Subheap *next = s->next;
    PVOID base = s->base;
    SIZE_T zero = 0;
    if (base != (PVOID)h) NtFreeVirtualMemory(NtCurrentProcess(), &base, &zero, MEM_RELEASE);
    s = next;
  }
  PVOID first = h;
  SIZE_T zero = 0;
  NtFreeVirtualMemory(NtCurrentProcess(), &first, &zero, MEM_RELEASE);
  return NULL;
}

EXPORT BOOLEAN NTAPI RtlLockHeap(PVOID heap) {
  Heap *h = heap;
  if (!heap_ok(h)) return FALSE;
  if (!(h->flags & HEAP_FLAG_NO_SERIALIZE)) user_lock(h);
  return TRUE;
}
EXPORT BOOLEAN NTAPI RtlUnlockHeap(PVOID heap) {
  Heap *h = heap;
  if (!heap_ok(h)) return FALSE;
  if (!(h->flags & HEAP_FLAG_NO_SERIALIZE)) user_unlock(h);
  return TRUE;
}
/* The size of the largest free chunk (what HeapCompact reports). */
EXPORT ULONG NTAPI RtlCompactHeap(PVOID heap, ULONG flags) {
  Heap *h = heap;
  if (!heap_ok(h)) return 0;
  void *held = heap_lock_for(h, flags, 0);
  ULONGLONG best = 0;
  for (Chunk *c = h->free_list.next; c != &h->free_list; c = c->next) if (CHUNK_SIZE(c) > best) best = CHUNK_SIZE(c);
  heap_unlock_for(h, held);
  return best > 32 ? (ULONG)(best - 32) : 0;
}
EXPORT NTSTATUS NTAPI RtlSetHeapInformation(PVOID heap, ULONG info_class, PVOID info, SIZE_T length) {
  Heap *h = heap;
  if (info_class == 1 /* HeapEnableTerminationOnCorruption */ ) { if (heap && !heap_ok(h)) return (NTSTATUS)0xC0000005u; return STATUS_SUCCESS; }
  if (!heap_ok(h)) return (NTSTATUS)0xC0000005u;
  if (info_class != 0) return STATUS_SUCCESS;      /* (other classes are accepted and ignored) */
  if (length < sizeof(ULONG) || !info) return (NTSTATUS)0xC0000023u;
  const ULONG value = *(ULONG *)info;
  if (h->flags & HEAP_FLAG_NO_SERIALIZE) return (NTSTATUS)0xC000000Du;      /* STATUS_INVALID_PARAMETER */
  if (h->compat == 2) return (NTSTATUS)0xC0000001u;      /* (low fragmentation cannot be undone) */
  if (value > 2 || value == 1) return (NTSTATUS)0xC0000001u;
  if (value == 2) h->compat = 2;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlQueryHeapInformation(PVOID heap, ULONG info_class, PVOID info, SIZE_T length, PSIZE_T returned) {
  Heap *h = heap;
  if (!heap_ok(h)) { if (returned) *returned = 0; return (NTSTATUS)0xC0000005u; }
  if (info_class != 0) return (NTSTATUS)0xC000000Du;
  if (returned) *returned = sizeof(ULONG);
  if (length < sizeof(ULONG) || !info) return (NTSTATUS)0xC0000023u;
  *(ULONG *)info = h->compat;
  return STATUS_SUCCESS;
}
EXPORT ULONG NTAPI RtlGetProcessHeaps(ULONG count, PVOID *heaps) {
  ULONG n = 0;
  heaps_lock();
  for (int i = 0; i < MAX_HEAPS; ++i) if (g_heaps[i]) { if (n < count && heaps) heaps[n] = g_heaps[i]; ++n; }
  heaps_unlock();
  return n;
}

/* RtlWalkHeap(heap, entry): the pieces of a heap one after the other, in the layout of the undocumented RTL_HEAP_WALK_ENTRY of the test (lpData NULL starts): for each region its
 * entry, its blocks (free ones too) and the part that is not committed; then the blocks of their own pages. */
typedef struct { PVOID lpData; SIZE_T cbData; BYTE cbOverhead, iRegionIndex; WORD wFlags; DWORD pad; union { struct { PVOID hMem; DWORD reserved[3]; } block; struct { DWORD committed, uncommitted; PVOID first, last; } region; } u; } HeapWalkEntry;
#define WALK_BUSY 0x4011      /* COMMITTED | BLOCK | BUSY */
#define WALK_REGION 2
#define WALK_UNCOMMITTED 0x1000
#define WALK_REGION_INDEX_LARGE 64
static Subheap *subheap_at(Heap *h, ULONG index) { for (Subheap *s = h->subheaps; s; s = s->next) if (s->index == index) return s; return NULL; }
EXPORT NTSTATUS NTAPI RtlWalkHeap(PVOID heap, PVOID entry) {
  Heap *h = heap;
  HeapWalkEntry *e = entry;
  if (!heap_ok(h) || !e) return STATUS_INVALID_HANDLE;
  NTSTATUS status = 0x8000001A;      /* STATUS_NO_MORE_ENTRIES */
  void *held = heap_lock_for(h, 0, 0);
  HeapWalkEntry out;
  memset(&out, 0, sizeof(out));
  int found = 0;
  if (!e->lpData) {      /* the first: the heap's own region */
    Subheap *s = &h->first;
    out.lpData = h; out.cbData = s->header; out.iRegionIndex = 0; out.wFlags = WALK_REGION | (h->compat == 2 ? 0x8000 : 0);
    out.u.region.committed = (DWORD)s->committed; out.u.region.uncommitted = (DWORD)(s->reserve - s->committed);
    out.u.region.first = (BYTE *)region_first(s) + 16; out.u.region.last = s->base + s->limit;
    found = 1;
  } else {
    const BYTE index = e->iRegionIndex;
    PVOID at = e->lpData;
    const WORD flags = e->wFlags;
    if (index == WALK_REGION_INDEX_LARGE) {      /* after a block of its own pages: the next of them */
      Large *l = h->large;
      while (l && (BYTE *)l + LARGE_HEADER != (BYTE *)at) l = l->next;
      for (Large *n = l ? l->next : NULL; n; n = NULL) {
        out.lpData = (BYTE *)n + LARGE_HEADER; out.cbData = n->asked; out.cbOverhead = 0; out.iRegionIndex = WALK_REGION_INDEX_LARGE; out.wFlags = WALK_BUSY;
        found = 1;
      }
    } else {
      Subheap *s = subheap_at(h, index);
      if (s) {
        Chunk *c = NULL;
        int stage = 0;      /* 0: a block follows, 1: the blocks are done, 2: the uncommitted part is done */
        if (flags & WALK_REGION) c = region_first(s);
        else if (flags & WALK_UNCOMMITTED) stage = 2;
        else c = chunk_next((Chunk *)((BYTE *)at - 16 - ((flags & 1) ? 0 : 16)));
        if (c && CHUNK_SIZE(c) == 0) { stage = 1; c = NULL; }      /* (the sentinel: what follows is the uncommitted part) */
        if (c) {
          if (c->size_flags & CHUNK_USED) {
            const ULONGLONG asked = chunk_asked(h, c);
            const int lfh = h->compat == 2 && asked < 0x4000;
            out.lpData = (BYTE *)c + 16; out.cbData = asked; out.cbOverhead = lfh ? 8 : (BYTE)(CHUNK_SIZE(c) - asked); out.wFlags = lfh ? 0x8001 : WALK_BUSY;
          } else {
            out.lpData = (BYTE *)c + 32; out.cbData = CHUNK_SIZE(c) - 80; out.cbOverhead = 32; out.wFlags = 0;
          }
          out.iRegionIndex = (BYTE)s->index;
          found = 1;
        } else if (stage == 1 && s->limit > s->committed) {
          out.lpData = s->base + s->committed; out.cbData = s->limit - s->committed; out.iRegionIndex = (BYTE)s->index; out.wFlags = WALK_UNCOMMITTED;
          found = 1;
        } else {      /* the next region, or the blocks of their own pages */
          Subheap *n = s->next;
          if (n) {
            out.lpData = n->base; out.cbData = n->header; out.iRegionIndex = (BYTE)n->index; out.wFlags = WALK_REGION | (h->compat == 2 ? 0x8000 : 0);
            out.u.region.committed = (DWORD)n->committed; out.u.region.uncommitted = (DWORD)(n->reserve - n->committed);
            out.u.region.first = (BYTE *)region_first(n) + 16; out.u.region.last = n->base + n->limit;
            found = 1;
          } else if (h->large) {
            Large *l = h->large;
            out.lpData = (BYTE *)l + LARGE_HEADER; out.cbData = l->asked; out.iRegionIndex = WALK_REGION_INDEX_LARGE; out.wFlags = WALK_BUSY;
            found = 1;
          }
        }
      }
    }
  }
  if (found) { *e = out; status = STATUS_SUCCESS; }
  heap_unlock_for(h, held);
  return status;
}
