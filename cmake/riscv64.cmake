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

# The RISC-V 64 UEFI image (BOOTRISCV64.EFI) and its QEMU "virt" targets. Included by the top-level
# CMakeLists.txt when TCOS_ARCH=riscv64. Like the AArch64 image it holds the portable layers plus trap handling,
# the SBI timer tick, the thread scheduler and the other harts (SBI HSM); see src/Arch/RiscV64/main.cpp.
add_executable(BOOTRISCV64
  src/Arch/RiscV64/main.cpp
  src/Arch/RiscV64/Core.cpp
  src/Arch/RiscV64/Sbi.cpp
  src/Arch/RiscV64/Timer.cpp
  src/Arch/RiscV64/Interrupts.cpp
  src/Arch/RiscV64/Trap.S
  src/Arch/RiscV64/Secondary.S
  src/Arch/RiscV64/Cpu.cpp
  src/Kernel/ThreadScheduler.cpp
  src/Kernel/Sync.cpp
  src/Kernel/Firmware.cpp
  src/Kernel/Delay.cpp
  src/Utility/Bounds.cpp
  src/console.cpp
  src/BootLog.cpp
  src/GraphicsConsole.cpp
  src/runtime.cpp
  src/Allocators/Allocator.cpp
  src/Hardware/TextConsole.cpp
  src/Hardware/RandomAccessMemory/UefiRandomAccessMemory.cpp
  src/Hardware/Acpi/AcpiTables.cpp
  src/Hardware/GraphicsProcessingUnit/FramebufferGpu.cpp
  src/Hardware/GraphicsProcessingUnit/Font8x16.cpp
  src/Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.cpp
)
target_include_directories(BOOTRISCV64 PRIVATE src)
set_target_properties(BOOTRISCV64 PROPERTIES SUFFIX ".efi" LINKER_LANGUAGE CXX)
# rv64imac without F/D: no kernel code touches floating-point state, so a trap frame holds integer registers only.
# -mno-relax keeps the code free of gp-relative addressing (the image is relocated at load time).
target_compile_options(BOOTRISCV64 PRIVATE
  ${TCOS_RISCV64_INCLUDES}
  -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany -mno-relax -fPIE
  -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-threadsafe-statics
  -fshort-wchar -fno-omit-frame-pointer
  -Wall -Wextra -g -O1
)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# ld.lld produces the ELF, scripts/elf2efi.py rewrites it as the PE32+ file the firmware loads.
set(CMAKE_CXX_LINK_EXECUTABLE
  "${LD_LLD} -pie --no-dynamic-linker --no-relax -z norelro -z noexecstack -T ${CMAKE_SOURCE_DIR}/scripts/riscv64-uefi.ld <OBJECTS> -o <TARGET>.elf"
  "${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/scripts/elf2efi.py <TARGET>.elf <TARGET>")

set(ESP_DIR ${CMAKE_BINARY_DIR}/esp)
add_custom_target(esp ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ESP_DIR}/EFI/BOOT
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:BOOTRISCV64> ${ESP_DIR}/EFI/BOOT/BOOTRISCV64.EFI
  DEPENDS BOOTRISCV64
  COMMENT "Assembling ESP directory"
)

find_program(QEMU_RISCV64 qemu-system-riscv64)
find_file(RISCV_VIRT_CODE NAMES RISCV_VIRT_CODE.fd PATHS /usr/share/edk2/riscv64 /usr/share/qemu-efi-riscv64 /usr/share/edk2-riscv64)
find_file(RISCV_VIRT_VARS NAMES RISCV_VIRT_VARS.fd PATHS /usr/share/edk2/riscv64 /usr/share/qemu-efi-riscv64 /usr/share/edk2-riscv64)
if(QEMU_RISCV64 AND RISCV_VIRT_CODE AND RISCV_VIRT_VARS)
  # The variable store is writable, so QEMU gets a private copy.
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/RISCV_VIRT_VARS.fd
    COMMAND ${CMAKE_COMMAND} -E copy ${RISCV_VIRT_VARS} ${CMAKE_BINARY_DIR}/RISCV_VIRT_VARS.fd)
  add_custom_target(firmware-image DEPENDS ${CMAKE_BINARY_DIR}/RISCV_VIRT_VARS.fd)
  # `cmake --build build-riscv64 --target run`: serial goes to this terminal, the screen to a ramfb GPU window.
  add_custom_target(run
    COMMAND ${QEMU_RISCV64} -machine virt,pflash0=pflash0,pflash1=pflash1 -cpu rv64 -smp 4 -m 2G
      -blockdev node-name=pflash0,driver=file,read-only=on,filename=${RISCV_VIRT_CODE}
      -blockdev node-name=pflash1,driver=file,filename=${CMAKE_BINARY_DIR}/RISCV_VIRT_VARS.fd
      -drive if=none,id=esp,format=raw,file=fat:rw:${ESP_DIR} -device virtio-blk-pci,drive=esp
      -device ramfb -device qemu-xhci -device usb-kbd -device usb-tablet
      -netdev user,id=net0 -device virtio-net-pci,netdev=net0
      -serial stdio
    DEPENDS esp firmware-image USES_TERMINAL)
else()
  message(WARNING "qemu-system-riscv64 or the RISCV_VIRT edk2 firmware not found; run target disabled")
endif()
