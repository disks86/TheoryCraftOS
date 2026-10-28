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

/* ntdll.dll: fiber local storage (FlsAlloc and friends), per thread. The callback of each index is process-wide (NULL: the index is free; ~0: allocated without a callback); a
 * thread keeps its values in TEB->FlsSlots, in chunks of 16 << n slots, and is linked into one list of all the threads that have any. When a thread ends, or the process, the
 * callbacks run for the values it holds, once, and its values are gone; FlsFree runs the callback of the index for the values every thread still has. */

#define FLS_MAX_INDEXES 4080
#define FLS_CHUNKS 8
#define TEB_FLS_SLOTS(teb) (*(FlsThread **)TEB_BYTE(teb, 0x17c8))
typedef struct FlsThread { LIST_ENTRY link; void **chunks[FLS_CHUNKS]; } FlsThread;

static PFLS_CALLBACK_FUNCTION g_fls_callbacks[FLS_MAX_INDEXES];
static LIST_ENTRY g_fls_list;
static void fls_lock(void) { while (__sync_lock_test_and_set(&g_fls_lock, 1)) NtYieldExecution(); }
static void fls_unlock(void) { __sync_lock_release(&g_fls_lock); }
static void fls_list_init(void) { if (!g_fls_list.Flink) { g_fls_list.Flink = g_fls_list.Blink = &g_fls_list; } }

/* The chunk of an index and the place in it. */
static void fls_locate(ULONG index, ULONG *chunk, ULONG *slot) {
  ULONG n = 0;
  while (index >= (16u << n)) { index -= 16u << n; ++n; }
  *chunk = n;
  *slot = index;
}

/* The values of the current thread's structure, made when the thread first uses fiber local storage. */
static FlsThread *fls_thread(int create) {
  FlsThread *t = TEB_FLS_SLOTS(NtCurrentTeb());
  if (t || !create) return t;
  t = RtlAllocateHeap(process_heap(), HEAP_ZERO_MEMORY, sizeof(FlsThread));
  if (!t) return NULL;
  fls_lock();
  fls_list_init();
  t->link.Flink = &g_fls_list;
  t->link.Blink = g_fls_list.Blink;
  g_fls_list.Blink->Flink = &t->link;
  g_fls_list.Blink = &t->link;
  fls_unlock();
  TEB_FLS_SLOTS(NtCurrentTeb()) = t;
  return t;
}

EXPORT NTSTATUS NTAPI RtlFlsAlloc(PFLS_CALLBACK_FUNCTION callback, PULONG index) {
  fls_lock();
  for (ULONG i = 0; i < FLS_MAX_INDEXES; ++i) {
    if (g_fls_callbacks[i]) continue;
    g_fls_callbacks[i] = callback ? callback : (PFLS_CALLBACK_FUNCTION)(ULONG_PTR)-1;
    fls_unlock();
    *index = i;
    return STATUS_SUCCESS;
  }
  fls_unlock();
  return STATUS_NO_MEMORY;
}

EXPORT NTSTATUS NTAPI RtlFlsGetValue(ULONG index, PVOID *value) {
  if (index >= FLS_MAX_INDEXES || !g_fls_callbacks[index]) return STATUS_INVALID_PARAMETER;
  FlsThread *t = fls_thread(0);
  ULONG chunk, slot;
  fls_locate(index, &chunk, &slot);
  *value = t && t->chunks[chunk] ? t->chunks[chunk][slot] : NULL;
  return STATUS_SUCCESS;
}

EXPORT NTSTATUS NTAPI RtlFlsSetValue(ULONG index, PVOID value) {
  if (index >= FLS_MAX_INDEXES || !g_fls_callbacks[index]) return STATUS_INVALID_PARAMETER;
  FlsThread *t = fls_thread(1);
  if (!t) return STATUS_NO_MEMORY;
  ULONG chunk, slot;
  fls_locate(index, &chunk, &slot);
  if (!t->chunks[chunk]) {
    t->chunks[chunk] = RtlAllocateHeap(process_heap(), HEAP_ZERO_MEMORY, (SIZE_T)(16u << chunk) * sizeof(void *));
    if (!t->chunks[chunk]) return STATUS_NO_MEMORY;
  }
  t->chunks[chunk][slot] = value;
  return STATUS_SUCCESS;
}

/* The index goes; every thread's value for it is handed to its callback first. */
EXPORT NTSTATUS NTAPI RtlFlsFree(ULONG index) {
  if (index >= FLS_MAX_INDEXES) return STATUS_INVALID_PARAMETER;
  fls_lock();
  PFLS_CALLBACK_FUNCTION callback = g_fls_callbacks[index];
  if (!callback) { fls_unlock(); return STATUS_INVALID_PARAMETER; }
  g_fls_callbacks[index] = NULL;
  ULONG chunk, slot;
  fls_locate(index, &chunk, &slot);
  void *values[64];
  ULONG count = 0;
  for (;;) {      /* (the callbacks run outside the lock, a batch at a time) */
    count = 0;
    fls_list_init();
    for (LIST_ENTRY *e = g_fls_list.Flink; e != &g_fls_list && count < 64; e = e->Flink) {
      FlsThread *t = (FlsThread *)e;
      if (t->chunks[chunk] && t->chunks[chunk][slot]) { values[count++] = t->chunks[chunk][slot]; t->chunks[chunk][slot] = NULL; }
    }
    fls_unlock();
    if (callback != (PFLS_CALLBACK_FUNCTION)(ULONG_PTR)-1) for (ULONG i = 0; i < count; ++i) callback(values[i]);
    if (count < 64) break;
    fls_lock();
  }
  return STATUS_SUCCESS;
}

/* The current thread's values: the callbacks run for each (the last index first), the chunks go and the thread leaves the list. The structure stays until the thread is gone, so
 * that the values can still be read (and are empty) while the libraries are told about the end. */
static void fls_destroy_current(int free_structure) {
  FlsThread *t = fls_thread(0);
  if (!t) return;
  fls_lock();
  if (t->link.Flink) {      /* (not unlinked yet) */
    t->link.Blink->Flink = t->link.Flink;
    t->link.Flink->Blink = t->link.Blink;
    t->link.Flink = t->link.Blink = NULL;
  }
  fls_unlock();
  for (ULONG index = FLS_MAX_INDEXES; index > 0; --index) {
    ULONG chunk, slot;
    fls_locate(index - 1, &chunk, &slot);
    if (!t->chunks[chunk]) continue;
    PVOID value = t->chunks[chunk][slot];
    if (!value) continue;
    t->chunks[chunk][slot] = NULL;
    PFLS_CALLBACK_FUNCTION callback = g_fls_callbacks[index - 1];
    if (callback && callback != (PFLS_CALLBACK_FUNCTION)(ULONG_PTR)-1) callback(value);
  }
  for (ULONG c = 0; c < FLS_CHUNKS; ++c) if (t->chunks[c]) { RtlFreeHeap(process_heap(), 0, t->chunks[c]); t->chunks[c] = NULL; }
  if (free_structure) { TEB_FLS_SLOTS(NtCurrentTeb()) = NULL; RtlFreeHeap(process_heap(), 0, t); }
}

/* Every thread has its structure from the start (it is on the list of threads even before it stores anything). */
static void fls_thread_begins(void) { fls_thread(1); }
