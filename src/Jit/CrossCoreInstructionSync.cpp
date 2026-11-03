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
#include "Jit/CrossCoreInstructionSync.hpp"
#include "Kernel/Thread.hpp"

namespace jit {

namespace {
void synchronise_this_core(void*) {
#if defined(__x86_64__)
  uint32_t eax = 0, ebx, ecx = 0, edx;
  __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx)::"memory");   // serialising
#elif defined(__aarch64__)
  __asm__ volatile("dsb ish\n\tisb" ::: "memory");
#elif defined(__riscv)
  __asm__ volatile("fence rw, rw\n\tfence.i" ::: "memory");
#elif defined(__loongarch64)
  __asm__ volatile("dbar 0\n\tibar 0" ::: "memory");
#else
#error "No instruction stream synchronisation for this architecture"
#endif
}
}  // namespace

bool CrossCoreInstructionSync::sync(ConstByteView code) {
  (void)code;   // (every core synchronises its whole instruction stream; there is no per-range form)
  return kernel::run_on_every_core(synchronise_this_core, nullptr);
}

}  // namespace jit
