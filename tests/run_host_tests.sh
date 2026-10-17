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
media="src/Media/*.cpp src/Media/Audio/*.cpp src/Media/Image/*.cpp"
gui_audio="src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp src/Audio/AudioPlayer.cpp src/Video/*.cpp src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Software/*.cpp $media"

build_and_run() {
  local name=$1; shift
  echo "== $name"
  $cxx $flags "$@" -o "$out/$name"
  "$out/$name"
}

want() { [ $# -eq 0 ] && return 0; for t in "${WANTED[@]}"; do [ "$t" = "$1" ] && return 0; done; return 1; }
WANTED=("$@")
if [ ${#WANTED[@]} -eq 0 ]; then WANTED=(ArrayViewTest StringTest AudioTest AudioMixerTest AudioCaptureTest AudioConvertTest AudioPlayerTest VideoCodecTest VideoPlayerTest MediaTest CryptoTest X509Test TlsReplayTest SnmpTest PrinterTest PartitionTest FileSystemTest StreamTest AsyncTest ParserFuzzTest NetworkTest GraphicsTest GuiTest GuiControlsTest GuiViewsTest GuiThreadTest); fi
for t in "${WANTED[@]}"; do
  case $t in
    ArrayViewTest) build_and_run ArrayViewTest tests/ArrayViewTest.cpp ;;
    StringTest) build_and_run StringTest tests/StringTest.cpp ;;
    AudioTest) build_and_run AudioTest tests/AudioTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                 src/Hardware/AudioDevice/PlaybackRing.cpp src/Hardware/AudioDevice/CaptureRing.cpp src/Hardware/AudioDevice/Software/*.cpp ;;
    AudioMixerTest) build_and_run AudioMixerTest tests/AudioMixerTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                      src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp ;;
    AudioCaptureTest) build_and_run AudioCaptureTest tests/AudioCaptureTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                        src/Hardware/AudioDevice/PlaybackRing.cpp src/Hardware/AudioDevice/CaptureRing.cpp \
                        src/Hardware/AudioDevice/Software/*.cpp src/Audio/Resampler.cpp src/Audio/CaptureStream.cpp src/Audio/AudioCaptureHub.cpp ;;
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
    PartitionTest) build_and_run PartitionTest tests/PartitionTest.cpp tests/KernelStubs.cpp src/Hardware/StorageDevice/Partition/PartitionScanner.cpp ;;
    FileSystemTest) build_and_run FileSystemTest tests/FileSystemTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                      src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/FileSystem/Fat/*.cpp ;;
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
    CryptoTest) build_and_run CryptoTest tests/CryptoTest.cpp tests/KernelStubs.cpp src/Crypto/*.cpp ;;
    X509Test) build_and_run X509Test tests/X509Test.cpp tests/KernelStubs.cpp src/Crypto/*.cpp ;;
    TlsReplayTest) build_and_run TlsReplayTest tests/TlsReplayTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Network/Tls/*.cpp src/Crypto/*.cpp ;;
    SnmpTest) build_and_run SnmpTest tests/SnmpTest.cpp tests/KernelStubs.cpp src/Crypto/*.cpp src/Network/Snmp/SnmpMessage.cpp src/Network/Snmp/SnmpUsm.cpp ;;
    PrinterTest) build_and_run PrinterTest tests/PrinterTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Hardware/Printer/*.cpp src/Hardware/Printer/Brother/*.cpp src/Hardware/Printer/Usb/*.cpp src/Network/Snmp/*.cpp src/Network/Ipp/*.cpp src/Network/Tls/*.cpp src/Network/Mdns/*.cpp src/Crypto/*.cpp \
                   src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp \
                   src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp ;;
    GraphicsTest) build_and_run GraphicsTest tests/GraphicsTest.cpp tests/KernelStubs.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp \
                    src/Graphics/Graphics2D/Software/*.cpp src/Graphics/Graphics2D/Device/*.cpp src/Graphics/Graphics3D/BuiltinShaders.cpp src/Graphics/Graphics3D/Software/*.cpp ;;
    GuiTest) build_and_run GuiTest tests/GuiTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp \
                src/Hardware/Keyboard/KeyCodes.cpp src/Graphics/Graphics2D/Software/*.cpp src/Gui/*.cpp src/Gui/Controls/*.cpp src/Gui/Windows/*.cpp src/Desktop/*.cpp $gui_audio ;;
    GuiControlsTest) build_and_run GuiControlsTest tests/GuiControlsTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp \
                src/Hardware/Keyboard/KeyCodes.cpp src/Graphics/Graphics2D/Software/*.cpp src/Gui/*.cpp src/Gui/Controls/*.cpp src/Gui/Windows/*.cpp src/Desktop/*.cpp $gui_audio ;;
    GuiViewsTest) build_and_run GuiViewsTest tests/GuiViewsTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp \
                src/Hardware/Keyboard/KeyCodes.cpp src/Graphics/Graphics2D/Software/*.cpp src/Gui/*.cpp src/Gui/Controls/*.cpp src/Gui/Windows/*.cpp src/Desktop/*.cpp $gui_audio ;;
    GuiThreadTest)  # real threads, checked with ThreadSanitizer (cannot be combined with the address sanitizer)
      echo "== GuiThreadTest"
      $cxx -std=c++20 -O1 -g -fsanitize=thread -pthread -I src -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-misleading-indentation -Wno-delete-non-virtual-dtor \
        -o "$out/GuiThreadTest" tests/GuiThreadTest.cpp tests/HostThreads.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp \
        src/Hardware/Keyboard/KeyCodes.cpp src/Graphics/Graphics2D/Software/*.cpp src/Gui/*.cpp src/Gui/Controls/*.cpp src/Gui/Windows/*.cpp src/Desktop/*.cpp $gui_audio
      "$out/GuiThreadTest" ;;
    *) echo "unknown test $t"; exit 2 ;;
  esac
done
