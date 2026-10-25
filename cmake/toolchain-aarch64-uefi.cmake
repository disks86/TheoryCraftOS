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

# Cross toolchain for AArch64 UEFI applications.
# clang targets aarch64-w64-mingw32 (PE32+ with the AAPCS64-compatible UEFI ABI) and rust-lld (its lld-link),
# which ships with rustup, links it. Override LLD_LINK (a path to an lld-link) to use another one.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(TRIPLE aarch64-w64-mingw32)
set(CMAKE_C_COMPILER   clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET   ${TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${TRIPLE})
set(CMAKE_AR     llvm-ar)
set(CMAKE_RANLIB llvm-ranlib)

# The mingw-w64 libstdc++ headers are plain C++ and serve the freestanding headers this OS uses
# (<new>, <type_traits>, <initializer_list>, ...); only the target-independent ones are needed.
set(MINGW_SYSROOT /usr/x86_64-w64-mingw32 CACHE PATH "mingw-w64 headers (for libstdc++'s freestanding headers)")
file(GLOB MINGW_CXX_DIRS ${MINGW_SYSROOT}/include/c++/*)
list(GET MINGW_CXX_DIRS -1 MINGW_CXX_DIR)
set(TCOS_AARCH64_INCLUDES "SHELL:-isystem ${MINGW_CXX_DIR}" "SHELL:-isystem ${MINGW_CXX_DIR}/x86_64-w64-mingw32" "SHELL:-isystem ${MINGW_SYSROOT}/include")

file(GLOB RUSTUP_LLD /usr/bin/lld-link $ENV{HOME}/.rustup/toolchains/*/lib/rustlib/*/bin/gcc-ld/lld-link)
list(GET RUSTUP_LLD 0 LLD_LINK_DEFAULT)
set(LLD_LINK ${LLD_LINK_DEFAULT} CACHE FILEPATH "lld-link used to link the PE image")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_EXECUTABLE_SUFFIX ".efi")
