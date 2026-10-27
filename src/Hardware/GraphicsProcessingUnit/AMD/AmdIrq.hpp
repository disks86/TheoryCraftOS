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
#include "Hardware/GraphicsProcessingUnit/AMD/AmdIh3.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// What the interrupt vectors of a Radeon RX Vega M mean: the dispatch half of the Linux amdgpu driver's amdgpu_irq_dispatch for the VI generation. The driver registers a handler
// per source id (amdgpu_irq_add_id) in each block's sw_init, and the handler decodes the vector's ring id; this does the same for the sources of the blocks AmdVegaM brings up:
//   gfx_v8_0   181 end of pipe (a fence), 184 privileged register, 185 privileged instruction, 197 ECC error   (ring id: bits 3:2 ME, 1:0 pipe, 6:4 queue)
//   sdma_v3_0  224 trap (a fence), 241 and 247 illegal instruction                                            (ring id: bits 1:0 engine, 3:2 queue)
//   gmc_v8_0   146 page fault, 147 memory protection fault: the fault's address and status are read from the memory controller and the registers cleared
// Anything else is counted and left. The driver's handlers hand fences to the scheduler and faults to its timeout handling; here a fence is an event for the sink (the engines'
// submit functions poll their fence words), and a fault is reported to the sink, which can stop the ring it came from. The caller polls (process): the card's interrupt line is
// not connected to anything (see AmdIh3).
class AmdIrq {
 public:
  enum class Kind : uint8_t {
    Unknown,
    GraphicsFence,              // gfx_v8_0_eop_irq, ME 0
    ComputeFence,               // gfx_v8_0_eop_irq, ME 1 or 2
    GfxPrivilegedRegister,      // gfx_v8_0_priv_reg_irq
    GfxPrivilegedInstruction,   // gfx_v8_0_priv_inst_irq
    GfxEccError,                // gfx_v8_0_cp_ecc_error_irq
    SdmaTrap,                   // sdma_v3_0_process_trap_irq
    SdmaIllegalInstruction,     // sdma_v3_0_process_illegal_inst_irq
    VmFault,                    // gmc_v8_0_process_interrupt
  };
  static constexpr uint32_t kKinds = 9;
  static const char* kind_name(Kind kind);

  // The source ids (ivsrcid_vislands30.h; the SDMA illegal instruction has a bare 241 in sdma_v3_0_sw_init).
  static constexpr uint32_t kSrcCpEndOfPipe = 181, kSrcCpPrivRegFault = 184, kSrcCpPrivInstrFault = 185, kSrcCpEccError = 197;
  static constexpr uint32_t kSrcSdmaTrap = 224, kSrcSdmaIllegalInstruction = 241, kSrcSdmaSrbmWrite = 247;
  static constexpr uint32_t kSrcGfxPageInvFault = 146, kSrcGfxMemProtFault = 147;

  // gmc_v8_0_process_interrupt: VM_CONTEXT1_PROTECTION_FAULT_ADDR / _STATUS / _MCCLIENT, decoded as gmc_v8_0_vm_decode_fault does.
  struct VmFaultInfo {
    bool valid = false;         // false when both registers were zero (nothing to report)
    uint32_t page = 0;          // the register: the faulting address in 4 KiB pages
    uint32_t status = 0;
    uint32_t mc_client = 0;     // four ASCII characters naming the block
    uint32_t vmid = 0, protections = 0, client_id = 0;
    bool write = false;
  };
  struct Event {
    Kind kind = Kind::Unknown;
    uint32_t me = 0, pipe = 0, queue = 0;   // the CP's: ring id decoded for the gfx_v8_0 sources
    uint32_t engine = 0;                    // the SDMA engine (0, 1) for the sdma_v3_0 sources
    AmdIh3::Vector vector;
    VmFaultInfo vm;                         // for VmFault
  };
  // The vector's source and ring id; no registers are read.
  static Event classify(const AmdIh3::Vector& vector);

  // Called for every vector, in order.
  class ISink {
   public:
    virtual void on_event(const Event& event) = 0;

   protected:
    ISink() = default;
    ~ISink() = default;
  };

  explicit AmdIrq(IVcnRegisters& registers) : registers_(registers) {}

  // Reads what is in the IH ring (up to `max_vectors`), classifies each vector, reads and clears the fault registers for VM faults and passes every event to `sink` (may be null).
  // Returns how many vectors were handled.
  uint32_t process(AmdIh3& ih, ISink* sink, uint32_t max_vectors = 64);

  uint32_t count(Kind kind) const { return counts_[static_cast<uint32_t>(kind)]; }
  uint32_t total() const { return total_; }
  uint32_t unknown_source(uint32_t src_id) const { return src_id < 256 ? unknown_[src_id] : 0; }
  const VmFaultInfo& last_vm_fault() const { return last_vm_fault_; }

 private:
  VmFaultInfo read_vm_fault();

  IVcnRegisters& registers_;
  uint32_t counts_[kKinds] = {};
  uint32_t unknown_[256] = {};
  uint32_t total_ = 0;
  VmFaultInfo last_vm_fault_;
};

}  // namespace hardware
