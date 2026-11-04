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


#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuFirmwareFile.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

namespace {
constexpr uint64_t kMaxFirmwareBytes = 32ull << 20;
const char* const k37xx[] = {"vpu_37xx_v1.bin", "vpu_37xx_v0.0.bin"};
const char* const k40xx[] = {"vpu_40xx_v1.bin", "vpu_40xx_v0.0.bin"};
const char* const k50xx[] = {"vpu_50xx_v1.bin", "vpu_50xx_v0.0.bin"};
const char* const k60xx[] = {"vpu_60xx_v1.bin"};
}  // namespace

const char* const* intel_npu_firmware_names(IntelNpuIp ip, uint32_t* count) {
  switch (ip) {
    case IntelNpuIp::Ip37xx: *count = 2; return k37xx;
    case IntelNpuIp::Ip40xx: *count = 2; return k40xx;
    case IntelNpuIp::Ip50xx: *count = 2; return k50xx;
    case IntelNpuIp::Ip60xx: *count = 1; return k60xx;
    default: *count = 0; return nullptr;
  }
}

bool intel_npu_read_firmware(filesystem::IFileSystem& file_system, allocators::IAllocator& allocator, IntelNpuIp ip, ByteBuffer& out, FixedString<48>* name_used) {
  uint32_t count = 0;
  const char* const* names = intel_npu_firmware_names(ip, &count);
  for (uint32_t i = 0; i < count; ++i) {
    String path(allocator);
    if (!path.assign(filesystem::layout::kLinuxFirmwareIntelVpu) || !path.append("/") || !path.append(StringView(names[i]))) return false;
    UniquePointer<filesystem::IFile> file;
    if (file_system.open_file(StringView(path.c_str()), filesystem::OpenMode::Read, &file) != filesystem::Status::Ok || !file) continue;
    const uint64_t size = file->size();
    if (size == 0 || size > kMaxFirmwareBytes) continue;
    out.clear();
    if (!out.put_zeros(size)) return false;
    uint64_t got = 0;
    if (file->read(0, out.writable_view(), &got) != filesystem::Status::Ok || got != size) {
      out.clear();
      continue;
    }
    if (name_used) name_used->assign(names[i]);
    return true;
  }
  return false;
}

}  // namespace hardware
