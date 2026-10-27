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

namespace hardware {

// The compute queues of the graphics engine (GFX 8) of a Radeon RX Vega M, ported from the Linux amdgpu driver's gfx_v8_0.c: the kernel interface queue (KIQ) and the kernel
// compute queues (KCQ), each a ring in video memory described by a memory queue descriptor (MQD, struct vi_mqd). The command processor's compute microcode (the MEC, loaded
// by the SMC, see AmdSmu7Ucode) runs a hardware queue descriptor (HQD) per queue; the driver programs the KIQ's HQD itself (gfx_v8_0_kiq_init_queue: the descriptor's fields
// written into the HQD registers, gfx_v8_0_mqd_commit) and then asks the KIQ to map the compute queues (SET_RESOURCES, MAP_QUEUES packets on the KIQ ring), so the
// microcode reads their descriptors and sets their HQDs up. Rings are driven by doorbells (an index into the card's doorbell BAR, BAR 2).
//
// Queue selection follows the driver's policy for a card with one MEC: the compute queues are queues 0..n-1 of pipe 0 of MEC 1, the KIQ the last pipe's queue 0 (MEC 1, pipe
// 3, queue 0), doorbells 0x10 + n for the queues and 0 for the KIQ (legacy_doorbell_index_init). The descriptor's register-order layout is generated from the kernel header
// (AmdGfx8MqdLayout.inc, scripts/gen_gfx8_mqd.py).
//
// This is driven by AmdGfx8 (attach_compute) at the places gfx_v8_0_cp_resume has them: the KIQ before the graphics ring, the compute queues and the ring tests after it.
// Left out: queue priorities (the driver makes none high priority unless asked), interrupts, the compute microcode's own state save area, per-queue VMIDs other than 0 and any
// submission (no fences, no indirect buffers): the rings carry the ring test and nothing else.
class AmdGfx8Compute {
 public:
  static constexpr uint32_t kMaxQueues = 8;                 // AMDGPU_MAX_COMPUTE_RINGS
  static constexpr uint32_t kRingBytes = 8192;              // amdgpu_ring_init(adev, ring, 1024, ...) with two submissions in flight
  static constexpr uint32_t kRingDwords = kRingBytes / 4;
  static constexpr uint32_t kEopBytes = 4096;               // GFX8_MEC_HPD_SIZE
  static constexpr uint32_t kAlignMask = 0xFF;
  // One queue's memory in the block: the descriptor (a page), the ring, the end-of-pipe buffer and a write-back page (the read pointer at offset 0, the write pointer at 8).
  static constexpr uint64_t kMqdArea = 4096, kRingArea = kRingBytes, kEopArea = kEopBytes, kWritebackArea = 4096;
  static constexpr uint64_t kSlotBytes = kMqdArea + kRingArea + kEopArea + kWritebackArea;
  static constexpr uint32_t kDefaultTimeoutUs = 100000;

  struct Config {
    uint32_t queues = 8;                                    // adev->gfx.num_compute_rings
    uint32_t kiq_me = 1, kiq_pipe = 3, kiq_queue = 0;
    uint32_t compute_me = 1, compute_pipe = 0;              // (queues 0..n-1 of this pipe)
    uint32_t kiq_doorbell = 0, first_compute_doorbell = 0x10;
  };
  // Slot 0 is the KIQ, slot i + 1 compute queue i. `block` is mapped for the CPU and starts at `gpu_address`, both page aligned. `doorbells` is the doorbell BAR.
  struct Memory {
    uint64_t gpu_address = 0;
    MmioRegion block;
    MmioRegion doorbells;
  };
  enum class Result : uint8_t {
    Ok,
    BadArgument,
    RingTestFailed,   // a queue did not execute its test packet (the Index of the first one is in failed_queue())
  };
  static const char* result_name(Result result);
  static uint64_t block_bytes(uint32_t queues) { return (uint64_t{queues} + 1) * kSlotBytes; }

  AmdGfx8Compute(IVcnRegisters& registers, const IClock& clock) : registers_(registers), clock_(clock) {}
  AmdGfx8Compute(IVcnRegisters& registers, const IClock& clock, const Config& config) : registers_(registers), clock_(clock), config_(config) {}

  // Checks the arguments and the memory and zeroes the end-of-pipe buffers (gfx_v8_0_mec_init); nothing is written to the card.
  Result attach(const Memory& memory);
  bool attached() const { return attached_; }

  // gfx_v8_0_kiq_resume / gfx_v8_0_kcq_resume / the compute part of gfx_v8_0_cp_test_all_rings / gfx_v8_0_kcq_disable (with the compute halt of cp_enable(false) left to the caller).
  void kiq_resume();
  void kcq_resume();
  Result test_rings(uint32_t timeout_us = kDefaultTimeoutUs);   // KIQ first, then the compute queues (all of them are tried; the first failure is reported)
  bool disable_queues(uint32_t timeout_us = kDefaultTimeoutUs);
  // Runs an indirect buffer on compute queue `queue` (0..queues-1) and waits for it (gfx_v8_0_ring_emit_ib_compute, gfx_v8_0_ring_emit_fence_compute): `dwords` PM4 dwords at
  // GPU address `ib_gpu_address` (4-byte aligned, below 2^48), padded by the caller to a multiple of 256 dwords with NOPs (AmdGfx8::pad_ib), then a RELEASE_MEM that writes the
  // fence number to a word of the queue's write-back page (offset 16). Only after the ring tests passed. False for a bad argument or a queue that is not ready; on a timeout the
  // queue is wedged (nothing more is queued on it) until the queues are started again.
  bool submit_ib(uint32_t queue, uint64_t ib_gpu_address, uint32_t dwords, uint32_t timeout_us = kDefaultTimeoutUs);
  bool queue_wedged(uint32_t queue) const { return queue < kMaxQueues && queues_[queue + 1].wedged; }
  uint32_t fence_sequence(uint32_t queue) const { return queue < kMaxQueues ? queues_[queue + 1].sequence : 0; }
  static constexpr uint32_t kFenceWordOffset = 16;   // in a queue's write-back page
  int failed_queue() const { return failed_queue_; }            // -1 none, 0 the KIQ, i + 1 compute queue i
  uint32_t queue_count() const { return config_.queues; }
  uint32_t write_pointer(uint32_t slot) const { return slot < kMaxQueues + 1 ? queues_[slot].wptr : 0; }

 private:
  struct Queue {
    uint32_t me = 0, pipe = 0, queue = 0, doorbell = 0;
    uint64_t mqd_gpu = 0, ring_gpu = 0, eop_gpu = 0, wb_gpu = 0;
    MmioRegion mqd, ring, wb;
    uint32_t wptr = 0;
    uint32_t sequence = 0;
    bool wedged = false;
  };
  uint32_t rd(uint32_t reg) { return registers_.read(reg); }
  void wr(uint32_t reg, uint32_t value) { registers_.write(reg, value); }
  void delay_us(uint32_t us);
  void srbm_select(uint32_t me, uint32_t pipe, uint32_t queue, uint32_t vmid);
  void cp_compute_enable(bool enable);
  void kiq_setting(const Queue& kiq);
  void mqd_init(const Queue& q, bool is_kiq, ArrayView<uint32_t> mqd);
  void mqd_commit(ArrayView<const uint32_t> mqd);
  void init_queue(const Queue& q, bool is_kiq);
  void set_mec_doorbell_range();
  void kiq_kcq_enable();
  void ring_write(Queue& q, uint32_t dword);
  void ring_commit(Queue& q);
  bool test_ring(Queue& q, uint32_t timeout_us);

  IVcnRegisters& registers_;
  const IClock& clock_;
  Config config_;
  Memory memory_;
  Queue queues_[kMaxQueues + 1];
  bool attached_ = false, ready_ = false;
  int failed_queue_ = -1;
};

}  // namespace hardware
