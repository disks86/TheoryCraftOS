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

#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvFb.hpp"

namespace hardware {
namespace nv {

namespace {

// ---- Registers (BAR0 offsets). Written from memory of nouveau's fb/ram*.c, not from NVIDIA documentation; marked where unsure. ----
//   Tesla     0x10020c  size: bits 31:8 the low bytes, bits 7:0 bits 39:32 (nv50_ram_ctor)               confident
//   Tesla     0x100714  bits 2:0 memory type (0 DDR1, 1 DDR2/DDR3, 2 GDDR3, 3 GDDR4, 4 GDDR5)              fairly sure
//   Fermi..   0x022438  number of FBPs (gf100_ram_ctor)                                                    confident
//   Fermi..   0x022554  FBPs disabled by fuse, a bit per FBP (gf100_ram_probe_fbp)                         confident
//   Fermi..   0x11020c + fbpa * 0x1000   MiB in that FBPA (gf100_ram_probe_fbpa_amount)                   confident
//   Maxwell   0x022450  LTCs per FBP, 0x022458 FBPAs per FBP, 0x021d38 disabled FBPs, 0x021c14 disabled
//                       FBPAs (gm200_ram_probe_fbp_amount). Used for GM107 too, which nouveau may size the
//                       gf100 way: unsure for GM107.                                                         fairly sure (GM20x)
//   Pascal..  0x02243c  number of FBPAs, 0x021c14 disabled FBPAs, 0x90020c + fbpa * 0x4000 MiB per FBPA
//                       (gp100_ram_new)                                                                       fairly sure
//   Turing..  0x1183a4  usable VRAM in MiB as the firmware left it (NV_USABLE_FB_SIZE_IN_MB, read by
//                       nouveau's GSP code); cross-checked against the FBPA sum                              fairly sure
//   GF119..   0x625f04  display VGA workspace: bit 3 valid, bits 31:8 the address >> 8 (nv50_disp / gf119)  unsure
constexpr uint32_t kNv50Size = 0x10020c, kNv50Type = 0x100714;
constexpr uint32_t kFbpCount = 0x022438, kFbpDisabled = 0x022554, kFbpaSize = 0x11020c, kFbpaStride = 0x1000;
constexpr uint32_t kGm200Ltcs = 0x022450, kGm200FbpasPerFbp = 0x022458, kGm200FbpDisabled = 0x021d38, kFbpaDisabled = 0x021c14;
constexpr uint32_t kGp100FbpaCount = 0x02243c, kGp100FbpaSize = 0x90020c, kGp100FbpaStride = 0x4000;
constexpr uint32_t kUsableFbMiB = 0x1183a4;
constexpr uint32_t kDispVgaWorkspace = 0x625f04;
//   fb_init_mmu: 0x100c80 bit 0 = 64 KiB big pages (gf100_fb_init_page), GM200 also bits 11/12 (mask 0x1801, 0x1001 for 16)  fairly sure
//                0x100c10 = sysmem flush page >> 8 (gf100_fb_init); Turing: upper bits in 0x100c40                         unsure (0x100c40)
constexpr uint32_t kMmuCtrl = 0x100c80, kSysmemFlush = 0x100c10, kSysmemFlushHi = 0x100c40;

constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kMinVram = 8 * kMiB, kMaxVram = 256ull << 30;   // plausibility bounds: nothing NVIDIA made is outside them
constexpr uint32_t kMaxPartitions = 32;
constexpr uint64_t kVgaReserve = 256 * 1024;                     // nouveau: rsvd_head ("vga memory")
constexpr uint64_t kVbiosReserve = 1 * kMiB;                     // nouveau: rsvd_tail ("vbios etc")

bool all_ones(uint32_t v) { return v == 0xFFFFFFFFu; }

// Adds one partition's size (in MiB) to the probe; false for a value no partition has.
bool add_partition(VramProbe& p, uint32_t mib) {
  if (all_ones(mib) || mib > 64 * 1024) return false;
  if (mib == 0) return true;  // an FBPA with no memory behind it (floorswept but not fused off)
  const uint64_t bytes = uint64_t(mib) * kMiB;
  if (p.info.partitions && bytes != p.info.partition_bytes) p.info.uneven = true;
  if (!p.info.partitions || bytes < p.info.partition_bytes) p.info.partition_bytes = bytes;
  ++p.info.partitions;
  p.info.bytes += bytes;
  return true;
}

FbError probe_nv50(INvRegisters& regs, VramProbe& p) {
  const uint32_t raw = regs.read32(kNv50Size);
  if (all_ones(raw)) return FbError::Garbage;
  p.info.bytes = (uint64_t(raw) & 0xFFFFFF00u) | (uint64_t(raw & 0xFF) << 32);
  p.info.partitions = 1;
  p.info.partition_bytes = p.info.bytes;
  switch (regs.read32(kNv50Type) & 7) {
    case 0: p.info.type = RamType::Ddr1; break;
    case 1: p.info.type = RamType::Ddr2; break;   // or DDR3: nouveau asks the BIOS
    case 2: p.info.type = RamType::Gddr3; break;
    case 3: p.info.type = RamType::Gddr4; break;
    case 4: p.info.type = RamType::Gddr5; break;
    default: break;
  }
  return FbError::None;
}

FbError probe_gf100(INvRegisters& regs, VramProbe& p) {
  const uint32_t fbps = regs.read32(kFbpCount), disabled = regs.read32(kFbpDisabled);
  if (all_ones(fbps) || all_ones(disabled) || fbps == 0 || fbps > kMaxPartitions) return fbps == 0 ? FbError::NoMemory : FbError::Garbage;
  for (uint32_t fbp = 0; fbp < fbps; ++fbp) {
    if (disabled & (1u << fbp)) continue;
    if (!add_partition(p, regs.read32(kFbpaSize + fbp * kFbpaStride))) return FbError::Garbage;
  }
  return FbError::None;
}

FbError probe_gm200(INvRegisters& regs, VramProbe& p) {
  const uint32_t fbps = regs.read32(kFbpCount), fbpas = regs.read32(kGm200FbpasPerFbp);
  const uint32_t fbp_off = regs.read32(kGm200FbpDisabled), fbpa_off = regs.read32(kFbpaDisabled);
  (void)regs.read32(kGm200Ltcs);  // read as nouveau does; the LTC count only matters for compression tags
  if (all_ones(fbps) || all_ones(fbpas) || all_ones(fbp_off) || all_ones(fbpa_off)) return FbError::Garbage;
  if (fbps == 0 || fbpas == 0) return FbError::NoMemory;
  if (fbps > kMaxPartitions || fbpas > 4 || fbps * fbpas > kMaxPartitions) return FbError::Garbage;
  for (uint32_t fbp = 0; fbp < fbps; ++fbp) {
    if (fbp_off & (1u << fbp)) continue;
    for (uint32_t i = 0; i < fbpas; ++i) {
      const uint32_t fbpa = fbp * fbpas + i;
      if (fbpa_off & (1u << fbpa)) continue;
      if (!add_partition(p, regs.read32(kFbpaSize + fbpa * kFbpaStride))) return FbError::Garbage;
    }
  }
  return FbError::None;
}

FbError probe_gp100(INvRegisters& regs, VramProbe& p) {
  const uint32_t fbpas = regs.read32(kGp100FbpaCount), disabled = regs.read32(kFbpaDisabled);
  if (all_ones(fbpas) || all_ones(disabled) || fbpas > kMaxPartitions) return FbError::Garbage;
  if (fbpas == 0) return FbError::NoMemory;
  for (uint32_t fbpa = 0; fbpa < fbpas; ++fbpa) {
    if (disabled & (1u << fbpa)) continue;
    if (!add_partition(p, regs.read32(kGp100FbpaSize + fbpa * kGp100FbpaStride))) return FbError::Garbage;
  }
  return FbError::None;
}

RamType default_type(const Chip& chip) {
  switch (chip.family) {
    case Family::Fermi: case Family::Kepler: case Family::Maxwell: return RamType::Gddr5;
    case Family::Pascal: return chip.chipset == 0x130 ? RamType::Hbm2 : RamType::Gddr5;  // GP100 HBM2; GP102/104 may be GDDR5X
    case Family::Volta: return RamType::Hbm2;
    case Family::Turing: return chip.chipset == 0x167 ? RamType::Gddr5 : RamType::Gddr6;  // TU117 boards come with either
    case Family::Ampere: case Family::Ada: return RamType::Gddr6;
    case Family::Hopper: return RamType::Hbm2;
    default: return RamType::Unknown;
  }
}

}  // namespace

const char* ram_type_name(RamType type) {
  switch (type) {
    case RamType::Ddr1: return "DDR1";
    case RamType::Ddr2: return "DDR2";
    case RamType::Ddr3: return "DDR3";
    case RamType::Gddr3: return "GDDR3";
    case RamType::Gddr4: return "GDDR4";
    case RamType::Gddr5: return "GDDR5";
    case RamType::Gddr5x: return "GDDR5X";
    case RamType::Gddr6: return "GDDR6";
    case RamType::Gddr6x: return "GDDR6X";
    case RamType::Hbm2: return "HBM2";
    default: return "unknown";
  }
}

namespace {
// The memory type, and the regions at the bottom (VGA) and the top (VBIOS, display/VGA workspace) the firmware keeps.
void finish_probe(INvRegisters& regs, const Chip& chip, RamType bios_type, VramProbe& p) {
  if (p.info.type == RamType::Unknown || bios_type != RamType::Unknown) {
    p.info.type = bios_type != RamType::Unknown ? bios_type : default_type(chip);
    p.info.type_guessed = bios_type == RamType::Unknown;
  }

  p.info.reserved_head = kVgaReserve;
  p.info.reserved_tail = kVbiosReserve;
  if (chip.family >= Family::Fermi && chip.chipset >= 0xd9) {
    // GF119 and later: the display's VGA workspace sits near the top; keep everything above it.
    const uint32_t vga = regs.read32(kDispVgaWorkspace);
    if (!all_ones(vga) && (vga & 8)) {
      const uint64_t base = uint64_t(vga & 0xFFFFFF00u) << 8;
      if (base >= p.info.bytes / 2 && base < p.info.bytes && p.info.bytes - base > p.info.reserved_tail)
        p.info.reserved_tail = (p.info.bytes - base + 0xFFFF) & ~uint64_t{0xFFFF};
    }
  }
  p.error = FbError::None;
}
}  // namespace

VramProbe probe_vram_display(INvRegisters& regs, const Chip& chip, RamType bios_type) {
  VramProbe p = probe_vram(regs, chip, bios_type);
  if (p.error != FbError::NeedsGsp || chip.family != Family::Ampere || chip.chipset < 0x172) return p;
  // ga102_ram_new: the size is what the firmware left usable (0x1183a4, MiB); the memory type from the VBIOS.
  if (p.info.bytes < kMinVram || p.info.bytes > kMaxVram) {
    p.error = p.info.bytes == 0 ? FbError::NoMemory : FbError::Garbage;
    p.info = VramInfo{};
    return p;
  }
  p.info.type = RamType::Unknown;
  finish_probe(regs, chip, bios_type, p);
  return p;
}

VramProbe probe_vram(INvRegisters& regs, const Chip& chip, RamType bios_type) {
  VramProbe p;
  FbError error = FbError::Unsupported;
  switch (chip.family) {
    case Family::Tesla: error = probe_nv50(regs, p); break;
    case Family::Fermi: case Family::Kepler: error = probe_gf100(regs, p); break;
    case Family::Maxwell: error = probe_gm200(regs, p); break;
    case Family::Pascal: case Family::Volta: case Family::Turing: error = probe_gp100(regs, p); break;
    case Family::Ampere: case Family::Hopper: case Family::Ada: case Family::Blackwell: {
      const uint32_t mib = regs.read32(kUsableFbMiB);
      if (!all_ones(mib) && mib) p.info.bytes = uint64_t(mib) * kMiB;
      p.info.type = default_type(chip);
      p.info.type_guessed = true;
      p.error = FbError::NeedsGsp;
      return p;
    }
    default: break;
  }
  if (error != FbError::None) {
    p.info = VramInfo{};
    p.error = error;
    return p;
  }
  if (chip.family == Family::Turing) {
    // The firmware's figure is what the VBIOS's devinit left usable; trust the smaller of the two when both look sane.
    const uint32_t mib = regs.read32(kUsableFbMiB);
    if (!all_ones(mib) && mib && uint64_t(mib) * kMiB < p.info.bytes) p.info.bytes = uint64_t(mib) * kMiB;
  }
  if (p.info.bytes == 0) { p.info = VramInfo{}; p.error = FbError::NoMemory; return p; }
  if (p.info.bytes < kMinVram || p.info.bytes > kMaxVram || p.info.bytes % kMiB) { p.info = VramInfo{}; p.error = FbError::Garbage; return p; }
  finish_probe(regs, chip, bios_type, p);
  return p;
}

bool fb_init_mmu(INvRegisters& regs, const Chip& chip, uint32_t big_page_shift, uint64_t sysmem_flush_page) {
  if (chip.family < Family::Fermi || chip.family > Family::Turing) return false;
  if (chip.family <= Family::Maxwell) {
    if (big_page_shift != 16 && big_page_shift != 17) return false;
    const bool gm200 = chip.family == Family::Maxwell && chip.chipset >= 0x120;
    if (gm200) regs.mask(kMmuCtrl, 0x00001801, big_page_shift == 16 ? 0x00001001 : 0);
    else regs.mask(kMmuCtrl, 0x00000001, big_page_shift == 16 ? 1 : 0);
  }
  if (sysmem_flush_page) {
    if (sysmem_flush_page % 4096) return false;
    regs.write32(kSysmemFlush, static_cast<uint32_t>(sysmem_flush_page >> 8));
    if (chip.family == Family::Turing) regs.write32(kSysmemFlushHi, static_cast<uint32_t>(sysmem_flush_page >> 40));
  }
  return true;
}

// ---- NvFb ----

bool NvFb::init(const VramInfo& info) {
  NvLockGuard guard(lock_);
  info_ = info;
  bad_releases_ = 0;
  if (info.usable_bytes() < kMinAlignment) return false;
  return heap_.reset(info.usable_start(), info.usable_bytes());
}

Optional<uint64_t> NvFb::allocate(uint64_t bytes, uint64_t alignment) {
  if (bytes == 0 || bytes > ~uint64_t{0} - (kMinAlignment - 1)) return {};
  if (alignment < kMinAlignment) alignment = kMinAlignment;
  const uint64_t rounded = (bytes + kMinAlignment - 1) & ~(kMinAlignment - 1);
  NvLockGuard guard(lock_);
  return heap_.allocate(rounded, alignment);
}

bool NvFb::release(uint64_t address) {
  NvLockGuard guard(lock_);
  if (heap_.release(address)) return true;
  ++bad_releases_;
  return false;
}

bool NvFb::owns(uint64_t address, uint64_t bytes) {
  NvLockGuard guard(lock_);
  const auto r = heap_.allocation_containing(address);
  return r && bytes <= r->end() - address;
}

uint64_t NvFb::free_bytes() {
  NvLockGuard guard(lock_);
  return heap_.free_bytes();
}

uint32_t NvFb::allocation_count() {
  NvLockGuard guard(lock_);
  return heap_.allocation_count();
}

// ---- NvVramMemory ----

GpuMemory NvVramMemory::allocate(MemoryTarget target, uint64_t bytes, uint64_t alignment) {
  if (target != MemoryTarget::Vram) return system_ ? system_->allocate(target, bytes, alignment) : GpuMemory{};
  if (bytes == 0 || bytes > ~uint64_t{0} - 4095 || (alignment & (alignment - 1)) != 0) return {};
  // Objects that ask for page alignment are rounded to whole pages, so the whole object can be mapped into a VMM.
  const uint64_t granule = alignment >= 4096 ? 4096 : 256;
  const uint64_t rounded = (bytes + granule - 1) & ~(granule - 1);
  const Optional<uint64_t> at = fb_.allocate(rounded, alignment);
  if (!at) return {};
  if (!access_.fill32(*at, rounded, 0)) {
    (void)fb_.release(*at);
    return {};
  }
  GpuMemory m;
  m.gpu_address = *at;
  m.bytes = rounded;
  m.target = MemoryTarget::Vram;
  return m;
}

void NvVramMemory::release(GpuMemory& memory) {
  if (!memory.valid()) return;
  if (memory.target != MemoryTarget::Vram) {
    if (system_) system_->release(memory);
    memory = GpuMemory{};
    return;
  }
  (void)fb_.release(memory.gpu_address);  // a double release is counted by NvFb (bad_releases) and changes nothing
  memory = GpuMemory{};
}

void NvVramMemory::flush(const GpuMemory& memory) {
  if (memory.target != MemoryTarget::Vram) {
    if (system_) system_->flush(memory);
    return;
  }
  (void)access_.flush_writes();
}

bool NvVramMemory::read32(const GpuMemory& memory, uint64_t offset, uint32_t* value) {
  if (memory.target != MemoryTarget::Vram) return system_ ? system_->read32(memory, offset, value) : INvMemory::read32(memory, offset, value);
  if (offset % 4 || offset > memory.bytes || memory.bytes - offset < 4) return false;
  return access_.read32(memory.gpu_address + offset, value);
}

bool NvVramMemory::write32(const GpuMemory& memory, uint64_t offset, uint32_t value) {
  if (memory.target != MemoryTarget::Vram) return system_ ? system_->write32(memory, offset, value) : INvMemory::write32(memory, offset, value);
  if (offset % 4 || offset > memory.bytes || memory.bytes - offset < 4) return false;
  return access_.write32(memory.gpu_address + offset, value);
}

}  // namespace nv
}  // namespace hardware
