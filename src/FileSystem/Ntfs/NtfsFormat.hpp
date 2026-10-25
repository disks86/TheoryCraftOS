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
#include "FileSystem/FileSystemTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace filesystem {
namespace ntfs {

// On-disk constants and small accessors shared by the NTFS sources. Everything on an NTFS volume is little endian. The accessors are
// bounds checked (a view that is too short stops the OS), so callers validate the offsets they take from the volume first.

constexpr uint32_t kAttrStandardInformation = 0x10, kAttrAttributeList = 0x20, kAttrFileName = 0x30, kAttrVolumeName = 0x60,
                   kAttrVolumeInformation = 0x70, kAttrData = 0x80, kAttrIndexRoot = 0x90, kAttrIndexAllocation = 0xA0, kAttrBitmap = 0xB0,
                   kAttrEnd = 0xFFFFFFFF;

constexpr uint64_t kRecMft = 0, kRecMirror = 1, kRecLogFile = 2, kRecVolume = 3, kRecRoot = 5, kRecBitmap = 6, kRecUpcase = 10, kFirstUserRecord = 24;
constexpr uint64_t kRefMask = 0x0000FFFFFFFFFFFFull;  // a file reference: 48 bits of record number, 16 bits of sequence number
constexpr uint64_t kHole = ~uint64_t{0};              // lcn of a sparse run
constexpr uint64_t kMaxClusters = uint64_t{1} << 40;

constexpr uint16_t kRecordInUse = 0x0001, kRecordDirectory = 0x0002;
constexpr uint32_t kFileReadOnly = 0x00000001, kFileArchive = 0x00000020, kFileIndexPresent = 0x10000000;
constexpr uint8_t kNamePosix = 0, kNameWin32 = 1, kNameDos = 2, kNameWin32AndDos = 3;
constexpr uint16_t kStreamCompressed = 0x0001, kStreamEncrypted = 0x4000, kStreamSparse = 0x8000;
constexpr uint16_t kEntryChild = 0x0001, kEntryLast = 0x0002;
constexpr uint32_t kVolumeDirty = 0x0001;
constexpr uint64_t kTimeStamp = 119600064000000000ull;  // 1980-01-01 in 100 ns steps since 1601: new entries have no real time
constexpr uint64_t kFileTimeOfUnixEpoch = 116444736000000000ull;  // 1970-01-01 in 100 ns steps since 1601
constexpr uint32_t kFixupUnit = 512;                    // multi sector protection always works on 512 byte pieces

// Time stamps are FILETIMEs (UTC). "Now" is the system wall clock, or the fixed 1980 time while there is none (0: no clock).
inline uint64_t filetime_now() {
  const uint64_t unix_seconds = unix_time_now();
  return unix_seconds ? kFileTimeOfUnixEpoch + unix_seconds * 10000000ull : 0;
}
inline uint64_t unix_from_filetime(uint64_t filetime) { return filetime > kFileTimeOfUnixEpoch ? (filetime - kFileTimeOfUnixEpoch) / 10000000ull : 0; }

inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) { return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24); }
inline uint64_t le64(const uint8_t* p) { return uint64_t{le32(p)} | (uint64_t{le32(p + 4)} << 32); }

inline uint16_t rd16(ConstByteView v, uint64_t o) { return le16(v.subview(o, 2).data()); }
inline uint32_t rd32(ConstByteView v, uint64_t o) { return le32(v.subview(o, 4).data()); }
inline uint64_t rd64(ConstByteView v, uint64_t o) { return le64(v.subview(o, 8).data()); }
inline void wr16(ByteView v, uint64_t o, uint32_t x) {
  ByteView p = v.subview(o, 2);
  p[0] = static_cast<uint8_t>(x);
  p[1] = static_cast<uint8_t>(x >> 8);
}
inline void wr32(ByteView v, uint64_t o, uint32_t x) {
  wr16(v, o, x);
  wr16(v, o + 2, x >> 16);
}
inline void wr64(ByteView v, uint64_t o, uint64_t x) {
  wr32(v, o, static_cast<uint32_t>(x));
  wr32(v, o + 4, static_cast<uint32_t>(x >> 32));
}
inline uint32_t align8(uint32_t x) { return (x + 7) & ~7u; }

}  // namespace ntfs
}  // namespace filesystem
