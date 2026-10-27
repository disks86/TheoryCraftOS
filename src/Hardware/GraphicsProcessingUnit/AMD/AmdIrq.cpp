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

#include "Hardware/GraphicsProcessingUnit/AMD/AmdIrq.hpp"

// Follows the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): gfx_v8_0.c (gfx_v8_0_eop_irq, _fault, _priv_reg_irq, _priv_inst_irq), sdma_v3_0.c
// (sdma_v3_0_process_trap_irq, _process_illegal_inst_irq), gmc_v8_0.c (gmc_v8_0_process_interrupt, _vm_decode_fault) and ivsrcid_vislands30.h. scripts/check_irq_port.py checks the
// numbers below against the kernel's headers.

namespace hardware {

namespace {
// gmc_8_1_d.h
constexpr uint32_t mmVM_CONTEXT1_CNTL2 = 0x50d, mmVM_CONTEXT1_PROTECTION_FAULT_STATUS = 0x537, mmVM_CONTEXT1_PROTECTION_FAULT_MCCLIENT = 0x539, mmVM_CONTEXT1_PROTECTION_FAULT_ADDR = 0x53f;
// gmc_8_1_sh_mask.h
constexpr uint32_t kStatusProtectionsMask = 0xff, kStatusClientIdMask = 0x1ff000, kStatusClientIdShift = 0xc, kStatusRwMask = 0x1000000, kStatusVmidMask = 0x1e000000, kStatusVmidShift = 0x19;
constexpr uint32_t kClearProtectionFaultStatusAddrMask = 0x1;
}  // namespace

const char* AmdIrq::kind_name(Kind kind) {
  switch (kind) {
    case Kind::Unknown: return "unknown";
    case Kind::GraphicsFence: return "graphics fence";
    case Kind::ComputeFence: return "compute fence";
    case Kind::GfxPrivilegedRegister: return "privileged register";
    case Kind::GfxPrivilegedInstruction: return "privileged instruction";
    case Kind::GfxEccError: return "ECC error";
    case Kind::SdmaTrap: return "SDMA trap";
    case Kind::SdmaIllegalInstruction: return "SDMA illegal instruction";
    case Kind::VmFault: return "VM fault";
  }
  return "unknown";
}

AmdIrq::Event AmdIrq::classify(const AmdIh3::Vector& vector) {
  Event e;
  e.vector = vector;
  // The CP's ring id (gfx_v8_0_eop_irq and gfx_v8_0_fault)
  e.me = (vector.ring_id & 0x0c) >> 2;
  e.pipe = (vector.ring_id & 0x03) >> 0;
  e.queue = (vector.ring_id & 0x70) >> 4;
  // The SDMA's (sdma_v3_0_process_trap_irq): the engine in bits 1:0, the queue in 3:2 (only queue 0, the ring, is a fence; the compute queues are "XXX" in the driver)
  const uint32_t sdma_engine = vector.ring_id & 0x3, sdma_queue = (vector.ring_id & 0xc) >> 2;
  switch (vector.src_id) {
    case kSrcCpEndOfPipe:
      if (e.me == 0) e.kind = Kind::GraphicsFence;
      else if (e.me == 1 || e.me == 2) e.kind = Kind::ComputeFence;
      break;
    case kSrcCpPrivRegFault: e.kind = Kind::GfxPrivilegedRegister; break;
    case kSrcCpPrivInstrFault: e.kind = Kind::GfxPrivilegedInstruction; break;
    case kSrcCpEccError: e.kind = Kind::GfxEccError; break;
    case kSrcSdmaTrap:
      e.engine = sdma_engine;
      if (sdma_engine < 2 && sdma_queue == 0) e.kind = Kind::SdmaTrap;
      break;
    case kSrcSdmaIllegalInstruction:
    case kSrcSdmaSrbmWrite:
      e.engine = sdma_engine;
      if (sdma_engine < 2) e.kind = Kind::SdmaIllegalInstruction;
      break;
    case kSrcGfxPageInvFault:
    case kSrcGfxMemProtFault: e.kind = Kind::VmFault; break;
    default: break;
  }
  return e;
}

AmdIrq::VmFaultInfo AmdIrq::read_vm_fault() {
  VmFaultInfo info;
  info.page = registers_.read(mmVM_CONTEXT1_PROTECTION_FAULT_ADDR);
  info.status = registers_.read(mmVM_CONTEXT1_PROTECTION_FAULT_STATUS);
  info.mc_client = registers_.read(mmVM_CONTEXT1_PROTECTION_FAULT_MCCLIENT);
  // reset addr and status: WREG32_P(mmVM_CONTEXT1_CNTL2, 1, ~1)
  registers_.write(mmVM_CONTEXT1_CNTL2, (registers_.read(mmVM_CONTEXT1_CNTL2) & ~kClearProtectionFaultStatusAddrMask) | kClearProtectionFaultStatusAddrMask);
  if (info.page == 0 && info.status == 0) return info;
  info.valid = true;
  info.vmid = (info.status & kStatusVmidMask) >> kStatusVmidShift;
  info.protections = info.status & kStatusProtectionsMask;
  info.client_id = (info.status & kStatusClientIdMask) >> kStatusClientIdShift;
  info.write = (info.status & kStatusRwMask) != 0;
  return info;
}

uint32_t AmdIrq::process(AmdIh3& ih, ISink* sink, uint32_t max_vectors) {
  if (max_vectors == 0 || max_vectors > 256) max_vectors = 256;
  AmdIh3::Vector vectors[256];
  const uint32_t got = ih.drain(ArrayView<AmdIh3::Vector>(vectors, max_vectors));
  for (uint32_t i = 0; i < got; ++i) {
    Event event = classify(vectors[i]);
    if (event.kind == Kind::VmFault) {
      event.vm = read_vm_fault();
      if (!event.vm.valid) continue;   // (gmc_v8_0_process_interrupt returns without a report)
      last_vm_fault_ = event.vm;
    }
    ++total_;
    ++counts_[static_cast<uint32_t>(event.kind)];
    if (event.kind == Kind::Unknown && vectors[i].src_id < 256) ++unknown_[vectors[i].src_id];
    if (sink) sink->on_event(event);
  }
  return got;
}

}  // namespace hardware
