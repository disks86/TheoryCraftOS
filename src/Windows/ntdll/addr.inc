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
/* ntdll.dll: RtlWaitOnAddress and RtlWakeAddressSingle/All, the park/unpark behind WaitOnAddress. A waiter puts a node (on its own stack) in one list under a spin lock, compares the
   value again with the lock held, and sleeps in NtWaitForAlertByThreadId; a waker takes the nodes of the address out of the list and alerts their threads. The kernel keeps an
   alert that comes before the wait, so no wake-up is lost. Included by ntdll.c after rtl3.inc. */

#ifndef STATUS_TIMEOUT
#define STATUS_TIMEOUT ((NTSTATUS)0x102)
#endif
IMPORT NTSTATUS NTAPI NtAlertThreadByThreadId(ULONG_PTR);
IMPORT NTSTATUS NTAPI NtWaitForAlertByThreadId(PVOID, PLARGE_INTEGER);

typedef struct AddressWaiter {
  struct AddressWaiter *next, *prev;
  const volatile void *address;
  ULONG_PTR thread;
  volatile LONG queued;
} AddressWaiter;
static AddressWaiter g_address_waiters = {&g_address_waiters, &g_address_waiters, NULL, 0, 0};
static volatile LONG g_address_lock;
static void address_lock(void) { while (__sync_lock_test_and_set(&g_address_lock, 1)) { while (g_address_lock) __asm__ volatile("pause"); } }
static void address_unlock(void) { __sync_lock_release(&g_address_lock); }
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
  AddressWaiter w;
  w.address = address;
  w.thread = TEB_CLIENT_THREAD(NtCurrentTeb());
  address_lock();
  if (!address_equal(address, compare, size)) { address_unlock(); return STATUS_SUCCESS; }
  w.next = &g_address_waiters;
  w.prev = g_address_waiters.prev;
  w.prev->next = &w;
  g_address_waiters.prev = &w;
  w.queued = 1;
  address_unlock();
  const NTSTATUS status = NtWaitForAlertByThreadId((PVOID)address, timeout);
  address_lock();
  const BOOLEAN removed_by_waker = !w.queued;
  if (w.queued) address_unlink(&w);
  address_unlock();
  if (status == STATUS_TIMEOUT && removed_by_waker) {      /* woken just as the time ran out: take the alert so that it does not end the next wait */
    LARGE_INTEGER none;
    none.QuadPart = 0;
    NtWaitForAlertByThreadId(NULL, &none);
    return STATUS_SUCCESS;
  }
  return status == STATUS_TIMEOUT ? STATUS_TIMEOUT : STATUS_SUCCESS;
}

static void address_wake(const volatile void *address, BOOLEAN all) {
  for (;;) {
    ULONG_PTR threads[16];
    unsigned count = 0;
    BOOLEAN more = FALSE;
    address_lock();
    for (AddressWaiter *w = g_address_waiters.next; w != &g_address_waiters;) {
      AddressWaiter *next = w->next;
      if (w->address == address) {
        if (count == 16) { more = TRUE; break; }
        threads[count++] = w->thread;
        address_unlink(w);
        if (!all) break;
      }
      w = next;
    }
    address_unlock();
    for (unsigned i = 0; i < count; ++i) NtAlertThreadByThreadId(threads[i]);
    if (!more) return;
  }
}
EXPORT VOID NTAPI RtlWakeAddressSingle(PVOID address) { address_wake(address, FALSE); }
EXPORT VOID NTAPI RtlWakeAddressAll(PVOID address) { address_wake(address, TRUE); }
EXPORT VOID NTAPI RtlWakeAddressSingleNoFence(PVOID address) { address_wake(address, FALSE); }
EXPORT VOID NTAPI RtlWakeAddressAllNoFence(PVOID address) { address_wake(address, TRUE); }
