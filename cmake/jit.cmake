# TheoryCraftOS - a minimal UEFI operating system.
# Copyright (C) 2026 Christopher Schaefer
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License

# The part of src/Jit that does not depend on the CPU's page tables: the IR, the assemblers, the three back ends, the code cache and the compiler. Every
# image includes it (the back ends generate code for any CPU, whatever the image runs on). KernelCodeMemory and CrossCoreTlbShootdown need
# kernel::PageTables, which only the x86-64 image has, and are listed there.
set(TCOS_JIT_PORTABLE_SOURCES
  src/Jit/CodeCache.cpp
  src/Jit/CodeMemory.cpp
  src/Jit/CodePageSource.cpp
  src/Jit/CrossCoreInstructionSync.cpp
  src/Jit/JitCompiler.cpp
  src/Jit/JitSelfTest.cpp
  src/Jit/ProtectedCodeMemory.cpp
  src/Jit/UefiPageProtector.cpp
  src/Jit/KernelJit.cpp
  src/Jit/Backend/AArch64Backend.cpp
  src/Jit/Backend/LoongArch64Backend.cpp
  src/Jit/Backend/RiscV64Backend.cpp
  src/Jit/Backend/X86_64Backend.cpp
  src/Jit/Emitter/AArch64Assembler.cpp
  src/Jit/Emitter/LoongArch64Assembler.cpp
  src/Jit/Emitter/CodeBuffer.cpp
  src/Jit/Emitter/RiscV64Assembler.cpp
  src/Jit/Emitter/X86_64Assembler.cpp
  src/Jit/Ir/Ir.cpp
  src/Jit/Ir/RegisterAllocator.cpp
)
