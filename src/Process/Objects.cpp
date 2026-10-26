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
#include "Process/Objects.hpp"

namespace process {

namespace {
ObjectManager* g_objects = nullptr;

constexpr uint32_t kSuccess = 0, kNameExists = 0x40000000u, kTypeMismatch = 0xC0000024u, kNotFound = 0xC0000034u, kNoResources = 0xC000009Au, kNotOwner = 0xC0000046u,
                   kLimit = 0xC0000047u, kInvalid = 0xC000000Du, kInvalidHandle = 0xC0000008u, kNameTooLong = 0xC0000106u;

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
bool same_name(const char* a, uint32_t a_size, StringView b) {
  if (a_size != b.size()) return false;
  for (uint32_t i = 0; i < a_size; ++i) if (lower(a[i]) != lower(b[i])) return false;
  return true;
}

class Locked {
 public:
  explicit Locked(ISystemSync& s) : s_(s) { s_.lock(); }
  ~Locked() { s_.unlock(); }
  Locked(const Locked&) = delete;
  Locked& operator=(const Locked&) = delete;

 private:
  ISystemSync& s_;
};
}  // namespace

ObjectManager* system_objects() { return g_objects; }
void set_system_objects(ObjectManager* manager) { g_objects = manager; }

void ObjectManager::notify() {
  Locked hold(sync_);
  sync_.notify_all();
}

ObjectManager::Object* ObjectManager::find_locked(StringView name) {
  if (name.empty()) return nullptr;
  for (Object& o : objects_) if (o.kind != ObjectKind::None && o.name_size != 0 && same_name(o.name, o.name_size, name)) return &o;
  return nullptr;
}

ObjectManager::Created ObjectManager::create(ObjectKind kind, StringView name, bool manual_reset, bool initial_state, int32_t initial, int32_t maximum, uint64_t owner) {
  Created result;
  if (kind == ObjectKind::None || name.size() >= kNameBytes) { result.status = name.size() >= kNameBytes ? kNameTooLong : kInvalid; return result; }
  if (kind == ObjectKind::Semaphore && (maximum <= 0 || initial < 0 || initial > maximum)) { result.status = kInvalid; return result; }
  Locked hold(sync_);
  if (Object* existing = find_locked(name)) {
    if (existing->kind != kind) { result.status = kTypeMismatch; return result; }
    ++existing->references;
    result.id = static_cast<uint32_t>(existing - objects_) + 1;
    result.status = kNameExists;
    return result;
  }
  for (uint32_t i = 0; i < kCapacity; ++i) {
    Object& o = objects_[i];
    if (o.kind != ObjectKind::None) continue;
    o = Object();
    o.kind = kind;
    o.references = 1;
    for (uint32_t k = 0; k < name.size(); ++k) o.name[k] = name[k];
    o.name_size = static_cast<uint32_t>(name.size());
    o.manual_reset = manual_reset;
    if (kind == ObjectKind::Event) o.signaled = initial_state;
    if (kind == ObjectKind::Mutex && initial_state) { o.owner = owner; o.recursion = 1; }
    if (kind == ObjectKind::Semaphore) { o.count = initial; o.maximum = maximum; }
    result.id = i + 1;
    result.status = kSuccess;
    return result;
  }
  result.status = kNoResources;
  return result;
}

ObjectManager::Created ObjectManager::create_process(uint64_t pid) {
  Created result;
  Locked hold(sync_);
  for (uint32_t i = 0; i < kCapacity; ++i) {
    Object& o = objects_[i];
    if (o.kind != ObjectKind::None) continue;
    o = Object();
    o.kind = ObjectKind::Process;
    o.references = 1;
    o.manual_reset = true;
    o.owner = pid;
    o.count = 259;      // STILL_ACTIVE
    result.id = i + 1;
    return result;
  }
  result.status = kNoResources;
  return result;
}

void ObjectManager::process_exited(uint64_t pid, uint32_t code) {
  Locked hold(sync_);
  for (Object& o : objects_) {
    if (o.kind != ObjectKind::Process || o.owner != pid || o.signaled) continue;
    o.signaled = true;
    o.count = static_cast<int32_t>(code);
  }
  sync_.notify_all();
}

bool ObjectManager::query_process(uint32_t id, bool* exited, uint32_t* code, uint64_t* pid) {
  Locked hold(sync_);
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::Process) return false;
  *exited = o->signaled;
  *code = static_cast<uint32_t>(o->count);
  *pid = o->owner;
  return true;
}

ObjectManager::Created ObjectManager::open(ObjectKind kind, StringView name) {
  Created result;
  Locked hold(sync_);
  Object* o = find_locked(name);
  if (!o) { result.status = kNotFound; return result; }
  if (o->kind != kind) { result.status = kTypeMismatch; return result; }
  ++o->references;
  result.id = static_cast<uint32_t>(o - objects_) + 1;
  return result;
}

void ObjectManager::add_ref(uint32_t id) {
  Locked hold(sync_);
  if (Object* o = at(id)) ++o->references;
}

void ObjectManager::release(uint32_t id) {
  Locked hold(sync_);
  Object* o = at(id);
  if (!o || o->references == 0) return;
  if (--o->references == 0) *o = Object();
}

ObjectKind ObjectManager::kind(uint32_t id) {
  Locked hold(sync_);
  Object* o = at(id);
  return o ? o->kind : ObjectKind::None;
}

uint32_t ObjectManager::set_event(uint32_t id, int32_t* previous) {
  Locked hold(sync_);
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::Event) return kInvalidHandle;
  if (previous) *previous = o->signaled ? 1 : 0;
  o->signaled = true;
  sync_.notify_all();
  return kSuccess;
}

uint32_t ObjectManager::reset_event(uint32_t id, int32_t* previous) {
  Locked hold(sync_);
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::Event) return kInvalidHandle;
  if (previous) *previous = o->signaled ? 1 : 0;
  o->signaled = false;
  return kSuccess;
}

uint32_t ObjectManager::pulse_event(uint32_t id, int32_t* previous) {
  Locked hold(sync_);
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::Event) return kInvalidHandle;
  if (previous) *previous = o->signaled ? 1 : 0;
  o->signaled = false;
  o->pulses = o->manual_reset ? o->waiters : (o->waiters > 0 ? 1 : 0);      // those waiting now go through, then it is not set
  sync_.notify_all();
  return kSuccess;
}

uint32_t ObjectManager::release_mutex(uint32_t id, uint64_t owner, int32_t* previous_count) {
  Locked hold(sync_);
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::Mutex) return kInvalidHandle;
  if (o->owner != owner || o->recursion <= 0) return kNotOwner;
  if (previous_count) *previous_count = o->recursion;
  if (--o->recursion == 0) {
    o->owner = 0;
    sync_.notify_all();
  }
  return kSuccess;
}

uint32_t ObjectManager::release_semaphore(uint32_t id, int32_t count, int32_t* previous) {
  Locked hold(sync_);
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::Semaphore) return kInvalidHandle;
  if (count <= 0) return kInvalid;
  if (o->count > o->maximum - count) return kLimit;
  if (previous) *previous = o->count;
  o->count += count;
  sync_.notify_all();
  return kSuccess;
}

void ObjectManager::abandon_thread(uint64_t thread) {
  Locked hold(sync_);
  bool any = false;
  for (Object& o : objects_) {
    if (o.kind != ObjectKind::Mutex || o.owner != thread || thread == 0) continue;
    o.owner = 0;
    o.recursion = 0;
    o.abandoned = true;
    any = true;
  }
  if (any) sync_.notify_all();
}

void ObjectManager::abandon_process(uint64_t process) {
  Locked hold(sync_);
  bool any = false;
  for (Object& o : objects_) {
    if (o.kind != ObjectKind::Mutex || o.owner == 0 || (o.owner & 0xFFFFF) != process) continue;      // (a thread id carries its process's id in the low 20 bits)
    o.owner = 0;
    o.recursion = 0;
    o.abandoned = true;
    any = true;
  }
  if (any) sync_.notify_all();
}

bool ObjectManager::ready_locked(uint32_t id, uint64_t waiter) {
  Object* o = at(id);
  if (!o) return true;      // gone: nothing to wait for
  switch (o->kind) {
    case ObjectKind::Event: return o->signaled || o->pulses > 0;
    case ObjectKind::Process: return o->signaled;
    case ObjectKind::Mutex: return o->owner == 0 || o->owner == waiter;
    case ObjectKind::Semaphore: return o->count > 0;
    default: return true;
  }
}

void ObjectManager::acquire_locked(uint32_t id, uint64_t waiter, bool* abandoned) {
  *abandoned = false;
  Object* o = at(id);
  if (!o) return;
  switch (o->kind) {
    case ObjectKind::Event:
      if (o->signaled) { if (!o->manual_reset) o->signaled = false; }
      else if (o->pulses > 0) --o->pulses;
      break;
    case ObjectKind::Mutex:
      if (o->owner == waiter) { ++o->recursion; }
      else {
        o->owner = waiter;
        o->recursion = 1;
        *abandoned = o->abandoned;
        o->abandoned = false;
      }
      break;
    case ObjectKind::Semaphore:
      if (o->count > 0) --o->count;
      break;
    default: break;
  }
}

void ObjectManager::waiting_locked(uint32_t id, int32_t delta) {
  if (Object* o = at(id)) o->waiters += delta;
}

}  // namespace process
