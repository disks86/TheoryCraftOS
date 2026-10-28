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

// The to-do items of the to-do application, kept in the application's own storage: one database file, /AppStore/Todo/todo.db
// (filesystem::layout::kTodoDatabase), so they are still there after a restart.
//
//   todo_items (item_id INTEGER PRIMARY KEY, bucket INTEGER, done INTEGER, title TEXT)
//
// An item is assigned to a Bucket (Today, Tomorrow, This week, Next week or Someday) and is either open or done. The buckets are chosen by the person,
// they do not move on their own when the day changes. Items come out in the order they were added. Every change is on disk when the call returns.
// Safe to use from any thread.
#pragma once
#include "Database/Local/LocalDatabase.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace desktop {

enum class Bucket : uint8_t { Today, Tomorrow, ThisWeek, NextWeek, Someday };
constexpr uint32_t kBucketCount = 5;
// The name of a bucket as the window shows it.
const char* bucket_name(Bucket bucket);

class TodoStore {
 public:
  static constexpr uint32_t kMaxItems = 5000, kMaxTitleBytes = 200;

  struct Item {
    explicit Item(allocators::IAllocator& allocator) : title(allocator) {}
    uint32_t id = 0;
    Bucket bucket = Bucket::Today;
    bool done = false;
    String title;
  };

  // `files` is the filesystem the database lives on (nullptr: the items are only kept in memory). It must outlive the store.
  TodoStore(allocators::IAllocator& allocator, filesystem::IFileSystem* files);

  // True while the items are kept in the file.
  bool persistent() const { return database_.persistent(); }
  uint32_t count() const;            // all items
  uint32_t open_count() const;       // those not done
  uint32_t open_count(Bucket bucket) const;
  uint32_t done_count() const;
  // Counts changes (for a window that shows them).
  uint64_t version() const;

  // Adds an open item; returns its number, 0 if it was refused (empty title, too many items, or the database could not be written). The title is cut at kMaxTitleBytes.
  uint32_t add(Bucket bucket, StringView title);
  // Changes the title and bucket of an item. False if there is none with that number, the title is empty or on failure (nothing changes then).
  bool update(uint32_t id, Bucket bucket, StringView title);
  bool set_bucket(uint32_t id, Bucket bucket);
  // Marks an item done (it leaves the open list) or open again.
  bool set_done(uint32_t id, bool done);
  bool remove(uint32_t id);
  // Deletes every item that is done; returns how many.
  uint32_t remove_done();
  bool get(uint32_t id, Item* out) const;

  // The open items of `bucket`, or of all buckets in the order Today ... Someday (each in the order added), or the items that are done (the latest first).
  void open_items(Bucket bucket, List<Item>* out) const;
  void open_items(List<Item>* out) const;
  void done_items(List<Item>* out) const;

 private:
  static bool valid(Bucket bucket, StringView title);
  bool run(StringView sql, ArrayView<const database::DbParam> params);
  void load();
  Item* find(uint32_t id);
  const Item* find(uint32_t id) const;
  static bool copy(allocators::IAllocator& allocator, const Item& from, Item* to);

  allocators::IAllocator& allocator_;
  database::local::LocalDatabase database_;
  database::local::LocalConnection connection_;
  mutable kernel::Mutex mutex_;
  List<Item> items_;   // in order of number
  uint32_t next_id_ = 1;
  uint64_t version_ = 0;
};

}  // namespace desktop
