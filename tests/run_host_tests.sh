#!/usr/bin/env bash
# Builds and runs the host-side unit tests (the freestanding code compiled for
# the development machine, with the address and undefined-behaviour sanitizers).
# Usage: tests/run_host_tests.sh [test-name...]   (default: all)
set -euo pipefail
cd "$(dirname "$0")/.."
out=${TMPDIR:-/tmp}/tcos-host-tests
mkdir -p "$out"
cxx=${CXX:-g++}
flags="-std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -I src -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -Wno-unused-variable"

# The GUI has player controls, which bring the audio player (and with it the mixer), the video player and the codecs along.
media="src/Media/*.cpp src/Media/Audio/*.cpp src/Media/Image/*.cpp src/Midi/MidiFile.cpp src/Midi/MidiStream.cpp src/Midi/SoftwareSynth.cpp"
gui_audio="src/Hardware/Monitor/*.cpp src/Hardware/Printer/PrinterTypes.cpp src/Hardware/Printer/TextLayout.cpp src/Hardware/Printer/Halftone.cpp src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp src/Audio/AudioPlayer.cpp src/Video/*.cpp src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Software/*.cpp $media"

build_and_run() {
  local name=$1; shift
  echo "== $name"
  $cxx $flags "$@" -o "$out/$name"
  "$out/$name"
}

want() { [ $# -eq 0 ] && return 0; for t in "${WANTED[@]}"; do [ "$t" = "$1" ] && return 0; done; return 1; }
WANTED=("$@")
if [ ${#WANTED[@]} -eq 0 ]; then WANTED=(ArrayViewTest AmlTest SheetTest DatabaseTest DatabaseNetworkTest SystemDatabaseTest StringTest ClipboardTest AllocatorTest PagingTest MmioTest AudioTest AudioMixerTest AudioCaptureTest AudioConvertTest UsbAudioTest AudioHotplugTest AudioHotplugThreadTest UvcTest CameraHotplugTest CameraHotplugThreadTest AudioPlayerTest VideoCodecTest VideoPlayerTest MediaTest StreamingTest MidiTest UsbMidiTest MonitorTest MidiTest CryptoTest X509Test TlsReplayTest SnmpTest PrinterTest PrinterHotplugTest PrinterHotplugThreadTest PartitionTest FileSystemTest StorageHotplugTest StorageEjectTest StorageHotplugThreadTest InputHotplugTest InputHotplugThreadTest StreamTest AsyncTest ParserFuzzTest NetworkTest HttpTest SmbTest NfsTest SshFtpTest GraphicsTest GuiTest GuiControlsTest GuiViewsTest GuiThreadTest); fi
for t in "${WANTED[@]}"; do
  case $t in
    ArrayViewTest) build_and_run ArrayViewTest tests/ArrayViewTest.cpp ;;
    DatabaseTest) build_and_run DatabaseTest tests/DatabaseTest.cpp tests/KernelStubs.cpp src/Database/*.cpp src/Database/Postgres/*.cpp src/Crypto/*.cpp src/Stream/StreamStatus.cpp ;;
    SystemDatabaseTest) build_and_run SystemDatabaseTest tests/SystemDatabaseTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Database/*.cpp src/Database/Local/*.cpp \
                          src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp ;;
    DatabaseNetworkTest) build_and_run DatabaseNetworkTest tests/DatabaseNetworkTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Database/*.cpp src/Database/Postgres/*.cpp src/Database/Net/*.cpp \
                           src/Stream/StreamStatus.cpp src/Async/Executor.cpp src/Stream/Tcp/TcpStream.cpp src/FileSystem/FileSystemTypes.cpp src/Network/ServerNetwork.cpp src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp \
                           src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp src/Network/Tls/*.cpp src/Crypto/*.cpp ;;
    SheetTest) build_and_run SheetTest tests/SheetTest.cpp tests/KernelStubs.cpp src/Spreadsheet/Sheet.cpp src/Math/FloatMath.cpp ;;
    AmlTest) build_and_run AmlTest tests/AmlTest.cpp tests/KernelStubs.cpp src/Hardware/Acpi/AmlInterpreter.cpp src/Hardware/Acpi/EmbeddedController.cpp src/Hardware/Acpi/AcpiTables.cpp src/Hardware/Battery/AcpiBattery.cpp ;;
    ClipboardTest) build_and_run ClipboardTest tests/ClipboardTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Kernel/Clipboard.cpp ;;
    StringTest) build_and_run StringTest tests/StringTest.cpp ;;
    AudioTest) build_and_run AudioTest tests/AudioTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                 src/Hardware/AudioDevice/PlaybackRing.cpp src/Hardware/AudioDevice/CaptureRing.cpp src/Hardware/AudioDevice/Software/*.cpp ;;
    AudioMixerTest) build_and_run AudioMixerTest tests/AudioMixerTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                      src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp ;;
    AudioCaptureTest) build_and_run AudioCaptureTest tests/AudioCaptureTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                        src/Hardware/AudioDevice/PlaybackRing.cpp src/Hardware/AudioDevice/CaptureRing.cpp \
                        src/Hardware/AudioDevice/Software/*.cpp src/Audio/Resampler.cpp src/Audio/CaptureStream.cpp src/Audio/AudioCaptureHub.cpp ;;
    UsbAudioTest) build_and_run UsbAudioTest tests/UsbAudioTest.cpp tests/UsbAudioStubs.cpp tests/KernelStubs.cpp src/Hardware/Bus/Virtio/VirtioPciDevice.cpp src/Hardware/Bus/Virtio/Virtqueue.cpp src/Kernel/Sync.cpp src/Kernel/Paging.cpp src/Hardware/MmioMapper.cpp src/Hardware/Bus/Pci/PciBar.cpp \
                    src/Hardware/AudioDevice/PlaybackRing.cpp src/Hardware/AudioDevice/CaptureRing.cpp src/Hardware/AudioDevice/SystemAudioDevices.cpp \
                    src/Hardware/AudioDevice/Software/*.cpp src/Hardware/AudioDevice/Intel/*.cpp src/Hardware/AudioDevice/Virtio/*.cpp src/Hardware/AudioDevice/Usb/*.cpp ;;
    AudioHotplugTest) build_and_run AudioHotplugTest tests/AudioHotplugTest.cpp tests/UsbAudioStubs.cpp tests/KernelStubs.cpp src/Hardware/Bus/Virtio/VirtioPciDevice.cpp src/Hardware/Bus/Virtio/Virtqueue.cpp src/Kernel/Sync.cpp src/Kernel/Paging.cpp src/Hardware/MmioMapper.cpp src/Hardware/Bus/Pci/PciBar.cpp \
                        src/Hardware/AudioDevice/PlaybackRing.cpp src/Hardware/AudioDevice/CaptureRing.cpp src/Hardware/AudioDevice/SystemAudioDevices.cpp \
                        src/Hardware/AudioDevice/Software/*.cpp src/Hardware/AudioDevice/Intel/*.cpp src/Hardware/AudioDevice/Virtio/*.cpp src/Hardware/AudioDevice/Usb/*.cpp \
                        src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp src/Audio/CaptureStream.cpp src/Audio/AudioCaptureHub.cpp src/Audio/AudioRouter.cpp ;;
    UvcTest) build_and_run UvcTest tests/UvcTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Hardware/Camera/SystemCameras.cpp src/Hardware/Camera/Usb/*.cpp \
               src/Hardware/VideoCodec/ColorConvert.cpp src/Hardware/VideoCodec/VideoFrameBuffer.cpp src/Hardware/VideoCodec/VideoTypes.cpp \
               src/Hardware/VideoCodec/Software/JpegDecoder.cpp src/Hardware/VideoCodec/Software/JpegEncoder.cpp ;;
    CameraHotplugTest) build_and_run CameraHotplugTest tests/CameraHotplugTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Hardware/Camera/SystemCameras.cpp src/Hardware/Camera/Usb/*.cpp \
                         src/Hardware/VideoCodec/ColorConvert.cpp src/Hardware/VideoCodec/VideoFrameBuffer.cpp src/Hardware/VideoCodec/VideoTypes.cpp \
                         src/Hardware/VideoCodec/Software/JpegDecoder.cpp src/Hardware/VideoCodec/Software/JpegEncoder.cpp ;;
    CameraHotplugThreadTest)  # real threads, checked with ThreadSanitizer
      echo "== CameraHotplugThreadTest"
      $cxx -std=c++20 -O1 -g -fsanitize=thread -pthread -I src -I tests -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-misleading-indentation -Wno-delete-non-virtual-dtor \
        -o "$out/CameraHotplugThreadTest" tests/CameraHotplugThreadTest.cpp tests/HostThreads.cpp src/Hardware/Camera/SystemCameras.cpp src/Hardware/Camera/Usb/*.cpp \
        src/Hardware/VideoCodec/ColorConvert.cpp src/Hardware/VideoCodec/VideoFrameBuffer.cpp src/Hardware/VideoCodec/VideoTypes.cpp \
        src/Hardware/VideoCodec/Software/JpegDecoder.cpp src/Hardware/VideoCodec/Software/JpegEncoder.cpp
      "$out/CameraHotplugThreadTest" ;;
    AudioConvertTest) build_and_run AudioConvertTest tests/AudioConvertTest.cpp src/Audio/Resampler.cpp ;;
    AudioPlayerTest) build_and_run AudioPlayerTest tests/AudioPlayerTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                       src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp src/Audio/WavFile.cpp src/Audio/AudioPlayer.cpp ;;
    VideoCodecTest) build_and_run VideoCodecTest tests/VideoCodecTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                      src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Software/*.cpp ;;
    VideoPlayerTest) build_and_run VideoPlayerTest tests/VideoPlayerTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                       src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Video/*.cpp src/Math/FloatMath.cpp \
                       src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp src/Audio/AudioPlayer.cpp $media ;;
    MediaTest) build_and_run MediaTest tests/MediaTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp $media \
                 src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Video/*.cpp src/Math/FloatMath.cpp \
                 src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp src/Audio/AudioPlayer.cpp ;;
    MidiTest) build_and_run MidiTest tests/MidiTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp $media \
                src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Math/FloatMath.cpp src/Hardware/MidiDevice/Mpu401.cpp ;;
    UsbMidiTest) build_and_run UsbMidiTest tests/UsbMidiTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Midi/MidiFile.cpp src/Midi/MidiStream.cpp src/Midi/SoftwareSynth.cpp \
                   src/Midi/LiveSynth.cpp src/Math/FloatMath.cpp src/Hardware/MidiDevice/Usb/*.cpp src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp \
                   src/Audio/AudioPlayer.cpp ;;
    MonitorTest) build_and_run MonitorTest tests/MonitorTest.cpp src/Hardware/Monitor/*.cpp ;;
    StreamingTest) build_and_run StreamingTest tests/StreamingTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp $media \
                     src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Math/FloatMath.cpp ;;
    PartitionTest) build_and_run PartitionTest tests/PartitionTest.cpp tests/KernelStubs.cpp src/Hardware/StorageDevice/Partition/PartitionScanner.cpp ;;
    FileSystemTest) build_and_run FileSystemTest tests/FileSystemTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                      src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/FileSystem/Fat/*.cpp ;;
    StorageHotplugTest) build_and_run StorageHotplugTest tests/StorageHotplugTest.cpp tests/UsbAudioStubs.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Kernel/Paging.cpp \
                          src/Hardware/MmioMapper.cpp src/Hardware/Bus/Pci/PciBar.cpp src/Hardware/Bus/Virtio/VirtioPciDevice.cpp src/Hardware/Bus/Virtio/Virtqueue.cpp \
                          src/Hardware/StorageDevice/StorageDeviceBase.cpp src/Hardware/StorageDevice/SystemStorageDevices.cpp src/Hardware/StorageDevice/*/*.cpp \
                          src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/FileSystem/Fat/*.cpp src/FileSystem/Mount/*.cpp src/SystemFileSystem.cpp ;;
    InputHotplugTest) build_and_run InputHotplugTest tests/InputHotplugTest.cpp tests/UsbAudioStubs.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Kernel/Paging.cpp \
                        src/Hardware/MmioMapper.cpp src/Hardware/Bus/Pci/PciBar.cpp src/Hardware/Bus/Virtio/VirtioPciDevice.cpp src/Hardware/Bus/Virtio/Virtqueue.cpp src/Hardware/Bus/Virtio/VirtioInputDevice.cpp \
                        src/Hardware/SystemInput.cpp src/Hardware/Bus/Ps2/*.cpp src/Hardware/Keyboard/*.cpp src/Hardware/Keyboard/*/*.cpp src/Hardware/Mouse/*.cpp src/Hardware/Mouse/*/*.cpp \
                        src/Hardware/Bus/Usb/Hid/*.cpp \
                        src/Hardware/StorageDevice/StorageDeviceBase.cpp src/Hardware/StorageDevice/SystemStorageDevices.cpp src/Hardware/StorageDevice/*/*.cpp \
                        src/FileSystem/FileSystemTypes.cpp ;;
    StreamTest) build_and_run StreamTest tests/StreamTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Stream/StreamStatus.cpp src/Async/Executor.cpp \
                  src/Stream/Memory/*.cpp src/Stream/File/*.cpp src/Stream/Tcp/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp \
                  src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp \
                  src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp ;;
    AsyncTest) build_and_run AsyncTest tests/AsyncTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Stream/StreamStatus.cpp src/Async/Executor.cpp \
                 src/Stream/Memory/*.cpp src/Stream/File/*.cpp src/Stream/Tcp/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp \
                 src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp \
                 src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp ;;
    ParserFuzzTest) build_and_run ParserFuzzTest tests/ParserFuzzTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                      src/Hardware/Bus/Usb/Hid/HidReportParser.cpp src/Hardware/Bus/Usb/Hid/UsbHidInterface.cpp ;;
    NetworkTest) build_and_run NetworkTest tests/NetworkTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                   src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp \
                   src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp ;;
    HttpTest) build_and_run HttpTest tests/HttpTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp \
                src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp \
                src/Network/Http/*.cpp src/Network/Tls/*.cpp src/Crypto/*.cpp src/Media/Zlib.cpp ;;
    SmbTest) build_and_run SmbTest tests/SmbTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/FileSystem/FileSystemTypes.cpp src/Network/Smb/*.cpp src/Network/ServerNetwork.cpp \
                src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp \
                src/Crypto/*.cpp ;;
    NfsTest) build_and_run NfsTest tests/NfsTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/FileSystem/FileSystemTypes.cpp src/Network/Nfs/*.cpp src/Network/ServerNetwork.cpp \
                src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp ;;
    SshFtpTest) build_and_run SshFtpTest tests/SshFtpTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/FileSystem/FileSystemTypes.cpp src/Network/Ssh/*.cpp src/Network/Sftp/*.cpp src/Network/Ftp/*.cpp \
                src/Network/ServerNetwork.cpp src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp \
                src/Network/Dns/*.cpp src/Network/Tls/*.cpp src/Crypto/*.cpp ;;
    CryptoTest) build_and_run CryptoTest tests/CryptoTest.cpp tests/KernelStubs.cpp src/Crypto/*.cpp ;;
    # (without the address sanitizer: this test maps memory above 4 GiB, which the sanitizer reserves for its shadow)
    AllocatorTest) echo "== AllocatorTest"; $cxx ${flags/address,undefined/undefined} tests/AllocatorTest.cpp tests/KernelStubs.cpp src/Allocators/Allocator.cpp -o "$out/AllocatorTest" && "$out/AllocatorTest" ;;
    PagingTest) build_and_run PagingTest tests/PagingTest.cpp tests/KernelStubs.cpp src/Kernel/Paging.cpp ;;
    MmioTest) build_and_run MmioTest tests/MmioTest.cpp tests/KernelStubs.cpp src/Kernel/Paging.cpp src/Hardware/MmioMapper.cpp src/Hardware/Bus/Pci/PciBar.cpp ;;
    X509Test) build_and_run X509Test tests/X509Test.cpp tests/KernelStubs.cpp src/Crypto/*.cpp ;;
    TlsReplayTest) build_and_run TlsReplayTest tests/TlsReplayTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Network/Tls/*.cpp src/Crypto/*.cpp ;;
    SnmpTest) build_and_run SnmpTest tests/SnmpTest.cpp tests/KernelStubs.cpp src/Crypto/*.cpp src/Network/Snmp/SnmpMessage.cpp src/Network/Snmp/SnmpUsm.cpp ;;
    PrinterTest) build_and_run PrinterTest tests/PrinterTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/SystemPrintService.cpp src/Hardware/Printer/*.cpp src/Hardware/Printer/Brother/*.cpp src/Hardware/Printer/Usb/*.cpp src/Network/Snmp/*.cpp src/Network/Ipp/*.cpp src/Network/Tls/*.cpp src/Network/Mdns/*.cpp src/Crypto/*.cpp \
                   src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp \
                   src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp ;;
    PrinterHotplugTest) build_and_run PrinterHotplugTest tests/PrinterHotplugTest.cpp tests/UsbAudioStubs.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Hardware/Printer/*.cpp src/Hardware/Printer/Brother/*.cpp src/Hardware/Printer/Usb/*.cpp src/Network/Snmp/*.cpp src/Network/Ipp/*.cpp src/Network/Tls/*.cpp src/Network/Mdns/*.cpp src/Crypto/*.cpp \
                          src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp \
                          src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp ;;
    GraphicsTest) build_and_run GraphicsTest tests/GraphicsTest.cpp tests/KernelStubs.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp \
                    src/Graphics/Graphics2D/Software/*.cpp src/Graphics/Graphics2D/Device/*.cpp src/Graphics/Graphics3D/BuiltinShaders.cpp src/Graphics/Graphics3D/Software/*.cpp ;;
    GuiTest) build_and_run GuiTest tests/GuiTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Kernel/Clipboard.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp \
                src/Hardware/Keyboard/KeyCodes.cpp src/Graphics/Graphics2D/Software/*.cpp src/Gui/*.cpp src/Gui/Controls/*.cpp src/Gui/Windows/*.cpp src/Desktop/*.cpp src/Spreadsheet/*.cpp src/Database/*.cpp src/Database/Local/*.cpp src/Graphics/Graphics3D/BuiltinShaders.cpp tests/DesktopStubs.cpp $gui_audio src/FileSystem/FileSystemTypes.cpp ;;
    GuiControlsTest) build_and_run GuiControlsTest tests/GuiControlsTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Kernel/Clipboard.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp \
                src/Hardware/Keyboard/KeyCodes.cpp src/Graphics/Graphics2D/Software/*.cpp src/Gui/*.cpp src/Gui/Controls/*.cpp src/Gui/Windows/*.cpp src/Desktop/*.cpp src/Spreadsheet/*.cpp src/Database/*.cpp src/Database/Local/*.cpp src/Graphics/Graphics3D/BuiltinShaders.cpp tests/DesktopStubs.cpp $gui_audio src/FileSystem/FileSystemTypes.cpp ;;
    GuiViewsTest) build_and_run GuiViewsTest tests/GuiViewsTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Kernel/Clipboard.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp \
                src/Hardware/Keyboard/KeyCodes.cpp src/Graphics/Graphics2D/Software/*.cpp src/Gui/*.cpp src/Gui/Controls/*.cpp src/Gui/Windows/*.cpp src/Desktop/*.cpp src/Spreadsheet/*.cpp src/Database/*.cpp src/Database/Local/*.cpp src/Graphics/Graphics3D/BuiltinShaders.cpp tests/DesktopStubs.cpp $gui_audio ;;
    StorageEjectTest) build_and_run StorageEjectTest tests/StorageEjectTest.cpp tests/UsbAudioStubs.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Kernel/Paging.cpp \
                        src/Hardware/MmioMapper.cpp src/Hardware/Bus/Pci/PciBar.cpp src/Hardware/Bus/Virtio/VirtioPciDevice.cpp src/Hardware/Bus/Virtio/Virtqueue.cpp \
                        src/Hardware/StorageDevice/StorageDeviceBase.cpp src/Hardware/StorageDevice/SystemStorageDevices.cpp src/Hardware/StorageDevice/*/*.cpp \
                        src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/FileSystem/Fat/*.cpp src/FileSystem/Mount/*.cpp src/SystemFileSystem.cpp ;;
    StorageHotplugThreadTest)  # real threads, checked with ThreadSanitizer
      echo "== StorageHotplugThreadTest"
      $cxx -std=c++20 -O1 -g -fsanitize=thread -pthread -I src -I tests -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-misleading-indentation -Wno-delete-non-virtual-dtor \
        -o "$out/StorageHotplugThreadTest" tests/StorageHotplugThreadTest.cpp tests/HostThreads.cpp tests/UsbAudioStubs.cpp src/Kernel/Paging.cpp \
        src/Hardware/MmioMapper.cpp src/Hardware/Bus/Pci/PciBar.cpp src/Hardware/Bus/Virtio/VirtioPciDevice.cpp src/Hardware/Bus/Virtio/Virtqueue.cpp \
        src/Hardware/StorageDevice/StorageDeviceBase.cpp src/Hardware/StorageDevice/SystemStorageDevices.cpp src/Hardware/StorageDevice/*/*.cpp \
        src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/FileSystem/Fat/*.cpp src/FileSystem/Mount/*.cpp src/SystemFileSystem.cpp
      "$out/StorageHotplugThreadTest" ;;
    InputHotplugThreadTest)  # real threads, checked with ThreadSanitizer
      echo "== InputHotplugThreadTest"
      $cxx -std=c++20 -O1 -g -fsanitize=thread -pthread -I src -I tests -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-misleading-indentation -Wno-delete-non-virtual-dtor \
        -o "$out/InputHotplugThreadTest" tests/InputHotplugThreadTest.cpp tests/HostThreads.cpp tests/UsbAudioStubs.cpp src/Kernel/Paging.cpp \
        src/Hardware/MmioMapper.cpp src/Hardware/Bus/Pci/PciBar.cpp src/Hardware/Bus/Virtio/VirtioPciDevice.cpp src/Hardware/Bus/Virtio/Virtqueue.cpp src/Hardware/Bus/Virtio/VirtioInputDevice.cpp \
        src/Hardware/SystemInput.cpp src/Hardware/Bus/Ps2/*.cpp src/Hardware/Keyboard/*.cpp src/Hardware/Keyboard/*/*.cpp src/Hardware/Mouse/*.cpp src/Hardware/Mouse/*/*.cpp \
        src/Hardware/Bus/Usb/Hid/*.cpp
      "$out/InputHotplugThreadTest" ;;
    AudioHotplugThreadTest)  # real threads, checked with ThreadSanitizer
      echo "== AudioHotplugThreadTest"
      $cxx -std=c++20 -O1 -g -fsanitize=thread -pthread -I src -I tests -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-misleading-indentation -Wno-delete-non-virtual-dtor \
        -o "$out/AudioHotplugThreadTest" tests/AudioHotplugThreadTest.cpp tests/HostThreads.cpp tests/UsbAudioStubs.cpp src/Kernel/Paging.cpp \
        src/Hardware/Bus/Virtio/VirtioPciDevice.cpp src/Hardware/Bus/Virtio/Virtqueue.cpp src/Hardware/MmioMapper.cpp src/Hardware/Bus/Pci/PciBar.cpp \
        src/Hardware/AudioDevice/PlaybackRing.cpp src/Hardware/AudioDevice/CaptureRing.cpp src/Hardware/AudioDevice/SystemAudioDevices.cpp \
        src/Hardware/AudioDevice/Software/*.cpp src/Hardware/AudioDevice/Intel/*.cpp src/Hardware/AudioDevice/Virtio/*.cpp src/Hardware/AudioDevice/Usb/*.cpp \
        src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp src/Audio/CaptureStream.cpp src/Audio/AudioCaptureHub.cpp src/Audio/AudioRouter.cpp
      "$out/AudioHotplugThreadTest" ;;
    PrinterHotplugThreadTest)  # real threads, checked with ThreadSanitizer
      echo "== PrinterHotplugThreadTest"
      $cxx -std=c++20 -O1 -g -fsanitize=thread -pthread -I src -I tests -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-misleading-indentation -Wno-delete-non-virtual-dtor \
        -o "$out/PrinterHotplugThreadTest" tests/PrinterHotplugThreadTest.cpp tests/HostThreads.cpp tests/UsbAudioStubs.cpp \
        src/Hardware/Printer/*.cpp src/Hardware/Printer/Brother/*.cpp src/Hardware/Printer/Usb/*.cpp src/Network/Snmp/*.cpp src/Network/Ipp/*.cpp src/Network/Tls/*.cpp src/Network/Mdns/*.cpp src/Crypto/*.cpp \
        src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp
      "$out/PrinterHotplugThreadTest" ;;
    GuiThreadTest)  # real threads, checked with ThreadSanitizer (cannot be combined with the address sanitizer)
      echo "== GuiThreadTest"
      $cxx -std=c++20 -O1 -g -fsanitize=thread -pthread -I src -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-misleading-indentation -Wno-delete-non-virtual-dtor \
        -o "$out/GuiThreadTest" tests/GuiThreadTest.cpp tests/HostThreads.cpp src/Kernel/Clipboard.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp \
        src/Hardware/Keyboard/KeyCodes.cpp src/Graphics/Graphics2D/Software/*.cpp src/Gui/*.cpp src/Gui/Controls/*.cpp src/Gui/Windows/*.cpp src/Desktop/*.cpp src/Spreadsheet/*.cpp src/Database/*.cpp src/Database/Local/*.cpp src/Graphics/Graphics3D/BuiltinShaders.cpp tests/DesktopStubs.cpp $gui_audio src/FileSystem/FileSystemTypes.cpp
      "$out/GuiThreadTest" ;;
    *) echo "unknown test $t"; exit 2 ;;
  esac
done
