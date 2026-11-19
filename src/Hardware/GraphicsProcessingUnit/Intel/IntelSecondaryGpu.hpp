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

// The display driver of a discrete Intel GPU that is NOT the boot display (a hybrid laptop's Arc, or a second card): nobody lit a screen on it, so
// every part of the display engine is set up from scratch (IntelDg2Display), and the picture lives in the card's own memory.
//
// Covered: Arc Alchemist (DG2, display version 13 - the Xe_LPD display of Alder Lake-P with Synopsys PHYs on every port). Refused, before anything is
// touched: DG1 (display version 12, combo PHYs and Dekel/TC PHYs: intel_combo_phy.c, the DPLL manager - not written) and Battlemage (display
// version 14, C10/C20 PHYs: intel_cx0_phy.c and the PICA power handshake - not written); the GpuPool then gives those cards the read-only probe.
//
// The start, in order:
//   - Local memory: GU_CNTL's LMEM_INIT says the card's own firmware has trained it (xe waits for it the same way); its size from
//     SG_TILE_ADDR_RANGE (IntelGpuProbe::tile_bytes).
//   - The display core (Engine::init: DC states off, PW1, CDCLK, DBUF, MBUS, PHY calibration, PGA and PG2).
//   - The outputs: for each of DDI A, B, C and D the AUX power well, then the DPCD over AUX (a DisplayPort sink) and its EDID (I2C over AUX), else
//     the EDID over the port's GMBUS pin (HDMI, or DVI when the EDID has no HDMI block). TC1 is skipped (see IntelDg2Registers.hpp).
//   - The modes: the EDID base block's detailed timings that the output carries - HDMI: a TMDS clock the SNPS PHY has a table for, up to 340 MHz;
//     DisplayPort: a link of DpLinkConfig::choose at RBR, HBR or HBR2 - and that fit the surface; the preferred one first.
//   - The surface: kSurfaceBytes of local memory at kSurfaceLmemOffset, reached by the CPU through LMEMBAR (BAR 2) and by the display through
//     global GTT entries at kSurfaceGgtt. On DG2 the GGTT itself is in local memory (the stolen part at the top, GSMBASE) and the CPU writes it through
//     the upper half of BAR 0, as i915's ggtt_probe_common maps it; each entry is the gen8 format with GEN12_GGTT_PTE_LM set (gen8_ggtt_pte_encode).
//   - The mode set (Engine::enable) on pipe A.
//
// Everything that writes to the card is built only with TCOS_INTEL_DGPU_NATIVE (off by default: none of it has run on a real card). Without it,
// initialize() fails before touching the GPU.
//
// Assumed: the bottom of local memory (from 16 MiB) is free for the OS - the card's firmware may have left its own framebuffer there, which nothing
// shows any more; BAR 2 shows at least kLmemViewBytes (every DG2 card's BAR 2 is 256 MiB or more); BAR 0 is the 16 MiB GTTMMADR with the GGTT's
// window at 8 MiB; GGTT addresses from kSurfaceGgtt are not used by anyone else (no GuC or other driver runs).
//
// Not done: hot plug, several pipes, TC1, HDMI 2.0 (scrambling above 340 MHz), the EDID's CTA extension timings, audio and infoframes, DSC, UHBR,
// eDP (a laptop's internal panel stays the integrated GPU's), DC states with the DMC, runtime power management.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/FramebufferGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/GpuContext.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelDg2Display.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpuProbe.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/Monitor/IMonitorSource.hpp"
#include "Utility/Optional.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {
namespace intel_secondary {

constexpr uint64_t kRegisterBytes = 2ull << 20;            // the display and SoC registers used are in the first 2 MiB of BAR 0
constexpr uint64_t kGgttWindowOffset = 8ull << 20;         // the GGTT's window in BAR 0 (gen6_gttadr_offset: half of the 16 MiB BAR)
constexpr uint64_t kGgttWindowBytes = 8ull << 20;          // 1 Mi entries: 4 GiB of graphics addresses
constexpr uint64_t kSurfaceLmemOffset = 16ull << 20;       // where the surface is in local memory
constexpr uint64_t kSurfaceBytes = 64ull << 20;            // 4096 x 4096 at 4 bytes per pixel
constexpr uint64_t kLmemViewBytes = kSurfaceLmemOffset + kSurfaceBytes;   // how much of BAR 2 the driver maps
constexpr uint64_t kSurfaceGgtt = 256ull << 20;            // the surface's graphics address
constexpr uint32_t kStrideAlignBytes = 64;                 // a linear plane's stride is in 64-byte units
constexpr uint32_t kMaxModes = 4;                          // the base block's detailed timings
constexpr uint32_t kMaxHdmiTmdsKhz = 340000;               // without scrambling
constexpr uint32_t kEdidBytes = 512;

struct Mode {
  CrtcMode timing;
  uint32_t refresh_millihz = 0;
  bool preferred = false;          // the first detailed timing (EDID 1.3 and later: the preferred mode)
  // DisplayPort: the link the mode is carried on
  uint32_t lanes = 0, link_khz = 0;
};
uint32_t refresh_millihz(const CrtcMode& mode);
// What the outputs carry: an HDMI/DVI TMDS clock of the PHY's tables (and at most kMaxHdmiTmdsKhz); a DisplayPort link at RBR/HBR/HBR2 for the sink.
// Both: a CDCLK of the table, progressive, a picture that fits kSurfaceBytes.
[[nodiscard]] bool hdmi_mode_ok(const CrtcMode& mode);
[[nodiscard]] bool dp_mode_ok(const CrtcMode& mode, const Dpcd::Capabilities& caps, uint32_t* lanes, uint32_t* link_khz);
// The base block's detailed timings, one per size (the first kept), that the output carries (`caps` null: HDMI/DVI). How many went into `out`.
uint32_t edid_modes(ConstByteView edid, const Dpcd::Capabilities* caps, ArrayView<Mode> out);
// The mode to start in: the preferred one, else the most pixels (then the highest refresh). Empty for an empty list.
Optional<uint32_t> choose_mode(ArrayView<const Mode> modes);
uint32_t stride_of(uint32_t width);

enum class Status : uint8_t {
  Ok,
  NoAccess,            // no registers, clock or views
  NoLocalMemory,       // LMEM_INIT is not set: the card's firmware has not trained local memory (or the GPU does not answer)
  TooLittleMemory,     // the BAR 2 view or the GGTT window is too small
  DisplayInit,         // a power well, the CDCLK PLL or a DBUF slice did not come up
  NoMonitor,           // no DDI has a sink with an EDID
  NoMode,              // a monitor, but none of its modes can be carried
  ModeSet,             // the mode set failed (stage() says where)
  NotRunning,
  BadIndex,
};
const char* status_name(Status status);

struct Connector {
  const intel_dg2::PortInfo* port = nullptr;
  bool looked = false;             // its AUX well came up and it was asked
  bool dp = false;                 // a DisplayPort sink answered on AUX
  bool hdmi = false;               // the EDID (over DDC) has an HDMI block
  Dpcd::Capabilities caps;
  uint8_t edid[kEdidBytes] = {};
  uint32_t edid_size = 0;
};

// The display (testable without PCI: everything comes in through Inputs).
class Display {
 public:
  struct Inputs {
    intel_gt::IRegisterBank* registers = nullptr;   // BAR 0 from offset 0 (at least kRegisterBytes)
    const IClock* clock = nullptr;
    allocators::IAllocator* allocator = nullptr;    // (the EDID reads' buffers)
    MmioRegion ggtt;                                // the GGTT's window (BAR 0 + kGgttWindowOffset)
    ByteView lmem;                                  // the CPU's view of local memory through BAR 2 from offset 0, at least kLmemViewBytes
  };

  // `inputs.registers` and `inputs.clock` must not be null (IntelSecondaryGpu checks before it builds one).
  explicit Display(const Inputs& inputs);
  ~Display();
  Display(const Display&) = delete;
  Display& operator=(const Display&) = delete;

  [[nodiscard]] Status start();
  // Another mode of the list on the same output (the surface cleared). On a failure the old mode is set again.
  [[nodiscard]] Status set_mode(uint32_t index);
  void shutdown();
  bool running() const { return running_; }

  uint64_t local_memory_bytes() const { return lmem_bytes_; }
  ArrayView<const Connector> connectors() const { return ArrayView<const Connector>(connectors_, connector_count_); }
  int32_t connector() const { return driven_; }    // index into connectors() of the one driven (-1: none)
  uint32_t mode_count() const { return mode_count_; }
  const Mode* mode(uint32_t index) const { return index < mode_count_ ? &modes_[index] : nullptr; }
  uint32_t current_mode() const { return current_; }
  intel_dg2::Stage stage() const { return stage_; }
  const intel_dg2::Engine& engine() const { return engine_; }

  // The picture: the current mode's rows, pitch_bytes() apart, X8R8G8B8 (B, G, R, x in memory).
  ByteView pixels() const { return pixels_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t pitch_bytes() const { return pitch_; }

  const char* status() const { return status_; }

 private:
  Status fail(Status status, const char* why);
  void look(Connector& c);
  [[nodiscard]] bool map_surface();
  intel_dg2::Stage program(const Mode& mode);

  Inputs in_;
  intel_dg2::Engine engine_;
  uint64_t lmem_bytes_ = 0;
  Connector connectors_[intel_dg2::kPortCount];
  uint32_t connector_count_ = 0;
  Mode modes_[kMaxModes];
  uint32_t mode_count_ = 0;
  uint32_t current_ = 0;
  int32_t driven_ = -1;
  bool running_ = false;
  bool mapped_ = false;
  intel_dg2::Stage stage_ = intel_dg2::Stage::Done;
  ByteView pixels_;
  uint32_t width_ = 0, height_ = 0, pitch_ = 0;
  const char* status_ = "not started";
};

// The kernel's microsecond timebase (Kernel/Delay.hpp) as an IClock.
class UptimeClock final : public IClock {
 public:
  const char* name() const override { return "uptime"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t now_us() const override;
};

}  // namespace intel_secondary

// ---------------------------------------------------------------------------------------------------------------------------------------
// The IGraphicsProcessingUnit the GpuPool builds for a non-boot discrete Intel GPU (GpuNativeDrivers.cpp)
// ---------------------------------------------------------------------------------------------------------------------------------------
class IntelSecondaryGpu final : public FramebufferGpu, public IMonitorSource {
 public:
  static constexpr uint16_t kPciVendor = 0x8086;
  // A discrete Intel GPU with a display: DG1, DG2 (Arc Alchemist) and Battlemage. (Only DG2 is driven; the others are refused in initialize().)
  static bool supports(const PciDeviceInfo& device);

  explicit IntelSecondaryGpu(const GpuContext& context);
  ~IntelSecondaryGpu();
  IntelSecondaryGpu(const IntelSecondaryGpu&) = delete;
  IntelSecondaryGpu& operator=(const IntelSecondaryGpu&) = delete;

  const char* name() const override { return name_; }
  // Without TCOS_INTEL_DGPU_NATIVE: false at once (nothing touched). With it: DG1 and Battlemage refused; for DG2 BAR 0 and BAR 2 mapped and
  // Display::start(); false (everything taken down) when that fails.
  bool initialize() override;
  void shutdown() override;
  const char* status() const { return status_; }
  intel_secondary::Display* display() { return display_.get(); }

  uint32_t mode_count() const override;
  bool mode_info(uint32_t index, DisplayMode* out) const override;
  uint32_t current_mode() const override;
  bool set_mode(uint32_t index) override;
  void present() override {}      // (the CPU writes local memory through BAR 2; the display reads it directly)
  IMonitorSource* monitor_source() override { return this; }
  bool pci_location(PciAddress* out) const override;
  bool framebuffer_range(uint64_t* base, uint64_t* bytes) const override { (void)base; (void)bytes; return false; }   // (no firmware buffer)

  // IMonitorSource: DDI A to D, the driven one first.
  uint32_t connector_count() const override { return ordered_; }
  bool connector_info(uint32_t connector, ConnectorInfo* out) const override;
  uint32_t read_edid(uint32_t connector, ByteView out) override;
  bool mode_available(uint32_t connector, uint32_t width, uint32_t height) const override;
  uint32_t device_modes(uint32_t connector, MonitorMode* out, uint32_t capacity) const override;
  bool set_display_mode(uint32_t connector, uint32_t width, uint32_t height) override;

  // For a host test: the display built on registers and views it supplies (no PCI, no mapping), for a GPU of `device_id`. Built with
  // TCOS_INTEL_DGPU_NATIVE only does it run.
  bool initialize_with(const intel_secondary::Display::Inputs& inputs, uint16_t device_id);

 private:
  bool fail(const char* why);
  bool refuse_platform(uint16_t device_id);
  bool start_display(const intel_secondary::Display::Inputs& inputs);
  void order();
  bool driven(uint32_t connector) const;
  void show();

  GpuContext context_;
  UniquePointer<intel_gt::MmioRegisterBank> registers_;
  intel_secondary::UptimeClock clock_;
  UniquePointer<intel_secondary::Display> display_;
  char name_[64] = "Intel Arc";
  uint8_t order_[intel_dg2::kPortCount] = {};
  uint32_t ordered_ = 0;
  const char* status_ = "not started";
};

}  // namespace hardware
