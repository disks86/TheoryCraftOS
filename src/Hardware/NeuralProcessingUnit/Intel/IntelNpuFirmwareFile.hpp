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
#include "FileSystem/IFileSystem.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuHw.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/FixedString.hpp"

namespace hardware {

// The firmware files an Intel NPU generation may use, newest first, in the order the Linux ivpu driver asks for them (intel/vpu/vpu_<gen>_v1.bin, then the
// older vpu_<gen>_v0.0.bin where one existed). `count` is set to how many there are (0 for an unknown generation).
const char* const* intel_npu_firmware_names(IntelNpuIp ip, uint32_t* count);

// Reads the first of those files that exists in `kLinuxFirmwareIntelVpu` into `out` and says which. False if there is none, or the file is empty or implausibly large
// (the largest firmware is a few MiB).
bool intel_npu_read_firmware(filesystem::IFileSystem& file_system, allocators::IAllocator& allocator, IntelNpuIp ip, ByteBuffer& out, FixedString<48>* name_used);

}  // namespace hardware
