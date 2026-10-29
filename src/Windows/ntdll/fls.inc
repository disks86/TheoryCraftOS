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

/* ntdll.dll: fiber local storage (FlsAlloc and friends), in the layout Windows keeps it in. The process has one table of callbacks (GlobalFls: eight chunks of 16 << n
 * entries, a list of the structures of every thread or fiber that holds values, the highest index handed out); index 0 is never used. A thread or fiber keeps its values in
 * TEB->FlsSlots (FlsThread: its place in that list, eight chunks of (16 << n) + 1 values - the first word of a chunk is not a value). Reading from a thread without such a
 * structure is an invalid parameter; storing makes it. When a thread (or fiber) ends the callbacks run for the values it holds, once; FlsFree runs the callback of the index for
 * the values every thread still has. RtlProcessFlsData(data, flags) is what both use: bit 0 takes the structure off the list and runs the callbacks, bit 1 frees it. */

#define FLS_MAX_INDEXES 4080
#define FLS_CHUNKS 8
#define TEB_FLS_SLOTS(teb) (*(FlsThread **)TEB_BYTE(teb, 0x17c8))
typedef struct FlsCallbackEntry { PVOID unknown; PFLS_CALLBACK_FUNCTION callback; } FlsCallbackEntry;
typedef struct FlsCallbackChunk { ULONG_PTR count; FlsCallbackEntry callbacks[1]; } FlsCallbackChunk;
typedef struct GlobalFls { FlsCallbackChunk *chunks[FLS_CHUNKS]; LIST_ENTRY list; ULONG high_index; } GlobalFls;
typedef struct FlsThread { LIST_ENTRY link; void **chunks[FLS_CHUNKS]; } FlsThread;

static GlobalFls *g_fls;
static void fls_lock(void) { while (__sync_lock_test_and_set(&g_fls_lock, 1)) NtYieldExecution(); }
static void fls_unlock(void) { __sync_lock_release(&g_fls_lock); }

/* The chunk of an index and the place in it. */
static void fls_locate(ULONG index, ULONG *chunk, ULONG *slot) {
  ULONG n = 0;
  while (index >= (16u << n)) { index -= 16u << n; ++n; }
  *chunk = n;
  *slot = index;
}

/* The chunk of callbacks (made when first needed; the lock is held). */
static FlsCallbackChunk *fls_callback_chunk(GlobalFls *g, ULONG chunk) {
  if (!g->chunks[chunk]) {
    const ULONG count = 16u << chunk;
    FlsCallbackChunk *c = RtlAllocateHeap(process_heap(), HEAP_ZERO_MEMORY, sizeof(ULONG_PTR) + sizeof(FlsCallbackEntry) * count);
    if (!c) return NULL;
    c->count = count;
    g->chunks[chunk] = c;
  }
  return g->chunks[chunk];
}

/* The process's table, made with the first use (the lock is held). */
static GlobalFls *fls_global(void) {
  if (!g_fls) {
    GlobalFls *g = RtlAllocateHeap(process_heap(), HEAP_ZERO_MEMORY, sizeof(GlobalFls));
    if (!g) return NULL;
    g->list.Flink = g->list.Blink = &g->list;
    FlsCallbackChunk *first = fls_callback_chunk(g, 0);
    if (!first) { RtlFreeHeap(process_heap(), 0, g); return NULL; }
    first->callbacks[0].callback = (PFLS_CALLBACK_FUNCTION)(ULONG_PTR)-1;      /* (index 0 is taken) */
    g_fls = g;
  }
  return g_fls;
}

/* The current thread's structure, made by the first store (it joins the list of all). */
static FlsThread *fls_thread(int create) {
  FlsThread *t = TEB_FLS_SLOTS(NtCurrentTeb());
  if (t || !create) return t;
  t = RtlAllocateHeap(process_heap(), HEAP_ZERO_MEMORY, sizeof(FlsThread));
  if (!t) return NULL;
  fls_lock();
  GlobalFls *g = fls_global();
  if (!g) { fls_unlock(); RtlFreeHeap(process_heap(), 0, t); return NULL; }
  t->link.Flink = &g->list;
  t->link.Blink = g->list.Blink;
  g->list.Blink->Flink = &t->link;
  g->list.Blink = &t->link;
  fls_unlock();
  TEB_FLS_SLOTS(NtCurrentTeb()) = t;
  return t;
}

EXPORT NTSTATUS NTAPI RtlFlsAlloc(PFLS_CALLBACK_FUNCTION callback, PULONG index) {
  fls_lock();
  GlobalFls *g = fls_global();
  if (!g) { fls_unlock(); return STATUS_NO_MEMORY; }
  for (ULONG i = 1; i < FLS_MAX_INDEXES; ++i) {
    ULONG chunk, slot;
    fls_locate(i, &chunk, &slot);
    FlsCallbackChunk *c = fls_callback_chunk(g, chunk);
    if (!c) { fls_unlock(); return STATUS_NO_MEMORY; }
    if (c->callbacks[slot].callback) continue;
    c->callbacks[slot].callback = callback ? callback : (PFLS_CALLBACK_FUNCTION)(ULONG_PTR)-1;
    if (i > g->high_index) g->high_index = i;
    fls_unlock();
    *index = i;
    return STATUS_SUCCESS;
  }
  fls_unlock();
  return STATUS_NO_MEMORY;
}

EXPORT NTSTATUS NTAPI RtlFlsGetValue(ULONG index, PVOID *value) {
  if (index == 0 || index >= FLS_MAX_INDEXES) return STATUS_INVALID_PARAMETER;
  FlsThread *t = fls_thread(0);
  if (!t) return STATUS_INVALID_PARAMETER;
  ULONG chunk, slot;
  fls_locate(index, &chunk, &slot);
  *value = t->chunks[chunk] ? t->chunks[chunk][slot + 1] : NULL;
  return STATUS_SUCCESS;
}

EXPORT NTSTATUS NTAPI RtlFlsSetValue(ULONG index, PVOID value) {
  if (index == 0 || index >= FLS_MAX_INDEXES) return STATUS_INVALID_PARAMETER;
  FlsThread *t = fls_thread(1);
  if (!t) return STATUS_NO_MEMORY;
  ULONG chunk, slot;
  fls_locate(index, &chunk, &slot);
  if (!t->chunks[chunk]) {
    t->chunks[chunk] = RtlAllocateHeap(process_heap(), HEAP_ZERO_MEMORY, ((SIZE_T)(16u << chunk) + 1) * sizeof(void *));
    if (!t->chunks[chunk]) return STATUS_NO_MEMORY;
  }
  t->chunks[chunk][slot + 1] = value;
  return STATUS_SUCCESS;
}

/* The index goes; every thread's value for it is handed to its callback first. */
EXPORT NTSTATUS NTAPI RtlFlsFree(ULONG index) {
  if (index == 0 || index >= FLS_MAX_INDEXES) return STATUS_INVALID_PARAMETER;
  fls_lock();
  GlobalFls *g = g_fls;
  ULONG chunk, slot;
  fls_locate(index, &chunk, &slot);
  PFLS_CALLBACK_FUNCTION callback = g && g->chunks[chunk] ? g->chunks[chunk]->callbacks[slot].callback : NULL;
  if (!callback) { fls_unlock(); return STATUS_INVALID_PARAMETER; }
  g->chunks[chunk]->callbacks[slot].callback = NULL;
  void *values[64];
  ULONG count = 0;
  for (;;) {      /* (the callbacks run outside the lock, a batch at a time) */
    count = 0;
    for (LIST_ENTRY *e = g->list.Flink; e != &g->list && count < 64; e = e->Flink) {
      FlsThread *t = (FlsThread *)e;
      if (t->chunks[chunk] && t->chunks[chunk][slot + 1]) { values[count++] = t->chunks[chunk][slot + 1]; t->chunks[chunk][slot + 1] = NULL; }
    }
    fls_unlock();
    if (callback != (PFLS_CALLBACK_FUNCTION)(ULONG_PTR)-1) for (ULONG i = 0; i < count; ++i) callback(values[i]);
    if (count < 64) break;
    fls_lock();
  }
  return STATUS_SUCCESS;
}

/* The callbacks run for the values of one structure (the last index first), which are gone after that. */
static void fls_run_callbacks(FlsThread *t) {
  if (!g_fls) return;
  for (ULONG index = FLS_MAX_INDEXES; index > 0; --index) {
    ULONG chunk, slot;
    fls_locate(index - 1, &chunk, &slot);
    if (!t->chunks[chunk]) continue;
    PVOID value = t->chunks[chunk][slot + 1];
    if (!value) continue;
    t->chunks[chunk][slot + 1] = NULL;
    fls_lock();
    PFLS_CALLBACK_FUNCTION callback = g_fls->chunks[chunk] ? g_fls->chunks[chunk]->callbacks[slot].callback : NULL;
    fls_unlock();
    if (callback && callback != (PFLS_CALLBACK_FUNCTION)(ULONG_PTR)-1) callback(value);
  }
}

/* Bit 0: the structure leaves the list (its own links stay as they were) and the callbacks run; bit 1: its memory goes. Other bits do nothing. */
EXPORT void NTAPI RtlProcessFlsData(PVOID data, ULONG flags) {
  FlsThread *t = data;
  if (!t) return;
  if (flags & 1) {
    fls_lock();
    if (t->link.Flink && t->link.Blink) {
      t->link.Blink->Flink = t->link.Flink;
      t->link.Flink->Blink = t->link.Blink;
    }
    fls_unlock();
    fls_run_callbacks(t);
  }
  if (flags & 2) {
    for (ULONG c = 0; c < FLS_CHUNKS; ++c) if (t->chunks[c]) { RtlFreeHeap(process_heap(), 0, t->chunks[c]); t->chunks[c] = NULL; }
    RtlFreeHeap(process_heap(), 0, t);
  }
}

/* The current thread's values: the callbacks run for each (the last index first); the structure stays until the thread is gone, so that the values can still be read (and are
 * empty) while the libraries are told about the end. */
static void fls_destroy_current(int free_structure) {
  FlsThread *t = fls_thread(0);
  if (!t) return;
  if (!free_structure) {      /* the structure leaves the list of all and the callbacks run; its links are cleared so that the end does not take it off again */
    RtlProcessFlsData(t, 1);
    t->link.Flink = t->link.Blink = NULL;
    return;
  }
  TEB_FLS_SLOTS(NtCurrentTeb()) = NULL;
  RtlProcessFlsData(t, 2);      /* (what a library stored while the thread detached is dropped: no callbacks) */
}

/* Every thread has its structure from the start (it is on the list of threads even before it stores anything). */
static void fls_thread_begins(void) { fls_thread(1); }
