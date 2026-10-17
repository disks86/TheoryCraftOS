# TheoryCraftOS - a minimal UEFI x86_64 operating system.
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
  src/Kernel/Delay.cpp
  src/Utility/Bounds.cpp
  src/Kernel/Interrupts.cpp
  src/Kernel/Interrupts.S
  src/Kernel/Apic.cpp
  src/Kernel/Paging.cpp
  src/Hardware/MmioMapper.cpp
  src/Hardware/Bus/Pci/PciBar.cpp
  src/Kernel/ThreadScheduler.cpp
  src/Kernel/Sync.cpp
  src/Kernel/Clipboard.cpp
  src/Kernel/Firmware.cpp
  src/KernelDemo.cpp
  src/ThreadSafetyDemo.cpp
  src/main.cpp
  src/console.cpp
  src/GraphicsConsole.cpp
  src/Allocators/Allocator.cpp
  src/CPUScheduler/Scheduler.cpp
  src/runtime.cpp
  src/Hardware/CentralProcessingUnit/X86_64Cpu.cpp
  src/Hardware/CentralProcessingUnit/Intel/IntelX64Cpu.cpp
  src/Hardware/CentralProcessingUnit/Intel/IntelGenerations.cpp
  src/Hardware/CentralProcessingUnit/AMD/AmdGenerations.cpp
  src/Hardware/CentralProcessingUnit/Hygon/HygonGenerations.cpp
  src/Hardware/CentralProcessingUnit/Zhaoxin/ZhaoxinGenerations.cpp
  src/Hardware/CentralProcessingUnit/SystemCpu.cpp
  src/Hardware/CentralProcessingUnit/X86_64Cpu.S
  src/Hardware/IoPort.cpp
  src/Hardware/RandomAccessMemory/UefiRandomAccessMemory.cpp
  src/Hardware/TextConsole.cpp
  src/Hardware/Acpi/AcpiTables.cpp
  src/Hardware/Keyboard/KeyCodes.cpp
  src/Hardware/Keyboard/KeyboardBase.cpp
  src/Hardware/Keyboard/CompositeKeyboard.cpp
  src/Hardware/Keyboard/Ps2/Ps2Keyboard.cpp
  src/Hardware/Mouse/MouseBase.cpp
  src/Hardware/Mouse/CompositeMouse.cpp
  src/Hardware/Mouse/Ps2/Ps2Mouse.cpp
  src/Hardware/Bus/Ps2/I8042Controller.cpp
  src/Hardware/Bus/Usb/Hid/HidReportParser.cpp
  src/Hardware/Bus/Usb/Hid/UsbHidInterface.cpp
  src/Hardware/Bus/Virtio/Virtqueue.cpp
  src/Hardware/Bus/Virtio/VirtioPciDevice.cpp
  src/Hardware/Bus/Virtio/VirtioInputDevice.cpp
  src/Hardware/Keyboard/Usb/UsbHidKeyboard.cpp
  src/Hardware/Keyboard/Virtio/VirtioKeyboard.cpp
  src/Hardware/Mouse/Usb/UsbHidMouse.cpp
  src/Hardware/Mouse/Virtio/VirtioMouse.cpp
  src/Hardware/SystemInput.cpp
  src/Hardware/ImageSignalProcessor/IspMath.cpp
  src/Hardware/ImageSignalProcessor/Software/SoftwareImageSignalProcessor.cpp
  src/Hardware/ImageSignalProcessor/SystemImageSignalProcessor.cpp
  src/IspDemo.cpp
  src/NpuDemo.cpp
  src/ContainerDemo.cpp
  src/UtilityDemo.cpp
  src/NetworkDemo.cpp
  src/NetworkStackDemo.cpp
  src/Network/Checksum.cpp
  src/Network/NetworkStack.cpp
  src/Network/Snmp/SnmpMessage.cpp
  src/Network/Snmp/SnmpUsm.cpp
  src/Network/Snmp/SnmpTrap.cpp
  src/Network/Ipp/IppMessage.cpp
  src/Network/Ipp/IppExchange.cpp
  src/Network/Mdns/MdnsMessage.cpp
  src/Network/Snmp/SnmpClient.cpp
  src/Crypto/Crypto.cpp
  src/Crypto/Aead.cpp
  src/Crypto/X25519.cpp
  src/Crypto/Random.cpp
  src/Crypto/BigInt.cpp
  src/Crypto/Rsa.cpp
  src/Crypto/Ecc.cpp
  src/Crypto/X509.cpp
  src/Network/Tls/TlsKeys.cpp
  src/Network/Tls/TlsSocket.cpp
  src/Network/NetworkStackSet.cpp
  src/Network/Udp/UdpSocket.cpp
  src/Network/Udp/UdpManager.cpp
  src/Network/Tcp/TcpSocket.cpp
  src/Network/Tcp/TcpManager.cpp
  src/Network/Dhcp/DhcpClient.cpp
  src/Network/Dns/DnsMessage.cpp
  src/Network/Dns/DnsResolver.cpp
  src/Hardware/Clock/TscClock.cpp
  src/AudioDemo.cpp
  src/Audio/Resampler.cpp
  src/Audio/AudioStream.cpp
  src/Audio/AudioMixer.cpp
  src/Audio/AudioRouter.cpp
  src/Audio/WavFile.cpp
  src/Audio/AudioPlayer.cpp
  src/Audio/CaptureStream.cpp
  src/Audio/AudioCaptureHub.cpp
  src/Hardware/AudioDevice/PlaybackRing.cpp
  src/Hardware/AudioDevice/CaptureRing.cpp
  src/Hardware/AudioDevice/SystemAudioDevices.cpp
  src/Hardware/AudioDevice/Software/SoftwareAudioDevice.cpp
  src/Hardware/AudioDevice/Software/SoftwareAudioCapture.cpp
  src/Hardware/AudioDevice/Intel/IntelHdaAudioDevice.cpp
  src/Hardware/AudioDevice/Intel/Ac97AudioDevice.cpp
  src/Hardware/AudioDevice/Virtio/VirtioSoundDevice.cpp
  src/Hardware/AudioDevice/Usb/UsbAudioDescriptors.cpp
  src/Hardware/AudioDevice/Usb/UsbAudioDevice.cpp
  src/Hardware/Camera/SystemCameras.cpp
  src/Hardware/Camera/Usb/UvcDescriptors.cpp
  src/Hardware/Camera/Usb/UvcMjpeg.cpp
  src/Hardware/Camera/Usb/UvcCamera.cpp
  src/Hardware/NetworkInterface/SystemNetworkInterfaces.cpp
  src/Hardware/NetworkInterface/Virtio/VirtioNetworkInterface.cpp
  src/Hardware/NetworkInterface/Intel/E1000NetworkInterface.cpp
  src/Hardware/NetworkInterface/Realtek/Rtl8139NetworkInterface.cpp
  src/Hardware/NetworkInterface/UEFI/UefiSimpleNetworkInterface.cpp
  src/StorageDemo.cpp
  src/Hardware/StorageDevice/StorageDeviceBase.cpp
  src/Hardware/StorageDevice/SystemStorageDevices.cpp
  src/Hardware/StorageDevice/Virtio/VirtioBlockDevice.cpp
  src/Hardware/StorageDevice/Virtio/VirtioScsiDevice.cpp
  src/Hardware/StorageDevice/NVMe/NvmeStorageDevice.cpp
  src/Hardware/StorageDevice/Ahci/AhciStorageDevice.cpp
  src/Hardware/StorageDevice/Ide/IdeStorageDevice.cpp
  src/Hardware/StorageDevice/UEFI/UefiBlockStorageDevice.cpp
  src/Hardware/StorageDevice/Usb/UsbMassStorageDevice.cpp
  src/FileSystemDemo.cpp
  src/SystemFileSystem.cpp
  src/Hardware/StorageDevice/Partition/PartitionScanner.cpp
  src/AsyncDemo.cpp
  src/Async/Executor.cpp
  src/Stream/StreamStatus.cpp
  src/Stream/Memory/MemoryStream.cpp
  src/Stream/File/FileStream.cpp
  src/Stream/Tcp/TcpStream.cpp
  src/FileSystem/FileSystemTypes.cpp
  src/FileSystem/Ram/RamFileSystem.cpp
  src/FileSystem/Mount/MountTable.cpp
  src/FileSystem/Fat/FatFileSystem.cpp
  src/ConcurrentDemo.cpp
  src/Hardware/NeuralProcessingUnit/Software/SoftwareNeuralProcessingUnit.cpp
  src/Hardware/NeuralProcessingUnit/SystemNeuralProcessingUnit.cpp
  src/Math/FloatMath.cpp
  src/Hardware/Bus/Pci/LegacyPciBus.cpp
  src/Hardware/Bus/Pcie/EcamPcieBus.cpp
  src/Hardware/Bus/Pci/FirmwareDriverDetach.cpp
  src/Hardware/Bus/Usb/UsbBus.cpp
  src/Hardware/Bus/Usb/Xhci/XhciController.cpp
  src/Hardware/Bus/Usb/Ehci/EhciController.cpp
  src/Hardware/Bus/Usb/Legacy/LegacyHostController.cpp
  src/Hardware/Bus/Usb/Uhci/UhciController.cpp
  src/Hardware/Bus/Usb/Ohci/OhciController.cpp
  src/Hardware/Bus/Usb/Xhci/XhciRing.cpp
  src/Hardware/Bus/SystemPciBus.cpp
  src/Hardware/GraphicsProcessingUnit/Font8x16.cpp
  src/Hardware/GraphicsProcessingUnit/FramebufferGpu.cpp
  src/Hardware/GraphicsProcessingUnit/SystemGpu.cpp
  src/Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.cpp
  src/Hardware/GraphicsProcessingUnit/QEMU/QemuStdVgaGpu.cpp
  src/Hardware/GraphicsProcessingUnit/Virtio/VirtioGpu.cpp
  src/Graphics/Graphics2D/Software/SoftwareRenderer2D.cpp
  src/Graphics/Graphics2D/Device/DeviceRenderer2D.cpp
  src/Graphics/Graphics3D/BuiltinShaders.cpp
  src/Graphics/Graphics3D/Virgl/VirglDevice.cpp
  src/Graphics/SystemGraphics.cpp
  src/BootLog.cpp
  src/Desktop/Desktop.cpp
  src/Desktop/DesktopSurface.cpp
  src/Desktop/FileExplorer.cpp
  src/Desktop/DirectoryListing.cpp
  src/Desktop/FileDialog.cpp
  src/Desktop/TaskManager.cpp
  src/Desktop/Notepad.cpp
  src/Desktop/Paint.cpp
  src/SystemMonitor.cpp
  src/Gui/Controls/MenuButton.cpp
  src/GraphicsDemo.cpp
  src/GuiDemo.cpp
  src/Gui/Painter.cpp
  src/Gui/Control.cpp
  src/Gui/Window.cpp
  src/Gui/RenderLoop.cpp
  src/Gui/InputPump.cpp
  src/Gui/Controls/Panel.cpp
  src/Gui/Controls/Label.cpp
  src/Gui/Controls/Button.cpp
  src/Gui/Controls/TextBox.cpp
  src/Gui/Controls/DropDown.cpp
  src/Gui/Controls/Slider.cpp
  src/Gui/Controls/LevelMeter.cpp
  src/Gui/Controls/CheckBox.cpp
  src/Gui/Controls/RadioButton.cpp
  src/Gui/Controls/Toggle.cpp
  src/Gui/Controls/ColorPicker.cpp
  src/Gui/Controls/DatePicker.cpp
  src/Gui/Controls/ScrollBar.cpp
  src/Gui/Controls/GridModel.cpp
  src/Gui/Controls/DataGrid.cpp
  src/Gui/Controls/ListView.cpp
  src/Gui/Controls/ToolStrip.cpp
  src/Gui/Controls/ColorPalette.cpp
  src/Gui/Controls/DrawingCanvas.cpp
  src/Gui/Controls/TabControl.cpp
  src/Gui/Controls/BreadcrumbBar.cpp
  src/Gui/Controls/TextEditor.cpp
  src/Gui/Controls/ImageView.cpp
  src/Gui/Controls/AudioPlayerControl.cpp
  src/Gui/Controls/VideoPlayerControl.cpp
  src/Media/Image.cpp
  src/Media/ImageCodecs.cpp
  src/Media/Zlib.cpp
  src/Media/Image/Png.cpp
  src/Media/Image/Jpeg.cpp
  src/Media/Image/Bmp.cpp
  src/Media/Image/Gif.cpp
  src/Media/Image/Pnm.cpp
  src/Media/Image/Qoi.cpp
  src/Media/Image/Tga.cpp
  src/Media/Image/Tiff.cpp
  src/Media/Image/Webp.cpp
  src/Media/Image/WebpLossy.cpp
  src/Media/AudioData.cpp
  src/Media/AudioCodecs.cpp
  src/Media/Audio/Pcm.cpp
  src/Media/Audio/Wav.cpp
  src/Media/Audio/Aiff.cpp
  src/Media/Audio/Au.cpp
  src/Media/Audio/Flac.cpp
  src/Media/Audio/Mdct.cpp
  src/Media/Audio/Vorbis.cpp
  src/Media/Audio/Mp3.cpp
  src/Media/Audio/Aac.cpp
  src/Media/Audio/Mp4Audio.cpp
  src/Media/Mp4.cpp
  src/Media/VideoFiles.cpp
  src/Media/MediaCodecs.cpp
  src/Video/AviFile.cpp
  src/Video/AviWriter.cpp
  src/Video/VideoPlayer.cpp
  src/Hardware/VideoCodec/VideoTypes.cpp
  src/Hardware/VideoCodec/VideoFrameBuffer.cpp
  src/Hardware/VideoCodec/ColorConvert.cpp
  src/Hardware/VideoCodec/SystemVideoCodecs.cpp
  src/Hardware/VideoCodec/Software/SoftwareVideoCodec.cpp
  src/Hardware/Printer/PrinterTypes.cpp
  src/Hardware/Printer/Halftone.cpp
  src/Hardware/Printer/PrintJob.cpp
  src/Hardware/Printer/PrintTransport.cpp
  src/Hardware/Printer/NetworkPrinter.cpp
  src/Hardware/Printer/SystemPrinters.cpp
  src/Hardware/Printer/Brother/BrotherHl2070n.cpp
  src/Hardware/Printer/PrinterMonitor.cpp
  src/Hardware/Printer/PrinterAlertListener.cpp
  src/Hardware/Printer/IppPrinter.cpp
  src/Hardware/Printer/PrinterDiscovery.cpp
  src/Hardware/Printer/PrinterFactory.cpp
  src/Hardware/Printer/Usb/Ieee1284.cpp
  src/Hardware/Printer/Usb/Dot4.cpp
  src/Hardware/Printer/Usb/UsbPrinter.cpp
  src/Hardware/VideoCodec/Software/JpegDecoder.cpp
  src/Hardware/VideoCodec/Software/H264Syntax.cpp
  src/Hardware/VideoCodec/Software/H264Macroblock.cpp
  src/Hardware/VideoCodec/Software/H264Cabac.cpp
  src/Hardware/VideoCodec/Software/H264Predict.cpp
  src/Hardware/VideoCodec/Software/H264Deblock.cpp
  src/Hardware/VideoCodec/Software/JpegEncoder.cpp
  src/Gui/Controls/Menu.cpp
  src/Gui/Windows/Frame.cpp
  src/Gui/Windows/WindowManager.cpp
  src/Gui/Windows/WindowSwitcher.cpp
  src/Gui/MessageDialog.cpp
  src/Graphics/Graphics3D/Software/SoftwareDevice.cpp
  src/Hardware/x86_64/port.S
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

option(TCOS_SELFTESTS "Run the boot-time self-checks and demos of every subsystem" OFF)
if(TCOS_SELFTESTS)
  target_compile_definitions(BOOTX64 PRIVATE TCOS_SELFTESTS)
endif()

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
    ${QEMU} -machine q35 -smp 4 -m 8G -netdev user,id=net0 -device virtio-net-pci,netdev=net0
    -rtc base=localtime  # the taskbar clock shows local time
    # An absolute pointer for the desktop (no click-to-grab; relative PS/2 grabbing is unreliable under Wayland).
    -device virtio-tablet-pci
    # An HD Audio controller with a duplex codec; the `none` backend discards the sound (use e.g. `pa` to hear it).
    -audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0
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
  # Same, with a virtio-gpu 3D device: QEMU renders through the host's OpenGL (virgl).
  add_custom_target(run-gl COMMAND ${QEMU_COMMON} -vga none -device virtio-gpu-gl-pci -display gtk,gl=on,show-cursor=off
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
