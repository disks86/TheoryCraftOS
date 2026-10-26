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

#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dGpu.hpp"

namespace hardware {

using namespace v3d;

namespace {
constexpr uint32_t kHubIdent0Expected = 0x02443356;  // the bytes "V", "3", "D" and the number 2 above them
}

V3dIdentity V3dGpu::identify(const MmioRegion& hub, const MmioRegion& core) {
  V3dIdentity id;
  if (!hub.valid() || !core.valid()) return id;
  if (hub.read32(kHubIdent0) != kHubIdent0Expected) return id;  // not a V3D hub
  const uint32_t hub1 = hub.read32(kHubIdent1);
  const uint32_t hub2 = hub.read32(kHubIdent2);
  const uint32_t core1 = core.read32(kCtlIdent1);
  id.technology_version = get_field(hub1, 3, 0);
  id.revision = get_field(hub1, 7, 4);
  id.cores = get_field(hub1, 11, 8);
  id.with_tfu = (hub1 & kHubIdent1WithTfu) != 0;
  id.with_l3_cache = (hub1 & kHubIdent1WithL3c) != 0;
  id.with_mmu = (hub2 & kHubIdent2WithMmu) != 0;
  id.slices = get_field(core1, 7, 4);
  id.qpus_per_slice = get_field(core1, 11, 8);
  id.texture_units = get_field(core1, 15, 12);
  id.semaphores = get_field(core1, 23, 16);
  id.vpm_kib = get_field(core1, 31, 28);
  id.valid = id.technology_version != 0 && id.cores != 0;
  return id;
}

bool V3dGpu::initialize() {
  identity_ = identify(hub_, core_);
  // The control list packets and cache handling here are those of V3D 4.x; version 4.2 is the Raspberry Pi 4's.
  if (!identity_.valid || identity_.technology_version != 4 || !identity_.with_mmu) return false;
  // As the Linux driver: the interrupts it uses are unmasked (nothing routes them to the GIC here, so they show up in the status registers only, which the job waits
  // on) and the L2T flush always covers the whole cache.
  hub_.write32(kHubIntClr, 0xFFFFFFFF);
  core_.write32(kCtlIntClr, 0xFFFFFFFF);
  hub_.write32(kHubIntMskSet, ~(kHubIntMmuWriteViolation | kHubIntMmuPageInvalid | kHubIntMmuCapExceeded | kHubIntTfuComplete));
  hub_.write32(kHubIntMskClr, kHubIntMmuWriteViolation | kHubIntMmuPageInvalid | kHubIntMmuCapExceeded | kHubIntTfuComplete);
  core_.write32(kCtlIntMskSet, ~(kIntOutOfMemory | kIntFlDone | kIntFrDone | kIntViolation));
  core_.write32(kCtlIntMskClr, kIntOutOfMemory | kIntFlDone | kIntFrDone | kIntViolation);
  core_.write32(kCtlL2TFlSta, 0);
  core_.write32(kCtlL2TFlEnd, 0xFFFFFFFF);
  initialized_ = true;
  return flush_caches();
}

void V3dGpu::shutdown() {
  if (!hub_.valid() || !core_.valid()) return;
  if (mmu_on_) hub_.clear_bits32(kMmuCtl, kMmuCtlEnable);
  mmu_on_ = false;
  hub_.write32(kHubIntMskSet, 0xFFFFFFFF);
  core_.write32(kCtlIntMskSet, 0xFFFFFFFF);
  initialized_ = false;
}

bool V3dGpu::enable_mmu(uint64_t table_bus_address, uint64_t illegal_page_bus_address) {
  const uint64_t page = V3dPageTable::kPageSize;
  if (!initialized_ || (table_bus_address | illegal_page_bus_address) & (page - 1)) return false;
  if ((table_bus_address >> kMmuPageShift) > UINT32_MAX || (illegal_page_bus_address >> kMmuPageShift) > kPtePageMask) return false;
  hub_.write32(kMmuPtPaBase, static_cast<uint32_t>(table_bus_address >> kMmuPageShift));
  hub_.write32(kMmuIllegalAddr, static_cast<uint32_t>(illegal_page_bus_address >> kMmuPageShift) | kMmuIllegalAddrEnable);
  hub_.write32(kMmuCtl, kMmuCtlEnable | kMmuCtlPtInvalidEnable | kMmuCtlPtInvalidAbort | kMmuCtlPtInvalidInt | kMmuCtlWriteViolationAbort |
                            kMmuCtlWriteViolationInt | kMmuCtlCapExceededAbort | kMmuCtlCapExceededInt);
  mmu_on_ = true;
  return flush_mmu();
}

bool V3dGpu::flush_mmu(uint32_t attempts) {
  if (!mmu_on_) return false;
  hub_.set_bits32(kMmuCtl, kMmuCtlTlbClear);
  return hub_.wait32(kMmuCtl, kMmuCtlTlbClearing, 0, attempts);
}

bool V3dGpu::flush_caches(uint32_t attempts) {
  if (!initialized_) return false;
  // From the outside in, as the Linux driver does. (The L2C cache and the GCA flush exist only before V3D 3.3.)
  core_.write32(kCtlL2TCaCtl, kL2TCaCtlFlush | kL2TCaCtlModeFlush);
  core_.write32(kCtlSlCaCtl, kSlCaCtlAll);
  return core_.wait32(kCtlL2TCaCtl, kL2TCaCtlFlush, 0, attempts);
}

V3dJobResult V3dGpu::wait_for_list(uint32_t done_bit, uint32_t attempts, ArrayView<const V3dMemoryBlock> overflow) {
  uint64_t next_overflow = 0;
  for (uint32_t i = 0; i < attempts; ++i) {
    const uint32_t status = core_.read32(kCtlIntSts);
    const uint32_t hub_status = hub_.read32(kHubIntSts);
    const uint32_t errors = core_.read32(kErrStat) & kErrAnyMask;
    const uint32_t hub_errors = hub_status & (kHubIntMmuWriteViolation | kHubIntMmuPageInvalid | kHubIntMmuCapExceeded);
    if (errors || hub_errors) {
      last_error_ = errors;
      last_hub_interrupts_ = hub_status;
      return V3dJobResult::Error;
    }
    if (status & kIntOutOfMemory) {
      // The out-of-memory status is edge signalled: it comes again only after memory was added, and the flush-done bit is held back until then.
      if (next_overflow >= overflow.size()) return V3dJobResult::OutOfMemory;
      const V3dMemoryBlock block = overflow[next_overflow++];
      core_.write32(kPtbBpoa, block.address);
      core_.write32(kPtbBpos, block.bytes);
      core_.write32(kCtlIntClr, kIntOutOfMemory);
      continue;
    }
    if (status & done_bit) return V3dJobResult::Done;
  }
  return V3dJobResult::TimedOut;
}

V3dJobResult V3dGpu::run_binning(const V3dBinJob& job, uint32_t attempts, ArrayView<const V3dMemoryBlock> overflow) {
  if (!initialized_ || job.end < job.start) return V3dJobResult::Error;
  last_error_ = 0;
  last_hub_interrupts_ = 0;
  core_.write32(kCtlIntClr, kIntFlDone | kIntOutOfMemory);
  hub_.write32(kHubIntClr, 0xFFFFFFFF);
  core_.write32(kPtbBpos, 0);  // no overflow block left over from the last job
  flush_caches(attempts);
  if (job.tile_alloc.bytes) {
    core_.write32(kCleCt0Qma, job.tile_alloc.address);
    core_.write32(kCleCt0Qms, job.tile_alloc.bytes);
  }
  if (job.tile_state) core_.write32(kCleCt0Qts, kCleCt0QtsEnable | job.tile_state);
  core_.write32(kCleCt0Qba, job.start);
  core_.write32(kCleCt0Qea, job.end);  // writing the end address starts the list
  return wait_for_list(kIntFlDone, attempts, overflow);
}

V3dJobResult V3dGpu::run_rendering(uint32_t start, uint32_t end, uint32_t attempts) {
  if (!initialized_ || end < start) return V3dJobResult::Error;
  last_error_ = 0;
  last_hub_interrupts_ = 0;
  core_.write32(kCtlIntClr, kIntFrDone);
  hub_.write32(kHubIntClr, 0xFFFFFFFF);
  flush_caches(attempts);
  core_.write32(kCleCt1Qba, start);
  core_.write32(kCleCt1Qea, end);
  return wait_for_list(kIntFrDone, attempts, ArrayView<const V3dMemoryBlock>());
}

}  // namespace hardware
