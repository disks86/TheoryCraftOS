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

#include "Hardware/GraphicsProcessingUnit/Intel/IntelGen9Setup.hpp"

namespace hardware {
namespace intel_gt {
namespace gen9 {

namespace {
// MOCS control value fields (LE_*): cacheability in bits 1:0, target cache in 3:2, LRU management in 5:4. L3 value: cacheability in bits 5:4.
constexpr uint32_t kLe0PageTable = 0, kLe1Uncached = 1, kLe3WriteBack = 3;
constexpr uint32_t kTc0PageTable = 0 << 2, kTc1Llc = 1 << 2, kTc2LlcEllc = 2 << 2;
constexpr uint32_t kLruAge3 = 3 << 4;
constexpr uint16_t kL3Uncached = 1 << 4, kL3WriteBack = 3 << 4;

// Registers and bits of the workarounds (gt/intel_gt_regs.h).
constexpr uint32_t kGamEcoChk = 0x4090, kEcoChkDisTlb = 1u << 8, kBdwDisableHdcInvalidation = 1u << 25;
constexpr uint32_t kMmcdMiscCtrl = 0x4DDC, kMmcdPcla = 1u << 31, kMmcdHotspotEn = 1u << 27;
constexpr uint32_t kUcgCtl4 = 0x940C, kEuGauntClockGateDisable = 1u << 14;
constexpr uint32_t kGamtEcoRegRwIa = 0x4AB0, kEnableInPlaceDecompress = 1u << 18;
constexpr uint32_t kGamtChknBit = 0x4AB8, kDisableDynamicCreditSharing = 1u << 28;

constexpr uint32_t kCommonSliceChicken2 = 0x7014, kPbeCompressedHashSelection = 1u << 13, kSbeDisableReplayBufOptimization = 1u << 8;
constexpr uint32_t kHalfSliceChicken7 = 0xE194, kSamplerHashCompressedReadAddr = 1u << 8, kEnableYv12Bugfix = 1u << 4, kEnableGpgpuPreemption = 1u << 2;
constexpr uint32_t kRowChicken = 0xE4F0, kFlowControlEnable = 1u << 15, kPartialInstructionShootdownDisable = 1u << 8;
constexpr uint32_t kCacheMode1 = 0x7004, k4x4StcOptimizationDisable = 1u << 6, kPartialResolveInVcDisable = 1u << 1;
constexpr uint32_t kHalfSliceChicken5 = 0xE188, kCcsTlbPrefetchEnable = 1u << 3;
constexpr uint32_t kHdcChicken0 = 0x7300, kHdcForceContextSaveRestoreNonCoherent = 1u << 5, kHdcForceCsrNonCoherentOvrDisable = 1u << 15, kHdcForceNonCoherent = 1u << 4;
constexpr uint32_t kHalfSliceChicken3 = 0xE184, kSamplerPowerBypassDis = 1u << 1;
constexpr uint32_t kHalfSliceChicken2 = 0xE180, kStPoDisable = 1u << 13;
constexpr uint32_t kCsChicken1 = 0x2580, kPreempt3dObjectLevel = 1u << 0, kPreemptGpgpuLevelMask = (1u << 2) | (1u << 1), kPreemptGpgpuCommandLevel = 1u << 2;
constexpr uint32_t kHalfSliceChicken1 = 0xE100, kSbeSsCacheDispatchPortSharingDisable = 1u << 4;

constexpr uint32_t kKabyLakeStepC0Revision = 2;   // intel_step.c: kbl_revids, revisions 2 and up are C0 or later

class WriteList {
 public:
  WriteList(MaskedWrite* out, uint32_t capacity) : out_(out), capacity_(capacity) {}
  void add(uint32_t reg, uint32_t value) {
    if (count_ < capacity_ && out_) out_[count_] = {reg, value};
    ++count_;
  }
  uint32_t count() const { return count_ < capacity_ ? count_ : capacity_; }
  uint32_t total() const { return count_; }

 private:
  MaskedWrite* out_;
  uint32_t capacity_, count_ = 0;
};

// gen9_ctx_workarounds_init for a part with a last level cache, then the Kaby Lake / Coffee Lake additions.
void build_render_list(const Identity& id, WriteList& list) {
  // WaCompressedResourceSamplerPbeMediaNewHashMode
  list.add(kCommonSliceChicken2, masked_enable(kPbeCompressedHashSelection));
  list.add(kHalfSliceChicken7, masked_enable(kSamplerHashCompressedReadAddr));
  // WaClearFlowControlGpgpuContextSave, WaDisablePartialInstShootdown
  list.add(kRowChicken, masked_enable(kFlowControlEnable | kPartialInstructionShootdownDisable));
  // WaEnableYV12BugFixInHalfSliceChicken7, WaEnableSamplerGPGPUPreemptionSupport
  list.add(kHalfSliceChicken7, masked_enable(kEnableYv12Bugfix | kEnableGpgpuPreemption));
  // Wa4x4STCOptimizationDisable, WaDisablePartialResolveInVc
  list.add(kCacheMode1, masked_enable(k4x4StcOptimizationDisable | kPartialResolveInVcDisable));
  // WaCcsTlbPrefetchDisable
  list.add(kHalfSliceChicken5, masked_disable(kCcsTlbPrefetchEnable));
  // WaForceContextSaveRestoreNonCoherent, WaForceEnableNonCoherent
  list.add(kHdcChicken0, masked_enable(kHdcForceContextSaveRestoreNonCoherent | kHdcForceCsrNonCoherentOvrDisable));
  list.add(kHdcChicken0, masked_enable(kHdcForceNonCoherent));
  // WaDisableSamplerPowerBypassForSOPingPong, WaDisableSTUnitPowerOptimization
  list.add(kHalfSliceChicken3, masked_enable(kSamplerPowerBypassDis));
  list.add(kHalfSliceChicken2, masked_enable(kStPoDisable));
  // WaDisable3DMidCmdPreemption, WaDisableGPGPUMidCmdPreemption
  list.add(kCsChicken1, masked_disable(kPreempt3dObjectLevel));
  list.add(kCsChicken1, (kPreemptGpgpuLevelMask << 16) | kPreemptGpgpuCommandLevel);
  // WaToEnableHwFixForPushConstHWBug: Kaby Lake from stepping C0, every Coffee Lake
  if (id.platform == Platform::CoffeeLake || id.revision >= kKabyLakeStepC0Revision) list.add(kCommonSliceChicken2, masked_enable(kSbeDisableReplayBufOptimization));
  // WaDisableSbeCacheDispatchPortSharing
  list.add(kHalfSliceChicken1, masked_enable(kSbeSsCacheDispatchPortSharingDisable));
}

void set_bits(IRegisterBank& registers, uint32_t reg, uint32_t bits) { registers.write32(reg, registers.read32(reg) | bits); }
}  // namespace

Platform platform_for_device(uint16_t device_id) { return device_id >= 0x5900 && device_id <= 0x593F ? Platform::KabyLake : Platform::CoffeeLake; }

Identity identity_for(uint16_t device_id, uint8_t revision) { return {platform_for_device(device_id), revision}; }

uint32_t mocs_control(uint32_t index) {
  switch (index) {
    case kMocsUncached: return kLe1Uncached | kTc2LlcEllc;
    case kMocsCached: return kLe3WriteBack | kTc2LlcEllc | kLruAge3;
    case kMocsL3Eviction: return kLe3WriteBack | kTc1Llc | kLruAge3;   // all of the L3's evictions
    default: return kLe0PageTable | kTc0PageTable | kLruAge3;           // the page-table entry's, for it and for every undefined entry
  }
}

uint16_t mocs_l3cc(uint32_t index) {
  switch (index) {
    case kMocsUncached: return kL3Uncached;
    case kMocsL3Eviction: return kL3Uncached;
    default: return kL3WriteBack;
  }
}

void program_mocs(IRegisterBank& registers, uint32_t engine_mocs_base) {
  for (uint32_t i = 0; i < kMocsEntries; ++i) registers.write32(engine_mocs_base + i * 4, mocs_control(i));
}

void program_l3cc(IRegisterBank& registers) {
  for (uint32_t i = 0; i < kL3ccRegisters; ++i) registers.write32(kL3ccBase + i * 4, mocs_l3cc(2 * i) | (static_cast<uint32_t>(mocs_l3cc(2 * i + 1)) << 16));
}

void apply_gt_workarounds(IRegisterBank& registers, const Identity& identity) {
  // WaDisableKillLogic (not on Coffee Lake or Comet Lake)
  if (identity.platform == Platform::KabyLake) set_bits(registers, kGamEcoChk, kEcoChkDisTlb);
  // WaCompressedResourceSamplerPbeMediaNewHashMode
  set_bits(registers, kMmcdMiscCtrl, kMmcdPcla | kMmcdHotspotEn);
  // WaDisableHDCInvalidation
  set_bits(registers, kGamEcoChk, kBdwDisableHdcInvalidation);
  // WaDisableDynamicCreditSharing: Kaby Lake before stepping C0
  if (identity.platform == Platform::KabyLake && identity.revision < kKabyLakeStepC0Revision) set_bits(registers, kGamtChknBit, kDisableDynamicCreditSharing);
  // WaDisableGafsUnitClkGating, WaInPlaceDecompressionHang
  set_bits(registers, kUcgCtl4, kEuGauntClockGateDisable);
  set_bits(registers, kGamtEcoRegRwIa, kEnableInPlaceDecompress);
}

void apply_render_workarounds(IRegisterBank& registers, const Identity& identity) {
  MaskedWrite writes[24];
  const uint32_t n = render_workaround_list(identity, writes, 24);
  for (uint32_t i = 0; i < n; ++i) registers.write32(writes[i].reg, writes[i].value);
}

uint32_t render_workaround_list(const Identity& identity, MaskedWrite* out, uint32_t capacity) {
  WriteList list(out, capacity);
  build_render_list(identity, list);
  return list.count();
}

}  // namespace gen9
}  // namespace intel_gt
}  // namespace hardware
