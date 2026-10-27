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
class AmdVegaM final : public amd_dce11::INativeDisplay {
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
  };
  // The sizes of the pieces of the block, for a given microcode set, GART table and framebuffer.
  struct Layout {
    bool ok = false;
    uint64_t dummy_page = 0, scratch_page = 0, gart_table = 0, smu = 0, gfx = 0, compute = 0, sdma = 0, ih = 0, framebuffer = 0;   // offsets in the block
    uint64_t smu_bytes = 0, gart_table_bytes = 0, framebuffer_bytes = 0, total = 0;
  };
  static constexpr uint64_t kGfxBytes = AmdGfx8::kRingBytes + 4096 + 4096;   // the ring, the write-back page, the clear-state buffer
  static constexpr uint64_t kIhBytes = (AmdIh3::kRingBytes + AmdIh3::kWritebackBytes + kPageBytes - 1) / kPageBytes * kPageBytes;
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

  // For tests and diagnostics
  const AmdGmc8GartPlan& gart_plan() const { return gart_plan_; }
  const Layout& layout() const { return layout_; }
  AmdGfx8* graphics() { return gfx_.get(); }
  AmdSdma3* sdma() { return sdma_.get(); }
  AmdIh3* interrupts() { return ih_.get(); }
  AmdGmc8* memory_controller() { return gmc_.get(); }

 private:
  bool fail(Step failed, const char* text);

  Step step_ = Step::None;
  String status_;
  Inputs inputs_;
  AmdGmc8GartPlan gart_plan_;
  Layout layout_;
  MmioRegion block_;
  uint64_t block_gpu_ = 0;
  UniquePointer<AmdGmc8> gmc_;
  UniquePointer<AmdIh3> ih_;
  UniquePointer<AmdGfx8Compute> compute_;
  UniquePointer<AmdGfx8> gfx_;
  UniquePointer<AmdSdma3> sdma_;
  UniquePointer<amd_dce11::Display> display_;
  uint64_t cpu_base_ = 0, gpu_base_ = 0;
};

}  // namespace hardware
