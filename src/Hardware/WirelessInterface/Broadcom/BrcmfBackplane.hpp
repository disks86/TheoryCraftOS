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

// The Broadcom chip backplane, after Linux brcmfmac chip.c (and bcma's scan.c, whose constants are the same): every Broadcom Wi-Fi chip is a small SoC whose cores
// (chipcommon, the ARM, the 802.11 MAC, RAM, the PCIe or SDIO device core) sit on one 32-bit address space. The host reaches it through a window (PCIe BAR0, SDIO
// function 1), which the transports implement as IBrcmfBackplane.
//
//   - chipcommon's first register, ChipID at 0x18000000, gives the chip number, revision, package, core count and the interconnect: the old Sonics backplane (SB, only
//     BCM4329 among the FullMAC chips) or AXI ("AI"), whose cores are listed in an enumeration ROM (EROM) that chipcommon's eromptr register points at.
//   - The EROM is a list of 32-bit descriptors: per core two component words (CIA: designer/part/class, CIB: revision and port and wrapper counts), master port
//     descriptors, and address descriptors (optionally with an upper 32-bit word, optionally followed by a size descriptor) for the core's registers and its AXI
//     wrappers; it ends with 0x0000000F. brcmf_chip_dmp_erom_scan reads it from the chip; here it is read into memory first and parsed with every access checked,
//     because a wrong window, a half-powered chip or a model can return anything.
//   - Each core's wrapper (the "DMP" agent, 4 KiB above its registers usually) holds ioctrl at 0x408 and resetctrl at 0x800: a core is up when its clock is on, the
//     force-gated-clock bit off and reset released. brcmf_chip_ai_coredisable / brcmf_chip_ai_resetcore are written out below.
//   - The RAM is the ARM CR4's tightly coupled memory (its size from the CR4's bank registers, its base address a per-chip constant), a SYS_MEM core (CA7 chips),
//     or the SOCRAM core (CM3 chips, at address 0).
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

// The chip's address space as a transport reaches it. Addresses are backplane addresses; reads and writes are 32 bits wide and 4-aligned except in the block calls.
class IBrcmfBackplane {
 public:
  [[nodiscard]] virtual bool read32(uint32_t address, uint32_t* value) = 0;
  [[nodiscard]] virtual bool write32(uint32_t address, uint32_t value) = 0;
  // RAM transfers (firmware download, shared structures). The address need not be aligned; transports split at their window boundaries.
  [[nodiscard]] virtual bool write_block(uint32_t address, ConstByteView data) = 0;
  [[nodiscard]] virtual bool read_block(uint32_t address, ByteView data) = 0;

 protected:
  constexpr IBrcmfBackplane() = default;
  ~IBrcmfBackplane() = default;
};

namespace brcmf_bp {
constexpr uint32_t kEnumBase = 0x18000000;          // SI_ENUM_BASE_DEFAULT: chipcommon's registers
constexpr uint32_t kCcChipId = 0x00;                // chipcregs.chipid
constexpr uint32_t kCcWatchdog = 0x80;              // chipcregs.watchdog
constexpr uint32_t kCcEromPtr = 0xfc;               // chipcregs.eromptr
constexpr uint32_t kCoreSize = 0x1000;              // BCMA_CORE_SIZE

// ChipID register (CID_*)
constexpr uint32_t kCidIdMask = 0x0000ffff;
constexpr uint32_t kCidRevMask = 0x000f0000, kCidRevShift = 16;
constexpr uint32_t kCidPkgMask = 0x00f00000, kCidPkgShift = 20;
constexpr uint32_t kCidCcMask = 0x0f000000, kCidCcShift = 24;
constexpr uint32_t kCidTypeMask = 0xf0000000, kCidTypeShift = 28;
constexpr uint32_t kSociSb = 0, kSociAi = 1;

// Core IDs (bcma.h BCMA_CORE_*)
constexpr uint16_t kCoreChipCommon = 0x800, kCoreInternalMem = 0x80e, kCore80211 = 0x812, kCorePcie = 0x820, kCorePmu = 0x827, kCoreSdioDev = 0x829,
                   kCoreArmCm3 = 0x82a, kCorePcie2 = 0x83c, kCoreArmCr4 = 0x83e, kCoreGci = 0x840, kCoreArmCa7 = 0x847, kCoreSysMem = 0x849;

// EROM descriptors (chip.c DMP_*)
constexpr uint32_t kDescTypeMask = 0x0000000f;
constexpr uint32_t kDescEmpty = 0x0, kDescValid = 0x1, kDescComponent = 0x1, kDescMasterPort = 0x3, kDescAddress = 0x5, kDescAddrSizeGt32 = 0x8, kDescEot = 0xf;
constexpr uint32_t kCompDesignerMask = 0xfff00000, kCompDesignerShift = 20;
constexpr uint32_t kCompPartNumMask = 0x000fff00, kCompPartNumShift = 8;
constexpr uint32_t kCompClassMask = 0x000000f0, kCompClassShift = 4;
constexpr uint32_t kCompRevisionMask = 0xff000000, kCompRevisionShift = 24;
constexpr uint32_t kCompNumSwrapMask = 0x00f80000, kCompNumSwrapShift = 19;
constexpr uint32_t kCompNumMwrapMask = 0x0007c000, kCompNumMwrapShift = 14;
constexpr uint32_t kCompNumSportMask = 0x00003e00, kCompNumSportShift = 9;
constexpr uint32_t kCompNumMportMask = 0x000001f0, kCompNumMportShift = 4;
constexpr uint32_t kSlaveAddrBaseMask = 0xfffff000;
constexpr uint32_t kSlavePortNumMask = 0x00000f00, kSlavePortNumShift = 8;
constexpr uint32_t kSlaveTypeMask = 0x000000c0, kSlaveTypeShift = 6;
constexpr uint32_t kSlaveTypeSlave = 0, kSlaveTypeBridge = 1, kSlaveTypeSwrap = 2, kSlaveTypeMwrap = 3;
constexpr uint32_t kSlaveSizeTypeMask = 0x00000030, kSlaveSizeTypeShift = 4;
constexpr uint32_t kSlaveSize4k = 0, kSlaveSize8k = 1, kSlaveSize16k = 2, kSlaveSizeDesc = 3;
// How much of the ROM is read: the EROM sits in one 4 KiB core window (bcma maps BCMA_CORE_SIZE for it).
constexpr uint32_t kEromBytes = 0x1000;

// AXI wrapper (bcma_regs.h)
constexpr uint32_t kWrapIoctl = 0x408, kWrapIostatus = 0x500, kWrapResetCtl = 0x800, kWrapResetStatus = 0x804;
constexpr uint32_t kIoctlClk = 0x0001, kIoctlFgc = 0x0002;
constexpr uint32_t kResetCtlReset = 0x0001;
// Core-specific ioctrl bits
constexpr uint32_t kArmCr4IoctlCpuHalt = 0x0020;               // ARMCR4_BCMA_IOCTL_CPUHALT
constexpr uint32_t kD11IoctlPhyClockEn = 0x0004, kD11IoctlPhyReset = 0x0008;

// ARM CR4 core registers (chip.c ARMCR4_*)
constexpr uint32_t kCr4Cap = 0x04, kCr4BankIdx = 0x40, kCr4BankInfo = 0x44, kCr4BankPda = 0x4c;
constexpr uint32_t kCr4TcbAnbMask = 0x0f, kCr4TcbBnbMask = 0xf0, kCr4TcbBnbShift = 4;
constexpr uint32_t kCr4BszMask = 0x7f, kCr4BszMult = 8192, kCr4Blk1kMask = 0x200;   // (BSZ_MASK was 0x3f in older kernels; 0x7f is current)

// SOCRAM / SYSMEM core registers (sbsocramregs; bankinfo/bankpda offsets after Linux's struct layout, unverified on hardware)
constexpr uint32_t kSocramCoreInfo = 0x00, kSocramBankIdx = 0x10, kSocramBankInfo = 0x40, kSocramBankPda = 0x44;
constexpr uint32_t kSrciLssMask = 0x00f00000, kSrciLssShift = 20, kSrciSrnbMask = 0xf0, kSrciSrnbMaskExt = 0x100, kSrciSrnbShift = 4, kSrciSrbszMask = 0xf;
constexpr uint32_t kSrBszBase = 14;
constexpr uint32_t kSocramMemtypeRam = 0, kSocramBankIdxMemtypeShift = 8;
constexpr uint32_t kSocramBankInfoSzMask = 0x7f, kSocramBankInfoSzBase = 8192, kSocramBankInfoRetntramMask = 0x00010000;
}  // namespace brcmf_bp

// The ChipID register, decoded.
struct BrcmfChipId {
  uint32_t chip = 0;          // 0x4356, 0x4345, 0xaa52 (43602), 0xa9a6 (43430) ...
  uint32_t revision = 0;
  uint32_t package = 0;
  uint32_t core_count = 0;    // (chipcommon rev >= 4)
  uint32_t socitype = 0;      // brcmf_bp::kSociSb or kSociAi
};
BrcmfChipId brcmf_decode_chip_id(uint32_t value);

struct BrcmfCore {
  uint16_t id = 0;           // BCMA_CORE_*
  uint8_t revision = 0;
  uint32_t base = 0;         // its registers
  uint32_t wrapper = 0;      // its AXI wrapper
};

// The cores found, in ROM order (brcmf_chip_add_core appends).
struct BrcmfCoreList {
  static constexpr uint32_t kMaxCores = 32;    // real chips have 8 to 20; more is a broken ROM
  BrcmfCore cores[kMaxCores];
  uint32_t count = 0;

  ArrayView<const BrcmfCore> view() const { return ArrayView<const BrcmfCore>(cores).first(count); }
  // The n-th core with this ID, or nullptr.
  const BrcmfCore* find(uint16_t id, uint32_t index = 0) const;
};

enum class BrcmfEromResult : uint8_t {
  Ok,
  Truncated,          // the ROM ended (in the window read) before the end-of-table descriptor
  BadComponent,       // a component descriptor (CIA) not followed by a second one (CIB): Linux gives up (-EFAULT)
  TooManyCores,
};
const char* brcmf_erom_result_name(BrcmfEromResult result);

// brcmf_chip_dmp_erom_scan over the ROM's words (in the chip's order, already converted from little endian). Cores without wrappers are skipped except PMU and GCI, cores
// whose address descriptors cannot be found are skipped, as in Linux. `out` holds the cores found so far even on failure.
[[nodiscard]] BrcmfEromResult brcmf_erom_scan(ArrayView<const uint32_t> rom, BrcmfCoreList& out);

// What brcmf_chip_cores_check wants: chipcommon, an ARM (CM3 needs SOCRAM next to it; CR4 and CA7 bring their own memory or SYS_MEM) and the bus's core.
enum class BrcmfCpu : uint8_t { None, Cm3, Cr4, Ca7 };
BrcmfCpu brcmf_cpu_of(const BrcmfCoreList& cores);
[[nodiscard]] bool brcmf_cores_usable(const BrcmfCoreList& cores, uint16_t bus_core);

// ---- core control through the wrapper ---------------------------------------------------------------------------------------------------------------------------------------

[[nodiscard]] bool brcmf_core_is_up(IBrcmfBackplane& bp, const BrcmfCore& core, bool* up);
// brcmf_chip_ai_coredisable
[[nodiscard]] bool brcmf_core_disable(IBrcmfBackplane& bp, const BrcmfCore& core, uint32_t prereset, uint32_t reset);
// brcmf_chip_ai_resetcore (without the special case for the second D11 core of RSDB chips such as 4359, which Linux releases together with the first)
[[nodiscard]] bool brcmf_core_reset(IBrcmfBackplane& bp, const BrcmfCore& core, uint32_t prereset, uint32_t reset, uint32_t postreset);

// ---- RAM --------------------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kBrcmfInvalidRambase = 0xffffffff;
// brcmf_chip_tcm_rambase: where the CR4/CA7 chips' RAM starts on the backplane (CM3 chips: 0). kBrcmfInvalidRambase for a chip without an entry.
uint32_t brcmf_tcm_rambase(uint32_t chip, uint32_t revision);

struct BrcmfRamInfo {
  uint32_t base = 0;
  uint32_t size = 0;
  uint32_t save_restore_size = 0;     // SOCRAM chips: the retention part (srsize)
};
enum class BrcmfRamResult : uint8_t { Ok, BusError, NoMemoryCore, UnknownBase, ZeroSize };
// brcmf_chip_get_raminfo: CR4 → its TCM banks, else SYS_MEM, else SOCRAM (reset first when down, as Linux does).
[[nodiscard]] BrcmfRamResult brcmf_read_ram_info(IBrcmfBackplane& bp, const BrcmfChipId& id, const BrcmfCoreList& cores, BrcmfRamInfo* out);

// ---- the ARM: halted for the download (brcmf_chip_set_passive) and released (brcmf_chip_set_active) --------------------------------------------------------------------------

[[nodiscard]] bool brcmf_set_passive(IBrcmfBackplane& bp, const BrcmfChipId& id, const BrcmfCoreList& cores);
// `activate` is the bus's step before the ARM runs (PCIe: the reset vector into TCM word 0; SDIO: clear interrupts, the vector if nonzero), called with the vector the
// ARM type gets (CM3: 0, as Linux passes).
class IBrcmfActivate {
 public:
  [[nodiscard]] virtual bool activate(uint32_t reset_vector) = 0;

 protected:
  constexpr IBrcmfActivate() = default;
  ~IBrcmfActivate() = default;
};
[[nodiscard]] bool brcmf_set_active(IBrcmfBackplane& bp, const BrcmfCoreList& cores, IBrcmfActivate& bus, uint32_t reset_vector);

}  // namespace hardware
