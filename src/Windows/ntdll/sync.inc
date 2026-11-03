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

/* ntdll.dll: waiting on an address, slim reader/writer locks, condition variables, one-time initialization, barriers and the old RTL_RESOURCE. Everything blocks in the kernel
 * with NtWaitForAlertByThreadId and is woken with NtAlertThreadByThreadId; the waiters of an address are on a list hashed by it. Included by ntdll.c after reg.inc. */

NTSTATUS NTAPI NtWaitForAlertByThreadId(PVOID address, PLARGE_INTEGER timeout);
NTSTATUS NTAPI NtAlertThreadByThreadId(ULONG_PTR thread_id);
NTSTATUS NTAPI NtQuerySystemTime(PLARGE_INTEGER time);

#define SYNC_STATUS_TIMEOUT_ ((NTSTATUS)0x102)
#define SYNC_STATUS_UNSUCCESSFUL_ ((NTSTATUS)0xC0000001)

typedef struct SYNC_WAITER {
  struct SYNC_WAITER *next;
  volatile const void *address;
  ULONG_PTR thread_id;
  volatile LONG woken;
} SYNC_WAITER;
#define SYNC_BUCKETS 64
static struct { volatile LONG lock; SYNC_WAITER *head, *tail; } g_sync_buckets[SYNC_BUCKETS];

static unsigned sync_bucket_of(volatile const void *address) { return (unsigned)(((ULONG_PTR)address >> 3) * 0x9E3779B1u >> 7) % SYNC_BUCKETS; }
static void sync_lock(unsigned bucket) { while (__sync_lock_test_and_set(&g_sync_buckets[bucket].lock, 1)) { while (g_sync_buckets[bucket].lock) __builtin_ia32_pause(); } }
static void sync_unlock(unsigned bucket) { __sync_lock_release(&g_sync_buckets[bucket].lock); }

static int sync_equal(volatile const void *address, const void *compare, SIZE_T size) {
  switch (size) {
    case 1: return *(volatile const BYTE *)address == *(const BYTE *)compare;
    case 2: return *(volatile const WORD *)address == *(const WORD *)compare;
    case 4: return *(volatile const DWORD *)address == *(const DWORD *)compare;
    default: return *(volatile const ULONGLONG *)address == *(const ULONGLONG *)compare;
  }
}

/* Blocks while *address equals *compare (size 1, 2, 4 or 8) until woken, the time is up (STATUS_TIMEOUT) or the value is different at the start (STATUS_SUCCESS). */
EXPORT NTSTATUS NTAPI RtlWaitOnAddress(volatile VOID *address, PVOID compare, SIZE_T size, PLARGE_INTEGER timeout) {
  if (size != 1 && size != 2 && size != 4 && size != 8) return STATUS_INVALID_PARAMETER;
  SYNC_WAITER me;
  me.next = NULL;
  me.address = address;
  me.thread_id = TEB_CLIENT_THREAD(NtCurrentTeb());
  me.woken = 0;
  const unsigned bucket = sync_bucket_of(address);
  sync_lock(bucket);
  if (!sync_equal(address, compare, size)) { sync_unlock(bucket); return STATUS_SUCCESS; }
  if (g_sync_buckets[bucket].tail) g_sync_buckets[bucket].tail->next = &me; else g_sync_buckets[bucket].head = &me;
  g_sync_buckets[bucket].tail = &me;
  sync_unlock(bucket);
  LARGE_INTEGER limit = {0};      /* the end of the wait as a system time, so that every wake-up that is not ours waits on for the rest */
  if (timeout) {
    if (timeout->QuadPart < 0) { NtQuerySystemTime(&limit); limit.QuadPart -= timeout->QuadPart; }
    else limit = *timeout;
  }
  for (;;) {
    const NTSTATUS status = NtWaitForAlertByThreadId((PVOID)address, timeout ? &limit : NULL);
    if (me.woken) return STATUS_SUCCESS;
    if (status == SYNC_STATUS_TIMEOUT_ || status == STATUS_INVALID_HANDLE || (status != STATUS_SUCCESS && status != STATUS_ALERTED)) {
      sync_lock(bucket);
      if (!me.woken) {      /* still on the list: leave it */
        SYNC_WAITER *previous = NULL;
        for (SYNC_WAITER *w = g_sync_buckets[bucket].head; w; previous = w, w = w->next) {
          if (w != &me) continue;
          if (previous) previous->next = w->next; else g_sync_buckets[bucket].head = w->next;
          if (g_sync_buckets[bucket].tail == w) g_sync_buckets[bucket].tail = previous;
          break;
        }
        sync_unlock(bucket);
        return status == SYNC_STATUS_TIMEOUT_ ? SYNC_STATUS_TIMEOUT_ : status;
      }
      sync_unlock(bucket);
      return STATUS_SUCCESS;
    }
  }
}

/* Takes the waiters of an address off its list (all of them, or the first) and alerts their threads. */
static void sync_wake(PVOID address, int all) {
  const unsigned bucket = sync_bucket_of(address);
  ULONG_PTR ids[16];
  for (;;) {
    unsigned count = 0;
    int more = 0;
    sync_lock(bucket);
    SYNC_WAITER *previous = NULL, *w = g_sync_buckets[bucket].head;
    while (w) {
      SYNC_WAITER *next = w->next;
      if (w->address == address) {
        if (count == 16) { more = 1; break; }
        ids[count++] = w->thread_id;
        if (previous) previous->next = next; else g_sync_buckets[bucket].head = next;
        if (g_sync_buckets[bucket].tail == w) g_sync_buckets[bucket].tail = previous;
        w->woken = 1;      /* (the waiter's record may be gone as soon as the lock is let go: nothing of it is touched after this) */
        if (!all) break;
      } else previous = w;
      w = next;
    }
    sync_unlock(bucket);
    for (unsigned i = 0; i < count; ++i) NtAlertThreadByThreadId(ids[i]);
    if (!more) break;
  }
}
EXPORT void NTAPI RtlWakeAddressAll(PVOID address) { sync_wake(address, 1); }
EXPORT void NTAPI RtlWakeAddressSingle(PVOID address) { sync_wake(address, 0); }
EXPORT void NTAPI RtlWakeAddressAllNoFence(PVOID address) { sync_wake(address, 1); }
EXPORT void NTAPI RtlWakeAddressSingleNoFence(PVOID address) { sync_wake(address, 0); }

/* ---- slim reader/writer locks: bit 0 held exclusively, bit 1 somebody waits, the readers are counted from bit 4 up ---- */

#define SRW_EXCLUSIVE_ 1u
#define SRW_WAITERS_ 2u
#define SRW_READER_ 16u
#define SRW_WORD(lock) ((volatile ULONG_PTR *)&(lock)->Ptr)

EXPORT void NTAPI RtlInitializeSRWLock(PSRWLOCK lock) { lock->Ptr = NULL; }
/* Waits for the word to change from `seen` (which has the waiters bit set, so that the one who lets go wakes). */
static void srw_wait(volatile ULONG_PTR *word, ULONG_PTR seen) {
  ULONG_PTR expected = seen | SRW_WAITERS_;
  if ((seen & SRW_WAITERS_) || __sync_bool_compare_and_swap(word, seen, expected)) RtlWaitOnAddress(word, &expected, sizeof(expected), NULL);
}
static void srw_wake_if_waited(volatile ULONG_PTR *word) {
  for (;;) {
    const ULONG_PTR v = *word;
    if (!(v & SRW_WAITERS_)) return;
    if (__sync_bool_compare_and_swap(word, v, v & ~(ULONG_PTR)SRW_WAITERS_)) break;
  }
  RtlWakeAddressAll((PVOID)word);
}
EXPORT void NTAPI RtlAcquireSRWLockExclusive(PSRWLOCK lock) {
  volatile ULONG_PTR *word = SRW_WORD(lock);
  for (;;) {
    const ULONG_PTR v = *word;
    if ((v & ~(ULONG_PTR)SRW_WAITERS_) == 0) { if (__sync_bool_compare_and_swap(word, v, v | SRW_EXCLUSIVE_)) return; }
    else srw_wait(word, v);
  }
}
EXPORT BOOLEAN NTAPI RtlTryAcquireSRWLockExclusive(PSRWLOCK lock) {
  volatile ULONG_PTR *word = SRW_WORD(lock);
  for (;;) {
    const ULONG_PTR v = *word;
    if ((v & ~(ULONG_PTR)SRW_WAITERS_) != 0) return FALSE;
    if (__sync_bool_compare_and_swap(word, v, v | SRW_EXCLUSIVE_)) return TRUE;
  }
}
EXPORT void NTAPI RtlReleaseSRWLockExclusive(PSRWLOCK lock) {
  volatile ULONG_PTR *word = SRW_WORD(lock);
  if (!(*word & SRW_EXCLUSIVE_)) {      /* (a lock that is not held: the real one raises STATUS_RESOURCE_NOT_OWNED) */
    EXCEPTION_RECORD record;
    memset(&record, 0, sizeof(record));
    record.ExceptionCode = 0xC0000264u;
    record.ExceptionFlags = 1;
    RtlRaiseException(&record);
    return;
  }
  __sync_fetch_and_and(word, ~(ULONG_PTR)SRW_EXCLUSIVE_);
  srw_wake_if_waited(word);
}
EXPORT void NTAPI RtlAcquireSRWLockShared(PSRWLOCK lock) {
  volatile ULONG_PTR *word = SRW_WORD(lock);
  for (;;) {
    const ULONG_PTR v = *word;
    if (!(v & SRW_EXCLUSIVE_)) { if (__sync_bool_compare_and_swap(word, v, v + SRW_READER_)) return; }
    else srw_wait(word, v);
  }
}
EXPORT BOOLEAN NTAPI RtlTryAcquireSRWLockShared(PSRWLOCK lock) {
  volatile ULONG_PTR *word = SRW_WORD(lock);
  for (;;) {
    const ULONG_PTR v = *word;
    if (v & SRW_EXCLUSIVE_) return FALSE;
    if (__sync_bool_compare_and_swap(word, v, v + SRW_READER_)) return TRUE;
  }
}
EXPORT void NTAPI RtlReleaseSRWLockShared(PSRWLOCK lock) {
  volatile ULONG_PTR *word = SRW_WORD(lock);
  if ((*word & SRW_EXCLUSIVE_) || *word < SRW_READER_) {
    EXCEPTION_RECORD record;
    memset(&record, 0, sizeof(record));
    record.ExceptionCode = 0xC0000264u;
    record.ExceptionFlags = 1;
    RtlRaiseException(&record);
    return;
  }
  const ULONG_PTR after = __sync_sub_and_fetch(word, SRW_READER_);
  if (after < SRW_READER_) srw_wake_if_waited(word);      /* the last reader: a writer may go on */
}
EXPORT void NTAPI RtlAcquireReleaseSRWLockExclusive(PSRWLOCK lock) { RtlAcquireSRWLockExclusive(lock); RtlReleaseSRWLockExclusive(lock); }
EXPORT void NTAPI RtlConvertSRWLockExclusiveToShared(PSRWLOCK lock) {
  volatile ULONG_PTR *word = SRW_WORD(lock);
  for (;;) {
    const ULONG_PTR v = *word;
    if (__sync_bool_compare_and_swap(word, v, (v & ~(ULONG_PTR)SRW_EXCLUSIVE_) + SRW_READER_)) break;
  }
  srw_wake_if_waited(word);      /* (other readers may come in now) */
}
/* Gives up the shared lock and takes the exclusive one when nobody else shares; if somebody does, the shared lock is let go and FALSE answered. */
EXPORT BOOLEAN NTAPI RtlTryConvertSRWLockSharedToExclusiveOrRelease(PSRWLOCK lock) {
  volatile ULONG_PTR *word = SRW_WORD(lock);
  for (;;) {
    const ULONG_PTR v = *word;
    if ((v & ~(ULONG_PTR)SRW_WAITERS_) == SRW_READER_) { if (__sync_bool_compare_and_swap(word, v, (v & SRW_WAITERS_) | SRW_EXCLUSIVE_)) return TRUE; }
    else { RtlReleaseSRWLockShared(lock); return FALSE; }
  }
}
EXPORT void NTAPI RtlUpdateClonedSRWLock(PSRWLOCK lock, LONG shared) { (void)shared; (void)lock; }

/* ---- condition variables: a counter of wake-ups; a sleeper waits for it to change ---- */

EXPORT void NTAPI RtlInitializeConditionVariable(PCONDITION_VARIABLE cv) { cv->Ptr = NULL; }
EXPORT void NTAPI RtlWakeConditionVariable(PCONDITION_VARIABLE cv) {
  __sync_fetch_and_add((volatile ULONG_PTR *)&cv->Ptr, 1);
  RtlWakeAddressSingle(&cv->Ptr);
}
EXPORT void NTAPI RtlWakeAllConditionVariable(PCONDITION_VARIABLE cv) {
  __sync_fetch_and_add((volatile ULONG_PTR *)&cv->Ptr, 1);
  RtlWakeAddressAll(&cv->Ptr);
}
static NTSTATUS cv_sleep(PCONDITION_VARIABLE cv, PLARGE_INTEGER timeout, void (*leave)(PVOID, ULONG), void (*enter)(PVOID, ULONG), PVOID lock, ULONG flags) {
  ULONG_PTR seen = (ULONG_PTR)cv->Ptr;
  leave(lock, flags);
  const NTSTATUS status = RtlWaitOnAddress(&cv->Ptr, &seen, sizeof(seen), timeout);
  enter(lock, flags);
  return status;
}
static void cv_cs_leave(PVOID lock, ULONG flags) { (void)flags; RtlLeaveCriticalSection(lock); }
static void cv_cs_enter(PVOID lock, ULONG flags) { (void)flags; RtlEnterCriticalSection(lock); }
static void cv_srw_leave(PVOID lock, ULONG flags) { if (flags & 1 /* CONDITION_VARIABLE_LOCKMODE_SHARED */) RtlReleaseSRWLockShared(lock); else RtlReleaseSRWLockExclusive(lock); }
static void cv_srw_enter(PVOID lock, ULONG flags) { if (flags & 1) RtlAcquireSRWLockShared(lock); else RtlAcquireSRWLockExclusive(lock); }
EXPORT NTSTATUS NTAPI RtlSleepConditionVariableCS(PCONDITION_VARIABLE cv, PRTL_CRITICAL_SECTION cs, PLARGE_INTEGER timeout) { return cv_sleep(cv, timeout, cv_cs_leave, cv_cs_enter, cs, 0); }
EXPORT NTSTATUS NTAPI RtlSleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, PLARGE_INTEGER timeout, ULONG flags) {
  if (flags & ~1u) return STATUS_INVALID_PARAMETER;
  return cv_sleep(cv, timeout, cv_srw_leave, cv_srw_enter, lock, flags);
}

/* ---- run-once initialization: the word is 0 (not begun), 1 (a thread runs the initializer), 2 | context (done) or 3 (asynchronous: several may run it at once) ---- */

#define RUN_ONCE_CHECK_ONLY_ 1u
#define RUN_ONCE_ASYNC_ 2u
#define RUN_ONCE_INIT_FAILED_ 4u
EXPORT void NTAPI RtlRunOnceInitialize(PINIT_ONCE once) { once->Ptr = NULL; }
EXPORT NTSTATUS NTAPI RtlRunOnceBeginInitialize(PINIT_ONCE once, ULONG flags, PVOID *context) {
  volatile ULONG_PTR *word = (volatile ULONG_PTR *)&once->Ptr;
  if (flags & ~(RUN_ONCE_CHECK_ONLY_ | RUN_ONCE_ASYNC_)) return STATUS_INVALID_PARAMETER;
  if (flags & RUN_ONCE_CHECK_ONLY_) {
    if (flags & RUN_ONCE_ASYNC_) return STATUS_INVALID_PARAMETER;
    const ULONG_PTR v = *word;
    if ((v & 3) != 2) return SYNC_STATUS_UNSUCCESSFUL_;
    if (context) *context = (PVOID)(v & ~(ULONG_PTR)3);
    return STATUS_SUCCESS;
  }
  for (;;) {
    const ULONG_PTR v = *word;
    switch (v & 3) {
      case 0:
        if (!__sync_bool_compare_and_swap(word, v, (flags & RUN_ONCE_ASYNC_) ? 3 : 1)) continue;
        return STATUS_PENDING;
      case 1:
        if (flags & RUN_ONCE_ASYNC_) return STATUS_INVALID_PARAMETER;
        { ULONG_PTR expected = v; RtlWaitOnAddress(word, &expected, sizeof(expected), NULL); }
        continue;
      case 2:
        if (context) *context = (PVOID)(v & ~(ULONG_PTR)3);
        return STATUS_SUCCESS;
      default:
        if (!(flags & RUN_ONCE_ASYNC_)) return STATUS_INVALID_PARAMETER;
        return STATUS_PENDING;
    }
  }
}
EXPORT NTSTATUS NTAPI RtlRunOnceComplete(PINIT_ONCE once, ULONG flags, PVOID context) {
  volatile ULONG_PTR *word = (volatile ULONG_PTR *)&once->Ptr;
  if (flags & ~(RUN_ONCE_ASYNC_ | RUN_ONCE_INIT_FAILED_)) return STATUS_INVALID_PARAMETER;
  ULONG_PTR value;
  if (flags & RUN_ONCE_INIT_FAILED_) {
    if (context) return STATUS_INVALID_PARAMETER;
    value = 0;
  } else {
    if ((ULONG_PTR)context & 3) return STATUS_INVALID_PARAMETER;
    value = (ULONG_PTR)context | 2;
  }
  for (;;) {
    const ULONG_PTR v = *word;
    switch (v & 3) {
      case 1:
        if (flags & RUN_ONCE_ASYNC_) return STATUS_INVALID_PARAMETER;
        if (!__sync_bool_compare_and_swap(word, v, value)) continue;
        RtlWakeAddressAll((PVOID)word);
        return STATUS_SUCCESS;
      case 3:
        if (!(flags & RUN_ONCE_ASYNC_)) return STATUS_INVALID_PARAMETER;
        if (!__sync_bool_compare_and_swap(word, v, value)) continue;
        return STATUS_SUCCESS;
      default: return STATUS_INVALID_PARAMETER;
    }
  }
}
typedef ULONG (NTAPI *PRTL_RUN_ONCE_INIT_FN_)(PINIT_ONCE, PVOID, PVOID *);
EXPORT NTSTATUS NTAPI RtlRunOnceExecuteOnce(PINIT_ONCE once, PRTL_RUN_ONCE_INIT_FN_ function, PVOID parameter, PVOID *context) {
  PVOID result = NULL;
  NTSTATUS status = RtlRunOnceBeginInitialize(once, 0, &result);
  if (status != STATUS_PENDING) {
    if (status == STATUS_SUCCESS && context) *context = result;
    return status;
  }
  if (!function(once, parameter, &result)) {
    RtlRunOnceComplete(once, RUN_ONCE_INIT_FAILED_, NULL);
    return SYNC_STATUS_UNSUCCESSFUL_;
  }
  status = RtlRunOnceComplete(once, 0, result);
  if (status == STATUS_SUCCESS && context) *context = result;
  return status;
}

/* ---- delays ---- */

EXPORT NTSTATUS NTAPI RtlDelayExecution(BOOLEAN alertable, PLARGE_INTEGER interval) { return NtDelayExecution(alertable, interval); }

/* ---- barriers (RTL_BARRIER: Reserved1 = the number that must arrive, Reserved2 = those who have, Reserved3[0] the generation) ---- */

typedef struct SYNC_BARRIER { volatile LONG total; volatile LONG arrived; volatile ULONG_PTR generation; volatile LONG leaving; LONG spin; } SYNC_BARRIER;
EXPORT NTSTATUS NTAPI RtlInitBarrier(PVOID barrier, ULONG total, ULONG spin) {
  SYNC_BARRIER *b = barrier;
  if (total == 0 || total > 0x7FFFFFFFu) return STATUS_INVALID_PARAMETER;
  memset(b, 0, 32);
  b->total = (LONG)total;
  b->spin = (LONG)spin;
  return STATUS_SUCCESS;
}
EXPORT BOOLEAN NTAPI RtlBarrier(PVOID barrier, ULONG flags) {
  SYNC_BARRIER *b = barrier;
  if (!(flags & 4 /* SYNCHRONIZATION_BARRIER_FLAGS_NO_DELETE */)) __sync_fetch_and_add(&b->leaving, 1);
  const ULONG_PTR generation = b->generation;
  BOOLEAN last = FALSE;
  if (__sync_add_and_fetch(&b->arrived, 1) >= b->total) {
    b->arrived = 0;
    __sync_fetch_and_add(&b->generation, 1);
    RtlWakeAddressAll((PVOID)&b->generation);
    last = TRUE;
  } else {
    while (b->generation == generation) { ULONG_PTR seen = generation; RtlWaitOnAddress(&b->generation, &seen, sizeof(seen), NULL); }
  }
  if (!(flags & 4)) { __sync_fetch_and_sub(&b->leaving, 1); RtlWakeAddressAll((PVOID)&b->leaving); }
  return last;
}
EXPORT BOOLEAN NTAPI RtlBarrierForDelete(PVOID barrier, ULONG flags) { return RtlBarrier(barrier, flags); }
EXPORT NTSTATUS NTAPI RtlDeleteBarrier(PVOID barrier) {
  SYNC_BARRIER *b = barrier;
  for (;;) { LONG seen = b->leaving; if (seen <= 0) break; RtlWaitOnAddress(&b->leaving, &seen, sizeof(seen), NULL); }
  return STATUS_SUCCESS;
}

/* ---- RTL_RESOURCE: many readers or one writer, recursive for the writer ---- */

typedef struct SYNC_RESOURCE {
  RTL_CRITICAL_SECTION guard;
  HANDLE shared_semaphore;
  ULONG shared_waiters;
  HANDLE exclusive_semaphore;
  ULONG exclusive_waiters;
  LONG active;      /* -n: held n times by the writer, n: held by n readers */
  HANDLE owner;
  ULONG timeout_boost;
  PVOID debug_info;
} SYNC_RESOURCE;
EXPORT void NTAPI RtlInitializeResource(PVOID resource) {
  SYNC_RESOURCE *r = resource;
  RtlInitializeCriticalSection(&r->guard);
  r->shared_semaphore = r->exclusive_semaphore = NULL;
  r->shared_waiters = r->exclusive_waiters = 0;
  r->active = 0;
  r->owner = NULL;
  r->timeout_boost = 0;
  r->debug_info = NULL;
}
EXPORT void NTAPI RtlDeleteResource(PVOID resource) { RtlDeleteCriticalSection(&((SYNC_RESOURCE *)resource)->guard); }
static HANDLE resource_me(void) { return (HANDLE)TEB_CLIENT_THREAD(NtCurrentTeb()); }
EXPORT BOOLEAN NTAPI RtlAcquireResourceShared(PVOID resource, BOOLEAN wait) {
  SYNC_RESOURCE *r = resource;
  RtlEnterCriticalSection(&r->guard);
  for (;;) {
    if (r->active > 0 || r->active == 0 || (r->active < 0 && r->owner == resource_me())) {
      if (r->active < 0) --r->active; else ++r->active;
      RtlLeaveCriticalSection(&r->guard);
      return TRUE;
    }
    if (!wait) { RtlLeaveCriticalSection(&r->guard); return FALSE; }
    LONG seen = r->active;
    ++r->shared_waiters;
    RtlLeaveCriticalSection(&r->guard);
    RtlWaitOnAddress(&r->active, &seen, sizeof(seen), NULL);
    RtlEnterCriticalSection(&r->guard);
    --r->shared_waiters;
  }
}
EXPORT BOOLEAN NTAPI RtlAcquireResourceExclusive(PVOID resource, BOOLEAN wait) {
  SYNC_RESOURCE *r = resource;
  RtlEnterCriticalSection(&r->guard);
  for (;;) {
    if (r->active == 0 || (r->active < 0 && r->owner == resource_me())) {
      --r->active;
      r->owner = resource_me();
      RtlLeaveCriticalSection(&r->guard);
      return TRUE;
    }
    if (!wait) { RtlLeaveCriticalSection(&r->guard); return FALSE; }
    LONG seen = r->active;
    ++r->exclusive_waiters;
    RtlLeaveCriticalSection(&r->guard);
    RtlWaitOnAddress(&r->active, &seen, sizeof(seen), NULL);
    RtlEnterCriticalSection(&r->guard);
    --r->exclusive_waiters;
  }
}
EXPORT void NTAPI RtlReleaseResource(PVOID resource) {
  SYNC_RESOURCE *r = resource;
  RtlEnterCriticalSection(&r->guard);
  if (r->active > 0) --r->active;
  else if (r->active < 0 && ++r->active == 0) r->owner = NULL;
  const int wake = (r->active == 0 || r->active == 1) && (r->shared_waiters || r->exclusive_waiters);
  RtlLeaveCriticalSection(&r->guard);
  if (wake) RtlWakeAddressAll(&r->active);
}
EXPORT BOOLEAN NTAPI RtlConvertSharedToExclusive(PVOID resource) {
  SYNC_RESOURCE *r = resource;
  RtlEnterCriticalSection(&r->guard);
  for (;;) {
    if (r->active == 1) { r->active = -1; r->owner = resource_me(); RtlLeaveCriticalSection(&r->guard); return TRUE; }
    LONG seen = r->active;
    ++r->exclusive_waiters;
    RtlLeaveCriticalSection(&r->guard);
    RtlWaitOnAddress(&r->active, &seen, sizeof(seen), NULL);
    RtlEnterCriticalSection(&r->guard);
    --r->exclusive_waiters;
  }
}
EXPORT void NTAPI RtlConvertExclusiveToShared(PVOID resource) {
  SYNC_RESOURCE *r = resource;
  RtlEnterCriticalSection(&r->guard);
  if (r->active == -1) { r->active = 1; r->owner = NULL; }
  const int wake = r->shared_waiters != 0;
  RtlLeaveCriticalSection(&r->guard);
  if (wake) RtlWakeAddressAll(&r->active);
}
EXPORT void NTAPI RtlDumpResource(PVOID resource) { (void)resource; }

/* NtAlertMultipleThreadByThreadId(PHANDLE ThreadIds, ULONG Count, PVOID, PVOID): alerts all of them, or none if one of the ids is not a thread of this process. */
EXPORT NTSTATUS NTAPI NtAlertMultipleThreadByThreadId(PHANDLE ids, ULONG count, PVOID unused1, PVOID unused2) {
  (void)unused1; (void)unused2;
  if (count == 0) return STATUS_SUCCESS;
  if (!ids) return STATUS_ACCESS_VIOLATION;
  for (ULONG i = 0; i < count; ++i) {      /* (asking for a handle to the thread is how it is found out whether it is there) */
    HANDLE thread = NULL;
    ULONG_PTR client[2] = {0, (ULONG_PTR)ids[i]};
    OBJECT_ATTRIBUTES attributes = {sizeof(attributes), NULL, NULL, 0, NULL, NULL};
    const NTSTATUS status = NtOpenThread(&thread, 0x100000 /* SYNCHRONIZE */, &attributes, client);
    if (status != STATUS_SUCCESS) return status == STATUS_ACCESS_DENIED ? status : (NTSTATUS)0xC000000B /* STATUS_INVALID_CID */;
    NtClose(thread);
  }
  NTSTATUS result = STATUS_SUCCESS;
  for (ULONG i = 0; i < count; ++i) {
    const NTSTATUS status = NtAlertThreadByThreadId((ULONG_PTR)ids[i]);
    if (status != STATUS_SUCCESS) result = status;
  }
  return result;
}
