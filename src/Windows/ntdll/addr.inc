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
/* ntdll.dll: RtlWaitOnAddress and RtlWakeAddressSingle/All, the park/unpark behind WaitOnAddress, SRW locks, condition variables and critical sections. Waiters are on one
   of 128 lists picked by a hash of the address, each under its own spin lock: a waiter counts itself in its list, puts a node (on its own stack) in it, compares the value
   again with the lock held, and sleeps in NtWaitForAlertByThreadId; a waker takes the nodes of the address out of the list and alerts their threads. A waker that finds
   nobody counted in the list (after its own store, with a full fence) leaves at once, so letting go of a lock nobody waits for costs no system call. The kernel keeps an
   alert that comes before the wait, so no wake-up is lost. Included by ntdll.c after rtl3.inc. */

#ifndef STATUS_TIMEOUT
#define STATUS_TIMEOUT ((NTSTATUS)0x102)
#endif
IMPORT NTSTATUS NTAPI NtAlertThreadByThreadId(ULONG_PTR);
IMPORT NTSTATUS NTAPI NtWaitForAlertByThreadId(PVOID, PLARGE_INTEGER);
IMPORT NTSTATUS NTAPI NtYieldExecution(void);

typedef struct AddressWaiter {
  struct AddressWaiter *next, *prev;
  const volatile void *address;
  ULONG_PTR thread;
  volatile LONG queued;
} AddressWaiter;
#define ADDRESS_BUCKETS 128
typedef struct AddressBucket {
  AddressWaiter head;
  volatile LONG lock;
  volatile LONG waiters;      /* threads that are in, or about to be in, the list */
  char pad[16];      /* (to a cache line of its own: 64 bytes) */
} AddressBucket;
static AddressBucket g_address_buckets[ADDRESS_BUCKETS];
static AddressBucket *address_bucket(const volatile void *address) {
  ULONG_PTR h = (ULONG_PTR)address;
  h ^= h >> 17;
  h *= 0x9E3779B97F4A7C15ull;
  return &g_address_buckets[(h >> 32) % ADDRESS_BUCKETS];
}
static void address_lock(AddressBucket *b) {
  for (unsigned spins = 0; __sync_lock_test_and_set(&b->lock, 1);) {
    while (b->lock) {
      if (++spins < 256) __asm__ volatile("pause");
      else NtYieldExecution();      /* (the holder may have been preempted) */
    }
  }
}
static void address_unlock(AddressBucket *b) { __sync_lock_release(&b->lock); }
/* (with the lock held) the list of a bucket that was never used is made: an empty list points at itself */
static void address_list_ready(AddressBucket *b) { if (!b->head.next) { b->head.next = &b->head; b->head.prev = &b->head; } }
static void address_unlink(AddressWaiter *w) { w->prev->next = w->next; w->next->prev = w->prev; w->queued = 0; }
static BOOLEAN address_equal(const volatile void *address, const void *compare, SIZE_T size) {
  switch (size) {
    case 1: return *(const volatile BYTE *)address == *(const BYTE *)compare;
    case 2: return *(const volatile WORD *)address == *(const WORD *)compare;
    case 4: return *(const volatile DWORD *)address == *(const DWORD *)compare;
    default: return *(const volatile ULONGLONG *)address == *(const ULONGLONG *)compare;
  }
}

EXPORT NTSTATUS NTAPI RtlWaitOnAddress(volatile VOID *address, PVOID compare, SIZE_T size, PLARGE_INTEGER timeout) {
  if (size != 1 && size != 2 && size != 4 && size != 8) return STATUS_INVALID_PARAMETER;
  AddressBucket *b = address_bucket(address);
  AddressWaiter w;
  w.address = address;
  w.thread = TEB_CLIENT_THREAD(NtCurrentTeb());
  address_lock(b);
  address_list_ready(b);
  __atomic_add_fetch(&b->waiters, 1, __ATOMIC_SEQ_CST);      /* (before the value is looked at: a waker that changed it then sees this count) */
  if (!address_equal(address, compare, size)) {
    __atomic_sub_fetch(&b->waiters, 1, __ATOMIC_SEQ_CST);
    address_unlock(b);
    return STATUS_SUCCESS;
  }
  w.next = &b->head;
  w.prev = b->head.prev;
  w.prev->next = &w;
  b->head.prev = &w;
  w.queued = 1;
  address_unlock(b);
  const NTSTATUS status = NtWaitForAlertByThreadId((PVOID)address, timeout);
  address_lock(b);
  const BOOLEAN removed_by_waker = !w.queued;
  if (w.queued) address_unlink(&w);
  __atomic_sub_fetch(&b->waiters, 1, __ATOMIC_SEQ_CST);
  address_unlock(b);
  if (status == STATUS_TIMEOUT && removed_by_waker) {      /* woken just as the time ran out: take the alert so that it does not end the next wait */
    LARGE_INTEGER none;
    none.QuadPart = 0;
    NtWaitForAlertByThreadId(NULL, &none);
    return STATUS_SUCCESS;
  }
  return status == STATUS_TIMEOUT ? STATUS_TIMEOUT : STATUS_SUCCESS;
}

static void address_wake(const volatile void *address, BOOLEAN all) {
  AddressBucket *b = address_bucket(address);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);      /* (the caller's store to the address comes before the count is read) */
  if (__atomic_load_n(&b->waiters, __ATOMIC_SEQ_CST) == 0) return;
  for (;;) {
    ULONG_PTR threads[16];
    unsigned count = 0;
    BOOLEAN more = FALSE;
    address_lock(b);
    address_list_ready(b);
    for (AddressWaiter *w = b->head.next; w != &b->head;) {
      AddressWaiter *next = w->next;
      if (w->address == address) {
        if (count == 16) { more = TRUE; break; }
        threads[count++] = w->thread;
        address_unlink(w);
        if (!all) break;
      }
      w = next;
    }
    address_unlock(b);
    for (unsigned i = 0; i < count; ++i) NtAlertThreadByThreadId(threads[i]);
    if (!more) return;
  }
}
EXPORT VOID NTAPI RtlWakeAddressSingle(PVOID address) { address_wake(address, FALSE); }
EXPORT VOID NTAPI RtlWakeAddressAll(PVOID address) { address_wake(address, TRUE); }
EXPORT VOID NTAPI RtlWakeAddressSingleNoFence(PVOID address) { address_wake(address, FALSE); }
EXPORT VOID NTAPI RtlWakeAddressAllNoFence(PVOID address) { address_wake(address, TRUE); }
