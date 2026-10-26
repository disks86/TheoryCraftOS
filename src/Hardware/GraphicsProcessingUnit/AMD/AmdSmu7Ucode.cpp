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


// Follows the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): smu7_smumgr.c and amdgpu_cgs.c.
#include "Hardware/GraphicsProcessingUnit/AMD/AmdSmu7Ucode.hpp"
#include "Hardware/VideoCodec/AMD/AmdGmc8Memory.hpp"

namespace hardware {

namespace {
constexpr uint32_t mmSMC_IND_INDEX_11 = 0x1AC, mmSMC_IND_DATA_11 = 0x1AD;   // smu_7_1_3_d.h

constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// The bytes of a file the driver copies for one table entry, and what the entry says (amdgpu_ucode_init_single_fw, amdgpu_cgs_get_firmware_info).
void set_slot(AmdSmu7Ucode::Layout::Slot& slot, uint32_t id, const AmdGfx8Image& image, ConstByteView source, uint32_t table_bytes, uint16_t flags) {
  slot.id = id;
  slot.source = source;
  slot.image_bytes = static_cast<uint32_t>(source.size());
  slot.table_bytes = table_bytes;
  slot.version = image.version;
  slot.flags = flags;
}

void put_le32(const MmioRegion& block, uint64_t offset, uint32_t value) { block.write32(offset, value); }
}  // namespace

const char* AmdSmu7Ucode::result_name(Result result) {
  switch (result) {
    case Result::Ok: return "ok";
    case Result::BadArgument: return "bad argument";
    case Result::NotAnswering: return "SMC not answering";
    case Result::NoSoftRegisters: return "no soft registers";
    case Result::Refused: return "SMC refused";
    case Result::TimedOut: return "ucode load timed out";
  }
  return "?";
}

AmdSmu7Ucode::Layout AmdSmu7Ucode::plan(const Images& images) {
  Layout layout;
  const AmdGfx8Image& mec = images.mec;
  if (!images.rlc.valid || !images.ce.valid || !images.pfp.valid || !images.me.valid || !mec.valid || !images.sdma0.valid || !images.sdma1.valid) return layout;
  if (mec.jump_table.empty()) return layout;
  // The order of smu7_request_smu_load_fw. The MEC's slot is the ucode without its jump table (AMDGPU_UCODE_ID_CP_MEC1: ucode_size_bytes - jt_size * 4), the table says
  // jt_offset * 4 bytes (amdgpu_cgs_get_firmware_info); the jump tables are slots of their own.
  const uint32_t mec_bytes = static_cast<uint32_t>(mec.ucode.size() - mec.jump_table.size());
  set_slot(layout.slot[0], kIdRlcG, images.rlc, images.rlc.ucode, static_cast<uint32_t>(images.rlc.ucode.size()), 1);
  set_slot(layout.slot[1], kIdCpCe, images.ce, images.ce.ucode, static_cast<uint32_t>(images.ce.ucode.size()), 0);
  set_slot(layout.slot[2], kIdCpPfp, images.pfp, images.pfp.ucode, static_cast<uint32_t>(images.pfp.ucode.size()), 0);
  set_slot(layout.slot[3], kIdCpMe, images.me, images.me.ucode, static_cast<uint32_t>(images.me.ucode.size()), 0);
  set_slot(layout.slot[4], kIdCpMec, mec, mec.ucode.first(mec_bytes), mec.jump_table_offset_bytes, 1);
  set_slot(layout.slot[5], kIdCpMecJt1, mec, mec.jump_table, static_cast<uint32_t>(mec.jump_table.size()), 0);
  layout.entries = 6;
  if (images.mec2.valid && !images.mec2.jump_table.empty()) {
    set_slot(layout.slot[layout.entries++], kIdCpMecJt2, images.mec2, images.mec2.jump_table, static_cast<uint32_t>(images.mec2.jump_table.size()), 0);
  }
  set_slot(layout.slot[layout.entries++], kIdSdma0, images.sdma0, images.sdma0.ucode, static_cast<uint32_t>(images.sdma0.ucode.size()), 0);
  set_slot(layout.slot[layout.entries++], kIdSdma1, images.sdma1, images.sdma1.ucode, static_cast<uint32_t>(images.sdma1.ucode.size()), 0);
  // A table entry says how many bytes of the slot to use: never more than the slot holds.
  uint64_t offset = kTocAreaBytes + kScratchBytes;
  layout.toc_offset = 0;
  layout.scratch_offset = kTocAreaBytes;
  for (uint32_t i = 0; i < layout.entries; ++i) {
    Layout::Slot& slot = layout.slot[i];
    if (slot.table_bytes > slot.image_bytes) return layout;
    slot.offset = offset;
    offset += align_up(slot.image_bytes, kPageSize);
  }
  layout.total_bytes = offset;
  layout.ok = true;
  return layout;
}

AmdSmu7Ucode::Result AmdSmu7Ucode::load(const Images& images, const Memory& memory, uint32_t* loaded, uint32_t timeout_us) {
  if (loaded) *loaded = 0;
  const Layout layout = plan(images);
  if (!layout.ok || (memory.gpu_address & (kPageSize - 1)) != 0 || !memory.block.valid() || memory.block.length() < layout.total_bytes ||
      (memory.block.address() & (kPageSize - 1)) != 0)
    return Result::BadArgument;

  // vegam_process_firmware_header (run when the SMU is started): where the soft registers are.
  uint32_t soft_registers = 0;
  if (!smu_.read_sram_dword(AmdSmu7::kFirmwareHeaderLocation + kHeaderSoftRegistersOffset, &soft_registers)) return Result::NoSoftRegisters;
  if (soft_registers == 0 || soft_registers == 0xFFFFFFFFu || (soft_registers & 3) != 0 || soft_registers > AmdSmu7::kSmcRamEnd - kUcodeLoadStatusOffset - 4) return Result::NoSoftRegisters;
  const uint32_t status_address = soft_registers + kUcodeLoadStatusOffset;

  // The images and the table go into video memory first (amdgpu_ucode_init_bo, then the table's memcpy_toio).
  for (uint32_t i = 0; i < layout.entries; ++i) {
    const Layout::Slot& slot = layout.slot[i];
    for (uint64_t o = 0; o < slot.image_bytes; o += 4) {
      const uint32_t word = static_cast<uint32_t>(slot.source[o]) | static_cast<uint32_t>(slot.source[o + 1]) << 8 | static_cast<uint32_t>(slot.source[o + 2]) << 16 |
                            static_cast<uint32_t>(slot.source[o + 3]) << 24;
      put_le32(memory.block, slot.offset + o, word);
    }
  }
  // struct SMU_DRAMData_TOC: structure_version (1), num_entries, then the entries; struct SMU_Entry: id and version (16 bits each), image address (high, low), meta data
  // address (both 0), data_size_byte, flags and num_register_entries (16 bits each).
  put_le32(memory.block, layout.toc_offset + 0, 1);
  put_le32(memory.block, layout.toc_offset + 4, layout.entries);
  for (uint32_t i = 0; i < kMaxEntries; ++i) {
    const uint64_t at = layout.toc_offset + 8 + uint64_t{i} * kEntryBytes;
    if (i >= layout.entries) {
      for (uint32_t w = 0; w < kEntryBytes; w += 4) put_le32(memory.block, at + w, 0);
      continue;
    }
    const Layout::Slot& slot = layout.slot[i];
    const uint64_t address = memory.gpu_address + slot.offset;
    put_le32(memory.block, at + 0, (slot.id & 0xFFFF) | (slot.version << 16));
    put_le32(memory.block, at + 4, static_cast<uint32_t>(address >> 32));
    put_le32(memory.block, at + 8, static_cast<uint32_t>(address));
    put_le32(memory.block, at + 12, 0);
    put_le32(memory.block, at + 16, 0);
    put_le32(memory.block, at + 20, slot.table_bytes);
    put_le32(memory.block, at + 24, slot.flags);
  }
  amd_gmc8_flush_hdp(registers_);   // what the CPU wrote through the memory BAR is visible to the SMC

  // smu7_request_smu_load_fw: clear the status, tell the SMC where its scratch area and the table are, ask for the images.
  if (!smu_.write_sram_dword(status_address, 0)) return Result::NoSoftRegisters;
  const uint64_t scratch = memory.gpu_address + layout.scratch_offset, toc = memory.gpu_address + layout.toc_offset;
  const struct { uint16_t message; uint32_t argument; } steps[] = {
      {kMsgSmuDramAddrHi, static_cast<uint32_t>(scratch >> 32)},
      {kMsgSmuDramAddrLo, static_cast<uint32_t>(scratch)},
      {kMsgDrvDramAddrHi, static_cast<uint32_t>(toc >> 32)},
      {kMsgDrvDramAddrLo, static_cast<uint32_t>(toc)},
      {kMsgLoadUcodes, kMaskLoaded},
  };
  for (const auto& step : steps) {
    const AmdSmu7::Result sent = smu_.send_with_parameter(step.message, step.argument, nullptr, timeout_us);
    if (sent == AmdSmu7::Result::NotAnswering) return Result::NotAnswering;
    if (sent != AmdSmu7::Result::Ok) return Result::Refused;
  }

  // smu7_check_fw_load_finish: phm_wait_on_indirect_register on SMC_IND_INDEX_11 / SMC_IND_DATA_11, until the status has every bit asked for.
  registers_.write(mmSMC_IND_INDEX_11, status_address);
  const uint64_t begin = clock_.now_us();
  uint32_t status = 0;
  do {
    status = registers_.read(mmSMC_IND_DATA_11);
    if (loaded) *loaded = status;
    if ((status & kMaskLoaded) == kMaskLoaded) return Result::Ok;
  } while (clock_.now_us() - begin < timeout_us);
  return Result::TimedOut;
}

}  // namespace hardware
