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

// Trust on first use: the identity (a fingerprint of the host key or certificate) a server showed the first time is remembered for as long as the
// program runs; a different one later is a server that is not who it was. Keys are made up by the caller ("sftp:name:port").
#pragma once
#include "Kernel/Sync.hpp"
#include "Utility/Move.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace network {

class HostTrust {
 public:
  explicit HostTrust(allocators::IAllocator& allocator) : allocator_(allocator), entries_(allocator) {}

  enum class Result : uint8_t { New, Match, Changed, OutOfMemory };
  static constexpr uint32_t kFingerprintSize = 32;

  // Remembers `fingerprint` (32 bytes) for `key` the first time (New); later answers Match if it is the same and Changed if it is not.
  Result check(StringView key, const uint8_t* fingerprint) {
    kernel::ScopedRecursiveMutex lock(mutex_);
    for (Entry& e : entries_.view()) {
      if (!e.key.view().equals(key)) continue;
      uint8_t diff = 0;
      for (uint32_t i = 0; i < kFingerprintSize; ++i) diff |= e.fingerprint[i] ^ fingerprint[i];
      return diff == 0 ? Result::Match : Result::Changed;
    }
    Entry e(allocator_);
    if (!e.key.assign(key)) return Result::OutOfMemory;
    for (uint32_t i = 0; i < kFingerprintSize; ++i) e.fingerprint[i] = fingerprint[i];
    return entries_.push_back(util::move(e)) ? Result::New : Result::OutOfMemory;
  }
  // Forgets what was remembered for `key`: the next visit is a first one (a user who knows the server's key really changed says so).
  void forget(StringView key) {
    kernel::ScopedRecursiveMutex lock(mutex_);
    for (uint64_t i = 0; i < entries_.count(); ++i)
      if (entries_[i].key.view().equals(key)) {
        entries_.remove_at(i);
        return;
      }
  }

 private:
  struct Entry {
    explicit Entry(allocators::IAllocator& a) : key(a) {}
    String key;
    uint8_t fingerprint[kFingerprintSize] = {};
  };
  allocators::IAllocator& allocator_;
  kernel::RecursiveMutex mutex_;
  List<Entry> entries_;
};

}  // namespace network
