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
#include "Jit/JitRuntime.hpp"
#include "Jit/Backend/AArch64Backend.hpp"
#include "Jit/Backend/RiscV64Backend.hpp"
#include "Jit/Backend/X86_64Backend.hpp"
#include "Jit/KernelJit.hpp"

namespace jit {

namespace {

UniquePointer<IJitBackend> make_backend(allocators::IAllocator& allocator) {
  switch (native_host_arch()) {
    case HostArch::X86_64: return UniquePointer<IJitBackend>(UniquePointer<X86_64Backend>::make(allocator, allocator));
    case HostArch::AArch64: return UniquePointer<IJitBackend>(UniquePointer<AArch64Backend>::make(allocator, allocator, 32u, false));
    case HostArch::RiscV64: return UniquePointer<IJitBackend>(UniquePointer<RiscV64Backend>::make(allocator, allocator, 32u, false));
  }
  return UniquePointer<IJitBackend>();
}

}  // namespace

JitRuntime::JitRuntime(allocators::IAllocator& allocator, ICodeMemory& memory, uint32_t hot_threshold, uint64_t cache_slots)
    : allocator_(allocator),
      backend_(make_backend(allocator)),
      cache_(allocator, cache_slots),
      hotness_(allocator, cache_slots, hot_threshold),
      compiler_(allocator, memory, *backend_, cache_) {}

JitRuntime::~JitRuntime() = default;

UniquePointer<JitRuntime> JitRuntime::create(allocators::IAllocator& allocator, uint32_t hot_threshold, ICodeMemory* memory, uint64_t cache_slots) {
  if (!memory) memory = kernel_code_memory();
  if (!memory || !memory->can_execute()) return UniquePointer<JitRuntime>();
  UniquePointer<JitRuntime> runtime = UniquePointer<JitRuntime>::make(allocator, allocator, *memory, hot_threshold, cache_slots);
  if (!runtime || !runtime->ok()) return UniquePointer<JitRuntime>();
  return runtime;
}

bool JitRuntime::run_cached(uint64_t key, ByteView state, uint64_t* exit_value) {
  kernel::RcuReader reader;
  const CompiledBlock* block = cache_.lookup(key);
  if (!block) return false;
  *exit_value = run_block(*block, state);
  __atomic_fetch_add(&runs_, 1, __ATOMIC_RELAXED);
  return true;
}

bool JitRuntime::try_run(IJitFrontend& frontend, uint64_t key, ByteView state, uint64_t* exit_value) {
  if (run_cached(key, state, exit_value)) return true;
  if (!hotness_.record(key)) return false;
  if (compiler_.compile(frontend, key) != CompileResult::Compiled) return false;
  return run_cached(key, state, exit_value);
}

}  // namespace jit
