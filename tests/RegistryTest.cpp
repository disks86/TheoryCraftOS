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

// Host-side tests for the registry tree of Windows programs.
#include <stdio.h>
#include <stdlib.h>
#include "Process/RegistryTree.hpp"

void bounds_violation(const char*, uint64_t, uint64_t) { abort(); }

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { free(p); }
};

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

using namespace process::windowsabi;

int main() {
  TestAllocator allocator;
  RegistryTree tree(allocator, 100);
  uint32_t id = 0, other = 0;
  bool created = false;

  // the hives are there
  CHECK(tree.resolve(RegistryTree::kRoot, "\\Registry\\Machine\\Software", false, 1, &id, &created) == kRegSuccess && !created);
  CHECK(tree.resolve(RegistryTree::kRoot, "machine\\SOFTWARE\\microsoft\\WINDOWS NT\\currentversion", false, 1, &id, &created) == kRegSuccess);
  const RegValue* name = tree.find_value(id, "productname");
  CHECK(name && name->type == 1 && name->data.size() > 2);
  String full(allocator);
  CHECK(tree.full_name(id, &full) && full.view().equals("\\REGISTRY\\MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"));

  // missing keys, creating, casing
  CHECK(tree.resolve(RegistryTree::kRoot, "MACHINE\\Software\\Wine", false, 1, &id, &created) == kRegNameNotFound);
  CHECK(tree.resolve(RegistryTree::kRoot, "MACHINE\\Software\\Wine\\Deep\\Er", true, 5, &id, &created) == kRegSuccess && created);
  CHECK(tree.resolve(RegistryTree::kRoot, "machine\\software\\WINE\\deep\\er", true, 6, &other, &created) == kRegSuccess && !created && other == id);
  CHECK(tree.resolve(RegistryTree::kRoot, "\\Other\\Machine", false, 1, &other, &created) == kRegNameNotFound);
  CHECK(tree.resolve(RegistryTree::kRoot, "MACHINE\\\\X", true, 1, &other, &created) == kRegPathSyntax);

  // values
  CHECK(tree.set_value(id, "Alpha", 4, ConstByteView(reinterpret_cast<const uint8_t*>("\x01\x00\x00\x00"), 4), 7) == kRegSuccess);
  CHECK(tree.set_value(id, "Beta", 3, ConstByteView(reinterpret_cast<const uint8_t*>("xyz"), 3), 8) == kRegSuccess);
  CHECK(tree.set_value(id, "ALPHA", 4, ConstByteView(reinterpret_cast<const uint8_t*>("\x02\x00\x00\x00"), 4), 9) == kRegSuccess);
  CHECK(tree.statistics(id).values == 2 && tree.statistics(id).max_value_data_bytes == 4 && tree.statistics(id).max_value_name_bytes == 10);
  const RegValue* alpha = tree.find_value(id, "alpha");
  CHECK(alpha && alpha->data.view()[0] == 2 && alpha->name.view().equals("Alpha"));
  CHECK(tree.delete_value(id, "beta", 10) == kRegSuccess && tree.delete_value(id, "beta", 10) == kRegNameNotFound && tree.statistics(id).values == 1);

  // subkeys are sorted, deleting needs them gone and waits for the handles
  uint32_t parent = 0;
  CHECK(tree.resolve(RegistryTree::kRoot, "MACHINE\\Software\\Wine", false, 1, &parent, &created) == kRegSuccess);
  CHECK(tree.resolve(parent, "zeta", true, 11, &other, &created) == kRegSuccess && tree.resolve(parent, "Alef", true, 11, &other, &created) == kRegSuccess);
  CHECK(tree.statistics(parent).subkeys == 3 && tree.key(tree.key(parent)->subkeys[0])->name.view().equals("Alef"));
  CHECK(tree.delete_key(parent) == kRegCannotDelete);
  tree.add_handle(other);
  CHECK(tree.delete_key(other) == kRegSuccess && !tree.alive(other) && tree.statistics(parent).subkeys == 2);
  CHECK(tree.delete_key(other) == kRegKeyDeleted && tree.set_value(other, "v", 1, ConstByteView(), 1) == kRegKeyDeleted);
  CHECK(tree.resolve(parent, "Alef", true, 12, &other, &created) == kRegSuccess && created);      // a new key of the same name
  tree.release_handle(other);
  CHECK(tree.delete_key(RegistryTree::kRoot) == kRegCannotDelete);

  // limits
  char longname[300];
  for (int i = 0; i < 299; ++i) longname[i] = 'a';
  longname[299] = 0;
  CHECK(tree.resolve(RegistryTree::kRoot, StringView(longname), true, 1, &other, &created) == kRegInvalidParameter);

  printf("RegistryTest: %s\n", g_failed ? "FAILED" : "ok");
  return g_failed ? 1 : 0;
}
