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
#include "Hardware/GraphicsProcessingUnit/Multi/GpuPool.hpp"
#include "Hardware/GraphicsProcessingUnit/SystemGpu.hpp"
#include "Hardware/Monitor/Edid.hpp"
#include "Hardware/Monitor/MonitorDisplay.hpp"
#include "Hardware/Monitor/SystemMonitors.hpp"
#include "SystemInventory.hpp"
#include "SystemNetworkDetails.hpp"
#include "SystemWirelessDetails.hpp"
#include "SystemBluetoothDetails.hpp"
#include "BootLog.hpp"
#include "Graphics/Logo/Logo.hpp"
#include "Utility/AllocatedView.hpp"
#include "Desktop/Desktop.hpp"
#include "Graphics/Graphics3D/WebGlAccelerator.hpp"
#include "Graphics/GraphicsPool.hpp"
#include "Graphics/SystemDxg.hpp"
#include "Kernel/Dxgkrnl/Adapter.hpp"
#include "Kernel/Dxgkrnl/Conformance.hpp"
#include "Graphics/SystemGraphics.hpp"
#include "WebGl/Accelerator.hpp"
#include "Hardware/ImageSignalProcessor/SystemImageSignalProcessor.hpp"
#include "Hardware/NeuralProcessingUnit/SystemNeuralProcessingUnit.hpp"
#include "Hardware/NetworkInterface/SystemNetworkInterfaces.hpp"
#include "Hardware/StorageDevice/SystemStorageDevices.hpp"
#include "Hardware/SystemHardware.hpp"
#include "Network/NetworkStackSet.hpp"
#include "Process/Sockets.hpp"
#include "Network/Ftp/FtpFileSystem.hpp"
#include "Network/NetworkVolume.hpp"
#include "Network/Nfs/NfsVolume.hpp"
#include "Network/Sftp/SftpFileSystem.hpp"
#include "Network/Smb/SmbVolume.hpp"
#include "Crypto/Random.hpp"
#include "Kernel/Thread.hpp"
#if defined(__x86_64__)
#include "Kernel/UserMode.hpp"
#endif
#include "Audio/AudioCaptureHub.hpp"
#include "Audio/AudioMixer.hpp"
#include "Audio/AudioRouter.hpp"
#include "Hardware/AudioDevice/SystemAudioDevices.hpp"
#include "Hardware/Camera/SystemCameras.hpp"
#include "Hardware/SerialPort/SystemSerialPorts.hpp"
#include "Hardware/SerialPort/X86SerialPortIo.hpp"
#include "Hardware/Clock/TscClock.hpp"
#include "Hardware/VideoCodec/SystemVideoCodecs.hpp"
#ifdef TCOS_AMD_VEGAM
#include "Hardware/GraphicsProcessingUnit/AMD/AmdVegaM.hpp"
#include "Hardware/VideoCodec/AMD/AmdGmc8Memory.hpp"
#include "Hardware/Bus/Pci/PciMsi.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdVbios.hpp"
#include "Hardware/Acpi/AcpiTables.hpp"
#include "Kernel/Apic.hpp"
#include "Kernel/Interrupts.hpp"
#endif
#include "Media/MediaCodecs.hpp"
#include "Hardware/Guest/SystemGuestServices.hpp"
#include "Hardware/HyperV/SystemHyperV.hpp"
#include "Hardware/VMware/VmwareTools.hpp"
#include "Hardware/SystemInput.hpp"
#include "Hardware/Mouse/I2c/PlatformI2cHid.hpp"
#ifdef TCOS_BLUETOOTH
#include "Hardware/Bluetooth/BluetoothService.hpp"
#include "Hardware/Bluetooth/HidOverGatt.hpp"
#include "Hardware/WirelessInterface/Intel/IwlDriver.hpp"      // (FileSystemFirmwareSource reads /firmware)
#endif
#ifdef TCOS_INTEL_WIFI
#include "Hardware/WirelessInterface/Intel/IwlWireless.hpp"
#endif
#ifdef TCOS_THUNDERBOLT
#include "Hardware/Bus/Pci/PciMsi.hpp"
#include "Hardware/Bus/Pci/PciMsix.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltDriver.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltNet.hpp"
#include "Kernel/Apic.hpp"
#include "Kernel/Interrupts.hpp"
#endif
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Kernel/Clipboard.hpp"
#include "Program/ProgramRunner.hpp"
#include "SystemFileSystem.hpp"
#include "Browser/HttpPageLoader.hpp"
#include "Browser/WebRandom.hpp"
#include "Network/Tls/CaBundle.hpp"
#include "Database/SystemDatabases.hpp"
#include "Database/Local/SystemSettings.hpp"
#include "Hardware/Acpi/SystemAcpiHost.hpp"
#include "Hardware/Acpi/SystemAcpiPorts.hpp"
#include "Hardware/Acpi/AcpiEvents.hpp"
#include "Hardware/Backlight/AcpiBacklight.hpp"
#include "Hardware/Backlight/AcpiVideoKeys.hpp"
#include "Hardware/Backlight/IntelPchBacklight.hpp"
#include "Hardware/Acpi/IoApic.hpp"
#include "Hardware/Battery/AcpiBattery.hpp"
#include "SystemMonitor.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "SystemPrintService.hpp"
#include "uefi.hpp"

// TCOS_SELFTESTS (cmake -DTCOS_SELFTESTS=ON) adds the boot-time self-checks and demos of every
// subsystem; the default boot only reports the hardware and opens the desktop.
#include "GraphicsDemo.hpp"
#ifdef TCOS_SELFTESTS
#include "AsyncDemo.hpp"
#include "AudioDemo.hpp"
#include "ConcurrentDemo.hpp"
#include "ContainerDemo.hpp"
#include "FileSystemDemo.hpp"
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
#if defined(TCOS_USERMODE_DEMO) || defined(TCOS_WINE_TEST_PROGRAM)
#include "UserModeDemo.hpp"
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

#ifdef TCOS_THUNDERBOLT
// The Thunderbolt host interface's MSI: the driver reads the chip's notify registers (which lets it interrupt again) and counts; the desktop loop then reads the control channel.
static kernel::Frame* on_thunderbolt_interrupt(kernel::Frame* frame, void* driver) {
  kernel::apic::eoi();
  static_cast<hardware::ThunderboltDriver*>(driver)->on_interrupt();
  return frame;
}
#endif

#ifdef TCOS_ACPI_EVENTS
// What the buttons of the laptop did, counted where the events are polled and acted on in the desktop loop.
struct AcpiButtonSink final : hardware::IAcpiButtonListener {
  uint32_t power = 0, sleep = 0, lid_closed = 0, lid_opened = 0;
  void on_button(hardware::AcpiButton button, bool lid_open) override {
    if (button == hardware::AcpiButton::Power) ++power;
    else if (button == hardware::AcpiButton::Sleep) ++sleep;
    else if (lid_open) ++lid_opened;
    else ++lid_closed;
  }
};

// The system control interrupt: the handler masks the I/O APIC input (it is level triggered: it stays up until the event registers are cleared) and says so; the loop reads the events and unmasks.
struct AcpiSciState {
  hardware::IoApic* ioapic = nullptr;
  uint32_t gsi = 0;
  uint32_t pending = 0, arrived = 0;
};
static kernel::Frame* on_acpi_sci(kernel::Frame* frame, void* context) {
  kernel::apic::eoi();
  AcpiSciState* sci = static_cast<AcpiSciState*>(context);
  sci->ioapic->mask(sci->gsi, true);
  __atomic_fetch_add(&sci->arrived, 1, __ATOMIC_ACQ_REL);
  __atomic_store_n(&sci->pending, 1, __ATOMIC_RELEASE);
  return frame;
}
#endif

#ifdef TCOS_THUNDERBOLT
// The firmware says Notify(device, 2) when the controller (or a root port above it) woke the machine: counted, for the desktop loop to act on.
struct ThunderboltWakeListener final : hardware::IAcpiNotifyListener {
  hardware::AcpiEvents* events = nullptr;
  acpi::Node* controller = nullptr;
  uint32_t wakes = 0;
  void on_notify(acpi::Node* device, uint32_t value) override {
    if (value == 2 && events && controller && events->is_within(device, controller)) ++wakes;
  }
};
#endif

#ifdef TCOS_AMD_VEGAM
// The Radeon RX Vega M's MSI: counts the interrupts; the desktop loop then reads the card's interrupt ring (no register is touched here).
static uint32_t g_vega_interrupts = 0;
static kernel::Frame* on_vega_interrupt(kernel::Frame* frame, void*) {
  kernel::apic::eoi();
  __atomic_fetch_add(&g_vega_interrupts, 1, __ATOMIC_ACQ_REL);
  return frame;
}

// What is on a connector, from its monitor's EDID over the DDC lines: "DELL U2720Q 2560x1440", the maker's name where the monitor gives none, or nothing. False if the connector
// cannot be read (no DDC line, DisplayPort) or nothing answered.
static bool describe_vega_monitor(hardware::AmdVegaM& card, const hardware::AtomConnectors::Connector& connector, allocators::IAllocator& allocator, FixedString<64>* text) {
  ByteBuffer raw(allocator);
  if (!card.read_edid(connector, &raw)) return false;
  hardware::EdidInfo info;
  if (!hardware::parse_edid(raw.view(), &info)) return false;
  text->assign("");
  if (info.name[0]) {
    text->append(StringView(info.name));
  } else {
    text->append(StringView(hardware::edid_vendor_name(info.manufacturer)));
  }
  hardware::EdidTiming best;
  if (info.best_timing(&best) && best.width && best.height) {
    text->append(StringView(" "));
    text->append_uint(best.width);
    text->append(StringView("x"));
    text->append_uint(best.height);
  }
  return true;
}
#endif

// Waits up to `microseconds` for a key press (any keyboard we have, else the firmware's input).
// The logo at the top left of the boot screen, the log below it. Skipped quietly if it cannot be decoded or there is no memory for it.
static void show_boot_logo(allocators::IAllocator& allocator) {
  constexpr uint32_t kSize = 128, kBackground = 0x101018;   // (the console's own background)
  graphics::Logo logo(allocator);
  ArrayView<uint32_t> pixels = allocate_view<uint32_t>(allocator, uint64_t{kSize} * kSize);
  if (!pixels.empty() && logo.load(graphics::embedded_logo_png()) && logo.render(kSize, kBackground, pixels))
    console::show_banner(ArrayView<const uint32_t>(pixels.data(), pixels.size()), kSize, kSize);
  release_view(allocator, pixels);
}

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
// VirtualBox asks for a window size: the nearest resolution the display can show that fits it.
struct VBoxDisplayContext {
  hardware::SystemMonitors* monitors;
  desktop::Desktop* desktop;
};
static void on_vbox_display_request(void* context, uint32_t width, uint32_t height) {
  auto* c = static_cast<VBoxDisplayContext*>(context);
  const hardware::MonitorSlot* primary = c->monitors->layout().primary();
  if (!primary) return;
  hardware::MonitorMode modes[32];
  const uint32_t count = c->monitors->compatible_resolutions(primary->id, modes, 32);
  const hardware::MonitorMode* best = nullptr;
  for (uint32_t i = 0; i < count; ++i) {
    if (modes[i].width > width || modes[i].height > height) continue;
    if (!best || uint64_t{modes[i].width} * modes[i].height > uint64_t{best->width} * best->height) best = &modes[i];
  }
  if (!best || (best->width == primary->physical_width && best->height == primary->physical_height)) return;
  if (c->monitors->set_resolution(primary->id, best->width, best->height)) c->desktop->apply_monitor_changes();
}

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

// The wall clock for the filesystems. The firmware clock can only be read here on the boot core, so it is read now and then and the time in
// between comes from the uptime counter: g_unix_us_at_boot is the (UTC) time of uptime 0 in microseconds, 0 while the clock is unknown.
static uint64_t g_unix_us_at_boot = 0;

static void sync_wall_clock(efi::SystemTable* st) {
  efi::Time t{};
  efi::Status status = efi::kSuccess;
  kernel::firmware([&] { status = st->runtime_services->get_time(&t, nullptr); });
  if (status != efi::kSuccess || t.year < 1970 || t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31 || t.hour > 23 || t.minute > 59 || t.second > 59) return;
  int64_t seconds = filesystem::days_from_civil(t.year, t.month, t.day) * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
  int32_t local_offset = 0;
  if (t.time_zone != efi::kUnspecifiedTimeZone && t.time_zone >= -1440 && t.time_zone <= 1440) {
    seconds += int64_t{t.time_zone} * 60;  // (UTC = local + zone)
    local_offset = -int32_t{t.time_zone} * 60;
  }
  filesystem::set_local_offset_seconds(local_offset);
  if (seconds <= 0) return;
  __atomic_store_n(&g_unix_us_at_boot, static_cast<uint64_t>(seconds) * 1000000ull - hardware::uptime_us(), __ATOMIC_RELAXED);
}

static uint64_t wall_clock_seconds(void*) {
  const uint64_t at_boot = __atomic_load_n(&g_unix_us_at_boot, __ATOMIC_RELAXED);
  return at_boot ? (at_boot + hardware::uptime_us()) / 1000000ull : 0;
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

// Seconds since 1970 for certificate dates (the local clock; 0 when it cannot be read, which skips the date checks).
static uint64_t unix_seconds(void* context) {
  const uint64_t filetime = smb_filetime(context);
  return filetime ? filetime / 10000000ull - 11644473600ull : 0;
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
#if defined(__x86_64__)
  kernel::usermode::set_memory_source(
      [](void* context, uint64_t* total_bytes, uint64_t* free_bytes) {      // (what Windows programs are told about the machine's memory)
        const allocators::Allocator& heap = *static_cast<const allocators::Allocator*>(context);
        *total_bytes = heap.total_bytes();
        *free_bytes = heap.free_bytes();
      },
      &allocator);
#endif
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
  struct MmioRegisterMapper final : hardware::IRegisterMapper {
    hardware::MmioMapper& mapper;
    explicit MmioRegisterMapper(hardware::MmioMapper& m) : mapper(m) {}
    bool map_registers(uint64_t physical, uint64_t length, hardware::MmioRegion* region) override { return mapper.ensure_mapped(physical, length, region); }
  } register_mapper(mmio);
  // Intel integrated GPUs get native mode setting through their registers; the others use the framebuffer the firmware set up.
  hardware::SystemGpu system_gpu(st, pci, shared_allocator, paging_ok ? &register_mapper : nullptr);
  hardware::IGraphicsProcessingUnit* gpu = system_gpu.gpu();
  if (gpu) {
    // Switch to 1024x768 if the GPU offers it. The screen is cleared by a mode change, so
    // do it before anything is printed.
    for (uint32_t i = 0; i < gpu->mode_count(); ++i) {
      hardware::DisplayMode m;
      if (gpu->mode_info(i, &m) && m.width == 1024 && m.height == 768 && gpu->set_mode(i)) break;
    }
    console::use_gpu(gpu);
    show_boot_logo(shared_allocator);
    console::puts("TheoryCraftOS: initializing hardware\n");
    bootlog::replay();  // the mode change cleared what was logged so far
    Entry(Level::Ok, "GPU").text(gpu->name()).text(", ").dec(gpu->width()).text("x").dec(gpu->height()).text(", ").dec(gpu->mode_count()).text(" modes");
    if (hardware::AmdGpu* amd = system_gpu.amd_gpu())
      Entry(amd->native_available() ? Level::Ok : Level::Info, "GPU").text(amd->native_available() ? "display engine: own framebuffer in video memory (AMD DCN 1.0), the firmware sets the modes" : "firmware framebuffer (own framebuffer needs a Raven-family APU)");
    if (hardware::IntelGpu* intel = system_gpu.intel_gpu())
      Entry(intel->native_available() ? Level::Ok : Level::Info, "GPU").text(intel->native_available() ? "native mode setting (Intel display engine)" : "firmware framebuffer (native mode setting needs a supported Intel GPU)");
  } else {
    Entry(Level::Warning, "GPU").text("no usable display; using the firmware text console, the desktop needs a graphics display");
  }
  Entry(Level::Ok, pci.name()).dec(pci.device_count()).text(" devices");

  // Every display controller, not only the one on screen: the driver of each that has a native one is built beside the boot display's, and a discrete
  // GPU beside the CPU's graphics sleeps until something needs it.
  hardware::GpuPool gpu_pool(pci, shared_allocator);
  gpu_pool.initialize();
  gpu_pool.bind(st, pci, system_gpu);
  if (gpu_pool.count() > 1) {
    for (uint32_t i = 0; i < gpu_pool.count(); ++i) {
      const hardware::GpuNode& n = *gpu_pool.node(i);
      Entry(Level::Info, "GPU").text(n.adapter.name).text(" (").text(hardware::gpu_kind_name(n.adapter.kind))
          .text(static_cast<int32_t>(i) == gpu_pool.manager().display_index() ? ", display" : gpu_pool.manager().asleep(i) ? ", asleep" : "")
          .text(n.gpu ? "" : ", no driver").text(")");
    }
  }

  // The monitors the display device has (their EDIDs tell what they are) and how they are arranged on the desktop. The layout is applied
  // just before the desktop starts, so that the boot console stays a plain screen.
  hardware::SystemMonitors monitors(gpu, shared_allocator);
  if (gpu) {
    monitors.initialize();
    for (uint32_t i = 0; i < monitors.count(); ++i) {
      const hardware::Monitor& m = monitors.monitor(i);
      Entry e(m.connected() ? Level::Ok : Level::Info, "Monitor");
      e.text(m.name());
      if (m.connector_name()[0]) e.text(" on ").text(m.connector_name());
      if (m.connected()) e.text(", ").dec(m.current_mode().width).text("x").dec(m.current_mode().height);
      else e.text(", not connected");
      if (m.edid() && m.width_mm()) e.text(", ").dec(m.width_mm()).text("x").dec(m.height_mm()).text(" mm");
      if (!m.edid() && m.connected() && monitors.multi_display()) e.text(", no EDID");
    }
  }

#ifdef TCOS_SELFTESTS
  console::puts("Containers:\n");
  if (!run_container_demo(allocator)) console::puts("container self-check failed\n");
#endif

  scheduling::Scheduler scheduler(cpu, shared_allocator);

  // From here on the OS is preemptively multithreaded on every core.
  if (kernel::start(cpu, shared_allocator)) {
    Entry(Level::Ok, "Kernel").text("preemptive threads on ").dec(kernel::core_count()).text(" cores");
    // The virtio-gpu's commands finish with an interrupt now (the host takes milliseconds for some; a thread waiting for one need not look again and again).
    if (hardware::VirtioGpu* virtio = system_gpu.virtio_gpu()) {
      if (virtio->enable_interrupts()) Entry(Level::Ok, "GPU").text("virtio-gpu commands complete by interrupt (MSI-X)");
      else Entry(Level::Info, "GPU").text("virtio-gpu has no usable interrupt; waiting for commands by polling");
    }
#ifdef TCOS_SELFTESTS
    if (!run_kernel_demo(shared_allocator)) console::puts("  kernel self-check incomplete\n");
#endif
#ifdef TCOS_USERMODE_DEMO
    run_usermode_demo(shared_allocator);
#endif
  } else {
    Entry(Level::Fault, "Kernel").text("could not start threads");
  }

  // 2D and 3D rendering: the GPU's 3D engine where we can drive it, the software renderer otherwise.
#ifdef TCOS_INTEL_RENDER
  constexpr bool kIntelRender = true;    // the Gen9.5 render engine path (on by default; it has not run on real hardware, so its self-test decides: -DTCOS_INTEL_RENDER=OFF to keep the CPU renderer)
#else
  constexpr bool kIntelRender = false;
#endif
  graphics::SystemGraphics system_graphics(system_gpu, shared_allocator, true, kIntelRender);
  // Rendering and presenting on any GPU of the machine (see Graphics/GraphicsPool.hpp); the display GPU's graphics are the ones above.
  graphics::GraphicsPool graphics_pool(gpu_pool, system_graphics, shared_allocator);
  if (graphics_pool.count() > 1)
    Entry(Level::Info, "Graphics").dec(graphics_pool.count()).text(" GPUs, ").dec(graphics_pool.accelerated_count()).text(" with a 3D engine");
#ifdef TCOS_AMD_VEGAM
  // The Vega M's 3D node, made when the card is up (further down). Declared here so that it outlives the Direct3D kernel, whose adapter for the card draws with it.
  UniquePointer<graphics::RenderNode> vega_node;
#endif
  // The kernel half of Direct3D (what a Windows program's D3D runtime reaches through the D3DKMT system calls): one adapter per GPU of the pool.
  dxgk::Kernel dxg_kernel(shared_allocator);
  uint32_t dxg_fallbacks = 0;
  graphics::DxgFallback dxg_why;
  uint32_t dxg_adapters = graphics::register_dxg_adapters(dxg_kernel, graphics_pool, shared_allocator, &dxg_fallbacks, &dxg_why);
  // The CPU renderer as an adapter of its own (Windows has its "WARP" one) next to the display GPU's, so the 3D test can draw on either and compare.
  if (system_graphics.accelerated() && dxg_kernel.add_adapter(system_graphics.software_device(), dxgk::AdapterConfig()) != dxgk::kInvalidIndex) ++dxg_adapters;
  dxgk::set_system_kernel(&dxg_kernel);
  // What each accelerated GPU really does with Direct3D 9 / 10 / 11 programs, against the CPU renderer (dxgk::run_conformance): the verdicts are in the log.
  if (dxg_adapters > 1)
    for (uint32_t i = 0; i + 1 < dxg_adapters; ++i) {
      dxgk::Adapter* a = dxg_kernel.adapter(i);
      if (!a || !a->caps().accelerated) continue;
      Entry(Level::Info, "Direct3D").text("conformance on ").text(a->caps().name);
      const uint32_t bad = dxgk::run_conformance(shared_allocator, dxg_kernel, i, dxg_adapters - 1, [](void*, const dxgk::SceneResult& r) {
        const bool wrong = r.verdict == dxgk::SceneVerdict::Differs || r.verdict == dxgk::SceneVerdict::Broken;
        Entry e(wrong ? Level::Warning : r.verdict == dxgk::SceneVerdict::Refused ? Level::Info : Level::Ok, "Direct3D");
        e.text(r.name).text(": ");
        switch (r.verdict) {
          case dxgk::SceneVerdict::Same: e.text("as the CPU renderer"); break;
          case dxgk::SceneVerdict::Differs: e.text("differs in ").dec(r.bad_pixels).text(" pixels, first at ").dec(r.first_bad % 32).text(",").dec(r.first_bad / 32).text(": ").hex(r.device_value).text(" instead of ").hex(r.reference_value); break;
          case dxgk::SceneVerdict::Refused: e.text("not supported (").text(r.step).text(")"); break;
          case dxgk::SceneVerdict::Broken: e.text("failed at ").text(r.step).text(", status ").hex(r.status); break;
        }
      }, nullptr);
      Entry(bad ? Level::Warning : Level::Ok, "Direct3D").dec(bad).text(" conformance scene(s) wrong");
    }
  Entry(dxg_fallbacks ? Level::Warning : Level::Ok, "Direct3D").text("kernel mode driver: ").dec(dxg_adapters).text(" adapter(s)");
  if (dxg_fallbacks) Entry(Level::Warning, "Direct3D").dec(dxg_fallbacks).text(" GPU(s) failed the Direct3D self-test and use the CPU renderer");
  if (dxg_fallbacks)
    Entry(Level::Warning, "Direct3D").text("self-test: ").text(dxg_why.picture_wrong ? "the picture is wrong" : dxg_why.step).text(" status ").hex(static_cast<uint32_t>(dxg_why.status));
  // WebGL and WebGPU draw on the 3D hardware when there is some (and on the CPU rasterizer in src/WebGl when there is not, or for a draw the hardware cannot do).
  graphics::WebGlAccelerator web_accelerator(system_graphics.device(), shared_allocator);
  if (system_graphics.accelerated()) webgl::set_accelerator(&web_accelerator);
  if (gpu) {
    graphics::IRenderer2D* renderer = system_graphics.renderer_2d();
    if (!renderer) {
      Entry(Level::Fault, "Graphics").text("no memory for a 2D renderer");
    } else {
      Entry(Level::Ok, "Graphics").text("3D: ").text(system_graphics.device().info().name)
          .text(system_graphics.accelerated() ? " (GPU accelerated)" : " (software)").text(", WebGL/WebGPU: ").text(system_graphics.accelerated() ? "GPU accelerated, CPU for what the GPU cannot draw" : "software").text(", 2D: ").text(renderer->name())
          .text(renderer->accelerated() ? " (GPU accelerated)" : " (software)");
      if (system_graphics.accelerated()) {
        const int groups = check_2d_groups(system_graphics.device(), shared_allocator);
        Entry(groups > 0 ? Level::Ok : groups == 0 ? Level::Warning : Level::Info, "Graphics").text("2D offscreen groups: ").text(groups > 3 ? "ok (opacity, composite, blur and colour filters, rounded and mask clips, drop shadows)" : groups > 2 ? "ok (opacity, composite, blur and colour filters, rounded and mask clips)" : groups > 1 ? "ok (opacity, composite, blur and colour filters; clips approximated)" : groups > 0 ? "ok (opacity, composite; no filters on this device)" : groups == 0 ? "WRONG on this device" : "not supported by this device");
      }
      if (system_graphics.accelerated()) {
        const int fills = check_2d_fills(system_graphics.device(), shared_allocator);
        Entry(fills > 0 ? Level::Ok : Level::Warning, "Graphics").text("2D tiled pictures and conic gradients: ").text(fills > 0 ? "ok" : "WRONG on this device");
      }
      if (system_graphics.accelerated()) {
        const int glyphs = check_2d_glyphs(system_graphics.device(), shared_allocator);
        Entry(glyphs > 0 ? Level::Ok : glyphs == 0 ? Level::Warning : Level::Info, "Graphics").text("2D glyph atlas: ").text(glyphs > 0 ? "ok (kept on the GPU, tinted by the GPU)" : glyphs == 0 ? "WRONG on this device" : "not supported by this device");
      }
      if (!system_graphics.accelerated())
        Entry(system_gpu.virtio_gpu() || system_gpu.vmware_gpu() ? Level::Warning : Level::Info, "Graphics").text("no 3D acceleration: ").text(system_graphics.software_reason());
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
  Entry(Level::Ok, "NPU").text(npu.name()).text(", ").dec(npu.compute_units()).text(" compute units; ").text(system_npu.description());
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
  process::SocketManager sockets(stacks);  // Windows sockets (ws2_32.dll) over the stacks above; wired in once at least one interface exists, below
  const bool clock_ok = clock.initialize();
  if (!clock_ok) Entry(Level::Warning, "Clock").text("could not calibrate the timer; networking is off");

  // Video codecs: a device per GPU video engine found (not driven yet: its sessions run on the CPU) and the software codec.
  hardware::SystemVideoCodecs video_codecs(pci, shared_allocator);
#ifdef TCOS_AMD_VEGAM
  UniquePointer<hardware::AmdVegaM> vega_m;   // the Radeon RX Vega M's memory controller, engines and display (opt-in, below)
  UniquePointer<hardware::PciMsi> vega_msi;   // its MSI, when the machine delivers it
  uint8_t vega_lit_crtc[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};   // the CRTC light_up() put each hot plug pin's output on (0xFF: none)
  bool vega_msi_working = false;              // the interrupt arrived in the self-test: the desktop loop looks at the ring only when one comes
#endif
#ifdef TCOS_AMD_UVD
  video_codecs.set_amd_uvd6(true);   // the Radeon RX Vega M's UVD 6 engine (opt-in: it has not run on a real card)
#endif
  struct PciBarSizer final : hardware::IBarSizer {
    hardware::IPciBus& bus;
    explicit PciBarSizer(hardware::IPciBus& b) : bus(b) {}
    bool bar_size(hardware::PciAddress address, uint8_t index, uint64_t* size) override {
      hardware::PciBarInfo info;
      if (!hardware::pci_read_bar(bus, address, index, &info) || !info.valid || info.io) return false;
      *size = info.size;
      return true;
    }
  } bar_sizer(pci);
  video_codecs.set_bar_sizer(paging_ok ? &bar_sizer : nullptr);  // AMD GPUs: read the IP discovery table at the end of video memory
  video_codecs.set_mmio(paging_ok ? &register_mapper : nullptr);  // Intel GPUs: look at the video boxes (sessions still run on the CPU)
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
    if (codec_device.engine_summary()[0]) Entry(Level::Info, "Video").text("  ").text(codec_device.engine_summary());
    // The engine probe may have read how much video memory the card has: give it to the display device of the same PCI function, for the computer manager.
    hardware::PciAddress engine_gpu{}, display_gpu{};
    if (gpu && codec_device.gpu_memory_bytes() && codec_device.gpu_location(&engine_gpu) && gpu->pci_location(&display_gpu) &&
        engine_gpu.bus == display_gpu.bus && engine_gpu.device == display_gpu.device && engine_gpu.function == display_gpu.function)
      gpu->set_video_memory_total(codec_device.gpu_memory_bytes());
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

  // Serial ports: the legacy COM ports that answer. They are only found here; a program opens one with the framing it wants. The service
  // moves the bytes between the ports and their queues.
  hardware::X86SerialPortIo serial_io;
  hardware::SystemSerialPorts serial_ports(serial_io, shared_allocator);
  serial_ports.initialize();
  for (uint32_t i = 0; i < serial_ports.count(); ++i) Entry(Level::Ok, "Serial").text(serial_ports.at(i)->name());
  if (serial_ports.count() == 0) Entry(Level::Info, "Serial").text("no serial port");
  else if (!serial_ports.start_service()) Entry(Level::Warning, "Serial").text("could not start the serial service; ports do not move bytes");
#ifdef TCOS_SELFTESTS
  UniquePointer<NetworkStackDemo> stack_demo;
  if (clock_ok && !run_async_demo(clock, shared_allocator)) console::puts("  async self-check incomplete\n");
#endif
  if (network.count() > 0 && clock_ok) {
    for (uint32_t i = 0; i < network.count(); ++i)
      if (!stacks.add(*network.at(i))) Entry(Level::Warning, "Network").text("could not start a network stack on interface ").dec(i);
    if (stacks.count() > 0) {
      stacks.start_service();  // one thread keeps every stack polled
      network::set_system_network_stacks(&stacks);
      process::set_system_sockets(&sockets);  // Windows sockets (ws2_32.dll): see src/Process/Sockets.hpp
#ifndef TCOS_SELFTESTS
      stacks.start_dhcp();     // (the network self-check does this itself)
#endif
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

  // Hypervisor guest services. QEMU: the host's entropy pool (virtio-rng), the memory balloon (virtio-balloon), the crash notification (pvpanic).
  // VirtualBox: its guest device (pointer integration, display size requests, balloon, heartbeat). Before the input devices, which use its pointer.
  hardware::SystemGuestServices guest(pci, shared_allocator, st);
  guest.initialize();
  if (guest.entropy()) Entry(Level::Ok, "Guest").text("virtio-rng: host entropy available");
  if (guest.balloon()) Entry(Level::Ok, "Guest").text("virtio-balloon: ").dec(guest.balloon()->target_pages() / 256).text(" MiB requested by the host");
  if (guest.virtualbox()) Entry(Level::Ok, "Guest").text("VirtualBox guest device: pointer integration, display requests, balloon, heartbeat");
  if (hardware::PvPanic::present()) Entry(Level::Ok, "Guest").text("pvpanic: crashes are reported to the host");
  if (guest.balloon() || guest.virtualbox()) {
    // (The service thread follows the host: balloon targets, and VirtualBox's pointer, window size and heartbeat.)
    if (!guest.start_service()) Entry(Level::Warning, "Guest").text("could not start the guest service; the host's requests are ignored");
  }

  // Hyper-V: the processor interface, VMBus and the integration components (heartbeat, shutdown, time). Needs the kernel's interrupt tables.
  hardware::SystemHyperV hyperv(shared_allocator);
  bool hyperv_restart = false;
  if (hyperv.initialize()) {
    Entry(Level::Ok, "Hyper-V").text("hypervisor interface, virtual processor ").dec(hyperv.platform()->vp_index());
    if (!hyperv.vmbus_connected()) {
      Entry(Level::Info, "Hyper-V").text("no VMBus connection; the integration services are not running");
    } else {
      Entry(Level::Ok, "Hyper-V").text("VMBus connected, ").dec(hyperv.vmbus()->offer_count()).text(" devices offered");
      hardware::HyperVIntegration* ic = hyperv.integration();
      Entry(ic->heartbeat_running() && ic->shutdown_running() ? Level::Ok : Level::Warning, "Hyper-V")
          .text("integration services: heartbeat ").text(ic->heartbeat_running() ? "yes" : "no")
          .text(", shutdown ").text(ic->shutdown_running() ? "yes" : "no")
          .text(", time ").text(ic->timesync_running() ? "yes" : "no");
      if (!hyperv.start_service(static_cast<int32_t>(cpu.current_core()))) Entry(Level::Warning, "Hyper-V").text("could not start the VMBus service thread");
    }
  }

  // VMware: the host's power and window-size commands over the backdoor message channel (the pointer is taken up with the input devices).
  hardware::VmwareTools vmware_tools;
  bool vmware_restart = false;
  if (hardware::Backdoor::probe()) {
    Entry(Level::Ok, "VMware").text("backdoor, version ").dec(hardware::Backdoor::version()).text(", product ").dec(hardware::Backdoor::product());
    if (vmware_tools.start()) Entry(Level::Ok, "VMware").text("tools channel: power commands and window size");
    else Entry(Level::Info, "VMware").text("the host has no tools message channel; power commands and window size are not followed");
  }

  // Input devices: PS/2, USB HID and virtio-input, merged into one keyboard and mouse.
  hardware::SystemInput input(pci, usb, shared_allocator, st);
#ifdef TCOS_BLUETOOTH
  hardware::HidOverGatt bluetooth_hid(shared_allocator);      // Bluetooth keyboards and mice: a profile for the Bluetooth manager and a source of input devices for SystemInput (it outlives both)
#endif
  input.set_vmmdev(guest.virtualbox());
  input.initialize();
  // The touchpad and the touchscreen of a laptop are HID devices on an I2C bus behind the chipset (ACPI PNP0C50): found through the ACPI tables, and added to the mouse.
  hardware::AcpiTables i2c_acpi_tables(st);
  hardware::SystemAcpiHost i2c_acpi_host(&pci, paging_ok ? &mmio : nullptr);
  hardware::MmioI2cRegisterSource i2c_registers(register_mapper);
  hardware::PlatformI2cHid i2c_hid(shared_allocator, pci, i2c_registers, register_mapper, clock, i2c_acpi_host);
  if (paging_ok && i2c_acpi_tables.initialize()) {
    const uint32_t started = i2c_hid.start(i2c_acpi_tables, input.mice());
    for (uint32_t i = 0; i < i2c_hid.report_count(); ++i) {
      const hardware::PlatformI2cHid::Report& r = i2c_hid.report(i);
      Entry(r.outcome == hardware::PlatformI2cHid::Outcome::Started ? Level::Ok : Level::Warning, "Input").text("I2C HID ").text(r.device).text(": ").text(hardware::PlatformI2cHid::outcome_name(r.outcome));
    }
    if (started) Entry(Level::Info, "Input").dec(started).text(" pointing device(s) on I2C");
  }
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

#ifdef TCOS_WINE_TEST_PROGRAM
  // Deliberately here, not with TCOS_USERMODE_DEMO above: every detach_firmware_drivers() caller (Network, Storage) has already run, so whatever a PE
  // program's execution leaves behind cannot crash one of those calls the way it does when a PE program runs before them (see UserModeDemo.hpp).
  run_wine_test_program(shared_allocator);
#endif

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

  // What the computer manager's Hardware tab lists: every subsystem above, looked at again once a second (hot-plugged devices come and go).
  SystemInventory inventory(shared_allocator);
  inventory.set_cpu(&cpu);
  inventory.set_memory(ram.total_bytes(), ram.usable_bytes());
  inventory.set_gpu(gpu);
  inventory.set_monitors(&monitors);
  inventory.set_storage(&storage);
  inventory.set_network(&network);
  inventory.set_audio(&audio);
  inventory.set_midi(&usb_midi, &midi_usual);
  inventory.set_input(&input);
  inventory.set_usb(&usb);
  inventory.set_printers(&printers);
  inventory.set_cameras(&cameras);
  inventory.set_pci(&pci);
  SystemNetworkDetails network_details(network, stacks);
  SystemWirelessDetails wireless_details(network);

  // The desktop: runs until Shut Down is chosen from the Start menu (or, with no display, until a key).
  // The monitors become the display: the surface is as big as all of them side by side, and when a monitor shows its picture at another size
  // (a resolution that is not what the desktop was drawn for, or a scale) the pictures go through MonitorDisplay. Applying again, after a change
  // in Display Settings or a hot plug, builds the renderer for the new size.
  struct DisplayPipeline {
    hardware::SystemMonitors* monitors;
    graphics::SystemGraphics* graphics;
    hardware::MonitorDisplay* display;
    static graphics::IRenderer2D* apply(void* context) {
      auto* self = static_cast<DisplayPipeline*>(context);
      if (!self->monitors->apply()) return nullptr;
      hardware::ScanoutPlacement placements[16];
      const uint32_t count = self->monitors->physical_placements(placements, 16);
      const bool compose = self->monitors->needs_compositing() && self->display->update(self->monitors->layout(), placements, count);
      self->graphics->set_display_override(compose ? self->display : nullptr);
      self->graphics->reset_renderer();
      return self->graphics->renderer_2d();
    }
  };
  UniquePointer<hardware::MonitorDisplay> monitor_display = gpu ? UniquePointer<hardware::MonitorDisplay>::make(shared_allocator, *gpu, shared_allocator) : UniquePointer<hardware::MonitorDisplay>();
  DisplayPipeline display_pipeline{&monitors, &system_graphics, monitor_display.get()};
  if (gpu && monitor_display && (monitors.layout().count() > 1 || monitors.needs_compositing())) {
    if (DisplayPipeline::apply(&display_pipeline)) {
      Entry(Level::Ok, "Monitors").dec(monitors.layout().enabled_count()).text(" monitors, desktop ").dec(monitors.desktop_width()).text("x").dec(monitors.desktop_height());
    } else {
      Entry(Level::Warning, "Monitors").text("the display device cannot show the monitors separately; using one");
    }
  }
  graphics::IRenderer2D* renderer = gpu ? system_graphics.renderer_2d() : nullptr;
  if (renderer) {
    // The desktop runs on a thread with a real stack: the page layout in the browser's tick would overflow the firmware's boot stack.
    kernel::run_on_large_stack([&] {
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
    crypto::IEntropySource& smb_entropy = crypto::CpuEntropySource::available() ? static_cast<crypto::IEntropySource&>(cpu_entropy)
        : guest.entropy() ? *guest.entropy() : static_cast<crypto::IEntropySource&>(timing_entropy);
    crypto::DrbgRandom smb_random(smb_entropy);
    crypto::DrbgRandom web_random(smb_entropy);   // crypto.getRandomValues() and the keys web pages make
    if (web_random.initialize()) browser::set_system_random(&web_random);
    network::StackSetNetwork smb_network(stacks);
    // Databases (PostgreSQL) for the programs that want them, reached over the same interfaces. TLS needs a trust store, which this system does not ship:
    // connections are plain until one is given (SystemDatabases::transport().set_tls()).
    database::SystemDatabases databases(shared_allocator, smb_network, &smb_random);
    desktop.set_databases(&databases.manager());
    // Web pages for the browser, over the same interfaces. https:// is given its trust store once the volumes are mounted (below).
    browser::HttpPageLoader page_loader(stacks, shared_allocator);
    desktop.set_page_loader(&page_loader);
    Entry(Level::Info, "Database").dec(databases.manager().driver_count()).text(" driver(s): ").text(databases.manager().driver(0)->name());
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
    sync_wall_clock(st);
    filesystem::set_unix_clock(wall_clock_seconds, nullptr);   // before the volumes are mounted: they stamp what they create
    SystemFileSystem explorer_files(shared_allocator, storage);
    desktop::KernelProgramLauncher program_launcher(shared_allocator);      // what the terminals run programs with
#ifdef TCOS_INTEL_WIFI
    // The Intel Wireless-AC 9560 (and the rest of the 9000 series) as a network interface: the firmware from /firmware is started, and the card then scans and joins networks through the
    // Wi-Fi window; its network stack gets an address (DHCP) whenever it is joined. Opt-in (-DTCOS_INTEL_WIFI=ON): none of it has run on a real card.
    if (clock_ok && paging_ok) {
      hardware::FileSystemFirmwareSource wifi_firmware(explorer_files.file_system(), shared_allocator);
      for (uint32_t i = 0; i < pci.device_count(); ++i) {
        hardware::PciDeviceInfo d;
        if (!pci.pci_device(i, &d) || !hardware::IwlDriver::supports(d)) continue;
        if (!smb_random.ready()) smb_random.initialize();
        hardware::IwlWirelessInterface::Inputs wifi_inputs;
        wifi_inputs.pci = &pci;
        wifi_inputs.device = d;
        wifi_inputs.mapper = &register_mapper;
        wifi_inputs.firmware = &wifi_firmware;
        wifi_inputs.allocator = &shared_allocator;
        wifi_inputs.clock = &clock;
        wifi_inputs.random = &smb_random;
        UniquePointer<hardware::IwlWirelessInterface> wifi = UniquePointer<hardware::IwlWirelessInterface>::make(shared_allocator, wifi_inputs);
        if (!wifi) break;
        if (wifi->initialize()) {
          hardware::IwlWirelessInterface* radio = wifi.get();
          const hardware::MacAddress mac = radio->mac_address();
          stacks.stop_service();                                       // (a stack for it; the others keep theirs)
          if (network.adopt(util::move(wifi)) && stacks.add(*radio)) {
            Entry e(Level::Ok, "Wi-Fi");
            e.text("Intel Wireless-AC ").text(radio->driver().firmware_name()).text(", ");
            for (int b2 = 0; b2 < 6; ++b2) e.hex2(mac.bytes[b2]).text(b2 < 5 ? ":" : "");
            e.text(", ready to scan");
          } else {
            Entry(Level::Warning, "Wi-Fi").text("could not add the Intel Wireless-AC to the network interfaces");
          }
          stacks.start_service();
        } else {
          Entry(Level::Warning, "Wi-Fi").text("Intel Wireless-AC not started: ").text(wifi->failure());
        }
        break;
      }
    }
#endif
#ifdef TCOS_BLUETOOTH
    // The Bluetooth controllers on the USB ports (the Intel one on the Wireless-AC 9560 among them): its firmware, /firmware/ibt-*.sfi (the name without Linux's intel/ directory), is
    // loaded and the controller brought up. Nothing is scanned, paired or connected. Opt-in (-DTCOS_BLUETOOTH=ON): none of it has run on a real controller.
    struct BluetoothFirmwareFiles final : hardware::IBluetoothFirmwareSource {
      hardware::FileSystemFirmwareSource files;
      BluetoothFirmwareFiles(filesystem::IFileSystem& file_system, allocators::IAllocator& allocator) : files(file_system, allocator) {}
      bool read(const char* name, ByteBuffer& out) override {
        const char prefix[] = "intel/";
        uint32_t skip = 0;
        while (prefix[skip] && name[skip] == prefix[skip]) ++skip;
        return files.read(name + (prefix[skip] ? 0 : skip), out);
      }
    } bluetooth_files(explorer_files.file_system(), shared_allocator);
    hardware::FileBluetoothBonds bluetooth_bonds(explorer_files.file_system(), shared_allocator);
    if (!smb_random.ready()) smb_random.initialize();
    UniquePointer<hardware::BluetoothService> bluetooth = UniquePointer<hardware::BluetoothService>::make(shared_allocator, &usb, &bluetooth_files, shared_allocator, smb_random.ready() ? &smb_random : nullptr, &bluetooth_bonds);
    uint32_t bluetooth_bonds_saved = 0;
    UniquePointer<SystemBluetoothDetails> bluetooth_details;
    if (bluetooth && paging_ok) {
      bluetooth_details = UniquePointer<SystemBluetoothDetails>::make(shared_allocator, *bluetooth);      // (the window's view of it, and the pairing questions)
      inventory.set_bluetooth(bluetooth.get());
      bluetooth->add_profile(&bluetooth_hid);
      input.add_source(&bluetooth_hid);
      bluetooth->start();
      bluetooth_bonds_saved = bluetooth->bonds().count();
      for (uint32_t i = 0; i < bluetooth->count(); ++i) {
        const hardware::BluetoothService::Entry* e = bluetooth->entry(i);
        Entry log(e->state == hardware::BluetoothState::Ready ? Level::Ok : Level::Warning, "Bluetooth");
        if (e->state == hardware::BluetoothState::Ready) {
          const hardware::hci::BdAddr& a = e->controller->info().address;
          log.text("controller ");
          for (int b = 5; b >= 0; --b) log.hex2(a.b[b]).text(b > 0 ? ":" : "");
          log.text(", ready");
        } else {
          log.text("controller not started: ").text(e->message);
          if (e->firmware[0]) log.text(" (").text(e->firmware).text(")");
        }
      }
    }
#endif
#ifdef TCOS_THUNDERBOLT
    // The Thunderbolt 3 host interface (Alpine Ridge): the control channel to the controller's own connection manager firmware, which the BIOS has to have started. Devices that are
    // connected are listed (and announced on the desktop); none gets a PCIe tunnel, which would give it access to memory, until something approves it. Opt-in (-DTCOS_THUNDERBOLT=ON): none
    // of it has run on a real controller.
    UniquePointer<hardware::ThunderboltDriver> thunderbolt;
    UniquePointer<hardware::FileTbKeyStore> thunderbolt_keys;       // the keys of devices approved at the secure level
    UniquePointer<hardware::ThunderboltNet> thunderbolt_net;        // networking to another host on the cable (after the driver: it goes first)
    hardware::ThunderboltNetLink* thunderbolt_link = nullptr;       // its entry in the list of network interfaces, which outlives them both
    uint64_t thunderbolt_devices = 0;
    hardware::PciAddress thunderbolt_address = {0, 0, 0};
    UniquePointer<hardware::PciMsix> thunderbolt_msix;              // its MSI-X, when the machine delivers it
    UniquePointer<hardware::PciMsi> thunderbolt_msi;                // else its MSI
    bool thunderbolt_msi_working = false;                           // an interrupt arrived in the start-up check: the loop reads the channel only when one comes
    if (clock_ok && paging_ok) {
      for (uint32_t i = 0; i < pci.device_count(); ++i) {
        hardware::PciDeviceInfo d;
        if (!pci.pci_device(i, &d) || !hardware::ThunderboltDriver::supports(d)) continue;
        hardware::ThunderboltDriver::Config thunderbolt_config;
#ifdef TCOS_THUNDERBOLT_START_FIRMWARE
        thunderbolt_config.start_firmware = true;
#endif
        thunderbolt = UniquePointer<hardware::ThunderboltDriver>::make(shared_allocator, shared_allocator, thunderbolt_config);
        if (!thunderbolt) break;
        thunderbolt_address = d.address;
        if (thunderbolt->start(pci, d, register_mapper) == hardware::ThunderboltDriver::Outcome::Started) {
          if (!smb_random.ready()) smb_random.initialize();
          thunderbolt_keys = UniquePointer<hardware::FileTbKeyStore>::make(shared_allocator, explorer_files.file_system(), shared_allocator);
          if (thunderbolt_keys && thunderbolt->icm()) thunderbolt->icm()->set_secure(&smb_random, thunderbolt_keys.get());
          // MSI-X first (the first entry of the table carries the control channel's rings), then MSI: the handler, then the message to the core that runs this. Kept only if an interrupt
          // arrives for a request of the controller.
          const uint32_t tb_apic_id = kernel::running() ? kernel::apic::this_apic_id() : 0xFFFF;
          if (tb_apic_id <= 0xFF) {
            kernel::set_interrupt_handler(kernel::kVectorThunderbolt, &on_thunderbolt_interrupt, thunderbolt.get());
            auto check_interrupt = [&]() {
              const uint32_t before = thunderbolt->interrupts();
              thunderbolt->probe_control();
              hardware::delay_us(2000);
              return thunderbolt->interrupts() != before;
            };
            thunderbolt_msix = UniquePointer<hardware::PciMsix>::make(shared_allocator, pci, register_mapper);
            kernel::set_interrupt_handler(kernel::kVectorThunderboltData, &on_thunderbolt_interrupt, thunderbolt.get());
            const uint8_t tb_vectors[2] = {kernel::kVectorThunderbolt, kernel::kVectorThunderboltData};     // (entry 0: the control channel, entry 1: the data rings)
            if (thunderbolt_msix && thunderbolt_msix->enable(d.address, tb_vectors, 2, tb_apic_id) && thunderbolt->use_interrupts(true, 0, 1)) thunderbolt_msi_working = check_interrupt();
            if (!thunderbolt_msi_working) {
              thunderbolt->use_interrupts(false);
              thunderbolt_msix = UniquePointer<hardware::PciMsix>();
              thunderbolt_msi = UniquePointer<hardware::PciMsi>::make(shared_allocator, pci);
              if (thunderbolt_msi && thunderbolt_msi->enable(d.address, 0xFEE00000u | (tb_apic_id << 12), kernel::kVectorThunderbolt) && thunderbolt->use_interrupts(true)) thunderbolt_msi_working = check_interrupt();
            }
            if (!thunderbolt_msi_working) {
              thunderbolt->use_interrupts(false);
              thunderbolt_msi = UniquePointer<hardware::PciMsi>();
              kernel::set_interrupt_handler(kernel::kVectorThunderbolt, nullptr, nullptr);
            }
          }
          // networking to another host: offered in our properties, an interface for the network stack that gets an address when the cable's other end answers
          if (thunderbolt->xdomain_host()) thunderbolt_net = UniquePointer<hardware::ThunderboltNet>::make(shared_allocator, *thunderbolt->xdomain_host(), shared_allocator, smb_random);
          if (thunderbolt_net && thunderbolt->xdomain_host()->offer_network(hardware::ThunderboltNet::offered_flags(hardware::ThunderboltNet::Config())) && thunderbolt_net->initialize()) {
            UniquePointer<hardware::ThunderboltNetLink> link = UniquePointer<hardware::ThunderboltNetLink>::make(shared_allocator, thunderbolt_net.get());
            hardware::ThunderboltNetLink* raw = link.get();
            stacks.stop_service();
            if (link && network.adopt(util::move(link)) && stacks.add(*raw)) thunderbolt_link = raw;
            stacks.start_service();
          }
          Entry e(Level::Ok, "Thunderbolt");
          e.text(thunderbolt->model()->name).text(thunderbolt_msix ? " (MSI-X)" : thunderbolt_msi_working ? " (MSI)" : " (polled)");
          if (thunderbolt->icm()) {
            e.text(", security level ").dec(static_cast<uint32_t>(thunderbolt->icm()->security_level())).text(", firmware ").dec(thunderbolt->icm()->driver_ready().rom_version).text(".").dec(thunderbolt->icm()->driver_ready().ram_version);
          } else if (thunderbolt->usb4()) {
            // the connection manager is ours: the routers found, and tunnels left by the BIOS that were switched off
            e.text(", software connection manager: ").dec(thunderbolt->usb4()->present_count()).text(" router(s)");
            if (thunderbolt->usb4()->preboot_tunnels_closed()) e.text(", ").dec(thunderbolt->usb4()->preboot_tunnels_closed()).text(" tunnel(s) of the BIOS closed");
            e.text("; no PCIe tunnel is made until one is approved");
          }
        } else {
          Entry(Level::Warning, "Thunderbolt").text(thunderbolt->model() ? thunderbolt->model()->name : "controller").text(" not started: ").text(thunderbolt->message());
          thunderbolt = UniquePointer<hardware::ThunderboltDriver>();
        }
        break;
      }
    }
#endif
#ifdef TCOS_AMD_VEGAM
    // The Radeon RX Vega M of a Kaby Lake-G machine: the memory controller with its GART, the interrupt ring, the engines' microcode through the SMC, the graphics engine with
    // its compute queues and the SDMA engines (in the order of the Linux driver, before the video engine, which needs the memory controller), and, if the card drives the display
    // the firmware set up, the OS's own framebuffer in its memory. Opt-in (-DTCOS_AMD_VEGAM=ON): none of it has run on a real card.
    if (clock_ok && paging_ok) {
      for (uint32_t i = 0; i < pci.device_count(); ++i) {
        hardware::PciDeviceInfo d;
        if (!pci.pci_device(i, &d) || d.vendor_id != hardware::AmdGpu::kPciVendor || !hardware::amd_uvd6_firmware_name(d.device_id)) continue;
        constexpr uint16_t kPciCommand = 0x04;
        const uint32_t command = pci.read32(d.address, kPciCommand) & 0xFFFF;   // (not the status bits above, which clear when written)
        pci.write32(d.address, kPciCommand, command | (1u << 1) | (1u << 2));   // memory space and bus mastering: the card writes its interrupt ring
        hardware::MmioRegion registers_window, doorbells;
        const uint64_t registers_base = pci.memory_bar(d.address, 5), doorbell_base = pci.memory_bar(d.address, 2), aperture_base = pci.memory_bar(d.address, 0);
        uint64_t aperture_bytes = 0;
        if (!registers_base || !doorbell_base || !aperture_base || !bar_sizer.bar_size(d.address, 0, &aperture_bytes) ||
            !register_mapper.map_registers(registers_base, hardware::kAmdGmc8RegisterBytes, &registers_window) || !register_mapper.map_registers(doorbell_base, 0x10000, &doorbells)) {
          Entry(Level::Warning, "Vega M").text("not started - the card's BARs cannot be mapped");
          break;
        }
        hardware::MmioVcnRegisters vega_registers(registers_window);
        ByteBuffer vbios_image(shared_allocator);   // (AmdVegaM keeps its own copy)
        hardware::AmdGpu* display = system_gpu.amd_gpu();
        hardware::PciAddress display_location{};
        const bool drives_display = display && display->pci_location(&display_location) && display_location.bus == d.address.bus && display_location.device == d.address.device &&
                                    display_location.function == d.address.function;
        uint64_t largest = 0;
        for (uint32_t m = 0; drives_display && m < display->mode_count(); ++m) {
          hardware::DisplayMode mode;
          if (display->mode_info(m, &mode)) largest = largest > (uint64_t{mode.width} + 64) * 4 * mode.height ? largest : (uint64_t{mode.width} + 64) * 4 * mode.height;
        }
        vega_m = UniquePointer<hardware::AmdVegaM>::make(shared_allocator, shared_allocator);
        if (!vega_m) break;
        hardware::AmdVegaM::Inputs inputs;
        inputs.registers = &vega_registers;
        inputs.clock = &clock;
        inputs.file_system = &explorer_files.file_system();
        inputs.allocator = &shared_allocator;
        inputs.mapper = &register_mapper;
        inputs.device_id = d.device_id;
        inputs.aperture_physical = aperture_base;
        inputs.aperture_bytes = aperture_bytes;
        inputs.doorbells = doorbells;
        inputs.max_framebuffer_bytes = largest;
        inputs.output_framebuffer_bytes = 16ull << 20;     // room for the picture of one external output that light_up() lights (1920x1200 at most)
        inputs.draw_area_bytes = 32ull << 20;              // room for the data of one draw of the 3D device (a target of 400x300 and its planes need about 10 MiB)
        // The full reset of the card as the last resort when an engine will not come back from its soft reset. This OS cannot POST a card afterwards (no AtomBIOS interpreter), so the
        // card would stay down and whatever it shows would go dark: only allowed when the card does not drive the display.
        inputs.pci = &pci;
        inputs.pci_address = d.address;
        inputs.allow_asic_reset = !drives_display;
        if (inputs.allow_asic_reset) {
          // The card's video BIOS, for the POST after a reset: the ACPI ATRM method, the ACPI VFCT table, the expansion ROM, the ROM registers (not read when a reset is not allowed:
          // nothing would use it). The ATRM method is AML of the machine's tables, so they are loaded into an interpreter of their own for the look (it is gone again afterwards).
          hardware::AcpiTables vfct_tables(st);
          hardware::SystemAcpiHost atrm_host(&pci, paging_ok ? &mmio : nullptr);
          acpi::AmlInterpreter atrm_aml(shared_allocator, atrm_host);
          hardware::AmdVbios::Sources sources;
          if (vfct_tables.initialize()) {
            sources.vfct = vfct_tables.find("VFCT");
            const ConstByteView dsdt = vfct_tables.dsdt();
            if (!dsdt.empty()) atrm_aml.load_table(dsdt);
            for (uint32_t n = 0; n < 32; ++n) {
              const ConstByteView ssdt = vfct_tables.find_nth("SSDT", n);
              if (ssdt.empty()) break;
              atrm_aml.load_table(ssdt);
            }
            sources.aml = &atrm_aml;
          }
          sources.pci = &pci;
          sources.mapper = &register_mapper;
          sources.registers = &vega_registers;
          sources.allocator = &shared_allocator;
          sources.bar0_physical = aperture_base;
          sources.bar0_bytes = aperture_bytes;
          sources.function = d.address;
          sources.vendor_id = d.vendor_id;
          sources.device_id = d.device_id;
          const hardware::AmdVbios::Source found = hardware::AmdVbios::find(sources, &vbios_image);
          if (found != hardware::AmdVbios::Source::None) {
            inputs.vbios = vbios_image.view();
            Entry(Level::Ok, "Vega M").text("video BIOS: ").dec(vbios_image.size()).text(" bytes from ").text(hardware::AmdVbios::source_name(found));
          } else {
            Entry(Level::Info, "Vega M").text("video BIOS: not found (a card reset could not be POSTed)");
          }
        }
        inputs.auto_recover = true;   // an engine that hung or faulted is reset from the desktop loop (service_interrupts), a few times at most
        // MSI first, as the driver does (the interrupt ring is told it is on): the handler, then the message, to the core that runs this.
        const uint32_t apic_id = kernel::running() ? kernel::apic::this_apic_id() : 0xFFFF;
        if (apic_id <= 0xFF) {
          kernel::set_interrupt_handler(kernel::kVectorAmdVegaM, &on_vega_interrupt, nullptr);
          vega_msi = UniquePointer<hardware::PciMsi>::make(shared_allocator, pci);
          if (vega_msi && vega_msi->enable(d.address, 0xFEE00000u | (apic_id << 12), kernel::kVectorAmdVegaM)) {
            inputs.msi = true;
          } else {
            vega_msi = UniquePointer<hardware::PciMsi>();
            kernel::set_interrupt_handler(kernel::kVectorAmdVegaM, nullptr, nullptr);
          }
        }
        const bool up = vega_m->bring_up(inputs);
        Entry(up ? Level::Ok : Level::Warning, "Vega M").text(up ? "started: " : "not started - ").text(vega_m->status());
        if (up) {
          const uint32_t connected = vega_m->connected_pins();
          if (connected == 0) {
            Entry(Level::Info, "Vega M").text("hot plug: no display on the outputs watched");
          } else {
            Entry(Level::Info, "Vega M").text("hot plug: display on output(s)");
            for (uint32_t pin = 0; pin < 6; ++pin) {
              if (!(connected >> pin & 1)) continue;
              const hardware::AtomConnectors::Connector* connector = hardware::AtomConnectors::find_pin(vega_m->connectors(), pin);
              if (connector) {
                char connector_name[16];
                hardware::AtomConnectors::name(*connector, ArrayView<char>(connector_name, sizeof connector_name));
                FixedString<64> monitor;
                if (describe_vega_monitor(*vega_m, *connector, shared_allocator, &monitor)) Entry(Level::Info, "Vega M").text("  ").text(connector_name).text(": ").text(monitor.c_str());
                else Entry(Level::Info, "Vega M").text("  ").text(connector_name);
                // lit with the mode the monitor prefers and a test picture (the desktop is not drawn on it)
                hardware::AmdVegaM::LightUp lit;
                if (vega_m->light_up(*connector, 0xFF, &lit)) {
                  vega_lit_crtc[pin] = lit.crtc;
                  Entry(Level::Ok, "Vega M").text("  ").text(connector_name).text(" lit: ").dec(lit.width).text("x").dec(lit.height);
                } else {
                  Entry(Level::Warning, "Vega M").text("  ").text(connector_name).text(" not lit (step ").dec(static_cast<uint32_t>(lit.failed)).text(", mode set stage ").dec(static_cast<uint32_t>(lit.set.failed)).text(")");
                }
              } else {
                Entry(Level::Info, "Vega M").text("  output ").dec(pin + 1);
              }
            }
          }
          // The audio endpoints with a port (how many displays could have sound), and the outputs that carry audio and have a display: which endpoint serves which output is not fixed.
          const uint32_t audio_mask = vega_m->audio_pins_connected();
          uint32_t audio_count = 0;
          for (uint32_t pin = 0; pin < hardware::AmdVegaM::kAudioPins; ++pin) audio_count += audio_mask >> pin & 1;
          Entry(Level::Info, "Vega M").text("audio: ").dec(audio_count).text(" of ").dec(hardware::AmdVegaM::kAudioPins).text(" endpoints with a port");
          hardware::AtomConnectors::Connector audio_outputs[hardware::AtomConnectors::kMaxConnectors];
          const uint32_t audio_output_count = vega_m->audio_outputs(audio_outputs, hardware::AtomConnectors::kMaxConnectors);
          for (uint32_t i = 0; i < audio_output_count; ++i) {
            char connector_name[16];
            hardware::AtomConnectors::name(audio_outputs[i], ArrayView<char>(connector_name, sizeof connector_name));
            Entry(Level::Info, "Vega M").text("  audio output with a display: ").text(connector_name);
          }
        }
        if (up && vega_msi) {
          // Try it out, as virtio-gpu does: a copy through the SDMA engine ends with a trap, which must come back as an interrupt. If it does not, polling stays.
          const uint32_t before = __atomic_load_n(&g_vega_interrupts, __ATOMIC_ACQUIRE);
          const uint8_t probe[16] = {};
          vega_m->upload(ConstByteView(probe, sizeof(probe)), 0);
          for (int wait = 0; wait < 50 && __atomic_load_n(&g_vega_interrupts, __ATOMIC_ACQUIRE) == before; ++wait) hardware::delay_us(100);
          vega_msi_working = __atomic_load_n(&g_vega_interrupts, __ATOMIC_ACQUIRE) != before;
          vega_m->service_interrupts();
          Entry(vega_msi_working ? Level::Ok : Level::Warning, "Vega M").text(vega_msi_working ? "interrupts: MSI wakes the CPU" : "interrupts: MSI did not arrive; the interrupt ring is polled");
          if (!vega_msi_working) {
            vega_msi = UniquePointer<hardware::PciMsi>();
            kernel::set_interrupt_handler(kernel::kVectorAmdVegaM, nullptr, nullptr);
          }
        }
        if (!up) {
          vega_msi = UniquePointer<hardware::PciMsi>();
          kernel::set_interrupt_handler(kernel::kVectorAmdVegaM, nullptr, nullptr);
          vega_m = UniquePointer<hardware::AmdVegaM>();
        } else if (drives_display) {
          display->set_native_display(vega_m.get());
          Entry(display->native_scanout() ? Level::Ok : Level::Info, "Vega M").text(display->native_scanout() ? "display: own framebuffer in video memory (DCE 11.2)" : "display: the firmware's framebuffer stays (the CRTC does not show it as expected)");
        } else {
          Entry(Level::Info, "Vega M").text("display: the card does not drive the display the firmware set up");
        }
        if (up) {
          // 3D on the card: its graphics engine as a render node of its own (the display is another GPU's, or the firmware's), tried with the same self-tests as the other back ends, then
          // a Direct3D adapter if it draws the Direct3D triangle. The desktop's own renderer does not use it: each draw is a round trip to the card (graphics/Graphics3D/Vega).
          vega_node = UniquePointer<graphics::RenderNode>::make(shared_allocator, nullptr, nullptr, nullptr, shared_allocator, true, nullptr, false, vega_m.get());
          if (vega_node && vega_node->accelerated()) {
            graphics::DxgFallback vega_why;
            bool vega_fell_back = false;
            if (graphics::register_dxg_node(dxg_kernel, *vega_node, shared_allocator, false, &vega_fell_back, &vega_why)) {
              ++dxg_adapters;
              Entry(Level::Ok, "Vega M").text("3D: the graphics engine is a Direct3D adapter (each draw is a round trip to the card)");
            } else {
              Entry(Level::Warning, "Vega M").text("3D: the graphics engine failed the Direct3D self-test, step ").text(vega_why.step).text(vega_why.picture_wrong ? " (the picture was wrong)" : "");
              vega_node = UniquePointer<graphics::RenderNode>();
            }
          } else {
            Entry(Level::Info, "Vega M").text("3D: not used - ").text(vega_node ? vega_node->software_reason() : "out of memory");
            vega_node = UniquePointer<graphics::RenderNode>();
          }
        }
        break;
      }
    }
#endif
    // Microcode the video engines need, from /firmware on the root volume (AMD VCN 1: Ryzen 2000/3000 "Raven" APUs). Only read and checked here; no engine is
    // started with it yet.
    for (uint32_t i = 0; i < video_codecs.count(); ++i) {
      hardware::IVideoCodecDevice& firmware_device = *video_codecs.at(i);
      const bool loaded = firmware_device.load_firmware(explorer_files.file_system());
      if (firmware_device.firmware_status()[0]) Entry(loaded ? Level::Ok : Level::Warning, "Firmware").text(firmware_device.name()).text(": ").text(firmware_device.firmware_status());
    }
    // With the microcode read, work out (from reads of the card and of the firmware's display buffer) where each engine's memory would go. Nothing is written to
    // a card and no engine is started; the block is only mapped.
    hardware::VideoEngineEnvironment engine_environment;
    engine_environment.mapper = paging_ok ? &register_mapper : nullptr;
    if (gpu) gpu->framebuffer_range(&engine_environment.framebuffer_base, &engine_environment.framebuffer_bytes);
    if (hardware::AmdGpu* amd = system_gpu.amd_gpu()) engine_environment.reserved_tail_bytes = amd->reserved_video_memory();
    for (uint32_t i = 0; i < video_codecs.count(); ++i) {
      hardware::IVideoCodecDevice& planned_device = *video_codecs.at(i);
      const bool planned = planned_device.plan_engine(engine_environment);
      if (planned_device.plan_status()[0]) Entry(planned ? Level::Ok : Level::Warning, "Firmware").text(planned_device.name()).text(": ").text(planned_device.plan_status());
    }
    // Bring up the video engines that were planned and confirmed (AMD VCN 1.0: Ryzen 2000/3000 APUs). An engine that comes up is tested at once: it decodes a short
    // H.264 stream this OS encodes, and must give the CPU's pictures byte for byte, or it is stopped again and H.264 stays on the CPU; H.265 and AV1 have their own tests and stay on the
    // CPU when they fail. Engines are stopped at shutdown.
    // This is the one switch: set it false to leave every engine unstarted.
    constexpr bool kStartVideoEngines = true;
    if (kStartVideoEngines && clock_ok && paging_ok) {
      for (uint32_t i = 0; i < video_codecs.count(); ++i) {
        hardware::IVideoCodecDevice& engine_device = *video_codecs.at(i);
        if (!engine_device.firmware_status()[0]) continue;   // nothing to start on this device
        const bool started = engine_device.start_hardware(clock);
        const bool decoding = started && engine_device.info().accelerated_decode_codecs != 0;
        Entry video(started ? Level::Ok : Level::Warning, "Video");
        video.text(engine_device.name());
        if (decoding) {
          // Which codecs run on the engine: each passed its own self-test.
          const uint32_t served = engine_device.info().accelerated_decode_codecs;
          video.text(": engine started, decode runs on it for");
          const struct { hardware::VideoCodec codec; const char* name; } codecs[] = {{hardware::VideoCodec::H264, " H.264"}, {hardware::VideoCodec::Hevc, " H.265"}, {hardware::VideoCodec::Vp9, " VP9"}, {hardware::VideoCodec::Av1, " AV1"}};
          for (const auto& c : codecs)
            if (served & hardware::codec_mask(c.codec)) video.text(c.name);
          video.text(" (self-tests passed)");
        } else {
          video.text(started ? ": engine started (idle)" : ": engine not started - ").text(started ? "" : engine_device.firmware_status());
        }
      }
    }
    // The system database: one file on the root volume (/AppStore/System/system.db) where the OS and its programs keep their settings; the database query
    // window can open it too (system://localhost/system). Without a writable root volume it lives in memory for this session.
    database::local::LocalDatabase system_database(shared_allocator, &explorer_files.file_system(), filesystem::layout::kSystemDatabase);
    system_database.open();
    // The root certificates for https: the bundle on the system volume (/certs/ca-bundle.pem) or else the one built into the image.
    crypto::TrustStore web_roots(shared_allocator);
    network::TlsConfig web_tls;
    const uint32_t roots = network::load_ca_bundle(web_roots, &explorer_files.file_system(), shared_allocator);
    web_tls.trust = &web_roots;
    web_tls.random = &smb_random;
    web_tls.clock = unix_seconds;
    web_tls.clock_context = st;
    page_loader.set_tls(&web_tls);
    Entry(Level::Info, "Web").dec(roots).text(" trusted root certificates");

    database::local::LocalDriver system_driver(shared_allocator);
    system_driver.add_database("system", &system_database);
    databases.manager().add_driver(&system_driver);
    database::local::SystemSettings settings(shared_allocator, system_database);
    const int64_t boots = settings.get_int("system", "boot_count", 0) + 1;
    settings.set_int("system", "boot_count", boots);
    desktop.set_settings(&settings, "system://localhost/system");
    if (system_database.persistent()) Entry(Level::Ok, "Database").text("system database ").text(system_database.path()).text(", boot ").dec(static_cast<uint64_t>(boots));
    else Entry(Level::Warning, "Database").text("system database is in memory only: ").text(system_database.problem().view());
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
    desktop.set_monitors(&monitors);
    desktop.set_graphics(&dxg_kernel, &system_graphics);
    VBoxDisplayContext vbox_display{&monitors, &desktop};
    guest.set_display_request_handler(on_vbox_display_request, &vbox_display);
    desktop.set_display_changer(&DisplayPipeline::apply, &display_pipeline);
    desktop.set_hardware_inventory(&inventory);
    desktop.set_device_details(&inventory);
    desktop.set_network_details(&network_details);
    desktop.set_wireless_details(&wireless_details);
#ifdef TCOS_BLUETOOTH
    if (bluetooth_details) desktop.set_bluetooth_details(bluetooth_details.get());
#endif
    desktop.set_media_services(mixer.get(), &video_codecs, &clock);
    desktop.set_capture_hub(capture_hub.get());
    desktop.set_midi_output(&usb_midi);
    desktop.set_clipboard(&clipboard);
    desktop.set_file_system(&explorer_files.file_system());
    desktop.set_program_launcher(&program_launcher);
    explorer_files.set_notifier(on_storage_change, &desktop);
    EjectContext eject_context{&explorer_files, {}};
    desktop.explorer().set_eject_handler(on_eject, &eject_context);
    if (!explorer_files.start_service()) Entry(Level::Warning, "Storage").text("could not start hot-plug polling; USB sticks plugged in later are not seen");
    SystemMonitor monitor(allocator, shared_allocator);
    monitor.set_gpu(gpu);
    if (system_npu.hardware_present()) {   // (the software NPU is the CPU: it has no meter of its own)
      npu.set_clock(&clock);
      monitor.set_npu(&npu);
    }
    monitor.set_pci(&pci);
    // The battery: only a machine whose ACPI tables describe one runs any AML (a laptop; a desktop or a virtual machine has none).
    hardware::SystemAcpiHost acpi_host(&pci, paging_ok ? &mmio : nullptr);
    hardware::AcpiBattery battery(acpi_host, shared_allocator);
    hardware::AcpiTables acpi_tables(st);
    if (acpi_tables.initialize()) {
      const bool has_battery = battery.initialize(acpi_tables);
      Entry e(battery.tables_faulty() ? Level::Warning : Level::Info, "ACPI");
      e.dec(battery.object_count()).text(" objects from ").dec(battery.tables_loaded()).text(" table(s)");
      if (battery.tables_faulty()) e.text("; ").dec(battery.tables_faulty()).text(" stopped at a fault (").text(battery.last_load_error()).text(")");
      if (has_battery) {
        monitor.set_battery(&battery);
        const hardware::BatteryStatus first = battery.read();
        Entry e2(Level::Ok, "Battery");
        e2.dec(battery.battery_count()).text(" battery device(s)");
        if (!first.present) e2.text(", none inserted");
        else if (first.percent < 0) e2.text(", charge unknown");
        else e2.text(", ").dec(static_cast<uint32_t>(first.percent)).text("%").text(first.state == hardware::BatteryState::Charging ? ", charging" : first.state == hardware::BatteryState::Discharging ? ", discharging" : first.state == hardware::BatteryState::Full ? ", full" : "");
      }
    }
#ifdef TCOS_ACPI_EVENTS
    // The ACPI events: the general purpose events and the fixed buttons are read by the loop (and when the SCI arrives, at once), the embedded controller's events are asked for, and the
    // buttons of the laptop (power, sleep, lid) are reported. With the Thunderbolt controller its _PRW wake event is armed too: the firmware's method for the event says Notify(device, 2).
    hardware::SystemAcpiPorts acpi_ports(paging_ok ? &register_mapper : nullptr);
    UniquePointer<hardware::AcpiEvents> acpi_events;
    AcpiButtonSink acpi_buttons;
    AcpiSciState acpi_sci;
    UniquePointer<hardware::MmioIoApicWindow> acpi_ioapic_window;
    UniquePointer<hardware::IoApic> acpi_ioapic;
    bool acpi_sci_working = false;
    if (acpi_tables.initialize()) {
      acpi_events = UniquePointer<hardware::AcpiEvents>::make(shared_allocator, acpi_host, acpi_ports, shared_allocator);
      if (acpi_events && acpi_events->initialize(acpi_tables) && acpi_events->enable_acpi_mode()) {
        acpi_events->set_button_listener(&acpi_buttons);
        const bool has_ec = acpi_events->enable_embedded_controller();
        const uint32_t buttons = acpi_events->enable_buttons();
        Entry(Level::Info, "ACPI").text("events: ").text(has_ec ? "embedded controller armed, " : "no embedded controller events, ").dec(buttons).text(" button source(s)");
        bool lid = false;
        if (acpi_events->lid_open(&lid)) Entry(Level::Info, "ACPI").text(lid ? "lid is open" : "lid is closed");
        // the SCI: the I/O APIC input the MADT names, the vector kVectorAcpiSci to this core
        const uint32_t apic_id = kernel::running() ? kernel::apic::this_apic_id() : 0xFFFF;
        const ConstByteView madt = acpi_tables.find("APIC");
        hardware::IoApicInfo ioapics[8];
        const uint32_t ioapic_count = hardware::madt_ioapics(madt, ArrayView<hardware::IoApicInfo>(ioapics, 8));
        const hardware::SciRoute route = hardware::madt_sci_route(madt, acpi_events->fadt().sci_interrupt);
        if (paging_ok && apic_id <= 0xFF && ioapic_count != 0 && acpi_events->fadt().sci_interrupt != 0) {
          for (uint32_t i = 0; i < ioapic_count && i < 8 && !acpi_sci_working; ++i) {
            hardware::MmioRegion region;
            if (!register_mapper.map_registers(ioapics[i].address, 0x20, &region)) continue;
            acpi_ioapic_window = UniquePointer<hardware::MmioIoApicWindow>::make(shared_allocator, region);
            if (!acpi_ioapic_window) break;
            acpi_ioapic = UniquePointer<hardware::IoApic>::make(shared_allocator, *acpi_ioapic_window, ioapics[i].gsi_base);
            if (acpi_ioapic && acpi_ioapic->contains(route.gsi)) {
              acpi_sci.ioapic = acpi_ioapic.get();
              acpi_sci.gsi = route.gsi;
              kernel::set_interrupt_handler(kernel::kVectorAcpiSci, &on_acpi_sci, &acpi_sci);
              acpi_sci_working = acpi_ioapic->route(route.gsi, kernel::kVectorAcpiSci, apic_id, route.level, route.active_low, false);
              if (!acpi_sci_working) kernel::set_interrupt_handler(kernel::kVectorAcpiSci, nullptr, nullptr);
            }
            if (!acpi_sci_working) {
              acpi_ioapic = UniquePointer<hardware::IoApic>();
              acpi_ioapic_window = UniquePointer<hardware::MmioIoApicWindow>();
            }
          }
        }
        Entry(acpi_sci_working ? Level::Ok : Level::Info, "ACPI").text(acpi_sci_working ? "SCI: GSI " : "SCI not routed (events are polled); GSI ").dec(route.gsi).text(route.level ? ", level" : ", edge").text(route.active_low ? ", active low" : ", active high");
      } else {
        acpi_events = UniquePointer<hardware::AcpiEvents>();
      }
    }
#endif
#ifdef TCOS_THUNDERBOLT
    ThunderboltWakeListener thunderbolt_wake;
    uint32_t thunderbolt_wakes_seen = 0;
    if (thunderbolt && acpi_events) {
      acpi::Node* controller = acpi_events->find_pci_device(pci, thunderbolt_address);
      thunderbolt_wake.events = acpi_events.get();
      thunderbolt_wake.controller = controller;
      acpi_events->set_listener(&thunderbolt_wake);
      hardware::AcpiEvents::WakeSource source;
      if (controller && acpi_events->wake_source(controller, &source) && acpi_events->enable_wake(controller))
        Entry(Level::Ok, "Thunderbolt").text("wake: ACPI event ").dec(source.gpe).text(" armed (from sleep state S").dec(source.deepest_state).text(")");
      else
        Entry(Level::Info, "Thunderbolt").text(controller ? "the controller names no wake event in the ACPI tables" : "the controller is not described in the ACPI tables");
    }
#endif
    // The screen's brightness (the Fn keys and the firmware's brightness notifications): the panel's ACPI methods if the tables have them, else the PWM of the PCH as the firmware left it.
    hardware::AcpiBacklight* acpi_backlight_ptr = nullptr;
    UniquePointer<hardware::AcpiBacklight> acpi_backlight;
    UniquePointer<hardware::AcpiVideoKeys> acpi_video_keys;
    UniquePointer<hardware::IntelPchBacklight> pch_backlight;
#ifdef TCOS_ACPI_EVENTS
    if (acpi_events) {
      acpi::Node* panels[4];
      const uint32_t found = hardware::AcpiBacklight::find_devices(acpi_events->interpreter(), panels, 4);
      for (uint32_t i = 0; i < found && !acpi_backlight; ++i) {
        auto candidate = UniquePointer<hardware::AcpiBacklight>::make(shared_allocator, acpi_events->interpreter(), panels[i]);
        if (candidate && candidate->initialize()) acpi_backlight = static_cast<UniquePointer<hardware::AcpiBacklight>&&>(candidate);
      }
      if (acpi_backlight) {
        acpi_backlight_ptr = acpi_backlight.get();
        desktop.set_backlight(acpi_backlight.get());
        acpi_video_keys = UniquePointer<hardware::AcpiVideoKeys>::make(shared_allocator, *acpi_backlight,
            [](void* d, bool brighter) { static_cast<desktop::Desktop*>(d)->adjust_brightness(brighter ? 10 : -10); }, &desktop);
        if (acpi_video_keys) acpi_events->add_listener(acpi_video_keys.get());
        Entry(Level::Ok, "Display").text("backlight: ACPI, ").dec(acpi_backlight->steps()).text(" levels");
      }
    }
#endif
    if (!acpi_backlight_ptr) {
      hardware::IntelRenderHardware render;
      if (hardware::IntelGpu* intel = system_gpu.intel_gpu(); intel && intel->render_hardware(&render) && render.registers) {
        pch_backlight = UniquePointer<hardware::IntelPchBacklight>::make(shared_allocator, *render.registers);
        if (pch_backlight && pch_backlight->initialize()) {
          desktop.set_backlight(pch_backlight.get());
          Entry(Level::Ok, "Display").text("backlight: Intel PCH PWM");
        }
      }
    }
    desktop.set_system_monitor(&monitor);
    desktop.set_clock_source(read_local_time, st);
    desktop.set_diagnostic_output(console::debug_puts);
    // Three threads: this one reads input and the clock (firmware calls must stay on the boot core), a render
    // thread redraws whenever something changed, and a handler thread runs what buttons and menus fire - so a
    // slow click handler can freeze neither the screen nor the pointer.
    // The windows that were open at the last shutdown come back (files that are gone are skipped).
    const uint32_t browser_items = desktop.restore_browser_data();   // the web browser's cookies and localStorage
    if (browser_items) Entry(Level::Info, "Desktop").text("restored ").dec(browser_items).text(" cookie(s) and stored item(s) of the web browser");
    const uint32_t restored = desktop.restore_session();
    if (restored) Entry(Level::Info, "Desktop").text("restored ").dec(restored).text(" window(s) from the last session");
    const bool threaded = desktop.start();
    if (!threaded) Entry(Level::Warning, "Desktop").text("could not start the render and handler threads; running single-threaded");
    uint64_t clock_synced_us = hardware::uptime_us();
    while (!desktop.shutdown_requested()) {
      if (hardware::uptime_us() - clock_synced_us >= 60000000ull) {   // once a minute, against drift
        sync_wall_clock(st);
        clock_synced_us = hardware::uptime_us();
      }
      if (hyperv.take_shutdown_request(&hyperv_restart)) desktop.request_shutdown();   // the Hyper-V manager asked
      if (vmware_tools.take_shutdown_request(&vmware_restart)) desktop.request_shutdown();   // so did VMware
#ifdef TCOS_AMD_VEGAM
      if (vega_m) {   // the Vega M's interrupt ring: faults wedge the ring they came from. With MSI the ring is read when an interrupt came (and now and then in case one was lost)
        static uint32_t vega_seen = 0, vega_idle = 0;
        const uint32_t arrived = __atomic_load_n(&g_vega_interrupts, __ATOMIC_ACQUIRE);
        if (!vega_msi_working || arrived != vega_seen || ++vega_idle >= 250) {
          vega_seen = arrived;
          vega_idle = 0;
          vega_m->service_interrupts();
        }
        // A display plugged into or pulled out of one of the card's outputs: say so, on the debug console and as a toast, and light it up / switch it off. (The card's outputs are not part
        // of the desktop layout: the lit output shows the test picture.)
        hardware::AmdVegaM::HotplugEvent plug;
        while (vega_m->take_hotplug(&plug)) {
          // "HDMI-A-1" where the video BIOS says what the pin is, else "output 2"
          FixedString<32> where;
          if (plug.has_connector) {
            char connector_name[16];
            hardware::AtomConnectors::name(plug.connector, ArrayView<char>(connector_name, sizeof connector_name));
            where.assign(connector_name);
          } else {
            where.assign("output ");
            where.append_uint(plug.pin + 1);
          }
          FixedString<192> text;
          if (plug.throttled) {
            text.assign("Hot plug detection of ");
            text.append(where.c_str());
            text.append(" on the Radeon RX Vega M is switched off: it kept changing");
          } else {
            // (what the monitor says it is, when it is just connected and the connector has DDC lines to ask over)
            FixedString<64> monitor;
            const bool known = plug.connected && plug.has_connector && describe_vega_monitor(*vega_m, plug.connector, shared_allocator, &monitor);
            text.assign(plug.connected ? "A display " : "A display was disconnected from ");
            if (known) {
              text.append("(");
              text.append(monitor.c_str());
              text.append(") ");
            }
            if (plug.connected) text.append("was connected to ");
            text.append(where.c_str());
            text.append(" of the Radeon RX Vega M");
          }
          // A display plugged in: lit up with the mode it prefers and a test picture (the desktop is not drawn on it); one pulled out: switched off.
          if (plug.has_connector && !plug.throttled && plug.pin < 6) {
            if (plug.connected && vega_lit_crtc[plug.pin] == 0xFF) {
              hardware::AmdVegaM::LightUp lit;
              if (vega_m->light_up(plug.connector, 0xFF, &lit)) {
                vega_lit_crtc[plug.pin] = lit.crtc;
                text.append(": lit ");
                text.append_uint(lit.width);
                text.append("x");
                text.append_uint(lit.height);
              } else {
                text.append(": not lit (step ");
                text.append_uint(static_cast<uint32_t>(lit.failed));
                text.append(", mode set stage ");
                text.append_uint(static_cast<uint32_t>(lit.set.failed));
                text.append(")");
              }
            } else if (!plug.connected && vega_lit_crtc[plug.pin] != 0xFF) {
              vega_m->disable_mode(plug.connector, vega_lit_crtc[plug.pin]);
              vega_lit_crtc[plug.pin] = 0xFF;
            }
          }
          console::debug_puts("Vega M: ");
          console::debug_puts(StringView(text.c_str()));
          console::debug_puts("\n");
          desktop.notify(StringView("Display"), StringView(text.c_str()), plug.throttled ? gui::NoticeKind::Warning : gui::NoticeKind::Information);
        }
      }
#endif
#ifdef TCOS_BLUETOOTH
      if (bluetooth) {
        static uint32_t bluetooth_idle = 0;
        if (++bluetooth_idle >= 25) {                                // (about every 100 ms: events read, controllers plugged in or pulled out noticed)
          bluetooth_idle = 0;
          bluetooth->step(true);
          if (bluetooth->bonds().count() != bluetooth_bonds_saved) {                  // (a device was paired: the keys are kept)
            bluetooth_bonds_saved = bluetooth->bonds().count();
            bluetooth->save_bonds();
          }
        }
      }
#endif
#ifdef TCOS_ACPI_EVENTS
      if (acpi_events) {
        static uint32_t acpi_idle = 0;
        // the events are read when the SCI came, and every so often regardless (about every 100 ms: a few port reads), in case it did not
        if (__atomic_exchange_n(&acpi_sci.pending, 0, __ATOMIC_ACQ_REL) || ++acpi_idle >= 25) {
          acpi_idle = 0;
          acpi_events->poll();
          if (acpi_sci_working && acpi_ioapic) {
            const uint64_t flags = kernel::irq_save();                // (the index and data registers are a pair: not while the handler could use them)
            acpi_ioapic->mask(acpi_sci.gsi, false);
            kernel::irq_restore(flags);
          }
        }
        static AcpiButtonSink acpi_seen;
        if (acpi_buttons.power != acpi_seen.power) {
          acpi_seen.power = acpi_buttons.power;
          Entry(Level::Info, "ACPI").text("power button");
          desktop.request_shutdown();
        }
        if (acpi_buttons.sleep != acpi_seen.sleep) {
          acpi_seen.sleep = acpi_buttons.sleep;
          desktop.notify(StringView("Power"), StringView("Sleep button pressed (this system cannot sleep yet)"), gui::NoticeKind::Information);
        }
        if (acpi_buttons.lid_closed != acpi_seen.lid_closed || acpi_buttons.lid_opened != acpi_seen.lid_opened) {
          const bool closed = acpi_buttons.lid_closed != acpi_seen.lid_closed;
          acpi_seen.lid_closed = acpi_buttons.lid_closed;
          acpi_seen.lid_opened = acpi_buttons.lid_opened;
          Entry(Level::Info, "ACPI").text(closed ? "lid closed" : "lid opened");
          desktop.notify(StringView("Lid"), StringView(closed ? "The lid was closed" : "The lid was opened"), gui::NoticeKind::Information);
        }
      }
#endif
#ifdef TCOS_THUNDERBOLT
      if (thunderbolt) {
        static uint32_t tb_seen = 0, tb_idle = 0;                   // with MSI the channel is read when an interrupt came (and now and then in case one was lost)
        const uint32_t tb_arrived = thunderbolt->interrupts();
#ifdef TCOS_THUNDERBOLT_RUNTIME_PM
        static bool tb_asleep = false;                              // in D3hot: its registers are not read
        static uint64_t tb_idle_since = hardware::uptime_us();
        if (tb_asleep) {
          // the wake: the firmware's notification (an ACPI event polled below) or, failing that, the PME status of the function
          if (thunderbolt_wake.wakes != thunderbolt_wakes_seen || thunderbolt->wake_pending()) {
            thunderbolt_wakes_seen = thunderbolt_wake.wakes;
            tb_asleep = thunderbolt->resume() != hardware::ThunderboltDriver::Outcome::Started;
            tb_idle_since = hardware::uptime_us();
            Entry(tb_asleep ? Level::Warning : Level::Info, "Thunderbolt").text(tb_asleep ? "did not come back from runtime suspend" : "back from runtime suspend");
          }
        }
        if (!tb_asleep)
#endif
        if (!thunderbolt_msi_working || tb_arrived != tb_seen || ++tb_idle >= 250) {
          tb_seen = tb_arrived;
          tb_idle = 0;
          thunderbolt->poll();
          if (thunderbolt->usb4()) thunderbolt->usb4()->tick(hardware::uptime_us() / 1000);       // (the bandwidth groups' timers)
          if (thunderbolt_net) thunderbolt_net->poll();
        }
#ifdef TCOS_THUNDERBOLT_RUNTIME_PM
        // Idle (no device, no other host, the firmware allows it) for half a minute: sleep, with the wake armed in ACPI so that a plugged in cable is noticed.
        if (!tb_asleep) {
          const uint64_t tb_now = hardware::uptime_us();
          if (thunderbolt->device_count() != 0 || thunderbolt->xdomain_count() != 0 || !acpi_events || !thunderbolt_wake.controller) {
            tb_idle_since = tb_now;
          } else if (tb_now - tb_idle_since >= 30000000ull && thunderbolt->runtime_allowed()) {
            tb_idle_since = tb_now;                                  // (whatever happens, the next try is half a minute away)
            if (thunderbolt->runtime_suspend(true)) {
              tb_asleep = true;
              Entry(Level::Info, "Thunderbolt").text(thunderbolt->wake_armed() ? "runtime suspend, wake armed" : "runtime suspend");
            }
          }
        }
#endif
        if (acpi_events) {
          if (thunderbolt_wake.wakes != thunderbolt_wakes_seen) {
            thunderbolt_wakes_seen = thunderbolt_wake.wakes;
            Entry(Level::Info, "Thunderbolt").text("woken by the firmware's notification");
            thunderbolt->poll();                                     // (what woke it is in the control channel)
          }
        }
        const uint64_t now_devices = thunderbolt->device_count();
        if (now_devices != thunderbolt_devices) {
          desktop.notify(StringView("Thunderbolt"), StringView(now_devices > thunderbolt_devices ? "A Thunderbolt device was connected (not authorized: no PCIe tunnel)" : "A Thunderbolt device was removed"),
                         gui::NoticeKind::Information);
          thunderbolt_devices = now_devices;
        }
      }
#endif
      uint32_t tools_width, tools_height;
      if (vmware_tools.take_resolution(&tools_width, &tools_height)) on_vbox_display_request(&vbox_display, tools_width, tools_height);
      if (threaded) {
        desktop.pump(hardware::uptime_us());
      } else {
        desktop.step(hardware::uptime_us());
      }
      hardware::delay_us(4000);
    }
#ifdef TCOS_ACPI_EVENTS
    if (acpi_sci_working && acpi_ioapic) {                           // (before the handler's state goes)
      acpi_ioapic->mask(acpi_sci.gsi, true);
      kernel::set_interrupt_handler(kernel::kVectorAcpiSci, nullptr, nullptr);
    }
#endif
#ifdef TCOS_THUNDERBOLT
    if (acpi_events && thunderbolt_wake.controller) acpi_events->disable_wake(thunderbolt_wake.controller);
    if (thunderbolt_link) thunderbolt_link->detach();                // (the network interface list keeps the entry; it stops using the connection before that goes)
    if (thunderbolt_msix || thunderbolt_msi) {                      // (before the driver goes: the handler holds a pointer to it)
      thunderbolt_msix = UniquePointer<hardware::PciMsix>();
      thunderbolt_msi = UniquePointer<hardware::PciMsi>();
      kernel::set_interrupt_handler(kernel::kVectorThunderbolt, nullptr, nullptr);
      kernel::set_interrupt_handler(kernel::kVectorThunderboltData, nullptr, nullptr);
    }
#endif
    desktop.save_browser_data();
    const uint32_t kept = desktop.save_session();   // what is open now, for the next start
    Entry(Level::Info, "Desktop").text("saved ").dec(kept).text(" window(s) for the next session");
    guest.stop_service();  // (VirtualBox's resize requests touch the desktop)
    input.stop_service();  // (its notices touch the desktop)
    desktop.stop();  // before anything else draws on the screen: the render thread is the renderer's only user
    });
  } else {
    console::puts("No desktop (no graphics display). Press any key to shut down.\n");
    wait_for_key(st, input, ~0ull >> 1);
  }

  // Orderly shutdown, back on the console. Services stop before the hardware under them, and storage
  // is flushed before the devices are handed back to the firmware.
  if (gpu) {
    if (monitors.layout().count() > 1) monitors.show_single_display();   // the console is a screen of one monitor
    console::use_gpu(gpu);
    console::puts("TheoryCraftOS: shutting down\n");
  }
  audio_router.stop_service();   // (it points the mixer and the hub at devices)
  capture_hub.reset();
  mixer.reset();   // the mixer thread ends before its audio device goes
  cameras.stop_service();
  serial_ports.stop_service();
  guest.stop_service();
  hyperv.stop_service();
  vmware_tools.stop();
  printers.stop_service();   // (its thread ends before the threads do: joining it afterwards would wait for ever)
  process::set_system_sockets(nullptr);
  network::set_system_network_stacks(nullptr);
  stacks.clear();  // the stacks go before the NICs underneath them
#ifdef TCOS_SELFTESTS
  stack_demo.reset();
#endif
  Entry(Level::Ok, "Shutdown").text("network stacks stopped");
  // The virtio-gpu's interrupts end before the kernel does: after that the firmware's interrupt table is back, and a message to our vector would hit it.
  if (hardware::VirtioGpu* virtio = system_gpu.virtio_gpu()) virtio->disable_interrupts();
  kernel::stop();  // threads end here; the rest of shutdown talks to the firmware
  Entry(Level::Ok, "Shutdown").text("threads stopped");
  i2c_hid.stop(input.mice());
  input.shutdown();
  guest.shutdown();
  hyperv.shutdown();
  for (uint32_t i = 0; i < storage.count(); ++i) {
    hardware::IStorageDevice& device = *storage.at(i);
    if (device.info().read_only) continue;
    if (!device.flush()) Entry(Level::Warning, "Shutdown").text("could not flush ").text(device.name());
  }
  storage.shutdown();
  Entry(Level::Ok, "Shutdown").text("storage flushed and released");
  audio.shutdown();
  cameras.shutdown();
  serial_ports.shutdown();
#ifdef TCOS_AMD_VEGAM
  if (hardware::AmdGpu* display = system_gpu.amd_gpu()) display->set_native_display(nullptr);   // (gives the firmware's framebuffer back before the card is stopped)
  vega_m = UniquePointer<hardware::AmdVegaM>();
  if (vega_msi) {   // (after the card stopped posting)
    vega_msi = UniquePointer<hardware::PciMsi>();
    kernel::set_interrupt_handler(kernel::kVectorAmdVegaM, nullptr, nullptr);
  }
#endif
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

  st->runtime_services->reset_system((hyperv_restart || vmware_restart) ? efi::ResetType::Cold : efi::ResetType::Shutdown, efi::kSuccess, 0, nullptr);
  for (;;) cpu.halt();
}
