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

// Where the Bluetooth bonds are kept: one file in the Bluetooth application's folder of the AppStore (the keys that make a paired device the same device, so only the OS reads it).
#pragma once
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

class IBluetoothBondStorage {
 public:
  // Replaces `out` with the stored bytes; false if there are none.
  virtual bool load(ByteBuffer& out) = 0;
  virtual bool save(ConstByteView bytes) = 0;

 protected:
  constexpr IBluetoothBondStorage() = default;
  ~IBluetoothBondStorage() = default;
};

class FileBluetoothBonds final : public IBluetoothBondStorage {
 public:
  FileBluetoothBonds(filesystem::IFileSystem& file_system, allocators::IAllocator& allocator) : file_system_(file_system), allocator_(allocator) {}
  bool load(ByteBuffer& out) override;
  bool save(ConstByteView bytes) override;

 private:
  filesystem::IFileSystem& file_system_;
  allocators::IAllocator& allocator_;
};

}  // namespace hardware
