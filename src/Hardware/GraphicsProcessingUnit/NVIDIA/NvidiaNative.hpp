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

// The native NVIDIA driver's parts for one GPU (Nvkm/*), put together for NvidiaGpu.
//
//   - start(): BAR0 mapped, the chip identified from NV_PMC_BOOT_0/42 (every generation from NV04 on), the VRAM size read, the video BIOS
//     read (Nvkm/NvBios: PRAMIN, PROM or the legacy shadow; the PRAMIN window and the ROM shadow bit are put back as they were, as nouveau
//     does on every boot) and its board tables parsed (outputs, I2C, GPIO, connectors), and the temperature and fan sensors started. This
//     runs on every NVIDIA GPU that is the boot display. A GPU that does not answer (powered down: BOOT_0 all ones) or a chip nobody knows
//     is left alone.
//   - The connectors (the board's connector table) and the EDID of what is plugged into each, over the connector's own DDC bus or
//     DisplayPort AUX channel (Nvkm/NvI2cEdid): read at start and again when a connector's hot plug line changes (refresh_connectors()),
//     so NvidiaGpu can list every monitor, not only the one the firmware shows.
//   - engage() is the display takeover (Nvkm/NvDisplay: the firmware's mode stays, the head is pointed at a framebuffer of the OS's own in
//     VRAM). It writes to the display engine, so NvidiaGpu only calls it with TCOS_NVIDIA_NATIVE (off by default: nothing of it has run on a
//     real card). Its VRAM comes from a window of BAR1 right after the firmware's framebuffer: the firmware leaves BAR1 mapping VRAM
//     linearly (that is how the GOP framebuffer is reached), so an object at VRAM offset X is at BAR1 + X for the CPU and needs no page
//     tables. That assumption is checked as far as it can be (the GOP framebuffer must lie in BAR1's first 256 MiB) and is the first thing to
//     verify on hardware.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/GpuContext.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBios.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBiosTables.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvI2cEdid.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDevinit.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplay.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvInitScript.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvFb.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvGpio.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvRegisters.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvTherm.hpp"
#include "Hardware/Monitor/IMonitorSource.hpp"
#include "Hardware/Monitor/MonitorTypes.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {
namespace nv {

// VRAM for the display takeover: a heap over a BAR1 window, each object's CPU view a part of that window.
class Bar1VramMemory final : public INvMemory {
 public:
  // `lock` serializes the heap (the kernel's RawNvLock; a test's own lock).
  Bar1VramMemory(ByteView window, uint64_t first, uint64_t vram_bytes, INvLock& lock) : window_(window), first_(first), vram_bytes_(vram_bytes), fb_(lock) {}
  // The heap over [first, window size); false if there is no room.
  [[nodiscard]] bool init();
  [[nodiscard]] GpuMemory allocate(MemoryTarget target, uint64_t bytes, uint64_t alignment) override;
  void release(GpuMemory& memory) override;
  void flush(const GpuMemory& memory) override { (void)memory; }   // (the window is mapped uncached)
  uint64_t vram_bytes() const override { return vram_bytes_; }

 private:
  ByteView window_;          // BAR1 from VRAM offset 0
  uint64_t first_;           // the first VRAM offset handed out (past the firmware's framebuffer)
  uint64_t vram_bytes_;
  NvFb fb_;
};

class NvidiaNative {
 public:
  static constexpr uint64_t kWindowBytes = 64u << 20;    // VRAM the takeover may use after the firmware's framebuffer
  static constexpr uint64_t kBar1Visible = 256u << 20;   // the least BAR1 any card with a native display has

  NvidiaNative(const GpuContext& context, allocators::IAllocator& allocator) : context_(context), allocator_(allocator) {}
  NvidiaNative(const NvidiaNative&) = delete;
  NvidiaNative& operator=(const NvidiaNative&) = delete;
  ~NvidiaNative();

  [[nodiscard]] bool start();
  // For a GPU that is not the boot display (a laptop's discrete GPU), before start(): its video BIOS is not the one shadowed at 0xC0000, and
  // laptops whose discrete GPU has no ROM of its own hand it out through the ACPI _ROM method (`acpi_rom`, may be nullptr; must outlive this).
  void configure_secondary(INvAcpiRom* acpi_rom) { boot_display_ = false; acpi_rom_ = acpi_rom; }
  bool boot_display() const { return boot_display_; }
  // After the GPU was powered off and on again (D3cold): checks that it answers and is the same chip, and starts the sensors again (what
  // start() read from the VBIOS and the connectors is kept). False if it does not answer.
  [[nodiscard]] bool resume();
  // A secondary GPU the system firmware did not POST (common for a hybrid laptop's discrete GPU, and always after a power cut) needs the VBIOS's
  // init scripts run (Nvkm/NvDevinit::post). They write to the card, so they only run with TCOS_NVIDIA_NATIVE; otherwise start() walks them
  // dry and this says what a POST would have done (an empty reason until start() has looked; nothing for the boot display).
  const PostReport& post_report() const { return post_; }
  const Chip& chip() const { return chip_; }
  // The video BIOS: where it came from (None if no source had one) and what its tables say (empty without one).
  BiosSource bios_source() const { return bios_source_; }
  const BoardInfo& board() const { return board_; }
  const BoardParseResult& board_result() const { return board_result_; }
  // The video BIOS image the tables were read from (empty without one), for what else reads it (the PLL limits of the mode setting).
  ConstByteView bios_image() const { return bios_image_ ? bios_image_->view() : ConstByteView(); }

  // --- Connectors (as the board's connector table lists them; none without a VBIOS).
  static constexpr uint32_t kMaxEdid = 512;
  struct Connector {
    uint8_t dcb_type = 0xff;         // DCB_CONNECTOR_*
    uint8_t hpd = 0xff;              // the hot plug GPIO function (0xff: none: looked at only at start)
    bool displayport = false;
    bool connected = false;          // an EDID was read (or, with no route to read one, the hot plug line says so)
    bool hpd_state = false;
    EdidSource edid_source = EdidSource::None;
    uint8_t edid[kMaxEdid] = {};
    uint32_t edid_bytes = 0;
  };
  uint32_t connector_count() const { return connector_count_; }
  const Connector* connector(uint32_t index) const { return index < connector_count_ ? &connectors_[index] : nullptr; }
  ArrayView<const Connector> connectors() const { return ArrayView<const Connector>(connectors_).first(connector_count_); }
  // Reads the EDID of every connector again whose hot plug line changed (all of them with `all`); true if anything differs.
  bool refresh_connectors(bool all = false);
  const char* status() const { return status_; }
  uint64_t vram_bytes() const { return vram_.ok() ? vram_.info.bytes : 0; }
  // What probe_vram read (the size and the regions the firmware keeps), for a driver that manages the VRAM itself (NvidiaSecondaryGpu).
  const VramProbe& vram() const { return vram_; }
  // BAR0 (null before start() or after a failed one), for the drivers built on this one (NvidiaSecondaryGpu's display engine).
  INvRegisters* registers() { return regs_.get(); }
  int32_t temperature_millicelsius();
  NvThermMonitor* sensors() { return therm_.get(); }

  // The takeover (see above). `firmware_physical` is where the firmware's framebuffer is (in BAR1).
  [[nodiscard]] bool engage(const NvFirmwareFramebuffer& firmware, uint64_t firmware_physical, ByteView* memory);
  void disengage();
  bool engaged() const { return display_ && display_->display_engaged(); }

 private:
  bool fail(const char* why) { status_ = why; return false; }

  GpuContext context_;
  allocators::IAllocator& allocator_;
  const char* status_ = "not started";
  UniquePointer<MmioNvRegisters> regs_;
  Chip chip_;
  bool read_bios();
  bool read_connector(uint32_t index);

  BoardInfo board_;                         // (empty when no VBIOS was found: the sensors work without it, the fan's tachometer line not)
  BoardParseResult board_result_;
  BiosSource bios_source_ = BiosSource::None;
  UniquePointer<ByteBuffer> bios_image_;    // the image the tables were read from (NvBios keeps views into it)
  VramProbe vram_;
  Connector connectors_[BoardInfo::kMaxConnectors];
  uint32_t connector_count_ = 0;
  UniquePointer<NvGpio> gpio_;
  UniquePointer<NvThermMonitor> therm_;
  RawNvLock memory_lock_;
  bool boot_display_ = true;
  MmioRegion bar0_{};
  PostReport post_;
  bool run_post();
  INvAcpiRom* acpi_rom_ = nullptr;
  UniquePointer<Bar1VramMemory> memory_;
  UniquePointer<NvNativeDisplay> display_;
};

// --- What NvidiaGpu shows the monitor layer (pure: tested on the host).
// The monitor layer's kind of connector for a DCB connector type (DCB_CONNECTOR_*).
ConnectorType connector_type_for_dcb(uint8_t dcb_type);
// The order connectors are listed in: the one whose EDID is the firmware's first (the display the firmware shows; `firmware_edid` empty:
// none), then the other connected ones, then the rest, each group in the board's order. Fills `order` with board indices; returns how many.
uint32_t order_connectors(ArrayView<const NvidiaNative::Connector> connectors, ConstByteView firmware_edid, ArrayView<uint8_t> order);
// "DP-1", "HDMI-2", "eDP-1" ...: the kind's short name and `ordinal` (1 for the first of that kind), cut to fit `out` (always terminated).
void connector_label(ConnectorType type, uint32_t ordinal, ArrayView<char> out);
// What the monitor layer is told about connector `index` of a list in `order` (board indices, as order_connectors fills it): connected, its
// kind, and its label numbered among the ones of the same kind before it. False if `index` is past the list.
[[nodiscard]] bool describe_connector(ArrayView<const NvidiaNative::Connector> connectors, ArrayView<const uint8_t> order, uint32_t index, ConnectorInfo* out);

// A mapped BAR as bytes (the one place an MMIO window becomes a pointer; see NvidiaNative.cpp).
ByteView mmio_bytes(const MmioRegion& region);

}  // namespace nv
}  // namespace hardware
