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
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGfx8Compute.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// The start-up of the graphics engine (GFX 8) of a Radeon RX Vega M, ported from the Linux amdgpu driver's gfx_v8_0.c (gfx_v8_0_hw_init: golden registers, run list controller
// (RLC) reset and start, command processor (CP) ring 0 set-up and start, ring test; gfx_v8_0_hw_fini) with the register numbers generated from AMD's own headers
// (AmdGfx8Registers.inc, scripts/gen_vcn1_regs.py --vi). The registers are the card's register BAR (BAR 5), numbered by dword.
//
// It starts from a card whose microcode is loaded: on this chip the SMC loads the engines' microcode (AmdSmu7Ucode), not the driver, so there is no firmware upload here. What
// it leaves out, and what is needed before the engine draws anything:
//  - gfx_v8_0_init_pg beyond the clear-state buffer (the save/restore list and power gating), and gfx_v8_0_get_cu_info (it only collects information for user space);
//  - the graphics ring's doorbell (its write pointer goes through its register) and every interrupt (the driver polls);
//  - the RLC's safe mode and the wait for an idle engine that gfx_v8_0_hw_fini makes before it halts the CP.
// It does do gfx_v8_0_constants_init (address configuration, the tile mode tables, the render back-end set-up with the harvested raster configuration, the shader memory
// apertures of every VMID, the GDS and the fixed-function FIFO sizes) and the clear state: the context register defaults of vi_cs_data go to the ring and to the RLC's
// clear-state buffer. The compute queues (AmdGfx8Compute) are started too when one is attached (attach_compute): the KIQ before the graphics ring, the compute queues after it,
// the ring tests of all of them together.
// The ring test is the one the driver runs: a register write through the ring.
class AmdGfx8 {
 public:
  struct Config {
    // What gfx_v8_0_gpu_early_init takes from the video BIOS (amdgpu_atombios_get_gfx_info) and its tables for the Vega M.
    uint32_t shader_engines = 2, sh_per_se = 1, max_backends_per_se = 2;
    uint32_t max_hw_contexts = 8;
    // The fixed-function FIFO sizes and the address configuration of the table for the Vega M (TONGA_GB_ADDR_CONFIG_GOLDEN).
    uint32_t sc_prim_fifo_size_frontend = 0x20, sc_prim_fifo_size_backend = 0x100, sc_hiz_tile_fifo_size = 0x30, sc_earlyz_tile_fifo_size = 0x130;
    uint32_t gb_addr_config = 0x22011003;
    // amdgpu_vmid_mgr_set_vmid_mask: graphics and compute use VMIDs 1-7, the HSA compute processes 8-15.
    uint32_t first_kfd_vmid = 8;
    // adev->gmc.shared_aperture_start >> 48 (gmc_v8_0_sw_init), the shader memory base of every VMID but 0.
    uint32_t shared_aperture_base = 0x2000;
  };
  // adev->usec_timeout
  static constexpr uint32_t kDefaultTimeoutUs = 100000;
  // amdgpu_ring_init(adev, ring, 1024, ...): ring_size = roundup_pow_of_two(max_dw * 4 * sched_hw_submission), two submissions in flight.
  static constexpr uint32_t kRingBytes = 8192;
  static constexpr uint32_t kRingDwords = kRingBytes / 4;
  static constexpr uint32_t kAlignMask = 0xFF;   // gfx8_0_ring_funcs_gfx.align_mask: submissions are padded to 256 dwords
  static constexpr uint32_t kWritebackBytes = 16;   // the read pointer at offset 0, the write pointer poll word at offset 8
  // gfx_v8_0_get_csb_size: the clear-state buffer holds the preamble (5 dwords), the 7 extents of vi_cs_data (2 + count dwords each; 885 values), the raster configuration (4),
  // the end of the preamble (2) and the CLEAR_STATE packet (2). Not the CE partition packet of the ring start stream.
  static constexpr uint32_t kClearStateDwords = 5 + (7 * 2 + 885) + 4 + 2 + 2;
  static constexpr uint32_t kClearStateBytes = kClearStateDwords * 4;
  static constexpr uint32_t kNumVmids = 16;

  // Where the ring lives: video memory the CPU can write, with the address the GPU reads it at. `writeback` is a window of the same kind (the CP writes its read pointer
  // there); both must be 4-byte aligned in the GPU's address space (the CP takes the low two bits of the address as zero).
  struct Memory {
    uint64_t ring_gpu_address = 0;
    MmioRegion ring;                // kRingBytes
    uint64_t writeback_gpu_address = 0;
    MmioRegion writeback;           // kWritebackBytes
    uint64_t csb_gpu_address = 0;   // the RLC's clear-state buffer (4-byte aligned; the driver's page)
    MmioRegion csb;                 // kClearStateBytes
  };
  enum class Result : uint8_t {
    Ok,
    BadArgument,
    RingTestFailed,   // the CP did not execute the test packet: its microcode is not running
    ComputeFailed,    // the graphics ring works but a compute queue (or the KIQ) did not execute its test packet
  };
  static const char* result_name(Result result);

  AmdGfx8(IVcnRegisters& registers, const IClock& clock) : registers_(registers), clock_(clock) {}
  AmdGfx8(IVcnRegisters& registers, const IClock& clock, const Config& config) : registers_(registers), clock_(clock), config_(config) {}

  // gfx_v8_0_hw_init (without constants_init and the parts listed above). The ring is empty and idle afterwards.
  Result start(const Memory& memory, uint32_t timeout_us = kDefaultTimeoutUs);
  // The compute queues to start with the engine (not owned; they must outlive this object, and be attached to their memory first). nullptr: none.
  void attach_compute(AmdGfx8Compute* compute) { compute_ = compute; }
  // gfx_v8_0_hw_fini: unmaps the compute queues, halts the command processors (graphics and compute) and the RLC.
  void stop();

  // gfx_v8_0_ring_test_ring on its own, for later use.
  bool test_ring(uint32_t timeout_us = kDefaultTimeoutUs);
  uint32_t write_pointer() const { return wptr_; }
  // The raster configuration the render back-ends were set up with (what the driver caches in rb_config[0][0]) and the number of back-ends found.
  uint32_t raster_config() const { return raster_config_; }
  uint32_t raster_config_1() const { return raster_config_1_; }
  uint32_t active_render_backends() const { return active_rbs_; }

  // The clear state (gfx_v8_0_get_csb_buffer): `out` receives kClearStateDwords dwords, the count is returned (0 if it does not fit). `with_ce_partitions` appends the CE
  // partition packet that the ring start stream (gfx_v8_0_cp_gfx_start) has after the CLEAR_STATE packet.
  static uint32_t build_clear_state(uint32_t raster_config, uint32_t raster_config_1, ArrayView<uint32_t> out, bool with_ce_partitions = false);
  // gfx_v8_0_raster_config for the Vega M, and the harvested variant (gfx_v8_0_write_harvested_raster_configs picks per shader engine from the enabled back-ends).
  static void default_raster_config(uint32_t* config, uint32_t* config_1);
  // Returns PA_SC_RASTER_CONFIG_1 in `config_1_out` and each shader engine's PA_SC_RASTER_CONFIG in per_se[0..num_se) (num_se is 1, 2 or 4).
  static void harvested_raster_configs(uint32_t config, uint32_t config_1, uint32_t rb_mask, uint32_t num_rb, uint32_t num_se, uint32_t sh_per_se, uint32_t* config_1_out,
                                       ArrayView<uint32_t> per_se);

 private:
  uint32_t rd(uint32_t reg) { return registers_.read(reg); }
  void wr(uint32_t reg, uint32_t value) { registers_.write(reg, value); }
  void delay_us(uint32_t us);

  void program_golden_registers();
  void select_se_sh(uint32_t se, uint32_t sh);
  void srbm_select(uint32_t me, uint32_t pipe, uint32_t queue, uint32_t vmid);
  void tiling_mode_table_init();
  uint32_t rb_active_bitmap();
  void write_harvested_raster_configs(uint32_t raster_config, uint32_t raster_config_1, uint32_t rb_mask, uint32_t num_rb);
  void setup_rb();
  void init_compute_vmid();
  void init_gds_vmid();
  void constants_init();
  void init_csb();
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
  uint32_t raster_config_ = 0, raster_config_1_ = 0, active_rbs_ = 0;
  AmdGfx8Compute* compute_ = nullptr;
};

}  // namespace hardware
