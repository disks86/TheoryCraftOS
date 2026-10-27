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

// The two system DMA engines (SDMA 3.x) of a Radeon RX Vega M, ported from the Linux amdgpu driver's sdma_v3_0.c: a ring each in memory the card can reach, driven by a
// doorbell, executing packets that copy memory, fill it, write words and write fences. The microcode of the engines is loaded by the SMC (AmdSmu7Ucode). This is the engine
// the driver uses to move buffers between video memory and system memory, so with the GART (AmdGmc8) it moves data between the two without the CPU touching it.
//
// start() is sdma_v3_0_hw_init + _start: the golden registers, both engines halted, the rings programmed (size, base, read pointer write-back, doorbell, no write pointer polling),
// the engines un-halted and context switching enabled, and a ring test (a packet that writes a word of memory) on each. copy() and fill() build the driver's packets
// (sdma_v3_0_emit_copy_buffer / _emit_fill_buffer, in chunks of at most 0x3fffe0 bytes) followed by a fence write and a trap (sdma_v3_0_ring_emit_fence) and wait for the fence.
//
// Left out: the engines' interrupts (the fence's trap is sent, nothing listens: see AmdIh3), indirect buffers, page table updates, the second ring (RLC queues), the
// virtual-memory flush packets (the rings use VMID 0, physical addresses and the GART), the tiled and sub-window copies, clock gating, and the soft reset. Addresses are GPU
// addresses (40 bits).
class AmdSdma3 {
 public:
  static constexpr uint32_t kInstances = 2;
  static constexpr uint32_t kRingBytes = 8192;                 // amdgpu_ring_init(adev, ring, 1024, ...), two submissions in flight
  static constexpr uint32_t kRingDwords = kRingBytes / 4;
  static constexpr uint32_t kAlignMask = 0xF;                  // sdma_v3_0_ring_funcs.align_mask
  static constexpr uint32_t kMaxChunkBytes = 0x3fffe0;         // copy_max_bytes / fill_max_bytes
  static constexpr uint32_t kRegisterOffset[kInstances] = {0x0, 0x200};   // SDMA0_REGISTER_OFFSET, SDMA1_REGISTER_OFFSET
  // One engine's memory in the block: the ring, then a write-back page: the read pointer the engine writes (offset 0), the write pointer (8), a word the ring test writes (16),
  // the fence word (24).
  static constexpr uint64_t kSlotBytes = kRingBytes + 4096;
  static constexpr uint32_t kDefaultTimeoutUs = 100000;
  static constexpr uint32_t kDefaultDoorbell[kInstances] = {0x1E0, 0x1E1};   // AMDGPU_DOORBELL_sDMA_ENGINE0/1 (legacy_doorbell_index_init)

  struct Config {
    uint32_t gb_addr_config = 0x22011003;     // the graphics block's address configuration (AmdGfx8::Config::gb_addr_config); the engines tile like it
    uint32_t doorbell[kInstances] = {kDefaultDoorbell[0], kDefaultDoorbell[1]};
    // adev->sdma.instance[i].burst_nop: set by the driver when the firmware's feature version is at least 20 (AmdGfx8Image::feature_version of vegam_sdma.bin is 31,
    // of vegam_sdma1.bin 0). With it ring padding is one NOP packet that covers the rest.
    bool burst_nop[kInstances] = {false, false};
  };
  // `block` holds kInstances slots and starts at `gpu_address` (both page aligned); `doorbells` is the doorbell BAR.
  struct Memory {
    uint64_t gpu_address = 0;
    MmioRegion block;
    MmioRegion doorbells;
  };
  enum class Result : uint8_t {
    Ok,
    BadArgument,
    RingTestFailed,
  };
  static const char* result_name(Result result);

  AmdSdma3(IVcnRegisters& registers, const IClock& clock) : registers_(registers), clock_(clock) {}
  AmdSdma3(IVcnRegisters& registers, const IClock& clock, const Config& config) : registers_(registers), clock_(clock), config_(config) {}

  Result start(const Memory& memory, uint32_t timeout_us = kDefaultTimeoutUs);
  // sdma_v3_0_hw_fini: context switching off, the rings off, the engines halted.
  void stop();
  bool running() const { return running_; }
  int failed_instance() const { return failed_instance_; }

  // Copies / fills `bytes` of GPU memory on engine `instance` and waits for it. False for a bad argument (nothing is queued), a transfer too big for one ring pass, or an engine that
  // did not finish in time (the engine is then wedged: later calls are refused until it is started again). `fill` writes `value` into every dword; `bytes` is a multiple of 4.
  bool copy(uint32_t instance, uint64_t source, uint64_t destination, uint64_t bytes, uint32_t timeout_us = kDefaultTimeoutUs);
  bool fill(uint32_t instance, uint64_t destination, uint32_t value, uint64_t bytes, uint32_t timeout_us = kDefaultTimeoutUs);
  // Runs an indirect buffer of SDMA packets on engine `instance` and waits for it (sdma_v3_0_ring_emit_ib, then the fence): `dwords` dwords at GPU address `ib_gpu_address`
  // (32-byte aligned, 40 bits), already written to memory the engine reads and padded with NOPs to a multiple of 8 dwords (pad_ib). The packet is placed so that it ends on an
  // 8-dword boundary of the ring, as the engine requires. False for a bad argument, an engine that is not running or wedged, or a timeout (which wedges it).
  bool submit_ib(uint32_t instance, uint64_t ib_gpu_address, uint32_t dwords, uint32_t timeout_us = kDefaultTimeoutUs);
  // sdma_v3_0_ring_pad_ib: NOPs appended to `words` (holding `count`) up to a multiple of 8 dwords (a burst NOP first when the engine's firmware has it); the new count, 0 if it does not fit.
  uint32_t pad_ib(uint32_t instance, ArrayView<uint32_t> words, uint32_t count) const;
  static constexpr uint32_t kMaxIbDwords = 0xFFFFF;
  // The engine reported an illegal instruction (sdma_v3_0_process_illegal_inst_irq): nothing more is queued on it until it is started again.
  void fault(uint32_t instance) { if (instance < kInstances && running_) state_[instance].wedged = true; }
  bool wedged(uint32_t instance) const { return instance < kInstances && state_[instance].wedged; }
  uint32_t write_pointer(uint32_t instance) const { return instance < kInstances ? state_[instance].wptr : 0; }

 private:
  struct State {
    MmioRegion ring, wb;
    uint64_t ring_gpu = 0, wb_gpu = 0;
    uint32_t wptr = 0;       // dwords
    uint32_t sequence = 0;
    bool wedged = false;
  };
  uint32_t rd(uint32_t reg) { return registers_.read(reg); }
  void wr(uint32_t reg, uint32_t value) { registers_.write(reg, value); }
  void delay_us(uint32_t us);
  void program_golden_registers();
  void gfx_stop();
  void ctx_switch_enable(bool enable);
  void enable(bool enable);
  bool gfx_resume(uint32_t timeout_us);
  void ring_write(State& s, uint32_t dword);
  void insert_nop(State& s, uint32_t count, bool burst);
  void ring_commit(uint32_t instance);
  bool test_ring(uint32_t instance, uint32_t timeout_us);
  bool wait_fence(uint32_t instance, uint32_t sequence, uint32_t timeout_us);
  bool can_queue(uint32_t instance, uint64_t dwords) const;
  bool finish(uint32_t instance, uint32_t timeout_us);

  IVcnRegisters& registers_;
  const IClock& clock_;
  Config config_;
  Memory memory_;
  State state_[kInstances];
  bool running_ = false;
  int failed_instance_ = -1;
};

}  // namespace hardware
