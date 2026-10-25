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
#include "Utility/ArrayView.hpp"

namespace filesystem {
namespace ext {

// On-disk constants and small accessors shared by the ext2/ext3/ext4 sources. Everything on these volumes is little endian. The accessors
// are bounds checked (a view that is too short stops the OS), so callers validate the offsets they take from the volume first.

constexpr uint16_t kMagic = 0xEF53;
constexpr uint32_t kSuperblockOffset = 1024, kSuperblockSize = 1024;
constexpr uint32_t kRootInode = 2, kJournalInode = 8, kOldFirstInode = 11, kOldInodeSize = 128;
constexpr uint32_t kMaxInodeSize = 1024;

// Superblock fields (offsets into the 1024 bytes).
constexpr uint32_t kSbInodesCount = 0x00, kSbBlocksCount = 0x04, kSbFreeBlocks = 0x0C, kSbFreeInodes = 0x10, kSbFirstDataBlock = 0x14,
                   kSbLogBlockSize = 0x18, kSbBlocksPerGroup = 0x20, kSbInodesPerGroup = 0x28, kSbWriteTime = 0x30, kSbMagic = 0x38,
                   kSbState = 0x3A, kSbRevLevel = 0x4C, kSbFirstIno = 0x54, kSbInodeSize = 0x58, kSbFeatureCompat = 0x5C,
                   kSbFeatureIncompat = 0x60, kSbFeatureRoCompat = 0x64, kSbUuid = 0x68, kSbVolumeName = 0x78, kSbReservedGdt = 0xCE,
                   kSbJournalInum = 0xE0, kSbLastOrphan = 0xE8, kSbDescSize = 0xFE, kSbFirstMetaBg = 0x104, kSbBlocksCountHi = 0x150,
                   kSbFreeBlocksHi = 0x158, kSbWantExtraIsize = 0x15E, kSbLogGroupsPerFlex = 0x174, kSbChecksumType = 0x175,
                   kSbBackupBgs = 0x24C, kSbChecksumSeed = 0x270, kSbChecksum = 0x3FC;

constexpr uint16_t kStateValid = 0x0001, kStateError = 0x0002, kStateOrphans = 0x0004;

// Feature flags.
constexpr uint32_t kCompatHasJournal = 0x0004, kCompatExtAttr = 0x0008, kCompatResizeInode = 0x0010, kCompatDirIndex = 0x0020,
                   kCompatSparseSuper2 = 0x0200, kCompatStableInodes = 0x0800, kCompatOrphanFile = 0x1000, kCompatOrphanPresent = 0x10000;
constexpr uint32_t kIncompatFiletype = 0x0002, kIncompatRecover = 0x0004, kIncompatJournalDev = 0x0008, kIncompatMetaBg = 0x0010,
                   kIncompatExtents = 0x0040, kIncompat64Bit = 0x0080, kIncompatMmp = 0x0100, kIncompatFlexBg = 0x0200,
                   kIncompatEaInode = 0x0400, kIncompatCsumSeed = 0x2000, kIncompatLargeDir = 0x4000, kIncompatInlineData = 0x8000,
                   kIncompatEncrypt = 0x10000, kIncompatCasefold = 0x20000;
constexpr uint32_t kRoSparseSuper = 0x0001, kRoLargeFile = 0x0002, kRoBtreeDir = 0x0004, kRoHugeFile = 0x0008, kRoGdtCsum = 0x0010,
                   kRoDirNlink = 0x0020, kRoExtraIsize = 0x0040, kRoQuota = 0x0100, kRoBigalloc = 0x0200, kRoMetadataCsum = 0x0400;

// Group descriptor fields (the first 32 bytes; the rest exists with the 64bit feature).
constexpr uint32_t kGdBlockBitmapCsum = 0x18, kGdInodeBitmapCsum = 0x1A, kGdBlockBitmapCsumHi = 0x38, kGdInodeBitmapCsumHi = 0x3A;
constexpr uint32_t kGdBlockBitmap = 0x00, kGdInodeBitmap = 0x04, kGdInodeTable = 0x08, kGdFreeBlocks = 0x0C, kGdFreeInodes = 0x0E,
                   kGdUsedDirs = 0x10, kGdFlags = 0x12, kGdItableUnused = 0x1C, kGdChecksum = 0x1E, kGdBlockBitmapHi = 0x20,
                   kGdInodeBitmapHi = 0x24, kGdInodeTableHi = 0x28, kGdFreeBlocksHi = 0x2C, kGdFreeInodesHi = 0x2E, kGdUsedDirsHi = 0x30,
                   kGdItableUnusedHi = 0x32;
constexpr uint16_t kGroupInodeUninit = 0x0001, kGroupBlockUninit = 0x0002, kGroupItableZeroed = 0x0004;

// Inode fields.
constexpr uint32_t kInMode = 0x00, kInUid = 0x02, kInSizeLo = 0x04, kInAtime = 0x08, kInCtime = 0x0C, kInMtime = 0x10, kInDtime = 0x14,
                   kInGid = 0x18, kInLinks = 0x1A, kInBlocksLo = 0x1C, kInFlags = 0x20, kInBlock = 0x28, kInGeneration = 0x64,
                   kInFileAcl = 0x68, kInSizeHi = 0x6C, kInBlocksHi = 0x74, kInFileAclHi = 0x76, kInChecksumLo = 0x7C, kInExtraIsize = 0x80,
                   kInChecksumHi = 0x82, kInCrtime = 0x90;
constexpr uint32_t kGoodOldInodeSize = 128;
constexpr uint32_t kInlineBlocks = 15;  // i_block holds 15 32-bit numbers
constexpr uint16_t kModeTypeMask = 0170000, kModeSocket = 0140000, kModeLink = 0120000, kModeRegular = 0100000, kModeBlock = 0060000,
                   kModeDirectory = 0040000, kModeChar = 0020000, kModeFifo = 0010000;
constexpr uint32_t kFlagImmutable = 0x00000010, kFlagAppend = 0x00000020, kFlagIndex = 0x00001000, kFlagHugeFile = 0x00040000,
                   kFlagExtents = 0x00080000, kFlagInlineData = 0x10000000;

// Directory entries: inode(4) rec_len(2) name_len(1) file_type(1) name.
constexpr uint32_t kDirHeader = 8;
constexpr uint8_t kFtUnknown = 0, kFtRegular = 1, kFtDirectory = 2, kFtSymlink = 7;

constexpr uint32_t kXattrMagic = 0xEA020000;
constexpr uint32_t kXattrRefcount = 4, kXattrChecksum = 0x10;

// Extent trees: a header (magic, entries, max, depth, generation) then 12 byte entries, in i_block (4 entries) or in a block (and then a
// checksum after the last possible entry).
constexpr uint16_t kExtentMagic = 0xF30A;
constexpr uint32_t kExtentHeader = 12, kExtentEntry = 12, kExtentMaxDepth = 5, kExtentRootMax = 4;
constexpr uint32_t kExtentMaxInit = 32768;  // longest initialized extent (a longer length field marks an unwritten one)

inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) { return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24); }

inline uint8_t rd8(ConstByteView v, uint64_t o) { return v[o]; }
inline uint16_t rd16(ConstByteView v, uint64_t o) { return le16(v.subview(o, 2).data()); }
inline uint32_t rd32(ConstByteView v, uint64_t o) { return le32(v.subview(o, 4).data()); }
inline void wr8(ByteView v, uint64_t o, uint32_t x) { v[o] = static_cast<uint8_t>(x); }
inline void wr16(ByteView v, uint64_t o, uint32_t x) {
  ByteView p = v.subview(o, 2);
  p[0] = static_cast<uint8_t>(x);
  p[1] = static_cast<uint8_t>(x >> 8);
}
inline void wr32(ByteView v, uint64_t o, uint32_t x) {
  wr16(v, o, x);
  wr16(v, o + 2, x >> 16);
}
inline uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }

inline bool bit_test(ConstByteView bits, uint64_t index) { return (bits[index >> 3] >> (index & 7)) & 1; }
inline void bit_set(ByteView bits, uint64_t index) { bits[index >> 3] = static_cast<uint8_t>(bits[index >> 3] | (1u << (index & 7))); }
inline void bit_clear(ByteView bits, uint64_t index) { bits[index >> 3] = static_cast<uint8_t>(bits[index >> 3] & ~(1u << (index & 7))); }

// The first clear bit in [from, to), or `to` if there is none (whole bytes of ones are skipped).
inline uint64_t bit_find_zero(ConstByteView bits, uint64_t from, uint64_t to) {
  uint64_t i = from;
  while (i < to) {
    if ((i & 7) == 0 && i + 8 <= to && bits[i >> 3] == 0xFF) {
      i += 8;
      continue;
    }
    if (!bit_test(bits, i)) return i;
    ++i;
  }
  return to;
}

}  // namespace ext
}  // namespace filesystem
