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
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

// The registry of Windows programs: a tree of keys, each with named values. It is kept in memory (nothing is written to disk) and shared by every program. The names are UTF-8
// here and compared without regard to case; the NT services (WindowsRegistry.cpp) convert from and to UTF-16. A key that is deleted while handles to it are open stays
// until the last one is closed, and then answers STATUS_KEY_DELETED.
namespace process::windowsabi {

constexpr uint32_t kRegSuccess = 0, kRegNameNotFound = 0xC0000034u, kRegNameCollision = 0xC0000035u, kRegInvalidParameter = 0xC000000Du, kRegKeyDeleted = 0xC000017Cu,
                   kRegCannotDelete = 0xC0000121u, kRegNoMemory = 0xC0000017u, kRegInvalidHandle = 0xC0000008u, kRegNoMoreEntries = 0x8000001Au, kRegPathSyntax = 0xC000003Bu;
constexpr uint32_t kRegMaxNameChars = 255, kRegMaxValueNameChars = 16383, kRegMaxDepth = 512;
constexpr uint32_t kRegMaxValueBytes = 1u << 20;

struct RegValue {
  explicit RegValue(allocators::IAllocator& allocator) : name(allocator), data(allocator) {}
  String name;
  uint32_t type = 0;
  ByteBuffer data;
};

struct RegKey {
  explicit RegKey(allocators::IAllocator& allocator) : name(allocator), subkeys(allocator), values(allocator) {}
  String name;
  uint32_t parent = 0;
  List<uint32_t> subkeys;                    // ids, in alphabetical order without regard to case
  List<UniquePointer<RegValue>> values;      // in the order they were made
  uint64_t write_time = 0;                   // FILETIME
  uint32_t open_handles = 0;
  bool deleted = false;
  bool volatile_key = false;
};

class RegistryTree {
 public:
  static constexpr uint32_t kRoot = 0;
  // Makes the root "REGISTRY" with the usual hives (MACHINE with HARDWARE, SAM, SECURITY, SOFTWARE and SYSTEM; USER with .DEFAULT and the user of the session).
  RegistryTree(allocators::IAllocator& allocator, uint64_t now);

  // Finds or makes the key named by `path` (components separated by '\'), starting at `base`. A path that starts with '\' is absolute: its first component must be the root's name,
  // and `base` is ignored. NTSTATUS; *created says whether `create` made the last key.
  uint32_t resolve(uint32_t base, StringView path, bool create, uint64_t now, uint32_t* id, bool* created);
  RegKey* key(uint32_t id) { UniquePointer<RegKey>* k = keys_.get(id); return k && *k ? k->get() : nullptr; }
  const RegKey* key(uint32_t id) const { const UniquePointer<RegKey>* k = keys_.get(id); return k && *k ? k->get() : nullptr; }
  bool alive(uint32_t id) const { const RegKey* k = key(id); return k && !k->deleted; }
  // The full name from the root, e.g. "\REGISTRY\MACHINE\SOFTWARE".
  bool full_name(uint32_t id, String* out) const;

  // Handle accounting: a deleted key is dropped when its last handle goes.
  void add_handle(uint32_t id);
  void release_handle(uint32_t id);

  // NtDeleteKey: a key with subkeys cannot be deleted (kRegCannotDelete), the root neither.
  uint32_t delete_key(uint32_t id);

  const RegValue* find_value(uint32_t id, StringView name) const;
  uint32_t set_value(uint32_t id, StringView name, uint32_t type, ConstByteView data, uint64_t now);
  uint32_t delete_value(uint32_t id, StringView name, uint64_t now);

  // Sizes for NtQueryKey (names in UTF-16 bytes).
  struct Statistics { uint32_t subkeys = 0, values = 0, max_subkey_name_bytes = 0, max_value_name_bytes = 0, max_value_data_bytes = 0; };
  Statistics statistics(uint32_t id) const;

  static bool names_equal(StringView a, StringView b);
  static bool valid_key_name(StringView name);

 private:
  uint32_t new_key(uint32_t parent, StringView name, uint64_t now);
  uint32_t find_child(uint32_t parent, StringView name) const;
  bool link_child(uint32_t parent, uint32_t child);
  void drop_if_unused(uint32_t id);

  allocators::IAllocator& allocator_;
  List<UniquePointer<RegKey>> keys_;
};

// The number of UTF-16 code units of UTF-8 text.
uint32_t utf16_units(StringView text);

}  // namespace process::windowsabi
