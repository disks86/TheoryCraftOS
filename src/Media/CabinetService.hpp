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
#include "Allocators/IAllocator.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

// What cabinet.dll (src/Windows/cabinet) asks of the kernel: NtTcosCabinet, see src/Process/WindowsCabinet.hpp for the service's arguments. A whole cabinet
// in memory is described (one fixed record a file) or one of its files taken out, by the reader of Media/Cabinet.hpp. Nothing is kept between calls, so
// taking a file out decodes its folder from the start every time: fine for the installers' cabinets of one or a few files that FDI is used for here,
// slow (quadratic) for a folder of thousands.
namespace media {

enum class CabinetServiceStatus : uint8_t { Ok, InvalidParameter, BufferTooSmall, NoMemory, BadData };

constexpr uint64_t kCabinetRecordSize = 280, kCabinetRecordName = 256;
constexpr uint32_t kCabinetRecordUtf8 = 1, kCabinetRecordNameCut = 2;
constexpr uint64_t kCabinetServiceMaxCabinet = 512ull << 20;      // the largest cabinet taken, and the largest file given out
constexpr uint64_t kCabinetServiceMaxFile = 512ull << 20;

// Appends the record of every file of `cabinet` to `records`, in the order the cabinet lists them, and sets `*files` to how many there are. With
// `capacity` (the bytes the caller has room for) too small for them, `*files` is set and nothing is appended: BufferTooSmall.
[[nodiscard]] CabinetServiceStatus describe_cabinet(allocators::IAllocator& allocator, ConstByteView cabinet, uint64_t capacity, ByteBuffer* records, uint64_t* files);
// Appends the contents of file number `index` (as describe_cabinet() numbers them) to `out` and sets `*size` to its size. A file larger than `capacity`
// is not decoded: BufferTooSmall, with `*size` set.
[[nodiscard]] CabinetServiceStatus extract_cabinet_file(allocators::IAllocator& allocator, ConstByteView cabinet, uint64_t index, uint64_t capacity, ByteBuffer* out,
                                                        uint64_t* size);

// NtTcosCabinet.
uint32_t windows_cabinet_service(process::Process& p, const process::SyscallRequest& r);
// Makes NtTcosCabinet reach windows_cabinet_service().
void install_windows_cabinet();

}  // namespace media
