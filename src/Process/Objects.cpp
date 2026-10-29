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

#include <new>

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
  for (Object& o : objects_) {
    if (o.kind == ObjectKind::None || o.name_size == 0) continue;
    if (o.kind == ObjectKind::Section) {      // (the names of file mappings are matched exactly; the other objects' ignore case)
      if (o.name_size == name.size() && StringView(o.name, o.name_size) == name) return &o;
    } else if (same_name(o.name, o.name_size, name)) {
      return &o;
    }
  }
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
    if (kind == ObjectKind::Job) {
      if (!jobs_ && allocator_) {
        void* memory = allocator_->allocate(sizeof(Job) * kJobMaxJobs);
        if (memory) { Job* made = new (memory) Job[kJobMaxJobs]; __atomic_store_n(&jobs_, made, __ATOMIC_RELEASE); }
      }
      int32_t slot = -1;
      for (uint32_t j = 0; jobs_ && j < kJobMaxJobs && slot < 0; ++j) if (!jobs_[j].used) slot = int32_t(j);
      if (slot < 0) { o = Object(); result.status = kNoResources; return result; }
      jobs_[slot] = Job();
      jobs_[slot].used = true;
      jobs_[slot].object = i + 1;
      o.job = slot;
    }
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

ObjectManager::Created ObjectManager::open_process(uint64_t pid, bool create) {
  {
    Locked hold(sync_);
    for (uint32_t i = 0; i < kCapacity; ++i) {
      Object& o = objects_[i];
      if (o.kind != ObjectKind::Process || o.owner != pid) continue;
      ++o.references;
      Created found;
      found.id = i + 1;
      return found;
    }
  }
  if (!create) { Created none; none.status = kNotFound; return none; }
  return create_process(pid);
}

void ObjectManager::process_exited(uint64_t pid, uint32_t code) {
  Locked hold(sync_);
  job_forget_locked(pid);
  for (Object& o : objects_) {
    if (o.kind != ObjectKind::Process || o.signaled) continue;
    const bool thread_of_pid = o.owner >= (uint64_t{1} << 20) && (o.owner & ((uint64_t{1} << 20) - 1)) == pid;      // (a handle to a thread of the process: thread ids are (counter << 20) | process id)
    if (o.owner != pid && !thread_of_pid) continue;
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

class ObjectManager::SectionRef final : public SharedMemory {
 public:
  void attach(ObjectManager* manager, uint32_t index) { manager_ = manager; index_ = index; }
  void retain() override { manager_->section_count(index_, 1); }
  void release() override { manager_->section_count(index_, -1); }
  void flush() override { manager_->flush_section_at(index_); }

 private:
  ObjectManager* manager_ = nullptr;
  uint32_t index_ = 0;
};

struct ObjectManager::Section {
  bool used = false, writable = false, file_backed = false;
  uint32_t page = 0, attributes = 0;
  bool image_mapped = false;
  bool reserve = false;                     // SEC_RESERVE: its pages are committed by VirtualAlloc, in the section and so in every view
  uint8_t committed[2048] = {};             // one bit a page of the section
  uint8_t* storage = nullptr;
  uint64_t size = 0, rounded = 0, reported_size = 0;
  int32_t mappings = 0;
  uint32_t object = 0;                      // its slot in objects_
  filesystem::IFileSystem* fs = nullptr;
  char path[512] = {};
  uint32_t path_size = 0;
  SectionRef ref;
};

// ---- pipes

struct ObjectManager::Pipe {
  bool used = false;
  uint8_t* data[2] = {};            // data[d]: what end d wrote and the other end has not read yet
  uint32_t capacity[2] = {}, head[2] = {}, count[2] = {};
  uint32_t end_refs[2] = {};
  bool anonymous = false, connected = false, listening = false, client_seen = false;
  uint32_t access = 0, max_instances = 0, generation = 0;
  // The end an end_code names; false for a client end of an earlier connection.
  bool end_of(uint32_t code, uint32_t* end) const {
    *end = code & 1;
    return *end == 0 || (code >> 1) == generation;
  }
};

namespace {
constexpr uint32_t kPipeBusy = 0xC00000AEu;
constexpr uint32_t kDefaultPipeBuffer = 4096, kMaxPipeBuffer = 1u << 20;
}  // namespace

struct ObjectManager::Mailslot {
  bool used = false;
  uint8_t* data = nullptr;
  uint32_t capacity = 0, head = 0, count = 0;      // a byte ring buffer, like a Pipe's one direction
  uint32_t message_lengths[kMaxMailslotMessages] = {};      // a ring of pending messages' byte lengths, oldest first
  uint32_t message_head = 0, message_count = 0;
  uint32_t max_message_size = 0;      // 0: no cap beyond the buffer's own capacity
  uint64_t read_timeout_100ns = ~uint64_t{0};
};

ObjectManager::~ObjectManager() {
  if (pipes_) {
    for (uint32_t i = 0; i < kMaxPipes; ++i) {
      for (uint8_t* d : pipes_[i].data) if (d && allocator_) allocator_->deallocate(d);
    }
    if (allocator_) allocator_->deallocate(pipes_);      // (Pipe is plain data: nothing to destroy)
  }
  if (jobs_ && allocator_) allocator_->deallocate(jobs_);
  if (locks_ && allocator_) allocator_->deallocate(locks_);
  if (shares_ && allocator_) allocator_->deallocate(shares_);
  if (sections_) {
    for (uint32_t i = 0; i < kMaxSections; ++i) if (sections_[i].storage && allocator_) allocator_->deallocate(sections_[i].storage);
    if (allocator_) allocator_->deallocate(sections_);
  }
  if (watches_ && allocator_) allocator_->deallocate(watches_);
  if (mailslots_) {
    for (uint32_t i = 0; i < kMaxMailslots; ++i) if (mailslots_[i].data && allocator_) allocator_->deallocate(mailslots_[i].data);
    if (allocator_) allocator_->deallocate(mailslots_);
  }
}

ObjectManager::Pipe* ObjectManager::pipe_of(uint32_t id) {
  Object* o = at(id);
  return o && o->kind == ObjectKind::Pipe && o->pipe >= 0 && pipes_ ? &pipes_[o->pipe] : nullptr;
}

void ObjectManager::free_pipe_locked(Object& o) {
  if (o.pipe < 0 || !pipes_) return;
  Pipe& p = pipes_[o.pipe];
  for (uint8_t*& d : p.data) {
    if (d && allocator_) allocator_->deallocate(d);
    d = nullptr;
  }
  p = Pipe();
}

ObjectManager::Created ObjectManager::create_pipe(StringView name, uint32_t out_size, uint32_t in_size, uint32_t max_instances, uint32_t access) {
  Created result;
  if (!allocator_ || name.size() >= kNameBytes || access < kPipeInbound || access > kPipeDuplex) { result.status = !allocator_ ? 0xC0000002u : kInvalid; return result; }
  Locked hold(sync_);
  if (!pipes_) {
    void* memory = allocator_->allocate(sizeof(Pipe) * kMaxPipes);
    if (!memory) { result.status = 0xC0000017u; return result; }
    pipes_ = static_cast<Pipe*>(memory);
    for (uint32_t i = 0; i < kMaxPipes; ++i) new (&pipes_[i]) Pipe();
  }
  uint32_t instances = 0;
  if (!name.empty()) {
    for (Object& o : objects_) {
      if (o.kind != ObjectKind::Pipe || o.name_size == 0 || !same_name(o.name, o.name_size, name)) continue;
      ++instances;
      const Pipe& other = pipes_[o.pipe];
      if (other.max_instances != 0 && instances >= other.max_instances) { result.status = kPipeBusy; return result; }
    }
    if (max_instances != 0 && instances >= max_instances) { result.status = kPipeBusy; return result; }
  }
  int32_t pipe_index = -1;
  for (uint32_t i = 0; i < kMaxPipes && pipe_index < 0; ++i) if (!pipes_[i].used) pipe_index = static_cast<int32_t>(i);
  if (pipe_index < 0) { result.status = kNoResources; return result; }
  for (uint32_t i = 0; i < kCapacity; ++i) {
    Object& o = objects_[i];
    if (o.kind != ObjectKind::None) continue;
    Pipe& p = pipes_[pipe_index];
    p = Pipe();
    const uint32_t sizes[2] = {out_size ? out_size : kDefaultPipeBuffer, in_size ? in_size : kDefaultPipeBuffer};
    for (uint32_t d = 0; d < 2; ++d) {
      p.capacity[d] = sizes[d] > kMaxPipeBuffer ? kMaxPipeBuffer : sizes[d];
      if (name.empty() && d == 1) { p.capacity[1] = 0; break; }      // (an anonymous pipe goes one way)
      p.data[d] = static_cast<uint8_t*>(allocator_->allocate(p.capacity[d]));
      if (!p.data[d]) {
        for (uint8_t*& freed : p.data) { if (freed) allocator_->deallocate(freed); freed = nullptr; }
        p = Pipe();
        result.status = 0xC0000017u;
        return result;
      }
    }
    p.used = true;
    p.anonymous = name.empty();
    p.connected = p.anonymous;
    p.access = p.anonymous ? kPipeOutbound : access;
    p.max_instances = max_instances;
    p.end_refs[0] = 1;
    if (p.anonymous) p.end_refs[1] = 0;
    o = Object();
    o.kind = ObjectKind::Pipe;
    o.references = 1;
    for (uint32_t k = 0; k < name.size(); ++k) o.name[k] = name[k];
    o.name_size = static_cast<uint32_t>(name.size());
    o.pipe = pipe_index;
    result.id = i + 1;
    return result;
  }
  result.status = kNoResources;
  return result;
}

ObjectManager::Created ObjectManager::connect_pipe(StringView name, bool want_read, bool want_write) {
  Created result;
  Locked hold(sync_);
  bool found = false;
  for (uint32_t i = 0; i < kCapacity; ++i) {
    Object& o = objects_[i];
    if (o.kind != ObjectKind::Pipe || o.name_size == 0 || !same_name(o.name, o.name_size, name)) continue;
    found = true;
    Pipe& p = pipes_[o.pipe];
    if (p.connected || p.end_refs[0] == 0) continue;
    if ((want_read && !(p.access & kPipeOutbound)) || (want_write && !(p.access & kPipeInbound))) { result.status = 0xC0000022u; return result; }
    p.connected = true;
    p.client_seen = true;
    p.end_refs[1] = 1;
    ++p.generation;
    ++o.references;
    sync_.notify_all();
    result.id = i + 1;
    result.end_code = 1 | (p.generation << 1);
    return result;
  }
  result.status = found ? kPipeBusy : kNotFound;
  return result;
}

void ObjectManager::add_pipe_end(uint32_t id, uint32_t end_code) {
  Locked hold(sync_);
  Object* o = at(id);
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!o || !p || !p->end_of(end_code, &end)) return;
  ++o->references;
  ++p->end_refs[end];
}

void ObjectManager::release_pipe_end(uint32_t id, uint32_t end_code) {
  Locked hold(sync_);
  Object* o = at(id);
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!o || !p || !p->end_of(end_code, &end) || p->end_refs[end] == 0) return;
  --p->end_refs[end];
  sync_.notify_all();      // the other end sees the end of the data
  if (o->references > 0 && --o->references == 0) {
    free_pipe_locked(*o);
    *o = Object();
  }
}

ObjectManager::PipeResult ObjectManager::pipe_read_locked(uint32_t id, uint32_t end_code, uint8_t* out, uint32_t capacity, uint32_t* got, bool peek, uint32_t* available) {
  *got = 0;
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end)) return PipeResult::Broken;
  if (end == 1 && !p->connected) return PipeResult::Broken;
  const uint32_t q = 1 - end;      // the queue the other end writes
  if (available) *available = p->count[q];
  if (end == 0 && !p->connected) return p->client_seen ? PipeResult::Broken : PipeResult::NotConnected;
  if (p->count[q] == 0) return p->end_refs[1 - end] == 0 || (end == 0 && !p->connected) ? PipeResult::Broken : PipeResult::WouldBlock;
  uint32_t n = p->count[q] < capacity ? p->count[q] : capacity;
  for (uint32_t i = 0; i < n; ++i) out[i] = p->data[q][(p->head[q] + i) % p->capacity[q]];
  if (!peek) {
    p->head[q] = (p->head[q] + n) % p->capacity[q];
    p->count[q] -= n;
    sync_.notify_all();      // room for the writer
  }
  *got = n;
  return PipeResult::Ok;
}

ObjectManager::PipeResult ObjectManager::pipe_write_locked(uint32_t id, uint32_t end_code, const uint8_t* data, uint32_t count, uint32_t* done) {
  *done = 0;
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end)) return PipeResult::Closing;
  if (!p->connected) return end == 0 ? (p->client_seen ? PipeResult::Closing : PipeResult::NotConnected) : PipeResult::Closing;
  if (p->end_refs[1 - end] == 0) return PipeResult::Closing;
  const uint32_t q = end;
  if (!p->data[q] || p->capacity[q] == 0) return PipeResult::Broken;
  const uint32_t room = p->capacity[q] - p->count[q];
  if (room == 0) return PipeResult::WouldBlock;
  const uint32_t n = room < count ? room : count;
  for (uint32_t i = 0; i < n; ++i) p->data[q][(p->head[q] + p->count[q] + i) % p->capacity[q]] = data[i];
  p->count[q] += n;
  *done = n;
  sync_.notify_all();      // data for the reader
  return PipeResult::Ok;
}

ObjectManager::PipeResult ObjectManager::pipe_listen_locked(uint32_t id) {
  Pipe* p = pipe_of(id);
  if (!p) return PipeResult::Broken;
  if (p->connected) return PipeResult::Ok;
  p->listening = true;
  return PipeResult::WouldBlock;
}

void ObjectManager::pipe_disconnect(uint32_t id) {
  Locked hold(sync_);
  Object* o = at(id);
  Pipe* p = pipe_of(id);
  if (!o || !p || p->anonymous) return;
  if (p->connected) {
    p->connected = false;
    o->references = o->references > p->end_refs[1] ? o->references - p->end_refs[1] : 0;
    p->end_refs[1] = 0;      // (the client's handles stay in its table, but see a broken pipe: their end no longer counts)
  }
  p->listening = false;
  p->client_seen = false;
  p->count[0] = p->count[1] = 0;
  p->head[0] = p->head[1] = 0;
  sync_.notify_all();
}

uint32_t ObjectManager::pipe_pending_locked(uint32_t id, uint32_t end_code) {
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end) || p->end_refs[1 - end] == 0 || !p->connected) return 0;
  return p->count[end];
}

bool ObjectManager::pipe_name_available_locked(StringView name) {
  for (Object& o : objects_) {
    if (o.kind != ObjectKind::Pipe || o.name_size == 0 || !same_name(o.name, o.name_size, name)) continue;
    const Pipe& p = pipes_[o.pipe];
    if (!p.connected && p.end_refs[0] != 0) return true;
  }
  return false;
}

bool ObjectManager::pipe_info(uint32_t id, PipeInfo* out) {
  Locked hold(sync_);
  Object* o = at(id);
  Pipe* p = pipe_of(id);
  if (!o || !p) return false;
  out->anonymous = p->anonymous;
  out->connected = p->connected;
  out->access = p->access;
  out->out_size = p->capacity[0];
  out->in_size = p->capacity[1];
  out->max_instances = p->max_instances;
  out->instances = 0;
  for (const Object& other : objects_) if (other.kind == ObjectKind::Pipe && o->name_size != 0 && other.name_size == o->name_size && same_name(other.name, other.name_size, StringView(o->name, o->name_size))) ++out->instances;
  if (p->anonymous) out->instances = 1;
  return true;
}

// ---- byte-range locks

struct ObjectManager::RangeLock {
  bool used = false, exclusive = false;
  const void* owner = nullptr;
  uint64_t offset = 0, length = 0;
  char path[260] = {};
  uint32_t path_size = 0;
};

namespace {
// Byte ranges [offset, offset + length) up to the end of the 64-bit space; a range of no bytes locks nothing and conflicts with nothing.
bool ranges_overlap(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) {
  if (alen == 0 || blen == 0) return false;
  const uint64_t alast = alen - 1 > ~uint64_t{0} - a ? ~uint64_t{0} : a + (alen - 1), blast = blen - 1 > ~uint64_t{0} - b ? ~uint64_t{0} : b + (blen - 1);
  return a <= blast && b <= alast;
}
}  // namespace

ObjectManager::LockResult ObjectManager::lock_range_locked(StringView path, const void* owner, uint64_t offset, uint64_t length, bool exclusive) {
  if (!allocator_ || path.size() >= 260) return LockResult::Full;
  if (!locks_) {
    void* memory = allocator_->allocate(sizeof(RangeLock) * kMaxLocks);
    if (!memory) return LockResult::Full;
    locks_ = static_cast<RangeLock*>(memory);
    for (uint32_t i = 0; i < kMaxLocks; ++i) new (&locks_[i]) RangeLock();
  }
  for (uint32_t i = 0; i < kMaxLocks; ++i) {
    const RangeLock& l = locks_[i];
    if (!l.used || !same_name(l.path, l.path_size, path) || !ranges_overlap(l.offset, l.length, offset, length)) continue;
    if (l.owner == owner) { if (exclusive) return LockResult::Conflict; continue; }      // (an open file does not lock itself out, except with an exclusive lock over its own)
    if (l.exclusive || exclusive) return LockResult::Conflict;      // (two shared locks may overlap, even of one file; anything with an exclusive one may not)
  }
  for (uint32_t i = 0; i < kMaxLocks; ++i) {
    RangeLock& l = locks_[i];
    if (l.used) continue;
    l = RangeLock();
    l.used = true;
    l.exclusive = exclusive;
    l.owner = owner;
    l.offset = offset;
    l.length = length;
    for (uint32_t k = 0; k < path.size(); ++k) l.path[k] = path[k];
    l.path_size = static_cast<uint32_t>(path.size());
    return LockResult::Granted;
  }
  return LockResult::Full;
}

bool ObjectManager::unlock_range(StringView path, const void* owner, uint64_t offset, uint64_t length) {
  Locked hold(sync_);
  if (!locks_) return false;
  for (int pass = 0; pass < 2; ++pass) {      // an exclusive lock of exactly this range goes before a shared one
    for (uint32_t i = 0; i < kMaxLocks; ++i) {
      RangeLock& l = locks_[i];
      if (!l.used || l.owner != owner || l.offset != offset || l.length != length || !same_name(l.path, l.path_size, path) || l.exclusive != (pass == 0)) continue;
      l.used = false;
      sync_.notify_all();      // whoever waits for the range looks again
      return true;
    }
  }
  return false;
}

struct ObjectManager::ShareEntry {
  bool used = false;
  const void* owner = nullptr;
  uint32_t access = 0, share = 0;
  char path[260] = {};
  uint32_t path_size = 0;
};

ObjectManager::ShareResult ObjectManager::share_acquire(StringView path, const void* owner, uint32_t access, uint32_t share) {
  Locked hold(sync_);
  if (!allocator_ || path.size() >= 260) return ShareResult::Full;
  if (!shares_) {
    void* memory = allocator_->allocate(sizeof(ShareEntry) * kMaxShares);
    if (!memory) return ShareResult::Full;
    shares_ = static_cast<ShareEntry*>(memory);
    for (uint32_t i = 0; i < kMaxShares; ++i) new (&shares_[i]) ShareEntry();
  }
  access &= kShareRead | kShareWrite | kShareDelete;
  if (access == 0) return ShareResult::Granted;      // (nothing to conflict with, and nothing to remember)
  for (uint32_t i = 0; i < kMaxShares; ++i) {
    const ShareEntry& e = shares_[i];
    if (!e.used || !same_name(e.path, e.path_size, path)) continue;
    if ((access & ~e.share) != 0 || (e.access & ~share) != 0) return ShareResult::Violation;
  }
  if (!owner) return ShareResult::Granted;      // only asked whether it would be allowed
  for (uint32_t i = 0; i < kMaxShares; ++i) {
    ShareEntry& e = shares_[i];
    if (e.used) continue;
    e.used = true;
    e.owner = owner;
    e.access = access;
    e.share = share & (kShareRead | kShareWrite | kShareDelete);
    for (uint32_t k = 0; k < path.size(); ++k) e.path[k] = path[k];
    e.path_size = static_cast<uint32_t>(path.size());
    return ShareResult::Granted;
  }
  return ShareResult::Full;
}

bool ObjectManager::share_allows(StringView path, uint32_t access) {
  Locked hold(sync_);
  if (!shares_) return true;
  for (uint32_t i = 0; i < kMaxShares; ++i) {
    const ShareEntry& e = shares_[i];
    if (e.used && same_name(e.path, e.path_size, path) && (access & ~e.share) != 0) return false;
  }
  return true;
}

uint32_t ObjectManager::share_owners(StringView path, const void** owners, uint32_t capacity) {
  Locked hold(sync_);
  uint32_t count = 0;
  if (!shares_) return 0;
  for (uint32_t i = 0; i < kMaxShares; ++i) {
    const ShareEntry& e = shares_[i];
    if (!e.used || !same_name(e.path, e.path_size, path)) continue;
    if (count < capacity) owners[count] = e.owner;
    ++count;
  }
  return count;
}

void ObjectManager::share_release(const void* owner) {
  Locked hold(sync_);
  if (!shares_) return;
  for (uint32_t i = 0; i < kMaxShares; ++i) if (shares_[i].used && shares_[i].owner == owner) shares_[i].used = false;
}

void ObjectManager::unlock_all(const void* owner) {
  Locked hold(sync_);
  if (!locks_) return;
  bool any = false;
  for (uint32_t i = 0; i < kMaxLocks; ++i) if (locks_[i].used && locks_[i].owner == owner) { locks_[i].used = false; any = true; }
  if (any) sync_.notify_all();
}

// ---- sections

void ObjectManager::write_section_locked(Section& s) {
  if (!s.file_backed || !s.writable || !s.fs || !s.storage) return;
  UniquePointer<filesystem::IFile> file;
  if (s.fs->open_file(StringView(s.path, s.path_size), filesystem::OpenMode::CreateOrOpen, &file) != filesystem::Status::Ok) return;
  uint64_t done = 0;
  while (done < s.size) {
    uint64_t wrote = 0;
    const uint64_t n = s.size - done < (1u << 20) ? s.size - done : (1u << 20);
    if (file->write(done, ConstByteView(s.storage + done, n), &wrote) != filesystem::Status::Ok || wrote == 0) break;
    done += wrote;
  }
  file->flush();
}

void ObjectManager::free_section_locked(Object& o) {
  if (o.section < 0 || !sections_) return;
  Section& s = sections_[o.section];
  write_section_locked(s);
  if (s.storage && allocator_) allocator_->deallocate(s.storage);
  s.storage = nullptr;
  s.used = false;
  s.file_backed = false;
  o.section = -1;
}

void ObjectManager::section_count(uint32_t index, int32_t delta) {
  Locked hold(sync_);
  if (!sections_ || index >= kMaxSections || !sections_[index].used) return;
  Section& s = sections_[index];
  s.mappings += delta;
  if (s.mappings > 0) return;
  s.mappings = 0;
  write_section_locked(s);      // (the last view went away)
  Object& o = objects_[s.object];
  if (o.kind == ObjectKind::Section && o.references == 0) {      // no handle either: the section is gone
    free_section_locked(o);
    o = Object();
  }
}

ObjectManager::Created ObjectManager::create_section(StringView name, uint64_t size, uint32_t page, uint32_t attributes, filesystem::IFileSystem* fs, StringView host_path) {
  Created result;
  if (!allocator_ || name.size() >= kNameBytes || host_path.size() >= 512) { result.status = !allocator_ ? 0xC0000002u : kInvalid; return result; }
  Locked hold(sync_);
  if (Object* existing = find_locked(name)) {
    if (existing->kind != ObjectKind::Section) { result.status = kTypeMismatch; return result; }
    ++existing->references;
    result.id = static_cast<uint32_t>(existing - objects_) + 1;
    result.status = kNameExists;
    return result;
  }
  if (!sections_) {
    void* memory = allocator_->allocate(sizeof(Section) * kMaxSections);
    if (!memory) { result.status = 0xC0000017u; return result; }
    sections_ = static_cast<Section*>(memory);
    for (uint32_t i = 0; i < kMaxSections; ++i) new (&sections_[i]) Section();
  }
  int32_t index = -1;
  for (uint32_t i = 0; i < kMaxSections && index < 0; ++i) if (!sections_[i].used) index = static_cast<int32_t>(i);
  if (index < 0) { result.status = kNoResources; return result; }
  uint64_t file_size = 0;
  UniquePointer<filesystem::IFile> file;
  if (fs) {
    if (fs->open_file(host_path, filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) { result.status = kNotFound; return result; }
    file_size = file->size();
  }
  const uint64_t bytes = size > file_size ? size : file_size;
  if (bytes == 0 || bytes > (64ull << 20)) { result.status = bytes == 0 ? kInvalid : kNoResources; return result; }
  const uint64_t rounded = (bytes + 4095) & ~uint64_t{4095};
  uint8_t* storage = static_cast<uint8_t*>(allocator_->allocate_aligned(rounded, 4096));
  if (!storage) { result.status = 0xC0000017u; return result; }
  for (uint64_t i = 0; i < rounded; ++i) storage[i] = 0;
  for (uint64_t done = 0; fs && done < file_size;) {
    uint64_t got = 0;
    const uint64_t n = file_size - done < (1u << 20) ? file_size - done : (1u << 20);
    if (file->read(done, ByteView(storage + done, n), &got) != filesystem::Status::Ok || got == 0) break;
    done += got;
  }
  file.reset();
  for (uint32_t i = 0; i < kCapacity; ++i) {
    Object& o = objects_[i];
    if (o.kind != ObjectKind::None) continue;
    o = Object();
    o.kind = ObjectKind::Section;
    o.references = 1;
    for (uint32_t k = 0; k < name.size(); ++k) o.name[k] = name[k];
    o.name_size = static_cast<uint32_t>(name.size());
    o.section = index;
    Section& s = sections_[index];
    s = Section();
    s.used = true;
    s.writable = page == 0x04 || page == 0x40;
    s.page = page;
    s.attributes = attributes;
    s.file_backed = fs != nullptr;
    s.reserve = (attributes & 0x4000000u) != 0 && !fs;
    s.storage = storage;
    s.size = bytes;
    s.rounded = rounded;
    s.object = i;
    s.fs = fs;
    for (uint32_t k = 0; k < host_path.size(); ++k) s.path[k] = host_path[k];
    s.path_size = static_cast<uint32_t>(host_path.size());
    s.ref.attach(this, static_cast<uint32_t>(index));
    result.id = i + 1;
    return result;
  }
  allocator_->deallocate(storage);
  result.status = kNoResources;
  return result;
}

bool ObjectManager::section_memory(uint32_t id, uint8_t** storage, uint64_t* size, SharedMemory** owner) {
  Locked hold(sync_);
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::Section || o->section < 0 || !sections_) return false;
  Section& s = sections_[o->section];
  *storage = s.storage;
  *size = s.rounded;
  *owner = &s.ref;
  return true;
}

ObjectManager::Section* ObjectManager::section_of_owner(const SharedMemory* owner) {
  if (!sections_) return nullptr;
  for (uint32_t i = 0; i < kMaxSections; ++i) if (sections_[i].used && static_cast<const SharedMemory*>(&sections_[i].ref) == owner) return &sections_[i];
  return nullptr;
}

bool ObjectManager::section_reserved(const SharedMemory* owner) {
  Locked hold(sync_);
  const Section* s = section_of_owner(owner);
  return s && s->reserve;
}

uint64_t ObjectManager::section_offset(const SharedMemory* owner, const uint8_t* pointer) {
  Locked hold(sync_);
  const Section* s = section_of_owner(owner);
  return s && pointer >= s->storage ? uint64_t(pointer - s->storage) : 0;
}

void ObjectManager::section_commit_pages(const SharedMemory* owner, uint64_t offset, uint64_t length) {
  Locked hold(sync_);
  Section* s = section_of_owner(owner);
  if (!s || !s->reserve) return;
  for (uint64_t page = offset / 4096; page * 4096 < offset + length && page < sizeof(s->committed) * 8; ++page) s->committed[page / 8] |= uint8_t(1u << (page % 8));
}

bool ObjectManager::section_page_committed(const SharedMemory* owner, uint64_t offset) {
  Locked hold(sync_);
  const Section* s = section_of_owner(owner);
  if (!s || !s->reserve) return true;
  const uint64_t page = offset / 4096;
  return page < sizeof(s->committed) * 8 && (s->committed[page / 8] >> (page % 8)) & 1;
}

bool ObjectManager::section_note_image_view(uint32_t id) {
  Locked hold(sync_);
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::Section || o->section < 0 || !sections_) return false;
  const bool before = sections_[o->section].image_mapped;
  sections_[o->section].image_mapped = true;
  return before;
}

bool ObjectManager::section_file_name(uint32_t id, char (&out)[40]) {
  Locked hold(sync_);
  out[0] = 0;
  const Object* o = at(id);
  if (!o || o->kind != ObjectKind::Section || o->section < 0 || !sections_ || !sections_[o->section].file_backed) return false;
  const Section& s = sections_[o->section];
  uint32_t start = 0;
  for (uint32_t i = 0; i < s.path_size; ++i) if (s.path[i] == '/' || s.path[i] == '\\') start = i + 1;
  uint32_t n = 0;
  for (uint32_t i = start; i < s.path_size && n < 39; ++i) out[n++] = lower(s.path[i]);
  out[n] = 0;
  return n != 0;
}

uint32_t ObjectManager::file_has_section(StringView host_path) {
  Locked hold(sync_);
  uint32_t found = 0;
  if (!sections_) return 0;
  for (uint32_t i = 0; i < kMaxSections; ++i) {
    const Section& s = sections_[i];
    if (s.used && s.file_backed && s.path_size == host_path.size() && same_name(s.path, s.path_size, host_path)) { if (s.attributes & 0x1000000u) return 2; found = 1; }
  }
  return found;
}

bool ObjectManager::section_info(uint32_t id, SectionInfo* out) {
  Locked hold(sync_);
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::Section || o->section < 0 || !sections_) return false;
  const Section& s = sections_[o->section];
  out->size = s.size;
  out->writable = s.writable;
  out->page = s.page;
  out->attributes = s.attributes;
  out->file_backed = s.file_backed;
  out->reported_size = s.reported_size;
  return true;
}

void ObjectManager::section_set_reported_size(uint32_t id, uint64_t size) {
  Locked hold(sync_);
  Object* o = at(id);
  if (o && o->kind == ObjectKind::Section && o->section >= 0 && sections_) sections_[o->section].reported_size = size;
}

void ObjectManager::flush_section_at(uint32_t index) {
  Locked hold(sync_);
  if (sections_ && index < kMaxSections && sections_[index].used) write_section_locked(sections_[index]);
}

void ObjectManager::flush_section(uint32_t id) {
  Locked hold(sync_);
  Object* o = at(id);
  if (o && o->kind == ObjectKind::Section && o->section >= 0 && sections_) write_section_locked(sections_[o->section]);
}

// ---- timers

void ObjectManager::fire_timer_locked(Object& o, uint64_t now) {
  if (o.kind != ObjectKind::Timer || !o.armed || now < o.due_ns) return;
  o.signaled = true;
  o.fired_at = o.due_ns;
  if (o.apc_routine != 0) o.apc_pending = true;
  if (o.period_ns != 0) {
    const uint64_t missed = (now - o.due_ns) / o.period_ns + 1;
    o.due_ns += missed * o.period_ns;
  } else {
    o.armed = false;
  }
}

uint32_t ObjectManager::set_timer(uint32_t id, uint64_t due_ns, uint64_t period_ns, uint64_t thread, uint64_t routine, uint64_t context, bool* was_signaled) {
  Locked hold(sync_);
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::Timer) return kInvalidHandle;
  fire_timer_locked(*o, sync_.now_ns());
  if (was_signaled) *was_signaled = o->signaled;
  o->signaled = false;      // (setting a timer makes it unsignalled again)
  o->armed = true;
  o->due_ns = due_ns;
  o->period_ns = period_ns;
  o->apc_thread = routine ? thread : 0;
  o->apc_routine = routine;
  o->apc_context = context;
  o->apc_pending = false;
  sync_.notify_all();      // whoever waits for it looks at the new time
  return kSuccess;
}

uint32_t ObjectManager::cancel_timer(uint32_t id, bool* was_signaled) {
  Locked hold(sync_);
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::Timer) return kInvalidHandle;
  fire_timer_locked(*o, sync_.now_ns());
  if (was_signaled) *was_signaled = o->signaled;
  o->armed = false;
  o->apc_pending = false;
  return kSuccess;
}

uint32_t ObjectManager::query_timer(uint32_t id, uint64_t* remaining_ns, bool* signaled) {
  Locked hold(sync_);
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::Timer) return kInvalidHandle;
  const uint64_t now = sync_.now_ns();
  fire_timer_locked(*o, now);
  *signaled = o->signaled;
  *remaining_ns = o->armed && o->due_ns > now ? o->due_ns - now : 0;
  return kSuccess;
}

void ObjectManager::take_timer_apcs_locked(uint64_t thread, TimerApc* out, uint32_t capacity, uint32_t* count) {
  *count = 0;
  const uint64_t now = sync_.now_ns();
  for (Object& o : objects_) {
    if (o.kind != ObjectKind::Timer || o.apc_thread != thread) continue;
    fire_timer_locked(o, now);
    if (!o.apc_pending || *count >= capacity) continue;
    o.apc_pending = false;
    out[(*count)++] = TimerApc{o.apc_routine, o.apc_context, o.fired_at};
  }
}

uint64_t ObjectManager::thread_timer_deadline_locked(uint64_t thread) {
  uint64_t soonest = ~uint64_t{0};
  for (const Object& o : objects_) if (o.kind == ObjectKind::Timer && o.apc_thread == thread && o.armed && o.due_ns < soonest) soonest = o.due_ns;
  return soonest;
}

uint64_t ObjectManager::timer_deadline_locked(uint32_t id) {
  Object* o = at(id);
  return o && o->kind == ObjectKind::Timer && o->armed ? o->due_ns : ~uint64_t{0};
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
  if (--o->references == 0) {
    if (o->kind == ObjectKind::Section && o->section >= 0 && sections_ && sections_[o->section].mappings > 0) {      // views keep the memory; its name goes
      o->name_size = 0;
      return;
    }
    free_pipe_locked(*o);
    free_section_locked(*o);
    free_job_locked(*o);
    free_watch_locked(*o);
    free_mailslot_locked(*o);
    *o = Object();
  }
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
  // A change notification handle (FindNextChangeNotification) is reset the same way: both are "stays signalled until told otherwise" handles with nothing else to it.
  if (!o || (o->kind != ObjectKind::Event && o->kind != ObjectKind::ChangeNotification)) return kInvalidHandle;
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
    case ObjectKind::ChangeNotification: return o->signaled;
    case ObjectKind::Process: return o->signaled;
    case ObjectKind::Timer: fire_timer_locked(*o, sync_.now_ns()); return o->signaled;
    case ObjectKind::Mutex: return o->owner == 0 || o->owner == waiter;
    case ObjectKind::Semaphore: return o->count > 0;
    case ObjectKind::Job: return false;      // (it is signalled when a time limit runs out, which is not kept here)
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
    case ObjectKind::Timer:
      if (!o->manual_reset) o->signaled = false;      // (a synchronization timer lets one waiter through per firing)
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


// ---- job objects

namespace {
constexpr uint32_t kAccessDenied = 0xC0000022u, kQuotaExceeded = 0xC0000044u;
}

ObjectManager::Job* ObjectManager::job_of(uint32_t id) {
  Object* o = at(id);
  return o && o->kind == ObjectKind::Job && o->job >= 0 && jobs_ ? &jobs_[o->job] : nullptr;
}

void ObjectManager::job_flag_all_locked(Job& j, uint32_t code) {
  for (JobMember& m : j.members) {
    if (m.pid == 0 || m.kill) continue;
    m.code = code;
    __atomic_store_n(&m.kill, 1u, __ATOMIC_RELEASE);
    __atomic_add_fetch(&pending_kills_, 1u, __ATOMIC_ACQ_REL);
  }
  sync_.notify_all();
}

void ObjectManager::free_job_locked(Object& o) {
  if (o.kind != ObjectKind::Job || o.job < 0 || !jobs_) return;
  Job& j = jobs_[o.job];
  if (j.info.limit_flags & kJobLimitKillOnClose) job_flag_all_locked(j, 1);
  for (JobMember& m : j.members) {      // the members stay members of nothing; those flagged keep their flag (it is found by their process id)
    if (m.pid == 0) continue;
    if (m.kill) { /* leave the entry so that the flagged process still finds it, until it ends */ continue; }
    m.pid = 0;
  }
  bool keep = false;
  for (const JobMember& m : j.members) if (m.pid != 0) keep = true;
  if (!keep) j.used = false;
  else j.object = 0;      // (the slot is freed when the last flagged member ends)
  o.job = -1;
}

uint32_t ObjectManager::job_assign(uint32_t job, uint64_t pid) {
  Locked hold(sync_);
  Job* j = job_of(job);
  if (!j) return kInvalidHandle;
  for (uint32_t k = 0; k < kJobMaxJobs; ++k) if (jobs_[k].used) for (const JobMember& m : jobs_[k].members) if (m.pid == pid) return kAccessDenied;
  if ((j->info.limit_flags & kJobLimitActiveProcess) && j->info.active_processes >= j->info.active_limit) return kQuotaExceeded;
  for (JobMember& m : j->members) {
    if (m.pid != 0) continue;
    m.kill = 0; m.code = 0;
    __atomic_store_n(&m.pid, pid, __ATOMIC_RELEASE);
    ++j->info.active_processes;
    ++j->info.total_processes;
    return kSuccess;
  }
  return kQuotaExceeded;
}

uint32_t ObjectManager::job_of_process(uint64_t pid) {
  Locked hold(sync_);
  if (!jobs_) return 0;
  for (uint32_t k = 0; k < kJobMaxJobs; ++k) {
    if (!jobs_[k].used || jobs_[k].object == 0) continue;
    for (const JobMember& m : jobs_[k].members) if (m.pid == pid) return jobs_[k].object;
  }
  return 0;
}

uint32_t ObjectManager::job_inherit(uint64_t parent, uint64_t child, bool breakaway) {
  Locked hold(sync_);
  if (!jobs_) return kSuccess;
  for (uint32_t k = 0; k < kJobMaxJobs; ++k) {
    Job& j = jobs_[k];
    if (!j.used || j.object == 0) continue;
    for (const JobMember& pm : j.members) {
      if (pm.pid != parent) continue;
      if (breakaway) return (j.info.limit_flags & kJobLimitBreakawayOk) ? kSuccess : kAccessDenied;
      if (j.info.limit_flags & kJobLimitSilentBreakawayOk) return kSuccess;
      if ((j.info.limit_flags & kJobLimitActiveProcess) && j.info.active_processes >= j.info.active_limit) return kQuotaExceeded;
      for (JobMember& m : j.members) {
        if (m.pid != 0) continue;
        m.kill = 0; m.code = 0;
        if (pm.kill) { m.code = pm.code; m.kill = 1; __atomic_add_fetch(&pending_kills_, 1u, __ATOMIC_ACQ_REL); }      // (a child of a process being ended is ended too)
        __atomic_store_n(&m.pid, child, __ATOMIC_RELEASE);
        ++j.info.active_processes;
        ++j.info.total_processes;
        return kSuccess;
      }
      return kQuotaExceeded;
    }
  }
  return kSuccess;
}

void ObjectManager::job_forget_locked(uint64_t pid) {
  if (!jobs_) return;
  for (uint32_t k = 0; k < kJobMaxJobs; ++k) {
    Job& j = jobs_[k];
    if (!j.used) continue;
    for (JobMember& m : j.members) {
      if (m.pid != pid) continue;
      if (m.kill) {
        __atomic_sub_fetch(&pending_kills_, 1u, __ATOMIC_ACQ_REL);
        ++j.info.terminated_processes;
      }
      __atomic_store_n(&m.kill, 0u, __ATOMIC_RELEASE);
      __atomic_store_n(&m.pid, uint64_t{0}, __ATOMIC_RELEASE);
      if (j.info.active_processes > 0) --j.info.active_processes;
      if (j.object == 0) {      // the job object is gone; this was one of the members still to end
        bool any = false;
        for (const JobMember& other : j.members) if (other.pid != 0) any = true;
        if (!any) j.used = false;
      }
      return;
    }
  }
}

void ObjectManager::job_forget(uint64_t pid) {
  Locked hold(sync_);
  job_forget_locked(pid);
}

uint32_t ObjectManager::job_terminate(uint32_t job, uint32_t code) {
  Locked hold(sync_);
  Job* j = job_of(job);
  if (!j) return kInvalidHandle;
  job_flag_all_locked(*j, code);
  return kSuccess;
}

bool ObjectManager::job_info(uint32_t job, JobInfo* out, uint64_t* pids, uint32_t capacity, uint32_t* pid_count) {
  Locked hold(sync_);
  Job* j = job_of(job);
  if (!j) return false;
  *out = j->info;
  uint32_t n = 0;
  for (const JobMember& m : j->members) if (m.pid != 0) { if (pids && n < capacity) pids[n] = m.pid; ++n; }
  if (pid_count) *pid_count = n;
  return true;
}

uint32_t ObjectManager::job_set(uint32_t job, const JobInfo& in, bool extended) {
  Locked hold(sync_);
  Job* j = job_of(job);
  if (!j) return kInvalidHandle;
  if (in.limit_flags & ~kJobLimitAll) return kInvalid;
  if ((in.limit_flags & kJobLimitKillOnClose) && !extended) return kInvalid;      // (only the extended structure can ask for it)
  if ((in.limit_flags & kJobLimitBreakawayOk) && (in.limit_flags & kJobLimitSilentBreakawayOk)) { /* both are allowed */ }
  if ((in.limit_flags & kJobLimitActiveProcess) && in.active_limit == 0) return kInvalid;
  if ((in.limit_flags & kJobLimitAffinity) && in.affinity == 0) return kInvalid;
  if ((in.limit_flags & kJobLimitPriorityClass) && (in.priority_class < 1 || in.priority_class > 6) && in.priority_class != 0x20 && in.priority_class != 0x40 && in.priority_class != 0x80 && in.priority_class != 0x100 && in.priority_class != 0x4000 && in.priority_class != 0x8000) return kInvalid;
  JobInfo& d = j->info;
  d.per_process_time = in.per_process_time; d.per_job_time = in.per_job_time;
  d.min_working_set = in.min_working_set; d.max_working_set = in.max_working_set;
  d.affinity = in.affinity; d.priority_class = in.priority_class; d.scheduling_class = in.scheduling_class;
  d.limit_flags = in.limit_flags; d.active_limit = in.active_limit;
  if (extended) { d.process_memory_limit = in.process_memory_limit; d.job_memory_limit = in.job_memory_limit; }
  return kSuccess;
}

bool ObjectManager::job_kill_for(uint64_t pid, uint32_t* code) {
  if (__atomic_load_n(&pending_kills_, __ATOMIC_ACQUIRE) == 0) return false;
  Job* jobs = __atomic_load_n(&jobs_, __ATOMIC_ACQUIRE);
  if (!jobs) return false;
  for (uint32_t k = 0; k < kJobMaxJobs; ++k) {
    for (const JobMember& m : jobs[k].members) {
      if (__atomic_load_n(&m.pid, __ATOMIC_ACQUIRE) != pid) continue;
      if (!__atomic_load_n(&m.kill, __ATOMIC_ACQUIRE)) return false;
      *code = m.code;
      return true;
    }
  }
  return false;
}

// ---- change notifications

void ObjectManager::free_watch_locked(Object& o) {
  if (o.kind != ObjectKind::ChangeNotification || o.watch < 0 || !watches_) return;
  watches_[o.watch].used = false;
  o.watch = -1;
}

ObjectManager::Created ObjectManager::create_change_notification(StringView path, bool watch_subtree, uint32_t filter) {
  Created result;
  if (path.size() >= kWatchPathBytes) { result.status = kNameTooLong; return result; }
  Locked hold(sync_);
  if (!watches_ && allocator_) {
    void* memory = allocator_->allocate(sizeof(Watch) * kMaxWatches);
    if (memory) watches_ = new (memory) Watch[kMaxWatches];
  }
  int32_t slot = -1;
  for (uint32_t i = 0; watches_ && i < kMaxWatches && slot < 0; ++i) if (!watches_[i].used) slot = int32_t(i);
  if (slot < 0) { result.status = kNoResources; return result; }
  for (uint32_t i = 0; i < kCapacity; ++i) {
    Object& o = objects_[i];
    if (o.kind != ObjectKind::None) continue;
    Watch& w = watches_[slot];
    w = Watch();
    w.used = true;
    for (uint64_t k = 0; k < path.size(); ++k) w.path[k] = path[k];
    w.path_size = static_cast<uint32_t>(path.size());
    w.subtree = watch_subtree;
    w.filter = filter;
    o = Object();
    o.kind = ObjectKind::ChangeNotification;
    o.references = 1;
    o.manual_reset = true;      // left signalled until FindNextChangeNotification (reset_event) explicitly resets it
    o.watch = slot;
    result.id = i + 1;
    result.status = kSuccess;
    return result;
  }
  watches_[slot].used = false;
  result.status = kNoResources;
  return result;
}

void ObjectManager::notify_path_changed(StringView affected_directory, uint32_t action, StringView entry_name) {
  Locked hold(sync_);
  if (!watches_) return;
  bool any = false;
  for (Object& o : objects_) {
    if (o.kind != ObjectKind::ChangeNotification || o.watch < 0) continue;
    Watch& w = watches_[o.watch];
    const StringView watched(w.path, w.path_size);
    bool matches = affected_directory == watched;
    if (!matches && w.subtree) {
      matches = watched.size() <= 1 ||      // the root ("/"): every directory is under it
                (affected_directory.size() > watched.size() && affected_directory[watched.size()] == '/' && affected_directory.first(watched.size()) == watched);
    }
    if (!matches) continue;
    o.signaled = true;
    any = true;
    if (w.record_count < kMaxWatchRecords && entry_name.size() + affected_directory.size() < kWatchNameBytes) {      // a full queue just drops it: signaled (above) still fires
      WatchRecord& r = w.records[(w.record_head + w.record_count) % kMaxWatchRecords];
      r.action = action;
      uint32_t n = 0;
      if (affected_directory.size() > watched.size() && watched.size() > 1) {      // a change below the watched folder: the name is the path from it, with backslashes
        for (uint64_t k = watched.size() + 1; k < affected_directory.size() && n + 1 < kWatchNameBytes; ++k) r.name[n++] = affected_directory[k] == '/' ? '\\' : affected_directory[k];
        if (n + 1 < kWatchNameBytes) r.name[n++] = '\\';
      }
      for (uint64_t k = 0; k < entry_name.size() && n < kWatchNameBytes; ++k) r.name[n++] = entry_name[k];
      r.name_size = n;
      ++w.record_count;
    }
  }
  if (any) sync_.notify_all();
}

bool ObjectManager::take_watch_records(uint32_t id, WatchRecord* out, uint32_t capacity, uint32_t* count) {
  Locked hold(sync_);
  return take_watch_records_locked(id, out, capacity, count);
}

bool ObjectManager::take_watch_records_locked(uint32_t id, WatchRecord* out, uint32_t capacity, uint32_t* count) {
  *count = 0;
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::ChangeNotification || o->watch < 0 || !watches_) return false;
  Watch& w = watches_[o->watch];
  const uint32_t n = w.record_count < capacity ? w.record_count : capacity;
  for (uint32_t i = 0; i < n; ++i) out[i] = w.records[(w.record_head + i) % kMaxWatchRecords];
  w.record_head = (w.record_head + n) % kMaxWatchRecords;
  w.record_count -= n;
  *count = n;
  return true;
}

// ---- mailslots

ObjectManager::Mailslot* ObjectManager::mailslot_of(uint32_t id) {
  Object* o = at(id);
  return o && o->kind == ObjectKind::Mailslot && o->mailslot >= 0 && mailslots_ ? &mailslots_[o->mailslot] : nullptr;
}

void ObjectManager::free_mailslot_locked(Object& o) {
  if (o.kind != ObjectKind::Mailslot || o.mailslot < 0 || !mailslots_) return;
  Mailslot& m = mailslots_[o.mailslot];
  if (m.data && allocator_) allocator_->deallocate(m.data);
  m = Mailslot();
  o.mailslot = -1;
}

ObjectManager::Created ObjectManager::create_mailslot(StringView name, uint32_t max_message_size, uint64_t read_timeout_100ns) {
  Created result;
  if (name.empty() || name.size() >= kNameBytes) { result.status = kInvalid; return result; }
  Locked hold(sync_);
  if (find_locked(name)) { result.status = kNameExists; return result; }      // one reader per name, like the real one (CreateMailslot refuses an existing name)
  if (!mailslots_ && allocator_) {
    void* memory = allocator_->allocate(sizeof(Mailslot) * kMaxMailslots);
    if (memory) mailslots_ = new (memory) Mailslot[kMaxMailslots];
  }
  int32_t slot = -1;
  for (uint32_t i = 0; mailslots_ && i < kMaxMailslots && slot < 0; ++i) if (!mailslots_[i].used) slot = int32_t(i);
  if (slot < 0) { result.status = kNoResources; return result; }
  uint8_t* buffer = allocator_ ? static_cast<uint8_t*>(allocator_->allocate(kMailslotBufferBytes)) : nullptr;
  if (!buffer) { result.status = kNoResources; return result; }
  for (uint32_t i = 0; i < kCapacity; ++i) {
    Object& o = objects_[i];
    if (o.kind != ObjectKind::None) continue;
    Mailslot& m = mailslots_[slot];
    m = Mailslot();
    m.used = true;
    m.data = buffer;
    m.capacity = kMailslotBufferBytes;
    m.max_message_size = max_message_size;
    m.read_timeout_100ns = read_timeout_100ns;
    o = Object();
    o.kind = ObjectKind::Mailslot;
    o.references = 1;
    for (uint64_t k = 0; k < name.size(); ++k) o.name[k] = name[k];
    o.name_size = static_cast<uint32_t>(name.size());
    o.mailslot = slot;
    result.id = i + 1;
    result.status = kSuccess;
    return result;
  }
  if (allocator_) allocator_->deallocate(buffer);
  mailslots_[slot].used = false;
  result.status = kNoResources;
  return result;
}

ObjectManager::Created ObjectManager::open_mailslot_writer(StringView name) {
  Created result;
  Locked hold(sync_);
  Object* o = find_locked(name);
  if (!o || o->kind != ObjectKind::Mailslot) { result.status = kNotFound; return result; }
  ++o->references;
  result.id = static_cast<uint32_t>(o - objects_) + 1;
  result.status = kSuccess;
  return result;
}

ObjectManager::MailslotResult ObjectManager::mailslot_write_locked(uint32_t id, ConstByteView data) {
  Mailslot* m = mailslot_of(id);
  if (!m) return MailslotResult::Gone;
  if (m->max_message_size != 0 && data.size() > m->max_message_size) return MailslotResult::TooBig;
  if (data.size() > m->capacity - m->count || m->message_count >= kMaxMailslotMessages) return MailslotResult::Full;
  for (uint64_t i = 0; i < data.size(); ++i) m->data[(m->head + m->count + i) % m->capacity] = data[i];
  m->count += static_cast<uint32_t>(data.size());
  m->message_lengths[(m->message_head + m->message_count) % kMaxMailslotMessages] = static_cast<uint32_t>(data.size());
  ++m->message_count;
  sync_.notify_all();
  return MailslotResult::Ok;
}

ObjectManager::MailslotResult ObjectManager::mailslot_read_locked(uint32_t id, uint8_t* out, uint32_t capacity, uint32_t* got, uint32_t* message_size) {
  *got = 0;
  Mailslot* m = mailslot_of(id);
  if (!m) return MailslotResult::Gone;
  if (m->message_count == 0) { if (message_size) *message_size = 0; return MailslotResult::NoMessage; }
  const uint32_t size = m->message_lengths[m->message_head];
  if (message_size) *message_size = size;
  if (size > capacity) return MailslotResult::TooSmall;      // left queued, like a real mailslot's ERROR_INSUFFICIENT_BUFFER
  for (uint32_t i = 0; i < size; ++i) out[i] = m->data[(m->head + i) % m->capacity];
  m->head = (m->head + size) % m->capacity;
  m->count -= size;
  m->message_head = (m->message_head + 1) % kMaxMailslotMessages;
  --m->message_count;
  *got = size;
  sync_.notify_all();
  return MailslotResult::Ok;
}

bool ObjectManager::mailslot_info(uint32_t id, uint32_t* max_message_size, uint64_t* read_timeout_100ns, uint32_t* next_message_size, uint32_t* message_count) {
  Locked hold(sync_);
  Mailslot* m = mailslot_of(id);
  if (!m) return false;
  *max_message_size = m->max_message_size;
  *read_timeout_100ns = m->read_timeout_100ns;
  *next_message_size = m->message_count ? m->message_lengths[m->message_head] : 0;
  *message_count = m->message_count;
  return true;
}

bool ObjectManager::mailslot_set_timeout(uint32_t id, uint64_t read_timeout_100ns) {
  Locked hold(sync_);
  Mailslot* m = mailslot_of(id);
  if (!m) return false;
  m->read_timeout_100ns = read_timeout_100ns;
  return true;
}

void ObjectManager::mailslot_close_reader(uint32_t id) {
  Locked hold(sync_);
  Object* o = at(id);
  if (o && o->kind == ObjectKind::Mailslot) { free_mailslot_locked(*o); sync_.notify_all(); }      // writers still holding the id now see Gone
}

}  // namespace process
