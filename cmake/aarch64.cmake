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

# The AArch64 UEFI image (BOOTAA64.EFI) and its QEMU "virt" targets. Included by the top-level
# CMakeLists.txt when TCOS_ARCH=aarch64. The image holds the portable layers, the GIC, exception
# vectors, the generic timer tick and the thread scheduler so far; see src/Arch/AArch64/main.cpp.
add_executable(BOOTAA64
  src/Arch/AArch64/main.cpp
  src/Arch/AArch64/Core.cpp
  src/Arch/AArch64/Spcr.cpp
  src/Arch/AArch64/Gic.cpp
  src/Arch/AArch64/Timer.cpp
  src/Arch/AArch64/Interrupts.cpp
  src/Arch/AArch64/Vectors.S
  src/Arch/AArch64/Secondary.S
  src/Arch/AArch64/Psci.cpp
  src/Arch/AArch64/Cpu.cpp
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
  src/Hardware/Platform/RaspberryPi/RaspberryPiBoard.cpp
  src/Hardware/Platform/RaspberryPi/RaspberryPiDetect.cpp
  src/Hardware/Platform/RaspberryPi/RaspberryPiPlatform.cpp
  src/Hardware/Platform/RaspberryPi/VcMailbox.cpp
  src/Hardware/Platform/RaspberryPi/VideoCoreFirmware.cpp
  src/Hardware/GraphicsProcessingUnit/Broadcom/V3dGpu.cpp
  src/Hardware/GraphicsProcessingUnit/Broadcom/V3dPageTable.cpp
  src/Hardware/GraphicsProcessingUnit/Broadcom/V3dControlList.cpp
  src/Hardware/GraphicsProcessingUnit/Broadcom/V3dFrame.cpp
  src/Hardware/GraphicsProcessingUnit/Broadcom/V3dDraw.cpp
  src/Hardware/GraphicsProcessingUnit/Broadcom/V3dRenderer.cpp
  src/Hardware/GraphicsProcessingUnit/Broadcom/V3dQpu.cpp
  src/Hardware/GraphicsProcessingUnit/Broadcom/V3dShaders.cpp
  src/Hardware/GraphicsProcessingUnit/Broadcom/V3dGpuMemory.cpp
  src/Hardware/GraphicsProcessingUnit/Broadcom/V3dSelfTest.cpp
  src/Hardware/VideoCodec/Broadcom/RpiHevcBlock.cpp
)
target_include_directories(BOOTAA64 PRIVATE src)
set_target_properties(BOOTAA64 PROPERTIES SUFFIX ".efi" LINKER_LANGUAGE CXX)
target_compile_options(BOOTAA64 PRIVATE
  ${TCOS_AARCH64_INCLUDES}
  -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-threadsafe-statics
  -fshort-wchar -mgeneral-regs-only -mno-outline-atomics -fno-omit-frame-pointer
  -Wall -Wextra -g -O1
)
# clang does not drive lld for PE here: link with lld-link directly.
set(CMAKE_CXX_LINK_EXECUTABLE "${LLD_LINK} /machine:arm64 /subsystem:efi_application /entry:efi_main /nodefaultlib /filealign:512 /base:0x400000 /dynamicbase <OBJECTS> /out:<TARGET>")

set(ESP_DIR ${CMAKE_BINARY_DIR}/esp)
add_custom_target(esp ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ESP_DIR}/EFI/BOOT
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:BOOTAA64> ${ESP_DIR}/EFI/BOOT/BOOTAA64.EFI
  DEPENDS BOOTAA64
  COMMENT "Assembling ESP directory"
)

find_program(QEMU_AARCH64 qemu-system-aarch64)
find_file(AAVMF_CODE NAMES AAVMF_CODE.fd QEMU_EFI.fd PATHS /usr/share/AAVMF /usr/share/qemu-efi-aarch64 /usr/share/edk2/aarch64 /usr/share/edk2/aarch64)
if(QEMU_AARCH64 AND AAVMF_CODE)
  # Pads the firmware to the 64 MiB a pflash bank must be.
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/AAVMF_CODE.img
    COMMAND truncate -s 64M ${CMAKE_BINARY_DIR}/AAVMF_CODE.img
    COMMAND dd if=${AAVMF_CODE} of=${CMAKE_BINARY_DIR}/AAVMF_CODE.img conv=notrunc status=none)
  add_custom_target(firmware-image DEPENDS ${CMAKE_BINARY_DIR}/AAVMF_CODE.img)
  # `cmake --build build-aarch64 --target run`: serial goes to this terminal, the screen to a ramfb/virtio GPU window.
  add_custom_target(run
    COMMAND ${QEMU_AARCH64} -machine virt -cpu max -smp 4 -m 2G
      -drive if=pflash,format=raw,readonly=on,file=${CMAKE_BINARY_DIR}/AAVMF_CODE.img
      -drive format=raw,file=fat:rw:${ESP_DIR}
      -device ramfb -device qemu-xhci -device usb-kbd -device usb-tablet
      -netdev user,id=net0 -device virtio-net-pci,netdev=net0
      -serial stdio
    DEPENDS esp firmware-image USES_TERMINAL)
else()
  message(WARNING "qemu-system-aarch64 or AAVMF firmware not found; run target disabled")
endif()
