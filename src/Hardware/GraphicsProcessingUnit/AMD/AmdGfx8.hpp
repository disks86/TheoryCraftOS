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

// The start-up of the graphics engine (GFX 8) of a Radeon RX Vega M, ported from the Linux amdgpu driver's gfx_v8_0.c (gfx_v8_0_hw_init: golden registers, run list controller
// (RLC) reset and start, command processor (CP) ring 0 set-up and start, ring test; gfx_v8_0_hw_fini) with the register numbers generated from AMD's own headers
// (AmdGfx8Registers.inc, scripts/gen_vcn1_regs.py --vi). The registers are the card's register BAR (BAR 5), numbered by dword.
//
// It starts from a card whose microcode is loaded: on this chip the SMC loads the engines' microcode (AmdSmu7Ucode), not the driver, so there is no firmware upload here. What
// it leaves out, and what is needed before the engine draws anything:
//  - gfx_v8_0_constants_init: the tiling mode tables, the render back-end set-up and the address configuration the 3D engine's memory accesses depend on;
//  - gfx_v8_0_init_pg (RLC clear-state buffer, save/restore list, power gating), and the clear-state context defaults (vi_cs_data) that the start sequence would send;
//  - the compute queues (KIQ and the kernel compute queues of the MEC), doorbells (the write pointer goes through its register) and every interrupt (the driver polls);
//  - the RLC's safe mode and the wait for an idle engine that gfx_v8_0_hw_fini makes before it halts the CP.
// The ring test is the one the driver runs: a register write through the ring.
class AmdGfx8 {
 public:
  struct Config {
    // What gfx_v8_0_gpu_early_init takes from the video BIOS (amdgpu_atombios_get_gfx_info) and its tables for the Vega M.
    uint32_t shader_engines = 2, sh_per_se = 1;
    uint32_t max_hw_contexts = 8;
    // The raster configuration of the render back-ends (the driver reads it from the harvest registers; these are the golden values of the Vega M's table, which is what a
    // part with every back-end enabled has).
    uint32_t raster_config = 0x3a00161a, raster_config_1 = 0x0000002e;
  };
  // adev->usec_timeout
  static constexpr uint32_t kDefaultTimeoutUs = 100000;
  // amdgpu_ring_init(adev, ring, 1024, ...): ring_size = roundup_pow_of_two(max_dw * 4 * sched_hw_submission), two submissions in flight.
  static constexpr uint32_t kRingBytes = 8192;
  static constexpr uint32_t kRingDwords = kRingBytes / 4;
  static constexpr uint32_t kAlignMask = 0xFF;   // gfx8_0_ring_funcs_gfx.align_mask: submissions are padded to 256 dwords
  static constexpr uint32_t kWritebackBytes = 16;   // the read pointer at offset 0, the write pointer poll word at offset 8

  // Where the ring lives: video memory the CPU can write, with the address the GPU reads it at. `writeback` is a window of the same kind (the CP writes its read pointer
  // there); both must be 4-byte aligned in the GPU's address space (the CP takes the low two bits of the address as zero).
  struct Memory {
    uint64_t ring_gpu_address = 0;
    MmioRegion ring;                // kRingBytes
    uint64_t writeback_gpu_address = 0;
    MmioRegion writeback;           // kWritebackBytes
  };
  enum class Result : uint8_t {
    Ok,
    BadArgument,
    RingTestFailed,   // the CP did not execute the test packet: its microcode is not running
  };
  static const char* result_name(Result result);

  AmdGfx8(IVcnRegisters& registers, const IClock& clock) : registers_(registers), clock_(clock) {}
  AmdGfx8(IVcnRegisters& registers, const IClock& clock, const Config& config) : registers_(registers), clock_(clock), config_(config) {}

  // gfx_v8_0_hw_init (without constants_init and the parts listed above). The ring is empty and idle afterwards.
  Result start(const Memory& memory, uint32_t timeout_us = kDefaultTimeoutUs);
  // gfx_v8_0_hw_fini: halts the command processors (graphics and compute) and the RLC.
  void stop();

  // gfx_v8_0_ring_test_ring on its own, for later use.
  bool test_ring(uint32_t timeout_us = kDefaultTimeoutUs);
  uint32_t write_pointer() const { return wptr_; }

  // The words of the start stream (gfx_v8_0_cp_gfx_start without the context register defaults), for tests and diagnostics: `out` receives the dwords, the count is returned (0 if it does not fit).
  static uint32_t build_start_stream(const Config& config, ArrayView<uint32_t> out);

 private:
  uint32_t rd(uint32_t reg) { return registers_.read(reg); }
  void wr(uint32_t reg, uint32_t value) { registers_.write(reg, value); }
  void delay_us(uint32_t us);

  void program_golden_registers();
  void select_se_sh(uint32_t se, uint32_t sh);
  void wait_for_rlc_serdes(uint32_t timeout_us);
  void gui_idle_interrupt(bool enable);
  void rlc_stop(uint32_t timeout_us);
  void rlc_reset();
  void rlc_start();
  void cp_gfx_enable(bool enable);
  void cp_resume_ring(const Memory& memory);
  void ring_write(uint32_t dword);
  void ring_commit();

  IVcnRegisters& registers_;
  const IClock& clock_;
  Config config_;
  Memory memory_;
  uint32_t wptr_ = 0;   // in dwords, modulo the ring
};

}  // namespace hardware
