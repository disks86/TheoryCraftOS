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
//   - start() is read-only: BAR0 mapped, the chip identified from NV_PMC_BOOT_0/42 (every generation from NV04 on), the VRAM size read,
//     and the temperature and fan sensors started (their first reads; nothing is written). This runs on every NVIDIA GPU that is the boot
//     display. A GPU that does not answer (powered down: BOOT_0 all ones) or a chip nobody knows is left alone.
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
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBiosTables.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplay.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvFb.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvGpio.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvRegisters.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvTherm.hpp"
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
  const Chip& chip() const { return chip_; }
  const char* status() const { return status_; }
  uint64_t vram_bytes() const { return vram_.ok() ? vram_.info.bytes : 0; }
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
  BoardInfo board_;                         // (empty until the VBIOS is read: the sensors work without it, the fan's tachometer line not)
  VramProbe vram_;
  UniquePointer<NvGpio> gpio_;
  UniquePointer<NvThermMonitor> therm_;
  RawNvLock memory_lock_;
  UniquePointer<Bar1VramMemory> memory_;
  UniquePointer<NvNativeDisplay> display_;
};

}  // namespace nv
}  // namespace hardware
