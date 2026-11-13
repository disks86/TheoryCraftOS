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

// The on-disk layout of SquashFS 4.0 (what mksquashfs, the Linux kernel and the AppImage runtime use). Everything is little-endian.
//
//   superblock (96 bytes) | [compressor options] | data blocks and fragments | inode table | directory table | fragment table | export table |
//   id table | xattr tables
//
// The inode and directory tables are streams of "metadata blocks": a 16-bit header (bit 15: stored uncompressed, low 15 bits: bytes that follow)
// and at most 8 KiB once decompressed. An inode reference is (offset of its metadata block from the start of the inode table) << 16 | (offset in the
// decompressed block). File data is in blocks of the superblock's block size, each with a 32-bit size word (bit 24: stored uncompressed, low 24 bits:
// bytes on disk, 0: a hole of zeros); the tail of a file may sit in a shared "fragment" block instead.
namespace filesystem::squash {

constexpr uint32_t kMagic = 0x73717368;      // "hsqs"
constexpr uint32_t kSuperblockSize = 96;
constexpr uint32_t kMetadataSize = 8192;      // a metadata block, decompressed
constexpr uint16_t kMetadataUncompressed = 0x8000;
constexpr uint32_t kDataUncompressed = 1u << 24;
constexpr uint32_t kDataSizeMask = kDataUncompressed - 1;
constexpr uint32_t kNoFragment = 0xFFFFFFFFu;
constexpr uint32_t kMinBlockSize = 4096, kMaxBlockSize = 1u << 20;
constexpr uint32_t kFragmentEntrySize = 16, kFragmentsPerBlock = kMetadataSize / kFragmentEntrySize;
constexpr uint32_t kMaxDirectoryRun = 256;      // entries under one directory header

enum class Compression : uint16_t { Gzip = 1, Lzma = 2, Lzo = 3, Xz = 4, Lz4 = 5, Zstd = 6 };

namespace flag {
constexpr uint16_t kUncompressedInodes = 0x0001, kUncompressedData = 0x0002, kUncompressedFragments = 0x0008, kNoFragments = 0x0010,
                   kAlwaysFragments = 0x0020, kDuplicates = 0x0040, kExportable = 0x0080, kUncompressedXattrs = 0x0100, kNoXattrs = 0x0200,
                   kCompressorOptions = 0x0400, kUncompressedIds = 0x0800;
}

// Inode types (the "extended" ones carry 64-bit sizes, hole counts and an xattr index).
enum class InodeType : uint16_t {
  Directory = 1, File = 2, Symlink = 3, BlockDevice = 4, CharDevice = 5, Fifo = 6, Socket = 7,
  ExtDirectory = 8, ExtFile = 9, ExtSymlink = 10, ExtBlockDevice = 11, ExtCharDevice = 12, ExtFifo = 13, ExtSocket = 14,
};

// Byte offsets in the superblock.
namespace sb {
constexpr uint32_t kMagicAt = 0, kInodeCount = 4, kModified = 8, kBlockSize = 12, kFragmentCount = 16, kCompression = 20, kBlockLog = 22,
                   kFlags = 24, kIdCount = 26, kVersionMajor = 28, kVersionMinor = 30, kRootInode = 32, kBytesUsed = 40, kIdTable = 48,
                   kXattrTable = 56, kInodeTable = 64, kDirectoryTable = 72, kFragmentTable = 80, kExportTable = 88;
}

}  // namespace filesystem::squash
