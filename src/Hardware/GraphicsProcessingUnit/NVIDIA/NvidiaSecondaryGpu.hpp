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

// The display driver of an NVIDIA GPU that is NOT the boot display (a hybrid laptop's discrete GPU, which often drives the HDMI and USB-C
// ports, or a second card): no firmware framebuffer and no mode the firmware left, so everything is set up from scratch.
//
//   - NvidiaNative's start as a secondary GPU (configure_secondary: the VBIOS from the card, the init scripts when it was not POSTed), the
//     board's connectors and their monitors' EDID.
//   - The first connected connector whose output a SOR can drive (TMDS for HDMI/DVI, DisplayPort with link training over the connector's AUX
//     channel; internal panels are left to the integrated GPU), the EDID's preferred mode (src/Hardware/Monitor/Edid) with its detailed
//     timing, or the largest mode of the EDID the output can carry.
//   - VRAM through BAR1 set up the way nouveau does it (Nvkm/NvBar: BAR1 and BAR2 instance blocks and page tables built through the PRAMIN
//     window, 0x001704/0x001714 pointed at them): the first Bar1LinearVram::linear_bytes_for() bytes of BAR1 map VRAM 1:1, so every display
//     object (scanout surface, push buffer, instance memory, notifier) is at BAR1 + its VRAM offset for the CPU. Not the legacy "BAR1 is
//     linear while 0x001704 is invalid" state the boot display relies on: nothing proves a secondary GPU's BAR1 is in that state (its VBIOS or
//     a previous driver may have left the virtual BAR on), whereas the page tables say exactly what BAR1 shows.
//   - The mode set on the GF119..Pascal display (Nvkm/NvDisplayMode's NvModeSetter: head, SOR, raster, the supervisor polled), the head
//     pointed at the surface, drawing through the CPU's view (FramebufferGpu).
//
// Everything that writes to the card is built only with TCOS_NVIDIA_NATIVE (off by default: none of it has run on a real card). Without it
// initialize() fails before touching the GPU, so the GpuPool gives it the read-only probe instead.
//
// The head's VPLL is set in supervisor 2.1 through Nvkm/NvPll (NvVpllClock: the VBIOS's PLL limits table, gf100_devinit_pll_set).
//
// Not done: Tesla (NV50 display, no virtual BAR here), Volta/Turing (their mode setting is not written), Ampere+ (GSP); the VBIOS IED scripts
// and DP drive levels (placeholders); HDMI infoframes/audio; several heads at once; hot plug re-routing (a monitor plugged in later is not
// lit until the driver is built again).
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/FramebufferGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/GpuContext.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaNative.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvAux.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBar.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplay.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplayMode.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvFb.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvInstmem.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvPll.hpp"
#include "Hardware/Monitor/IMonitorSource.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {
namespace nv {

// ---------------------------------------------------------------------------------------------------------------------------------------
// Modes from an EDID
// ---------------------------------------------------------------------------------------------------------------------------------------
struct EdidMode {
  DisplayTiming timing;
  uint32_t refresh_millihz = 0;
  bool preferred = false;
};
// An 18-byte detailed timing descriptor as DRM's drm_mode_detailed reads it (an interlaced one's vertical values doubled to the frame, the total
// made odd). False for a monitor descriptor (clock 0), a stereo mode, or one with no size or sync width.
[[nodiscard]] bool decode_detailed_timing(ConstByteView descriptor, DisplayTiming* out);
// The detailed timings of the base block and of every CTA extension, one per size (the first kept), into `out`; the one parse_edid calls
// preferred (else the first of the base block) is marked. Returns how many.
uint32_t edid_modes(ConstByteView edid, ArrayView<EdidMode> out);
// The highest pixel clock an output carries at 24 bits per pixel: single-link TMDS 165 MHz, dual-link 330 MHz (HDMI 2.0 scrambling is not
// done), DisplayPort what the DCB's link rate and lanes allow (HBR2 x4 when it says nothing; the sink's own limit is met in link training).
uint32_t max_output_clock_khz(const DcbOutput& output);
// The mode to start in: the preferred one when the output carries it, else the one with the most pixels (then the highest refresh); empty when
// none fits.
Optional<uint32_t> choose_mode(ArrayView<const EdidMode> modes, uint32_t max_clock_khz);

// ---------------------------------------------------------------------------------------------------------------------------------------
// VRAM through a BAR1 of this driver's own
// ---------------------------------------------------------------------------------------------------------------------------------------
// The locks the BAR set-up's parts serialize with (each its own: the VMM's is held while tables are allocated and written). In the kernel
// RawNvLocks (NvSecondaryDisplay's own); a host test passes locks that do not mask interrupts.
struct Bar1Locks {
  INvLock* heap = nullptr;
  INvLock* pramin = nullptr;
  INvLock* tlb = nullptr;
  INvLock* bar1 = nullptr;
  INvLock* bar2 = nullptr;
  bool complete() const { return heap && pramin && tlb && bar1 && bar2; }
};

// The VRAM heap (NvFb) over [reserved head, linear bytes), page tables and BAR instance blocks through the PRAMIN window (NvVramMemory), and
// NvBar with the first `linear` bytes of BAR1 mapped 1:1 to VRAM. allocate() hands out VRAM objects with their CPU view in that window.
class Bar1LinearVram final : public INvMemory {
 public:
  static constexpr uint64_t kBar1VirtualBytes = 256u << 20;   // the BAR1 address space set up (NvidiaNative::kBar1Visible: the least such a card has)
  static constexpr uint64_t kBar2VirtualBytes = 32u << 20;    // BAR2's (nothing is mapped there; nouveau sets both up)
  static constexpr uint64_t kMaxLinear = 128u << 20;          // two surfaces of 4096x2160 and the display's objects

  // `window`: the CPU's view of BAR1 from offset 0, at least linear_bytes_for(vram) long. `vram`: what probe_vram read. `locks` must be
  // complete and outlive this.
  Bar1LinearVram(INvRegisters& registers, const Chip& chip, const VramInfo& vram, ByteView window, const Bar1Locks& locks, NvTimeouts timeouts = {});
  ~Bar1LinearVram();
  Bar1LinearVram(const Bar1LinearVram&) = delete;
  Bar1LinearVram& operator=(const Bar1LinearVram&) = delete;

  // How much of BAR1 is mapped linearly: kMaxLinear, less when the VRAM below the firmware's tail is smaller; 0 when there is too little VRAM.
  static uint64_t linear_bytes_for(const VramInfo& vram);
  // Builds the BARs (writes: the page tables through PRAMIN, 0x001714 and 0x001704) and flushes BAR1's TLB.
  [[nodiscard]] BarStatus init();
  // Turns the virtual BARs off again (BAR1 back to the reset state). Objects still allocated stay allocated (and their CPU view stale).
  void fini();
  bool ready() const { return bar_.ready(); }

  [[nodiscard]] GpuMemory allocate(MemoryTarget target, uint64_t bytes, uint64_t alignment) override;
  void release(GpuMemory& memory) override;
  void flush(const GpuMemory& memory) override;
  uint64_t vram_bytes() const override { return vram_.bytes; }
  // Makes CPU writes through BAR1 visible to the GPU (the BAR flush); false on a timeout.
  [[nodiscard]] bool flush_writes() { return bar_.ready() ? bar_.flush_writes() : true; }

  uint64_t linear_bytes() const { return window_.size(); }
  NvBar& bar() { return bar_; }
  NvFb& heap() { return fb_; }
  // Heap allocations the BARs' own tables take (counted at init(): what is left allocated once every object was released).
  uint32_t table_allocations() const { return table_allocations_; }

 private:
  static VramInfo heap_info(const VramInfo& vram, uint64_t linear);

  INvRegisters& r_;
  Chip chip_;
  VramInfo vram_;
  ByteView window_;
  NvFb fb_;
  PraminWindow pramin_;
  NvVramMemory tables_;
  NvTlb tlb_;
  NvBar bar_;
  uint32_t table_allocations_ = 0;
};

// The heads' pixel clocks (nouveau: nvkm_devinit_pll_set(devinit, PLL_VPLL0 + head, khz)) through Nvkm/NvPll: the VBIOS's PLL limits, the
// coefficients, the VPLL registers of the chip's family. Heads 0..3.
class NvVpllClock final : public INvPixelClock {
 public:
  NvVpllClock(ConstByteView bios_image, const Chip& chip, INvRegisters& registers) : bios_(bios_image), chip_(chip), r_(registers) {}
  [[nodiscard]] bool set_vpll(uint32_t head, uint32_t khz) override;
  const NvPllSetReport& last() const { return last_; }

 private:
  NvBios bios_;
  Chip chip_;
  INvRegisters& r_;
  NvPllSetReport last_;
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// The display (testable without PCI: everything comes in through Inputs)
// ---------------------------------------------------------------------------------------------------------------------------------------
class NvSecondaryDisplay {
 public:
  static constexpr uint32_t kMaxModes = 16;
  static constexpr uint32_t kDepthBpp = 24;

  struct Inputs {
    INvRegisters* registers = nullptr;                  // BAR0
    Chip chip;
    const BoardInfo* board = nullptr;
    ArrayView<const NvidiaNative::Connector> connectors;  // the board's connectors with their EDIDs (NvidiaNative::connectors())
    VramInfo vram;                                      // what probe_vram read
    ByteView bar1;                                      // the CPU's view of BAR1 from offset 0 (at least Bar1LinearVram::linear_bytes_for(vram))
    allocators::IAllocator* allocator = nullptr;
    INvPixelClock* clock = nullptr;                     // the VPLL (null: left as the VBIOS set it)
    IDpcdAux* aux = nullptr;                            // a DisplayPort output's AUX channel (null: the connector's own, NvAuxChannel)
    DisplayTimeouts timeouts;
    NvTimeouts memory_timeouts;
    Bar1Locks locks;                                    // (not complete: the display's own kernel locks)
  };

  explicit NvSecondaryDisplay(const Inputs& inputs) : in_(inputs) {}
  ~NvSecondaryDisplay() { shutdown(); }
  NvSecondaryDisplay(const NvSecondaryDisplay&) = delete;
  NvSecondaryDisplay& operator=(const NvSecondaryDisplay&) = delete;

  // Which connector, which modes (reads only: the display engine's probe), then BAR1 and the mode set (writes). On a failure everything that
  // was set up is taken down again and status() says why.
  [[nodiscard]] DisplayStatus start();
  // Another mode of the list: a new surface, the mode set, the old surface released (on a failure the old mode is set again).
  [[nodiscard]] DisplayStatus set_mode(uint32_t index);
  // Blanks the head, stops the display's channels, releases every VRAM object and turns the virtual BARs off.
  void shutdown();
  bool running() const { return running_; }

  uint32_t mode_count() const { return mode_count_; }
  const EdidMode* mode(uint32_t index) const { return index < mode_count_ ? &modes_[index] : nullptr; }
  uint32_t current_mode() const { return current_; }
  int32_t connector() const { return connector_; }    // the board connector driven (-1: none)
  const DcbOutput& output() const { return output_; }
  const OutputRoute& route() const { return route_; }
  // The picture: the current mode's rows, `pitch_bytes()` apart, X8R8G8B8 (B, G, R, x in memory).
  ByteView pixels() const { return surface_.memory.cpu.first(uint64_t{surface_.scanout.pitch_bytes} * surface_.scanout.height); }
  const ScanoutSurface& surface() const { return surface_.scanout; }
  uint32_t width() const { return surface_.scanout.width; }
  uint32_t height() const { return surface_.scanout.height; }
  uint32_t pitch_bytes() const { return surface_.scanout.pitch_bytes; }
  // Makes what the CPU drew visible to the scanout (the BAR write flush).
  void flush();

  const char* status() const { return status_; }
  DisplayStatus last_status() const { return last_; }
  Bar1LinearVram* memory() { return memory_.get(); }
  NvDisplayEngine* engine() { return engine_.get(); }

 private:
  struct Surface {
    GpuMemory memory;
    ScanoutSurface scanout;
  };
  DisplayStatus fail(DisplayStatus status, const char* why);
  bool pick_output();
  [[nodiscard]] bool allocate_surface(const EdidMode& mode, Surface* out);
  void release_surface(Surface& surface);
  DisplayStatus program(const EdidMode& mode, const Surface& surface);

  Inputs in_;
  RawNvLock kernel_locks_[5];
  EdidMode modes_[kMaxModes];
  uint32_t mode_count_ = 0;
  uint32_t current_ = 0;
  int32_t connector_ = -1;
  DcbOutput output_;
  OutputRoute route_;
  UniquePointer<Bar1LinearVram> memory_;
  UniquePointer<NvDisplayEngine> engine_;              // (after memory_: destroyed first, its objects released into the heap)
  UniquePointer<NvAuxChannel> own_aux_;
  IDpcdAux* aux_ = nullptr;
  Surface surface_;
  bool running_ = false;
  bool mode_set_ = false;
  DisplayStatus last_ = DisplayStatus::NotClaimed;
  const char* status_ = "not started";
};

}  // namespace nv

// ---------------------------------------------------------------------------------------------------------------------------------------
// The IGraphicsProcessingUnit the GpuPool builds for a non-boot NVIDIA GPU (GpuNativeDrivers.cpp)
// ---------------------------------------------------------------------------------------------------------------------------------------
class NvidiaSecondaryGpu final : public FramebufferGpu, public IMonitorSource {
 public:
  static constexpr uint16_t kPciVendor = 0x10DE;
  static bool supports(const PciDeviceInfo& device) { return device.vendor_id == kPciVendor && device.class_code == kPciClassDisplay; }

  explicit NvidiaSecondaryGpu(const GpuContext& context);
  ~NvidiaSecondaryGpu();
  NvidiaSecondaryGpu(const NvidiaSecondaryGpu&) = delete;
  NvidiaSecondaryGpu& operator=(const NvidiaSecondaryGpu&) = delete;

  const char* name() const override { return name_[0] ? name_ : "NVIDIA GPU"; }
  // Without TCOS_NVIDIA_NATIVE: false at once (nothing touched). With it: the start, the output, BAR1 and the mode set; false (everything
  // taken down) when the chip's display is not one this driver sets modes on, no monitor is on an output it drives, or the mode set fails.
  bool initialize() override;
  void shutdown() override;
  // Why initialize() failed, or "running".
  const char* status() const { return status_; }
  nv::NvidiaNative* native() { return native_.get(); }
  nv::NvSecondaryDisplay* display() { return display_.get(); }

  uint32_t mode_count() const override;
  bool mode_info(uint32_t index, DisplayMode* out) const override;
  uint32_t current_mode() const override;
  bool set_mode(uint32_t index) override;
  void present() override;
  IMonitorSource* monitor_source() override { return this; }
  bool pci_location(PciAddress* out) const override;
  bool framebuffer_range(uint64_t* base, uint64_t* bytes) const override { (void)base; (void)bytes; return false; }   // (no firmware buffer)
  int32_t temperature_millicelsius() override { return native_ ? native_->temperature_millicelsius() : -1; }

  // IMonitorSource: every connector of the board (the connected ones first); the one driven is the only one modes can be set on.
  uint32_t connector_count() const override { return ordered_; }
  bool connector_info(uint32_t connector, ConnectorInfo* out) const override;
  uint32_t read_edid(uint32_t connector, ByteView out) override;
  bool refresh_connectors() override;
  bool mode_available(uint32_t connector, uint32_t width, uint32_t height) const override;
  uint32_t device_modes(uint32_t connector, MonitorMode* out, uint32_t capacity) const override;
  bool set_display_mode(uint32_t connector, uint32_t width, uint32_t height) override;

 private:
  bool fail(const char* why);
  void order();
  bool driven(uint32_t connector) const;
  void show();                                          // the display's surface as the framebuffer

  GpuContext context_;
  UniquePointer<nv::NvidiaNative> native_;
  UniquePointer<nv::NvVpllClock> clock_;           // (after native_: its VBIOS view is native_'s image)
  UniquePointer<nv::NvSecondaryDisplay> display_;
  char name_[48] = {};
  uint8_t order_[16] = {};
  uint32_t ordered_ = 0;
  const char* status_ = "not started";
};

}  // namespace hardware
