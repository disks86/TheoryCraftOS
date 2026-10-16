cmake_minimum_required(VERSION 3.20)

# Select the UEFI cross toolchain unless the caller supplied one.
if(NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-x86_64-uefi.cmake)
endif()

project(TheoryCraftOS LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optional boot-time pause for debuggers that cannot use hardware breakpoints.
set(TCOS_DEBUG_WAIT_SECS 0 CACHE STRING
    "Seconds the OS pauses at boot (or until g_debugger_attached=1); 0 disables")

add_executable(BOOTX64
  src/main.cpp
  src/console.cpp
  src/Allocators/Allocator.cpp
  src/runtime.cpp
  src/hardware/CentralProcessingUnit/X86_64Cpu.cpp
  src/hardware/CentralProcessingUnit/X86_64Cpu.S
  src/hardware/IoPort.cpp
  src/hardware/TextConsole.cpp
  src/hardware/x86_64/port.S
)

target_include_directories(BOOTX64 PRIVATE src)
set_target_properties(BOOTX64 PROPERTIES SUFFIX ".efi")

target_compile_options(BOOTX64 PRIVATE
  -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector
  -fno-threadsafe-statics -mno-red-zone -fshort-wchar
  -Wall -Wextra -g -O1
)
# Keep frame pointers so gdb backtraces work.
target_compile_options(BOOTX64 PRIVATE -fno-omit-frame-pointer)

if(TCOS_DEBUG_WAIT_SECS GREATER 0)
  target_compile_definitions(BOOTX64 PRIVATE TCOS_DEBUG_WAIT=${TCOS_DEBUG_WAIT_SECS})
endif()

target_link_options(BOOTX64 PRIVATE
  -nostdlib -static
  -Wl,--subsystem,10          # EFI application
  -Wl,-e,efi_main
  -Wl,--file-alignment,0x200
  -Wl,--section-alignment,0x1000
  -Wl,--image-base,0x400000
  -Wl,--dynamicbase           # emit .reloc; firmware relocates the image
)
target_link_libraries(BOOTX64 PRIVATE gcc)

# ---------------------------------------------------------------------------
# ESP directory + QEMU targets
# ---------------------------------------------------------------------------
set(ESP_DIR ${CMAKE_BINARY_DIR}/esp)
add_custom_target(esp ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ESP_DIR}/EFI/BOOT
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:BOOTX64> ${ESP_DIR}/EFI/BOOT/BOOTX64.EFI
  DEPENDS BOOTX64
  COMMENT "Assembling ESP directory"
)

find_program(QEMU qemu-system-x86_64)
find_file(OVMF_CODE NAMES OVMF_CODE.4m.fd OVMF_CODE_4M.fd OVMF_CODE.fd
  PATHS /usr/share/edk2/x64 /usr/share/OVMF /usr/share/edk2-ovmf/x64 /usr/share/edk2/ovmf
        /usr/share/ovmf/x64 /usr/share/qemu)
find_file(OVMF_VARS NAMES OVMF_VARS.4m.fd OVMF_VARS_4M.fd OVMF_VARS.fd
  PATHS /usr/share/edk2/x64 /usr/share/OVMF /usr/share/edk2-ovmf/x64 /usr/share/edk2/ovmf
        /usr/share/ovmf/x64 /usr/share/qemu)

if(QEMU AND OVMF_CODE AND OVMF_VARS)
  set(QEMU_COMMON
    ${QEMU} -machine q35 -m 256M -net none
    -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
    -drive if=pflash,format=raw,file=${CMAKE_BINARY_DIR}/OVMF_VARS.fd
    -drive format=raw,file=fat:rw:${ESP_DIR}
    # OS debug output (port 0x402) goes to this terminal and debugcon.log.
    -chardev stdio,id=dbg,logfile=${CMAKE_BINARY_DIR}/debugcon.log,signal=off
    -device isa-debugcon,iobase=0x402,chardev=dbg
    -serial null
  )
  add_custom_target(vars
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OVMF_VARS} ${CMAKE_BINARY_DIR}/OVMF_VARS.fd
    COMMAND chmod u+w ${CMAKE_BINARY_DIR}/OVMF_VARS.fd)

  # `cmake --build build --target run`
  add_custom_target(run COMMAND ${QEMU_COMMON}
    DEPENDS esp vars USES_TERMINAL)
  # Starts the gdbstub on :1234 (-s) without halting; use with TCOS_DEBUG_WAIT.
  add_custom_target(debug COMMAND ${QEMU_COMMON} -s
    DEPENDS esp vars USES_TERMINAL)
  # Freeze the CPU at reset until gdb connects (-S).
  add_custom_target(debug-halted COMMAND ${QEMU_COMMON} -s -S
    DEPENDS esp vars USES_TERMINAL)
else()
  message(WARNING "qemu-system-x86_64 or OVMF firmware not found; run/debug targets disabled")
endif()
