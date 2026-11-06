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

# The LoongArch 64 UEFI image (BOOTLOONGARCH64.EFI) and its QEMU "virt" targets. Included by the top-level
# CMakeLists.txt when TCOS_ARCH=loongarch64. Like the RISC-V image it holds the portable layers plus exception handling,
# the per-core timer tick, the thread scheduler and the other cores (IOCSR mailbox start); see src/Arch/LoongArch64/main.cpp.
add_executable(BOOTLOONGARCH64
  src/Arch/LoongArch64/main.cpp
  src/Arch/LoongArch64/Core.cpp
  src/Arch/LoongArch64/Uart.cpp
  src/Arch/LoongArch64/Timer.cpp
  src/Arch/LoongArch64/Interrupts.cpp
  src/Arch/LoongArch64/Trap.S
  src/Arch/LoongArch64/Secondary.S
  src/Arch/LoongArch64/Cpu.cpp
  src/Arch/LoongArch64/PageProtector.cpp
  src/Kernel/ThreadScheduler.cpp
  src/CPUScheduler/IoAffinity.cpp
  src/Kernel/Sync.cpp
  src/Kernel/Firmware.cpp
  src/Kernel/Delay.cpp
  src/Utility/Bounds.cpp
  src/console.cpp
  src/BootLog.cpp
  src/GraphicsConsole.cpp
  src/runtime.cpp
  src/Allocators/Allocator.cpp
  src/Allocators/PageFrameAllocator.cpp
  src/Hardware/TextConsole.cpp
  src/Hardware/RandomAccessMemory/UefiRandomAccessMemory.cpp
  src/Hardware/Acpi/AcpiTables.cpp
  src/Hardware/GraphicsProcessingUnit/FramebufferGpu.cpp
  src/Hardware/GraphicsProcessingUnit/Font8x16.cpp
  src/Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.cpp
  ${TCOS_JIT_PORTABLE_SOURCES}
)
target_include_directories(BOOTLOONGARCH64 PRIVATE src)
set_target_properties(BOOTLOONGARCH64 PROPERTIES SUFFIX ".efi" LINKER_LANGUAGE CXX)
# The soft-float LP64S ABI (-mfpu=none): no kernel code touches floating-point state, so a trap frame holds integer registers only.
# -mstrict-align because the firmware's unaligned-access setting is not known; -fPIE because the image is relocated at load time.
target_compile_options(BOOTLOONGARCH64 PRIVATE
  ${TCOS_LOONGARCH64_INCLUDES}
  -march=loongarch64 -mabi=lp64s -mfpu=none -mstrict-align -fPIE
  -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-threadsafe-statics
  -fshort-wchar -fno-omit-frame-pointer
  -Wall -Wextra -g -O1
)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# ld.lld produces the ELF, scripts/elf2efi.py rewrites it as the PE32+ file the firmware loads.
set(CMAKE_CXX_LINK_EXECUTABLE
  "${LD_LLD} -pie --no-dynamic-linker --no-relax -z norelro -z noexecstack -T ${CMAKE_SOURCE_DIR}/scripts/loongarch64-uefi.ld <OBJECTS> -o <TARGET>.elf"
  "${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/scripts/elf2efi.py <TARGET>.elf <TARGET>")

set(ESP_DIR ${CMAKE_BINARY_DIR}/esp)
add_custom_target(esp ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ESP_DIR}/EFI/BOOT
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:BOOTLOONGARCH64> ${ESP_DIR}/EFI/BOOT/BOOTLOONGARCH64.EFI
  DEPENDS BOOTLOONGARCH64
  COMMENT "Assembling ESP directory"
)

find_program(QEMU_LOONGARCH64 qemu-system-loongarch64)
find_file(LOONGARCH_CODE NAMES QEMU_EFI.fd PATHS /usr/share/edk2/loongarch64 /usr/share/qemu-efi-loongarch64 /usr/share/edk2-loongarch64)
find_file(LOONGARCH_VARS NAMES QEMU_VARS.fd PATHS /usr/share/edk2/loongarch64 /usr/share/qemu-efi-loongarch64 /usr/share/edk2-loongarch64)
if(QEMU_LOONGARCH64 AND LOONGARCH_CODE AND LOONGARCH_VARS)
  # The variable store is writable, so QEMU gets a private copy.
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/LOONGARCH_VARS.fd
    COMMAND ${CMAKE_COMMAND} -E copy ${LOONGARCH_VARS} ${CMAKE_BINARY_DIR}/LOONGARCH_VARS.fd)
  add_custom_target(firmware-image DEPENDS ${CMAKE_BINARY_DIR}/LOONGARCH_VARS.fd)
  # `cmake --build build-loongarch64 --target run`: serial goes to this terminal, the screen to a ramfb GPU window.
  add_custom_target(run
    COMMAND ${QEMU_LOONGARCH64} -machine virt -cpu la464 -smp ${TCOS_SMP} -m 2G
      -drive if=pflash,format=raw,unit=0,readonly=on,file=${LOONGARCH_CODE}
      -drive if=pflash,format=raw,unit=1,file=${CMAKE_BINARY_DIR}/LOONGARCH_VARS.fd
      -drive if=none,id=esp,format=raw,file=fat:rw:${ESP_DIR} -device virtio-blk-pci,drive=esp
      -device ramfb -device qemu-xhci -device usb-kbd -device usb-tablet
      -netdev user,id=net0 -device virtio-net-pci,netdev=net0
      -serial stdio
    DEPENDS esp firmware-image USES_TERMINAL)
else()
  message(WARNING "qemu-system-loongarch64 or the LoongArch edk2 firmware (QEMU_EFI.fd, QEMU_VARS.fd) not found; run target disabled")
endif()
