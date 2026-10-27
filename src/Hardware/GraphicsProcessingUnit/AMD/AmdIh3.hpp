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
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// The interrupt handler (IH 3.0) of a Radeon RX Vega M, ported from the Linux amdgpu driver's tonga_ih.c (the Polaris family and the VEGAM use it): a ring of 16-byte
// interrupt vectors in memory that the card's blocks (the graphics engine, the SDMA engines, the memory controller, the display) write to, a write pointer the card keeps
// (in a register and in a word after the ring) and a read pointer the driver keeps (in a register, or through a doorbell). Every other block reports faults, fences and
// end-of-pipe events here, so the engines' interrupt enables (CP_INT_CNTL_RING0 and the compute pipes' EOP enables, the memory controller's fault interrupts) mean nothing
// until this is running; this OS polls, so that is all it is used for yet: drain() returns the vectors that arrived.
//
// Left out: the interrupt sources' own enables (each block's), the PCI side (bus mastering, MSI vectors and the CPU's handler: the card's interrupt line is not
// connected to anything here), the software ring (ih_soft) and the dispatch of vectors by source. The ring is in memory the card can reach and the CPU can read; with the
// GART (AmdGmc8) that can be ordinary RAM.
class AmdIh3 {
 public:
  static constexpr uint32_t kRingBytes = 64 * 1024;       // amdgpu_ih_ring_init(adev, &adev->irq.ih, 64 * 1024, true)
  static constexpr uint32_t kVectorBytes = 16;
  static constexpr uint32_t kWritebackBytes = 8;          // after the ring: the write pointer (offset kRingBytes), the read pointer (kRingBytes + 4)
  static constexpr uint32_t kDefaultDoorbell = 0x1E8;     // AMDGPU_DOORBELL_IH (legacy_doorbell_index_init)
  static constexpr uint32_t kDefaultTimeoutUs = 100000;

  struct Config {
    bool use_doorbell = true;      // adev->irq.ih.use_doorbell: the read pointer goes through the doorbell BAR
    uint32_t doorbell_index = kDefaultDoorbell;
    bool msi = false;              // adev->irq.msi_enabled: RPTR_REARM
    bool ring_in_video_memory = false;   // the ring is in non-cacheable memory, e.g. video memory: INTERRUPT_CNTL.IH_REQ_NONSNOOP_EN (the driver puts its ring in system memory)
  };
  // `ring` is kRingBytes + kWritebackBytes long, page aligned in the GPU's address space (`ring_gpu_address`). `dummy_page_gpu_address` is the page the controller reads
  // after an interrupt (the driver's dummy page). `doorbells` is the doorbell BAR (only used with use_doorbell).
  struct Memory {
    uint64_t ring_gpu_address = 0;
    MmioRegion ring;
    uint64_t dummy_page_gpu_address = 0;
    MmioRegion doorbells;
  };
  // tonga_ih_decode_iv: the fields of the 16 bytes the Polaris family uses (the client is always the legacy one).
  struct Vector {
    uint32_t src_id = 0;
    uint32_t src_data = 0;      // 28 bits
    uint32_t ring_id = 0;
    uint32_t vmid = 0;
    uint32_t pasid = 0;
  };
  enum class Result : uint8_t { Ok, BadArgument };
  static const char* result_name(Result result);

  AmdIh3(IVcnRegisters& registers, const IClock& clock) : registers_(registers), clock_(clock) {}
  AmdIh3(IVcnRegisters& registers, const IClock& clock, const Config& config) : registers_(registers), clock_(clock), config_(config) {}

  // tonga_ih_hw_init (tonga_ih_irq_init): disables the ring, sets the interrupt control up, programs the ring and the write-back address, resets both pointers and enables.
  Result start(const Memory& memory);
  // tonga_ih_irq_disable
  void stop();
  bool running() const { return running_; }

  // amdgpu_ih_process without the dispatch: reads the write pointer (tonga_ih_get_wptr, with its ring overflow recovery), decodes up to out.size() vectors from the read
  // pointer on (tonga_ih_decode_iv) and sets the read pointer (tonga_ih_set_rptr). Returns how many. After an overflow the oldest vectors are lost; overflows() counts them.
  uint32_t drain(ArrayView<Vector> out);
  uint32_t overflows() const { return overflows_; }
  uint32_t read_pointer() const { return rptr_; }

 private:
  uint32_t rd(uint32_t reg) { return registers_.read(reg); }
  void wr(uint32_t reg, uint32_t value) { registers_.write(reg, value); }
  void enable_interrupts();
  void disable_interrupts();
  uint32_t get_wptr();
  void set_rptr();

  IVcnRegisters& registers_;
  const IClock& clock_;
  Config config_;
  Memory memory_;
  uint32_t rptr_ = 0;   // bytes
  uint32_t overflows_ = 0;
  bool running_ = false;
};

}  // namespace hardware
