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
#include "Jit/CodeMemory.hpp"

namespace jit {

// The kernel's code memory, for whoever wants to run generated code (the emulator, the VMs). main() builds a KernelCodeMemory over its page tables and a
// CodePool, and registers it here; users ask for it. Null before that, or where the page tables are not usable (5-level paging), in which case there is
// no JIT and the callers keep interpreting.
void set_kernel_code_memory(ICodeMemory* memory);
ICodeMemory* kernel_code_memory();

}  // namespace jit
