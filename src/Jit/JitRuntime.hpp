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
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Jit/Backend/IJitBackend.hpp"
#include "Jit/CodeCache.hpp"
#include "Jit/JitCompiler.hpp"
#include "Utility/UniquePointer.hpp"

namespace jit {

// Everything one virtual machine (a WebAssembly store, a script engine) needs to run generated code: the back end for this CPU, a code cache, a hotness
// tracker and the compiler, over the kernel's executable memory. The VM keeps one, asks `try_run` at the points where it could enter compiled code, and drops
// it with the VM (the blocks it compiled go back to the code memory then).
//
// Unlike JitDispatcher the front end is passed per call: a VM's front end needs to know which function the key belongs to, and gets that from the VM's own state
// just before the call.
class JitRuntime {
 public:
  // Null when there is nothing to run generated code from: no executable code memory (`memory` is null and the kernel has registered none, or it cannot execute,
  // as in a build that only cross-compiles), or no memory for the tables.
  static UniquePointer<JitRuntime> create(allocators::IAllocator& allocator, uint32_t hot_threshold, ICodeMemory* memory = nullptr, uint64_t cache_slots = 4096);

  JitRuntime(allocators::IAllocator& allocator, ICodeMemory& memory, uint32_t hot_threshold, uint64_t cache_slots);
  ~JitRuntime();
  JitRuntime(const JitRuntime&) = delete;
  JitRuntime& operator=(const JitRuntime&) = delete;
  bool ok() const { return backend_ && cache_.ok() && hotness_.ok(); }

  // At a point where compiled code for `key` could be entered: runs it against `state` and returns true with its exit value; or counts the visit, compiles
  // when it just became hot (the front end translates `key`) and runs that. False means "interpret".
  bool try_run(IJitFrontend& frontend, uint64_t key, ByteView state, uint64_t* exit_value);

  uint64_t compiled() const { return compiler_.compiled(); }
  uint64_t failed() const { return compiler_.failed(); }
  uint64_t runs() const { return __atomic_load_n(&runs_, __ATOMIC_RELAXED); }

 private:
  bool run_cached(uint64_t key, ByteView state, uint64_t* exit_value);

  allocators::IAllocator& allocator_;
  UniquePointer<IJitBackend> backend_;
  CodeCache cache_;
  HotnessTracker hotness_;
  JitCompiler compiler_;
  uint64_t runs_ = 0;
};

}  // namespace jit
