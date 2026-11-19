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

// The display driver of an AMD GPU that is NOT the boot display (a hybrid laptop's discrete Radeon, which often drives the HDMI and USB-C ports,
// or a second card): nobody lit a screen on it, so there is no firmware framebuffer and no mode to keep - everything is set up from scratch.
// The display engines covered are DCE 11.2: Polaris 10, 11 and 12 (Radeon RX 460/470/480/5x0, their mobile parts) and Vega M. The DCN of
// later discrete cards (Navi: DCN 2.0 and later) is not written here; the DCN 1.0 code of the Raven APU (AmdDcn1) only repoints a surface the
// firmware set up and is no starting point for it.
//
// The order is amdgpu's for the VI generation, with the legacy display code (dce_v11_0.c, kernel 6.12) and the AtomBIOS mode setting
// (amdgpu_atombios_crtc.c, amdgpu_atombios_encoders.c) the repository already has:
//
//   - The POST when the card was not POSTed (amdgpu_device_need_post: BIOS_SCRATCH_7 lacks ATOM_S7_ASIC_INIT_COMPLETE): the video BIOS's
//     ASIC_Init through the AtomBIOS interpreter (amdgpu_atom_asic_init), which trains the memory and sets the clocks.
//   - The memory controller as gmc_v8_0_mc_program leaves it (AmdGmc8::program_apertures: the system aperture over video memory, where the
//     display reads physical memory, the VGA aperture locked out, the framebuffer access on). Video memory is where MC_VM_FB_LOCATION says (the
//     POST sets it, as the driver reads it on bare metal); the CPU reaches it through the memory BAR (BAR 0). No GART: the display needs none.
//   - The display block's start (dce_v11_0_hw_init): the display power gating initialised, every DIG's PHY initialised (ATOM_TRANSMITTER_ACTION_INIT),
//     the hot plug pins enabled (without interrupts).
//   - The connectors from the video BIOS's object table (AtomConnectors), each one's EDID over its DDC lines (AmdDdc) or, for DisplayPort, I2C
//     over the AUX channel (AmdAux); internal panels (eDP, LVDS) are skipped - they belong to the integrated GPU unless a mux switches them.
//   - The first connector with a monitor and a mode: the EDID's preferred detailed timing when the output carries it (AmdMonitor's mode_valid,
//     with the DisplayPort sink's capabilities), else the largest one; the colour depth as amdgpu_connector_get_monitor_bpc picks it.
//   - The mode set through the video BIOS's command tables in the order of dce_v11_0_crtc_prepare / _mode_set / _commit and the encoder's
//     prepare / mode_set / commit (AmdModeSet: EnableDispPowerGating, UpdateCRTC_DoubleBufferRegisters, EnableCRTC, BlankCRTC, SelectCRTC_Source,
//     SetPixelClock, SetCRTC_UsingDTDTiming, SetCRTC_OverScan, EnableScaler, DIGxEncoderControl, UNIPHYTransmitterControl), the CRTC's graphics
//     plane pointed at the OS's surface (amd_dce11::Scanout), the FMT block, the line buffer and watermarks (amd_dce11::Watermarks, for the video
//     BIOS's default engine and memory clocks), and for DisplayPort the link trained (DpLinkTrainer with AmdDpSource) and the video stream on.
//   - Drawing by the CPU through the memory BAR (FramebufferGpu); present() flushes the host data path. set_mode() programs another mode of
//     the list on the same CRTC; shutdown() switches the output and CRTC off, power gates the CRTC and turns its PLL off.
//
// Everything that writes to the card is built only with TCOS_AMD_NATIVE (off by default: none of it has run on a real card). Without it,
// initialize() fails before touching the GPU, so the GpuPool gives the card the read-only probe (AmdGpuProbe) instead.
//
// Not done: the DCE clock (SetDCEClock: the display clock stays as the POST set it), the Polaris memory controller's microcode update
// (gmc_v8_0_polaris_mc_load_microcode: the POST's stays), the display golden registers, HDMI audio and infoframes, the EDID's CTA extension
// timings (the base block's detailed timings only), several CRTCs at once, hot plug (a monitor plugged in later is not lit until the driver
// is built again), Resizable BAR (the surface is in the first 96 MiB of video memory), the DCN of Navi and later.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDce11.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdModeSet.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdMonitor.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AtomBios.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AtomConnectors.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/DpLinkTrain.hpp"
#include "Hardware/GraphicsProcessingUnit/FramebufferGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/GpuContext.hpp"
#include "Hardware/Monitor/IMonitorSource.hpp"
#include "Hardware/VideoCodec/AMD/AmdGmc8Memory.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/Optional.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {
namespace amd_secondary {

// Where the driver's memory is in video memory (offsets from its first byte, which is BAR 0's first byte): clear of the start, which the firmware
// keeps (the VGA area and the copy of the video BIOS), and well inside the 256 MiB BAR every card of these families has.
constexpr uint64_t kScratchOffset = 16ull << 20;       // the page unmapped system aperture accesses go to (adev->mem_scratch)
constexpr uint64_t kSurfaceOffset = 32ull << 20;       // the scanout surface
constexpr uint64_t kSurfaceBytes = 64ull << 20;        // its room: 4096 x 4096 at 4 bytes per pixel
constexpr uint64_t kApertureBytes = kSurfaceOffset + kSurfaceBytes;   // how much of BAR 0 the driver maps
constexpr uint32_t kPitchAlignPixels = 64;             // rows 256 bytes apart at least (GRPH_PITCH of a linear surface)
constexpr uint32_t kMaxModes = 4;                      // the base block's detailed timings

// A mode of the monitor's EDID that the output can carry.
struct Mode {
  CrtcMode timing;
  uint32_t refresh_millihz = 0;
  bool preferred = false;          // the first detailed timing (EDID 1.3 and later: the preferred mode)
};
// The base block's detailed timings (edid_detailed_timing), one per size (the first kept), that `check` accepts (its clock and size are set
// per mode; null: all) and that fit kSurfaceBytes. Returns how many went into `out`.
uint32_t edid_modes(ConstByteView edid, const ModeCheck* check, ArrayView<Mode> out);
uint32_t refresh_millihz(const CrtcMode& mode);
// The mode to start in: the preferred one, else the one with the most pixels (then the highest refresh). Empty for an empty list.
Optional<uint32_t> choose_mode(ArrayView<const Mode> modes);
// A connector this driver lights: HDMI, DVI and DisplayPort (DIG outputs); not the internal panels (eDP, LVDS), not the analogue ones.
bool drivable(AtomConnectors::Type type);
// ATOM_ENCODER_MODE_* of a DIG output (false for any other connector).
[[nodiscard]] bool encoder_mode_of(AtomConnectors::Type type, uint8_t* mode);

// Why start() or set_mode() stopped.
enum class Status : uint8_t {
  Ok,
  NoAccess,            // no registers, clock, allocator or BAR 0 view
  NoBios,              // no video BIOS image, or not an AtomBIOS one
  PostFailed,          // the card needed the POST and ASIC_Init failed
  NoMemoryMap,         // the memory controller's registers do not describe video memory
  TooLittleMemory,     // video memory or the BAR 0 view is smaller than kApertureBytes
  MemoryController,    // the aperture set-up was refused
  NoConnectors,        // the video BIOS's object table lists none
  NoMonitor,           // no external connector has a monitor (no EDID)
  NoMode,              // a monitor, but none of its modes can be carried
  ModeSet,             // a command table, the scanout or the link training failed (mode_stage() says which)
  NotRunning,
  BadIndex,
};
const char* status_name(Status status);

// The step of the mode set that failed.
enum class ModeStage : uint8_t {
  Done, Unsupported, Prepare, SinkCapabilities, CrtcSource, PixelClock, Timing, Scanout, Overscan, Scaler, EncoderOff, CrtcEnable, Unblank, EncoderSetup, PanelMode,
  TransmitterEnable, LinkTraining, VideoOn,
};

// The kernel's microsecond timebase (Kernel/Delay.hpp) as an IClock.
class UptimeClock final : public IClock {
 public:
  const char* name() const override { return "uptime"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t now_us() const override;
};

// The display (testable without PCI: everything comes in through Inputs).
class Display {
 public:
  struct Inputs {
    IVcnRegisters* registers = nullptr;      // BAR 5
    const IClock* clock = nullptr;
    allocators::IAllocator* allocator = nullptr;
    ConstByteView vbios;                     // the card's video BIOS (copied by start())
    ByteView aperture;                       // the CPU's view of BAR 0 from offset 0, at least kApertureBytes
  };
  struct LinkTraining {
    Dpcd::Capabilities caps;
    DpLinkConfig config;
    DpLinkTrainer::Outcome outcome;
    uint8_t panel_mode = 0;
  };

  explicit Display(const Inputs& inputs);
  ~Display();
  Display(const Display&) = delete;
  Display& operator=(const Display&) = delete;

  // The POST if needed, the memory controller, the display block, the connectors and their monitors, the mode set. On a failure what was lit is
  // switched off again and status() says why.
  [[nodiscard]] Status start();
  // Another mode of the list on the same output (the surface cleared). On a failure the old mode is set again.
  [[nodiscard]] Status set_mode(uint32_t index);
  // The output and CRTC off, the CRTC power gated, its PLL off, the hot plug pins off.
  void shutdown();
  bool running() const { return running_; }

  bool posted() const { return posted_; }                  // start() ran ASIC_Init
  const AmdGmc8MemoryMap& memory_map() const { return map_; }
  ArrayView<const AtomConnectors::Connector> connectors() const { return ArrayView<const AtomConnectors::Connector>(connectors_, connector_count_); }
  // The EDID read from connector `index` by start() (empty: none, or not looked at).
  ConstByteView edid(uint32_t index) const { return index < connector_count_ ? ConstByteView(edid_bytes_[index]).first(edid_sizes_[index]) : ConstByteView(); }
  int32_t connector() const { return driven_; }            // the connector driven (-1: none)
  uint32_t mode_count() const { return mode_count_; }
  const Mode* mode(uint32_t index) const { return index < mode_count_ ? &modes_[index] : nullptr; }
  uint32_t current_mode() const { return current_; }
  uint8_t crtc() const { return kCrtc; }
  uint8_t pll() const { return pll_; }
  uint32_t bpc() const { return bpc_; }
  ModeStage mode_stage() const { return stage_; }
  const LinkTraining& link() const { return link_; }

  // The picture: the current mode's rows, pitch_bytes() apart, X8R8G8B8 (B, G, R, x in memory).
  ByteView pixels() const { return pixels_; }
  uint32_t width() const { return surface_.width; }
  uint32_t height() const { return surface_.height; }
  uint32_t pitch_bytes() const { return surface_.pitch_pixels * 4; }
  uint64_t surface_address() const { return surface_.address; }    // the GPU's address of the surface
  // Makes what the CPU drew visible to the display (vi_flush_hdp).
  void flush();

  const char* status() const { return status_; }
  Status last_status() const { return last_; }

 private:
  static constexpr uint8_t kCrtc = 0;
  class Host;
  Status fail(Status status, const char* why);
  bool init_display_block();
  bool read_monitor(uint32_t index);
  bool pick_output();
  ModeStage program(const AtomConnectors::Connector& connector, const CrtcMode& mode);
  void disable(const AtomConnectors::Connector& connector);
  bool plane_surface(const CrtcMode& mode);
  void update_watermarks();
  uint32_t default_clock_10khz(uint32_t offset);

  Inputs in_;
  UniquePointer<ByteBuffer> vbios_;
  UniquePointer<Host> host_;
  UniquePointer<AtomBios> bios_;
  AmdGmc8MemoryMap map_;
  AtomConnectors::Connector connectors_[AtomConnectors::kMaxConnectors];
  uint32_t connector_count_ = 0;
  static constexpr uint32_t kEdidBytes = 512;
  uint8_t edid_bytes_[AtomConnectors::kMaxConnectors][kEdidBytes] = {};
  uint32_t edid_sizes_[AtomConnectors::kMaxConnectors] = {};
  MonitorInfo monitor_;
  bool dp_sink_ = false;
  Dpcd::Capabilities dpcd_;
  Mode modes_[kMaxModes];
  uint32_t mode_count_ = 0;
  uint32_t current_ = 0;
  int32_t driven_ = -1;
  uint8_t pll_ = AmdModeSet::kPllInvalid;
  uint32_t bpc_ = 8;
  bool crtc_enabled_ = false;
  bool mode_set_ = false;
  bool running_ = false;
  bool posted_ = false;
  bool hpd_on_ = false;
  amd_dce11::ScanoutSurface surface_;
  ByteView pixels_;
  amd_dce11::WatermarkHead heads_[amd_dce11::kMaxCrtcs];
  ModeStage stage_ = ModeStage::Done;
  LinkTraining link_;
  Status last_ = Status::NotRunning;
  const char* status_ = "not started";
};

}  // namespace amd_secondary

// ---------------------------------------------------------------------------------------------------------------------------------------
// The IGraphicsProcessingUnit the GpuPool builds for a non-boot AMD GPU (GpuNativeDrivers.cpp)
// ---------------------------------------------------------------------------------------------------------------------------------------
class AmdSecondaryGpu final : public FramebufferGpu, public IMonitorSource {
 public:
  static constexpr uint16_t kPciVendor = 0x1002;
  // A discrete Polaris-family GPU (Polaris 10/11/12, Vega M): the DCE 11.2 display this driver sets modes on.
  static bool supports(const PciDeviceInfo& device);

  explicit AmdSecondaryGpu(const GpuContext& context);
  ~AmdSecondaryGpu();
  AmdSecondaryGpu(const AmdSecondaryGpu&) = delete;
  AmdSecondaryGpu& operator=(const AmdSecondaryGpu&) = delete;

  const char* name() const override { return name_; }
  // Without TCOS_AMD_NATIVE: false at once (nothing touched). With it: the BARs, the video BIOS (ACPI VFCT, the copy in video memory, the ROM
  // through the SMC), then Display::start(); false (everything taken down) when that fails.
  bool initialize() override;
  void shutdown() override;
  const char* status() const { return status_; }
  amd_secondary::Display* display() { return display_.get(); }

  uint32_t mode_count() const override;
  bool mode_info(uint32_t index, DisplayMode* out) const override;
  uint32_t current_mode() const override;
  bool set_mode(uint32_t index) override;
  void present() override;
  IMonitorSource* monitor_source() override { return this; }
  bool pci_location(PciAddress* out) const override;
  bool framebuffer_range(uint64_t* base, uint64_t* bytes) const override { (void)base; (void)bytes; return false; }   // (no firmware buffer)

  // IMonitorSource: every connector of the video BIOS's table (the driven one first, then those with a monitor); modes are set on the driven one only.
  uint32_t connector_count() const override { return ordered_; }
  bool connector_info(uint32_t connector, ConnectorInfo* out) const override;
  uint32_t read_edid(uint32_t connector, ByteView out) override;
  bool mode_available(uint32_t connector, uint32_t width, uint32_t height) const override;
  uint32_t device_modes(uint32_t connector, MonitorMode* out, uint32_t capacity) const override;
  bool set_display_mode(uint32_t connector, uint32_t width, uint32_t height) override;

  // For a host test: the display built on registers and a BAR 0 view it supplies (no PCI, no mapping). Built with TCOS_AMD_NATIVE only does it run.
  bool initialize_with(const amd_secondary::Display::Inputs& inputs);

 private:
  bool fail(const char* why);
  bool start_display(const amd_secondary::Display::Inputs& inputs);
  void order();
  bool driven(uint32_t connector) const;
  void show();

  GpuContext context_;
  UniquePointer<MmioVcnRegisters> registers_;    // BAR 5
  amd_secondary::UptimeClock clock_;
  UniquePointer<amd_secondary::Display> display_;
  char name_[64] = "AMD Radeon";
  uint8_t order_[AtomConnectors::kMaxConnectors] = {};
  uint32_t ordered_ = 0;
  const char* status_ = "not started";
};

}  // namespace hardware
