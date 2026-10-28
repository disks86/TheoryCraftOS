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

// Host-side test of the to-do list's storage: items are added into buckets, changed, marked done (they leave the open list), restored and deleted, and survive a
// restart (a new store on the same filesystem). Damaged files do not crash it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Desktop/TodoStore.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Database/Local/LocalDatabase.hpp"

using namespace desktop;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static bool read_all(filesystem::IFileSystem& fs, const char* path, ByteBuffer* data) {
  UniquePointer<filesystem::IFile> file;
  if (fs.open_file(path, filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) return false;
  data->clear();
  uint8_t chunk[4096];
  for (uint64_t offset = 0;;) {
    uint64_t got = 0;
    if (file->read(offset, ByteView(chunk, sizeof chunk), &got) != filesystem::Status::Ok) return false;
    if (got == 0) return true;
    data->append(ConstByteView(chunk, got));
    offset += got;
  }
}

static bool write_all(filesystem::IFileSystem& fs, const char* path, ConstByteView data) {
  UniquePointer<filesystem::IFile> file;
  uint64_t put = 0;
  return fs.open_file(path, filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok && file->write(0, data, &put) == filesystem::Status::Ok && put == data.size();
}

int main() {
  TestAllocator a;
  CHECK(StringView(bucket_name(Bucket::ThisWeek)).equals("This week") && StringView(bucket_name(Bucket::Someday)).equals("Someday"));
  {
    filesystem::RamFileSystem files(a);
    uint32_t milk = 0, taxes = 0, gym = 0;
    {
      TodoStore store(a, &files);
      CHECK(store.persistent() && store.count() == 0 && store.open_count() == 0);
      milk = store.add(Bucket::Today, "Buy milk");
      taxes = store.add(Bucket::NextWeek, "File taxes");
      gym = store.add(Bucket::Today, "Gym");
      const uint32_t later = store.add(Bucket::Someday, "Learn Rust");
      CHECK(milk && taxes && gym && later && milk != taxes && store.count() == 4 && store.open_count() == 4);
      CHECK(store.add(Bucket::Today, "") == 0 && store.add(static_cast<Bucket>(9), "Bad bucket") == 0 && store.count() == 4);

      List<TodoStore::Item> items(a);
      store.open_items(Bucket::Today, &items);
      CHECK(items.count() == 2 && items[0].id == milk && items[1].id == gym);   // in the order added
      items.clear();
      store.open_items(&items);
      CHECK(items.count() == 4 && items[0].id == milk && items[1].id == gym && items[2].id == taxes && items[3].id == later);   // Today ... Someday
      CHECK(store.open_count(Bucket::Today) == 2 && store.open_count(Bucket::Tomorrow) == 0 && store.open_count(Bucket::NextWeek) == 1);

      // Done items leave the open list and come back when restored.
      const uint64_t before = store.version();
      CHECK(store.set_done(milk, true) && store.version() > before);
      items.clear();
      store.open_items(Bucket::Today, &items);
      CHECK(items.count() == 1 && items[0].id == gym && store.open_count() == 3 && store.done_count() == 1 && store.count() == 4);
      CHECK(store.set_done(gym, true));
      items.clear();
      store.done_items(&items);
      CHECK(items.count() == 2 && items[0].id == gym && items[1].id == milk);   // the latest first
      CHECK(store.set_done(milk, false) && store.open_count(Bucket::Today) == 1 && store.done_count() == 1);
      CHECK(!store.set_done(999, true));

      // Moving and renaming.
      CHECK(store.set_bucket(taxes, Bucket::Tomorrow) && store.open_count(Bucket::Tomorrow) == 1 && store.open_count(Bucket::NextWeek) == 0);
      CHECK(!store.set_bucket(taxes, static_cast<Bucket>(7)) && !store.set_bucket(999, Bucket::Today));
      CHECK(store.update(taxes, Bucket::ThisWeek, "File the taxes"));
      CHECK(!store.update(taxes, Bucket::ThisWeek, "") && !store.update(999, Bucket::Today, "x"));
      TodoStore::Item item(a);
      CHECK(store.get(taxes, &item) && item.title.view().equals("File the taxes") && item.bucket == Bucket::ThisWeek && !item.done);
      CHECK(store.get(gym, &item) && item.done);
      CHECK(!store.get(999, &item));

      // A long title is cut.
      String longer(a);
      for (uint32_t i = 0; i < TodoStore::kMaxTitleBytes + 50; ++i) longer.push_back('x');
      const uint32_t cut = store.add(Bucket::Today, longer.view());
      CHECK(cut && store.get(cut, &item) && item.title.size() == TodoStore::kMaxTitleBytes);
      CHECK(store.remove(cut) && !store.remove(cut));
    }
    {
      // After a restart everything is as it was, and new numbers do not reuse old ones.
      TodoStore store(a, &files);
      CHECK(store.count() == 4 && store.open_count() == 3 && store.done_count() == 1);
      TodoStore::Item item(a);
      CHECK(store.get(taxes, &item) && item.title.view().equals("File the taxes") && item.bucket == Bucket::ThisWeek);
      CHECK(store.get(gym, &item) && item.done);
      CHECK(store.get(milk, &item) && !item.done && item.bucket == Bucket::Today);
      const uint32_t fresh = store.add(Bucket::Today, "New");
      CHECK(fresh > taxes && fresh > gym);
      CHECK(store.remove_done() == 1 && store.done_count() == 0 && store.count() == 4 && store.remove_done() == 0);
    }
    {
      TodoStore store(a, &files);
      CHECK(store.count() == 4 && store.done_count() == 0);
    }
  }
  {
    // Without a filesystem the list still works, in memory.
    TodoStore store(a, nullptr);
    CHECK(!store.persistent());
    CHECK(store.add(Bucket::Today, "Only in memory") != 0 && store.count() == 1);
  }
  {
    // A damaged file is set aside and the list starts empty; flipped bits or a cut file never crash it.
    filesystem::RamFileSystem files(a);
    files.create_directory("/AppStore");
    files.create_directory("/AppStore/Todo");
    {
      TodoStore store(a, &files);
      store.add(Bucket::Today, "Kept");
      store.set_done(store.add(Bucket::Someday, "Finished"), true);
    }
    ByteBuffer data(a);
    CHECK(read_all(files, filesystem::layout::kTodoDatabase, &data) && data.size() > 16);
    const uint64_t cuts[] = {3, data.size() / 2, data.size() - 1};
    for (uint64_t cut : cuts) {   // truncated
      ByteBuffer copy(a);
      copy.append(data.view().first(cut));
      write_all(files, filesystem::layout::kTodoDatabase, copy.view());
      TodoStore store(a, &files);
      CHECK(store.count() <= 2);
      CHECK(store.add(Bucket::Today, "Works after damage") != 0);
    }
    for (uint64_t bit = 0; bit < 64; ++bit) {   // bit flips
      ByteBuffer copy(a);
      copy.append(data.view());
      copy.writable_view()[(bit * 7) % copy.size()] ^= static_cast<uint8_t>(1u << (bit % 8));
      write_all(files, filesystem::layout::kTodoDatabase, copy.view());
      TodoStore store(a, &files);
      CHECK(store.count() <= 3);
    }
  }
  CHECK(a.live == 0);
  if (g_failed) {
    printf("TodoStoreTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("TodoStoreTest: all passed\n");
  return 0;
}
