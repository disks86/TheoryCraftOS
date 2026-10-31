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

namespace process::filesecurity {

// A minimal model of file security: a directory or file made with a security descriptor keeps it (owner, group and DACL, in the self-relative form; the SACL is dropped), kept by
// path in the object manager (ObjectManager::security_set). What it decides, for the one user this system has and the groups that user is in (user_sid(), in_token()):
// adding a file or directory to a directory, opening a file or directory with some access, and deleting one. Nothing is inherited: a file made in a directory with a descriptor
// has none of its own. A node with no descriptor is open to everybody, as is one with a NULL DACL; an empty DACL grants nothing.
constexpr uint32_t kMaxDescriptorBytes = 416;      // 20 (header) + 2 * 68 (a SID of 15 subauthorities) + 256 (the DACL)
constexpr uint32_t kMaxAclBytes = 256;
struct Stored {
  uint8_t bytes[kMaxDescriptorBytes] = {};
  uint32_t size = 0;
};

// Access rights of files and directories (the ones that matter here).
constexpr uint32_t kFileAddFile = 0x2, kFileAddSubdirectory = 0x4, kFileDeleteChild = 0x40, kDelete = 0x10000, kReadControl = 0x20000, kWriteDac = 0x40000, kMaximumAllowed = 0x02000000;

// Reads memory of the program that made the call: false if it cannot be read.
using ReadMemory = bool (*)(void* context, uint64_t address, ByteView out);

// The descriptor at `address` of the program (self-relative, or absolute with pointers) as it is kept. False if it is malformed, or does not fit.
bool import_descriptor(ReadMemory read, void* context, uint64_t address, Stored* out);

// The access a program gets when it asks for the rights in `desired` (generic rights are mapped for files): do the owner and the DACL grant all of them? The owner always has READ_CONTROL
// and WRITE_DAC. A descriptor that cannot be read grants nothing.
bool allows(const Stored& descriptor, uint32_t desired);

// The generic rights of a file, as the specific ones.
uint32_t map_generic(uint32_t access);

}  // namespace process::filesecurity
