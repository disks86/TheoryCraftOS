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
# -DTCOS_ARCH=aarch64 picks the AArch64 toolchain (and the AArch64 image, see cmake/aarch64.cmake), -DTCOS_ARCH=riscv64
# the RISC-V one (cmake/riscv64.cmake).
set(TCOS_ARCH "x86_64" CACHE STRING "Target architecture: x86_64, aarch64 or riscv64")
if(NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-${TCOS_ARCH}-uefi.cmake)
endif()

project(TheoryCraftOS LANGUAGES C CXX ASM)
set(TCOS_SMP 4 CACHE STRING "Cores of the QEMU machine of the run targets")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(TCOS_ARCH STREQUAL "aarch64")
  include(cmake/aarch64.cmake)
  return()
endif()
if(TCOS_ARCH STREQUAL "riscv64")
  include(cmake/riscv64.cmake)
  return()
endif()

# Optional boot-time pause for debuggers that cannot use hardware breakpoints.
set(TCOS_DEBUG_WAIT_SECS 0 CACHE STRING
    "Seconds the OS pauses at boot (or until g_debugger_attached=1); 0 disables")

add_executable(BOOTX64
  src/Kernel/Delay.cpp
  src/Utility/Bounds.cpp
  src/Kernel/Interrupts.cpp
  src/Kernel/Interrupts.S
  src/Kernel/UserEntry.S
  src/Kernel/UserMode.cpp
  src/UserModeDemo.cpp
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
  src/Hardware/CentralProcessingUnit/CpuTemperature.cpp
  src/Hardware/CentralProcessingUnit/X86_64Cpu.S
  src/Hardware/IoPort.cpp
  src/Hardware/RandomAccessMemory/UefiRandomAccessMemory.cpp
  src/Hardware/TextConsole.cpp
  src/Hardware/Acpi/AcpiTables.cpp
  src/Hardware/Acpi/AmlInterpreter.cpp
  src/Hardware/Acpi/EmbeddedController.cpp
  src/Hardware/Acpi/SystemAcpiHost.cpp
  src/Hardware/Acpi/AcpiEvents.cpp
  src/Hardware/Acpi/IoApic.cpp
  src/Hardware/Acpi/SystemAcpiPorts.cpp
  src/Hardware/Battery/AcpiBattery.cpp
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
  src/Hardware/VirtualBox/VmmDev.cpp
  src/Hardware/HyperV/HyperV.cpp
  src/Hardware/HyperV/VmBus.cpp
  src/Hardware/HyperV/HyperVIntegration.cpp
  src/Hardware/HyperV/SystemHyperV.cpp
  src/Hardware/Mouse/VirtualBox/VBoxMouse.cpp
  src/Hardware/Mouse/VMware/VmwareMouse.cpp
  src/Hardware/VMware/Backdoor.cpp
  src/Hardware/VMware/VmwareTools.cpp
  src/Hardware/Guest/PvPanic.cpp
  src/Hardware/Guest/VirtioRng.cpp
  src/Hardware/Guest/VirtioBalloon.cpp
  src/Hardware/Guest/SystemGuestServices.cpp
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
  src/Network/Http/HttpUrl.cpp
  src/Network/Http/HttpClient.cpp
  src/Network/Http/WebSocket.cpp
  src/Network/Http/AltSvc.cpp
  src/Network/Smb/Ntlm.cpp
  src/Network/Smb/SmbConnection.cpp
  src/Network/ServerNetwork.cpp
  src/Network/Smb/SmbFileSystem.cpp
  src/Network/Nfs/RpcClient.cpp
  src/Network/Nfs/NfsFileSystem.cpp
  src/Network/Ssh/SshSession.cpp
  src/Network/Sftp/SftpFileSystem.cpp
  src/Network/Ftp/FtpFileSystem.cpp
  src/Desktop/CredentialsDialog.cpp
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
  src/Network/Tls/TlsCertificate.cpp
  src/Network/Tls/TlsSession.cpp
  src/Network/Tls/Tls13Client.cpp
  src/Network/Quic/QuicCommon.cpp
  src/Network/Quic/QuicCrypto.cpp
  src/Network/Quic/QuicStreams.cpp
  src/Network/Quic/QuicConnection.cpp
  src/Network/Quic/QuicRecovery.cpp
  src/Network/Quic/QuicSend.cpp
  src/Network/Quic/QuicStreamApi.cpp
  src/Network/Quic/Qpack.cpp
  src/Network/Quic/Http3.cpp
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
  src/Hardware/NetworkInterface/AMD/PcnetNetworkInterface.cpp
  src/Hardware/NetworkInterface/VMware/Vmxnet3NetworkInterface.cpp
  src/Hardware/NetworkInterface/Intel/E1000NetworkInterface.cpp
  src/Hardware/NetworkInterface/Realtek/Rtl8139NetworkInterface.cpp
  src/Hardware/NetworkInterface/UEFI/UefiSimpleNetworkInterface.cpp
  src/Hardware/WirelessInterface/WirelessTypes.cpp
  src/Hardware/WirelessInterface/Software/SoftwareWirelessInterface.cpp
  src/StorageDemo.cpp
  src/Hardware/StorageDevice/StorageDeviceBase.cpp
  src/Hardware/StorageDevice/SystemStorageDevices.cpp
  src/Hardware/StorageDevice/Virtio/VirtioBlockDevice.cpp
  src/Hardware/StorageDevice/Virtio/VirtioScsiDevice.cpp
  src/Hardware/StorageDevice/VMware/PvscsiDevice.cpp
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
  src/FileSystem/Befs/BefsFileSystem.cpp
  src/FileSystem/Ntfs/NtfsFileSystem.cpp
  src/FileSystem/Ntfs/NtfsMount.cpp
  src/FileSystem/Ntfs/NtfsWrite.cpp
  src/FileSystem/Ext/ExtFileSystem.cpp
  src/FileSystem/Ext/ExtWrite.cpp
  src/FileSystem/Ext/ExtJournal.cpp
  src/FileSystem/Ext/ExtExtent.cpp
  src/ConcurrentDemo.cpp
  src/Hardware/NeuralProcessingUnit/Software/SoftwareNeuralProcessingUnit.cpp
  src/Hardware/NeuralProcessingUnit/SystemNeuralProcessingUnit.cpp
  src/Math/FloatMath.cpp
  src/Spreadsheet/Sheet.cpp
  src/Database/DatabaseTypes.cpp
  src/Database/NumberText.cpp
  src/Database/TextResultSet.cpp
  src/Database/ConnectionOptions.cpp
  src/Database/DatabaseManager.cpp
  src/Browser/HtmlDocument.cpp
  src/Browser/Css.cpp
  src/Browser/Media.cpp
  src/Browser/Renderer.cpp
  src/Browser/RendererBoxes.cpp
  src/Browser/Page.cpp
  src/Browser/DomBindings.cpp
  src/Browser/DomBindingsNode.cpp
  src/Browser/DomBindingsEvents.cpp
  src/Browser/DomBindingsStyle.cpp
  src/Browser/DomBindingsWindow.cpp
  src/Browser/DomBindingsMedia.cpp
  src/Browser/DomBindingsMse.cpp
  src/Browser/DomBindingsTracks.cpp
  src/Browser/DomBindingsDash.cpp
  src/Browser/DomBindingsForm.cpp
  src/Browser/DomBindingsNet.cpp
  src/Browser/DomBindingsCrypto.cpp
  src/Browser/DomBindingsWasm.cpp
  src/Browser/DomBindingsGl.cpp
  src/Browser/DomBindingsGpu.cpp
  src/Browser/WebGpuPrelude.cpp
  src/Browser/WebGlPrelude.cpp
  src/WebGl/ShaderMachine.cpp
  src/WebGl/ShaderBuiltins.cpp
  src/WebGl/ShaderLayout.cpp
  src/WebGl/Glsl.cpp
  src/WebGl/Wgsl.cpp
  src/WebGl/GpuDevice.cpp
  src/WebGl/GpuRender.cpp
  src/WebGl/Raster.cpp
  src/WebGl/Accelerator.cpp
  src/WebGl/TgsiTranslator.cpp
  src/WebGl/GlAccel.cpp
  src/WebGl/GpuAccel.cpp
  src/WebGl/GlContext.cpp
  src/WebGl/GlProgram.cpp
  src/WebGl/GlTexture.cpp
  src/WebGl/GlCompressed.cpp
  src/WebGl/GlSampler.cpp
  src/WebGl/GlDraw.cpp
  src/Browser/DomBindingsServiceWorker.cpp
  src/Browser/DomBindingsRtc.cpp
  src/Browser/CacheStore.cpp
  src/Browser/ServiceWorker.cpp
  src/Browser/WebPush.cpp
  src/Rtc/Dtls.cpp
  src/Rtc/Ice.cpp
  src/Rtc/PeerConnection.cpp
  src/Rtc/Rtp.cpp
  src/Rtc/RtcService.cpp
  src/Rtc/Sctp.cpp
  src/Rtc/Sdp.cpp
  src/Rtc/Srtp.cpp
  src/Browser/StackRtcNetwork.cpp
  src/Rtc/Stun.cpp
  src/Browser/ServiceWorkerPrelude.cpp
  src/Wasm/WasmDecode.cpp
  src/Wasm/WasmRun.cpp
  src/Wasm/WasmSimd.cpp
  src/Browser/WebPrelude.cpp
  src/Browser/WebUrl.cpp
  src/Browser/CookieJar.cpp
  src/Browser/PublicSuffix.cpp
  src/Browser/Fetcher.cpp
  src/Browser/PageHub.cpp
  src/Browser/ResourcePrefetcher.cpp
  src/Browser/SessionHistory.cpp
  src/Browser/Canvas.cpp
  src/Browser/Objects.cpp
  src/Browser/MseClip.cpp
  src/Browser/Svg.cpp
  src/Dom/Dom.cpp
  src/Dom/Selector.cpp
  src/Dom/Form.cpp
  src/Dom/Entities.cpp
  src/Dom/LegacyEncodings.cpp
  src/Dom/HtmlBuilder.cpp
  src/Regex/Regex.cpp
  src/Script/Number.cpp
  src/Script/Parser.cpp
  src/Script/Compiler.cpp
  src/Script/Vm.cpp
  src/Script/Builtins.cpp
  src/Script/RegExp.cpp
  src/Script/TypedArray.cpp
  src/Script/Stdlib.cpp
  src/Script/Engine.cpp
  src/Browser/PageLayout.cpp
  src/Layout/BoxBuilder.cpp
  src/Layout/Layout.cpp
  src/Layout/Inline.cpp
  src/Layout/Intrinsic.cpp
  src/Layout/Positioned.cpp
  src/Layout/FontMeasurer.cpp
  src/Layout/Flex.cpp
  src/Layout/Grid.cpp
  src/Layout/Table.cpp
  src/Layout/TableFixup.cpp
  src/Layout/Multicol.cpp
  src/Paint/PaintBuilder.cpp
  src/Paint/DisplayPainter.cpp
  src/Paint/HitTest.cpp
  src/Paint/Raster.cpp
  src/Paint/PainterEffects.cpp
  src/Browser/Url.cpp
  src/Browser/HttpPageLoader.cpp
  src/Network/Tls/CaBundle.cpp
  src/Network/Tls/CaBundle.S
  src/Graphics/Logo/Logo.cpp
  src/Graphics/Logo/LogoData.cpp
  src/Graphics/Logo/Logo.S
  src/Database/SqlText.cpp
  src/Database/Local/LocalCatalog.cpp
  src/Database/Local/LocalSql.cpp
  src/Database/Local/LocalDatabase.cpp
  src/Database/Local/SystemSettings.cpp
  src/Database/Net/NetworkTransport.cpp
  src/Database/Postgres/PostgresAuth.cpp
  src/Database/Postgres/PostgresConnection.cpp
  src/Database/Postgres/PostgresDriver.cpp
  src/Math/Vector.hpp
  src/Math/Matrix.hpp
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
  src/Font/TrueType.cpp
  src/Font/WoffDecode.cpp
  src/Font/GlyphRaster.cpp
  src/Font/GlyphCache.cpp
  src/Font/FontSet.cpp
  src/Font/Shaper.cpp
  src/Font/FontDataSans.cpp
  src/Font/FontDataSerif.cpp
  src/Font/FontDataMono.cpp
  src/Font/FontDataFallback.cpp
  src/Hardware/GraphicsProcessingUnit/FramebufferGpu.cpp
  src/Hardware/GraphicsProcessingUnit/SystemGpu.cpp
  src/Hardware/GraphicsProcessingUnit/Multi/GpuTopology.cpp
  src/Hardware/GraphicsProcessingUnit/Multi/GpuPowerPolicy.cpp
  src/Hardware/GraphicsProcessingUnit/Multi/GpuManager.cpp
  src/Hardware/GraphicsProcessingUnit/Multi/GpuPool.cpp
  src/Hardware/GraphicsProcessingUnit/Multi/GpuNativeDrivers.cpp
  src/Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdGpuGenerations.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdGpu.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdDcn1.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdScanout.cpp
  src/Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaGpuGenerations.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelGpu.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelDisplay.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelScanout.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelGtt.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelLrc.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelExeclist.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelForcewake.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelRenderCommands.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelGen9Setup.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelRenderBatch.cpp
  src/Graphics/Graphics3D/IntelGen9/Gen9GraphicsDevice.cpp
  src/Graphics/Graphics3D/IntelGen9/IntelRenderStack.cpp
  src/Graphics/Graphics3D/Vega/VegaGraphicsDevice.cpp
  src/Hardware/GraphicsProcessingUnit/QEMU/QemuStdVgaGpu.cpp
  src/Hardware/GraphicsProcessingUnit/Virtio/VirtioGpu.cpp
  src/Hardware/GraphicsProcessingUnit/VMware/VmwareSvgaGpu.cpp
  src/Graphics/Graphics2D/Software/SoftwareRenderer2D.cpp
  src/Graphics/Graphics2D/Device/DeviceRenderer2D.cpp
  src/Graphics/Graphics3D/BuiltinShaders.cpp
  src/Graphics/Graphics3D/WebGlAccelerator.cpp
  src/Graphics/Graphics3D/Virgl/VirglDevice.cpp
  src/Graphics/Graphics3D/Svga/SvgaDevice.cpp
  src/Graphics/RenderNode.cpp
  src/Graphics/RenderNodeAccel.cpp
  src/Graphics/Renderer2DSlot.cpp
  src/Graphics/GraphicsPool.cpp
  src/Graphics/SystemDxg.cpp
  src/Kernel/Dxgkrnl/VidMm.cpp
  src/Kernel/Dxgkrnl/D3d12.cpp
  src/Kernel/Dxgkrnl/DxilBitcode.cpp
  src/Kernel/Dxgkrnl/DxilTranslate.cpp
  src/Kernel/Dxgkrnl/D3d9Shader.cpp
  src/Kernel/Dxgkrnl/D3d9Tgsi.cpp
  src/Kernel/Dxgkrnl/D3d9Dxbc.cpp
  src/Kernel/Dxgkrnl/D3d9Ps1.cpp
  src/Kernel/Dxgkrnl/SelfTest.cpp
  src/Kernel/Dxgkrnl/Conformance.cpp
  src/Kernel/Dxgkrnl/D3d9Devices.cpp
  src/Kernel/Dxgkrnl/D3d9Emit.cpp
  src/Kernel/Dxgkrnl/Adapter.cpp
  src/Kernel/Dxgkrnl/DxgKernel.cpp
  src/Graphics/SystemGraphics.cpp
  src/BootLog.cpp
  src/Desktop/Desktop.cpp
  src/Desktop/DesktopSurface.cpp
  src/Desktop/FileExplorer.cpp
  src/Desktop/DisplaySettings.cpp
  src/Desktop/MonitorMap.cpp
  src/Desktop/FileTransfer.cpp
  src/Desktop/Shell.cpp
  src/Desktop/Terminal.cpp
  src/Desktop/Calculator.cpp
  src/Desktop/Calendar.cpp
  src/Desktop/CalendarStore.cpp
  src/Desktop/Todo.cpp
  src/Desktop/TodoStore.cpp
  src/Desktop/FileProperties.cpp
  src/Desktop/CopyProgressDialog.cpp
  src/Desktop/DirectoryListing.cpp
  src/Desktop/FileDialog.cpp
  src/Desktop/ColorDialog.cpp
  src/Desktop/PrintDialog.cpp
  src/Desktop/PrintPreview.cpp
  src/SystemPrintService.cpp
  src/Gui/Controls/SpinBox.cpp
  src/Gui/Controls/ColorSpectrum.cpp
  src/Desktop/ComputerManager.cpp
  src/Desktop/HardwareInventory.cpp
  src/Desktop/Notepad.cpp
  src/Desktop/Spreadsheet.cpp
  src/Desktop/DatabaseQuery.cpp
  src/Desktop/WebBrowser.cpp
  src/Desktop/WebNotifications.cpp
  src/Desktop/WebAudio.cpp
  src/Desktop/PictureInPicture.cpp
  src/Desktop/BrowserStore.cpp
  src/Desktop/SessionStore.cpp
  src/Desktop/DesktopSession.cpp
  src/Desktop/Paint.cpp
  src/Desktop/MediaPlayer.cpp
  src/Desktop/Graphics3DTest.cpp
  src/Desktop/CubeRenderer.cpp
  src/SystemMonitor.cpp
  src/SystemInventory.cpp
  src/SystemDeviceDetails.cpp
  src/SystemNetworkDetails.cpp
  src/SystemWirelessDetails.cpp
  src/Desktop/NetworkProperties.cpp
  src/Desktop/DeviceProperties.cpp
  src/Desktop/WirelessProperties.cpp
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
  src/Gui/Controls/ProgressBar.cpp
  src/Gui/Controls/TerminalView.cpp
  src/Gui/Controls/TreeView.cpp
  src/Gui/Controls/Button.cpp
  src/Gui/Controls/TextBox.cpp
  src/Gui/Controls/DropDown.cpp
  src/Gui/Controls/Slider.cpp
  src/Gui/Controls/Knob.cpp
  src/Gui/Controls/LevelMeter.cpp
  src/Gui/Controls/PieChart.cpp
  src/Gui/Controls/BarChart.cpp
  src/Gui/Controls/HeatMap.cpp
  src/Gui/Controls/WordCloud.cpp
  src/Gui/Controls/Tooltip.cpp
  src/Gui/Controls/ToastCenter.cpp
  src/Gui/Controls/CheckBox.cpp
  src/Gui/Controls/RadioButton.cpp
  src/Gui/Controls/Toggle.cpp
  src/Gui/Controls/ColorPicker.cpp
  src/Gui/Controls/DatePicker.cpp
  src/Gui/Controls/ScrollBar.cpp
  src/Gui/Controls/ScrollPanel.cpp
  src/Gui/Controls/GridModel.cpp
  src/Gui/Controls/DataGrid.cpp
  src/Gui/Controls/ListView.cpp
  src/Gui/Controls/ContextMenu.cpp
  src/Gui/Controls/ToolStrip.cpp
  src/Gui/Controls/ColorPalette.cpp
  src/Gui/Controls/DrawingCanvas.cpp
  src/Gui/Controls/TabControl.cpp
  src/Gui/Controls/Accordion.cpp
  src/Gui/Controls/Pagination.cpp
  src/Gui/Controls/Carousel.cpp
  src/Gui/Controls/BreadcrumbBar.cpp
  src/Gui/Controls/SpreadsheetGrid.cpp
  src/Gui/Controls/PageView.cpp
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
  src/Media/AudioStreaming.cpp
  src/Midi/MidiFile.cpp
  src/Midi/MidiStream.cpp
  src/Midi/SoftwareSynth.cpp
  src/Hardware/Monitor/Edid.cpp
  src/Hardware/Monitor/Monitor.cpp
  src/Hardware/Monitor/MonitorDisplay.cpp
  src/Hardware/Monitor/MonitorLayout.cpp
  src/Hardware/Monitor/SystemMonitors.cpp
  src/Hardware/MidiDevice/Mpu401.cpp
  src/Hardware/SerialPort/SerialRing.cpp
  src/Hardware/SerialPort/Uart16550.cpp
  src/Hardware/SerialPort/SystemSerialPorts.cpp
  src/Hardware/MidiDevice/Usb/UsbMidiDescriptors.cpp
  src/Hardware/MidiDevice/Usb/UsbMidiDevice.cpp
  src/Hardware/MidiDevice/Usb/UsbMidiService.cpp
  src/Midi/LiveSynth.cpp
  src/Media/Audio/Pcm.cpp
  src/Media/Audio/Wav.cpp
  src/Media/Audio/Aiff.cpp
  src/Media/Audio/Au.cpp
  src/Media/Audio/Flac.cpp
  src/Media/Audio/Mdct.cpp
  src/Media/Audio/Vorbis.cpp
  src/Media/Audio/Mp3.cpp
  src/Media/Audio/Aac.cpp
  src/Media/Audio/AacEncoder.cpp
  src/Media/Audio/Opus.cpp
  src/Media/Audio/OpusCelt.cpp
  src/Media/Audio/OpusFile.cpp
  src/Media/Audio/OpusSilk.cpp
  src/Media/Audio/Mp4Audio.cpp
  src/Media/Mp4.cpp
  src/Media/Webm.cpp
  src/Media/WebVtt.cpp
  src/Executable/ElfFile.cpp
  src/Executable/ExecutableKind.cpp
  src/Executable/PeFile.cpp
  src/Process/Syscall.cpp
  src/Process/Process.cpp
  src/Process/AddressSpace.cpp
  src/Process/ImageLoader.cpp
  src/Process/UserProgram.cpp
  src/Process/UserPageTables.cpp
  src/Process/WindowsStartup.cpp
  src/Process/WindowsException.cpp
  src/Process/WindowsThreads.cpp
  src/Process/WindowsObjects.cpp
  src/Process/WindowsProcess.cpp
  src/Process/WindowsSystem.cpp
  src/Process/WindowsModules.cpp
  src/Process/WindowsPipes.cpp
  src/Process/WindowsLocks.cpp
  src/Process/WindowsJobs.cpp
  src/Process/WindowsDebug.cpp
  src/Process/WindowsMemory.cpp
  src/Process/WindowsSockets.cpp
  src/Process/Sockets.cpp
  src/Process/WindowsMailslots.cpp
  src/Process/WindowsRegistry.cpp
  src/Process/RegistryTree.cpp
  src/Process/DosWildcard.cpp
  src/Process/Objects.cpp
  src/Process/ModuleLoader.cpp
  src/Process/LinuxSyscalls.cpp
  src/Process/LinuxFiles.cpp
  src/Process/LinuxThreads.cpp
  src/Process/ProcessFiles.cpp
  src/Process/WindowsFiles.cpp
  src/Process/ProgramEnvironment.cpp
  src/Program/ProgramHost.cpp
  src/Program/ProgramRunner.cpp
  src/Program/EmulatedTask.cpp
  src/Emulation/GuestCpu.cpp
  src/Emulation/EmulatedProcess.cpp
  src/Emulation/X86_64Cpu.cpp
  src/Emulation/SoftFloat.cpp
  src/Emulation/SoftFloatExt.cpp
  src/Emulation/SoftFloatExtMath.cpp
  src/Emulation/X86_64X87.cpp
  src/Emulation/X86_64Vex.cpp
  src/Emulation/X86_64Crypto.cpp
  src/Emulation/X86_64Misc.cpp
  src/Emulation/X86_64Mmx.cpp
  src/Emulation/X86_64Amx.cpp
  src/Emulation/X86_64Xop.cpp
  src/Emulation/X86_64Lwp.cpp
  src/Emulation/X86_64Cet.cpp
  src/Emulation/X86_64Segment.cpp
  src/Emulation/X86_64Apx.cpp
  src/Emulation/X86_64EvexBf16.cpp
  src/Emulation/X86_64Evex.cpp
  src/Emulation/X86_64EvexMisc.cpp
  src/Emulation/X86_64EvexFp.cpp
  src/Emulation/X86_64EvexFp16.cpp
  src/Emulation/X86_64EvexExt.cpp
  src/Emulation/AArch64Cpu.cpp
  src/Emulation/AArch64Fp.cpp
  src/Emulation/AArch64Simd.cpp
  src/Emulation/AArch64Crypto.cpp
  src/Emulation/AArch64Sve.cpp
  src/Emulation/AArch64SveInt.cpp
  src/Emulation/AArch64SveMem.cpp
  src/Emulation/AArch64SvePred.cpp
  src/Emulation/AArch64SvePermute.cpp
  src/Emulation/AArch64SveFloat.cpp
  src/Emulation/AArch64Sve2.cpp
  src/Emulation/AArch64Sme.cpp
  src/Emulation/AArch64Sme2.cpp
  src/Emulation/AArch64Fp8.cpp
  src/Emulation/AArch64Fp8Neon.cpp
  src/Emulation/AArch64Fp8Sve.cpp
  src/Emulation/AArch64Lut.cpp
  src/Emulation/AArch64NeonShift.cpp
  src/Emulation/AArch64NeonThree.cpp
  src/Emulation/AArch64NeonElement.cpp
  src/Emulation/AArch64NeonTwo.cpp
  src/Emulation/AArch64NeonDiff.cpp
  src/Emulation/RiscV64Cpu.cpp
  src/Emulation/RiscV64Fp.cpp
  src/Emulation/RiscV64Vector.cpp
  src/Emulation/RiscV64VectorInt.cpp
  src/Emulation/RiscV64VectorFp.cpp
  src/Process/WindowsSyscalls.cpp
  src/Media/Dash.cpp
  src/Media/DashClient.cpp
  src/Media/Audio/WebmAudio.cpp
  src/Media/MseParsers.cpp
  src/Media/MseBuffer.cpp
  src/Media/MseHub.cpp
  src/Media/Eme.cpp
  src/Media/VideoFiles.cpp
  src/Media/MediaCodecs.cpp
  src/Video/AviFile.cpp
  src/Video/AviWriter.cpp
  src/Video/VideoPlayer.cpp
  src/Hardware/VideoCodec/VideoTypes.cpp
  src/Hardware/VideoCodec/VideoFrameBuffer.cpp
  src/Hardware/VideoCodec/ColorConvert.cpp
  src/Hardware/VideoCodec/SystemVideoCodecs.cpp
  src/Hardware/VideoCodec/AMD/AmdIpDiscovery.cpp
  src/Hardware/VideoCodec/AMD/AmdVcnFirmware.cpp
  src/Hardware/VideoCodec/AMD/AmdVcn1.cpp
  src/Hardware/VideoCodec/AMD/AmdUvd6.cpp
  src/Hardware/VideoCodec/AMD/AmdVce3.cpp
  src/Hardware/VideoCodec/AMD/AmdVce3Ring.cpp
  src/Hardware/VideoCodec/AMD/AmdVce3Messages.cpp
  src/Hardware/VideoCodec/AMD/AmdVce3Encoder.cpp
  src/Hardware/VideoCodec/AMD/AmdUvd6Ring.cpp
  src/Hardware/VideoCodec/AMD/AmdSmu7.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdGfx8Firmware.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdSmu7Ucode.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdGfx8.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdGfx8Compute.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdGmc8.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdIh3.cpp
  src/Hardware/Bus/Pci/PciMsi.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdVbios.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AtomBios.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AtomConnectors.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AtomI2c.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdDdc.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/I2cEdid.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdAux.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdPanelPower.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/Dpcd.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/DpLinkTrain.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdDpSource.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdModeSet.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdMonitor.cpp
  src/Hardware/Acpi/AcpiResources.cpp
  src/Hardware/Bus/I2c/DesignWareI2c.cpp
  src/Hardware/Bus/I2c/HidInputMap.cpp
  src/Hardware/Bus/I2c/I2cHidDevice.cpp
  src/Hardware/Bus/I2c/IntelGpio.cpp
  src/Hardware/Bus/I2c/LpssI2c.cpp
  src/Hardware/Mouse/I2c/I2cHidPointer.cpp
  src/Hardware/Mouse/I2c/PlatformI2cHid.cpp
  src/Hardware/Bus/Thunderbolt/ThunderboltDevices.cpp
  src/Hardware/Bus/Pci/PciMsix.cpp
  src/Hardware/Bus/Thunderbolt/ThunderboltCio.cpp
  src/Hardware/Bus/Thunderbolt/ThunderboltKeys.cpp
  src/Hardware/Bus/Thunderbolt/ThunderboltNet.cpp
  src/Hardware/Bus/Thunderbolt/ThunderboltPower.cpp
  src/Hardware/Bus/Thunderbolt/ThunderboltNvm.cpp
  src/Hardware/Bus/Thunderbolt/Usb4Domain.cpp
  src/Hardware/Bus/Thunderbolt/ThunderboltXdomainHost.cpp
  src/Hardware/Bus/Thunderbolt/ThunderboltRing.cpp
  src/Hardware/Bus/Thunderbolt/ThunderboltXdomain.cpp
  src/Hardware/Bus/Thunderbolt/ThunderboltControl.cpp
  src/Hardware/Bus/Thunderbolt/ThunderboltIcl.cpp
  src/Hardware/Bus/Thunderbolt/ThunderboltIcm.cpp
  src/Hardware/Bus/Thunderbolt/ThunderboltDriver.cpp
  src/Hardware/Bluetooth/BtCrypto.cpp
  src/Hardware/Bluetooth/BondFile.cpp
  src/Hardware/Bluetooth/L2cap.cpp
  src/Hardware/Bluetooth/Att.cpp
  src/Hardware/Bluetooth/Gatt.cpp
  src/Hardware/Bluetooth/Smp.cpp
  src/Hardware/Bluetooth/HciBluetoothAdapter.cpp
  src/Hardware/Bluetooth/HidOverGatt.cpp
  src/Hardware/Hid/HidDescriptor.cpp
  src/Hardware/Bluetooth/HciPackets.cpp
  src/Hardware/Bluetooth/UsbHciTransport.cpp
  src/Hardware/Bluetooth/HciController.cpp
  src/Hardware/Bluetooth/IntelBluetooth.cpp
  src/Hardware/Bluetooth/BluetoothService.cpp
  src/Hardware/WirelessInterface/Intel/IwlDevices.cpp
  src/Hardware/WirelessInterface/Intel/IwlFirmware.cpp
  src/Hardware/WirelessInterface/Intel/IwlPcie.cpp
  src/Hardware/WirelessInterface/Intel/IwlFwLoad.cpp
  src/Hardware/WirelessInterface/Intel/IwlRx.cpp
  src/Hardware/WirelessInterface/Intel/IwlTx.cpp
  src/Hardware/WirelessInterface/Intel/IwlTransport.cpp
  src/Hardware/WirelessInterface/Intel/IwlNvm.cpp
  src/Hardware/WirelessInterface/Intel/IwlInit.cpp
  src/Hardware/WirelessInterface/Intel/IwlDriver.cpp
  src/Hardware/WirelessInterface/Intel/IwlMvm.cpp
  src/Hardware/WirelessInterface/Intel/IwlScan.cpp
  src/Hardware/WirelessInterface/Intel/IwlLink.cpp
  src/Hardware/WirelessInterface/Intel/IwlWireless.cpp
  src/Hardware/WirelessInterface/Ieee80211Scan.cpp
  src/Hardware/WirelessInterface/Ieee80211Frames.cpp
  src/Hardware/WirelessInterface/Ieee80211Association.cpp
  src/Hardware/WirelessInterface/Wpa2.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnIsa.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnEmu.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/Gcn/TgsiParse.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnCompiler.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnDraw.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnTexture.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnTiling.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnPm4.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnState.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnCp.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdVegaDraw.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdAsicReset.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdIrq.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdSdma3.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdDce11.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/EdidAudio.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/HdmiInfoframe.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdVegaM.cpp
  src/Hardware/VideoCodec/AMD/AmdUvd6Messages.cpp
  src/Hardware/VideoCodec/AMD/AmdUvd6AvcDecoder.cpp
  src/Hardware/VideoCodec/AMD/AmdUvd6HevcDecoder.cpp
  src/Hardware/VideoCodec/AMD/AmdUvd6Mpeg2Decoder.cpp
  src/Hardware/VideoCodec/AMD/AmdUvd6Mpeg4Decoder.cpp
  src/Hardware/VideoCodec/AMD/AmdUvd6Vc1Decoder.cpp
  src/Hardware/VideoCodec/AMD/AmdGmc8Memory.cpp
  src/Hardware/VideoCodec/AMD/AmdRavenMemory.cpp
  src/Hardware/VideoCodec/AMD/AmdVcn1Ring.cpp
  src/Hardware/VideoCodec/AMD/AmdVcnAvcDecoder.cpp
  src/Hardware/VideoCodec/AMD/AmdVcnHevcDecoder.cpp
  src/Hardware/VideoCodec/AMD/AmdVcnVp9Decoder.cpp
  src/Hardware/VideoCodec/AMD/AmdVcnAv1Decoder.cpp
  src/Hardware/VideoCodec/AMD/AmdVcnMpeg2Decoder.cpp
  src/Hardware/VideoCodec/AMD/AmdVcnVc1Decoder.cpp
  src/Hardware/VideoCodec/AMD/AmdVcnSelfTest.cpp
  src/Hardware/VideoCodec/AMD/AmdVcnDecodeMessages.cpp
  src/Hardware/VideoCodec/AMD/AmdMediaEngineHw.cpp
  src/Hardware/VideoCodec/NVIDIA/NvidiaMediaEngineHw.cpp
  src/Hardware/VideoCodec/Intel/IntelMediaEngineHw.cpp
  src/Hardware/VideoCodec/Intel/IntelMfxCommands.cpp
  src/Hardware/VideoCodec/Intel/IntelAvcDecode.cpp
  src/Hardware/VideoCodec/Intel/IntelTiling.cpp
  src/Hardware/VideoCodec/Intel/IntelAvcSession.cpp
  src/Hardware/VideoCodec/Intel/IntelEngineBuffers.cpp
  src/Hardware/VideoCodec/Intel/IntelMpeg2Session.cpp
  src/Hardware/VideoCodec/Intel/IntelVc1Session.cpp
  src/Hardware/VideoCodec/Intel/IntelHevcSession.cpp
  src/Hardware/VideoCodec/Intel/IntelVp9Probs.cpp
  src/Hardware/VideoCodec/Intel/IntelVp9Session.cpp
  src/Hardware/VideoCodec/Intel/IntelAvpCommands.cpp
  src/Hardware/VideoCodec/Intel/IntelAv1Cdf.cpp
  src/Hardware/VideoCodec/Intel/IntelAv1Session.cpp
  src/Hardware/VideoCodec/Intel/IntelMediaVideoEngine.cpp
  src/Hardware/VideoCodec/Software/SoftwareVideoCodec.cpp
  src/Hardware/Printer/PrinterTypes.cpp
  src/Hardware/Printer/Halftone.cpp
  src/Hardware/Printer/PrintJob.cpp
  src/Hardware/Printer/TextLayout.cpp
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
  src/Hardware/VideoCodec/Software/H264Parser.cpp
  src/Hardware/VideoCodec/Software/H264Dpb.cpp
  src/Hardware/VideoCodec/Software/H264Encoder.cpp
  src/Hardware/VideoCodec/Software/H264Macroblock.cpp
  src/Hardware/VideoCodec/Software/H264Cabac.cpp
  src/Hardware/VideoCodec/Software/H264Predict.cpp
  src/Hardware/VideoCodec/Software/H264Deblock.cpp
  src/Hardware/VideoCodec/Software/H265.cpp
  src/Hardware/VideoCodec/Software/H265Syntax.cpp
  src/Hardware/VideoCodec/Software/H265Picture.cpp
  src/Hardware/VideoCodec/Software/H265Slice.cpp
  src/Hardware/VideoCodec/Software/H265Predict.cpp
  src/Hardware/VideoCodec/Software/H265Filter.cpp
  src/Hardware/VideoCodec/Software/Mpeg2Syntax.cpp
  src/Hardware/VideoCodec/Software/Mpeg2Slice.cpp
  src/Hardware/VideoCodec/Software/Mpeg2Recon.cpp
  src/Hardware/VideoCodec/Software/Mpeg4Headers.cpp
  src/Hardware/VideoCodec/Software/Vc1Syntax.cpp
  src/Hardware/VideoCodec/Software/Vc1Block.cpp
  src/Hardware/VideoCodec/Software/Vc1Recon.cpp
  src/Hardware/VideoCodec/Software/Vp8.cpp
  src/Hardware/VideoCodec/Software/Vp9.cpp
  src/Hardware/VideoCodec/Software/Vp9Syntax.cpp
  src/Hardware/VideoCodec/Software/Vp9Recon.cpp
  src/Hardware/VideoCodec/Software/Vp9LoopFilter.cpp
  src/Hardware/VideoCodec/Software/Av1.cpp
  src/Hardware/VideoCodec/Software/Av1Frame.cpp
  src/Hardware/VideoCodec/Software/Av1Decode.cpp
  src/Hardware/VideoCodec/Software/Av1Recon.cpp
  src/Hardware/VideoCodec/Software/Av1Obu.cpp
  src/Hardware/VideoCodec/Software/Av1Cdf.cpp
  src/Hardware/VideoCodec/Software/Av1Itx.cpp
  src/Hardware/VideoCodec/Software/Av1Ipred.cpp
  src/Hardware/VideoCodec/Software/Av1Mc.cpp
  src/Hardware/VideoCodec/Software/Av1Wedge.cpp
  src/Hardware/VideoCodec/Software/Av1Warp.cpp
  src/Hardware/VideoCodec/Software/Av1Refmvs.cpp
  src/Hardware/VideoCodec/Software/Av1LoopFilter.cpp
  src/Hardware/VideoCodec/Software/Av1Cdef.cpp
  src/Hardware/VideoCodec/Software/Av1LoopRestoration.cpp
  src/Hardware/VideoCodec/Software/Av1SuperRes.cpp
  src/Hardware/VideoCodec/Software/Av1FilmGrain.cpp
  src/Hardware/VideoCodec/Software/JpegEncoder.cpp
  src/Gui/Controls/Menu.cpp
  src/Gui/Windows/Frame.cpp
  src/Gui/Windows/TabFrame.cpp
  src/Gui/Windows/WindowManager.cpp
  src/Gui/Windows/WindowSwitcher.cpp
  src/Gui/MessageDialog.cpp
  src/Gui/NoticeDialog.cpp
  src/Graphics/Graphics3D/Software/SoftwareDevice.cpp
  src/Hardware/x86_64/port.S
)

target_include_directories(BOOTX64 PRIVATE src)
# The root certificates built into the image (.incbin looks for the file from the source folder).
set_source_files_properties(src/Network/Tls/CaBundle.S PROPERTIES COMPILE_OPTIONS "-I${CMAKE_SOURCE_DIR}" OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/certs/ca-bundle.pem)
# The logo built into the image (assets/logo.png).
set_source_files_properties(src/Graphics/Logo/Logo.S PROPERTIES COMPILE_OPTIONS "-I${CMAKE_SOURCE_DIR}" OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/assets/logo.png)
set_target_properties(BOOTX64 PROPERTIES SUFFIX ".efi")

target_compile_options(BOOTX64 PRIVATE
  -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector
  -fno-threadsafe-statics -mno-red-zone -fshort-wchar
  -Wall -Wextra -g -O1
)
# Keep frame pointers so gdb backtraces work.
target_compile_options(BOOTX64 PRIVATE -fno-omit-frame-pointer)

option(TCOS_USERMODE_DEMO "Run two small programs in ring 3 at boot (x86-64)" OFF)
option(TCOS_USERMODE_DEMO_PAIR "With TCOS_USERMODE_DEMO_PROGRAM: also run it twice at the same time, as \"server\" and \"client\"" OFF)
if(TCOS_USERMODE_DEMO)
  target_compile_definitions(BOOTX64 PRIVATE TCOS_USERMODE_DEMO)
endif()
if(TCOS_USERMODE_DEMO_PAIR)
  target_compile_definitions(BOOTX64 PRIVATE TCOS_USERMODE_DEMO_PAIR)
endif()

# A PE program to run once, after all hardware is ready (not before, like TCOS_USERMODE_DEMO): works around a firmware-driver-detach bug a PE program's
# execution triggers (see the "usermode-windows" project memory), by running after every detach_firmware_drivers() caller (network, storage) instead of
# before it. Independent of TCOS_USERMODE_DEMO, so it can run without the full built-in demo.
set(TCOS_WINE_TEST_PROGRAM "" CACHE FILEPATH "A PE program to run once, after all hardware bring-up (a TCOS_USERMODE_DEMO alternative that avoids the firmware-driver-detach bug)")
if(TCOS_WINE_TEST_PROGRAM)
  file(WRITE ${CMAKE_BINARY_DIR}/wine_test_program.S
    ".section .rdata,\"dr\"\n.balign 16\n.global tcos_wine_test_program\ntcos_wine_test_program:\n.incbin \"${TCOS_WINE_TEST_PROGRAM}\"\n.global tcos_wine_test_program_end\ntcos_wine_test_program_end:\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TCOS_WINE_TEST_PROGRAM})
  target_sources(BOOTX64 PRIVATE ${CMAKE_BINARY_DIR}/wine_test_program.S)
  set_source_files_properties(${CMAKE_BINARY_DIR}/wine_test_program.S PROPERTIES OBJECT_DEPENDS ${TCOS_WINE_TEST_PROGRAM})
  target_compile_definitions(BOOTX64 PRIVATE TCOS_WINE_TEST_PROGRAM)
  # More files for C:\\Windows\\System32 (DLLs the test program uses, say): a ;-separated list.
  set(TCOS_WINE_TEST_FILES "" CACHE STRING "Files TCOS_WINE_TEST_PROGRAM puts next to it in C:\\Windows\\System32")
  set(_wine_test_files_s ".section .rdata,\"dr\"\n.balign 16\n")
  set(_wine_test_table "")
  set(_wine_test_index 0)
  foreach(_file ${TCOS_WINE_TEST_FILES})
    get_filename_component(_name ${_file} NAME)
    string(APPEND _wine_test_files_s ".balign 16\ntcos_wine_test_file_${_wine_test_index}:\n.incbin \"${_file}\"\ntcos_wine_test_file_${_wine_test_index}_end:\ntcos_wine_test_name_${_wine_test_index}:\n.asciz \"${_name}\"\n")
    string(APPEND _wine_test_table ".quad tcos_wine_test_name_${_wine_test_index}, tcos_wine_test_file_${_wine_test_index}, tcos_wine_test_file_${_wine_test_index}_end\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_file})
    math(EXPR _wine_test_index "${_wine_test_index}+1")
  endforeach()
  string(APPEND _wine_test_files_s ".balign 8\n.global tcos_wine_test_files\ntcos_wine_test_files:\n${_wine_test_table}.global tcos_wine_test_file_count\ntcos_wine_test_file_count:\n.quad ${_wine_test_index}\n")
  file(WRITE ${CMAKE_BINARY_DIR}/wine_test_files.S "${_wine_test_files_s}")
  target_sources(BOOTX64 PRIVATE ${CMAKE_BINARY_DIR}/wine_test_files.S)
  set_source_files_properties(${CMAKE_BINARY_DIR}/wine_test_files.S PROPERTIES OBJECT_DEPENDS "${TCOS_WINE_TEST_FILES}")
endif()
# ntdll.dll, kernel32.dll and msvcrt.dll for Windows programs (src/Windows): freestanding DLLs built with the same mingw compiler. They are not part of the kernel: they are
# files of the primary disk image (/Windows/Windows/System32, see `primary` below) that the loader maps into each Windows process. kernel32.dll is linked against ntdll's import
# library, as a real one would be.
if(TCOS_ARCH STREQUAL "x86_64")
  set(WINDOWS_DIR ${CMAKE_BINARY_DIR}/windows)
  set(WINDOWS_DLL_FLAGS -O2 -Wall -Wextra -Wno-unused-parameter -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-stack-protector
      -fasynchronous-unwind-tables -nostdlib -shared -Wl,--entry=DllMain)
  file(GLOB WINDOWS_HEADERS ${CMAKE_SOURCE_DIR}/src/Windows/*.h)
  set(WINDOWS_STUBS_C ${CMAKE_SOURCE_DIR}/src/Windows/stubs/stubs.c)
  # The exports that are not implemented yet (src/Windows/stubs/<dll>.inc, see scripts/gen-windows-stubs.py) are compiled in as functions that do nothing: ntdll.dll's
  # answer STATUS_NOT_IMPLEMENTED, the others' answer 0.
  function(windows_stub_flags out dll result)
    set(${out} -Wno-builtin-declaration-mismatch -DSTUB_FILE=${dll}.inc -DSTUB_RESULT=${result} ${WINDOWS_STUBS_C} PARENT_SCOPE)
  endfunction()
  windows_stub_flags(ntdll_stubs ntdll 0xC0000002u)
  add_custom_command(OUTPUT ${WINDOWS_DIR}/ntdll.dll ${WINDOWS_DIR}/libntdll.a
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WINDOWS_DIR}
    COMMAND ${CMAKE_C_COMPILER} ${WINDOWS_DLL_FLAGS} ${ntdll_stubs} -Wl,--image-base=0x180000000 -Wl,--out-implib=${WINDOWS_DIR}/libntdll.a
            -o ${WINDOWS_DIR}/ntdll.dll ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/ntdll.c
    DEPENDS ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/ntdll.c ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/except.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/heap.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/format.inc ${CMAKE_SOURCE_DIR}/src/Windows/libc_string.inc ${CMAKE_SOURCE_DIR}/src/Windows/fmtengine.inc ${WINDOWS_HEADERS} ${CMAKE_SOURCE_DIR}/src/Windows/stubs/ntdll.inc ${WINDOWS_STUBS_C} ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/services.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/zw.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/rtl.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/libc.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/rtl2.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/rtl3.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/sec.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/status.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/casetab.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/normalize.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/uilang.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/locale_names.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/unidata.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/idn_data.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/reg.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/fls.inc ${CMAKE_SOURCE_DIR}/src/Windows/ntdll/actctx.inc
    COMMENT "Building ntdll.dll")
  windows_stub_flags(kernel32_stubs kernel32 0)
  add_custom_command(OUTPUT ${WINDOWS_DIR}/kernel32.dll
    COMMAND ${CMAKE_C_COMPILER} ${WINDOWS_DLL_FLAGS} ${kernel32_stubs} -Wl,--image-base=0x180200000 -o ${WINDOWS_DIR}/kernel32.dll
            ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/kernel32.c ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/kernel32.def ${WINDOWS_DIR}/libntdll.a
    DEPENDS ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/kernel32.c ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/extra.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/extra2.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/volume.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/extra3.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/console.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/legacy.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/fmtmsg.inc ${CMAKE_SOURCE_DIR}/src/Windows/fmtengine.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/time.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/comm.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/memory.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/nls.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/actctx.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/codepage.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/codepage_data.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/ctype_data.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/idn_data.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/idn.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/locale.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/nls2.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/geo_data.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/locale_data.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/rsrcupdate.inc ${CMAKE_SOURCE_DIR}/src/Windows/kernel32/kernel32.def ${WINDOWS_DIR}/libntdll.a ${WINDOWS_HEADERS} ${CMAKE_SOURCE_DIR}/src/Windows/stubs/kernel32.inc ${WINDOWS_STUBS_C}
    COMMENT "Building kernel32.dll")
  windows_stub_flags(msvcrt_stubs msvcrt 0)
  add_custom_command(OUTPUT ${WINDOWS_DIR}/msvcrt.dll
    COMMAND ${CMAKE_C_COMPILER} ${WINDOWS_DLL_FLAGS} ${msvcrt_stubs} -Wno-unused-value -Wl,--image-base=0x180400000 -o ${WINDOWS_DIR}/msvcrt.dll
            ${CMAKE_SOURCE_DIR}/src/Windows/msvcrt/msvcrt.c ${CMAKE_SOURCE_DIR}/src/Windows/msvcrt/msvcrt.def ${WINDOWS_DIR}/libntdll.a ${WINDOWS_DIR}/kernel32.dll
    DEPENDS ${CMAKE_SOURCE_DIR}/src/Windows/msvcrt/msvcrt.c ${CMAKE_SOURCE_DIR}/src/Windows/msvcrt/msvcrt.def ${CMAKE_SOURCE_DIR}/src/Windows/msvcrt/format.inc ${CMAKE_SOURCE_DIR}/src/Windows/libc_string.inc
            ${WINDOWS_DIR}/libntdll.a ${WINDOWS_DIR}/kernel32.dll ${WINDOWS_HEADERS} ${CMAKE_SOURCE_DIR}/src/Windows/stubs/msvcrt.inc ${WINDOWS_STUBS_C}
    COMMENT "Building msvcrt.dll")
  windows_stub_flags(advapi32_stubs advapi32 0)
  add_custom_command(OUTPUT ${WINDOWS_DIR}/advapi32.dll
    COMMAND ${CMAKE_C_COMPILER} ${WINDOWS_DLL_FLAGS} ${advapi32_stubs} -Wl,--image-base=0x180600000 -o ${WINDOWS_DIR}/advapi32.dll
            ${CMAKE_SOURCE_DIR}/src/Windows/advapi32/advapi32.c ${CMAKE_SOURCE_DIR}/src/Windows/advapi32/advapi32.def ${WINDOWS_DIR}/libntdll.a ${WINDOWS_DIR}/kernel32.dll
    DEPENDS ${CMAKE_SOURCE_DIR}/src/Windows/advapi32/advapi32.c ${CMAKE_SOURCE_DIR}/src/Windows/advapi32/advapi32.def ${CMAKE_SOURCE_DIR}/src/Windows/advapi32/registry.inc
            ${WINDOWS_DIR}/libntdll.a ${WINDOWS_DIR}/kernel32.dll ${WINDOWS_HEADERS} ${CMAKE_SOURCE_DIR}/src/Windows/stubs/advapi32.inc ${WINDOWS_STUBS_C}
    COMMENT "Building advapi32.dll")
  windows_stub_flags(ws2_32_stubs ws2_32 0)
  add_custom_command(OUTPUT ${WINDOWS_DIR}/ws2_32.dll
    COMMAND ${CMAKE_C_COMPILER} ${WINDOWS_DLL_FLAGS} ${ws2_32_stubs} -Wl,--image-base=0x180800000 -o ${WINDOWS_DIR}/ws2_32.dll
            ${CMAKE_SOURCE_DIR}/src/Windows/ws2_32/ws2_32.c ${CMAKE_SOURCE_DIR}/src/Windows/ws2_32/ws2_32.def ${WINDOWS_DIR}/libntdll.a ${WINDOWS_DIR}/kernel32.dll
    DEPENDS ${CMAKE_SOURCE_DIR}/src/Windows/ws2_32/ws2_32.c ${CMAKE_SOURCE_DIR}/src/Windows/ws2_32/ws2_32.def
            ${WINDOWS_DIR}/libntdll.a ${WINDOWS_DIR}/kernel32.dll ${WINDOWS_HEADERS} ${CMAKE_SOURCE_DIR}/src/Windows/stubs/ws2_32.inc ${WINDOWS_STUBS_C}
    COMMENT "Building ws2_32.dll")
  # The other system libraries of Windows (user32.dll, gdi32.dll ...): files with every export in place, none of them implemented yet. They are only stubs (see above).
  set(WINDOWS_STUB_LIBRARIES user32 gdi32 shell32 ole32 oleaut32 comctl32 comdlg32 rpcrt4 shlwapi winmm version imm32 secur32 crypt32 bcrypt ncrypt userenv setupapi iphlpapi
      dbghelp psapi wininet winhttp wintrust netapi32 dwmapi uxtheme mswsock gdiplus msimg32 opengl32 glu32 dnsapi mpr wtsapi32 powrprof cfgmgr32 d3d9 d3d11 dxgi dwrite shcore
      avrt winspool wevtapi pdh wldap32 urlmon msi propsys hid usp10 winscard odbc32 xinput1_4 dsound dinput8 d2d1 d3dcompiler_47 lz32 oleacc oledlg credui ntdsapi samlib
      activeds normaliz sensapi authz rasapi32)
  # win32u.dll holds the system-call stubs of win32k (NtUser*, NtGdi*: src/Windows/win32u/services.inc).
  add_custom_command(OUTPUT ${WINDOWS_DIR}/win32u.dll
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WINDOWS_DIR}
    COMMAND ${CMAKE_C_COMPILER} ${WINDOWS_DLL_FLAGS} -Wl,--image-base=0x180900000 -o ${WINDOWS_DIR}/win32u.dll ${CMAKE_SOURCE_DIR}/src/Windows/win32u/win32u.c
    DEPENDS ${CMAKE_SOURCE_DIR}/src/Windows/win32u/win32u.c ${CMAKE_SOURCE_DIR}/src/Windows/win32u/services.inc ${WINDOWS_HEADERS}
    COMMENT "Building win32u.dll")
  set(WINDOWS_LIBRARIES ntdll kernel32 msvcrt advapi32 ws2_32 win32u ${WINDOWS_STUB_LIBRARIES})
  set(_stub_image_base 0x180A00000)
  foreach(_library ${WINDOWS_STUB_LIBRARIES})
    windows_stub_flags(_flags ${_library} 0)
    # A library with code of its own (src/Windows/<dll>/<dll>.c and the .inc files next to it) is linked over kernel32.dll; the stubs fill in what it does not define yet.
    set(_own ${CMAKE_SOURCE_DIR}/src/Windows/${_library}/${_library}.c)
    set(_own_sources "")
    set(_own_depends "")
    if(EXISTS ${_own})
      file(GLOB _own_incs ${CMAKE_SOURCE_DIR}/src/Windows/${_library}/*.inc)
      set(_own_sources ${_own} ${WINDOWS_DIR}/libntdll.a ${WINDOWS_DIR}/kernel32.dll)
      set(_own_depends ${_own} ${_own_incs} ${WINDOWS_DIR}/libntdll.a ${WINDOWS_DIR}/kernel32.dll ${WINDOWS_HEADERS})
    endif()
    add_custom_command(OUTPUT ${WINDOWS_DIR}/${_library}.dll
      COMMAND ${CMAKE_COMMAND} -E make_directory ${WINDOWS_DIR}
      COMMAND ${CMAKE_C_COMPILER} ${WINDOWS_DLL_FLAGS} ${_flags} -DSTUB_DLLMAIN -Wl,--image-base=${_stub_image_base} -o ${WINDOWS_DIR}/${_library}.dll ${_own_sources}
      DEPENDS ${CMAKE_SOURCE_DIR}/src/Windows/stubs/${_library}.inc ${WINDOWS_STUBS_C} ${_own_depends}
      COMMENT "Building ${_library}.dll")
    math(EXPR _stub_image_base "${_stub_image_base}+0x100000" OUTPUT_FORMAT HEXADECIMAL)
  endforeach()
  set(WINDOWS_DLL_FILES "")
  foreach(_library ${WINDOWS_LIBRARIES})
    list(APPEND WINDOWS_DLL_FILES ${WINDOWS_DIR}/${_library}.dll)
  endforeach()
  add_custom_target(windows_dlls DEPENDS ${WINDOWS_DLL_FILES})
  if(TCOS_USERMODE_DEMO OR TCOS_WINE_TEST_PROGRAM)
    # The demo (and TCOS_WINE_TEST_PROGRAM, below) run on a RAM file system, not on the primary disk: they get copies of the libraries inside the build and write them to /Windows/Windows/System32 themselves.
    set(_demo_libraries_s ".section .rdata,\"dr\"\n.balign 16\n")
    set(_demo_libraries_table "")
    set(_demo_library_index 0)
    foreach(_library ${WINDOWS_LIBRARIES})
      string(APPEND _demo_libraries_s ".balign 16\ntcos_demo_library_${_demo_library_index}:\n.incbin \"${WINDOWS_DIR}/${_library}.dll\"\ntcos_demo_library_${_demo_library_index}_end:\ntcos_demo_library_name_${_demo_library_index}:\n.asciz \"${_library}.dll\"\n")
      string(APPEND _demo_libraries_table ".quad tcos_demo_library_name_${_demo_library_index}, tcos_demo_library_${_demo_library_index}, tcos_demo_library_${_demo_library_index}_end\n")
      math(EXPR _demo_library_index "${_demo_library_index}+1")
    endforeach()
    string(APPEND _demo_libraries_s ".balign 8\n.global tcos_demo_libraries\ntcos_demo_libraries:\n${_demo_libraries_table}.global tcos_demo_library_count\ntcos_demo_library_count:\n.quad ${_demo_library_index}\n")
    file(WRITE ${WINDOWS_DIR}/demo_libraries.S "${_demo_libraries_s}")
    target_sources(BOOTX64 PRIVATE ${WINDOWS_DIR}/demo_libraries.S)
    set_source_files_properties(${WINDOWS_DIR}/demo_libraries.S PROPERTIES OBJECT_DEPENDS "${WINDOWS_DLL_FILES}")
  endif()
endif()

# An extra program for the demo to run (a static Linux x86-64 ELF, say a C program built with `gcc -static -no-pie`); its output and exit code are logged.
set(TCOS_USERMODE_DEMO_PROGRAM "" CACHE FILEPATH "An ELF or PE program the user-mode demo also runs")
if(TCOS_USERMODE_DEMO AND TCOS_USERMODE_DEMO_PROGRAM)
  file(WRITE ${CMAKE_BINARY_DIR}/demo_program.S
    ".section .rdata,\"dr\"\n.balign 16\n.global tcos_demo_program\ntcos_demo_program:\n.incbin \"${TCOS_USERMODE_DEMO_PROGRAM}\"\n.global tcos_demo_program_end\ntcos_demo_program_end:\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TCOS_USERMODE_DEMO_PROGRAM})
  target_sources(BOOTX64 PRIVATE ${CMAKE_BINARY_DIR}/demo_program.S)
  set_source_files_properties(${CMAKE_BINARY_DIR}/demo_program.S PROPERTIES OBJECT_DEPENDS ${TCOS_USERMODE_DEMO_PROGRAM})
  target_compile_definitions(BOOTX64 PRIVATE TCOS_USERMODE_DEMO_PROGRAM)
  # More files for C:\\Windows\\System32 (DLLs and programs the extra program uses, say): a ;-separated list.
  set(TCOS_USERMODE_DEMO_FILES "" CACHE STRING "Files the user-mode demo puts next to the extra program")
  set(_demo_files_s ".section .rdata,\"dr\"\n.balign 16\n")
  set(_demo_table "")
  set(_demo_index 0)
  foreach(_file ${TCOS_USERMODE_DEMO_FILES})
    get_filename_component(_name ${_file} NAME)
    string(APPEND _demo_files_s ".balign 16\ntcos_demo_file_${_demo_index}:\n.incbin \"${_file}\"\ntcos_demo_file_${_demo_index}_end:\ntcos_demo_name_${_demo_index}:\n.asciz \"${_name}\"\n")
    string(APPEND _demo_table ".quad tcos_demo_name_${_demo_index}, tcos_demo_file_${_demo_index}, tcos_demo_file_${_demo_index}_end\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_file})
    math(EXPR _demo_index "${_demo_index}+1")
  endforeach()
  string(APPEND _demo_files_s ".balign 8\n.global tcos_demo_files\ntcos_demo_files:\n${_demo_table}.global tcos_demo_file_count\ntcos_demo_file_count:\n.quad ${_demo_index}\n")
  file(WRITE ${CMAKE_BINARY_DIR}/demo_files.S "${_demo_files_s}")
  target_sources(BOOTX64 PRIVATE ${CMAKE_BINARY_DIR}/demo_files.S)
  set_source_files_properties(${CMAKE_BINARY_DIR}/demo_files.S PROPERTIES OBJECT_DEPENDS "${TCOS_USERMODE_DEMO_FILES}")
endif()
option(TCOS_INTEL_RENDER "Use the render engine of an Intel Gen9.5 display GPU for 3D and for the desktop (not yet run on real hardware; the CPU renderer is used if its self-test fails; -DTCOS_INTEL_RENDER=OFF keeps the CPU renderer)" ON)
if(TCOS_INTEL_RENDER)
  target_compile_definitions(BOOTX64 PRIVATE TCOS_INTEL_RENDER)
endif()
option(TCOS_AMD_UVD "Start the UVD 6 video engine of a Radeon RX Vega M (Kaby Lake-G) and decode H.264 on it (not yet run on a real card; H.264 stays on the CPU if its self-test fails)" OFF)
if(TCOS_AMD_UVD)
  target_compile_definitions(BOOTX64 PRIVATE TCOS_AMD_UVD)
endif()
option(TCOS_AMD_VEGAM "Bring up the Radeon RX Vega M (Kaby Lake-G): memory controller and GART, interrupt ring, microcode through the SMC, graphics engine with compute queues, SDMA, and the display surface (not yet run on a real card)" OFF)
if(TCOS_AMD_VEGAM)
  target_compile_definitions(BOOTX64 PRIVATE TCOS_AMD_VEGAM)
endif()
option(TCOS_INTEL_WIFI "Start the Intel Wireless-AC 9000 series card (firmware from /firmware, initialisation, hardware address and channels; no scanning or joining yet; not yet run on a real card)" OFF)
if(TCOS_INTEL_WIFI)
  target_compile_definitions(BOOTX64 PRIVATE TCOS_INTEL_WIFI)
endif()
option(TCOS_BLUETOOTH "Start the Bluetooth controllers on the USB ports (Intel firmware from /firmware/ibt-*.sfi, bring-up, events; nothing is scanned or paired; not yet run on a real controller)" OFF)
if(TCOS_BLUETOOTH)
  target_compile_definitions(BOOTX64 PRIVATE TCOS_BLUETOOTH)
endif()
option(TCOS_THUNDERBOLT "Start the Thunderbolt 3 host interface (Alpine Ridge: control channel, the controller's firmware, devices reported as they are connected; PCIe tunnels only on request; not yet run on a real controller)" OFF)
option(TCOS_THUNDERBOLT_START_FIRMWARE "With TCOS_THUNDERBOLT: start the controller's connection manager firmware if the BIOS did not (resets the controller's CPU through the upstream port's vendor registers)" OFF)
option(TCOS_ACPI_EVENTS "Handle ACPI events: the power button, the sleep button and the lid, the embedded controller's events, the SCI through the I/O APIC (implied by TCOS_THUNDERBOLT; not yet run on a real machine)" OFF)
if(TCOS_ACPI_EVENTS OR TCOS_THUNDERBOLT)
  target_compile_definitions(BOOTX64 PRIVATE TCOS_ACPI_EVENTS)
endif()
option(TCOS_THUNDERBOLT_RUNTIME_PM "With TCOS_THUNDERBOLT: put the controller in D3hot after half a minute without devices or another host, and wake it by the ACPI event the tables name (polled)" OFF)
if(TCOS_THUNDERBOLT)
  target_compile_definitions(BOOTX64 PRIVATE TCOS_THUNDERBOLT)
  if(TCOS_THUNDERBOLT_RUNTIME_PM)
    target_compile_definitions(BOOTX64 PRIVATE TCOS_THUNDERBOLT_RUNTIME_PM)
  endif()
  if(TCOS_THUNDERBOLT_START_FIRMWARE)
    target_compile_definitions(BOOTX64 PRIVATE TCOS_THUNDERBOLT_START_FIRMWARE)
  endif()
endif()
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
  # Microcode the OS loads from /firmware (see firmware/README.md; AMD's and Intel's license files travel with it)
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/firmware ${ESP_DIR}/firmware
  DEPENDS BOOTX64
  COMMENT "Assembling ESP directory"
)

# The primary disk image: a FAT32 volume with what belongs on the root volume of an installed system (today the Windows libraries in /Windows/Windows/System32). It is
# filled by the OS's own FAT driver (tools/PrimaryImage.cpp, built for this machine). An image that exists keeps its files, so the disk the OS writes to survives builds;
# the listed files are put on it again each time. Write it to a disk or a USB stick, or let the run targets attach it.
set(TCOS_PRIMARY_IMAGE ${CMAKE_BINARY_DIR}/primary.img)
set(TCOS_PRIMARY_IMAGE_SIZE 256M CACHE STRING "Size of the primary disk image (when it is made)")
find_program(MKFS_FAT mkfs.fat)
find_program(TCOS_HOST_CXX NAMES c++ g++ clang++)
if(TCOS_ARCH STREQUAL "x86_64" AND MKFS_FAT AND TCOS_HOST_CXX)
  set(_primary_tool ${CMAKE_BINARY_DIR}/tools/primary_image)
  file(GLOB _primary_tool_sources ${CMAKE_SOURCE_DIR}/src/FileSystem/Fat/*.cpp ${CMAKE_SOURCE_DIR}/src/FileSystem/Fat/*.hpp)
  set(_primary_tool_inputs ${CMAKE_SOURCE_DIR}/tools/PrimaryImage.cpp ${CMAKE_SOURCE_DIR}/tests/KernelStubs.cpp ${CMAKE_SOURCE_DIR}/src/Kernel/Sync.cpp
      ${CMAKE_SOURCE_DIR}/src/FileSystem/FileSystemTypes.cpp ${_primary_tool_sources})
  list(FILTER _primary_tool_inputs EXCLUDE REGEX "\\.hpp$")
  add_custom_command(OUTPUT ${_primary_tool}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/tools
    COMMAND ${TCOS_HOST_CXX} -std=c++20 -O1 -I ${CMAKE_SOURCE_DIR}/src -o ${_primary_tool} ${_primary_tool_inputs}
    DEPENDS ${_primary_tool_inputs} ${CMAKE_SOURCE_DIR}/src/FileSystem/Fat/FatFileSystem.hpp
    COMMENT "Building the primary image tool")
  set(_system32 /Windows/Windows/System32)
  set(_system32_files "")
  foreach(_library ${WINDOWS_LIBRARIES})
    list(APPEND _system32_files file:${_system32}/${_library}.dll=${WINDOWS_DIR}/${_library}.dll)
  endforeach()
  add_custom_target(primary ALL
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/make-primary-image.sh ${TCOS_PRIMARY_IMAGE} ${TCOS_PRIMARY_IMAGE_SIZE} ${_primary_tool}
           
            ${_system32_files}
    DEPENDS ${_primary_tool}
    COMMENT "Filling the primary disk image")
  add_dependencies(primary windows_dlls)
else()
  message(WARNING "No primary disk image: it needs an x86-64 build, mkfs.fat and a host C++ compiler")
  add_custom_target(primary COMMAND ${CMAKE_COMMAND} -E touch ${TCOS_PRIMARY_IMAGE})
endif()

find_program(QEMU qemu-system-x86_64)
find_file(OVMF_CODE NAMES OVMF_CODE.4m.fd OVMF_CODE_4M.fd OVMF_CODE.fd
  PATHS /usr/share/edk2/x64 /usr/share/OVMF /usr/share/edk2-ovmf/x64 /usr/share/edk2/ovmf
        /usr/share/ovmf/x64 /usr/share/qemu)
find_file(OVMF_VARS NAMES OVMF_VARS.4m.fd OVMF_VARS_4M.fd OVMF_VARS.fd
  PATHS /usr/share/edk2/x64 /usr/share/OVMF /usr/share/edk2-ovmf/x64 /usr/share/edk2/ovmf
        /usr/share/ovmf/x64 /usr/share/qemu)

if(QEMU AND OVMF_CODE AND OVMF_VARS)
  # The host sound backend: PipeWire, else PulseAudio, ALSA or SDL, whichever this QEMU has first (`none` discards the sound).
  # Override with -DQEMU_AUDIO_DRIVER=wav,path=... or any other -audiodev driver.
  if(NOT QEMU_AUDIO_DRIVER)
    execute_process(COMMAND ${QEMU} -audiodev help OUTPUT_VARIABLE QEMU_AUDIO_DRIVERS ERROR_QUIET)
    set(QEMU_AUDIO_DRIVER none)
    foreach(driver pipewire pa alsa sdl)
      if(QEMU_AUDIO_DRIVERS MATCHES "(^|\n)${driver}(\n|$)")
        set(QEMU_AUDIO_DRIVER ${driver})
        break()
      endif()
    endforeach()
    set(QEMU_AUDIO_DRIVER ${QEMU_AUDIO_DRIVER} CACHE STRING "QEMU -audiodev driver for the guest's sound card")
  endif()
  message(STATUS "QEMU audio backend: ${QEMU_AUDIO_DRIVER}")
  set(QEMU_COMMON
    ${QEMU} -machine q35 -smp ${TCOS_SMP} -m 8G -netdev user,id=net0 -device virtio-net-pci,netdev=net0
    -rtc base=localtime  # the taskbar clock shows local time
    # An absolute pointer for the desktop (no click-to-grab; relative PS/2 grabbing is unreliable under Wayland).
    -device virtio-tablet-pci
    # Guest services: the host's entropy pool, a memory balloon (`balloon` in the monitor) and the crash notification.
    -device virtio-rng-pci -device virtio-balloon-pci -device pvpanic
    # An HD Audio controller with a duplex codec, played through the host's sound server (QEMU_AUDIO_DRIVER).
    -audiodev ${QEMU_AUDIO_DRIVER},id=snd0 -device ich9-intel-hda -device hda-duplex,audiodev=snd0
    -drive if=pflash,format=raw,readonly=on,file=${OVMF_CODE}
    -drive if=pflash,format=raw,file=${CMAKE_BINARY_DIR}/OVMF_VARS.fd
    -drive format=raw,file=fat:rw:${ESP_DIR}
    # The primary disk (the `primary` target makes it): the root volume, with the Windows libraries in /Windows/Windows/System32.
    -drive if=none,id=data,format=raw,file=${TCOS_PRIMARY_IMAGE} -device virtio-blk-pci,drive=data
    # OS debug output (port 0x402) goes to this terminal and debugcon.log.
    -chardev stdio,id=dbg,logfile=${CMAKE_BINARY_DIR}/debugcon.log,signal=off
    -device isa-debugcon,iobase=0x402,chardev=dbg
    -serial null
    # `scripts/qemu-status.py` asks this socket why the VM is paused.
    -qmp unix:${CMAKE_BINARY_DIR}/qmp.sock,server=on,wait=off
  )
  add_custom_target(vars
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OVMF_VARS} ${CMAKE_BINARY_DIR}/OVMF_VARS.fd
    COMMAND chmod u+w ${CMAKE_BINARY_DIR}/OVMF_VARS.fd)

  # `cmake --build build --target run`
  # The standard VGA card gets 64 MiB of video memory (the default 16 MiB leaves out the largest resolutions).
  set(QEMU_STD_VGA -vga none -device VGA,vgamem_mb=64)
  # The same with a virtio-gpu 3D device: QEMU renders through the host's OpenGL (virgl).
  set(QEMU_GL_GPU -vga none -device virtio-gpu-gl-pci -display gtk,gl=on,show-cursor=off,show-menubar=off)
  # `run` and `debug` use the 3D device when this QEMU has it (so the 3D test is accelerated), else standard VGA.
  execute_process(COMMAND ${QEMU} -device help OUTPUT_VARIABLE QEMU_DEVICES ERROR_QUIET)
  if(QEMU_DEVICES MATCHES "virtio-gpu-gl-pci")
    set(QEMU_DEFAULT_GPU ${QEMU_GL_GPU})
    message(STATUS "QEMU has virtio-gpu-gl: the run and debug targets use virgl 3D (run-std for the standard VGA card)")
  else()
    set(QEMU_DEFAULT_GPU ${QEMU_STD_VGA})
  endif()
  add_custom_target(run COMMAND ${QEMU_COMMON} ${QEMU_DEFAULT_GPU}
    DEPENDS esp vars primary USES_TERMINAL)
  add_custom_target(run-std COMMAND ${QEMU_COMMON} ${QEMU_STD_VGA}
    DEPENDS esp vars primary USES_TERMINAL)
  add_custom_target(run-gl COMMAND ${QEMU_COMMON} ${QEMU_GL_GPU}
    DEPENDS esp vars primary USES_TERMINAL)
  # Starts the gdbstub on :2159 without halting; use with TCOS_DEBUG_WAIT.
  add_custom_target(debug COMMAND ${QEMU_COMMON} ${QEMU_DEFAULT_GPU} -gdb tcp:127.0.0.1:2159
    DEPENDS esp vars primary USES_TERMINAL)
  # Freeze the CPU at reset until gdb connects (-S).
  add_custom_target(debug-halted COMMAND ${QEMU_COMMON} ${QEMU_DEFAULT_GPU} -gdb tcp:127.0.0.1:2159 -S
    DEPENDS esp vars primary USES_TERMINAL)
else()
  message(WARNING "qemu-system-x86_64 or OVMF firmware not found; run/debug targets disabled")
endif()
