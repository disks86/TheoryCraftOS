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
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGfx8Firmware.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/VideoCodec/AMD/AmdSmu7.hpp"

namespace hardware {

// How the microcode of the graphics engine gets into a Radeon RX Vega M: not by the driver writing it into the engines, but by the card's SMC. The driver hands the SMC a table
// of contents in video memory (struct SMU_DRAMData_TOC of smu_ucode_xfer_vi.h: one entry per image with its id, version, address and size), tells it where the table and
// a scratch area are, sends PPSMC_MSG_LoadUcodes with a mask of the images to load, and waits for the SMC to set the same bits in the UcodeLoadStatus word of its soft
// registers. Ported from the Linux amdgpu driver's smu7_smumgr.c (smu7_request_smu_load_fw, smu7_populate_single_firmware_entry, smu7_check_fw_load_finish; the SMC
// itself is started by the card's firmware, vegam_start_smu only starts it if it is not running) and amdgpu_cgs.c (amdgpu_cgs_get_firmware_info: which bytes of each file are
// the image). The microcode of the SMC is not touched.
//
// The images go to video memory the CPU can write (through the card's memory BAR): one block holding the table, the SMC's scratch area and one page-aligned slot per
// image, in the order of the table. The GPU addresses of the slots are the block's GPU address plus the offsets, so the memory controller must let the SMC reach them (VMID 0).
class AmdSmu7Ucode {
 public:
  // smu_ucode_xfer_vi.h
  static constexpr uint32_t kIdSdma0 = 1, kIdSdma1 = 2, kIdCpCe = 3, kIdCpPfp = 4, kIdCpMe = 5, kIdCpMec = 6, kIdCpMecJt1 = 7, kIdCpMecJt2 = 8, kIdRlcG = 10;
  static constexpr uint32_t kMaskLoaded = (1u << kIdSdma0) | (1u << kIdSdma1) | (1u << kIdCpCe) | (1u << kIdCpMe) | (1u << kIdCpPfp) | (1u << kIdCpMec) | (1u << kIdRlcG);
  static constexpr uint32_t kMaxEntries = 12;                 // SMU_MAX_ENTRIES
  static constexpr uint32_t kEntryBytes = 28;                 // sizeof(struct SMU_Entry)
  static constexpr uint32_t kTocBytes = 8 + kMaxEntries * kEntryBytes;   // sizeof(struct SMU_DRAMData_TOC)
  static constexpr uint64_t kPageSize = 4096;
  static constexpr uint64_t kTocAreaBytes = kPageSize;        // smu7_init: header_buffer, (sizeof TOC / 4096 + 1) pages
  static constexpr uint64_t kScratchBytes = 200 * kPageSize;  // smu7_init: smu_buffer
  // Where the SMC keeps UcodeLoadStatus: SMU7_FIRMWARE_HEADER_LOCATION + offsetof(SMU75_Firmware_Header, SoftRegisters) holds the address of the soft registers, and
  // the word is at offsetof(SMU75_SoftRegisters, UcodeLoadStatus) in them (both from the real smu75.h with offsetof).
  static constexpr uint32_t kHeaderSoftRegistersOffset = 48;
  static constexpr uint32_t kUcodeLoadStatusOffset = 108;
  static constexpr uint16_t kMsgSmuDramAddrHi = 0x252, kMsgSmuDramAddrLo = 0x253, kMsgDrvDramAddrHi = 0x250, kMsgDrvDramAddrLo = 0x251, kMsgLoadUcodes = 0x254;

  // The images, parsed (amd_gfx8_parse_firmware). The second MEC file is optional: only its jump table is used, for the table's JT2 entry.
  struct Images {
    AmdGfx8Image rlc, ce, pfp, me, mec, mec2, sdma0, sdma1;
  };
  // Where in the block everything goes.
  struct Layout {
    bool ok = false;
    uint64_t toc_offset = 0, scratch_offset = 0, total_bytes = 0;
    uint32_t entries = 0;
    struct Slot {
      uint32_t id = 0;
      uint64_t offset = 0;
      uint32_t image_bytes = 0;     // the bytes copied
      uint32_t table_bytes = 0;     // data_size_byte in the table entry
      uint32_t version = 0;
      uint16_t flags = 0;
      ConstByteView source;
    } slot[kMaxEntries];
  };
  static Layout plan(const Images& images);

  struct Memory {
    uint64_t gpu_address = 0;    // of the first byte of the block, page aligned
    MmioRegion block;            // at least Layout::total_bytes
  };
  enum class Result : uint8_t {
    Ok,
    BadArgument,       // an image is missing or invalid, or the block is too small or not aligned
    NotAnswering,      // the SMC's mailbox does not answer: it is not running
    NoSoftRegisters,   // the firmware header does not say where the soft registers are
    Refused,           // the SMC answered a message with an error
    TimedOut,          // UcodeLoadStatus never showed every image
  };
  static const char* result_name(Result result);

  AmdSmu7Ucode(IVcnRegisters& registers, const IClock& clock) : registers_(registers), clock_(clock), smu_(registers, clock) {}

  // smu7_request_smu_load_fw. `loaded` (if given) receives UcodeLoadStatus as it was last seen.
  Result load(const Images& images, const Memory& memory, uint32_t* loaded = nullptr, uint32_t timeout_us = AmdSmu7::kDefaultTimeoutUs);

 private:
  IVcnRegisters& registers_;
  const IClock& clock_;
  AmdSmu7 smu_;
};

}  // namespace hardware
