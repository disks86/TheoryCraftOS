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

// Memory the GPU's engines read (nouveau's nvkm_memory over instmem and VRAM): the display engine's push buffers and context DMA objects, the
// FIFO's channel instance blocks, page tables. Implemented by the memory subsystem (Nvkm/Fb + Instmem); the display and FIFO code takes this
// interface, so it can be tested against a fake.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace nv {

enum class MemoryTarget : uint8_t { Vram, SystemCoherent, SystemNoncoherent };

struct GpuMemory {
  uint64_t gpu_address = 0;         // what the GPU's engines are given (a VRAM offset for Vram, the bus address for system memory)
  uint64_t bytes = 0;
  MemoryTarget target = MemoryTarget::Vram;
  ByteView cpu;                     // the CPU's view (through BAR2/PRAMIN or the system memory itself); empty if the CPU cannot see it
  bool valid() const { return bytes != 0; }
};

class INvMemory {
 public:
  // `bytes` aligned to `alignment` (a power of two, at least 256), zeroed; an invalid GpuMemory if there is no room.
  [[nodiscard]] virtual GpuMemory allocate(MemoryTarget target, uint64_t bytes, uint64_t alignment) = 0;
  virtual void release(GpuMemory& memory) = 0;
  // Makes CPU writes to `memory` visible to the GPU (a flush of the BAR2 write path / system memory caches).
  virtual void flush(const GpuMemory& memory) = 0;
  // The VRAM the board has, in bytes (0 if not known).
  virtual uint64_t vram_bytes() const = 0;

  // One 32-bit word of `memory` at byte `offset` (4-aligned, inside the object); false if out of range or the CPU cannot reach it.
  // Why accessors and not only `cpu`: VRAM is reached through the 1 MiB PRAMIN window (moved with 0x001700) or through BAR2 slots that
  // are remapped on demand, and both are MMIO, so an object larger than the window (or any VRAM object) has no flat view a plain
  // ByteView could describe; the memory subsystem moves the window under its lock for each access. The default serves objects that do
  // have a `cpu` view (system memory), little-endian as the GPU reads it.
  [[nodiscard]] virtual bool read32(const GpuMemory& memory, uint64_t offset, uint32_t* value) {
    if (offset % 4 || offset > memory.cpu.size() || memory.cpu.size() - offset < 4) return false;
    const ByteView b = memory.cpu.subview(offset, 4);
    *value = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    return true;
  }
  [[nodiscard]] virtual bool write32(const GpuMemory& memory, uint64_t offset, uint32_t value) {
    if (offset % 4 || offset > memory.cpu.size() || memory.cpu.size() - offset < 4) return false;
    const ByteView b = memory.cpu.subview(offset, 4);
    for (uint32_t i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>(value >> (8 * i));
    return true;
  }

 protected:
  INvMemory() = default;
  ~INvMemory() = default;
};

// Word access to VRAM by physical address (the PRAMIN window, or BAR2 slots once BAR2 is up; Nvkm/NvInstmem). The VRAM INvMemory
// (Nvkm/NvFb: NvVramMemory) is built on one of these.
class INvVramAccess {
 public:
  [[nodiscard]] virtual bool read32(uint64_t vram_address, uint32_t* value) = 0;
  [[nodiscard]] virtual bool write32(uint64_t vram_address, uint32_t value) = 0;
  // Writes `value` to every word of [vram_address, vram_address + bytes) (both 4-aligned).
  [[nodiscard]] virtual bool fill32(uint64_t vram_address, uint64_t bytes, uint32_t value) {
    if (vram_address % 4 || bytes % 4 || bytes > ~uint64_t{0} - vram_address) return false;
    for (uint64_t at = 0; at < bytes; at += 4)
      if (!write32(vram_address + at, value)) return false;
    return true;
  }
  // Makes earlier CPU writes visible to the GPU's engines (the BAR write path flush); true if nothing needed doing.
  [[nodiscard]] virtual bool flush_writes() { return true; }

 protected:
  INvVramAccess() = default;
  ~INvVramAccess() = default;
};

// The lock the memory subsystem serializes its shared state with (the VRAM heap, the PRAMIN window, a VMM's tables, the TLB invalidate
// registers, the FIFO's channel table). In the kernel this is a kernel::RawSpinLock (RawNvLock, Nvkm/NvFb.hpp); the host tests use a
// std::mutex, because SpinLock masks interrupts, which a user-mode test cannot do (the IoScheduler takes its lock the same way).
class INvLock {
 public:
  virtual void lock() = 0;
  virtual void unlock() = 0;

 protected:
  INvLock() = default;
  ~INvLock() = default;
};

class NvLockGuard {
 public:
  explicit NvLockGuard(INvLock& lock) : lock_(lock) { lock_.lock(); }
  ~NvLockGuard() { lock_.unlock(); }
  NvLockGuard(const NvLockGuard&) = delete;
  NvLockGuard& operator=(const NvLockGuard&) = delete;

 private:
  INvLock& lock_;
};

}  // namespace nv
}  // namespace hardware
