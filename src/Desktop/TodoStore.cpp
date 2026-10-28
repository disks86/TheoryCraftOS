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

#include "Desktop/TodoStore.hpp"
#include "FileSystem/SystemLayout.hpp"

namespace desktop {

using database::DbParam;
using database::DbStatus;

const char* bucket_name(Bucket bucket) {
  static const char* const kNames[kBucketCount] = {"Today", "Tomorrow", "This week", "Next week", "Someday"};
  const uint8_t index = static_cast<uint8_t>(bucket);
  return index < kBucketCount ? kNames[index] : "";
}

TodoStore::TodoStore(allocators::IAllocator& allocator, filesystem::IFileSystem* files)
    : allocator_(allocator), database_(allocator, files, filesystem::layout::kTodoDatabase), connection_(allocator, database_), items_(allocator) {
  database_.open();   // (memory only if the file cannot be used: the list still works for this session)
  if (run("CREATE TABLE IF NOT EXISTS todo_items (item_id INTEGER PRIMARY KEY, bucket INTEGER, done INTEGER, title TEXT NOT NULL)", ArrayView<const DbParam>())) load();
}

bool TodoStore::run(StringView sql, ArrayView<const DbParam> params) { return connection_.execute(sql, params, nullptr) == DbStatus::Ok; }

bool TodoStore::valid(Bucket bucket, StringView title) { return static_cast<uint8_t>(bucket) < kBucketCount && !title.empty(); }

void TodoStore::load() {
  UniquePointer<database::IResultSet> rows;
  if (connection_.execute("SELECT item_id, bucket, done, title FROM todo_items ORDER BY item_id", &rows) != DbStatus::Ok || !rows) return;
  for (uint64_t r = 0; r < rows->row_count() && items_.count() < kMaxItems; ++r) {
    int64_t id = 0, bucket = 0, done = 0;
    if (!rows->get_int(r, 0, &id) || id <= 0 || id > 0x7FFFFFFF) continue;
    rows->get_int(r, 1, &bucket);
    rows->get_int(r, 2, &done);
    StringView title = rows->text(r, 3);
    if (title.size() > kMaxTitleBytes) title = title.first(kMaxTitleBytes);
    if (bucket < 0 || bucket >= static_cast<int64_t>(kBucketCount) || title.empty()) continue;   // (a damaged row)
    if (items_.count() > 0 && items_[items_.count() - 1].id >= id) continue;                      // (numbers are unique and ascending)
    Item item(allocator_);
    item.id = static_cast<uint32_t>(id);
    item.bucket = static_cast<Bucket>(bucket);
    item.done = done != 0;
    if (!item.title.assign(title) || !items_.push_back(static_cast<Item&&>(item))) continue;
    next_id_ = static_cast<uint32_t>(id) + 1;
  }
}

TodoStore::Item* TodoStore::find(uint32_t id) {
  for (Item& item : items_)
    if (item.id == id) return &item;
  return nullptr;
}
const TodoStore::Item* TodoStore::find(uint32_t id) const {
  for (const Item& item : items_)
    if (item.id == id) return &item;
  return nullptr;
}

bool TodoStore::copy(allocators::IAllocator&, const Item& from, Item* to) {
  to->id = from.id;
  to->bucket = from.bucket;
  to->done = from.done;
  return to->title.assign(from.title.view());
}

uint32_t TodoStore::count() const {
  kernel::ScopedMutex lock(mutex_);
  return static_cast<uint32_t>(items_.count());
}

uint32_t TodoStore::open_count() const {
  kernel::ScopedMutex lock(mutex_);
  uint32_t n = 0;
  for (const Item& item : items_) n += item.done ? 0 : 1;
  return n;
}

uint32_t TodoStore::open_count(Bucket bucket) const {
  kernel::ScopedMutex lock(mutex_);
  uint32_t n = 0;
  for (const Item& item : items_) n += !item.done && item.bucket == bucket ? 1 : 0;
  return n;
}

uint32_t TodoStore::done_count() const {
  kernel::ScopedMutex lock(mutex_);
  uint32_t n = 0;
  for (const Item& item : items_) n += item.done ? 1 : 0;
  return n;
}

uint64_t TodoStore::version() const {
  kernel::ScopedMutex lock(mutex_);
  return version_;
}

uint32_t TodoStore::add(Bucket bucket, StringView title) {
  if (title.size() > kMaxTitleBytes) title = title.first(kMaxTitleBytes);
  kernel::ScopedMutex lock(mutex_);
  if (!valid(bucket, title) || items_.count() >= kMaxItems || next_id_ == 0x7FFFFFFF) return 0;
  Item item(allocator_);
  item.id = next_id_;
  item.bucket = bucket;
  if (!item.title.assign(title)) return 0;
  const DbParam params[] = {DbParam::from_int(item.id), DbParam::from_int(static_cast<int64_t>(bucket)), DbParam::from_int(0), DbParam::from_text(title)};
  if (!run("INSERT INTO todo_items VALUES (?, ?, ?, ?)", ArrayView<const DbParam>(params, 4))) return 0;
  if (!items_.push_back(static_cast<Item&&>(item))) {
    const DbParam gone[] = {DbParam::from_int(next_id_)};
    run("DELETE FROM todo_items WHERE item_id = ?", ArrayView<const DbParam>(gone, 1));
    return 0;
  }
  ++version_;
  return next_id_++;
}

bool TodoStore::update(uint32_t id, Bucket bucket, StringView title) {
  if (title.size() > kMaxTitleBytes) title = title.first(kMaxTitleBytes);
  kernel::ScopedMutex lock(mutex_);
  Item* item = find(id);
  if (!item || !valid(bucket, title)) return false;
  String copy(allocator_);
  if (!copy.assign(title)) return false;
  const DbParam params[] = {DbParam::from_int(static_cast<int64_t>(bucket)), DbParam::from_text(title), DbParam::from_int(id)};
  if (!run("UPDATE todo_items SET bucket = ?, title = ? WHERE item_id = ?", ArrayView<const DbParam>(params, 3))) return false;
  item->bucket = bucket;
  item->title.assign(copy.view());
  ++version_;
  return true;
}

bool TodoStore::set_bucket(uint32_t id, Bucket bucket) {
  kernel::ScopedMutex lock(mutex_);
  Item* item = find(id);
  if (!item || static_cast<uint8_t>(bucket) >= kBucketCount) return false;
  const DbParam params[] = {DbParam::from_int(static_cast<int64_t>(bucket)), DbParam::from_int(id)};
  if (!run("UPDATE todo_items SET bucket = ? WHERE item_id = ?", ArrayView<const DbParam>(params, 2))) return false;
  item->bucket = bucket;
  ++version_;
  return true;
}

bool TodoStore::set_done(uint32_t id, bool done) {
  kernel::ScopedMutex lock(mutex_);
  Item* item = find(id);
  if (!item) return false;
  const DbParam params[] = {DbParam::from_int(done ? 1 : 0), DbParam::from_int(id)};
  if (!run("UPDATE todo_items SET done = ? WHERE item_id = ?", ArrayView<const DbParam>(params, 2))) return false;
  item->done = done;
  ++version_;
  return true;
}

bool TodoStore::remove(uint32_t id) {
  kernel::ScopedMutex lock(mutex_);
  for (uint64_t i = 0; i < items_.count(); ++i) {
    if (items_[i].id != id) continue;
    const DbParam params[] = {DbParam::from_int(id)};
    if (!run("DELETE FROM todo_items WHERE item_id = ?", ArrayView<const DbParam>(params, 1))) return false;
    items_.remove_at(i);
    ++version_;
    return true;
  }
  return false;
}

uint32_t TodoStore::remove_done() {
  kernel::ScopedMutex lock(mutex_);
  uint32_t n = 0;
  for (const Item& item : items_) n += item.done ? 1 : 0;
  if (n == 0 || !run("DELETE FROM todo_items WHERE done <> 0", ArrayView<const DbParam>())) return 0;
  for (uint64_t i = items_.count(); i > 0; --i)
    if (items_[i - 1].done) items_.remove_at(i - 1);
  ++version_;
  return n;
}

bool TodoStore::get(uint32_t id, Item* out) const {
  kernel::ScopedMutex lock(mutex_);
  const Item* item = find(id);
  return item && copy(allocator_, *item, out);
}

void TodoStore::open_items(Bucket bucket, List<Item>* out) const {
  kernel::ScopedMutex lock(mutex_);
  for (const Item& item : items_) {
    if (item.done || item.bucket != bucket) continue;
    Item c(allocator_);
    if (copy(allocator_, item, &c)) out->push_back(static_cast<Item&&>(c));
  }
}

void TodoStore::open_items(List<Item>* out) const {
  for (uint32_t b = 0; b < kBucketCount; ++b) open_items(static_cast<Bucket>(b), out);
}

void TodoStore::done_items(List<Item>* out) const {
  kernel::ScopedMutex lock(mutex_);
  for (uint64_t i = items_.count(); i > 0; --i) {
    const Item& item = items_[i - 1];
    if (!item.done) continue;
    Item c(allocator_);
    if (copy(allocator_, item, &c)) out->push_back(static_cast<Item&&>(c));
  }
}

}  // namespace desktop
