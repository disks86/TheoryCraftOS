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

// Host tests of the registry on disk (Process/RegistryStore.cpp, and the attach/save/detach of WindowsRegistry.cpp): what is saved and what is not
// (volatile keys, loaded hives), reading it back into a new tree as a restart would, the new-file/old-file handover after a power cut at each step,
// damaged files (every truncation, bit flips), and programs writing keys on several threads while the writer thread saves.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Process/Objects.hpp"
#include "Process/RegistryStore.hpp"
#include "Process/RegistryTree.hpp"
#include "Process/WindowsRegistry.hpp"
#include "Utility/UniquePointer.hpp"

using namespace process;
using namespace process::windowsabi;

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t align) override { return aligned_alloc(align, (size + align - 1) / align * align); }
  void deallocate(void* p) override { free(p); }
};

static std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct SystemSync final : ISystemSync {
  std::mutex m;
  std::condition_variable cv;
  static thread_local std::unique_lock<std::mutex>* mine;
  void lock() override { mine = new std::unique_lock<std::mutex>(m); }
  void unlock() override { delete mine; mine = nullptr; }
  bool wait(uint64_t ns) override {
    if (ns == ~uint64_t{0}) { cv.wait(*mine); return true; }
    return cv.wait_for(*mine, std::chrono::nanoseconds(ns)) == std::cv_status::no_timeout;
  }
  void notify_all() override { cv.notify_all(); }
};
thread_local std::unique_lock<std::mutex>* SystemSync::mine = nullptr;

static ConstByteView bytes_of(const char* text) {
  uint64_t n = 0;
  while (text[n]) ++n;
  return ConstByteView(reinterpret_cast<const uint8_t*>(text), n);
}

// The key at `path` (made level by level).
static uint32_t make_key(RegistryTree& tree, const char* path) {
  const StringView whole(path);
  uint32_t id = 0;
  bool created = false;
  for (uint64_t end = whole.find('\\');; end = whole.find('\\', end + 1)) {
    if (tree.resolve(RegistryTree::kRoot, end == StringView::npos ? whole : whole.first(end), true, 7, &id, &created) != kRegSuccess) return ~0u;
    if (end == StringView::npos) return id;
  }
}
static bool has_key(RegistryTree& tree, const char* path) {
  uint32_t id = 0;
  bool created = false;
  return tree.resolve(RegistryTree::kRoot, StringView(path), false, 1, &id, &created) == kRegSuccess;
}
static bool value_is(RegistryTree& tree, const char* path, const char* name, const char* data) {
  uint32_t id = 0;
  bool created = false;
  if (tree.resolve(RegistryTree::kRoot, StringView(path), false, 1, &id, &created) != kRegSuccess) return false;
  const RegValue* v = tree.find_value(id, StringView(name));
  return v && v->data.view().equals(bytes_of(data));
}

static bool read_file(filesystem::RamFileSystem& fs, const char* path, ByteBuffer* out) {
  UniquePointer<filesystem::IFile> file;
  if (fs.open_file(StringView(path), filesystem::OpenMode::Read, &file) != filesystem::Status::Ok || !file) return false;
  out->clear();
  if (!out->put_zeros(file->size())) return false;
  uint64_t got = 0;
  return file->read(0, out->writable_view(), &got) == filesystem::Status::Ok && got == file->size();
}
static bool write_file(filesystem::RamFileSystem& fs, const char* path, ConstByteView bytes) {
  UniquePointer<filesystem::IFile> file;
  if (fs.open_file(StringView(path), filesystem::OpenMode::CreateTruncate, &file) != filesystem::Status::Ok || !file) return false;
  uint64_t put = 0;
  return file->write(0, bytes, &put) == filesystem::Status::Ok && put == bytes.size();
}
static bool exists(filesystem::RamFileSystem& fs, const char* path) {
  filesystem::DirectoryEntry entry;
  return fs.stat(StringView(path), &entry) == filesystem::Status::Ok;
}

static constexpr const char* kElevator = "MACHINE\\SOFTWARE\\Classes\\CLSID\\{708860E0-F641-4611-8895-7D867DD3675B}";
static constexpr const char* kElevatorAppId = "MACHINE\\SOFTWARE\\Classes\\AppID\\{708860E0-F641-4611-8895-7D867DD3675B}";

// What is saved, what is not, and a restart that reads it back.
static void test_round_trip(TestAllocator& allocator) {
  filesystem::RamFileSystem fs(allocator);
  RegistryTree before(allocator, 100);
  const uint64_t start = before.changes();
  const uint32_t clsid = make_key(before, kElevator);
  CHECK(clsid != ~0u && before.changes() > start);
  CHECK(before.set_value(clsid, "AppID", 1, bytes_of("{708860E0}"), 12345) == kRegSuccess);
  const uint32_t appid = make_key(before, kElevatorAppId);
  CHECK(before.set_value(appid, "LocalService", 1, bytes_of("GoogleChromeElevationService"), 12346) == kRegSuccess);
  // a value the system defines, changed: at the next boot it is the system's again; one it does not define, set on a key it does, is kept
  CHECK(before.set_value(make_key(before, "MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"), "ProductName", 1, bytes_of("changed"), 5) == kRegSuccess);
  CHECK(before.set_value(make_key(before, "MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"), "RegisteredOwner", 1, bytes_of("someone"), 5) == kRegSuccess);
  // not saved: a volatile key and what is under it, the built-in volatile keys, a loaded hive
  const uint32_t running = make_key(before, "MACHINE\\SOFTWARE\\TheoryCraftOS\\COM\\Running");
  before.set_volatile(running, true);
  CHECK(before.set_value(running, "{1}", 4, bytes_of("1234"), 1) == kRegSuccess);
  CHECK(make_key(before, "MACHINE\\SOFTWARE\\TheoryCraftOS\\COM\\Running\\Below") != ~0u);
  CHECK(make_key(before, "MACHINE\\HARDWARE\\DESCRIPTION\\System") != ~0u);
  const uint32_t hive = make_key(before, "MACHINE\\Loaded");
  before.key(hive)->loaded_hive = true;
  // deleted keys are not saved either
  const uint32_t gone = make_key(before, "MACHINE\\SOFTWARE\\Gone");
  const uint64_t ahead = before.changes();
  CHECK(before.delete_key(gone) == kRegSuccess && before.changes() > ahead);

  ByteBuffer image(allocator);
  CHECK(before.save_persistent(&image) && RegistryTree::valid_image(image.view()));
  CHECK(write_saved_registry(fs, image.view()));
  CHECK(exists(fs, filesystem::layout::kWindowsRegistryFile) && !exists(fs, filesystem::layout::kWindowsRegistryNewFile));

  RegistryTree after(allocator, 200);      // the next boot
  CHECK(load_saved_registry(after, fs, 300) == RegistryLoad::Loaded);
  CHECK(value_is(after, kElevator, "AppID", "{708860E0}"));
  CHECK(value_is(after, kElevatorAppId, "LocalService", "GoogleChromeElevationService"));
  CHECK(!value_is(after, "MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName", "changed"));
  CHECK(value_is(after, "MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "RegisteredOwner", "someone"));
  CHECK(has_key(after, "MACHINE\\SOFTWARE\\TheoryCraftOS\\COM"));
  CHECK(!has_key(after, "MACHINE\\SOFTWARE\\TheoryCraftOS\\COM\\Running"));
  CHECK(has_key(after, "MACHINE\\HARDWARE"));      // (the tree makes it itself, empty and volatile)
  CHECK(!has_key(after, "MACHINE\\HARDWARE\\DESCRIPTION"));
  CHECK(!has_key(after, "MACHINE\\Loaded"));
  CHECK(!has_key(after, "MACHINE\\SOFTWARE\\Gone"));
  CHECK(has_key(after, "USER\\S-1-5-21-0-0-0-1000\\Environment"));      // the built-in keys are all there
  uint32_t id = 0;
  bool created = false;
  CHECK(after.resolve(RegistryTree::kRoot, kElevatorAppId, false, 1, &id, &created) == kRegSuccess && after.key(id)->write_time == 12346);      // (its time, not the boot's)
  // the volatile flag of the built-in keys stays after the merge
  CHECK(after.resolve(RegistryTree::kRoot, "MACHINE\\HARDWARE", false, 1, &id, &created) == kRegSuccess && after.key(id)->volatile_key);
  // saving the tree that was read back gives the same keys again
  ByteBuffer again(allocator);
  CHECK(after.save_persistent(&again) && RegistryTree::valid_image(again.view()));
}

// A power cut at each step of a save, and files that are not images.
static void test_handover(TestAllocator& allocator) {
  RegistryTree old_tree(allocator, 1), new_tree(allocator, 1);
  CHECK(old_tree.set_value(make_key(old_tree, "MACHINE\\SOFTWARE\\Which"), "is", 1, bytes_of("old"), 1) == kRegSuccess);
  CHECK(new_tree.set_value(make_key(new_tree, "MACHINE\\SOFTWARE\\Which"), "is", 1, bytes_of("new"), 1) == kRegSuccess);
  ByteBuffer old_image(allocator), new_image(allocator), got(allocator);
  CHECK(old_tree.save_persistent(&old_image) && new_tree.save_persistent(&new_image));
  auto loaded = [&](filesystem::RamFileSystem& fs, const char* expect) {
    RegistryTree t(allocator, 1);
    return load_saved_registry(t, fs, 1) == RegistryLoad::Loaded && value_is(t, "MACHINE\\SOFTWARE\\Which", "is", expect);
  };
  {      // nothing yet
    filesystem::RamFileSystem fs(allocator);
    CHECK(read_saved_registry(fs, &got) == RegistryLoad::NoFile && got.size() == 0);
  }
  {      // cut while the new file was written: the old one is read, the partial new one is dropped
    filesystem::RamFileSystem fs(allocator);
    CHECK(write_saved_registry(fs, old_image.view()));
    CHECK(write_file(fs, filesystem::layout::kWindowsRegistryNewFile, new_image.view().first(new_image.size() / 2)));
    CHECK(loaded(fs, "old"));
  }
  {      // cut after the new file was whole, before the old one went: the new one is read
    filesystem::RamFileSystem fs(allocator);
    CHECK(write_saved_registry(fs, old_image.view()));
    CHECK(write_file(fs, filesystem::layout::kWindowsRegistryNewFile, new_image.view()));
    CHECK(loaded(fs, "new"));
  }
  {      // cut after the old file went, before the rename: the new one is read, and the next save puts things right
    filesystem::RamFileSystem fs(allocator);
    CHECK(write_saved_registry(fs, old_image.view()));
    CHECK(fs.remove(StringView(filesystem::layout::kWindowsRegistryFile)) == filesystem::Status::Ok);
    CHECK(write_file(fs, filesystem::layout::kWindowsRegistryNewFile, new_image.view()));
    CHECK(loaded(fs, "new"));
    CHECK(write_saved_registry(fs, new_image.view()));
    CHECK(exists(fs, filesystem::layout::kWindowsRegistryFile) && !exists(fs, filesystem::layout::kWindowsRegistryNewFile));
    CHECK(loaded(fs, "new"));
  }
  {      // a damaged file: kept aside as .bad, and the registry starts with the built-in keys
    filesystem::RamFileSystem fs(allocator);
    CHECK(write_saved_registry(fs, old_image.view()));
    CHECK(write_file(fs, filesystem::layout::kWindowsRegistryFile, bytes_of("not a registry")));
    RegistryTree t(allocator, 1);
    CHECK(load_saved_registry(t, fs, 1) == RegistryLoad::Damaged);
    CHECK(!has_key(t, "MACHINE\\SOFTWARE\\Which") && has_key(t, "MACHINE\\SOFTWARE\\Classes"));
    CHECK(exists(fs, filesystem::layout::kWindowsRegistryBadFile) && !exists(fs, filesystem::layout::kWindowsRegistryFile));
    CHECK(read_file(fs, filesystem::layout::kWindowsRegistryBadFile, &got) && got.view().equals(bytes_of("not a registry")));
    CHECK(read_saved_registry(fs, &got) == RegistryLoad::NoFile);      // (the bad file is not read again)
  }
  {      // an empty file
    filesystem::RamFileSystem fs(allocator);
    CHECK(write_saved_registry(fs, old_image.view()));
    CHECK(write_file(fs, filesystem::layout::kWindowsRegistryFile, ConstByteView()));
    CHECK(read_saved_registry(fs, &got) == RegistryLoad::Damaged);
  }
}

// Every truncation of a saved image is refused; bit flips are refused or read without harm (the sanitizers watch).
static void test_damage(TestAllocator& allocator) {
  RegistryTree tree(allocator, 1);
  const uint32_t id = make_key(tree, kElevator);
  CHECK(tree.set_value(id, "AppID", 1, bytes_of("{708860E0}"), 1) == kRegSuccess);
  CHECK(tree.set_value(id, "", 3, bytes_of("\x01\x02\x03"), 1) == kRegSuccess);
  CHECK(make_key(tree, "USER\\S-1-5-21-0-0-0-1000\\Software\\Google\\Chrome") != ~0u);
  ByteBuffer image(allocator);
  CHECK(tree.save_persistent(&image));
  filesystem::RamFileSystem fs(allocator);
  CHECK(write_saved_registry(fs, image.view()));      // (makes the folders)
  uint32_t refused = 0;
  for (uint64_t n = 0; n < image.size(); ++n) {
    CHECK(!RegistryTree::valid_image(image.view().first(n)));
    if (n % 97 != 0) continue;      // (through the file system too, for some)
    CHECK(write_file(fs, filesystem::layout::kWindowsRegistryFile, image.view().first(n)));
    RegistryTree t(allocator, 1);
    if (load_saved_registry(t, fs, 1) == RegistryLoad::Damaged) ++refused;
  }
  CHECK(refused == (image.size() + 96) / 97);
  ByteBuffer flipped(allocator);
  uint32_t loaded = 0, damaged = 0;
  for (uint64_t bit = 0; bit < image.size() * 8; bit += 3) {
    flipped.clear();
    CHECK(flipped.append(image.view()));
    flipped.writable_view()[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    (void)fs.remove(StringView(filesystem::layout::kWindowsRegistryBadFile));
    CHECK(write_file(fs, filesystem::layout::kWindowsRegistryFile, flipped.view()));
    RegistryTree t(allocator, 1);
    const RegistryLoad result = load_saved_registry(t, fs, 1);
    if (result == RegistryLoad::Loaded) {
      ++loaded;
      ByteBuffer out(allocator);
      CHECK(t.save_persistent(&out) && RegistryTree::valid_image(out.view()));      // whatever was read is a tree that saves again
    } else {
      CHECK(result == RegistryLoad::Damaged);
      ++damaged;
    }
  }
  CHECK(loaded > 0 && damaged > 0);      // (a flip in a value's bytes still reads; one in a length does not)
}

// Programs set values on several threads while the writer thread saves; then a restart reads the last save.
static void test_threads(TestAllocator& allocator) {
  SystemSync sync;
  ObjectManager objects(sync);
  set_system_objects(&objects);
  filesystem::RamFileSystem fs(allocator);
  CHECK(registry_attach_storage(allocator, fs, 1700000000) == RegistryLoad::NoFile);
  constexpr int kThreads = 4, kValues = 300;
  std::atomic<bool> done{false};
  std::atomic<int> saves{0};
  std::thread writer([&] {
    while (!done.load()) {
      if (registry_save(true)) ++saves;
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });
  std::vector<std::thread> programs;
  for (int t = 0; t < kThreads; ++t)
    programs.emplace_back([&, t] {
      char key[64], name[32];
      for (int i = 0; i < kValues; ++i) {
        snprintf(key, sizeof(key), "SOFTWARE\\Program%d\\Group%d", t, i % 10);
        snprintf(name, sizeof(name), "Value%d", i);
        const uint32_t data = static_cast<uint32_t>(t * 100000 + i);
        CHECK(registry_set_value(allocator, RegistryHive::LocalMachine, StringView(key), StringView(name), 4, ConstByteView(reinterpret_cast<const uint8_t*>(&data), 4)));
        if (i % 50 == 0) CHECK(registry_save(false));      // (as RegFlushKey does, on the program's own thread)
      }
    });
  for (std::thread& p : programs) p.join();
  done = true;
  writer.join();
  CHECK(saves.load() > 0);
  CHECK(registry_detach_storage());
  set_system_objects(nullptr);

  RegistryTree after(allocator, 1);      // the next boot
  CHECK(load_saved_registry(after, fs, 1) == RegistryLoad::Loaded);
  int found = 0;
  for (int t = 0; t < kThreads; ++t)
    for (int i = 0; i < kValues; ++i) {
      char key[96], name[32];
      snprintf(key, sizeof(key), "MACHINE\\SOFTWARE\\Program%d\\Group%d", t, i % 10);
      snprintf(name, sizeof(name), "Value%d", i);
      uint32_t id = 0;
      bool created = false;
      if (after.resolve(RegistryTree::kRoot, StringView(key), false, 1, &id, &created) != kRegSuccess) continue;
      const RegValue* v = after.find_value(id, StringView(name));
      const uint32_t want = static_cast<uint32_t>(t * 100000 + i);
      if (v && v->type == 4 && v->data.view().equals(ConstByteView(reinterpret_cast<const uint8_t*>(&want), 4))) ++found;
    }
  CHECK(found == kThreads * kValues);
}

int main() {
  TestAllocator allocator;
  test_round_trip(allocator);
  test_handover(allocator);
  test_damage(allocator);
  test_threads(allocator);
  if (g_failed) {
    printf("RegistryStoreTest: %d FAILED\n", g_failed.load());
    return 1;
  }
  printf("RegistryStoreTest: all passed\n");
  return 0;
}
