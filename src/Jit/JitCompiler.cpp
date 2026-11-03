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
#include "Jit/JitCompiler.hpp"
#include <new>

namespace jit {

namespace {
// The entry point of a region is its first byte: turning that address into a function pointer is what "loading code" means. Only regions that
// make_executable() accepted get here.
JitEntry entry_of(const CodeRegion& region) { return reinterpret_cast<JitEntry>(region.bytes.data()); }
}  // namespace

CompileResult JitCompiler::generate(IJitFrontend& frontend, uint64_t guest_pc, CodeBuffer& out, uint64_t* guest_size) {
  IrBlock block(allocator_);
  IrBuilder builder(block);
  *guest_size = 0;
  if (!frontend.translate(guest_pc, builder, guest_size)) return CompileResult::Untranslatable;
  if (builder.failed()) return CompileResult::OutOfMemory;
  if (!validate(allocator_, block)) return CompileResult::Untranslatable;
  if (!backend_.compile(block, out)) return out.failed() ? CompileResult::OutOfMemory : CompileResult::BackendFailed;
  return CompileResult::Compiled;
}

CompileResult JitCompiler::compile(IJitFrontend& frontend, uint64_t guest_pc, uint32_t tier) {
  CodeBuffer code(allocator_);
  uint64_t guest_size = 0;
  CompileResult result = generate(frontend, guest_pc, code, &guest_size);
  if (result != CompileResult::Compiled) {
    __atomic_fetch_add(&failed_, 1, __ATOMIC_RELAXED);
    return result;
  }
  CodeRegion region;
  if (!memory_.allocate(code.size(), &region)) {
    __atomic_fetch_add(&failed_, 1, __ATOMIC_RELAXED);
    return CompileResult::OutOfMemory;
  }
  if (!region.bytes.copy_from(code.view()) || !memory_.make_executable(region)) {
    memory_.release(region);
    __atomic_fetch_add(&failed_, 1, __ATOMIC_RELAXED);
    return CompileResult::NotExecutable;
  }
  // The block is built in memory from the allocator, like UniquePointer does (placement new into allocator memory is the one place a
  // RCU-retired object is created, since the cache, not an owner, ends its life).
  void* memory = allocator_.allocate(sizeof(CompiledBlock));
  if (!memory) {
    memory_.release(region);
    __atomic_fetch_add(&failed_, 1, __ATOMIC_RELAXED);
    return CompileResult::OutOfMemory;
  }
  CompiledBlock* block = new (memory) CompiledBlock();
  block->guest_start = guest_pc;
  block->guest_size = guest_size;
  block->region = region;
  block->code_size = code.size();
  block->tier = tier;
  block->memory = &memory_;
  block->allocator = &allocator_;
  block->entry = entry_of(region);
  cache_.publish(block);
  __atomic_fetch_add(&compiled_, 1, __ATOMIC_RELAXED);
  return CompileResult::Compiled;
}

uint64_t run_block(const CompiledBlock& block, ByteView state) { return block.entry(state.data()); }

bool JitDispatcher::run_cached(uint64_t guest_pc, ByteView state, uint64_t* exit_value) {
  kernel::RcuReader reader;
  const CompiledBlock* block = cache_.lookup(guest_pc);
  if (!block) return false;
  *exit_value = run_block(*block, state);
  return true;
}

bool JitDispatcher::try_run(uint64_t guest_pc, ByteView state, uint64_t* exit_value) {
  if (run_cached(guest_pc, state, exit_value)) return true;
  if (!hotness_.record(guest_pc)) return false;
  if (compiler_.compile(frontend_, guest_pc) != CompileResult::Compiled) return false;
  return run_cached(guest_pc, state, exit_value);
}

void JitDispatcher::invalidate(uint64_t start, uint64_t size) {
  if (cache_.invalidate(start, size)) hotness_.reset_all();
}

}  // namespace jit
