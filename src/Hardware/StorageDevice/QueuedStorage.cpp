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

#include "Hardware/StorageDevice/QueuedStorage.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
// The storage vectors (kVectorStorageFirst...) in use, one bit each, shared by every controller.
uint32_t g_storage_vectors = 0;
}  // namespace

uint8_t claim_storage_vector() {
#if defined(__x86_64__)
  uint32_t used = __atomic_load_n(&g_storage_vectors, __ATOMIC_RELAXED);
  for (;;) {
    if (used == ~0u) return 0;
    const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(~used));
    if (bit >= kernel::kVectorStorageCount) return 0;
    if (__atomic_compare_exchange_n(&g_storage_vectors, &used, used | (1u << bit), false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
      return static_cast<uint8_t>(kernel::kVectorStorageFirst + bit);
  }
#else
  return 0;
#endif
}

void release_storage_vector(uint8_t vector) {
#if defined(__x86_64__)
  if (vector >= kernel::kVectorStorageFirst && vector < kernel::kVectorStorageFirst + kernel::kVectorStorageCount)
    __atomic_fetch_and(&g_storage_vectors, ~(1u << (vector - kernel::kVectorStorageFirst)), __ATOMIC_ACQ_REL);
#else
  (void)vector;
#endif
}

uint64_t storage_now_us() {
  const uint64_t per_us = tsc_ticks_per_us();
  return per_us ? read_tsc() / per_us : 0;
}

void describe_queues(IPropertySink& sink, uint32_t queue_count, uint32_t depth, bool interrupts, const QueueReport* queues) {
  sink.add_number("I/O queues", queue_count);
  sink.add_number("Queue depth", depth);
  sink.add_text("Completions", interrupts ? (queue_count > 1 ? "MSI-X interrupt per queue, on its core" : "interrupt") : "polled");
  if (!interrupts) return;
  char text[32 * 21];   // "core:count " per queue
  uint32_t n = 0;
  auto put = [&](uint64_t v) {
    char digits[20];
    uint32_t d = 0;
    do digits[d++] = static_cast<char>('0' + v % 10); while ((v /= 10) && d < sizeof digits);
    while (d && n < sizeof text) text[n++] = digits[--d];
  };
  for (uint32_t i = 0; i < queue_count && i < 32; ++i) {
    put(queues[i].core);
    if (n < sizeof text) text[n++] = ':';
    put(queues[i].interrupts);
    if (n < sizeof text && i + 1 < queue_count) text[n++] = ' ';
  }
  sink.add("Interrupts (core:count)", StringView(text, n));
}

void SlotPool::reset(uint32_t count) {
  count_ = count < kMaxSlots ? count : kMaxSlots;
  __atomic_store_n(&free_, count_ == 64 ? ~uint64_t{0} : (uint64_t{1} << count_) - 1, __ATOMIC_RELEASE);
}

bool SlotPool::try_acquire(uint32_t* slot) {
  uint64_t free = __atomic_load_n(&free_, __ATOMIC_ACQUIRE);
  while (free) {
    const uint32_t s = static_cast<uint32_t>(__builtin_ctzll(free));
    if (__atomic_compare_exchange_n(&free_, &free, free & ~(uint64_t{1} << s), true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      *slot = s;
      return true;
    }
  }
  return false;
}

uint32_t SlotPool::acquire() {
  uint32_t slot;
  while (!try_acquire(&slot)) {
    if (kernel::running()) kernel::yield();
    else kernel::cpu_relax();
  }
  return slot;
}

void SlotPool::release(uint32_t slot) {
  if (slot < count_) __atomic_fetch_or(&free_, uint64_t{1} << slot, __ATOMIC_RELEASE);
}

}  // namespace hardware
