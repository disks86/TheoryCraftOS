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
// (sdma_v3_0_process_trap_irq, _process_illegal_inst_irq, gfx_v8_0_parse_sq_irq, _select_se_sh), gmc_v8_0.c (gmc_v8_0_process_interrupt, _vm_decode_fault) and ivsrcid_vislands30.h. scripts/check_irq_port.py checks the
// numbers below against the kernel's headers.

namespace hardware {

namespace {
// gmc_8_1_d.h
constexpr uint32_t mmVM_CONTEXT1_CNTL2 = 0x50d, mmVM_CONTEXT1_PROTECTION_FAULT_STATUS = 0x537, mmVM_CONTEXT1_PROTECTION_FAULT_MCCLIENT = 0x539, mmVM_CONTEXT1_PROTECTION_FAULT_ADDR = 0x53f;
// gmc_8_1_sh_mask.h
constexpr uint32_t kStatusProtectionsMask = 0xff, kStatusClientIdMask = 0x1ff000, kStatusClientIdShift = 0xc, kStatusRwMask = 0x1000000, kStatusVmidMask = 0x1e000000, kStatusVmidShift = 0x19;
constexpr uint32_t kClearProtectionFaultStatusAddrMask = 0x1;
// vce_3_0_d.h and vce_3_0_sh_mask.h
constexpr uint32_t mmVCE_SYS_INT_STATUS = 0x8541, kVceSysIntTrapInterruptIntMask = 0x8;
// gfx_8_0_d.h
constexpr uint32_t mmGRBM_GFX_INDEX = 0xc200, mmSQ_EDC_INFO = 0x23a3;
// gfx_8_0_sh_mask.h
constexpr uint32_t kGrbmInstanceIndexShift = 0x0, kGrbmShIndexShift = 0x8, kGrbmSeIndexShift = 0x10, kGrbmBroadcastAll = 0x20000000 | 0x40000000 | 0x80000000;   // SH, INSTANCE, SE_BROADCAST_WRITES
constexpr uint32_t kSqEdcSourceMask = 0x1c0, kSqEdcSourceShift = 0x6;
constexpr uint32_t kSqSeIdMask = 0x3000000, kSqSeIdShift = 24, kSqEncodingMask = 0xc000000, kSqEncodingShift = 26;
constexpr uint32_t kSqWaveShIdMask = 0x100, kSqWaveShIdShift = 8, kSqWavePrivMask = 0x200, kSqWaveVmIdMask = 0x3c00, kSqWaveVmIdShift = 10, kSqWaveWaveIdMask = 0x3c000, kSqWaveWaveIdShift = 14;
constexpr uint32_t kSqWaveSimdIdMask = 0xc0000, kSqWaveSimdIdShift = 18, kSqWaveCuIdMask = 0xf00000, kSqWaveCuIdShift = 20;
constexpr uint32_t kSqAutoFlagsMask = 0xff;   // THREAD_TRACE, WLT, THREAD_TRACE_BUF_FULL, REG_TIMESTAMP, CMD_TIMESTAMP, HOST_CMD_OVERFLOW, HOST_REG_OVERFLOW, IMMED_OVERFLOW
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
    case Kind::SqMessage: return "SQ message";
    case Kind::DisplayVblank: return "display vblank";
    case Kind::DisplayVline: return "display vline";
    case Kind::DisplayPageFlip: return "display page flip";
    case Kind::DisplayHotplug: return "display hot plug";
    case Kind::DisplayHpdRx: return "display DisplayPort short pulse";
    case Kind::UvdFence: return "UVD fence";
    case Kind::UvdEncodeFence: return "UVD encode fence";
    case Kind::VceFence: return "VCE fence";

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
    case kSrcSqInterruptMsg: e.kind = Kind::SqMessage; break;
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
  // The video engines' (uvd_v6_0_sw_init registers 124 and 119 + i for the encode rings; vce_v3_0_sw_init 167, whose data names the ring)
  if (vector.src_id == kSrcUvdSystemMessage) {
    e.kind = Kind::UvdFence;
  } else if (vector.src_id == kSrcUvdEncodeFirst || vector.src_id == kSrcUvdEncodeFirst + 1) {
    e.ring = vector.src_id - kSrcUvdEncodeFirst;
    e.kind = Kind::UvdEncodeFence;
  } else if (vector.src_id == kSrcVceTrap && vector.src_data < 3) {
    e.ring = vector.src_data;
    e.kind = Kind::VceFence;
  }
  // The display engine's (dce_v11_0_sw_init registers 1..6 for the controllers, the even ids 8..18 for page flips and 42 for the hot plug pins)
  if (vector.src_id >= kSrcDisplayCrtcFirst && vector.src_id < kSrcDisplayCrtcFirst + amd_dce11::kMaxCrtcs) {
    e.crtc = vector.src_id - kSrcDisplayCrtcFirst;
    if (vector.src_data == 0) e.kind = Kind::DisplayVblank;
    else if (vector.src_data == 1) e.kind = Kind::DisplayVline;
  } else if (vector.src_id >= kSrcPageFlipFirst && vector.src_id <= kSrcPageFlipLast && ((vector.src_id - kSrcPageFlipFirst) & 1) == 0) {
    e.crtc = (vector.src_id - kSrcPageFlipFirst) >> 1;
    e.kind = Kind::DisplayPageFlip;
  } else if (vector.src_id == kSrcHotplugDetect && vector.src_data < amd_dce11::Interrupts::kMaxHpd) {
    e.pin = vector.src_data;
    e.kind = Kind::DisplayHotplug;
  } else if (vector.src_id == kSrcHotplugDetect && vector.src_data >= kDataHpdRxFirst && vector.src_data < kDataHpdRxFirst + amd_dce11::Interrupts::kMaxHpd) {
    e.pin = vector.src_data - kDataHpdRxFirst;
    e.kind = Kind::DisplayHpdRx;
  }
  return e;
}

const char* AmdIrq::sq_edc_source_name(uint32_t source) {
  static const char* const kNames[] = {"invalid (no EDC error has occurred)", "instruction fetch", "SGPR or SQC data return", "VGPR", "LDS", "GDS", "TA"};
  return source < sizeof(kNames) / sizeof(kNames[0]) ? kNames[source] : "invalid";
}

AmdIrq::SqInfo AmdIrq::read_sq(uint32_t word) {
  SqInfo info;
  info.encoding = (word & kSqEncodingMask) >> kSqEncodingShift;
  info.se_id = (word & kSqSeIdMask) >> kSqSeIdShift;
  if (info.encoding == 0) {
    info.general = word & kSqAutoFlagsMask;
    return info;
  }
  if (info.encoding > 2) return info;   // ("SQ invalid encoding type")
  info.sh_id = (word & kSqWaveShIdMask) >> kSqWaveShIdShift;
  info.cu_id = (word & kSqWaveCuIdMask) >> kSqWaveCuIdShift;
  info.simd_id = (word & kSqWaveSimdIdMask) >> kSqWaveSimdIdShift;
  info.wave_id = (word & kSqWaveWaveIdMask) >> kSqWaveWaveIdShift;
  info.vm_id = (word & kSqWaveVmIdMask) >> kSqWaveVmIdShift;
  info.priv = (word & kSqWavePrivMask) != 0;
  // gfx_v8_0_select_se_sh(se_id, sh_id, cu_id), the read of SQ_EDC_INFO, and back to broadcasting to everything
  registers_.write(mmGRBM_GFX_INDEX, (info.cu_id << kGrbmInstanceIndexShift) | (info.sh_id << kGrbmShIndexShift) | (info.se_id << kGrbmSeIndexShift));
  info.edc_source = (registers_.read(mmSQ_EDC_INFO) & kSqEdcSourceMask) >> kSqEdcSourceShift;
  info.edc_valid = true;
  registers_.write(mmGRBM_GFX_INDEX, kGrbmBroadcastAll);
  return info;
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
    switch (event.kind) {
      case Kind::UvdEncodeFence: ++uvd_encode_[event.ring]; break;
      case Kind::VceFence:   // WREG32_FIELD(VCE_SYS_INT_STATUS, VCE_SYS_INT_TRAP_INTERRUPT_INT, 1): acknowledged before the ring is looked at
        registers_.write(mmVCE_SYS_INT_STATUS, registers_.read(mmVCE_SYS_INT_STATUS) | kVceSysIntTrapInterruptIntMask);
        ++vce_[event.ring];
        break;
      case Kind::DisplayVblank:   // (the driver acknowledges what the status says is pending, and counts the vblank either way)
        event.asserted = display_.vblank_asserted(event.crtc);
        if (event.asserted) display_.ack_vblank(event.crtc);
        ++vblanks_[event.crtc];
        break;
      case Kind::DisplayVline:
        event.asserted = display_.vline_asserted(event.crtc);
        if (event.asserted) display_.ack_vline(event.crtc);
        ++vlines_[event.crtc];
        break;
      case Kind::DisplayPageFlip:
        display_.ack_pageflip(event.crtc);
        ++pageflips_[event.crtc];
        break;
      case Kind::DisplayHotplug:
        if (!display_.hpd_asserted(event.pin)) continue;   // (dce_v11_0_hpd_irq does nothing without the pin's bit)
        display_.ack_hpd(event.pin);
        display_.hpd_set_polarity(event.pin);   // the next change is the other way (the driver's hotplug work does this when it detects the connector again)
        event.connected = display_.hpd_sense(event.pin);
        connected_ = event.connected ? (connected_ | 1u << event.pin) : (connected_ & ~(1u << event.pin));
        if (++hotplugs_[event.pin] > kHotplugStormLimit && !(throttled_ >> event.pin & 1)) {
          display_.hpd_set_interrupt(event.pin, false);
          throttled_ |= 1u << event.pin;
        }
        break;
      case Kind::DisplayHpdRx:
        if (!display_.hpd_rx_asserted(event.pin)) continue;   // (nothing pending: not a pulse)
        display_.ack_hpd_rx(event.pin);
        if (++hpd_rxs_[event.pin] > kHpdRxStormLimit && !(rx_throttled_ >> event.pin & 1)) {
          display_.hpd_rx_set_interrupt(event.pin, false);
          rx_throttled_ |= 1u << event.pin;
        }
        break;
      default: break;
    }
    if (event.kind == Kind::SqMessage) last_sq_ = event.sq = read_sq(vectors[i].src_data);
    ++total_;
    ++counts_[static_cast<uint32_t>(event.kind)];
    if (event.kind == Kind::Unknown && vectors[i].src_id < 256) ++unknown_[vectors[i].src_id];
    if (sink) sink->on_event(event);
  }
  return got;
}

}  // namespace hardware
