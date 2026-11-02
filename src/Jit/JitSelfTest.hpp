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
#include "Jit/CodeMemory.hpp"

namespace jit {

enum class SelfTestResult : uint8_t {
  Passed,
  NoCodeMemory,    // the code memory could not provide or switch a region
  CompileFailed,   // the front end, validation or the back end refused the program
  Mismatch,        // the compiled code returned something else than the interpreter
};

// Compiles a small program (a loop with a helper call and values that live across it) with the back end for this CPU, runs it from `memory`, and compares
// the result and the state with the IR interpreter. A boot-time check that the code memory really gives executable pages, that the instruction caches are
// synchronised and that the calling convention matches. Needs the scheduler (RCU and the cross-core calls) to be running. `compiled_bytes` (optional)
// receives the size of the generated code.
SelfTestResult jit_self_test(allocators::IAllocator& allocator, ICodeMemory& memory, uint64_t* compiled_bytes = nullptr);

const char* self_test_result_name(SelfTestResult result);

}  // namespace jit
