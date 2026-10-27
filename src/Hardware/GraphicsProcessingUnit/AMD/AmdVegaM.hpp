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


#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDce11.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGfx8.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGfx8Compute.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGmc8.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdIh3.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdIrq.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdSdma3.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdSmu7Ucode.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// Brings the parts of a Radeon RX Vega M up together, in the order of the Linux amdgpu driver's hw_init of the VI generation: the memory controller and the GART (AmdGmc8),
// the interrupt handler ring (AmdIh3), the engines' microcode through the SMC (AmdSmu7Ucode), the graphics engine with its compute queues (AmdGfx8, AmdGfx8Compute) and the
// SDMA engines (AmdSdma3); afterwards the display surface can be taken over (amd_dce11::Display). Each piece is the one tested on its own; this is the glue that gives them
// memory, firmware files and each other. It is opt-in (TCOS_AMD_VEGAM) because none of it has run on a real card: the pieces were written against simulated hardware, the
// start-up of a card that was left by the firmware but never driven by this OS may differ, and a wrong register on a real GPU can hang the machine.
//
// Memory: one block of video memory the CPU reaches through the memory BAR, at a fixed place in the visible part (kRegionOffset), clear of the firmware's framebuffer and of
// the video engine's memory (which sits at the end of the visible part, see AmdUvd6Plan): the dummy and scratch pages, the GART's page table, the microcode images and the SMC's
// scratch area, the graphics ring and its clear-state buffer, the compute queues, the SDMA rings, the interrupt ring and the OS's framebuffer. Everything is in video memory:
// the interrupt ring is told it is not snooped.
//
// A step that fails stops the ones before it, in reverse, and leaves the card as the firmware had it as far as the pieces' own stop() does that; status() says which step.
// How the CPU's pointers turn into the addresses the card is given for system memory (the page table entries of the GART). The OS keeps its memory mapped at its own
// addresses, so the default is the pointer's value; a platform that maps differently (and a test that needs addresses inside the card's 40 bits) supplies this.
class ISystemAddresses {
 public:
  virtual ~ISystemAddresses() = default;
  virtual uint64_t physical_address(const void* p) const = 0;
};

class AmdVegaM final : public amd_dce11::INativeDisplay, private AmdIrq::ISink {
 public:
  // Where the block starts in video memory, and the most the whole block may reach: the visible part of video memory is 256 MiB without Resizable BAR, and the video engine's
  // memory is in its last half.
  static constexpr uint64_t kRegionOffset = 48ull << 20;
  static constexpr uint64_t kRegionLimit = 128ull << 20;
  static constexpr uint64_t kPageBytes = 4096;

  struct Inputs {
    IVcnRegisters* registers = nullptr;           // the register BAR (BAR 5), by dword
    const IClock* clock = nullptr;
    filesystem::IFileSystem* file_system = nullptr;   // where /firmware is
    allocators::IAllocator* allocator = nullptr;
    IRegisterMapper* mapper = nullptr;            // maps the block of the memory BAR for the CPU
    uint16_t device_id = 0;
    uint64_t aperture_physical = 0;               // the memory BAR (BAR 0): where video memory shows for the CPU
    uint64_t aperture_bytes = 0;
    MmioRegion doorbells;                         // the doorbell BAR (BAR 2), at least the first 8 KiB
    uint64_t max_framebuffer_bytes = 0;           // the largest picture the display could show (0: no display part)
    uint32_t hpd_pins = 0x3F;                     // the hot plug detect pins to watch (bit n: pin n); which pin a connector is on is in the video BIOS, so by default all of them
    bool auto_recover = false;                    // service_interrupts() resets an engine that a fault or a timeout left wedged (at most kMaxRecoveries times in all)
    bool msi = false;                             // the card's MSI is on (PciMsi): the interrupt ring is told to re-arm it as the driver does (adev->irq.msi_enabled)
    const ISystemAddresses* system_addresses = nullptr;   // for upload() and download(); null: a pointer is its own address
  };
  // The sizes of the pieces of the block, for a given microcode set, GART table and framebuffer.
  struct Layout {
    bool ok = false;
    uint64_t dummy_page = 0, scratch_page = 0, gart_table = 0, smu = 0, gfx = 0, compute = 0, sdma = 0, ih = 0, ib = 0, framebuffer = 0;   // offsets in the block
    uint64_t smu_bytes = 0, gart_table_bytes = 0, framebuffer_bytes = 0, total = 0;
  };
  static constexpr uint64_t kGfxBytes = AmdGfx8::kRingBytes + 4096 + 4096;   // the ring, the write-back page, the clear-state buffer
  static constexpr uint64_t kIhBytes = (AmdIh3::kRingBytes + AmdIh3::kWritebackBytes + kPageBytes - 1) / kPageBytes * kPageBytes;
  // The staging area for the command buffers the submit functions below take (one at a time), in dwords.
  static constexpr uint32_t kIbDwords = 2048;
  static constexpr uint64_t kIbBytes = kIbDwords * 4;
  static Layout plan(uint64_t smu_bytes, uint64_t gart_table_bytes, uint32_t compute_queues, uint64_t framebuffer_bytes);

  enum class Step : uint8_t { None, MemoryMap, Firmware, Memory, MemoryController, InterruptHandler, Microcode, GraphicsEngine, Sdma, Done };
  static const char* step_name(Step step);

  explicit AmdVegaM(allocators::IAllocator& allocator) : status_(allocator) {}
  ~AmdVegaM() { shutdown(); }
  AmdVegaM(const AmdVegaM&) = delete;
  AmdVegaM& operator=(const AmdVegaM&) = delete;

  // Runs every step. True if all came up. The Config of each piece is its default (the Vega M's own values where the driver takes them from the video BIOS are guesses: see
  // AmdGfx8::Config).
  bool bring_up(const Inputs& inputs);
  bool up() const { return step_ == Step::Done; }
  Step step() const { return step_; }          // the last step that completed (Done: all); the one that failed is the next
  const char* status() const { return status_.c_str(); }
  // Stops what came up, in reverse.
  void shutdown();

  // The display part: takes the CRTC that shows the firmware's framebuffer over for a framebuffer of ours in the block. False if the block has no room for it or the display is
  // not what is expected (see amd_dce11::Display).
  bool engage_display(const amd_dce11::FirmwareFramebuffer& firmware, ByteView* memory) override;
  bool display_engaged() const override { return display_ && display_->engaged(); }
  void disengage_display() override;
  // The picture in system memory to the display's framebuffer through the SDMA engines (upload()).
  bool upload_picture(ConstByteView pixels, uint64_t offset) override { return display_ && display_->engaged() && upload(pixels, offset); }

  // Command buffers: the commands are copied into the block's staging area, padded the way the engine's driver pads (AmdGfx8::pad_ib, AmdSdma3::pad_ib), run as an indirect
  // buffer, and waited for by a fence. One at a time (the call returns when the engine is done). `commands` are PM4 packets for the graphics ring and the compute queues, SDMA
  // packets for the DMA engines; at most kIbDwords with the padding. False if the card is not up, the buffer does not fit, or the engine does not finish (see AmdGfx8::submit_ib).
  bool submit_graphics(ArrayView<const uint32_t> commands, uint32_t timeout_us = 100000);
  bool submit_compute(uint32_t queue, ArrayView<const uint32_t> commands, uint32_t timeout_us = 100000);
  bool submit_sdma(uint32_t instance, ArrayView<const uint32_t> commands, uint32_t timeout_us = 100000);

  // Copies between system memory and the block's framebuffer area (layout().framebuffer_bytes, the only part of the block meant for pictures and other data), through the
  // GART: the system pages are mapped into a window of kTransferPages GART pages (as amdgpu_ttm_gart_bind does, snooped system pages), and the SDMA engine `instance` copies
  // between the window and video memory, a window at a time. `offset` is in the framebuffer area. One call at a time; the call returns when the data has been copied. The
  // memory is addressed through Inputs::system_addresses (by default a buffer's address is the address the card is given: the OS runs with its memory mapped at its own addresses).
  // False if the card is not up, the range leaves the area, an address is beyond the card's 40 bits, or the engine does not finish (then the window stays mapped: the engine may
  // still be using the pages, and the caller must not free the memory).
  static constexpr uint32_t kTransferPages = 512;   // the first pages of the GART: 2 MiB a window
  bool upload(ConstByteView source, uint64_t offset, uint32_t instance = 0, uint32_t timeout_us = 100000);
  bool download(ByteView destination, uint64_t offset, uint32_t instance = 0, uint32_t timeout_us = 100000);

  // Interrupts: reads what the card posted to the interrupt ring and dispatches it by source (AmdIrq). A fault the engines report (a privileged register or instruction in a
  // command buffer, an SDMA illegal instruction) wedges the ring it came from, as the driver's fault handlers hand the ring to its scheduler's timeout handling; fences and
  // other events are counted. The card's interrupt line is not connected, so the caller polls this (the desktop loop does). Returns how many vectors were handled.
  uint32_t service_interrupts(uint32_t max_vectors = 64);
  const AmdIrq* irq() const { return irq_.get(); }
  // Everything the interrupt ring delivers is also passed to `observer` (not owned; null to stop), after the card has dealt with it: how the video engines, which run on their own,
  // learn of their fences. Called from service_interrupts().
  void set_event_observer(AmdIrq::ISink* observer) { observer_ = observer; }
  // Recovery. An engine that timed out on a fence or reported a fault is wedged: nothing more is queued on it. recover() resets the ones that are - the graphics engine with its
  // compute queues (AmdGfx8::recover) if the graphics ring or a compute queue is wedged, and the SDMA engines (AmdSdma3::recover) if one is - and brings them back with empty
  // rings. A fault of one compute queue resets all of them, as the driver's soft reset of the IP does. `failed` is set if an engine did not come back (its ring test failed): it is
  // then stopped; recover() tries it again (an engine that is not running counts as needing it), and if that does not help either the card needs a full restart (shutdown and
  // bring_up). Nothing is done for engines that are running and not wedged.
  struct Recovery {
    bool graphics = false, sdma = false, failed = false;
  };
  Recovery recover();
  bool wedged() const;   // any engine is, or was left stopped by a recovery that did not work
  uint32_t recoveries() const { return recoveries_; }
  static constexpr uint32_t kMaxRecoveries = 8;   // what service_interrupts() will do by itself in one life of the card (a card that keeps hanging is left alone)
  // The vertical blank interrupt of the controller that shows the OS's framebuffer (it comes every frame, so it is off until asked for): counted per controller in irq()->vblank_count().
  // False if the display is not engaged. It is switched off again when the display is given back or the card stopped.
  bool set_vblank_interrupt(bool enable);

  // For tests and diagnostics
  const AmdGmc8GartPlan& gart_plan() const { return gart_plan_; }
  const Layout& layout() const { return layout_; }
  AmdGfx8* graphics() { return gfx_.get(); }
  AmdSdma3* sdma() { return sdma_.get(); }
  AmdIh3* interrupts() { return ih_.get(); }
  AmdGmc8* memory_controller() { return gmc_.get(); }

 private:
  bool fail(Step failed, const char* text);
  void on_event(const AmdIrq::Event& event) override;
  bool transfer(uint64_t system_address, uint64_t bytes, uint64_t offset, bool to_video, uint32_t instance, uint32_t timeout_us);

  Step step_ = Step::None;
  String status_;
  Inputs inputs_;
  AmdGmc8GartPlan gart_plan_;
  Layout layout_;
  MmioRegion block_;
  uint64_t block_gpu_ = 0;
  UniquePointer<AmdGmc8> gmc_;
  UniquePointer<AmdIh3> ih_;
  UniquePointer<AmdIrq> irq_;
  UniquePointer<amd_dce11::Interrupts> display_irq_;
  int vblank_crtc_ = -1;
  uint32_t recoveries_ = 0;
  AmdIrq::ISink* observer_ = nullptr;
  UniquePointer<AmdGfx8Compute> compute_;
  UniquePointer<AmdGfx8> gfx_;
  UniquePointer<AmdSdma3> sdma_;
  UniquePointer<amd_dce11::Display> display_;
  uint64_t cpu_base_ = 0, gpu_base_ = 0;
};

}  // namespace hardware
