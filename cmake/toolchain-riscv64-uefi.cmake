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
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

# Cross toolchain for RISC-V 64 UEFI applications.
# LLVM has no RISC-V PE/COFF target, so (like EDK2's GenFw) the image is built as a position-independent ELF
# executable with clang and ld.lld, then scripts/elf2efi.py rewrites it as a PE32+ file (machine 0x5064) whose base
# relocation table is made from the ELF's relative relocations. Override LD_LLD to use another ld.lld.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

set(TRIPLE riscv64-unknown-elf)
set(CMAKE_C_COMPILER   clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET   ${TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${TRIPLE})
set(CMAKE_AR     llvm-ar)
set(CMAKE_RANLIB llvm-ranlib)

# The mingw-w64 libstdc++ headers are plain C++ and serve the freestanding headers this OS uses
# (<new>, <type_traits>, <initializer_list>, ...); only the target-independent ones are needed. Its C headers are
# not used: clang's own freestanding stdint.h and stddef.h serve the ELF target.
set(MINGW_SYSROOT /usr/x86_64-w64-mingw32 CACHE PATH "mingw-w64 headers (for libstdc++'s freestanding headers)")
file(GLOB MINGW_CXX_DIRS ${MINGW_SYSROOT}/include/c++/*)
list(GET MINGW_CXX_DIRS -1 MINGW_CXX_DIR)
set(TCOS_RISCV64_INCLUDES "SHELL:-isystem ${MINGW_CXX_DIR}" "SHELL:-isystem ${MINGW_CXX_DIR}/x86_64-w64-mingw32")

file(GLOB RUSTUP_LD_LLD /usr/bin/ld.lld $ENV{HOME}/.rustup/toolchains/*/lib/rustlib/*/bin/gcc-ld/ld.lld)
list(GET RUSTUP_LD_LLD 0 LD_LLD_DEFAULT)
set(LD_LLD ${LD_LLD_DEFAULT} CACHE FILEPATH "ld.lld used to link the ELF image")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_EXECUTABLE_SUFFIX ".efi")
