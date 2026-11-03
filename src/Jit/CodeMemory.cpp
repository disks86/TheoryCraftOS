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
#include "Jit/CodeMemory.hpp"

namespace jit {

void flush_instruction_cache(ConstByteView code) {
  if (code.empty()) return;
#if defined(__x86_64__)
  // The instruction cache of an x86 core is coherent with its data writes; only the pipeline must not run ahead of them, and the privilege/permission
  // change that follows (or the serialising jump into the code) takes care of it.
  __asm__ volatile("" ::: "memory");
#elif defined(__aarch64__)
  // Clean the data cache to the point of unification, invalidate the instruction cache over the same lines, and synchronise. The line sizes are in CTR_EL0
  // (the compiler builtin would call __clear_cache, which a freestanding kernel does not have).
  uint64_t ctr;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  const uint64_t dline = 4ull << (ctr >> 16 & 15), iline = 4ull << (ctr & 15);
  const uint64_t begin = reinterpret_cast<uint64_t>(code.data()), end = begin + code.size();
  for (uint64_t a = begin & ~(dline - 1); a < end; a += dline) __asm__ volatile("dc cvau, %0" ::"r"(a) : "memory");
  __asm__ volatile("dsb ish" ::: "memory");
  for (uint64_t a = begin & ~(iline - 1); a < end; a += iline) __asm__ volatile("ic ivau, %0" ::"r"(a) : "memory");
  __asm__ volatile("dsb ish\n\tisb" ::: "memory");
#elif defined(__riscv)
  // FENCE.I synchronises this hart only. Other harts that may run the code need an inter-processor FENCE.I (SBI remote fence) before they enter it:
  // that is the job of the ITlbShootdown-style hook of the code memory that makes the region executable.
  __asm__ volatile("fence rw, rw\n\tfence.i" ::: "memory");
#else
#error "No instruction cache flush for this architecture"
#endif
}

bool HeapCodeMemory::allocate(uint64_t size, CodeRegion* out) {
  if (size == 0 || size > UINT64_MAX - (kPage - 1)) return false;
  const uint64_t rounded = (size + kPage - 1) & ~(kPage - 1);
  auto* memory = static_cast<uint8_t*>(allocator_.allocate_aligned(rounded, kPage));
  if (!memory) return false;
  out->bytes = ByteView(memory, rounded);
  out->bytes.fill(0);
  return true;
}

void HeapCodeMemory::release(const CodeRegion& region) {
  if (!region.empty()) allocator_.deallocate(region.bytes.data());
}

}  // namespace jit
