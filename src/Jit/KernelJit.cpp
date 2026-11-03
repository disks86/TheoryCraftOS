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
#include "Jit/KernelJit.hpp"

namespace jit {

namespace {
ICodeMemory* g_kernel_code_memory = nullptr;
}

void set_kernel_code_memory(ICodeMemory* memory) { __atomic_store_n(&g_kernel_code_memory, memory, __ATOMIC_RELEASE); }
ICodeMemory* kernel_code_memory() { return __atomic_load_n(&g_kernel_code_memory, __ATOMIC_ACQUIRE); }

}  // namespace jit
