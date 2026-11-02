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
#include "Process/IoRequest.hpp"
#include "Utility/AllocatedView.hpp"

namespace process::io {

IoRequestTable::~IoRequestTable() {
  for (IoRequest& r : requests_) release(r.data);
}

void IoRequestTable::release(ByteView& data) {
  if (!data.empty()) release_view(allocator_, data);
  data = ByteView();
}

IoRequest* IoRequestTable::add(const IoRequest& request) {
  if (requests_.count() >= kMaxRequests) return nullptr;
  IoRequest* made = requests_.insert(request);
  if (!made) return nullptr;
  made->id = next_id_++;
  if (next_id_ == 0) next_id_ = 1;
  made->completed = false;
  made->cancel_requested = false;
  made->status = 0;
  made->information = 0;
  made->data = ByteView();
  ++pending_;
  return made;
}

IoRequest* IoRequestTable::find(uint32_t id) {
  if (id == 0) return nullptr;
  for (IoRequest& r : requests_) if (r.id == id) return &r;
  return nullptr;
}

bool IoRequestTable::complete(uint32_t id, uint32_t status, uint64_t information, ByteView data) {
  IoRequest* r = find(id);
  if (!r || r->completed) return false;
  if (!done_.enqueue(id)) return false;
  r->completed = true;
  r->status = status;
  r->information = information;
  r->sequence = ++sequence_;
  r->data = data;
  --pending_;
  __atomic_add_fetch(&ready_, 1, __ATOMIC_ACQ_REL);
  return true;
}

uint32_t IoRequestTable::take_completed(uint64_t owner, IoRequest* out, uint32_t capacity) {
  uint32_t taken = 0;
  // Walk the queue of finished ids once, keeping the ones that are not this owner's (or do not fit), in order.
  const uint64_t n = done_.count();
  for (uint64_t i = 0; i < n; ++i) {
    uint32_t id = 0;
    done_.dequeue(&id);
    IoRequest* r = find(id);
    if (!r || !r->completed) continue;      // (removed since)
    if (r->owner == owner && taken < capacity) {
      out[taken++] = *r;
      r->data = ByteView();
      requests_.erase(r);
      __atomic_sub_fetch(&ready_, 1, __ATOMIC_ACQ_REL);
    } else {
      done_.enqueue(id);      // (cannot fail: the slot just freed is there)
    }
  }
  return taken;
}

uint32_t IoRequestTable::remove_owner(uint64_t owner, IoRequest* out, uint32_t capacity) {
  uint32_t removed = 0;
  ObjectCursor cursor;
  bool more = requests_.first(&cursor);
  while (more && removed < capacity) {
    IoRequest* r = requests_.element(cursor);
    if (r->owner != owner) { more = requests_.next(&cursor); continue; }
    if (r->completed) __atomic_sub_fetch(&ready_, 1, __ATOMIC_ACQ_REL);
    else --pending_;
    release(r->data);
    out[removed++] = *r;
    more = requests_.erase(&cursor);
  }
  return removed;
}

void IoRequestTable::erase(uint32_t id) {
  IoRequest* r = find(id);
  if (!r) return;
  if (r->completed) __atomic_sub_fetch(&ready_, 1, __ATOMIC_ACQ_REL);
  else --pending_;
  release(r->data);
  requests_.erase(r);
}

bool CompletionQueue::post(const Packet& packet) {
  if (packets_.count() >= kMaxPackets) return false;
  return packets_.enqueue(packet);
}

uint32_t CompletionQueue::remove_many(Packet* out, uint32_t capacity) {
  uint32_t n = 0;
  while (n < capacity && packets_.dequeue(&out[n])) ++n;
  return n;
}

}  // namespace process::io
