/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "Kernel/Delay.hpp"
#include "Kernel/Firmware.hpp"
#include "Allocators/Allocator.hpp"
#include "Allocators/ThreadSafeAllocator.hpp"
#include "Hardware/Bus/Pci/PciBar.hpp"
#include "Hardware/MmioMapper.hpp"
#include "Hardware/MidiDevice/X86MidiPortIo.hpp"
#include "Hardware/MidiDevice/Usb/UsbMidiService.hpp"
#include "Kernel/Paging.hpp"
#include "CPUScheduler/Scheduler.hpp"
#include "console.hpp"
#include "Hardware/Bus/SystemPciBus.hpp"
#include "Hardware/Bus/Usb/UsbBus.hpp"
#include "Hardware/Printer/SystemPrinters.hpp"
#include "Hardware/GraphicsProcessingUnit/SystemGpu.hpp"
#include "BootLog.hpp"
#include "Desktop/Desktop.hpp"
#include "Graphics/SystemGraphics.hpp"
#include "Hardware/ImageSignalProcessor/SystemImageSignalProcessor.hpp"
#include "Hardware/NeuralProcessingUnit/SystemNeuralProcessingUnit.hpp"
#include "Hardware/NetworkInterface/SystemNetworkInterfaces.hpp"
#include "Hardware/StorageDevice/SystemStorageDevices.hpp"
#include "Hardware/SystemHardware.hpp"
#include "Network/NetworkStackSet.hpp"
#include "Network/Ftp/FtpFileSystem.hpp"
#include "Network/NetworkVolume.hpp"
#include "Network/Nfs/NfsVolume.hpp"
#include "Network/Sftp/SftpFileSystem.hpp"
#include "Network/Smb/SmbVolume.hpp"
#include "Crypto/Random.hpp"
#include "Kernel/Thread.hpp"
#include "Audio/AudioCaptureHub.hpp"
#include "Audio/AudioMixer.hpp"
#include "Audio/AudioRouter.hpp"
#include "Hardware/AudioDevice/SystemAudioDevices.hpp"
#include "Hardware/Camera/SystemCameras.hpp"
#include "Hardware/Clock/TscClock.hpp"
#include "Hardware/VideoCodec/SystemVideoCodecs.hpp"
#include "Media/MediaCodecs.hpp"
#include "Hardware/SystemInput.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Kernel/Clipboard.hpp"
#include "SystemFileSystem.hpp"
#include "SystemMonitor.hpp"
#include "SystemPrintService.hpp"
#include "uefi.hpp"

// TCOS_SELFTESTS (cmake -DTCOS_SELFTESTS=ON) adds the boot-time self-checks and demos of every
// subsystem; the default boot only reports the hardware and opens the desktop.
#ifdef TCOS_SELFTESTS
#include "AsyncDemo.hpp"
#include "AudioDemo.hpp"
#include "ConcurrentDemo.hpp"
#include "ContainerDemo.hpp"
#include "FileSystemDemo.hpp"
#include "GraphicsDemo.hpp"
#include "GuiDemo.hpp"
#include "IspDemo.hpp"
#include "KernelDemo.hpp"
#include "NetworkDemo.hpp"
#include "NetworkStackDemo.hpp"
#include "NpuDemo.hpp"
#include "StorageDemo.hpp"
#include "ThreadSafetyDemo.hpp"
#include "UtilityDemo.hpp"
#endif

// gdb: `set var g_debugger_attached = 1` releases the boot-time spin loop.
extern "C" {
volatile int g_debugger_attached = 0;
// Exported so gdb/scripts can locate the image without symbols.
uint64_t g_image_base = 0;
}

#ifdef TCOS_SELFTESTS
// Scheduler demo. Tasks may run on other cores, where firmware (and so the
// console) is off limits, so they only touch memory; the boot core reports.
struct DemoState {
  hardware::ICentralProcessingUnit* cpu;
  uint32_t done;
  uint64_t cores_used;  // bit n set => core n ran a task
};

static void demo_task(void* context) {
  auto* state = static_cast<DemoState*>(context);
  for (volatile int i = 0; i < 2000000; i = i + 1) {}  // long enough for tasks to overlap
  __atomic_fetch_or(&state->cores_used, 1ull << state->cpu->current_core(), __ATOMIC_SEQ_CST);
  __atomic_fetch_add(&state->done, 1, __ATOMIC_SEQ_CST);
}
#endif

using bootlog::Entry;
using bootlog::Level;

// Waits up to `microseconds` for a key press (any keyboard we have, else the firmware's input).
static void wait_for_key(efi::SystemTable* st, hardware::SystemInput& input, uint64_t microseconds) {
  const uint64_t start = hardware::uptime_us();
  while (hardware::uptime_us() - start < microseconds) {
    hardware::KeyEvent key;
    if (input.keyboards().count() > 0) {
      if (input.keyboard().poll(&key) && key.pressed) return;
    } else {
      efi::InputKey firmware_key;
      if (st->con_in && st->con_in->read_key_stroke(st->con_in, &firmware_key) == efi::kSuccess) return;
    }
    hardware::delay_us(10000);
  }
}

// The taskbar clock: the firmware's real-time clock, whose fields are local time (QEMU: -rtc base=localtime).
// A USB mass-storage device was plugged in or pulled out: say so on the debug console and bring the file explorer up to date.
static void on_storage_change(void* context, StringView text) {
  console::debug_puts("Storage: ");
  console::debug_puts(text);
  console::debug_puts("\n");
  static_cast<desktop::Desktop*>(context)->refresh_explorers();
}

// File > Eject in the file explorer: safely remove the drive holding a volume. The answer is what the notice dialog says.
struct EjectContext {
  SystemFileSystem* files;
  FixedString<192> message;
};
static const char* on_eject(void* context, StringView volume, bool* succeeded) {
  auto* c = static_cast<EjectContext*>(context);
  c->message.assign(volume);
  const SystemFileSystem::EjectStatus status = c->files->eject(volume);
  *succeeded = status == SystemFileSystem::EjectStatus::Ok;
  switch (status) {
    case SystemFileSystem::EjectStatus::Ok: c->message.append(" can be removed safely"); break;
    case SystemFileSystem::EjectStatus::InUse: c->message.append(" is in use: close its files and folders first"); break;
    case SystemFileSystem::EjectStatus::NotEjectable: c->message.append(" cannot be ejected"); break;
    case SystemFileSystem::EjectStatus::NotFound: c->message.append(": no such drive"); break;
    case SystemFileSystem::EjectStatus::Failed: c->message.append(" could not be ejected: the drive did not take its data"); break;
  }
  return c->message.c_str();
}

// A USB keyboard or mouse was plugged in or pulled out: say so on the debug console, and show the pointer only while there is a mouse.
struct InputHotplugContext {
  hardware::SystemInput* input;
  desktop::Desktop* desktop;
};
static void on_input_change(void* context, StringView text) {
  auto* c = static_cast<InputHotplugContext*>(context);
  console::debug_puts("Input: ");
  console::debug_puts(text);
  console::debug_puts("\n");
  c->desktop->window().show_cursor(c->input->mice().count() > 0);
}

static bool read_local_time(void* context, desktop::DateTime* out) {
  auto* st = static_cast<efi::SystemTable*>(context);
  efi::Time t{};
  efi::Status status = efi::kSuccess;
  kernel::firmware([&] { status = st->runtime_services->get_time(&t, nullptr); });
  if (status != efi::kSuccess || t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31 || t.hour > 23 || t.minute > 59)
    return false;
  out->year = t.year;
  out->month = t.month;
  out->day = t.day;
  out->hour = t.hour;
  out->minute = t.minute;
  return true;
}

// Unpredictable bytes from the timing of things, for machines whose CPU has neither RDSEED nor RDRAND (QEMU's default CPU, for one): the cycle
// counter sampled around short waits, hashed. Weaker than a hardware generator, and good enough for what SMB uses it for (client nonces).
struct TimingEntropy final : crypto::IEntropySource {
  bool read(ByteView out) override {
    uint8_t pool[64] = {};
    for (uint64_t i = 0; i < out.size(); ++i) {
      for (int round = 0; round < 24; ++round) {
        const uint64_t t = __builtin_ia32_rdtsc() ^ (hardware::uptime_us() << 17);
        pool[(i + round) % 64] = static_cast<uint8_t>(pool[(i + round) % 64] * 31 + (t ^ (t >> 7) ^ (t >> 15)));
        hardware::delay_us(1);
      }
      uint8_t digest[32];
      crypto::hash(crypto::HashKind::Sha256, ConstByteView(pool, sizeof pool), digest);
      out[i] = digest[i % 32];
    }
    return true;
  }
};

// The local clock as a Windows FILETIME (100 ns steps since 1601), the way SMB logins state the time.
static uint64_t smb_filetime(void* context) {
  desktop::DateTime t;
  if (!read_local_time(context, &t)) return 0;
  int64_t y = t.year, m = t.month;
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.day - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = era * 146097 + doe - 719468;   // since 1970
  const uint64_t seconds = static_cast<uint64_t>(days) * 86400 + t.hour * 3600u + t.minute * 60u + 11644473600ull;
  return seconds * 10000000ull;
}

struct SmbLoginContext {
  network::SmbFileSystem* smb;
};
static bool on_smb_login(void* context, StringView server, StringView user, StringView password) {
  static_cast<SmbLoginContext*>(context)->smb->set_credentials(server, user, password);
  return true;
}

// Network filesystems that sign in with a user name and a password (SFTP, FTPS) share this.
template <class Fs>
struct PasswordLoginContext {
  Fs* fs;
};
template <class Fs>
static bool on_password_login(void* context, StringView server, StringView user, StringView password) {
  static_cast<PasswordLoginContext<Fs>*>(context)->fs->set_credentials(server, user, password);
  return true;
}

struct NfsLoginContext {
  network::NfsFileSystem* nfs;
};
static uint32_t parse_id(StringView text) {
  uint64_t v = 0;
  bool any = false;
  for (char c : text) {
    if (c < '0' || c > '9') break;
    v = v * 10 + static_cast<uint32_t>(c - '0');
    any = true;
    if (v > 0xFFFFFFFEull) return 65534;
  }
  return any ? static_cast<uint32_t>(v) : 65534u;
}
static bool on_nfs_login(void* context, StringView server, StringView uid, StringView gid) {
  static_cast<NfsLoginContext*>(context)->nfs->set_credentials(server, parse_id(uid), parse_id(gid));
  return true;
}

extern "C" efi::Status efi_main(efi::Handle image, efi::SystemTable* st) {
  console::init(st);
  hardware::delay_init(st);  // timing that works on every core, before anything waits
  st->boot_services->set_watchdog_timer(0, 0, 0, nullptr);  // no 5-minute reset

  efi::LoadedImage* li = nullptr;
  st->boot_services->handle_protocol(image, &efi::kLoadedImageGuid,
                                     reinterpret_cast<void**>(&li));
  if (li) g_image_base = reinterpret_cast<uint64_t>(li->image_base);

  // Line parsed by scripts/tcos.gdb to relocate symbols.
  console::puts("TCOS_IMAGE_BASE=");
  console::put_hex(g_image_base);
  console::puts("\n");
  console::puts("TheoryCraftOS: initializing hardware\n");

#ifdef TCOS_DEBUG_WAIT
  // Gives a debugger time to attach (IDE) or lets you flip the flag by hand.
  console::puts("Waiting for debugger...\n");
  for (int i = 0; i < TCOS_DEBUG_WAIT * 10 && !g_debugger_attached; ++i)
    hardware::delay_us(100000);
#endif

  Entry(Level::Info, "Firmware").text("UEFI ").dec(st->hdr.revision >> 16).text(".").dec((st->hdr.revision & 0xFFFF) / 10);

  // Memory first: the PCI bus keeps its device list in it. (Nothing is lost by
  // being quiet for a moment; log lines also go to the debug console.)
  // Objects live on this stack for the life of the OS; nothing needs static constructors.
  hardware::SystemRam ram(st);
  if (!ram.initialize()) {
    Entry(Level::Fault, "RAM").text("initialization failed");
    return efi::kBufferTooSmall;
  }
  allocators::Allocator allocator(ram);
  if (!allocator.initialize()) {
    Entry(Level::Fault, "Allocator").text("initialization failed");
    return efi::kBufferTooSmall;
  }
  Entry(Level::Ok, "RAM").size(ram.total_bytes()).text(" total, ").size(ram.usable_bytes()).text(" in the allocator pool");

  // Page tables: the firmware identity-maps its memory map, and we add what it left out. First the memory above 4 GiB the RAM class claimed: it goes to the
  // allocator's high pool, which serves everything except what devices read by themselves (allocate_dma stays below 4 GiB).
  kernel::PageTables paging(kernel::cpu_paging_host(), allocator);
  const bool paging_ok = paging.initialize();
  if (!paging_ok) Entry(Level::Warning, "Paging").text("5-level paging: memory above 4 GiB and MMIO mapping are not used");
  uint64_t high_bytes = 0;
  for (uint32_t i = 0; paging_ok && i < ram.extra_region_count(); ++i) {
    const hardware::MemoryRegion r = ram.extra_region(i);
    // (a region the firmware did not map after all is mapped here; one it did map is left as it is)
    if (paging.map_missing(r.base, r.bytes, kernel::PageAttributes()) && paging.is_mapped(r.base, r.bytes) && allocator.add_region(reinterpret_cast<void*>(r.base), r.bytes))
      high_bytes += r.bytes;
  }
  if (high_bytes) Entry(Level::Ok, "RAM").size(high_bytes).text(" above 4 GiB for ordinary allocations (").size(allocator.free_bytes_low()).text(" below 4 GiB left for DMA)");

  // The CPU comes next: the allocator's thread-safe front needs it, and from here on
  // every subsystem allocates through that front, so any thread may use any of them.
  // The CPU, then the scheduler.
  hardware::SystemCpu system_cpu(st);
  hardware::ICentralProcessingUnit& cpu = system_cpu.cpu();
  if (!cpu.initialize()) {
    Entry(Level::Fault, "CPU").text("initialization failed");
    return efi::kBufferTooSmall;
  }
  {
    uint32_t efficiency = 0;
    for (uint32_t i = 0; i < cpu.core_count(); ++i) efficiency += cpu.core_kind(i) == hardware::CoreKind::Efficiency;
    Entry e(Level::Ok, "CPU");
    e.text(cpu.name()).text(" ").text(cpu.model()).text(", ").dec(cpu.core_count()).text(" cores");
    if (efficiency) e.text(" (").dec(efficiency).text(" efficiency)");
  }
  // Everything that may run on several cores shares ONE thread-safe front for the
  // allocator: the scheduler's queues grow through it, and so do the concurrent
  // containers. (Two separate wrappers would use two locks and not exclude each other.)
  allocators::ThreadSafeAllocator shared_allocator(allocator, cpu);

  // Then the display, so everything after it appears on screen. Falls back to the
  // firmware text console if no GPU class can drive a framebuffer.
  hardware::SystemPciBus system_pci(st, shared_allocator);
  hardware::IPciBus& pci = system_pci.pci();
  // Device registers the firmware left unmapped (64-bit BARs placed above its map) become reachable before any driver wants them.
  hardware::MmioMapper mmio(paging, ram);
  if (paging_ok) {
    const uint32_t mapped_bars = hardware::pci_map_all_bars(pci, mmio);
    if (mapped_bars) Entry(Level::Info, "MMIO").dec(mapped_bars).text(" BAR(s) mapped that the firmware had not mapped");
  }
  hardware::SystemGpu system_gpu(st, pci, shared_allocator);
  hardware::IGraphicsProcessingUnit* gpu = system_gpu.gpu();
  if (gpu) {
    // Switch to 1024x768 if the GPU offers it. The screen is cleared by a mode change, so
    // do it before anything is printed.
    for (uint32_t i = 0; i < gpu->mode_count(); ++i) {
      hardware::DisplayMode m;
      if (gpu->mode_info(i, &m) && m.width == 1024 && m.height == 768 && gpu->set_mode(i)) break;
    }
    console::use_gpu(gpu);
    console::puts("TheoryCraftOS: initializing hardware\n");
    bootlog::replay();  // the mode change cleared what was logged so far
    Entry(Level::Ok, "GPU").text(gpu->name()).text(", ").dec(gpu->width()).text("x").dec(gpu->height()).text(", ").dec(gpu->mode_count()).text(" modes");
  } else {
    Entry(Level::Warning, "GPU").text("no usable display; using the firmware text console, the desktop needs a graphics display");
  }
  Entry(Level::Ok, pci.name()).dec(pci.device_count()).text(" devices");

#ifdef TCOS_SELFTESTS
  console::puts("Containers:\n");
  if (!run_container_demo(allocator)) console::puts("container self-check failed\n");
#endif

  scheduling::Scheduler scheduler(cpu, shared_allocator);

  // From here on the OS is preemptively multithreaded on every core.
  if (kernel::start(cpu, shared_allocator)) {
    Entry(Level::Ok, "Kernel").text("preemptive threads on ").dec(kernel::core_count()).text(" cores");
#ifdef TCOS_SELFTESTS
    if (!run_kernel_demo(shared_allocator)) console::puts("  kernel self-check incomplete\n");
#endif
  } else {
    Entry(Level::Fault, "Kernel").text("could not start threads");
  }

  // 2D and 3D rendering: the GPU's 3D engine where we can drive it, the software renderer otherwise.
  graphics::SystemGraphics system_graphics(system_gpu, shared_allocator);
  if (gpu) {
    graphics::IRenderer2D* renderer = system_graphics.renderer_2d();
    if (!renderer) {
      Entry(Level::Fault, "Graphics").text("no memory for a 2D renderer");
    } else {
      Entry(Level::Ok, "Graphics").text("3D: ").text(system_graphics.device().info().name)
          .text(system_graphics.accelerated() ? " (GPU accelerated)" : " (software)").text(", 2D: ").text(renderer->name())
          .text(renderer->accelerated() ? " (GPU accelerated)" : " (software)");
      if (!system_graphics.accelerated() && system_gpu.virtio_gpu())
        Entry(Level::Warning, "Graphics").text("virtio-gpu has no 3D engine here (start QEMU with virtio-gpu-gl for acceleration)");
    }
  }

  // USB comes after the allocator: the xHCI driver needs DMA-able memory.
  hardware::UsbBus usb(pci, shared_allocator, st);
  usb.initialize();
  Entry(Level::Ok, "USB").dec(usb.controller_count()).text(" controllers, ").dec(usb.device_count()).text(" devices");
  for (uint32_t c = 0; c < usb.controller_count(); ++c) {
    for (uint32_t p = 0; p < usb.port_count(c); ++p) {
      hardware::UsbDeviceDescriptor d;
      if (!usb.device_descriptor(c, p, &d)) continue;
      Entry(Level::Info, "USB").text("port ").dec(p).text(": device ").hex(d.vendor_id).text(":").hex(d.product_id).text(" class ").hex(d.device_class);
    }
  }

  Entry(Level::Info, "Allocator").size(allocator.free_bytes()).text(" free");

#ifdef TCOS_SELFTESTS
  DemoState demo{&cpu, 0, 0};
  for (int i = 0; i < 8; ++i) scheduler.submit(demo_task, &demo);
  scheduler.run();
  console::puts("Scheduler finished tasks: ");
  console::put_hex(demo.done);
  console::puts(", core mask: ");
  console::put_hex(demo.cores_used);
  console::puts("\n");
#endif

  // Neural processing unit: a hardware one if the machine has a known NPU,
  // otherwise matrix maths spread over the CPU cores by the scheduler.
  hardware::SystemNeuralProcessingUnit system_npu(pci, cpu, scheduler);
  hardware::INeuralProcessingUnit& npu = system_npu.npu();
  Entry(Level::Ok, "NPU").text(npu.name()).text(", ").dec(npu.compute_units()).text(" compute units");
#ifdef TCOS_SELFTESTS
  if (!run_npu_demo(npu, shared_allocator)) console::puts("NPU self-check failed\n");

  console::puts("Smart pointers and value types:\n");
  if (!run_utility_demo(allocator, scheduler)) console::puts("utility self-check failed\n");

  console::puts("Concurrent containers:\n");
  if (!run_concurrent_demo(allocator, shared_allocator, cpu, scheduler)) console::puts("concurrent self-check failed\n");
#endif

  // Network interfaces: native drivers where we have them, the firmware's driver otherwise.
  hardware::SystemNetworkInterfaces network(pci, shared_allocator, st);
  network.initialize();
  if (network.count() == 0) Entry(Level::Info, "Network").text("no interfaces");
  for (uint32_t i = 0; i < network.count(); ++i) {
    hardware::INetworkInterface& nic = *network.at(i);
    const hardware::MacAddress mac = nic.mac_address();
    Entry e(nic.link_up() ? Level::Ok : Level::Warning, "Network");
    e.text(nic.name()).text(" ");
    for (int b = 0; b < 6; ++b) e.hex2(mac.bytes[b]).text(b < 5 ? ":" : "");
    e.text(nic.link_up() ? ", link up" : ", link DOWN (no cable?)");
#ifdef TCOS_SELFTESTS
    run_network_probe(nic, st);
#endif
  }

  // The network stack proper: IPv4 (ARP, ICMP), UDP, TCP, DHCP and DNS - one stack per
  // network interface, timed by a clock calibrated against the firmware's timer.
  hardware::TscClock clock(cpu, st);
  network::NetworkStackSet stacks(clock, shared_allocator);
  const bool clock_ok = clock.initialize();
  if (!clock_ok) Entry(Level::Warning, "Clock").text("could not calibrate the timer; networking is off");

  // Video codecs: a device per GPU video engine found (not driven yet: its sessions run on the CPU) and the software codec.
  hardware::SystemVideoCodecs video_codecs(pci, shared_allocator);
  video_codecs.initialize();
  for (uint32_t i = 0; i < video_codecs.count(); ++i) {
    hardware::IVideoCodecDevice& codec_device = *video_codecs.at(i);
    const hardware::VideoCodecInfo& info = codec_device.info();
    Entry e(Level::Ok, "Video");
    e.text(codec_device.name()).text(": ");
    bool first = true;
    for (uint32_t c = 0; c < hardware::kVideoCodecCount; ++c) {
      const hardware::VideoCodec codec = static_cast<hardware::VideoCodec>(c);
      if (!codec_device.can_decode(codec) && !codec_device.can_encode(codec)) continue;
      e.text(first ? "" : ", ").text(hardware::codec_name(codec)).text(codec_device.can_decode(codec) ? (codec_device.can_encode(codec) ? " dec/enc" : " dec") : " enc");
      first = false;
    }
    if (info.hardware_engine && !info.accelerated) e.text(" (engine present, not driven yet)");
  }

  // Picture, sound and video files: software codecs for every format, JPEG handed to a video engine first when one really accelerates it.
  {
    const media::MediaCodecs media_codecs(&video_codecs);
    Entry images(Level::Ok, "Media");
    images.text("images:");
    for (uint8_t f = 1; f <= static_cast<uint8_t>(media::ImageFormat::Webp); ++f) images.text(" ").text(media::image_format_name(static_cast<media::ImageFormat>(f)));
    Entry sounds(Level::Ok, "Media");
    sounds.text("sound:");
    for (uint8_t f = 1; f <= static_cast<uint8_t>(media::AudioFileFormat::Midi); ++f) sounds.text(" ").text(media::audio_format_name(static_cast<media::AudioFileFormat>(f)));
    Entry movies(Level::Ok, "Media");
    movies.text("video: AVI (Motion JPEG, raw, H.264), MP4/MOV (H.264), GIF, APNG, Y4M; JPEG on ").text(media_codecs.jpeg_accelerated() ? "a video engine, else the CPU" : "the CPU");
  }

  // Audio outputs: native drivers (HD Audio, AC'97, virtio-sound), or a silent software device.
  hardware::SystemAudioDevices audio(pci, clock, shared_allocator, st, &usb);
  audio.initialize();
  for (uint32_t i = 0; i < audio.count(); ++i) {
    hardware::IAudioDevice& device = *audio.at(i);
    if (audio.is_hardware(i)) {
      Entry(Level::Ok, "Audio").text(device.name()).text(", ").dec(hardware::kAudioSampleRate).text(" Hz 16-bit stereo");
    } else {
      Entry(Level::Info, "Audio").text("no sound hardware found; ").text(device.name());
    }
#ifdef TCOS_SELFTESTS
    if (!run_audio_demo(device)) console::puts("  audio self-check failed\n");
#endif
  }

  // The sound server: the first device belongs to the mixer, which every program's audio goes through.
  UniquePointer<audio::AudioMixer> mixer;
  if (audio.count() > 0) {
    mixer = UniquePointer<audio::AudioMixer>::make(shared_allocator, audio.share(0), shared_allocator);
    if (mixer && mixer->initialize() && mixer->start_service()) {
      Entry(Level::Ok, "Audio").text("mixer running (up to ").dec(audio::AudioMixer::kMaxStreams).text(" streams)");
#ifdef TCOS_SELFTESTS
      if (!run_mixer_demo(*mixer)) console::puts("  mixer self-check failed\n");
#endif
    } else {
      Entry(Level::Warning, "Audio").text("could not start the mixer");
      mixer.reset();
    }
  }

  // The recording side: the first device with an input, shared by every program through the capture hub. The hub exists without one too:
  // a USB microphone plugged in later is taken up by the audio router below.
  UniquePointer<audio::AudioCaptureHub> capture_hub = UniquePointer<audio::AudioCaptureHub>::make(shared_allocator, shared_allocator);
  if (capture_hub && capture_hub->initialize()) {
    for (uint32_t i = 0; i < audio.count(); ++i) {
      hardware::IAudioCapture* input = audio.at(i)->capture();
      if (!input) continue;
      capture_hub->set_source(input, audio.share(i));
      Entry(Level::Ok, "Audio").text("capture running: ").text(input->name());
      break;
    }
    if (capture_hub->start_service()) {
#ifdef TCOS_SELFTESTS
      if (capture_hub->source() && !run_capture_demo(*capture_hub)) console::puts("  capture self-check failed\n");
#endif
    } else {
      Entry(Level::Warning, "Audio").text("could not start capture");
      capture_hub.reset();
    }
  } else {
    Entry(Level::Warning, "Audio").text("could not start capture");
    capture_hub.reset();
  }
  if (!capture_hub || !capture_hub->source()) Entry(Level::Info, "Audio").text("no audio input");

  // USB audio devices come and go: the router keeps the mixer and the capture hub pointed at the right ones.
  audio::AudioRouter audio_router(audio, mixer.get(), capture_hub.get());
  audio_router.set_notifier([](void*, StringView text) {
    console::debug_puts(text);
    console::debug_puts("\n");
  }, nullptr);
  if (!audio_router.start_service()) Entry(Level::Warning, "Audio").text("could not start audio hot-plug polling; USB audio devices plugged in later are not seen");

  // Cameras: USB video class devices. They are only found here; a program starts one and reads its pictures.
  hardware::SystemCameras cameras(usb, shared_allocator);
  cameras.initialize();
  for (uint32_t i = 0; i < cameras.count(); ++i) Entry(Level::Ok, "Camera").text(cameras.at(i)->name()).text(", ").dec(cameras.at(i)->mode_count()).text(" modes");
  if (cameras.count() == 0) Entry(Level::Info, "Camera").text("no camera");
  // The service pumps the cameras that stream and notices cameras plugged in or pulled out later.
  if (!cameras.start_service([](void*, StringView text) {
        console::debug_puts("Camera: ");
        console::debug_puts(text);
        console::debug_puts("\n");
      }, nullptr))
    Entry(Level::Warning, "Camera").text("could not start the camera service; USB cameras plugged in later are not seen");
#ifdef TCOS_SELFTESTS
  UniquePointer<NetworkStackDemo> stack_demo;
  if (clock_ok && !run_async_demo(clock, shared_allocator)) console::puts("  async self-check incomplete\n");
#endif
  if (network.count() > 0 && clock_ok) {
    for (uint32_t i = 0; i < network.count(); ++i)
      if (!stacks.add(*network.at(i))) Entry(Level::Warning, "Network").text("could not start a network stack on interface ").dec(i);
    if (stacks.count() > 0) {
      stacks.start_service();  // one thread keeps every stack polled
      Entry(Level::Ok, "Network").text("stacks running on ").dec(stacks.count()).text(" interfaces (DHCP started)");
#ifdef TCOS_SELFTESTS
      stack_demo = UniquePointer<NetworkStackDemo>::make(shared_allocator, stacks, shared_allocator);
      if (stack_demo && !stack_demo->start()) console::puts("  network stack self-check incomplete\n");
#endif
    }
  }

  // Storage devices: native drivers where we have them, the firmware's driver otherwise.
  hardware::SystemStorageDevices storage(pci, &usb, shared_allocator, st);
  storage.initialize();
  if (storage.count() == 0) Entry(Level::Info, "Storage").text("no devices");
  for (uint32_t i = 0; i < storage.count(); ++i) {
    const hardware::StorageInfo info = storage.at(i)->info();
    Entry e(Level::Ok, "Storage");
    e.text(storage.at(i)->name()).text(" ").text(info.model[0] ? info.model : "(no model)").text(", ")
        .size(info.block_count * info.block_size);
    if (info.read_only) e.text(", read-only");
    if (info.removable) e.text(", removable");
  }
#ifdef TCOS_SELFTESTS
  if (!run_storage_demo(storage, shared_allocator)) console::puts("  storage self-check incomplete\n");
  if (!run_filesystem_demo(storage, shared_allocator)) console::puts("  filesystem self-check incomplete\n");

  console::puts("Thread safety (concurrent use of the shared subsystems):\n");
  if (!run_thread_safety_demo(storage, stacks, shared_allocator)) console::puts("  thread-safety self-check incomplete\n");
#endif

  // Image signal processor: a hardware one if the machine has a known ISP,
  // otherwise the software pipeline.
  hardware::SystemImageSignalProcessor system_isp(pci);
  hardware::IImageSignalProcessor& isp = system_isp.isp();
  Entry(Level::Ok, "ISP").text(isp.name());
#ifdef TCOS_SELFTESTS
  const bool isp_drawn = gpu && run_isp_demo(*gpu, isp, shared_allocator);
  if (gpu) {
    // These demos take over the whole screen, so the console is attached afresh afterwards.
    const bool graphics_ok = run_graphics_demo(system_graphics, shared_allocator);
    const bool gui_ok = run_gui_demo(system_graphics, shared_allocator);
    console::use_gpu(gpu);
    Entry(isp_drawn ? Level::Ok : Level::Fault, "Self-test").text("ISP demo ").text(isp_drawn ? "drawn" : "failed");
    Entry(graphics_ok ? Level::Ok : Level::Fault, "Self-test").text("graphics demo ").text(graphics_ok ? "ok" : "FAILED");
    Entry(gui_ok ? Level::Ok : Level::Fault, "Self-test").text("GUI demo ").text(gui_ok ? "ok" : "FAILED");
  }
#endif

  // Input devices: PS/2, USB HID and virtio-input, merged into one keyboard and mouse.
  hardware::SystemInput input(pci, usb, shared_allocator, st);
  input.initialize();
  Entry(input.keyboards().count() ? Level::Ok : Level::Warning, "Input").dec(input.keyboards().count()).text(" keyboards");
  for (uint32_t i = 0; i < input.keyboards().count(); ++i) Entry(Level::Info, "Input").text("  ").text(input.keyboards().at(i)->name());
  Entry(input.mice().count() ? Level::Ok : Level::Warning, "Input").dec(input.mice().count()).text(" mice");
  for (uint32_t i = 0; i < input.mice().count(); ++i) Entry(Level::Info, "Input").text("  ").text(input.mice().at(i)->name());

  // Printers: those on USB are found on the bus (network printers are added by address, by whoever knows it).
  hardware::SystemPrinters printers(shared_allocator);
  printers.discover_usb(usb);
  Entry(Level::Ok, "Printer").dec(printers.count()).text(" USB printers");
  for (uint32_t i = 0; i < printers.count(); ++i) Entry(Level::Info, "Printer").text("  ").text(printers.at(i)->name());
  // USB printers come and go: the service watches for that and delivers the USB printers' jobs.
  if (!printers.start_service([](void*, StringView text) {
        console::debug_puts("Printer: ");
        console::debug_puts(text);
        console::debug_puts("\n");
      }, nullptr))
    Entry(Level::Warning, "Printer").text("could not start the printer service; USB printers plugged in later are not seen and jobs are not delivered");

  // The log is read before the desktop covers it: a clean boot pauses a moment, a boot with warnings
  // or failures waits (up to 15 s) for a key so they can be read.
  const uint32_t warnings = bootlog::warnings(), faults = bootlog::faults();
  {
    console::Guard guard;
    console::puts("\n");
    if (faults || warnings) {
      Entry(faults ? Level::Fault : Level::Warning, "Boot").dec(faults).text(" failures, ").dec(warnings).text(" warnings: press a key to continue");
    } else {
      Entry(Level::Ok, "Boot").text("hardware ready, starting the desktop");
    }
  }
  wait_for_key(st, input, faults || warnings ? 15000000 : 1500000);

  // MIDI (set up before the desktop, which is gone first: songs on the hardware are silenced when the player closes): an MPU-401 port if the machine has one, and the USB MIDI devices (keyboards, controllers, synthesizers). A song is played on the hardware
  // that makes sound when there is some, else synthesized by the media player; what a keyboard plays goes to the same hardware or, with none, to
  // a software synthesizer through the mixer.
  hardware::X86MidiPortIo midi_ports;
  hardware::Mpu401Output midi_usual(midi_ports, hardware::Mpu401Output::kUsualBase);   // (0x300, the other base, is a network card's)
  hardware::UsbMidiService usb_midi(&usb, mixer.get(), shared_allocator);
  if (midi_usual.initialize()) {
    usb_midi.set_fallback_output(&midi_usual);
    Entry(Level::Ok, "MIDI").text("MPU-401 output at port 0x330");
  }
  if (!usb_midi.start()) Entry(Level::Warning, "MIDI").text("could not start the USB MIDI service; keyboards plugged in later are not seen");
  for (uint32_t i = 0; i < usb_midi.device_count(); ++i) {
    hardware::UsbMidiService::DeviceInfo info;
    if (usb_midi.device_info(i, &info))
      Entry(Level::Ok, "MIDI").text(info.name).text(info.input ? (info.output ? ": keys and output" : ": keys") : ": output").text(info.synthesizer ? ", a synthesizer" : "");
  }

  // The desktop: runs until Shut Down is chosen from the Start menu (or, with no display, until a key).
  graphics::IRenderer2D* renderer = gpu ? system_graphics.renderer_2d() : nullptr;
  if (renderer) {
    // The merged keyboard and mouse are always handed over, even when there are none yet: USB ones may be plugged in later.
    kernel::Clipboard clipboard(shared_allocator);  // copy and paste between the desktop's programs; outlives the desktop
    desktop::Desktop desktop(shared_allocator, *renderer, &input.keyboard(), &input.mouse());
    desktop.window().show_cursor(input.mice().count() > 0);
    InputHotplugContext input_hotplug{&input, &desktop};
    input.set_notifier(on_input_change, &input_hotplug);
    if (!input.start_service()) Entry(Level::Warning, "Input").text("could not start hot-plug polling; USB keyboards and mice plugged in later are not seen");
    // Network shares (SMB, NFS, SFTP, FTPS): folders of the explorer's tree whose contents are the shares of the servers the user goes to.
    TimingEntropy timing_entropy;
    crypto::CpuEntropySource cpu_entropy;
    crypto::DrbgRandom smb_random(crypto::CpuEntropySource::available() ? static_cast<crypto::IEntropySource&>(cpu_entropy) : static_cast<crypto::IEntropySource&>(timing_entropy));
    network::StackSetNetwork smb_network(stacks);
    network::SmbEnvironment smb_environment;
    smb_environment.random = &smb_random;
    smb_environment.filetime = smb_filetime;
    smb_environment.context = st;
    network::SmbFileSystem smb(smb_network, shared_allocator, smb_environment);
    SmbLoginContext smb_login{&smb};
    network::NfsFileSystem nfs(smb_network, shared_allocator);
    NfsLoginContext nfs_login{&nfs};
    network::HostTrust host_trust(shared_allocator);   // the host keys and certificates seen first
    network::SftpFileSystem sftp(smb_network, shared_allocator, smb_random, host_trust);
    PasswordLoginContext<network::SftpFileSystem> sftp_login{&sftp};
    network::FtpEnvironment ftp_environment;
    ftp_environment.random = &smb_random;
    ftp_environment.trust = &host_trust;
    network::FtpFileSystem ftps(smb_network, shared_allocator, ftp_environment);
    PasswordLoginContext<network::FtpFileSystem> ftps_login{&ftps};
    SystemFileSystem explorer_files(shared_allocator, storage);
    if (stacks.count() > 0) {
      SharedPointer<network::NfsVolume> nfs_volume = SharedPointer<network::NfsVolume>::make(shared_allocator, nfs);
      if (nfs_volume && explorer_files.mounts().mount(desktop::FileExplorer::kNfsFolder, SharedPointer<filesystem::IVolume>(nfs_volume), 0xFFFFFFFFu))
        desktop.explorer().set_nfs_login_handler(on_nfs_login, &nfs_login);
    }
    if (stacks.count() > 0 && smb_random.initialize()) {
      SharedPointer<network::SmbVolume> smb_volume = SharedPointer<network::SmbVolume>::make(shared_allocator, smb);
      if (smb_volume && explorer_files.mounts().mount(desktop::FileExplorer::kNetworkFolder, SharedPointer<filesystem::IVolume>(smb_volume), 0xFFFFFFFFu))
        desktop.explorer().set_login_handler(on_smb_login, &smb_login);
      SharedPointer<network::NetworkVolume<network::SftpFileSystem>> sftp_volume = SharedPointer<network::NetworkVolume<network::SftpFileSystem>>::make(shared_allocator, sftp);
      if (sftp_volume && explorer_files.mounts().mount(desktop::FileExplorer::kSftpFolder, SharedPointer<filesystem::IVolume>(sftp_volume), 0xFFFFFFFFu))
        desktop.explorer().set_sftp_login_handler(on_password_login<network::SftpFileSystem>, &sftp_login);
      SharedPointer<network::NetworkVolume<network::FtpFileSystem>> ftps_volume = SharedPointer<network::NetworkVolume<network::FtpFileSystem>>::make(shared_allocator, ftps);
      if (ftps_volume && explorer_files.mounts().mount(desktop::FileExplorer::kFtpsFolder, SharedPointer<filesystem::IVolume>(ftps_volume), 0xFFFFFFFFu))
        desktop.explorer().set_ftps_login_handler(on_password_login<network::FtpFileSystem>, &ftps_login);
    }
    SystemPrintService print_service(shared_allocator, printers);
    desktop.set_print_service(&print_service);
    desktop.set_media_services(mixer.get(), &video_codecs, &clock);
    desktop.set_midi_output(&usb_midi);
    desktop.set_clipboard(&clipboard);
    desktop.set_file_system(&explorer_files.file_system());
    explorer_files.set_notifier(on_storage_change, &desktop);
    EjectContext eject_context{&explorer_files, {}};
    desktop.explorer().set_eject_handler(on_eject, &eject_context);
    if (!explorer_files.start_service()) Entry(Level::Warning, "Storage").text("could not start hot-plug polling; USB sticks plugged in later are not seen");
    SystemMonitor monitor(allocator, shared_allocator);
    desktop.set_system_monitor(&monitor);
    desktop.set_clock_source(read_local_time, st);
    desktop.set_diagnostic_output(console::debug_puts);
    // Three threads: this one reads input and the clock (firmware calls must stay on the boot core), a render
    // thread redraws whenever something changed, and a handler thread runs what buttons and menus fire - so a
    // slow click handler can freeze neither the screen nor the pointer.
    const bool threaded = desktop.start();
    if (!threaded) Entry(Level::Warning, "Desktop").text("could not start the render and handler threads; running single-threaded");
    while (!desktop.shutdown_requested()) {
      if (threaded) {
        desktop.pump(hardware::uptime_us());
      } else {
        desktop.step(hardware::uptime_us());
      }
      hardware::delay_us(4000);
    }
    input.stop_service();  // (its notices touch the desktop)
    desktop.stop();  // before anything else draws on the screen: the render thread is the renderer's only user
  } else {
    console::puts("No desktop (no graphics display). Press any key to shut down.\n");
    wait_for_key(st, input, ~0ull >> 1);
  }

  // Orderly shutdown, back on the console. Services stop before the hardware under them, and storage
  // is flushed before the devices are handed back to the firmware.
  if (gpu) {
    console::use_gpu(gpu);
    console::puts("TheoryCraftOS: shutting down\n");
  }
  audio_router.stop_service();   // (it points the mixer and the hub at devices)
  capture_hub.reset();
  mixer.reset();   // the mixer thread ends before its audio device goes
  cameras.stop_service();
  printers.stop_service();   // (its thread ends before the threads do: joining it afterwards would wait for ever)
  stacks.clear();  // the stacks go before the NICs underneath them
#ifdef TCOS_SELFTESTS
  stack_demo.reset();
#endif
  Entry(Level::Ok, "Shutdown").text("network stacks stopped");
  kernel::stop();  // threads end here; the rest of shutdown talks to the firmware
  Entry(Level::Ok, "Shutdown").text("threads stopped");
  input.shutdown();
  for (uint32_t i = 0; i < storage.count(); ++i) {
    hardware::IStorageDevice& device = *storage.at(i);
    if (device.info().read_only) continue;
    if (!device.flush()) Entry(Level::Warning, "Shutdown").text("could not flush ").text(device.name());
  }
  storage.shutdown();
  Entry(Level::Ok, "Shutdown").text("storage flushed and released");
  audio.shutdown();
  cameras.shutdown();
  video_codecs.shutdown();
  network.shutdown();
  printers.shutdown();
  usb.shutdown();  // resets the USB controllers and returns them to the firmware
  Entry(Level::Ok, "Shutdown").text("USB and network released");

  allocator.deallocate(allocator.allocate(1));  // exercise the pool once
  ram.shutdown();
  cpu.shutdown();
  if (hardware::VirtioGpu* virtio = system_gpu.virtio_gpu(); virtio && virtio->timeouts())
    Entry(Level::Warning, "Shutdown").text("virtio-gpu: ").dec(virtio->timeouts()).text(" commands timed out (the host display was not answering)");
  Entry(Level::Ok, "Shutdown").text("powering off");

  st->runtime_services->reset_system(efi::ResetType::Shutdown, efi::kSuccess, 0, nullptr);
  for (;;) cpu.halt();
}
