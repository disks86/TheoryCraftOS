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

// The content decryption module under the Encrypted Media Extensions of the page: the one key system a machine without a licensed DRM can have, W3C Clear
// Key ("org.w3.clearkey"), in which the license is the key itself in a JSON Web Key Set. The MediaKeys, MediaKeySession and MediaKeySystemAccess objects
// of script are written in script (WebPrelude.cpp: the sessions, the messages, the license parsing); what must be native is the key table the decryption of
// the media samples (Mse.hpp: MseBuffer::decrypt()) looks the keys up in, and the lifetime of the keys: a session that closes takes its keys with it, and
// they are wiped from memory.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Media/Mse.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace media {

// The keys of one MediaKeys object: those of all its sessions.
class ClearKeyStore final : public IMseKeys {
 public:
  explicit ClearKeyStore(allocators::IAllocator& allocator) : entries_(allocator) {}
  ~ClearKeyStore() { wipe(); }
  ClearKeyStore(const ClearKeyStore&) = delete;
  ClearKeyStore& operator=(const ClearKeyStore&) = delete;

  // Adds a key to a session (a key identifier already in the session gets the new key). False if memory ran out or the store is full.
  bool add_key(uint32_t session, const uint8_t kid[16], const uint8_t key[16]);
  // A session closed: its keys are gone.
  void remove_session(uint32_t session);
  bool has_key(const uint8_t kid[16]) const;
  bool key_for(const uint8_t kid[16], uint8_t key[16]) const override;
  uint64_t key_epoch() const override { return epoch_; }
  uint64_t count() const { return entries_.count(); }

  static constexpr uint64_t kMaxKeys = 4096;

 private:
  struct Entry {
    uint32_t session;
    uint8_t kid[16];
    uint8_t key[16];
  };
  void wipe();
  List<Entry> entries_;
  uint64_t epoch_ = 1;
};

// The key stores of a page, by the numbers script knows them by.
class EmeHub {
 public:
  explicit EmeHub(allocators::IAllocator& allocator) : allocator_(allocator), stores_(allocator) {}
  static constexpr uint32_t kMaxStores = 64;
  uint32_t create();   // 0 if there are too many
  ClearKeyStore* find(uint32_t id);

 private:
  struct Slot {
    uint32_t id;
    UniquePointer<ClearKeyStore> store;
  };
  allocators::IAllocator& allocator_;
  List<Slot> stores_;
  uint32_t next_ = 1;
};

}  // namespace media
