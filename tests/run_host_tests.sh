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
media="src/Crypto/*.cpp src/Media/*.cpp src/Media/Audio/*.cpp src/Media/Image/*.cpp src/Midi/MidiFile.cpp src/Midi/MidiStream.cpp src/Midi/SoftwareSynth.cpp"
gui_audio="src/Hardware/Monitor/*.cpp src/Hardware/Printer/PrinterTypes.cpp src/Hardware/Printer/TextLayout.cpp src/Hardware/Printer/Halftone.cpp src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp src/Audio/AudioCaptureHub.cpp src/Audio/CaptureStream.cpp src/Hardware/AudioDevice/CaptureRing.cpp src/Audio/AudioPlayer.cpp src/Video/*.cpp src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/Intel/*.cpp src/Hardware/VideoCodec/Software/*.cpp $media"

build_and_run() {
  local name=$1; shift
  echo "== $name"
  $cxx $flags "$@" -o "$out/$name"
  "$out/$name"
}

want() { [ $# -eq 0 ] && return 0; for t in "${WANTED[@]}"; do [ "$t" = "$1" ] && return 0; done; return 1; }
WANTED=("$@")
if [ ${#WANTED[@]} -eq 0 ]; then WANTED=(ArrayViewTest ShaderTest GlTest GpuTest WasmTest RtcTest AmlTest SheetTest DomTest CanvasTest BrowserTest CssTest UrlTest CookieTest BrowserStoreTest FetcherTest PrefetchTest HistoryTest PageTest LayoutUnitTest PixelGoldenTest LayoutTest FlexTest GridTest TableTest PaintTest ScriptTest DatabaseTest DatabaseNetworkTest SystemDatabaseTest StringTest RegexTest ClipboardTest AllocatorTest PagingTest MmioTest AudioTest AudioMixerTest AudioCaptureTest AudioConvertTest UsbAudioTest AudioHotplugTest AudioHotplugThreadTest UvcTest CameraHotplugTest CameraHotplugThreadTest AudioPlayerTest VideoCodecTest VideoPlayerTest MediaTest WebmTest WebVttTest ExecutableTest SyscallTest LoaderTest ProcessFilesTest DashTest MseTest StreamingTest MidiTest UsbMidiTest MonitorTest MidiTest CryptoTest X509Test TlsReplayTest SnmpTest PrinterTest PrinterHotplugTest PrinterHotplugThreadTest PartitionTest FileSystemTest BefsTest NtfsTest ExtTest StorageHotplugTest StorageEjectTest StorageHotplugThreadTest InputHotplugTest InputHotplugThreadTest StreamTest AsyncTest ParserFuzzTest NetworkTest HttpTest WebSocketTest QuicTest Http3Test SmbTest NfsTest SshFtpTest GraphicsTest GuiTest GuiControlsTest GuiViewsTest GuiThreadTest FontTest IntelGenerationTest AmdGenerationTest AmdDiscoveryTest AmdVcnFirmwareTest AmdVcn1Test AmdRavenMemoryTest AmdVcnDecodeMessagesTest AmdVcn1RingTest AmdVcnAvcDecoderTest AmdVcnHevcDecoderTest AmdVcnAv1DecoderTest AmdVcnVp9DecoderTest AmdVcnMpeg2DecoderTest AmdVcnVc1DecoderTest H265Test Mpeg2Test Vc1Test NvidiaGenerationTest IntelMfxTest IntelGtTest IntelVcsDriverTest IntelAvcSessionTest IntelMpeg2SessionTest IntelVc1SessionTest IntelHevcSessionTest IntelVp9SessionTest IntelGen12DriverTest IntelAvpTest IntelAv1SessionTest SerialTest WirelessTest); fi
for t in "${WANTED[@]}"; do
  case $t in
    FontTest) build_and_run FontTest tests/FontTest.cpp tests/KernelStubs.cpp src/Font/*.cpp src/Media/Zlib.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp ;;
    DashTest) build_and_run DashTest tests/DashTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp $media \
                src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/Intel/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Video/*.cpp src/Math/FloatMath.cpp \
                src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp src/Audio/AudioPlayer.cpp ;;
    ArrayViewTest) build_and_run ArrayViewTest tests/ArrayViewTest.cpp ;;
    WasmTest) build_and_run WasmTest tests/WasmTest.cpp tests/KernelStubs.cpp src/Wasm/*.cpp ;;
    ExecutableTest) build_and_run ExecutableTest tests/ExecutableTest.cpp src/Executable/*.cpp src/Desktop/Shell.cpp src/Desktop/DirectoryListing.cpp src/Desktop/FileTransfer.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                      src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp ;;
    SyscallTest) build_and_run SyscallTest tests/SyscallTest.cpp src/Process/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/Kernel/Sync.cpp tests/KernelStubs.cpp ;;
    ProcessFilesTest) build_and_run ProcessFilesTest tests/ProcessFilesTest.cpp src/Process/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/Kernel/Sync.cpp tests/KernelStubs.cpp ;;
    LoaderTest) build_and_run LoaderTest tests/LoaderTest.cpp src/Process/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/Kernel/Sync.cpp tests/KernelStubs.cpp ;;
    WebVttTest) build_and_run WebVttTest tests/WebVttTest.cpp src/Media/WebVtt.cpp tests/KernelStubs.cpp ;;
    LayoutUnitTest) build_and_run LayoutUnitTest tests/LayoutUnitTest.cpp ;;
    LayoutTest) build_and_run LayoutTest tests/LayoutTest.cpp tests/KernelStubs.cpp src/Layout/*.cpp src/Browser/HtmlDocument.cpp src/Browser/Renderer.cpp src/Browser/RendererBoxes.cpp src/Browser/Media.cpp src/Dom/*.cpp \
                  src/Browser/Css.cpp src/Browser/PageLayout.cpp src/Browser/Url.cpp src/Network/Http/HttpUrl.cpp \
                  src/Font/*.cpp src/Media/Zlib.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp ;;
    TableTest) build_and_run TableTest tests/TableTest.cpp tests/KernelStubs.cpp src/Layout/*.cpp src/Browser/HtmlDocument.cpp src/Browser/Renderer.cpp src/Browser/RendererBoxes.cpp src/Browser/Media.cpp src/Dom/*.cpp \
                  src/Browser/Css.cpp src/Browser/PageLayout.cpp src/Browser/Url.cpp src/Network/Http/HttpUrl.cpp \
                  src/Font/*.cpp src/Media/Zlib.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp ;;
    GridTest) build_and_run GridTest tests/GridTest.cpp tests/KernelStubs.cpp src/Layout/*.cpp src/Browser/HtmlDocument.cpp src/Browser/Renderer.cpp src/Browser/RendererBoxes.cpp src/Browser/Media.cpp src/Dom/*.cpp \
                  src/Browser/Css.cpp src/Browser/PageLayout.cpp src/Browser/Url.cpp src/Network/Http/HttpUrl.cpp \
                  src/Font/*.cpp src/Media/Zlib.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp ;;
    FlexTest) build_and_run FlexTest tests/FlexTest.cpp tests/KernelStubs.cpp src/Layout/*.cpp src/Browser/HtmlDocument.cpp src/Browser/Renderer.cpp src/Browser/RendererBoxes.cpp src/Browser/Media.cpp src/Dom/*.cpp \
                  src/Browser/Css.cpp src/Browser/PageLayout.cpp src/Browser/Url.cpp src/Network/Http/HttpUrl.cpp \
                  src/Font/*.cpp src/Media/Zlib.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp ;;
    PaintTest) build_and_run PaintTest tests/PaintTest.cpp tests/KernelStubs.cpp src/Layout/*.cpp src/Paint/*.cpp src/Font/*.cpp src/Browser/HtmlDocument.cpp src/Browser/Renderer.cpp src/Browser/RendererBoxes.cpp \
                 src/Browser/Media.cpp src/Dom/*.cpp src/Browser/Css.cpp src/Browser/PageLayout.cpp src/Browser/Url.cpp src/Network/Http/HttpUrl.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp \
                 src/Graphics/Graphics2D/Software/SoftwareRenderer2D.cpp src/Math/FloatMath.cpp src/Media/Image.cpp src/Media/ImageCodecs.cpp src/Media/Zlib.cpp src/Media/Image/*.cpp \
                 src/Hardware/VideoCodec/Software/JpegDecoder.cpp src/Hardware/VideoCodec/Software/JpegEncoder.cpp src/Hardware/VideoCodec/VideoTypes.cpp \
                 src/Hardware/VideoCodec/VideoFrameBuffer.cpp src/Hardware/VideoCodec/ColorConvert.cpp ;;
    PixelGoldenTest) build_and_run PixelGoldenTest tests/PixelGoldenTest.cpp tests/KernelStubs.cpp src/Media/Image.cpp src/Media/ImageCodecs.cpp src/Media/Zlib.cpp src/Media/Image/*.cpp \
                       src/Hardware/VideoCodec/Software/JpegDecoder.cpp src/Hardware/VideoCodec/Software/JpegEncoder.cpp src/Hardware/VideoCodec/VideoTypes.cpp \
                       src/Hardware/VideoCodec/VideoFrameBuffer.cpp src/Hardware/VideoCodec/ColorConvert.cpp ;;
    DatabaseTest) build_and_run DatabaseTest tests/DatabaseTest.cpp tests/KernelStubs.cpp src/Database/*.cpp src/Database/Postgres/*.cpp src/Crypto/*.cpp src/Stream/StreamStatus.cpp ;;
    SystemDatabaseTest) build_and_run SystemDatabaseTest tests/SystemDatabaseTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Database/*.cpp src/Database/Local/*.cpp \
                          src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp ;;
    DatabaseNetworkTest) build_and_run DatabaseNetworkTest tests/DatabaseNetworkTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Database/*.cpp src/Database/Postgres/*.cpp src/Database/Net/*.cpp \
                           src/Stream/StreamStatus.cpp src/Async/Executor.cpp src/Stream/Tcp/TcpStream.cpp src/FileSystem/FileSystemTypes.cpp src/Network/ServerNetwork.cpp src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp \
                           src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp src/Network/Tls/TlsSocket.cpp src/Network/Tls/TlsKeys.cpp src/Network/Tls/TlsCertificate.cpp src/Network/Tls/TlsSession.cpp src/Crypto/*.cpp ;;
    DomTest) build_and_run DomTest tests/DomTest.cpp tests/KernelStubs.cpp src/Dom/*.cpp src/Browser/Media.cpp ;;
    PageTest) build_and_run PageTest tests/PageTest.cpp src/Rtc/*.cpp src/Network/Tls/TlsKeys.cpp tests/KernelStubs.cpp src/Browser/Page.cpp src/Layout/*.cpp src/Paint/*.cpp src/Font/*.cpp src/Browser/Objects.cpp src/Media/WebVtt.cpp src/Media/Dash.cpp src/Media/DashClient.cpp src/Browser/MseClip.cpp src/Media/MseParsers.cpp src/Media/MseBuffer.cpp src/Media/MseHub.cpp src/Media/Eme.cpp src/Browser/Svg.cpp src/Browser/Canvas.cpp src/Browser/DomBindings*.cpp src/Browser/WebGlPrelude.cpp src/Browser/WebGpuPrelude.cpp src/WebGl/*.cpp src/Wasm/*.cpp src/Crypto/*.cpp src/Kernel/Sync.cpp src/Media/Image.cpp src/Media/ImageCodecs.cpp src/Media/Zlib.cpp src/Media/Image/*.cpp src/Hardware/VideoCodec/Software/JpegDecoder.cpp src/Hardware/VideoCodec/Software/JpegEncoder.cpp src/Hardware/VideoCodec/VideoTypes.cpp src/Hardware/VideoCodec/VideoFrameBuffer.cpp src/Hardware/VideoCodec/ColorConvert.cpp src/Graphics/Graphics2D/Software/SoftwareRenderer2D.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp src/Browser/HtmlDocument.cpp src/Browser/Renderer.cpp src/Browser/RendererBoxes.cpp src/Browser/Media.cpp src/Browser/Css.cpp src/Browser/PageLayout.cpp src/Browser/Url.cpp src/Browser/WebUrl.cpp src/Browser/CookieJar.cpp src/Browser/PublicSuffix.cpp src/Browser/Fetcher.cpp src/Browser/WebPrelude.cpp src/Browser/PageHub.cpp src/Browser/SessionHistory.cpp src/Browser/CacheStore.cpp src/Browser/ServiceWorker.cpp src/Browser/WebPush.cpp src/Browser/ServiceWorkerPrelude.cpp src/Dom/*.cpp src/Network/Http/HttpUrl.cpp src/Script/*.cpp src/Regex/Regex.cpp src/Math/FloatMath.cpp ;;
    ShaderTest) build_and_run ShaderTest tests/ShaderTest.cpp tests/KernelStubs.cpp src/WebGl/*.cpp src/Math/FloatMath.cpp ;;
    GpuTest) build_and_run GpuTest tests/GpuTest.cpp tests/KernelStubs.cpp src/WebGl/*.cpp src/Math/FloatMath.cpp ;;
    GlTest) build_and_run GlTest tests/GlTest.cpp tests/KernelStubs.cpp src/WebGl/*.cpp src/Math/FloatMath.cpp ;;
    RtcTest) build_and_run RtcTest tests/RtcTest.cpp tests/KernelStubs.cpp src/Rtc/*.cpp src/Crypto/*.cpp src/Network/Tls/TlsKeys.cpp src/Kernel/Sync.cpp ;;
    CanvasTest) build_and_run CanvasTest tests/CanvasTest.cpp tests/KernelStubs.cpp src/Browser/Canvas.cpp src/Browser/Css.cpp src/Graphics/Graphics2D/Software/SoftwareRenderer2D.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp src/Math/FloatMath.cpp src/Dom/*.cpp src/Browser/Media.cpp ;;
    BrowserTest) build_and_run BrowserTest tests/BrowserTest.cpp tests/KernelStubs.cpp src/Browser/HtmlDocument.cpp src/Browser/Renderer.cpp src/Browser/RendererBoxes.cpp src/Browser/Media.cpp src/Dom/*.cpp src/Browser/Css.cpp src/Browser/PageLayout.cpp src/Browser/Url.cpp src/Network/Http/HttpUrl.cpp ;;
    BrowserStoreTest) build_and_run BrowserStoreTest tests/BrowserStoreTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Database/*.cpp src/Database/Local/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp \
                       src/Desktop/BrowserStore.cpp src/Browser/CacheStore.cpp src/Browser/CookieJar.cpp src/Browser/PublicSuffix.cpp src/Browser/WebUrl.cpp src/Browser/PageHub.cpp ;;
    CookieTest) build_and_run CookieTest tests/CookieTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Browser/WebUrl.cpp src/Browser/CookieJar.cpp src/Browser/PublicSuffix.cpp ;;
    PrefetchTest) ASAN_OPTIONS=detect_leaks=0 build_and_run PrefetchTest tests/PrefetchTest.cpp tests/HostThreads.cpp src/Browser/ResourcePrefetcher.cpp -pthread ;;
    FetcherTest) build_and_run FetcherTest tests/FetcherTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Browser/WebUrl.cpp src/Browser/CookieJar.cpp src/Browser/PublicSuffix.cpp src/Browser/Fetcher.cpp ;;
    HistoryTest) build_and_run HistoryTest tests/HistoryTest.cpp tests/KernelStubs.cpp src/Browser/SessionHistory.cpp src/Browser/PageHub.cpp ;;
    CssTest) build_and_run CssTest tests/CssTest.cpp tests/KernelStubs.cpp src/Browser/HtmlDocument.cpp src/Browser/Renderer.cpp src/Browser/RendererBoxes.cpp src/Browser/Media.cpp src/Dom/*.cpp src/Browser/Css.cpp src/Browser/PageLayout.cpp src/Browser/Url.cpp src/Network/Http/HttpUrl.cpp ;;
    UrlTest) build_and_run UrlTest tests/UrlTest.cpp tests/KernelStubs.cpp src/Browser/WebUrl.cpp ;;
    ScriptTest) build_and_run ScriptTest tests/ScriptTest.cpp tests/KernelStubs.cpp src/Script/*.cpp src/Regex/Regex.cpp src/Math/FloatMath.cpp ;;
    SheetTest) build_and_run SheetTest tests/SheetTest.cpp tests/KernelStubs.cpp src/Spreadsheet/Sheet.cpp src/Math/FloatMath.cpp ;;
    AmlTest) build_and_run AmlTest tests/AmlTest.cpp tests/KernelStubs.cpp src/Hardware/Acpi/AmlInterpreter.cpp src/Hardware/Acpi/EmbeddedController.cpp src/Hardware/Acpi/AcpiTables.cpp src/Hardware/Battery/AcpiBattery.cpp ;;
    ClipboardTest) build_and_run ClipboardTest tests/ClipboardTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Kernel/Clipboard.cpp ;;
    StringTest) build_and_run StringTest tests/StringTest.cpp ;;
    RegexTest) build_and_run RegexTest tests/RegexTest.cpp src/Regex/Regex.cpp ;;
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
                      src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/Intel/*.cpp src/Hardware/VideoCodec/Software/*.cpp ;;
    VideoPlayerTest) build_and_run VideoPlayerTest tests/VideoPlayerTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                       src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/Intel/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Video/*.cpp src/Math/FloatMath.cpp \
                       src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp src/Audio/AudioPlayer.cpp $media ;;
    WebmTest) build_and_run WebmTest tests/WebmTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                 src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/Intel/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Video/*.cpp src/Math/FloatMath.cpp \
                 src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp src/Audio/AudioPlayer.cpp $media ;;
    MseTest) build_and_run MseTest tests/MseTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                 src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/Intel/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Video/*.cpp src/Math/FloatMath.cpp \
                 src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp src/Audio/AudioPlayer.cpp $media ;;
    MediaTest) build_and_run MediaTest tests/MediaTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp $media \
                 src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/Intel/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Video/*.cpp src/Math/FloatMath.cpp \
                 src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp src/Audio/AudioPlayer.cpp ;;
    MidiTest) build_and_run MidiTest tests/MidiTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp $media \
                src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/Intel/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Math/FloatMath.cpp src/Hardware/MidiDevice/Mpu401.cpp ;;
    WirelessTest) build_and_run WirelessTest tests/WirelessTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Hardware/WirelessInterface/*.cpp src/Hardware/WirelessInterface/Software/*.cpp ;;
    SerialTest) build_and_run SerialTest tests/SerialTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Hardware/SerialPort/*.cpp ;;
    UsbMidiTest) build_and_run UsbMidiTest tests/UsbMidiTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Midi/MidiFile.cpp src/Midi/MidiStream.cpp src/Midi/SoftwareSynth.cpp \
                   src/Midi/LiveSynth.cpp src/Math/FloatMath.cpp src/Hardware/MidiDevice/Usb/*.cpp src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp \
                   src/Audio/AudioPlayer.cpp ;;
    IntelGenerationTest) build_and_run IntelGenerationTest tests/IntelGenerationTest.cpp tests/KernelStubs.cpp src/Hardware/CentralProcessingUnit/X86_64Cpu.cpp src/Hardware/CentralProcessingUnit/Intel/IntelX64Cpu.cpp src/Hardware/CentralProcessingUnit/Intel/IntelGenerations.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.cpp src/Hardware/VideoCodec/Intel/IntelMediaEngineHw.cpp ;;
    AmdGenerationTest) build_and_run AmdGenerationTest tests/AmdGenerationTest.cpp tests/KernelStubs.cpp src/Hardware/CentralProcessingUnit/X86_64Cpu.cpp src/Hardware/CentralProcessingUnit/AMD/AmdGenerations.cpp src/Hardware/GraphicsProcessingUnit/AMD/AmdGpuGenerations.cpp ;;
    AmdDiscoveryTest) build_and_run AmdDiscoveryTest tests/AmdDiscoveryTest.cpp tests/KernelStubs.cpp src/Hardware/VideoCodec/AMD/AmdIpDiscovery.cpp src/Hardware/VideoCodec/AMD/AmdMediaEngineHw.cpp ;;
    AmdVcnFirmwareTest) build_and_run AmdVcnFirmwareTest tests/AmdVcnFirmwareTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Hardware/VideoCodec/AMD/AmdVcnFirmware.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp ;;
    AmdVcn1Test) build_and_run AmdVcn1Test tests/AmdVcn1Test.cpp tests/KernelStubs.cpp src/Hardware/VideoCodec/AMD/AmdVcn1.cpp src/Hardware/VideoCodec/AMD/AmdVcnFirmware.cpp ;;
    AmdRavenMemoryTest) build_and_run AmdRavenMemoryTest tests/AmdRavenMemoryTest.cpp tests/KernelStubs.cpp src/Hardware/VideoCodec/AMD/AmdRavenMemory.cpp src/Hardware/VideoCodec/AMD/AmdVcn1.cpp src/Hardware/VideoCodec/AMD/AmdVcnFirmware.cpp ;;
    AmdVcnDecodeMessagesTest) build_and_run AmdVcnDecodeMessagesTest tests/AmdVcnDecodeMessagesTest.cpp tests/KernelStubs.cpp src/Hardware/VideoCodec/AMD/AmdVcnDecodeMessages.cpp src/Hardware/VideoCodec/Software/H26*.cpp src/Hardware/VideoCodec/VideoTypes.cpp ;;
    AmdVcn1RingTest) build_and_run AmdVcn1RingTest tests/AmdVcn1RingTest.cpp tests/KernelStubs.cpp src/Hardware/VideoCodec/AMD/AmdVcn1Ring.cpp ;;
    AmdVcnAvcDecoderTest) build_and_run AmdVcnAvcDecoderTest tests/AmdVcnAvcDecoderTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/Intel/*.cpp ;;
    AmdVcnHevcDecoderTest) build_and_run AmdVcnHevcDecoderTest tests/AmdVcnHevcDecoderTest.cpp tests/KernelStubs.cpp src/Crypto/Crypto.cpp src/Kernel/Sync.cpp src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/Intel/*.cpp ;;
    AmdVcnAv1DecoderTest) build_and_run AmdVcnAv1DecoderTest tests/AmdVcnAv1DecoderTest.cpp tests/KernelStubs.cpp src/Crypto/Crypto.cpp src/Kernel/Sync.cpp src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/Intel/*.cpp ;;
    AmdVcnVp9DecoderTest) build_and_run AmdVcnVp9DecoderTest tests/AmdVcnVp9DecoderTest.cpp tests/KernelStubs.cpp src/Crypto/Crypto.cpp src/Kernel/Sync.cpp src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/Intel/*.cpp ;;
    AmdVcnMpeg2DecoderTest) build_and_run AmdVcnMpeg2DecoderTest tests/AmdVcnMpeg2DecoderTest.cpp tests/KernelStubs.cpp src/Crypto/Crypto.cpp src/Kernel/Sync.cpp src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/Intel/*.cpp ;;
    AmdVcnVc1DecoderTest) build_and_run AmdVcnVc1DecoderTest tests/AmdVcnVc1DecoderTest.cpp tests/KernelStubs.cpp src/Crypto/Crypto.cpp src/Kernel/Sync.cpp src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/Intel/*.cpp ;;
    H265Test) build_and_run H265Test tests/H265Test.cpp tests/KernelStubs.cpp src/Crypto/Crypto.cpp src/Hardware/VideoCodec/Software/H26*.cpp src/Hardware/VideoCodec/VideoTypes.cpp ;;
    Mpeg2Test) build_and_run Mpeg2Test tests/Mpeg2Test.cpp tests/KernelStubs.cpp src/Hardware/VideoCodec/Software/Mpeg2*.cpp src/Hardware/VideoCodec/VideoTypes.cpp ;;
    Vc1Test) build_and_run Vc1Test tests/Vc1Test.cpp tests/KernelStubs.cpp src/Hardware/VideoCodec/Software/Vc1*.cpp src/Hardware/VideoCodec/VideoTypes.cpp ;;
    NvidiaGenerationTest) build_and_run NvidiaGenerationTest tests/NvidiaGenerationTest.cpp tests/KernelStubs.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaGpuGenerations.cpp src/Hardware/VideoCodec/NVIDIA/NvidiaMediaEngineHw.cpp ;;
    IntelMfxTest) build_and_run IntelMfxTest tests/IntelMfxTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                    src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Hardware/GraphicsProcessingUnit/Intel/*.cpp ;;
    IntelAvpTest) build_and_run IntelAvpTest tests/IntelAvpTest.cpp tests/KernelStubs.cpp src/Hardware/VideoCodec/Intel/IntelAvpCommands.cpp src/Hardware/VideoCodec/Intel/IntelAv1Cdf.cpp src/Hardware/VideoCodec/Intel/IntelMfxCommands.cpp ;;
    IntelGtTest) build_and_run IntelGtTest tests/IntelGtTest.cpp tests/KernelStubs.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelGtt.cpp \
                   src/Hardware/GraphicsProcessingUnit/Intel/IntelLrc.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelExeclist.cpp ;;
    IntelVcsDriverTest) build_and_run IntelVcsDriverTest tests/IntelVcsDriverTest.cpp tests/KernelStubs.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelGtt.cpp \
                          src/Hardware/GraphicsProcessingUnit/Intel/IntelLrc.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelExeclist.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelForcewake.cpp \
                          src/Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.cpp src/Hardware/VideoCodec/Intel/IntelMfxCommands.cpp ;;
    IntelGen12DriverTest) build_and_run IntelGen12DriverTest tests/IntelGen12DriverTest.cpp tests/KernelStubs.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelGtt.cpp \
                          src/Hardware/GraphicsProcessingUnit/Intel/IntelLrc.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelExeclist.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelForcewake.cpp \
                          src/Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.cpp src/Hardware/VideoCodec/Intel/IntelMfxCommands.cpp ;;
    IntelAvcSessionTest) build_and_run IntelAvcSessionTest tests/IntelAvcSessionTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                           src/Hardware/GraphicsProcessingUnit/Intel/IntelGtt.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelLrc.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelExeclist.cpp \
                           src/Hardware/GraphicsProcessingUnit/Intel/IntelForcewake.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.cpp \
                           src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/VideoCodec/Software/*.cpp ;;
    IntelMpeg2SessionTest) build_and_run IntelMpeg2SessionTest tests/IntelMpeg2SessionTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                           src/Hardware/GraphicsProcessingUnit/Intel/IntelGtt.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelLrc.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelExeclist.cpp \
                           src/Hardware/GraphicsProcessingUnit/Intel/IntelForcewake.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.cpp \
                           src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/VideoCodec/Software/*.cpp ;;
    IntelVc1SessionTest) build_and_run IntelVc1SessionTest tests/IntelVc1SessionTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                           src/Hardware/GraphicsProcessingUnit/Intel/IntelGtt.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelLrc.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelExeclist.cpp \
                           src/Hardware/GraphicsProcessingUnit/Intel/IntelForcewake.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.cpp \
                           src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/VideoCodec/Software/*.cpp ;;
    IntelHevcSessionTest) build_and_run IntelHevcSessionTest tests/IntelHevcSessionTest.cpp tests/KernelStubs.cpp src/Crypto/Crypto.cpp src/Kernel/Sync.cpp \
                           src/Hardware/GraphicsProcessingUnit/Intel/IntelGtt.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelLrc.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelExeclist.cpp \
                           src/Hardware/GraphicsProcessingUnit/Intel/IntelForcewake.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.cpp \
                           src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/VideoCodec/Software/*.cpp ;;
    IntelVp9SessionTest) build_and_run IntelVp9SessionTest tests/IntelVp9SessionTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                           src/Hardware/GraphicsProcessingUnit/Intel/IntelGtt.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelLrc.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelExeclist.cpp \
                           src/Hardware/GraphicsProcessingUnit/Intel/IntelForcewake.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.cpp \
                           src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/VideoCodec/Software/*.cpp ;;
    IntelAv1SessionTest) build_and_run IntelAv1SessionTest tests/IntelAv1SessionTest.cpp tests/KernelStubs.cpp src/Crypto/Crypto.cpp src/Kernel/Sync.cpp \
                           src/Hardware/GraphicsProcessingUnit/Intel/IntelGtt.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelLrc.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelExeclist.cpp \
                           src/Hardware/GraphicsProcessingUnit/Intel/IntelForcewake.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.cpp src/Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.cpp \
                           src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/VideoCodec/Software/*.cpp ;;
    MonitorTest) build_and_run MonitorTest tests/MonitorTest.cpp src/Hardware/Monitor/*.cpp ;;
    StreamingTest) build_and_run StreamingTest tests/StreamingTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp $media \
                     src/Hardware/VideoCodec/*.cpp src/Hardware/VideoCodec/Intel/*.cpp src/Hardware/VideoCodec/AMD/*.cpp src/Hardware/VideoCodec/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/AMD/*.cpp src/Hardware/GraphicsProcessingUnit/NVIDIA/*.cpp src/Hardware/GraphicsProcessingUnit/Intel/*.cpp src/Hardware/VideoCodec/Software/*.cpp src/Math/FloatMath.cpp ;;
    PartitionTest) build_and_run PartitionTest tests/PartitionTest.cpp tests/KernelStubs.cpp src/Hardware/StorageDevice/Partition/PartitionScanner.cpp ;;
    FileSystemTest) build_and_run FileSystemTest tests/FileSystemTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                      src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/FileSystem/Fat/*.cpp src/FileSystem/Befs/*.cpp src/FileSystem/Ntfs/*.cpp src/FileSystem/Ext/*.cpp ;;
    BefsTest) build_and_run BefsTest tests/BefsTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                src/FileSystem/FileSystemTypes.cpp src/FileSystem/Befs/*.cpp ;;
    NtfsTest) build_and_run NtfsTest tests/NtfsTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ntfs/*.cpp ;;
    ExtTest) build_and_run ExtTest tests/ExtTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
               src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ext/*.cpp ;;
    StorageHotplugTest) build_and_run StorageHotplugTest tests/StorageHotplugTest.cpp tests/UsbAudioStubs.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Kernel/Paging.cpp \
                          src/Hardware/MmioMapper.cpp src/Hardware/Bus/Pci/PciBar.cpp src/Hardware/Bus/Virtio/VirtioPciDevice.cpp src/Hardware/Bus/Virtio/Virtqueue.cpp \
                          src/Hardware/StorageDevice/StorageDeviceBase.cpp src/Hardware/StorageDevice/SystemStorageDevices.cpp src/Hardware/StorageDevice/*/*.cpp \
                          src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/FileSystem/Fat/*.cpp src/FileSystem/Befs/*.cpp src/FileSystem/Ntfs/*.cpp src/FileSystem/Ext/*.cpp src/FileSystem/Mount/*.cpp src/SystemFileSystem.cpp ;;
    InputHotplugTest) build_and_run InputHotplugTest tests/InputHotplugTest.cpp tests/UsbAudioStubs.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Kernel/Paging.cpp \
                        src/Hardware/MmioMapper.cpp src/Hardware/Bus/Pci/PciBar.cpp src/Hardware/Bus/Virtio/VirtioPciDevice.cpp src/Hardware/Bus/Virtio/Virtqueue.cpp src/Hardware/Bus/Virtio/VirtioInputDevice.cpp \
                        src/Hardware/SystemInput.cpp tests/HypervisorStubs.cpp src/Hardware/Bus/Ps2/*.cpp src/Hardware/Keyboard/*.cpp src/Hardware/Keyboard/*/*.cpp src/Hardware/Mouse/*.cpp src/Hardware/Mouse/*/*.cpp \
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
                src/Network/Http/*.cpp src/Network/Tls/TlsSocket.cpp src/Network/Tls/TlsKeys.cpp src/Network/Tls/TlsCertificate.cpp src/Network/Tls/TlsSession.cpp src/Network/Tls/Tls13Client.cpp src/Network/Quic/*.cpp src/Crypto/*.cpp src/Media/Zlib.cpp ;;
    WebSocketTest) build_and_run WebSocketTest tests/WebSocketTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Network/Dns/*.cpp src/Network/Http/*.cpp src/Network/Tls/TlsSocket.cpp src/Network/Tls/TlsKeys.cpp src/Network/Tls/TlsCertificate.cpp src/Network/Tls/TlsSession.cpp src/Network/Tls/Tls13Client.cpp src/Network/Quic/*.cpp \
                src/Crypto/*.cpp src/Media/Zlib.cpp ;;
    Http3Test) build_and_run Http3Test tests/Http3Test.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Network/Quic/Qpack.cpp src/Network/Http/AltSvc.cpp ;;
    QuicTest) build_and_run QuicTest tests/QuicTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Network/Quic/*.cpp src/Network/Tls/TlsKeys.cpp src/Network/Tls/TlsSocket.cpp src/Network/Tls/TlsCertificate.cpp src/Network/Tls/TlsSession.cpp src/Network/Tls/Tls13Client.cpp src/Crypto/*.cpp ;;
    InteropTool) build_and_run InteropTool tests/InteropTool.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Network/Dns/*.cpp src/Network/Http/*.cpp src/Network/Tls/TlsSocket.cpp src/Network/Tls/TlsKeys.cpp src/Network/Tls/TlsCertificate.cpp src/Network/Tls/TlsSession.cpp src/Network/Tls/Tls13Client.cpp src/Network/Quic/*.cpp \
                src/Crypto/*.cpp src/Media/Zlib.cpp ;;
    SmbTest) build_and_run SmbTest tests/SmbTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/FileSystem/FileSystemTypes.cpp src/Network/Smb/*.cpp src/Network/ServerNetwork.cpp \
                src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp \
                src/Crypto/*.cpp ;;
    NfsTest) build_and_run NfsTest tests/NfsTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/FileSystem/FileSystemTypes.cpp src/Network/Nfs/*.cpp src/Network/ServerNetwork.cpp \
                src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp ;;
    SshFtpTest) build_and_run SshFtpTest tests/SshFtpTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/FileSystem/FileSystemTypes.cpp src/Network/Ssh/*.cpp src/Network/Sftp/*.cpp src/Network/Ftp/*.cpp \
                src/Network/ServerNetwork.cpp src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp \
                src/Network/Dns/*.cpp src/Network/Tls/TlsSocket.cpp src/Network/Tls/TlsKeys.cpp src/Network/Tls/TlsCertificate.cpp src/Network/Tls/TlsSession.cpp src/Crypto/*.cpp ;;
    CryptoTest) build_and_run CryptoTest tests/CryptoTest.cpp tests/KernelStubs.cpp src/Crypto/*.cpp ;;
    # (without the address sanitizer: this test maps memory above 4 GiB, which the sanitizer reserves for its shadow)
    AllocatorTest) echo "== AllocatorTest"; $cxx ${flags/address,undefined/undefined} tests/AllocatorTest.cpp tests/KernelStubs.cpp src/Allocators/Allocator.cpp -o "$out/AllocatorTest" && "$out/AllocatorTest" ;;
    PagingTest) build_and_run PagingTest tests/PagingTest.cpp tests/KernelStubs.cpp src/Kernel/Paging.cpp ;;
    MmioTest) build_and_run MmioTest tests/MmioTest.cpp tests/KernelStubs.cpp src/Kernel/Paging.cpp src/Hardware/MmioMapper.cpp src/Hardware/Bus/Pci/PciBar.cpp ;;
    X509Test) build_and_run X509Test tests/X509Test.cpp tests/KernelStubs.cpp src/Crypto/*.cpp ;;
    TlsReplayTest) build_and_run TlsReplayTest tests/TlsReplayTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Network/Tls/TlsSocket.cpp src/Network/Tls/TlsKeys.cpp src/Network/Tls/TlsCertificate.cpp src/Network/Tls/TlsSession.cpp src/Crypto/*.cpp ;;
    SnmpTest) build_and_run SnmpTest tests/SnmpTest.cpp tests/KernelStubs.cpp src/Crypto/*.cpp src/Network/Snmp/SnmpMessage.cpp src/Network/Snmp/SnmpUsm.cpp ;;
    PrinterTest) build_and_run PrinterTest tests/PrinterTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/SystemPrintService.cpp src/Hardware/Printer/*.cpp src/Hardware/Printer/Brother/*.cpp src/Hardware/Printer/Usb/*.cpp src/Network/Snmp/*.cpp src/Network/Ipp/*.cpp src/Network/Tls/TlsSocket.cpp src/Network/Tls/TlsKeys.cpp src/Network/Tls/TlsCertificate.cpp src/Network/Tls/TlsSession.cpp src/Network/Mdns/*.cpp src/Crypto/*.cpp \
                   src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp \
                   src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp ;;
    PrinterHotplugTest) build_and_run PrinterHotplugTest tests/PrinterHotplugTest.cpp tests/UsbAudioStubs.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Hardware/Printer/*.cpp src/Hardware/Printer/Brother/*.cpp src/Hardware/Printer/Usb/*.cpp src/Network/Snmp/*.cpp src/Network/Ipp/*.cpp src/Network/Tls/TlsSocket.cpp src/Network/Tls/TlsKeys.cpp src/Network/Tls/TlsCertificate.cpp src/Network/Tls/TlsSession.cpp src/Network/Mdns/*.cpp src/Crypto/*.cpp \
                          src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp \
                          src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp ;;
    GraphicsTest) build_and_run GraphicsTest tests/GraphicsTest.cpp tests/KernelStubs.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp \
                    src/Graphics/Graphics2D/Software/*.cpp src/Graphics/Graphics2D/Device/*.cpp src/Graphics/Graphics3D/BuiltinShaders.cpp src/Graphics/Graphics3D/Software/*.cpp ;;
    GuiTest) build_and_run GuiTest tests/GuiTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Kernel/Clipboard.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp \
                src/Hardware/Keyboard/KeyCodes.cpp src/Graphics/Graphics2D/Software/*.cpp src/Gui/*.cpp src/Gui/Controls/*.cpp src/Gui/Windows/*.cpp src/Desktop/*.cpp src/Rtc/*.cpp src/Network/Tls/TlsKeys.cpp src/Executable/*.cpp src/Spreadsheet/*.cpp src/Browser/HtmlDocument.cpp src/Browser/Renderer.cpp src/Browser/RendererBoxes.cpp src/Browser/Media.cpp src/Browser/Page.cpp src/Layout/*.cpp src/Paint/*.cpp src/Font/*.cpp src/Browser/Objects.cpp src/Browser/MseClip.cpp src/Browser/Svg.cpp src/Browser/Canvas.cpp src/Browser/DomBindings*.cpp src/Browser/WebGlPrelude.cpp src/Browser/WebGpuPrelude.cpp src/WebGl/*.cpp src/Wasm/*.cpp src/Dom/*.cpp src/Browser/Css.cpp src/Browser/PageLayout.cpp src/Browser/Url.cpp src/Browser/WebUrl.cpp src/Browser/CookieJar.cpp src/Browser/PublicSuffix.cpp src/Browser/Fetcher.cpp src/Browser/WebPrelude.cpp src/Browser/PageHub.cpp src/Browser/CacheStore.cpp src/Browser/ServiceWorker.cpp src/Browser/WebPush.cpp src/Browser/ServiceWorkerPrelude.cpp src/Browser/ResourcePrefetcher.cpp src/Browser/SessionHistory.cpp src/Network/Http/HttpUrl.cpp src/Script/*.cpp src/Regex/Regex.cpp src/Database/*.cpp src/Database/Local/*.cpp src/Graphics/Graphics3D/BuiltinShaders.cpp tests/DesktopStubs.cpp $gui_audio src/FileSystem/FileSystemTypes.cpp ;;
    GuiControlsTest) build_and_run GuiControlsTest tests/GuiControlsTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Kernel/Clipboard.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp \
                src/Hardware/Keyboard/KeyCodes.cpp src/Graphics/Graphics2D/Software/*.cpp src/Gui/*.cpp src/Gui/Controls/*.cpp src/Gui/Windows/*.cpp src/Desktop/*.cpp src/Rtc/*.cpp src/Network/Tls/TlsKeys.cpp src/Executable/*.cpp src/Spreadsheet/*.cpp src/Browser/HtmlDocument.cpp src/Browser/Renderer.cpp src/Browser/RendererBoxes.cpp src/Browser/Media.cpp src/Browser/Page.cpp src/Layout/*.cpp src/Paint/*.cpp src/Font/*.cpp src/Browser/Objects.cpp src/Browser/MseClip.cpp src/Browser/Svg.cpp src/Browser/Canvas.cpp src/Browser/DomBindings*.cpp src/Browser/WebGlPrelude.cpp src/Browser/WebGpuPrelude.cpp src/WebGl/*.cpp src/Wasm/*.cpp src/Dom/*.cpp src/Browser/Css.cpp src/Browser/PageLayout.cpp src/Browser/Url.cpp src/Browser/WebUrl.cpp src/Browser/CookieJar.cpp src/Browser/PublicSuffix.cpp src/Browser/Fetcher.cpp src/Browser/WebPrelude.cpp src/Browser/PageHub.cpp src/Browser/CacheStore.cpp src/Browser/ServiceWorker.cpp src/Browser/WebPush.cpp src/Browser/ServiceWorkerPrelude.cpp src/Browser/ResourcePrefetcher.cpp src/Browser/SessionHistory.cpp src/Network/Http/HttpUrl.cpp src/Script/*.cpp src/Regex/Regex.cpp src/Database/*.cpp src/Database/Local/*.cpp src/Graphics/Graphics3D/BuiltinShaders.cpp tests/DesktopStubs.cpp $gui_audio src/FileSystem/FileSystemTypes.cpp ;;
    GuiViewsTest) build_and_run GuiViewsTest tests/GuiViewsTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Kernel/Clipboard.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp \
                src/Hardware/Keyboard/KeyCodes.cpp src/Graphics/Graphics2D/Software/*.cpp src/Gui/*.cpp src/Gui/Controls/*.cpp src/Gui/Windows/*.cpp src/Desktop/*.cpp src/Rtc/*.cpp src/Network/Tls/TlsKeys.cpp src/Executable/*.cpp src/Spreadsheet/*.cpp src/Browser/HtmlDocument.cpp src/Browser/Renderer.cpp src/Browser/RendererBoxes.cpp src/Browser/Media.cpp src/Browser/Page.cpp src/Layout/*.cpp src/Paint/*.cpp src/Font/*.cpp src/Browser/Objects.cpp src/Browser/MseClip.cpp src/Browser/Svg.cpp src/Browser/Canvas.cpp src/Browser/DomBindings*.cpp src/Browser/WebGlPrelude.cpp src/Browser/WebGpuPrelude.cpp src/WebGl/*.cpp src/Wasm/*.cpp src/Dom/*.cpp src/Browser/Css.cpp src/Browser/PageLayout.cpp src/Browser/Url.cpp src/Browser/WebUrl.cpp src/Browser/CookieJar.cpp src/Browser/PublicSuffix.cpp src/Browser/Fetcher.cpp src/Browser/WebPrelude.cpp src/Browser/PageHub.cpp src/Browser/CacheStore.cpp src/Browser/ServiceWorker.cpp src/Browser/WebPush.cpp src/Browser/ServiceWorkerPrelude.cpp src/Browser/ResourcePrefetcher.cpp src/Browser/SessionHistory.cpp src/Network/Http/HttpUrl.cpp src/Script/*.cpp src/Regex/Regex.cpp src/Database/*.cpp src/Database/Local/*.cpp src/Graphics/Graphics3D/BuiltinShaders.cpp tests/DesktopStubs.cpp $gui_audio ;;
    StorageEjectTest) build_and_run StorageEjectTest tests/StorageEjectTest.cpp tests/UsbAudioStubs.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/Kernel/Paging.cpp \
                        src/Hardware/MmioMapper.cpp src/Hardware/Bus/Pci/PciBar.cpp src/Hardware/Bus/Virtio/VirtioPciDevice.cpp src/Hardware/Bus/Virtio/Virtqueue.cpp \
                        src/Hardware/StorageDevice/StorageDeviceBase.cpp src/Hardware/StorageDevice/SystemStorageDevices.cpp src/Hardware/StorageDevice/*/*.cpp \
                        src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/FileSystem/Fat/*.cpp src/FileSystem/Befs/*.cpp src/FileSystem/Ntfs/*.cpp src/FileSystem/Ext/*.cpp src/FileSystem/Mount/*.cpp src/SystemFileSystem.cpp ;;
    StorageHotplugThreadTest)  # real threads, checked with ThreadSanitizer
      echo "== StorageHotplugThreadTest"
      $cxx -std=c++20 -O1 -g -fsanitize=thread -pthread -I src -I tests -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-misleading-indentation -Wno-delete-non-virtual-dtor \
        -o "$out/StorageHotplugThreadTest" tests/StorageHotplugThreadTest.cpp tests/HostThreads.cpp tests/UsbAudioStubs.cpp src/Kernel/Paging.cpp \
        src/Hardware/MmioMapper.cpp src/Hardware/Bus/Pci/PciBar.cpp src/Hardware/Bus/Virtio/VirtioPciDevice.cpp src/Hardware/Bus/Virtio/Virtqueue.cpp \
        src/Hardware/StorageDevice/StorageDeviceBase.cpp src/Hardware/StorageDevice/SystemStorageDevices.cpp src/Hardware/StorageDevice/*/*.cpp \
        src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/FileSystem/Fat/*.cpp src/FileSystem/Befs/*.cpp src/FileSystem/Ntfs/*.cpp src/FileSystem/Ext/*.cpp src/FileSystem/Mount/*.cpp src/SystemFileSystem.cpp
      "$out/StorageHotplugThreadTest" ;;
    InputHotplugThreadTest)  # real threads, checked with ThreadSanitizer
      echo "== InputHotplugThreadTest"
      $cxx -std=c++20 -O1 -g -fsanitize=thread -pthread -I src -I tests -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-misleading-indentation -Wno-delete-non-virtual-dtor \
        -o "$out/InputHotplugThreadTest" tests/InputHotplugThreadTest.cpp tests/HostThreads.cpp tests/UsbAudioStubs.cpp src/Kernel/Paging.cpp \
        src/Hardware/MmioMapper.cpp src/Hardware/Bus/Pci/PciBar.cpp src/Hardware/Bus/Virtio/VirtioPciDevice.cpp src/Hardware/Bus/Virtio/Virtqueue.cpp src/Hardware/Bus/Virtio/VirtioInputDevice.cpp \
        src/Hardware/SystemInput.cpp tests/HypervisorStubs.cpp src/Hardware/Bus/Ps2/*.cpp src/Hardware/Keyboard/*.cpp src/Hardware/Keyboard/*/*.cpp src/Hardware/Mouse/*.cpp src/Hardware/Mouse/*/*.cpp \
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
        src/Hardware/Printer/*.cpp src/Hardware/Printer/Brother/*.cpp src/Hardware/Printer/Usb/*.cpp src/Network/Snmp/*.cpp src/Network/Ipp/*.cpp src/Network/Tls/TlsSocket.cpp src/Network/Tls/TlsKeys.cpp src/Network/Tls/TlsCertificate.cpp src/Network/Tls/TlsSession.cpp src/Network/Mdns/*.cpp src/Crypto/*.cpp \
        src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp
      "$out/PrinterHotplugThreadTest" ;;
    GuiThreadTest)  # real threads, checked with ThreadSanitizer (cannot be combined with the address sanitizer)
      echo "== GuiThreadTest"
      $cxx -std=c++20 -O1 -g -fsanitize=thread -pthread -I src -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-misleading-indentation -Wno-delete-non-virtual-dtor \
        -o "$out/GuiThreadTest" tests/GuiThreadTest.cpp tests/HostThreads.cpp src/Kernel/Clipboard.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp \
        src/Hardware/Keyboard/KeyCodes.cpp src/Graphics/Graphics2D/Software/*.cpp src/Gui/*.cpp src/Gui/Controls/*.cpp src/Gui/Windows/*.cpp src/Desktop/*.cpp src/Rtc/*.cpp src/Network/Tls/TlsKeys.cpp src/Executable/*.cpp src/Spreadsheet/*.cpp src/Browser/HtmlDocument.cpp src/Browser/Renderer.cpp src/Browser/RendererBoxes.cpp src/Browser/Media.cpp src/Browser/Page.cpp src/Layout/*.cpp src/Paint/*.cpp src/Font/*.cpp src/Browser/Objects.cpp src/Browser/MseClip.cpp src/Browser/Svg.cpp src/Browser/Canvas.cpp src/Browser/DomBindings*.cpp src/Browser/WebGlPrelude.cpp src/Browser/WebGpuPrelude.cpp src/WebGl/*.cpp src/Wasm/*.cpp src/Crypto/*.cpp src/Dom/*.cpp src/Browser/Css.cpp src/Browser/PageLayout.cpp src/Browser/Url.cpp src/Browser/WebUrl.cpp src/Browser/CookieJar.cpp src/Browser/PublicSuffix.cpp src/Browser/Fetcher.cpp src/Browser/WebPrelude.cpp src/Browser/PageHub.cpp src/Browser/CacheStore.cpp src/Browser/ServiceWorker.cpp src/Browser/WebPush.cpp src/Browser/ServiceWorkerPrelude.cpp src/Browser/ResourcePrefetcher.cpp src/Browser/SessionHistory.cpp src/Network/Http/HttpUrl.cpp src/Script/*.cpp src/Regex/Regex.cpp src/Database/*.cpp src/Database/Local/*.cpp src/Graphics/Graphics3D/BuiltinShaders.cpp tests/DesktopStubs.cpp $gui_audio src/FileSystem/FileSystemTypes.cpp
      "$out/GuiThreadTest" ;;
    *) echo "unknown test $t"; exit 2 ;;
  esac
done
