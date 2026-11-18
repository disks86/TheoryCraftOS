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

// Where Wi-Fi firmware files come from. Every chip of the popular families needs one or more files (microcode, patches, board and calibration data, regulatory blobs) that
// the vendor publishes in linux-firmware; the system keeps them in the "/firmware" folder under the same relative names Linux uses ("rtw88/rtw8822c_fw.bin",
// "ath10k/QCA6174/hw3.0/board-2.bin", "mediatek/WIFI_RAM_CODE_MT7961_1.bin", "brcm/brcmfmac4356-pcie.bin"), so a copy of linux-firmware can be dropped in as it is.
// Intel's files sit at the top ("iwlwifi-9000-pu-b0-jf-b0-46.ucode"), as they do in linux-firmware.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/StringView.hpp"

namespace hardware {

class IWifiFirmwareSource {
 public:
  // Replaces `out` with the whole file called `name` (relative to the firmware folder); false if there is no such file (or it is empty or larger than kMaxBytes).
  virtual bool read(const char* name, ByteBuffer& out) = 0;
  static constexpr uint64_t kMaxBytes = 16u << 20;     // ath11k/ath12k amss.bin is the largest at about 6 MiB

 protected:
  constexpr IWifiFirmwareSource() = default;
  ~IWifiFirmwareSource() = default;
};

// The system's: "/firmware/<name>" of a file system. A name that leaves the folder ("..", a leading '/') or is empty is refused.
class FileSystemFirmwareSource final : public IWifiFirmwareSource {
 public:
  FileSystemFirmwareSource(filesystem::IFileSystem& file_system, allocators::IAllocator& allocator) : file_system_(file_system), allocator_(allocator) {}
  bool read(const char* name, ByteBuffer& out) override;

 private:
  filesystem::IFileSystem& file_system_;
  allocators::IAllocator& allocator_;
};

// True if `name` is a relative path that stays inside the firmware folder: not empty, no leading '/', no "." or ".." component, no empty component, printable ASCII.
[[nodiscard]] bool wifi_firmware_name_valid(StringView name);

}  // namespace hardware
