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

// The chip backplane after Linux brcmfmac chip.c; see BrcmfBackplane.hpp.
#include "Hardware/WirelessInterface/Broadcom/BrcmfBackplane.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace brcmf_bp;

BrcmfChipId brcmf_decode_chip_id(uint32_t value) {
  BrcmfChipId id;
  id.chip = value & kCidIdMask;
  id.revision = (value & kCidRevMask) >> kCidRevShift;
  id.package = (value & kCidPkgMask) >> kCidPkgShift;
  id.core_count = (value & kCidCcMask) >> kCidCcShift;
  id.socitype = (value & kCidTypeMask) >> kCidTypeShift;
  return id;
}

const BrcmfCore* BrcmfCoreList::find(uint16_t id, uint32_t index) const {
  for (const BrcmfCore& core : view()) {
    if (core.id != id) continue;
    if (index == 0) return &core;
    --index;
  }
  return nullptr;
}

const char* brcmf_erom_result_name(BrcmfEromResult result) {
  switch (result) {
    case BrcmfEromResult::Ok: return "ok";
    case BrcmfEromResult::Truncated: return "enumeration ROM has no end marker";
    case BrcmfEromResult::BadComponent: return "enumeration ROM component descriptor is malformed";
    case BrcmfEromResult::TooManyCores: return "enumeration ROM lists too many cores";
  }
  return "?";
}

namespace {

// The ROM read position (eromaddr). Reads past the end of what was read from the chip give an end-of-table descriptor and mark the ROM as truncated, so every loop
// of the scan ends there.
struct EromCursor {
  ArrayView<const uint32_t> rom;
  uint64_t position = 0;
  bool ran_out = false;

  // brcmf_chip_dmp_get_desc
  uint32_t next(uint8_t* type) {
    uint32_t value = kDescEot;
    if (position < rom.size()) {
      value = rom[position];
    } else {
      ran_out = true;
    }
    ++position;
    if (type) {
      *type = static_cast<uint8_t>(value & kDescTypeMask);
      if ((*type & ~kDescAddrSizeGt32) == kDescAddress) *type = kDescAddress;
    }
    return value;
  }
  void back() { --position; }
};

enum class RegAddr : uint8_t { Ok, NotAddress, TableEnd };

// brcmf_chip_dmp_get_regaddr
RegAddr get_regaddr(EromCursor& cursor, uint32_t* regbase, uint32_t* wrapbase) {
  *regbase = 0;
  *wrapbase = 0;
  uint8_t desc = 0;
  uint32_t value = cursor.next(&desc);
  uint32_t wraptype;
  if (desc == kDescMasterPort) {
    wraptype = kSlaveTypeMwrap;
  } else if (desc == kDescAddress) {
    cursor.back();
    wraptype = kSlaveTypeSwrap;
  } else {
    cursor.back();
    return RegAddr::NotAddress;
  }
  do {
    // the next address descriptor
    do {
      value = cursor.next(&desc);
      if (desc == kDescEot) {
        cursor.back();
        return RegAddr::TableEnd;
      }
    } while (desc != kDescAddress && desc != kDescComponent);
    // the next component's descriptors: stop
    if (desc == kDescComponent) {
      cursor.back();
      return RegAddr::Ok;
    }
    if (value & kDescAddrSizeGt32) (void)cursor.next(nullptr);    // the upper 32 address bits
    const uint32_t size_type = (value & kSlaveSizeTypeMask) >> kSlaveSizeTypeShift;
    if (size_type == kSlaveSizeDesc) {
      const uint32_t size_desc = cursor.next(nullptr);
      if (size_desc & kDescAddrSizeGt32) (void)cursor.next(nullptr);
    }
    // only 4 KiB and 8 KiB regions are register windows
    if (size_type != kSlaveSize4k && size_type != kSlaveSize8k) continue;
    const uint32_t slave_type = (value & kSlaveTypeMask) >> kSlaveTypeShift;
    if (*regbase == 0 && slave_type == kSlaveTypeSlave) *regbase = value & kSlaveAddrBaseMask;
    if (*wrapbase == 0 && slave_type == wraptype) *wrapbase = value & kSlaveAddrBaseMask;
  } while (*regbase == 0 || *wrapbase == 0);
  return RegAddr::Ok;
}

}  // namespace

BrcmfEromResult brcmf_erom_scan(ArrayView<const uint32_t> rom, BrcmfCoreList& out) {
  out.count = 0;
  EromCursor cursor{rom};
  uint8_t type = 0;
  while (type != kDescEot) {
    uint32_t value = cursor.next(&type);
    if (!(value & kDescValid)) continue;
    if (type == kDescEmpty) continue;
    if (type != kDescComponent) continue;    // need a component descriptor

    const uint16_t id = static_cast<uint16_t>((value & kCompPartNumMask) >> kCompPartNumShift);
    // the second component word must follow
    value = cursor.next(&type);
    if ((value & kDescTypeMask) != kDescComponent) return cursor.ran_out ? BrcmfEromResult::Truncated : BrcmfEromResult::BadComponent;
    const uint32_t master_wrappers = (value & kCompNumMwrapMask) >> kCompNumMwrapShift;
    const uint32_t slave_wrappers = (value & kCompNumSwrapMask) >> kCompNumSwrapShift;
    const uint8_t revision = static_cast<uint8_t>((value & kCompRevisionMask) >> kCompRevisionShift);
    if (master_wrappers + slave_wrappers == 0 && id != kCorePmu && id != kCoreGci) continue;

    uint32_t base = 0, wrap = 0;
    if (get_regaddr(cursor, &base, &wrap) != RegAddr::Ok) continue;
    if (out.count >= BrcmfCoreList::kMaxCores) return BrcmfEromResult::TooManyCores;
    BrcmfCore& core = out.cores[out.count++];
    core.id = id;
    core.revision = revision;
    core.base = base;
    core.wrapper = wrap;
  }
  return cursor.ran_out ? BrcmfEromResult::Truncated : BrcmfEromResult::Ok;
}

BrcmfCpu brcmf_cpu_of(const BrcmfCoreList& cores) {
  if (cores.find(kCoreArmCr4)) return BrcmfCpu::Cr4;
  if (cores.find(kCoreArmCa7)) return BrcmfCpu::Ca7;
  if (cores.find(kCoreArmCm3)) return BrcmfCpu::Cm3;
  return BrcmfCpu::None;
}

bool brcmf_cores_usable(const BrcmfCoreList& cores, uint16_t bus_core) {
  if (!cores.find(kCoreChipCommon)) return false;
  const BrcmfCpu cpu = brcmf_cpu_of(cores);
  if (cpu == BrcmfCpu::None) return false;
  if (cpu == BrcmfCpu::Cm3 && !cores.find(kCoreInternalMem)) return false;     // a CM3 needs SOCRAM
  return bus_core == 0 || cores.find(bus_core) != nullptr;
}

// ---- core control -----------------------------------------------------------------------------------------------------------------------------------------------------------

bool brcmf_core_is_up(IBrcmfBackplane& bp, const BrcmfCore& core, bool* up) {
  uint32_t ioctl = 0, reset = 0;
  if (!bp.read32(core.wrapper + kWrapIoctl, &ioctl) || !bp.read32(core.wrapper + kWrapResetCtl, &reset)) return false;
  *up = (ioctl & (kIoctlFgc | kIoctlClk)) == kIoctlClk && (reset & kResetCtlReset) == 0;
  return true;
}

bool brcmf_core_disable(IBrcmfBackplane& bp, const BrcmfCore& core, uint32_t prereset, uint32_t reset) {
  uint32_t value = 0;
  if (!bp.read32(core.wrapper + kWrapResetCtl, &value)) return false;
  if ((value & kResetCtlReset) == 0) {
    // configure reset, then put the core in reset and wait until it says so (SPINWAIT 300 us)
    if (!bp.write32(core.wrapper + kWrapIoctl, prereset | kIoctlFgc | kIoctlClk) || !bp.read32(core.wrapper + kWrapIoctl, &value)) return false;
    if (!bp.write32(core.wrapper + kWrapResetCtl, kResetCtlReset)) return false;
    delay_us(10);
    for (int i = 0; i < 30; ++i) {
      if (!bp.read32(core.wrapper + kWrapResetCtl, &value)) return false;
      if (value == kResetCtlReset) break;
      delay_us(10);
    }
  }
  // in-reset configuration
  return bp.write32(core.wrapper + kWrapIoctl, reset | kIoctlFgc | kIoctlClk) && bp.read32(core.wrapper + kWrapIoctl, &value);
}

bool brcmf_core_reset(IBrcmfBackplane& bp, const BrcmfCore& core, uint32_t prereset, uint32_t reset, uint32_t postreset) {
  if (!brcmf_core_disable(bp, core, prereset, reset)) return false;
  uint32_t value = 0;
  for (int count = 0;; ++count) {
    if (!bp.read32(core.wrapper + kWrapResetCtl, &value)) return false;
    if ((value & kResetCtlReset) == 0) break;
    if (!bp.write32(core.wrapper + kWrapResetCtl, 0)) return false;
    if (count >= 50) break;
    delay_us(40);
  }
  return bp.write32(core.wrapper + kWrapIoctl, postreset | kIoctlClk) && bp.read32(core.wrapper + kWrapIoctl, &value);
}

// ---- RAM --------------------------------------------------------------------------------------------------------------------------------------------------------------------

uint32_t brcmf_tcm_rambase(uint32_t chip, uint32_t revision) {
  switch (chip) {
    case 0x4345:
    case 0xa9be:    // 43454
      return 0x198000;
    case 0x4335: case 0x4339: case 0x4350: case 0x4354: case 0x4356:
    case 0xaa2f:    // 43567
    case 0xaa31:    // 43569
    case 0xaa32:    // 43570
    case 0x4358:
    case 0xaa52:    // 43602
    case 0x4371:
      return 0x180000;
    case 0xa9c9:    // 43465
    case 0xaa05:    // 43525
    case 0x4365: case 0x4366:
    case 0xaa90:    // 43664
    case 0xaa92:    // 43666
      return 0x200000;
    case 0x4355:
    case 0x4359:
      return revision < 9 ? 0x180000 : 0x160000;
    case 0x4364:
    case 0x4373:
      return 0x160000;
    case 0xaae8:    // 43752
    case 0x4377:
      return 0x170000;
    case 0x4378: return 0x352000;
    case 0x4387: return 0x740000;
    default: break;
  }
  return kBrcmfInvalidRambase;
}

namespace {

bool core_write(IBrcmfBackplane& bp, const BrcmfCore& core, uint32_t offset, uint32_t value) { return bp.write32(core.base + offset, value); }
bool core_read(IBrcmfBackplane& bp, const BrcmfCore& core, uint32_t offset, uint32_t* value) { return bp.read32(core.base + offset, value); }

// brcmf_chip_tcm_ramsize
bool tcm_ramsize(IBrcmfBackplane& bp, const BrcmfCore& cr4, uint32_t* size) {
  uint32_t cap = 0;
  if (!core_read(bp, cr4, kCr4Cap, &cap)) return false;
  const uint32_t banks = (cap & kCr4TcbAnbMask) + ((cap & kCr4TcbBnbMask) >> kCr4TcbBnbShift);
  uint64_t total = 0;
  for (uint32_t index = 0; index < banks; ++index) {
    uint32_t info = 0;
    if (!core_write(bp, cr4, kCr4BankIdx, index) || !core_read(bp, cr4, kCr4BankInfo, &info)) return false;
    uint32_t block = kCr4BszMult;
    if (info & kCr4Blk1kMask) block >>= 3;
    total += uint64_t{(info & kCr4BszMask) + 1} * block;
  }
  if (total > 0xffffffffull) return false;
  *size = static_cast<uint32_t>(total);
  return true;
}

// brcmf_chip_socram_banksize
bool socram_banksize(IBrcmfBackplane& bp, const BrcmfCore& core, uint32_t index, uint32_t* size, bool* retention) {
  const uint32_t bankidx = (kSocramMemtypeRam << kSocramBankIdxMemtypeShift) | index;
  uint32_t info = 0;
  if (!core_write(bp, core, kSocramBankIdx, bankidx) || !core_read(bp, core, kSocramBankInfo, &info)) return false;
  *size = ((info & kSocramBankInfoSzMask) + 1) * kSocramBankInfoSzBase;
  *retention = (info & kSocramBankInfoRetntramMask) != 0;
  return true;
}

bool ensure_up(IBrcmfBackplane& bp, const BrcmfCore& core) {
  bool up = false;
  if (!brcmf_core_is_up(bp, core, &up)) return false;
  return up || brcmf_core_reset(bp, core, 0, 0, 0);
}

// brcmf_chip_sysmem_ramsize
bool sysmem_ramsize(IBrcmfBackplane& bp, const BrcmfCore& sysmem, uint32_t* size) {
  if (!ensure_up(bp, sysmem)) return false;
  uint32_t info = 0;
  if (!core_read(bp, sysmem, kSocramCoreInfo, &info)) return false;
  const uint32_t banks = (info & kSrciSrnbMask) >> kSrciSrnbShift;
  uint64_t total = 0;
  for (uint32_t i = 0; i < banks; ++i) {
    uint32_t bank = 0;
    bool retention = false;
    if (!socram_banksize(bp, sysmem, i, &bank, &retention)) return false;
    total += bank;
  }
  if (total > 0xffffffffull) return false;
  *size = static_cast<uint32_t>(total);
  return true;
}

// brcmf_chip_socram_ramsize
bool socram_ramsize(IBrcmfBackplane& bp, const BrcmfChipId& id, const BrcmfCore& socram, uint32_t* size, uint32_t* srsize) {
  if (!ensure_up(bp, socram)) return false;
  uint32_t info = 0;
  if (!core_read(bp, socram, kSocramCoreInfo, &info)) return false;
  uint64_t total = 0, retained = 0;
  if (socram.revision <= 7 || socram.revision == 12) {
    uint32_t banks = (info & kSrciSrnbMask) >> kSrciSrnbShift;
    const uint32_t banksize = info & kSrciSrbszMask;
    const uint32_t lss = (info & kSrciLssMask) >> kSrciLssShift;
    if (lss != 0 && banks > 0) --banks;
    total = uint64_t{banks} << (banksize + kSrBszBase);
    if (lss != 0) total += uint64_t{1} << ((lss - 1) + kSrBszBase);
  } else {
    // more banks from core revision 23 on
    const uint32_t mask = socram.revision >= 23 ? (kSrciSrnbMask | kSrciSrnbMaskExt) : kSrciSrnbMask;
    const uint32_t banks = (info & mask) >> kSrciSrnbShift;
    for (uint32_t i = 0; i < banks; ++i) {
      uint32_t bank = 0;
      bool retention = false;
      if (!socram_banksize(bp, socram, i, &bank, &retention)) return false;
      total += bank;
      if (retention) retained += bank;
    }
  }
  // hardcoded save/restore sizes
  if (id.chip == 0x4334 && id.revision < 2) retained = 32 * 1024;
  if (id.chip == 0xa9a6 /* 43430 */ || id.chip == 0xa9af /* 43439 */) retained = 64 * 1024;
  if (total > 0xffffffffull) return false;
  *size = static_cast<uint32_t>(total);
  *srsize = static_cast<uint32_t>(retained);
  return true;
}

}  // namespace

BrcmfRamResult brcmf_read_ram_info(IBrcmfBackplane& bp, const BrcmfChipId& id, const BrcmfCoreList& cores, BrcmfRamInfo* out) {
  *out = BrcmfRamInfo();
  if (const BrcmfCore* cr4 = cores.find(kCoreArmCr4)) {
    if (!tcm_ramsize(bp, *cr4, &out->size)) return BrcmfRamResult::BusError;
    out->base = brcmf_tcm_rambase(id.chip, id.revision);
    if (out->base == kBrcmfInvalidRambase) return BrcmfRamResult::UnknownBase;
  } else if (const BrcmfCore* sysmem = cores.find(kCoreSysMem)) {
    if (!sysmem_ramsize(bp, *sysmem, &out->size)) return BrcmfRamResult::BusError;
    out->base = brcmf_tcm_rambase(id.chip, id.revision);
    if (out->base == kBrcmfInvalidRambase) return BrcmfRamResult::UnknownBase;
  } else if (const BrcmfCore* socram = cores.find(kCoreInternalMem)) {
    if (!socram_ramsize(bp, id, *socram, &out->size, &out->save_restore_size)) return BrcmfRamResult::BusError;
    out->base = 0;
  } else {
    return BrcmfRamResult::NoMemoryCore;
  }
  if (out->size == 0) return BrcmfRamResult::ZeroSize;
  if (uint64_t{out->base} + out->size > 0x100000000ull) return BrcmfRamResult::UnknownBase;    // RAM past the 32-bit backplane: a lying bank register
  return BrcmfRamResult::Ok;
}

// ---- the ARM ----------------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

// brcmf_chip_disable_arm
bool disable_arm(IBrcmfBackplane& bp, const BrcmfCore& arm) {
  if (arm.id == kCoreArmCm3) return brcmf_core_disable(bp, arm, 0, 0);
  // CR4, CA7: clear every ioctrl bit but HALT, then hold the CPU halted
  uint32_t value = 0;
  if (!bp.read32(arm.wrapper + kWrapIoctl, &value)) return false;
  value &= kArmCr4IoctlCpuHalt;
  return brcmf_core_reset(bp, arm, value, kArmCr4IoctlCpuHalt, kArmCr4IoctlCpuHalt);
}

}  // namespace

bool brcmf_set_passive(IBrcmfBackplane& bp, const BrcmfChipId& id, const BrcmfCoreList& cores) {
  if (const BrcmfCore* cr4 = cores.find(kCoreArmCr4)) {
    // brcmf_chip_cr4_set_passive: the D11 cores are only disabled, the firmware enables them (releasing them here breaks BCM4387)
    if (!disable_arm(bp, *cr4)) return false;
    for (uint32_t i = 0;; ++i) {
      const BrcmfCore* d11 = cores.find(kCore80211, i);
      if (!d11) break;
      if (!brcmf_core_disable(bp, *d11, kD11IoctlPhyReset | kD11IoctlPhyClockEn, kD11IoctlPhyClockEn)) return false;
    }
    return true;
  }
  if (const BrcmfCore* ca7 = cores.find(kCoreArmCa7)) {
    // brcmf_chip_ca7_set_passive
    if (!disable_arm(bp, *ca7)) return false;
    const BrcmfCore* d11 = cores.find(kCore80211);
    return !d11 || brcmf_core_reset(bp, *d11, kD11IoctlPhyReset | kD11IoctlPhyClockEn, kD11IoctlPhyClockEn, kD11IoctlPhyClockEn);
  }
  if (const BrcmfCore* cm3 = cores.find(kCoreArmCm3)) {
    // brcmf_chip_cm3_set_passive
    if (!disable_arm(bp, *cm3)) return false;
    if (const BrcmfCore* d11 = cores.find(kCore80211))
      if (!brcmf_core_reset(bp, *d11, kD11IoctlPhyReset | kD11IoctlPhyClockEn, kD11IoctlPhyClockEn, kD11IoctlPhyClockEn)) return false;
    const BrcmfCore* socram = cores.find(kCoreInternalMem);
    if (!socram || !brcmf_core_reset(bp, *socram, 0, 0, 0)) return false;
    // 43430 / 43439: no remap of bank 3
    if (id.chip == 0xa9a6 || id.chip == 0xa9af)
      if (!core_write(bp, *socram, kSocramBankIdx, 3) || !core_write(bp, *socram, kSocramBankPda, 0)) return false;
    return true;
  }
  return false;
}

bool brcmf_set_active(IBrcmfBackplane& bp, const BrcmfCoreList& cores, IBrcmfActivate& bus, uint32_t reset_vector) {
  if (const BrcmfCore* cr4 = cores.find(kCoreArmCr4)) {
    // brcmf_chip_cr4_set_active: the vector first, then the ARM out of halt
    return bus.activate(reset_vector) && brcmf_core_reset(bp, *cr4, kArmCr4IoctlCpuHalt, 0, 0);
  }
  if (const BrcmfCore* ca7 = cores.find(kCoreArmCa7)) return bus.activate(reset_vector) && brcmf_core_reset(bp, *ca7, kArmCr4IoctlCpuHalt, 0, 0);
  if (const BrcmfCore* cm3 = cores.find(kCoreArmCm3)) {
    // brcmf_chip_cm3_set_active: SOCRAM must be up; the CM3 boots from address 0 of it
    const BrcmfCore* socram = cores.find(kCoreInternalMem);
    bool up = false;
    if (!socram || !brcmf_core_is_up(bp, *socram, &up) || !up) return false;
    return bus.activate(0) && brcmf_core_reset(bp, *cm3, 0, 0, 0);
  }
  return false;
}

}  // namespace hardware
