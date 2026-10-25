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
  src/Hardware/CentralProcessingUnit/CpuTemperature.cpp
  src/Hardware/CentralProcessingUnit/X86_64Cpu.S
  src/Hardware/IoPort.cpp
  src/Hardware/RandomAccessMemory/UefiRandomAccessMemory.cpp
  src/Hardware/TextConsole.cpp
  src/Hardware/Acpi/AcpiTables.cpp
  src/Hardware/Acpi/AmlInterpreter.cpp
  src/Hardware/Acpi/EmbeddedController.cpp
  src/Hardware/Acpi/SystemAcpiHost.cpp
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
  src/Browser/DomBindingsServiceWorker.cpp
  src/Browser/CacheStore.cpp
  src/Browser/ServiceWorker.cpp
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
  src/Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.cpp
  src/Hardware/GraphicsProcessingUnit/AMD/AmdGpuGenerations.cpp
  src/Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaGpuGenerations.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelGtt.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelLrc.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelExeclist.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelForcewake.cpp
  src/Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.cpp
  src/Hardware/GraphicsProcessingUnit/QEMU/QemuStdVgaGpu.cpp
  src/Hardware/GraphicsProcessingUnit/Virtio/VirtioGpu.cpp
  src/Hardware/GraphicsProcessingUnit/VMware/VmwareSvgaGpu.cpp
  src/Graphics/Graphics2D/Software/SoftwareRenderer2D.cpp
  src/Graphics/Graphics2D/Device/DeviceRenderer2D.cpp
  src/Graphics/Graphics3D/BuiltinShaders.cpp
  src/Graphics/Graphics3D/Virgl/VirglDevice.cpp
  src/Graphics/Graphics3D/Svga/SvgaDevice.cpp
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
  src/Desktop/PictureInPicture.cpp
  src/Desktop/BrowserStore.cpp
  src/Desktop/SessionStore.cpp
  src/Desktop/DesktopSession.cpp
  src/Desktop/Paint.cpp
  src/Desktop/MediaPlayer.cpp
  src/Desktop/Graphics3DTest.cpp
  src/SystemMonitor.cpp
  src/SystemInventory.cpp
  src/SystemNetworkDetails.cpp
  src/Desktop/NetworkProperties.cpp
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
  # Microcode the OS loads from /firmware (see firmware/README.md; AMD's license files travel with it)
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/firmware ${ESP_DIR}/firmware
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
    ${QEMU} -machine q35 -smp 4 -m 8G -netdev user,id=net0 -device virtio-net-pci,netdev=net0
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
    # A persistent writable FAT32 disk (the `data` target creates it); it appears in the file explorer as "DATA".
    -drive if=none,id=data,format=raw,file=${CMAKE_BINARY_DIR}/data.img -device virtio-blk-pci,drive=data
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

  find_program(QEMU_IMG qemu-img)
  find_program(MKFS_FAT mkfs.fat)
  if(QEMU_IMG AND MKFS_FAT)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/data.img
      COMMAND ${QEMU_IMG} create -f raw ${CMAKE_BINARY_DIR}/data.img 256M
      COMMAND ${MKFS_FAT} -F 32 -n DATA ${CMAKE_BINARY_DIR}/data.img)
  else()
    message(WARNING "qemu-img or mkfs.fat not found: no data disk for the run targets")
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/data.img COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/data.img)
  endif()
  add_custom_target(data DEPENDS ${CMAKE_BINARY_DIR}/data.img)

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
    DEPENDS esp vars data USES_TERMINAL)
  add_custom_target(run-std COMMAND ${QEMU_COMMON} ${QEMU_STD_VGA}
    DEPENDS esp vars data USES_TERMINAL)
  add_custom_target(run-gl COMMAND ${QEMU_COMMON} ${QEMU_GL_GPU}
    DEPENDS esp vars data USES_TERMINAL)
  # Starts the gdbstub on :2159 without halting; use with TCOS_DEBUG_WAIT.
  add_custom_target(debug COMMAND ${QEMU_COMMON} ${QEMU_DEFAULT_GPU} -gdb tcp:127.0.0.1:2159
    DEPENDS esp vars data USES_TERMINAL)
  # Freeze the CPU at reset until gdb connects (-S).
  add_custom_target(debug-halted COMMAND ${QEMU_COMMON} ${QEMU_DEFAULT_GPU} -gdb tcp:127.0.0.1:2159 -S
    DEPENDS esp vars data USES_TERMINAL)
else()
  message(WARNING "qemu-system-x86_64 or OVMF firmware not found; run/debug targets disabled")
endif()
