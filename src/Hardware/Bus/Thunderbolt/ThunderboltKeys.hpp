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

// Where the one-time keys of Thunderbolt devices are kept (secure connect). A device that supports it stores a 32 byte key in its own NVM when it is first approved at the secure
// security level; afterwards it proves it still has the key by answering a random challenge with HMAC-SHA256 of the challenge under the key. The key is the secret that makes the device
// the same device, so it lives where only the OS reads it: one file per device in `filesystem::layout::kThunderboltKeys`, named by the device's UUID.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Utility/String.hpp"

namespace hardware {

constexpr uint32_t kTbKeyBytes = 32;

class ITbKeyStore {
 public:
  // The key of the device with this UUID; false if there is none.
  virtual bool load(const uint8_t uuid[16], uint8_t key[kTbKeyBytes]) = 0;
  // Keeps the key (replacing one); false if it could not be kept.
  virtual bool store(const uint8_t uuid[16], const uint8_t key[kTbKeyBytes]) = 0;
  virtual void remove(const uint8_t uuid[16]) = 0;

 protected:
  constexpr ITbKeyStore() = default;
  ~ITbKeyStore() = default;
};

class FileTbKeyStore final : public ITbKeyStore {
 public:
  FileTbKeyStore(filesystem::IFileSystem& file_system, allocators::IAllocator& allocator) : file_system_(file_system), allocator_(allocator) {}
  bool load(const uint8_t uuid[16], uint8_t key[kTbKeyBytes]) override;
  bool store(const uint8_t uuid[16], const uint8_t key[kTbKeyBytes]) override;
  void remove(const uint8_t uuid[16]) override;

 private:
  bool path_of(const uint8_t uuid[16], String& path);
  filesystem::IFileSystem& file_system_;
  allocators::IAllocator& allocator_;
};

}  // namespace hardware
