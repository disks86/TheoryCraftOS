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

#include <initializer_list>
#include "Utility/AllocatedView.hpp"

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

ObjectManager::Object* ObjectManager::find_locked(StringView name, bool case_insensitive) {
  if (name.empty()) return nullptr;
  Object* similar = nullptr;
  for (Object& o : objects_) {
    if (o.kind == ObjectKind::None || o.name_size == 0) continue;
    if (o.kind == ObjectKind::Pipe) continue;      // (pipes have a namespace of their own)
    if (o.name_size != name.size()) continue;
    if (StringView(o.name, o.name_size) == name) return &o;      // (the names of kernel objects are case sensitive, unless OBJ_CASE_INSENSITIVE is asked for)
    if (case_insensitive && StringView(o.name, o.name_size).equals_ignore_case(name)) similar = &o;
  }
  return similar;
}

ObjectManager::Created ObjectManager::create(ObjectKind kind, StringView name, bool manual_reset, bool initial_state, int32_t initial, int32_t maximum, uint64_t owner, StringView text) {
  Created result;
  if (text.size() >= kNameBytes) { result.status = kNameTooLong; return result; }
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
    for (uint32_t k = 0; k < text.size(); ++k) o.text[k] = text[k];
    o.text_size = static_cast<uint32_t>(text.size());
    o.manual_reset = manual_reset;
    if (kind == ObjectKind::Event) o.signaled = initial_state;
    if (kind == ObjectKind::Mutex && initial_state) { o.owner = owner; o.recursion = 1; }
    if (kind == ObjectKind::Semaphore) { o.count = initial; o.maximum = maximum; }
    if (kind == ObjectKind::Reserve) o.maximum = maximum;      // (the type: 0 a user APC, 1 an I/O completion)
    if (kind == ObjectKind::IoCompletion) {
      if (!ports_ && allocator_) {
        void* memory = allocator_->allocate(sizeof(PortState) * kMaxPorts);
        if (memory) ports_ = new (memory) PortState[kMaxPorts];
      }
      int32_t slot = -1;
      for (uint32_t j = 0; ports_ && j < kMaxPorts && slot < 0; ++j) if (!ports_[j].used) slot = int32_t(j);
      void* queue_memory = slot >= 0 ? allocator_->allocate(sizeof(io::CompletionQueue)) : nullptr;
      if (slot < 0 || !queue_memory) { o = Object(); result.status = kNoResources; return result; }
      ports_[slot].used = true;
      ports_[slot].object = i + 1;
      ports_[slot].queue = new (queue_memory) io::CompletionQueue(*allocator_);
      o.port = slot;
    }
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

struct ObjectManager::PipeOp {
  uint32_t tag = 0;      // 0: no operation here
  PipeOpKind kind = PipeOpKind::None;
  uint8_t end = 0;      // the end that made the call
  uint8_t state = 0;      // 0 free, 1 pending, 2 finished, 3 finished and its result taken
  bool message_mode = false, detached = false;      // (detached: nobody waits for it; it goes when it is over)
  uint32_t status = 0, length = 0, transferred = 0;      // (length: what a read asks for, what a write has in all)
  uint64_t owner = 0;      // the process that made the call
  uint32_t request = 0;      // the IoRequest that waits for it (0: none): it ends with the operation, and takes its data
  ByteView data;      // a read that was answered: what it took
};

struct ObjectManager::Pipe {
  static constexpr uint32_t kRecords = 128, kOps = 32;
  // What one write put in the queue of its direction, oldest first (a message of a message pipe). `length` is what of it has not been read; `op` the write that is not
  // complete yet because there is no room for it (0: complete).
  struct Record { uint32_t length = 0, op = 0; };
  bool used = false;
  ByteView data[2];            // data[d]: a ring with what end d wrote and the other end has not read yet; larger than `capacity` when writes are waiting for room
  uint32_t capacity[2] = {}, head[2] = {}, count[2] = {};      // (count: all of it, the writes that wait for room included)
  Record records[2][kRecords];
  uint32_t record_head[2] = {}, record_count[2] = {};
  PipeOp ops[kOps];
  uint32_t next_tag = 1;
  uint32_t end_refs[2] = {};
  uint32_t stale_refs = 0;      // the handles of a client the server disconnected (they keep the object, and see a disconnected pipe)
  uint32_t port_object[2] = {};      // the completion port each end is tied to (a referenced port object), the key, and the FILE_SKIP_* flags
  uint64_t port_key[2] = {};
  uint32_t port_flags[2] = {};
  uint64_t process_id[2] = {};      // the processes of the server and the client
  uint64_t listen_order = 0;      // when it started to listen: the client goes to the instance that has listened longest
  bool file_signaled[2] = {true, true};      // (the event of each end's file object)
  bool anonymous = false, connected = false, listening = false, client_seen = false, disconnected = false;
  uint32_t access = 0, max_instances = 0, generation = 0;
  bool message_type = false;
  uint32_t requested_size[2] = {};      // the buffer sizes the server asked for (0 is possible; the capacity then has a default)
  // The end an end_code names; false for a client end of an earlier connection.
  bool end_of(uint32_t code, uint32_t* end) const {
    *end = code & 1;
    return *end == 0 || (code >> 1) == generation;
  }
};

namespace {
constexpr uint32_t kPipeBusy = 0xC00000AEu, kInstanceNotAvailable = 0xC00000ABu;
constexpr uint32_t kDefaultPipeBuffer = 4096, kMaxPipeBuffer = 1u << 20;
constexpr uint32_t kMaxPipeQueue = 32u << 20;      // what a direction may hold in all (the buffer and the writes that wait)
constexpr uint32_t kStatusCancelled = 0xC0000120u, kStatusBroken = 0xC000014Bu, kStatusDisconnected = 0xC00000B0u, kStatusMoreData = 0x80000005u, kStatusNoMemory = 0xC0000017u;

// Bytes of a ring (the storage, where it starts) `offset` bytes in, into `out` / from `in`.
void ring_read(ConstByteView ring, uint64_t head, uint64_t offset, ByteView out) {
  if (out.empty()) return;
  const uint64_t at = (head + offset) % ring.size(), first = ring.size() - at < out.size() ? ring.size() - at : out.size();
  out.subview(0, first).copy_from(ring.subview(at, first));
  if (first < out.size()) out.subview(first, out.size() - first).copy_from(ring.subview(0, out.size() - first));
}
void ring_write(ByteView ring, uint64_t head, uint64_t offset, ConstByteView in) {
  if (in.empty()) return;
  const uint64_t at = (head + offset) % ring.size(), first = ring.size() - at < in.size() ? ring.size() - at : in.size();
  ring.subview(at, first).copy_from(in.subview(0, first));
  if (first < in.size()) ring.subview(0, in.size() - first).copy_from(in.subview(first, in.size() - first));
}
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
      if (!allocator_) continue;
      for (ByteView& d : pipes_[i].data) release_view(*allocator_, d);
      for (PipeOp& op : pipes_[i].ops) release_view(*allocator_, op.data);
    }
    if (allocator_) allocator_->deallocate(pipes_);      // (Pipe is plain data: nothing to destroy)
  }
  if (ports_) {
    for (uint32_t i = 0; i < kMaxPorts; ++i) if (ports_[i].queue) { ports_[i].queue->~CompletionQueue(); allocator_->deallocate(ports_[i].queue); }
    if (allocator_) allocator_->deallocate(ports_);
  }
  if (requests_) { requests_->~IoRequestTable(); if (allocator_) allocator_->deallocate(requests_); }
  if (jobs_ && allocator_) allocator_->deallocate(jobs_);
  if (locks_ && allocator_) allocator_->deallocate(locks_);
  if (shares_ && allocator_) allocator_->deallocate(shares_);
  if (times_ && allocator_) allocator_->deallocate(times_);
  if (links_ && allocator_) allocator_->deallocate(links_);
  if (images_ && allocator_) allocator_->deallocate(images_);
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
  for (uint32_t& port : p.port_object) if (port != 0) { release_locked(port); port = 0; }
  if (allocator_) {
    for (ByteView& d : p.data) release_view(*allocator_, d);
    for (PipeOp& op : p.ops) release_view(*allocator_, op.data);
  }
  p = Pipe();
}

ObjectManager::Created ObjectManager::create_pipe(StringView name, uint32_t out_size, uint32_t in_size, uint32_t max_instances, uint32_t access, bool message_type, uint64_t pid, uint32_t disposition) {
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
    const Pipe* first = nullptr;
    for (Object& o : objects_) {
      if (o.kind != ObjectKind::Pipe || o.name_size == 0 || !same_name(o.name, o.name_size, name) || pipes_[o.pipe].end_refs[0] == 0) continue;      // (an instance with no server end left is gone, though a client may still hold it)
      ++instances;
      if (!first) first = &pipes_[o.pipe];
      if (pipes_[o.pipe].max_instances != 0 && instances >= pipes_[o.pipe].max_instances) { result.status = kInstanceNotAvailable; return result; }      // (the limit the pipe was made with)
    }
    if (max_instances != 0 && instances >= max_instances) { result.status = kInstanceNotAvailable; return result; }
    if (first && (disposition == 2 || first->access != access)) { result.status = 0xC0000022u; return result; }      // FILE_CREATE: the name is taken; every instance of a pipe has the same access (STATUS_ACCESS_DENIED)
    if (disposition == 1 && instances == 0) { result.status = 0xC0000034u; return result; }      // FILE_OPEN: STATUS_OBJECT_NAME_NOT_FOUND
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
      p.data[d] = allocate_view<uint8_t>(*allocator_, p.capacity[d]);
      if (p.data[d].empty()) {
        for (ByteView& freed : p.data) release_view(*allocator_, freed);
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
    p.message_type = message_type && !p.anonymous;
    p.requested_size[0] = out_size;
    p.requested_size[1] = in_size;
    p.end_refs[0] = 1;
    p.process_id[0] = p.process_id[1] = pid;
    p.listen_order = ++pipe_sequence_;
    if (p.anonymous) p.end_refs[1] = 0;
    o = Object();
    o.kind = ObjectKind::Pipe;
    o.references = 1;
    for (uint32_t k = 0; k < name.size(); ++k) o.name[k] = name[k];
    o.name_size = static_cast<uint32_t>(name.size());
    o.pipe = pipe_index;
    sync_.notify_all();      // (a new instance takes the clients that wait for one)
    result.id = i + 1;
    return result;
  }
  result.status = kNoResources;
  return result;
}

ObjectManager::Created ObjectManager::connect_pipe(StringView name, bool want_read, bool want_write, uint64_t pid) {
  Created result;
  Locked hold(sync_);
  bool found = false;
  int32_t best = -1;      // the instance that has been listening longest
  for (uint32_t i = 0; i < kCapacity; ++i) {
    Object& o = objects_[i];
    if (o.kind != ObjectKind::Pipe || o.name_size == 0 || !same_name(o.name, o.name_size, name)) continue;
    found = true;
    const Pipe& p = pipes_[o.pipe];
    if (p.connected || p.disconnected || p.end_refs[0] == 0) continue;      // (an instance its server disconnected takes no client until it listens again)
    if (best < 0 || p.listen_order < pipes_[objects_[best].pipe].listen_order) best = static_cast<int32_t>(i);
  }
  if (best >= 0) {
    Object& o = objects_[best];
    Pipe& p = pipes_[o.pipe];
    if ((want_read && !(p.access & kPipeOutbound)) || (want_write && !(p.access & kPipeInbound))) { result.status = 0xC0000022u; return result; }
    p.connected = true;
    p.client_seen = true;
    p.disconnected = false;
    p.end_refs[1] = 1;
    p.process_id[1] = pid;
    if (p.port_object[1] != 0) { release_locked(p.port_object[1]); p.port_object[1] = 0; p.port_flags[1] = 0; }      // (the port was tied to the client before)
    ++p.generation;
    ++o.references;
    for (PipeOp& op : p.ops) if (op.state == 1 && op.kind == PipeOpKind::Listen) pipe_finish_locked(p, op, 0, 0);      // (a ConnectNamedPipe that waits is over)
    p.listening = false;
    p.file_signaled[1] = true;      // (a new file object)
    sync_.notify_all();
    result.id = static_cast<uint32_t>(best) + 1;
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
  if (!o || !p) return;
  if (p->end_of(end_code, &end) && p->end_refs[end] > 0) ++p->end_refs[end];
  else if ((end_code & 1) && !p->anonymous) ++p->stale_refs;      // (a handle of a client the server disconnected, or of an earlier client)
  else if (p->end_of(end_code, &end)) ++p->end_refs[end];
  else return;
  ++o->references;
}

void ObjectManager::release_pipe_end(uint32_t id, uint32_t end_code) {
  Locked hold(sync_);
  Object* o = at(id);
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!o || !p) return;
  if (!p->end_of(end_code, &end) || p->end_refs[end] == 0) {      // a handle of a client the server disconnected: it only holds the object
    if ((end_code & 1) && p->stale_refs > 0) {
      --p->stale_refs;
      if (o->references > 0 && --o->references == 0) { free_pipe_locked(*o); *o = Object(); }
    }
    return;
  }
  --p->end_refs[end];
  if (p->end_refs[end] == 0) {      // what this end waits for is given up, and the other end gets nothing more from it
    pipe_fail_ops_locked(*p, static_cast<int>(end), kStatusCancelled);
    pipe_fail_ops_locked(*p, static_cast<int>(1 - end), kStatusBroken);
    pipe_clear_queue_locked(*p, 1 - end);
    if (p->port_object[end] != 0) { release_locked(p->port_object[end]); p->port_object[end] = 0; }
  }
  sync_.notify_all();      // the other end sees the end of the data
  if (o->references > 0 && --o->references == 0) {
    free_pipe_locked(*o);
    *o = Object();
  }
}

// ---- the queues and operations of a pipe

bool ObjectManager::pipe_grow_locked(Pipe& p, uint32_t q, uint64_t needed) {
  const uint64_t have = p.data[q].size();
  if (needed <= have) return true;
  if (!allocator_ || needed > kMaxPipeQueue) return false;
  uint64_t size = have ? have : 1;
  while (size < needed) size *= 2;      // (needed is at most kMaxPipeQueue)
  if (size > kMaxPipeQueue) size = kMaxPipeQueue;
  ByteView bigger = allocate_view<uint8_t>(*allocator_, size);
  if (bigger.empty()) return false;
  if (p.count[q] != 0) ring_read(ConstByteView(p.data[q]), p.head[q], 0, bigger.first(p.count[q]));
  release_view(*allocator_, p.data[q]);
  p.data[q] = bigger;
  p.head[q] = 0;
  return true;
}

// Puts `data` at the end of the queue of direction q as a record (of the write `op`, 0 if it is complete). False if there is no room for that in the tables.
bool ObjectManager::pipe_append_locked(Pipe& p, uint32_t q, ConstByteView data, uint32_t op) {
  if (p.data[q].empty() || data.size() > kMaxPipeQueue) return false;
  const bool merge = !p.message_type && op == 0 && !data.empty() && p.record_count[q] > 0 &&
                     p.records[q][(p.record_head[q] + p.record_count[q] - 1) % Pipe::kRecords].op == 0;      // (a byte pipe has no messages: complete writes are one run)
  if (!merge && p.record_count[q] == Pipe::kRecords) return false;
  if (!pipe_grow_locked(p, q, uint64_t{p.count[q]} + data.size())) return false;
  ring_write(p.data[q], p.head[q], p.count[q], data);
  p.count[q] += static_cast<uint32_t>(data.size());
  if (merge) {
    p.records[q][(p.record_head[q] + p.record_count[q] - 1) % Pipe::kRecords].length += static_cast<uint32_t>(data.size());
  } else {
    Pipe::Record& r = p.records[q][(p.record_head[q] + p.record_count[q]) % Pipe::kRecords];
    r.length = static_cast<uint32_t>(data.size());
    r.op = op;
    ++p.record_count[q];
  }
  return true;
}

ObjectManager::PipeOp* ObjectManager::pipe_new_op_locked(Pipe& p, PipeOpKind kind, uint32_t end, uint64_t owner) {
  for (PipeOp& op : p.ops) {
    if (op.state != 0) continue;
    op = PipeOp();
    op.tag = p.next_tag++;
    if (p.next_tag == 0) p.next_tag = 1;
    op.kind = kind;
    op.end = static_cast<uint8_t>(end);
    op.state = 1;
    op.owner = owner;
    return &op;
  }
  return nullptr;
}

ObjectManager::PipeOp* ObjectManager::pipe_find_op_locked(Pipe& p, uint32_t tag) {
  if (tag == 0) return nullptr;
  for (PipeOp& op : p.ops) if (op.state != 0 && op.tag == tag) return &op;
  return nullptr;
}

void ObjectManager::pipe_free_op_locked(Pipe& p, PipeOp& op) {
  if (allocator_) release_view(*allocator_, op.data);
  op = PipeOp();
  sync_.notify_all();
}

void ObjectManager::pipe_finish_locked(Pipe& p, PipeOp& op, uint32_t status, uint32_t transferred) {
  if (op.request != 0) {      // the call waits in the kernel for its answer: the request ends, and its owner applies the outcome
    ByteView data = op.data;
    op.data = ByteView();
    const uint32_t request = op.request;
    op.request = 0;
    if (!requests_ || !requests_->complete(request, status, transferred, data)) { if (allocator_) release_view(*allocator_, data); }
    pipe_free_op_locked(p, op);
    return;
  }
  op.state = 2;
  op.status = status;
  op.transferred = transferred;
  if (op.detached) pipe_free_op_locked(p, op);
  sync_.notify_all();
}

void ObjectManager::pipe_pop_record_locked(Pipe& p, uint32_t q) {
  if (p.record_count[q] == 0) return;
  Pipe::Record& first = p.records[q][p.record_head[q]];
  if (first.op != 0) {      // (a write that was read to the end is complete)
    PipeOp* op = pipe_find_op_locked(p, first.op);
    if (op && op->state == 1) pipe_finish_locked(p, *op, 0, op->length);
    first.op = 0;
  }
  p.record_head[q] = (p.record_head[q] + 1) % Pipe::kRecords;
  --p.record_count[q];
}

// n bytes were read from the front of the queue.
void ObjectManager::pipe_consume_locked(Pipe& p, uint32_t q, uint32_t n) {
  if (n == 0) return;
  p.head[q] = static_cast<uint32_t>((uint64_t{p.head[q]} + n) % p.data[q].size());
  p.count[q] -= n;
  uint32_t left = n;
  while (left > 0 && p.record_count[q] > 0) {
    Pipe::Record& first = p.records[q][p.record_head[q]];
    if (first.length > left) { first.length -= left; left = 0; break; }
    left -= first.length;
    pipe_pop_record_locked(p, q);
  }
  if (p.count[q] == 0) {
    p.head[q] = 0;
    if (allocator_ && p.capacity[q] != 0 && p.data[q].size() > p.capacity[q]) {      // (what grew for writes that waited goes back)
      ByteView smaller = allocate_view<uint8_t>(*allocator_, p.capacity[q]);
      if (!smaller.empty()) { release_view(*allocator_, p.data[q]); p.data[q] = smaller; }
    }
  }
}

// Reads from the queue of direction q into `out` (at most its size): the first message only in message mode, or when peeking at a message pipe. `more`: part of the message is
// left. The queue must hold something (bytes, or a message even if it is empty).
uint32_t ObjectManager::pipe_take_locked(Pipe& p, uint32_t q, ByteView out, bool message_mode, bool peek, bool* more, uint32_t* front_length) {
  const bool messages = p.message_type && p.record_count[q] > 0;
  const uint32_t front = messages ? p.records[q][p.record_head[q]].length : 0;
  if (front_length) *front_length = front;
  uint32_t limit = p.count[q];
  if (messages && (message_mode || peek) && front < limit) limit = front;
  const uint32_t n = limit < out.size() ? limit : static_cast<uint32_t>(out.size());
  ring_read(ConstByteView(p.data[q]), p.head[q], 0, out.first(n));
  *more = messages && message_mode && front > n;
  if (!peek) {
    pipe_consume_locked(p, q, n);
    if (n == 0 && messages && front == 0) pipe_pop_record_locked(p, q);      // (an empty message is read as nothing)
  }
  return n;
}

// Something changed in the queue of direction q: the reads that wait take their share in the order they were made, the writes that fit now are complete, and a flush is
// over when everything was read.
void ObjectManager::pipe_settle_locked(Pipe& p, uint32_t q) {
  const uint32_t reader = 1 - q;
  for (;;) {
    PipeOp* op = nullptr;
    for (PipeOp& c : p.ops) if (c.state == 1 && c.kind == PipeOpKind::Read && c.end == reader && (!op || c.tag < op->tag)) op = &c;
    if (!op) break;
    const bool messages = p.message_type && p.record_count[q] > 0;
    if (p.count[q] == 0 && !messages) break;
    ByteView held;
    const uint32_t want = op->length < p.count[q] ? op->length : p.count[q];
    if (want != 0) {
      held = allocator_ ? allocate_view<uint8_t>(*allocator_, want) : ByteView();
      if (held.empty()) { pipe_finish_locked(p, *op, kStatusNoMemory, 0); continue; }
    }
    bool more = false;
    const uint32_t n = pipe_take_locked(p, q, held, op->message_mode, false, &more, nullptr);
    op->data = held;
    pipe_finish_locked(p, *op, more ? kStatusMoreData : 0, n);
  }
  uint64_t available = 0;
  for (uint32_t i = 0; i < p.record_count[q]; ++i) {
    Pipe::Record& r = p.records[q][(p.record_head[q] + i) % Pipe::kRecords];
    available += r.length;
    if (r.op == 0) continue;
    PipeOp* op = pipe_find_op_locked(p, r.op);
    if (!op || op->state != 1) { r.op = 0; continue; }
    if (available <= p.capacity[q]) { pipe_finish_locked(p, *op, 0, op->length); r.op = 0; }
  }
  if (p.count[q] == 0) {
    for (PipeOp& c : p.ops) if (c.state == 1 && c.kind == PipeOpKind::Flush && c.end == q) pipe_finish_locked(p, c, 0, 0);
  }
  sync_.notify_all();
}

void ObjectManager::pipe_clear_queue_locked(Pipe& p, uint32_t q) {
  p.head[q] = p.count[q] = 0;
  p.record_head[q] = p.record_count[q] = 0;
}

// A pending operation ends with `status`; a write takes its bytes back out of the queue (the ones that were read stay read).
void ObjectManager::pipe_cancel_op_locked(Pipe& p, PipeOp& op, uint32_t status) {
  if (op.state != 1) return;
  const uint32_t q = op.end;
  if (op.kind == PipeOpKind::Write && p.record_count[q] > 0 && !p.data[q].empty()) {
    uint64_t offset = 0;
    for (uint32_t i = 0; i < p.record_count[q]; ++i) {
      Pipe::Record& r = p.records[q][(p.record_head[q] + i) % Pipe::kRecords];
      if (r.op != op.tag) { offset += r.length; continue; }
      const uint32_t length = r.length;
      for (uint64_t at = offset; at + length < p.count[q]; ++at)      // (the bytes after it move down)
        p.data[q][(p.head[q] + at) % p.data[q].size()] = p.data[q][(p.head[q] + at + length) % p.data[q].size()];
      p.count[q] -= length;
      for (uint32_t k = i; k + 1 < p.record_count[q]; ++k) p.records[q][(p.record_head[q] + k) % Pipe::kRecords] = p.records[q][(p.record_head[q] + k + 1) % Pipe::kRecords];
      --p.record_count[q];
      break;
    }
  }
  const PipeOpKind kind = op.kind;      // (a detached operation is gone when it is finished)
  pipe_finish_locked(p, op, status, 0);
  if (kind == PipeOpKind::Listen) {
    bool any = false;
    for (const PipeOp& c : p.ops) if (c.state == 1 && c.kind == PipeOpKind::Listen) any = true;
    p.listening = any;
  }
  if (kind == PipeOpKind::Write || kind == PipeOpKind::Flush) pipe_settle_locked(p, q);
}

// Every pending operation of an end (-1: of both) ends with `status`.
void ObjectManager::pipe_fail_ops_locked(Pipe& p, int end, uint32_t status) {
  for (PipeOp& op : p.ops) if (op.state == 1 && op.kind == PipeOpKind::Flush && (end < 0 || op.end == end)) pipe_cancel_op_locked(p, op, status);      // (before the writes they wait for are given up: that would end them well)
  for (PipeOp& op : p.ops) if (op.state == 1 && (end < 0 || op.end == end)) pipe_cancel_op_locked(p, op, status);
}

ObjectManager::PipeResult ObjectManager::pipe_read_locked(uint32_t id, uint32_t end_code, uint8_t* out, uint32_t capacity, uint32_t* got, bool peek, uint32_t* available,
                                                          bool message_mode, uint32_t* message_length) {
  *got = 0;
  if (message_length) *message_length = 0;
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (p && !p->end_of(end_code, &end) && (end_code & 1)) return PipeResult::NotConnected;      // (a client of an earlier connection)
  if (!p || !p->end_of(end_code, &end)) return PipeResult::Broken;
  if (end == 1 && !p->connected) return p->disconnected ? PipeResult::NotConnected : PipeResult::Broken;
  const uint32_t q = 1 - end;      // the queue the other end writes
  if (available) *available = p->count[q];
  if (end == 0 && !p->connected) return p->client_seen ? PipeResult::Broken : (p->disconnected ? PipeResult::NotConnected : PipeResult::Listening);
  if (p->count[q] == 0 && !(p->message_type && p->record_count[q] > 0)) return p->end_refs[1 - end] == 0 || (end == 0 && !p->connected) ? PipeResult::Broken : PipeResult::WouldBlock;
  bool more = false;
  uint32_t front = 0;
  const uint32_t n = pipe_take_locked(*p, q, capacity ? ByteView(out, capacity) : ByteView(), message_mode, peek, &more, &front);
  if (message_length) *message_length = front;
  if (!peek) pipe_settle_locked(*p, q);      // room for the writer
  *got = n;
  return more ? PipeResult::MoreData : PipeResult::Ok;
}

ObjectManager::PipeResult ObjectManager::pipe_write_locked(uint32_t id, uint32_t end_code, const uint8_t* data, uint32_t count, uint32_t* done) {
  *done = 0;
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end)) return PipeResult::Closing;
  if (!p->connected) {
    if (p->disconnected) return PipeResult::NotConnected;
    return end == 0 ? (p->client_seen ? PipeResult::Closing : PipeResult::Listening) : PipeResult::Closing;
  }
  if (p->end_refs[1 - end] == 0) return PipeResult::Closing;
  const uint32_t q = end;
  if (p->data[q].empty() || p->capacity[q] == 0) return PipeResult::Broken;
  if (count == 0) {      // an empty write: a message of no bytes on a message pipe, nothing on a byte one
    if (p->message_type) pipe_append_locked(*p, q, ConstByteView(), 0);
    pipe_settle_locked(*p, q);
    return PipeResult::Ok;
  }
  const uint32_t room = p->capacity[q] > p->count[q] ? p->capacity[q] - p->count[q] : 0;
  if (room == 0 || p->record_count[q] == Pipe::kRecords) return PipeResult::WouldBlock;
  const uint32_t n = room < count ? room : count;
  if (!pipe_append_locked(*p, q, ConstByteView(data, n), 0)) return PipeResult::WouldBlock;
  *done = n;
  pipe_settle_locked(*p, q);      // data for the reader
  return PipeResult::Ok;
}

ObjectManager::PipeResult ObjectManager::pipe_listen_locked(uint32_t id) {
  Pipe* p = pipe_of(id);
  if (!p) return PipeResult::Broken;
  if (p->connected) return p->end_refs[1] == 0 ? PipeResult::Closing : PipeResult::Ok;      // (Closing: the client came and went)
  if (p->disconnected) { p->listen_order = ++pipe_sequence_; sync_.notify_all(); }
  p->listening = true;
  p->disconnected = false;
  return PipeResult::WouldBlock;
}

ObjectManager::PipeResult ObjectManager::pipe_begin_read_locked(uint32_t id, uint32_t end_code, uint32_t length, bool message_mode, uint64_t owner, uint32_t* tag) {
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end)) return PipeResult::Broken;
  uint32_t got = 0;
  const PipeResult now = pipe_read_locked(id, end_code, nullptr, 0, &got, true, nullptr, message_mode);
  if (now != PipeResult::WouldBlock) return now;      // (it can be answered: by the call itself)
  PipeOp* op = pipe_new_op_locked(*p, PipeOpKind::Read, end, owner);
  if (!op) return PipeResult::NoResources;
  op->length = length;
  op->message_mode = message_mode;
  *tag = op->tag;
  return PipeResult::WouldBlock;
}

ObjectManager::PipeResult ObjectManager::pipe_begin_write_locked(uint32_t id, uint32_t end_code, ConstByteView data, uint64_t owner, uint32_t* tag, uint32_t* done) {
  *done = 0;
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end)) return PipeResult::Closing;
  if (!p->connected) {
    if (p->disconnected) return PipeResult::NotConnected;
    return end == 0 ? (p->client_seen ? PipeResult::Closing : PipeResult::Listening) : PipeResult::Closing;
  }
  if (p->end_refs[1 - end] == 0) return PipeResult::Closing;
  const uint32_t q = end;
  if (p->data[q].empty() || p->capacity[q] == 0) return PipeResult::Broken;
  if (data.size() > kMaxPipeWrite) return PipeResult::NoResources;
  if (data.empty()) {      // an empty write is never kept waiting
    if (p->message_type) pipe_append_locked(*p, q, data, 0);
    pipe_settle_locked(*p, q);
    return PipeResult::Ok;
  }
  if (p->record_count[q] == Pipe::kRecords) return PipeResult::WouldBlock;
  PipeOp* op = pipe_new_op_locked(*p, PipeOpKind::Write, end, owner);
  if (!op) return PipeResult::NoResources;
  if (!pipe_append_locked(*p, q, data, op->tag)) { pipe_free_op_locked(*p, *op); return PipeResult::NoResources; }
  op->length = static_cast<uint32_t>(data.size());
  pipe_settle_locked(*p, q);      // the reads that wait take their share, and the write may fit then
  if (op->state == 2) {
    *done = op->length;
    pipe_free_op_locked(*p, *op);
    return PipeResult::Ok;
  }
  *tag = op->tag;
  return PipeResult::WouldBlock;
}

ObjectManager::PipeResult ObjectManager::pipe_begin_transact_locked(uint32_t id, uint32_t end_code, ConstByteView data, uint32_t read_length, bool message_mode, uint64_t owner,
                                                                    uint32_t* tag) {
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end)) return PipeResult::Closing;
  if (!p->connected) {
    if (p->disconnected) return PipeResult::NotConnected;
    return end == 0 ? (p->client_seen ? PipeResult::Closing : PipeResult::Listening) : PipeResult::Closing;
  }
  if (!p->message_type || !message_mode) return PipeResult::InvalidState;
  if (p->end_refs[1 - end] == 0) return PipeResult::Closing;
  const uint32_t q = end, reader = 1 - end;
  if (p->data[q].empty() || p->capacity[q] == 0) return PipeResult::Broken;
  if (p->count[reader] != 0 || p->record_count[reader] != 0) return PipeResult::Busy;
  if (data.size() > kMaxPipeWrite || p->record_count[q] == Pipe::kRecords) return PipeResult::NoResources;
  PipeOp* read = pipe_new_op_locked(*p, PipeOpKind::Read, end, owner);
  if (!read) return PipeResult::NoResources;
  const uint32_t read_tag = read->tag;
  read->length = read_length;
  read->message_mode = true;
  PipeOp* write = data.empty() ? nullptr : pipe_new_op_locked(*p, PipeOpKind::Write, end, owner);
  if (!data.empty() && !write) { pipe_free_op_locked(*p, *read); return PipeResult::NoResources; }
  if (!pipe_append_locked(*p, q, data, write ? write->tag : 0)) {
    if (write) pipe_free_op_locked(*p, *write);
    pipe_free_op_locked(*p, *read);
    return PipeResult::NoResources;
  }
  if (write) { write->length = static_cast<uint32_t>(data.size()); write->detached = true; }
  pipe_settle_locked(*p, q);      // the other end's reads that wait take the message
  *tag = read_tag;
  return PipeResult::WouldBlock;
}

ObjectManager::PipeResult ObjectManager::pipe_begin_listen_locked(uint32_t id, uint64_t owner, uint32_t* tag) {
  Pipe* p = pipe_of(id);
  if (!p) return PipeResult::Broken;
  if (p->connected) return PipeResult::Ok;
  PipeOp* op = pipe_new_op_locked(*p, PipeOpKind::Listen, 0, owner);
  if (!op) return PipeResult::NoResources;
  if (p->disconnected) { p->listen_order = ++pipe_sequence_; sync_.notify_all(); }      // (it can take a client again, after the ones that listened before)
  p->listening = true;
  p->disconnected = false;
  *tag = op->tag;
  return PipeResult::WouldBlock;
}

ObjectManager::PipeResult ObjectManager::pipe_begin_flush_locked(uint32_t id, uint32_t end_code, uint64_t owner, uint32_t* tag) {
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end) || p->end_refs[1 - end] == 0 || !p->connected || p->count[end] == 0) return PipeResult::Ok;
  PipeOp* op = pipe_new_op_locked(*p, PipeOpKind::Flush, end, owner);
  if (!op) return PipeResult::NoResources;
  *tag = op->tag;
  return PipeResult::WouldBlock;
}

ObjectManager::PipeOpPoll ObjectManager::pipe_op_poll_locked(uint32_t id, uint32_t end_code, uint32_t tag, PipeOpResult* result) {
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end)) return PipeOpPoll::Missing;
  PipeOp* op = pipe_find_op_locked(*p, tag);
  if (!op || op->end != end || op->state == 3) return PipeOpPoll::Missing;
  if (op->state == 1) return PipeOpPoll::Pending;
  result->status = op->status;
  result->transferred = op->transferred;
  result->data = op->data;
  op->data = ByteView();
  op->state = 3;
  return PipeOpPoll::Done;
}

ObjectManager::PipeResult ObjectManager::pipe_op_cancel_locked(uint32_t id, uint32_t end_code, uint32_t tag, uint32_t status) {
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end)) return PipeResult::Broken;
  PipeOp* op = pipe_find_op_locked(*p, tag);
  if (!op || op->end != end || op->state != 1) return PipeResult::Broken;
  pipe_cancel_op_locked(*p, *op, status);
  return PipeResult::Ok;
}

void ObjectManager::pipe_op_release(uint32_t id, uint32_t tag) {
  Locked hold(sync_);
  Pipe* p = pipe_of(id);
  if (!p) return;
  if (PipeOp* op = pipe_find_op_locked(*p, tag)) pipe_free_op_locked(*p, *op);
}

void ObjectManager::pipe_op_free_data(ByteView& data) {
  if (allocator_) release_view(*allocator_, data);
}

void ObjectManager::pipe_owner_gone(uint64_t pid) {
  io_owner_gone(pid);
  Locked hold(sync_);
  if (!pipes_) return;
  for (uint32_t i = 0; i < kMaxPipes; ++i) {
    Pipe& p = pipes_[i];
    if (!p.used) continue;
    for (PipeOp& op : p.ops) {
      if (op.state == 0 || op.owner != pid) continue;
      if (op.state == 1) pipe_cancel_op_locked(p, op, kStatusCancelled);
      if (op.state != 0) pipe_free_op_locked(p, op);
    }
  }
}

bool ObjectManager::pipe_set_port(uint32_t id, uint32_t end_code, uint32_t port_object, uint64_t key) {
  Locked hold(sync_);
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  Object* port = at(port_object);
  if (!p || !p->end_of(end_code, &end) || !port || port->kind != ObjectKind::IoCompletion) return false;
  if (p->port_object[end] != 0) return false;      // (an end is tied once)
  ++port->references;
  p->port_object[end] = port_object;
  p->port_key[end] = key;
  p->port_flags[end] = 0;
  return true;
}

void ObjectManager::pipe_set_port_flags(uint32_t id, uint32_t end_code, uint32_t flags) {
  Locked hold(sync_);
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end)) return;
  p->port_flags[end] = flags;
  if (flags & 2) { p->file_signaled[end] = false; sync_.notify_all(); }      // FILE_SKIP_SET_EVENT_ON_HANDLE: the file object is never signalled again
}

bool ObjectManager::pipe_port(uint32_t id, uint32_t end_code, uint32_t* port_object, uint64_t* key, uint32_t* flags) {
  Locked hold(sync_);
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end) || p->port_object[end] == 0) return false;
  *port_object = p->port_object[end];
  *key = p->port_key[end];
  *flags = p->port_flags[end];
  return true;
}

bool ObjectManager::pipe_pend_locked(uint32_t id, uint32_t end_code, uint32_t tag, const io::IoRequest& request, bool signal_file) {
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end)) return false;
  PipeOp* op = pipe_find_op_locked(*p, tag);
  if (!op || op->end != end || op->state != 1) return false;
  io::IoRequest* r = io_add_locked(request);
  if (!r) {
    pipe_cancel_op_locked(*p, *op, kStatusCancelled);
    if (PipeOp* left = pipe_find_op_locked(*p, tag)) pipe_free_op_locked(*p, *left);
    return false;
  }
  r->source = id;
  r->source_end = end;
  r->source_tag = tag;
  op->request = r->id;
  (void)signal_file;
  p->file_signaled[end] = false;      // (the file object is not signalled while a call is under way; one that has no event of its own signals it when it ends)
  return true;
}

uint32_t ObjectManager::pipe_sync_begin(uint32_t id, uint32_t end_code, uint64_t thread, uint64_t iosb) {
  Locked hold(sync_);
  for (uint32_t i = 0; i < kMaxSyncWaits; ++i) {
    SyncWait& w = sync_waits_[i];
    if (w.used) continue;
    w = SyncWait();
    w.used = true;
    w.thread = thread;
    w.iosb = iosb;
    w.pipe = id;
    w.end_code = end_code;
    Pipe* p = pipe_of(id);
    uint32_t end = 0;
    if (p && p->end_of(end_code, &end)) p->file_signaled[end] = false;      // (a call is under way)
    return i + 1;
  }
  return 0;
}

bool ObjectManager::pipe_sync_cancelled_locked(uint32_t slot) { return slot >= 1 && slot <= kMaxSyncWaits && sync_waits_[slot - 1].used && sync_waits_[slot - 1].cancelled; }

void ObjectManager::pipe_sync_end(uint32_t slot) {
  Locked hold(sync_);
  if (slot < 1 || slot > kMaxSyncWaits || !sync_waits_[slot - 1].used) return;
  SyncWait& w = sync_waits_[slot - 1];
  Pipe* p = pipe_of(w.pipe);
  uint32_t end = 0;
  if (p && p->end_of(w.end_code, &end)) p->file_signaled[end] = true;
  w = SyncWait();
  sync_.notify_all();
}

bool ObjectManager::pipe_cancel_synchronous(uint64_t thread, uint64_t iosb) {
  Locked hold(sync_);
  bool found = false;
  for (SyncWait& w : sync_waits_) if (w.used && w.thread == thread && !w.cancelled && (iosb == 0 || w.iosb == 0 || w.iosb == iosb)) { w.cancelled = true; found = true; }
  if (found) sync_.notify_all();
  return found;
}

bool ObjectManager::pipe_disconnect(uint32_t id) {
  Locked hold(sync_);
  Object* o = at(id);
  Pipe* p = pipe_of(id);
  if (!o || !p || p->anonymous) return false;
  const bool already = p->disconnected;      // (a second disconnect is an error; one after the client has gone is not)
  p->disconnected = true;
  if (p->connected) {
    p->connected = false;
    p->stale_refs += p->end_refs[1];      // (the client's handles stay in its table and see a disconnected pipe; they keep the object alive)
    p->end_refs[1] = 0;
  }
  p->listening = false;
  p->client_seen = false;
  if (p->port_object[1] != 0) { release_locked(p->port_object[1]); p->port_object[1] = 0; p->port_flags[1] = 0; }
  pipe_fail_ops_locked(*p, -1, kStatusDisconnected);
  pipe_clear_queue_locked(*p, 0);
  pipe_clear_queue_locked(*p, 1);
  sync_.notify_all();
  return !already;
}

uint32_t ObjectManager::pipe_room_locked(uint32_t id, uint32_t end_code) {
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end) || p->data[end].empty() || p->capacity[end] == 0) return 0;
  return p->capacity[end] > p->count[end] ? p->capacity[end] - p->count[end] : 0;
}

uint32_t ObjectManager::pipe_pending_locked(uint32_t id, uint32_t end_code) {
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end) || p->end_refs[1 - end] == 0 || !p->connected) return 0;
  return p->count[end];
}

uint32_t ObjectManager::pipe_state_locked(uint32_t id, uint32_t end_code) {
  Pipe* p = pipe_of(id);
  if (!p) return 0;
  uint32_t end = 0;
  const bool valid = p->end_of(end_code, &end) && p->end_refs[end] > 0;
  if (p->anonymous) return p->end_refs[end ? 0 : 1] == 0 ? 4 : 3;
  if (end_code & 1) {      // a client
    if (!valid || !p->connected) return 1;
    return p->end_refs[0] == 0 ? 4 : 3;
  }
  if (p->connected) return p->end_refs[1] == 0 ? 4 : 3;
  return p->disconnected ? 1 : 2;
}

uint32_t ObjectManager::pipe_end_refs_locked(uint32_t id, uint32_t end_code) {
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  return p && p->end_of(end_code, &end) ? p->end_refs[end] : 0;
}

bool ObjectManager::pipe_file_signaled_locked(uint32_t id, uint32_t end_code) {
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  return !p || !p->end_of(end_code, &end) || p->file_signaled[end];
}

void ObjectManager::pipe_set_file_signaled(uint32_t id, uint32_t end_code, bool signaled) {
  Locked hold(sync_);
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end)) return;
  p->file_signaled[end] = signaled;
  sync_.notify_all();
}

bool ObjectManager::pipe_client_gone_locked(uint32_t id, uint32_t end_code) {
  Pipe* p = pipe_of(id);
  return p && (end_code & 1) && !p->connected && p->disconnected;
}

uint32_t ObjectManager::object_name(uint32_t id, char* out, uint32_t capacity) {
  Locked hold(sync_);
  const Object* o = at(id);
  if (!o || o->name_size == 0 || o->name_size > capacity) return 0;
  for (uint32_t i = 0; i < o->name_size; ++i) out[i] = o->name[i];
  return o->name_size;
}

bool ObjectManager::pipe_name_exists_locked(StringView name) {
  for (Object& o : objects_) if (o.kind == ObjectKind::Pipe && o.name_size != 0 && same_name(o.name, o.name_size, name)) return true;
  return false;
}

bool ObjectManager::pipe_name_available_locked(StringView name) {
  for (Object& o : objects_) {
    if (o.kind != ObjectKind::Pipe || o.name_size == 0 || !same_name(o.name, o.name_size, name)) continue;
    const Pipe& p = pipes_[o.pipe];
    if (!p.connected && !p.disconnected && p.end_refs[0] != 0) return true;
  }
  return false;
}

ObjectManager::PipeResult ObjectManager::pipe_process(uint32_t id, uint32_t end_code, uint32_t which, uint64_t* pid) {
  Locked hold(sync_);
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end)) return PipeResult::Broken;
  if (!p->connected && (end == 1 || which == 0)) return end == 1 ? PipeResult::NotConnected : PipeResult::Listening;
  *pid = p->process_id[which ? 0 : 1];
  return PipeResult::Ok;
}

ObjectManager::PipeResult ObjectManager::pipe_write_nowait_locked(uint32_t id, uint32_t end_code, ConstByteView data, uint32_t* done) {
  *done = 0;
  Pipe* p = pipe_of(id);
  uint32_t end = 0;
  if (!p || !p->end_of(end_code, &end)) return PipeResult::Closing;
  if (!p->connected) {
    if (p->disconnected) return PipeResult::NotConnected;
    return end == 0 ? (p->client_seen ? PipeResult::Closing : PipeResult::Listening) : PipeResult::Closing;
  }
  if (p->end_refs[1 - end] == 0) return PipeResult::Closing;
  const uint32_t q = end, reader = 1 - end;
  if (p->data[q].empty() || p->capacity[q] == 0) return PipeResult::Broken;
  if (data.empty()) {
    if (p->message_type) pipe_append_locked(*p, q, data, 0);
    pipe_settle_locked(*p, q);
    return PipeResult::Ok;
  }
  if (data.size() > kMaxPipeWrite || p->record_count[q] == Pipe::kRecords) return PipeResult::Ok;
  if (p->message_type) {      // a message goes whole or not at all: what the reads that wait leave of it must fit
    uint64_t left = data.size();
    uint32_t last = 0;
    for (;;) {
      const PipeOp* next = nullptr;
      for (const PipeOp& c : p->ops) if (c.state == 1 && c.kind == PipeOpKind::Read && c.end == reader && c.tag > last && (!next || c.tag < next->tag)) next = &c;
      if (!next || left == 0) break;
      last = next->tag;
      left -= next->length < left ? next->length : left;
    }
    const uint32_t room = p->capacity[q] > p->count[q] ? p->capacity[q] - p->count[q] : 0;
    if (left > room) return PipeResult::Ok;
  }
  PipeOp* op = pipe_new_op_locked(*p, PipeOpKind::Write, end, 0);
  if (!op) return PipeResult::Ok;
  if (!pipe_append_locked(*p, q, data, op->tag)) { pipe_free_op_locked(*p, *op); return PipeResult::Ok; }
  op->length = static_cast<uint32_t>(data.size());
  const uint32_t tag = op->tag;
  pipe_settle_locked(*p, q);      // the reads that wait take their share
  op = pipe_find_op_locked(*p, tag);
  if (op->state == 2) { *done = op->length; pipe_free_op_locked(*p, *op); return PipeResult::Ok; }
  uint32_t left = 0;      // what of the write is still queued is taken back
  for (uint32_t i = 0; i < p->record_count[q]; ++i) {
    const Pipe::Record& r = p->records[q][(p->record_head[q] + i) % Pipe::kRecords];
    if (r.op == tag) left = r.length;
  }
  *done = op->length - left;
  pipe_cancel_op_locked(*p, *op, kStatusCancelled);
  pipe_free_op_locked(*p, *op);
  return PipeResult::Ok;
}

bool ObjectManager::pipe_info(uint32_t id, PipeInfo* out) {
  Locked hold(sync_);
  Object* o = at(id);
  Pipe* p = pipe_of(id);
  if (!o || !p) return false;
  out->anonymous = p->anonymous;
  out->message_type = p->message_type;
  out->connected = p->connected;
  out->access = p->access;
  out->out_size = p->anonymous ? p->capacity[0] : p->requested_size[0];
  out->in_size = p->anonymous ? p->capacity[1] : p->requested_size[1];
  out->max_instances = p->max_instances;
  out->instances = 0;      // (an instance whose server end is closed is no instance any more, though a client may still hold it)
  for (const Object& other : objects_) if (other.kind == ObjectKind::Pipe && other.pipe >= 0 && pipes_[other.pipe].end_refs[0] > 0 && o->name_size != 0 && other.name_size == o->name_size && same_name(other.name, other.name_size, StringView(o->name, o->name_size))) ++out->instances;
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

struct ObjectManager::ImageEntry {
  uint64_t pid = 0;
  uint32_t size = 0;
  char path[520] = {};
};
constexpr uint32_t kMaxImages = 128;

void ObjectManager::process_image_note(uint64_t pid, StringView path) {
  Locked hold(sync_);
  if (!allocator_ || path.size() >= sizeof(ImageEntry::path)) return;
  if (!images_) {
    void* memory = allocator_->allocate(sizeof(ImageEntry) * kMaxImages);
    if (!memory) return;
    images_ = static_cast<ImageEntry*>(memory);
    for (uint32_t i = 0; i < kMaxImages; ++i) new (&images_[i]) ImageEntry();
  }
  ImageEntry* slot = nullptr;
  for (uint32_t i = 0; i < kMaxImages && !slot; ++i) if (images_[i].pid == pid) slot = &images_[i];
  if (!slot) { slot = &images_[image_next_]; image_next_ = (image_next_ + 1) % kMaxImages; }      // (the oldest is replaced)
  slot->pid = pid;
  for (uint64_t k = 0; k < path.size(); ++k) slot->path[k] = path[k];
  slot->size = static_cast<uint32_t>(path.size());
}

uint32_t ObjectManager::process_image_get(uint64_t pid, char* out, uint32_t capacity) {
  Locked hold(sync_);
  if (!images_) return 0;
  for (uint32_t i = 0; i < kMaxImages; ++i) {
    const ImageEntry& e = images_[i];
    if (e.pid != pid || e.size == 0 || e.size > capacity) continue;
    for (uint32_t k = 0; k < e.size; ++k) out[k] = e.path[k];
    return e.size;
  }
  return 0;
}

struct ObjectManager::LinkEntry {
  bool used = false;
  char path[260] = {};
  char text[520] = {};
  uint32_t path_size = 0, text_size = 0;
};

void ObjectManager::link_note(StringView link_path, StringView target_text) {
  Locked hold(sync_);
  if (!allocator_ || link_path.size() >= 260 || target_text.size() >= 520) return;
  if (!links_) {
    void* memory = allocator_->allocate(sizeof(LinkEntry) * kMaxLinkTexts);
    if (!memory) return;
    links_ = static_cast<LinkEntry*>(memory);
    for (uint32_t i = 0; i < kMaxLinkTexts; ++i) new (&links_[i]) LinkEntry();
  }
  LinkEntry* slot = nullptr;
  for (uint32_t i = 0; i < kMaxLinkTexts && !slot; ++i) if (links_[i].used && same_name(links_[i].path, links_[i].path_size, link_path)) slot = &links_[i];
  for (uint32_t i = 0; i < kMaxLinkTexts && !slot; ++i) if (!links_[i].used) slot = &links_[i];
  if (!slot) slot = &links_[0];      // (all taken: the first one makes room)
  slot->used = true;
  for (uint32_t k = 0; k < link_path.size(); ++k) slot->path[k] = link_path[k];
  slot->path_size = static_cast<uint32_t>(link_path.size());
  for (uint32_t k = 0; k < target_text.size(); ++k) slot->text[k] = target_text[k];
  slot->text_size = static_cast<uint32_t>(target_text.size());
}

uint32_t ObjectManager::link_text(StringView link_path, char* out, uint32_t capacity) {
  Locked hold(sync_);
  if (!links_) return 0;
  for (uint32_t i = 0; i < kMaxLinkTexts; ++i) {
    const LinkEntry& e = links_[i];
    if (!e.used || !same_name(e.path, e.path_size, link_path)) continue;
    if (e.text_size > capacity) return 0;
    for (uint32_t k = 0; k < e.text_size; ++k) out[k] = e.text[k];
    return e.text_size;
  }
  return 0;
}

void ObjectManager::link_forget(StringView link_path) {
  Locked hold(sync_);
  if (!links_) return;
  for (uint32_t i = 0; i < kMaxLinkTexts; ++i) if (links_[i].used && same_name(links_[i].path, links_[i].path_size, link_path)) links_[i].used = false;
}

struct ObjectManager::TimesEntry {
  bool used = false;
  FileTimes times;
  char path[260] = {};
  uint32_t path_size = 0;
  uint32_t age = 0;
};

void ObjectManager::file_times_set(StringView path, uint32_t which, const FileTimes& times) {
  Locked hold(sync_);
  if (!allocator_ || path.size() >= 260 || which == 0) return;
  if (!times_) {
    void* memory = allocator_->allocate(sizeof(TimesEntry) * kMaxTimes);
    if (!memory) return;
    times_ = static_cast<TimesEntry*>(memory);
    for (uint32_t i = 0; i < kMaxTimes; ++i) new (&times_[i]) TimesEntry();
  }
  TimesEntry* slot = nullptr;
  uint32_t newest = 0;
  for (uint32_t i = 0; i < kMaxTimes; ++i) {
    TimesEntry& e = times_[i];
    if (e.used && e.age > newest) newest = e.age;
    if (e.used && same_name(e.path, e.path_size, path)) { slot = &e; break; }
  }
  if (!slot) {      // a free entry, or the one that was used longest ago
    uint32_t oldest = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < kMaxTimes; ++i) {
      TimesEntry& e = times_[i];
      if (!e.used) { slot = &e; break; }
      if (e.age < oldest) { oldest = e.age; slot = &e; }
    }
    slot->used = true;
    slot->times = FileTimes();
    for (uint32_t k = 0; k < path.size(); ++k) slot->path[k] = path[k];
    slot->path_size = static_cast<uint32_t>(path.size());
  }
  slot->age = newest + 1;
  if (which & 1) slot->times.created = times.created;
  if (which & 2) slot->times.accessed = times.accessed;
  if (which & 4) slot->times.written = times.written;
}

bool ObjectManager::file_times_get(StringView path, FileTimes* times) {
  Locked hold(sync_);
  if (!times_) return false;
  for (uint32_t i = 0; i < kMaxTimes; ++i) {
    const TimesEntry& e = times_[i];
    if (!e.used || !same_name(e.path, e.path_size, path)) continue;
    *times = e.times;
    return true;
  }
  return false;
}

void ObjectManager::file_times_move(StringView from, StringView to) {
  Locked hold(sync_);
  if (!times_ || to.size() >= 260) return;
  for (uint32_t i = 0; i < kMaxTimes; ++i) if (times_[i].used && same_name(times_[i].path, times_[i].path_size, to)) times_[i].used = false;
  for (uint32_t i = 0; i < kMaxTimes; ++i) {
    TimesEntry& e = times_[i];
    if (!e.used || !same_name(e.path, e.path_size, from)) continue;
    for (uint32_t k = 0; k < to.size(); ++k) e.path[k] = to[k];
    e.path_size = static_cast<uint32_t>(to.size());
  }
}

void ObjectManager::file_times_forget(StringView path) {
  Locked hold(sync_);
  if (!times_) return;
  for (uint32_t i = 0; i < kMaxTimes; ++i) if (times_[i].used && same_name(times_[i].path, times_[i].path_size, path)) times_[i].used = false;
}

struct ObjectManager::SecurityEntry {
  bool used = false;
  char path[260] = {};
  uint32_t path_size = 0;
  uint32_t size = 0;
  uint8_t* bytes = nullptr;      // `size` bytes from the allocator
};

bool ObjectManager::security_room() {
  Locked hold(sync_);
  if (!security_) return allocator_ != nullptr;
  for (uint32_t i = 0; i < kMaxSecurity; ++i) if (!security_[i].used) return true;
  return false;
}

// Keeps the blob under the key (a path, or the name of an object: "\x01..."). The room is bounded: kMaxSecurity entries, kMaxSecurityBytes in all.
bool ObjectManager::security_set(StringView path, ConstByteView descriptor) {
  Locked hold(sync_);
  if (!allocator_ || path.size() >= 260 || descriptor.size() > kMaxSecurityBlob) return false;
  if (!security_) {
    void* memory = allocator_->allocate(sizeof(SecurityEntry) * kMaxSecurity);
    if (!memory) return false;
    security_ = static_cast<SecurityEntry*>(memory);
    for (uint32_t i = 0; i < kMaxSecurity; ++i) new (&security_[i]) SecurityEntry();
  }
  SecurityEntry* slot = nullptr;
  uint64_t in_use = 0;
  for (uint32_t i = 0; i < kMaxSecurity; ++i) {
    SecurityEntry& e = security_[i];
    if (e.used && same_name(e.path, e.path_size, path)) { slot = &e; continue; }
    if (e.used) in_use += e.size;
    if (!e.used && !slot) slot = &e;
  }
  if (!slot || in_use + descriptor.size() > kMaxSecurityBytes) return false;
  uint8_t* bytes = nullptr;
  if (descriptor.size() != 0) {
    bytes = static_cast<uint8_t*>(allocator_->allocate(descriptor.size()));
    if (!bytes) return false;
    ByteView(bytes, descriptor.size()).copy_from(descriptor);
  }
  if (slot->bytes) allocator_->deallocate(slot->bytes);
  slot->bytes = bytes;
  slot->used = true;
  for (uint32_t k = 0; k < path.size(); ++k) slot->path[k] = path[k];
  slot->path_size = static_cast<uint32_t>(path.size());
  slot->size = static_cast<uint32_t>(descriptor.size());
  return true;
}

uint32_t ObjectManager::security_get(StringView path, ByteView out) {
  Locked hold(sync_);
  if (!security_) return 0;
  for (uint32_t i = 0; i < kMaxSecurity; ++i) {
    const SecurityEntry& e = security_[i];
    if (!e.used || !same_name(e.path, e.path_size, path) || e.size > out.size()) continue;
    if (e.size != 0) out.first(e.size).copy_from(ConstByteView(e.bytes, e.size));
    return e.size;
  }
  return 0;
}

uint32_t ObjectManager::security_size(StringView path) {
  Locked hold(sync_);
  if (!security_) return 0;
  for (uint32_t i = 0; i < kMaxSecurity; ++i) if (security_[i].used && same_name(security_[i].path, security_[i].path_size, path)) return security_[i].size;
  return 0;
}

void ObjectManager::security_erase_locked(SecurityEntry& e) {
  if (e.bytes && allocator_) allocator_->deallocate(e.bytes);
  e = SecurityEntry();
}

void ObjectManager::security_move(StringView from, StringView to) {
  Locked hold(sync_);
  if (!security_ || to.size() >= 260) return;
  for (uint32_t i = 0; i < kMaxSecurity; ++i) {      // what was at the new name is replaced
    SecurityEntry& e = security_[i];
    if (e.used && (same_name(e.path, e.path_size, to) || (e.path_size > to.size() && e.path[to.size()] == '/' && same_name(e.path, static_cast<uint32_t>(to.size()), to)))) security_erase_locked(e);
  }
  for (uint32_t i = 0; i < kMaxSecurity; ++i) {
    SecurityEntry& e = security_[i];
    if (!e.used) continue;
    const bool exact = same_name(e.path, e.path_size, from);
    const bool below = e.path_size > from.size() && e.path[from.size()] == '/' && same_name(e.path, static_cast<uint32_t>(from.size()), from);
    if (!exact && !below) continue;
    char moved[260];
    const uint64_t tail = e.path_size - from.size();
    if (to.size() + tail >= 260) { security_erase_locked(e); continue; }      // (it no longer fits a name: its protection is gone with it)
    for (uint32_t k = 0; k < to.size(); ++k) moved[k] = to[k];
    for (uint64_t k = 0; k < tail; ++k) moved[to.size() + k] = e.path[from.size() + k];
    for (uint64_t k = 0; k < to.size() + tail; ++k) e.path[k] = moved[k];
    e.path_size = static_cast<uint32_t>(to.size() + tail);
  }
}

void ObjectManager::security_forget(StringView path) {
  Locked hold(sync_);
  if (!security_) return;
  for (uint32_t i = 0; i < kMaxSecurity; ++i) if (security_[i].used && same_name(security_[i].path, security_[i].path_size, path)) security_erase_locked(security_[i]);
}

void ObjectManager::share_retarget(const void* owner, StringView new_path) {
  Locked hold(sync_);
  if (!shares_ || new_path.size() >= 260) return;
  for (uint32_t i = 0; i < kMaxShares; ++i) {
    ShareEntry& e = shares_[i];
    if (!e.used || e.owner != owner) continue;
    for (uint32_t k = 0; k < new_path.size(); ++k) e.path[k] = new_path[k];
    e.path_size = static_cast<uint32_t>(new_path.size());
  }
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

// A reserve object (NtAllocateReserveObject) holds the memory of one queued APC: taken for a user APC reserve (0: it is free, kSuccess; 0xC00000F0 busy; 0xC0000024 wrong type)
// and given back when the APC has run.
uint32_t ObjectManager::reserve_take(uint32_t id) {
  Locked hold(sync_);
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::Reserve || o->maximum != 0) return 0xC0000024u;
  if (o->signaled) return 0xC00000F0u;      // STATUS_INVALID_PARAMETER_2
  o->signaled = true;
  return kSuccess;
}
void ObjectManager::reserve_give_back(uint32_t id) {
  Locked hold(sync_);
  Object* o = at(id);
  if (o && o->kind == ObjectKind::Reserve) o->signaled = false;
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

bool ObjectManager::lookup_entry(StringView flat, bool case_insensitive, objpath::Entry* out) {
  Locked hold(sync_);
  const Object* o = find_locked(flat, case_insensitive);
  if (!o) return false;
  *out = objpath::Entry();
  for (uint32_t i = 0; i < o->name_size && i < objpath::kMaxPath; ++i) out->flat[i] = o->name[i];
  out->flat_size = o->name_size < objpath::kMaxPath ? o->name_size : objpath::kMaxPath;
  switch (o->kind) {
    case ObjectKind::Directory: out->kind = objpath::EntryKind::Directory; break;
    case ObjectKind::SymbolicLink: out->kind = objpath::EntryKind::Link; break;
    default: out->kind = objpath::EntryKind::Object; out->object_kind = static_cast<uint8_t>(o->kind); break;
  }
  for (uint32_t i = 0; i < o->text_size && i < objpath::kMaxPath; ++i) out->text[i] = o->text[i];
  out->text_size = o->text_size < objpath::kMaxPath ? o->text_size : objpath::kMaxPath;
  return true;
}

uint32_t ObjectManager::object_text(uint32_t id, char* out, uint32_t capacity) {
  Locked hold(sync_);
  const Object* o = at(id);
  if (!o || o->text_size == 0 || o->text_size > capacity) return 0;
  for (uint32_t i = 0; i < o->text_size; ++i) out[i] = o->text[i];
  return o->text_size;
}

bool ObjectManager::directory_child(StringView directory, uint32_t index, char* name, uint32_t capacity, uint32_t* name_size, ObjectKind* kind) {
  Locked hold(sync_);
  uint32_t seen = 0;
  for (const Object& o : objects_) {
    if (o.kind == ObjectKind::None || o.kind == ObjectKind::Pipe || o.name_size == 0) continue;
    StringView parent, leaf;
    objpath::split_flat(StringView(o.name, o.name_size), &parent, &leaf);
    if (parent != directory || leaf.empty()) continue;
    if (seen++ != index) continue;
    if (leaf.size() > capacity) return false;
    for (uint32_t i = 0; i < leaf.size(); ++i) name[i] = leaf[i];
    *name_size = static_cast<uint32_t>(leaf.size());
    *kind = o.kind;
    return true;
  }
  return false;
}

void ObjectManager::add_ref(uint32_t id) {
  Locked hold(sync_);
  if (Object* o = at(id)) ++o->references;
}

namespace windowsabi { void debug_object_freed_locked(uint32_t object); }

void ObjectManager::free_object_locked(Object& o) {
  forget_object_security_locked(static_cast<uint32_t>(&o - objects_) + 1);
  if (o.kind == ObjectKind::DebugObject) windowsabi::debug_object_freed_locked(static_cast<uint32_t>(&o - objects_) + 1);
  free_pipe_locked(o);
  free_section_locked(o);
  free_job_locked(o);
  free_watch_locked(o);
  free_mailslot_locked(o);
  free_port_locked(o);
  o = Object();
}

// An object nobody has a handle to goes at once - unless a thread is waiting for it, or a thread owns it (the mutex of a thread that ends is abandoned and its waiters told).
void ObjectManager::finish_dying_locked(Object& o) {
  if (o.dying && o.waiters <= 0 && !o.abandoned && !(o.kind == ObjectKind::Mutex && o.owner != 0)) free_object_locked(o);
}

void ObjectManager::release(uint32_t id) {
  Locked hold(sync_);
  release_locked(id);
}

void ObjectManager::release_locked(uint32_t id) {
  Object* o = at(id);
  if (!o || o->references == 0) return;
  if (--o->references == 0) {
    if (o->kind == ObjectKind::Section && o->section >= 0 && sections_ && sections_[o->section].mappings > 0) {      // views keep the memory; its name goes
      o->name_size = 0;
      return;
    }
    if (o->waiters > 0 || (o->kind == ObjectKind::Mutex && o->owner != 0)) {
      o->dying = true;
      o->name_size = 0;
      return;
    }
    const bool was_port = o->kind == ObjectKind::Port;
    free_object_locked(*o);
    if (was_port) sync_.notify_all();      // (the other end of a port sees it go)
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
    finish_dying_locked(*o);
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
  for (Object& o : objects_) {      // the registry watches the process left behind
    if (o.kind != ObjectKind::ChangeNotification || o.watch < 0 || !watches_) continue;
    Watch& w = watches_[o.watch];
    if (w.event_object == 0 || w.owner != process) continue;
    release_locked(w.event_object);
    w.event_object = 0;
    release_locked(static_cast<uint32_t>(&o - objects_) + 1);
  }
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

bool ObjectManager::object_state_locked(uint32_t id, uint64_t waiter, ObjectState* out) {
  Object* o = at(id);
  if (!o) return false;
  out->kind = o->kind;
  out->manual_reset = o->manual_reset;
  out->signaled = o->signaled || o->pulses > 0;
  out->abandoned = o->abandoned;
  out->owned_by_waiter = o->kind == ObjectKind::Mutex && o->owner == waiter && o->recursion > 0;
  out->count = o->count;
  out->maximum = o->maximum;
  out->recursion = o->recursion;
  return true;
}

uint32_t ObjectManager::clear_event(uint32_t id) {
  Locked hold(sync_);
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::Event) return kInvalidHandle;
  o->signaled = false;
  return kSuccess;
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
  if (Object* o = at(id)) {
    o->waiters += delta;
    if (delta < 0) finish_dying_locked(*o);
  }
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
  if (j.port_object != 0) { release_locked(j.port_object); j.port_object = 0; j.port_set = false; }
  if (j.info.limit_flags & kJobLimitKillOnClose) job_flag_all_locked(j, 1);
  for (JobMember& m : j.members) {      // the members stay members of nothing; those flagged keep their flag (it is found by their process id)
    if (m.pid == 0) continue;
    if (m.kill && !m.gone) { /* leave the entry so that the flagged process still finds it, until it ends */ continue; }
    m.pid = 0;
    m.gone = 0;
  }
  bool keep = false;
  for (const JobMember& m : j.members) if (m.pid != 0) keep = true;
  if (!keep) j.used = false;
  else j.object = 0;      // (the slot is freed when the last flagged member ends)
  o.job = -1;
}

void ObjectManager::job_event_locked(Job& j, uint32_t message, uint64_t pid) {
  if (!j.port_set) return;
  if (j.port_object != 0) {      // JOB_OBJECT_MSG_*: a packet at the port (the key, the process id as the OVERLAPPED, the message as the byte count)
    io::Packet packet;
    packet.key = j.port_key; packet.context = pid; packet.information = message;
    port_post_locked(j.port_object, packet);
    return;
  }
  if (j.event_count == Job::kMaxEvents) { j.event_head = (j.event_head + 1) % Job::kMaxEvents; --j.event_count; }      // (the oldest is lost)
  j.events[(j.event_head + j.event_count++) % Job::kMaxEvents] = JobEvent{message, pid};
}

bool ObjectManager::job_add_locked(Job& j, uint64_t pid) {
  for (int pass = 0; pass < 2; ++pass) {      // a free place, else one of a process that has ended
    for (JobMember& m : j.members) {
      if (pass == 0 ? m.pid != 0 : !m.gone) continue;
      m.kill = 0; m.code = 0; m.gone = 0;
      m.joined = ++job_joins_;
      __atomic_store_n(&m.pid, pid, __ATOMIC_RELEASE);
      ++j.info.active_processes;
      ++j.info.total_processes;
      job_event_locked(j, 6, pid);      // JOB_OBJECT_MSG_NEW_PROCESS
      return true;
    }
  }
  return false;
}

uint32_t ObjectManager::job_assign(uint32_t job, uint64_t pid) {
  Locked hold(sync_);
  Job* j = job_of(job);
  if (!j) return kInvalidHandle;
  auto alive_in = [pid](const Job& in) {
    for (const JobMember& m : in.members) if (m.pid == pid && !m.gone) return true;
    return false;
  };
  if (alive_in(*j)) return kAccessDenied;      // (already in it)
  // A process that is in a job may join another one only if that job never had a process: it then nests inside the process's innermost job, as on Windows 8 and later.
  uint32_t innermost = 0;
  uint64_t latest = 0;
  for (uint32_t k = 0; k < kJobMaxJobs; ++k) {
    if (!jobs_[k].used || jobs_[k].object == 0 || &jobs_[k] == j) continue;
    for (const JobMember& m : jobs_[k].members) {
      if (m.pid != pid || m.gone || (innermost != 0 && m.joined <= latest)) continue;
      innermost = jobs_[k].object;
      latest = m.joined;
    }
  }
  if (innermost != 0) {
    if (j->info.total_processes > 0 && j->parent != innermost) return kAccessDenied;
    if (j->parent == 0) j->parent = innermost;
  }
  if ((j->info.limit_flags & kJobLimitActiveProcess) && j->info.active_processes >= j->info.active_limit) return kQuotaExceeded;
  // The jobs it nests in, outermost first (so that it joins them before this one, which stays its innermost).
  static constexpr uint32_t kMaxDepth = 16;
  Job* chain[kMaxDepth];
  uint32_t depth = 0;
  for (Job* up = job_of(j->parent); up && up != j && depth < kMaxDepth; up = job_of(up->parent)) chain[depth++] = up;
  for (uint32_t d = depth; d-- > 0;) if (!alive_in(*chain[d]) && !job_add_locked(*chain[d], pid)) return kQuotaExceeded;
  return job_add_locked(*j, pid) ? kSuccess : kQuotaExceeded;
}

uint32_t ObjectManager::job_of_process(uint64_t pid) {
  Locked hold(sync_);
  if (!jobs_) return 0;
  uint32_t innermost = 0;
  uint64_t latest = 0;
  for (uint32_t k = 0; k < kJobMaxJobs; ++k) {
    if (!jobs_[k].used || jobs_[k].object == 0) continue;
    for (const JobMember& m : jobs_[k].members) {
      if (m.pid != pid || (innermost != 0 && m.joined <= latest)) continue;
      innermost = jobs_[k].object;
      latest = m.joined;
    }
  }
  return innermost;
}

bool ObjectManager::job_contains(uint32_t job, uint64_t pid) {
  Locked hold(sync_);
  if (!jobs_) return false;
  Job* j = job_of(job);
  if (!j) return false;
  for (const JobMember& m : j->members) if (m.pid == pid) return true;
  return false;
}

uint32_t ObjectManager::job_inherit(uint64_t parent, uint64_t child, bool breakaway) {
  Locked hold(sync_);
  if (!jobs_) return kSuccess;
  // Every job of the parent (they are nested) takes the child, in the order the parent joined them; a breakaway needs the innermost one to allow it.
  bool any = false;
  const Job* innermost = nullptr;
  uint64_t latest = 0;
  for (uint32_t k = 0; k < kJobMaxJobs; ++k) {
    Job& j = jobs_[k];
    if (!j.used || j.object == 0) continue;
    for (const JobMember& pm : j.members) {
      if (pm.pid != parent || pm.gone) continue;
      if (!any || pm.joined > latest) { innermost = &j; latest = pm.joined; }
      any = true;
      if (!breakaway && !(j.info.limit_flags & kJobLimitSilentBreakawayOk) && (j.info.limit_flags & kJobLimitActiveProcess) && j.info.active_processes >= j.info.active_limit) return kQuotaExceeded;
    }
  }
  if (breakaway && innermost && !(innermost->info.limit_flags & kJobLimitBreakawayOk)) return kAccessDenied;
  if (!any) return kSuccess;
  // A breakaway leaves the jobs from the innermost outwards while each allows it: the child stays in the first that does not, and in those outside it.
  uint64_t cutoff = ~uint64_t(0);      // (the child keeps the jobs the parent joined before this)
  while (breakaway) {
    const Job* next = nullptr;
    uint64_t next_joined = 0;
    for (uint32_t k = 0; k < kJobMaxJobs; ++k) {
      const Job& j = jobs_[k];
      if (!j.used || j.object == 0) continue;
      for (const JobMember& pm : j.members) {
        if (pm.pid != parent || pm.gone || pm.joined >= cutoff || (next && pm.joined <= next_joined)) continue;
        next = &j;
        next_joined = pm.joined;
      }
    }
    if (!next || !(next->info.limit_flags & kJobLimitBreakawayOk)) break;
    cutoff = next_joined;
  }
  for (uint32_t k = 0; k < kJobMaxJobs; ++k) {
    Job& j = jobs_[k];
    if (!j.used || j.object == 0 || (j.info.limit_flags & kJobLimitSilentBreakawayOk)) continue;
    const JobMember* parent_member = nullptr;
    for (const JobMember& pm : j.members) if (pm.pid == parent && !pm.gone && pm.joined < cutoff) parent_member = &pm;
    if (!parent_member) continue;
    JobMember* place = nullptr;
    for (JobMember& m : j.members) if (!place && (m.pid == 0 || m.gone)) place = &m;
    if (!place) return kQuotaExceeded;
    place->kill = 0; place->code = 0; place->gone = 0;
    if (parent_member->kill) { place->code = parent_member->code; place->kill = 1; __atomic_add_fetch(&pending_kills_, 1u, __ATOMIC_ACQ_REL); }      // (a child of a process being ended is ended too)
    place->joined = parent_member->joined;      // (the same depth as the parent's: the order of its jobs stays)
    __atomic_store_n(&place->pid, child, __ATOMIC_RELEASE);
    ++j.info.active_processes;
    ++j.info.total_processes;
    job_event_locked(j, 6, child);      // JOB_OBJECT_MSG_NEW_PROCESS
  }
  return kSuccess;
}

void ObjectManager::job_forget_locked(uint64_t pid) {
  if (!jobs_) return;
  for (uint32_t k = 0; k < kJobMaxJobs; ++k) {
    Job& j = jobs_[k];
    if (!j.used) continue;
    for (JobMember& m : j.members) {
      if (m.pid != pid || m.gone) continue;
      if (m.kill) __atomic_sub_fetch(&pending_kills_, 1u, __ATOMIC_ACQ_REL);
      __atomic_store_n(&m.kill, 0u, __ATOMIC_RELEASE);
      m.gone = 1;      // (it stays in the job, as far as IsProcessInJob goes, until its place is wanted)
      if (j.info.active_processes > 0) --j.info.active_processes;
      job_event_locked(j, 7, pid);      // JOB_OBJECT_MSG_EXIT_PROCESS
      if (j.info.active_processes == 0) job_event_locked(j, 4, 0);      // JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO
      if (j.object == 0) {      // the job object is gone; this was one of the members still to end
        bool any = false;
        for (const JobMember& other : j.members) if (other.pid != 0 && !other.gone) any = true;
        if (!any) j.used = false;
      }
      break;      // (the next job: a process may be in several)
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
  for (const JobMember& m : j->members) if (m.pid != 0 && !m.gone) { if (pids && n < capacity) pids[n] = m.pid; ++n; }
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

uint32_t ObjectManager::job_set_port(uint32_t job, uint64_t key, uint32_t port_object) {
  Locked hold(sync_);
  Job* j = job_of(job);
  if (!j) return kInvalidHandle;
  if (port_object == 0) {
    if (j->port_object != 0) release_locked(j->port_object);
    j->port_object = 0;
    j->port_set = false;
    j->event_head = j->event_count = 0;
    return kSuccess;
  }
  if (j->port_set) return kInvalid;
  Object* port = at(port_object);
  if (!port || port->kind != ObjectKind::IoCompletion) return kInvalidHandle;
  ++port->references;
  j->port_object = port_object;
  j->port_set = true;
  j->port_key = key;
  j->event_head = j->event_count = 0;
  for (const JobMember& m : j->members) if (m.pid != 0 && !m.gone) job_event_locked(*j, 6, m.pid);      // (the processes that are in the job already)
  return kSuccess;
}

bool ObjectManager::job_kill_for(uint64_t pid, uint32_t* code) {
  if (__atomic_load_n(&pending_kills_, __ATOMIC_ACQUIRE) == 0) return false;
  Job* jobs = __atomic_load_n(&jobs_, __ATOMIC_ACQUIRE);
  if (!jobs) return false;
  for (uint32_t k = 0; k < kJobMaxJobs; ++k) {
    for (const JobMember& m : jobs[k].members) {
      if (__atomic_load_n(&m.pid, __ATOMIC_ACQUIRE) != pid || !__atomic_load_n(&m.kill, __ATOMIC_ACQUIRE)) continue;      // (a process in several jobs ends when any of them is ended)
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

namespace {

// Where in the path `entry_path` (a full virtual path) the part below `directory` starts, if the entry is directly in `directory` or (with `subtree`) anywhere below it.
bool watch_covers(StringView watched, bool subtree, StringView entry_path, uint64_t* name_start) {
  if (watched.size() <= 1) {      // the root ("/"): every entry is under it
    if (entry_path.size() < 2 || entry_path[0] != '/') return false;
    if (!subtree) for (uint64_t k = 1; k < entry_path.size(); ++k) if (entry_path[k] == '/') return false;
    *name_start = 1;
    return true;
  }
  if (entry_path.size() <= watched.size() + 1 || entry_path[watched.size()] != '/' || entry_path.first(watched.size()) != watched) return false;
  if (!subtree) for (uint64_t k = watched.size() + 1; k < entry_path.size(); ++k) if (entry_path[k] == '/') return false;
  *name_start = watched.size() + 1;
  return true;
}

}  // namespace

ObjectManager::Created ObjectManager::create_key_watch(StringView path, bool watch_subtree, uint32_t filter, uint32_t event_object, uint64_t owner) {
  Created result = create_change_notification(path, watch_subtree, filter);
  if (result.id == 0) return result;
  Locked hold(sync_);
  Object* o = at(result.id);
  if (!o || o->watch < 0) return result;
  Watch& w = watches_[o->watch];
  w.want_data = false;
  if (event_object != 0) {
    Object* event = at(event_object);
    if (!event || event->kind != ObjectKind::Event) { release_locked(result.id); result.id = 0; result.status = kInvalidHandle; return result; }
    ++event->references;
    w.event_object = event_object;
    w.owner = owner;
  }
  return result;
}

bool ObjectManager::key_watch_fired_locked(uint32_t id) {
  Object* o = at(id);
  return o && o->kind == ObjectKind::ChangeNotification && o->signaled;
}

void ObjectManager::notify_entry_changed(StringView changed_path, uint32_t action, uint32_t filter_mask) {
  Locked hold(sync_);
  if (!watches_) return;
  bool any = false;
  uint32_t finished[16];
  uint32_t finished_count = 0;
  for (Object& o : objects_) {
    if (o.kind != ObjectKind::ChangeNotification || o.watch < 0) continue;
    Watch& w = watches_[o.watch];
    const StringView watched(w.path, w.path_size);
    uint64_t start = 0;
    if (!watch_covers(watched, w.subtree, changed_path, &start)) {
      if (action == 2 /* FILE_ACTION_REMOVED */ && (filter_mask & kChangeDirName) && changed_path == watched) { o.signaled = true; any = true; }      // (the watched directory itself is gone)
      continue;
    }
    if (filter_mask != 0 && (w.filter & filter_mask) == 0) continue;
    o.signaled = true;
    any = true;
    if (w.event_object != 0 && finished_count < 16) {      // a registry watch of the kernel: it signals its event, and it is over
      if (Object* event = at(w.event_object)) { event->signaled = true; release_locked(w.event_object); }
      w.event_object = 0;
      finished[finished_count++] = static_cast<uint32_t>(&o - objects_) + 1;
      continue;
    }
    if (!w.want_data) { w.overflow = true; if (w.waiters != 0) w.owes_ack = true; continue; }
    const StringView name = changed_path.without_prefix(start);
    bool duplicate = false;      // one record for a run of changes of the same entry
    if (action == 3 && w.record_count != 0) {
      const WatchRecord& last = w.records[(w.record_head + w.record_count - 1) % kMaxWatchRecords];
      if (last.action == 3 && last.name_size == name.size()) {
        duplicate = true;
        for (uint64_t k = 0; k < name.size(); ++k) if (last.name[k] != (name[k] == '/' ? '\\' : name[k])) { duplicate = false; break; }
      }
    }
    if (duplicate) continue;
    if (w.record_count >= kMaxWatchRecords || name.size() >= kWatchNameBytes) { w.overflow = true; if (w.waiters != 0) w.owes_ack = true; continue; }
    WatchRecord& r = w.records[(w.record_head + w.record_count) % kMaxWatchRecords];
    r.action = action;
    uint32_t n = 0;
    for (uint64_t k = 0; k < name.size(); ++k) r.name[n++] = name[k] == '/' ? '\\' : name[k];
    r.name_size = n;
    ++w.record_count;
    if (w.waiters != 0) w.owes_ack = true;
  }
  for (uint32_t i = 0; i < finished_count; ++i) release_locked(finished[i]);
  if (any) sync_.notify_all();
}

void ObjectManager::notify_entry_renamed(StringView old_path, StringView new_path, uint32_t filter_mask) {
  Locked hold(sync_);
  if (!watches_) return;
  bool any = false;
  for (Object& o : objects_) {
    if (o.kind != ObjectKind::ChangeNotification || o.watch < 0) continue;
    Watch& w = watches_[o.watch];
    if (filter_mask != 0 && (w.filter & filter_mask) == 0) continue;
    const StringView watched(w.path, w.path_size);
    uint64_t old_start = 0, new_start = 0;
    const bool had = watch_covers(watched, w.subtree, old_path, &old_start), has = watch_covers(watched, w.subtree, new_path, &new_start);
    if (!had && !has) continue;
    o.signaled = true;
    any = true;
    if (!w.want_data) { w.overflow = true; if (w.waiters != 0) w.owes_ack = true; continue; }
    const uint32_t first = had && has ? 4 /* RENAMED_OLD_NAME */ : had ? 2 /* REMOVED */ : 1 /* ADDED */;
    const StringView names[2] = {first == 1 ? new_path.without_prefix(new_start) : old_path.without_prefix(old_start), new_path.without_prefix(new_start)};
    const uint32_t count = had && has ? 2 : 1;
    if (w.record_count + count > kMaxWatchRecords || names[0].size() >= kWatchNameBytes || names[1].size() >= kWatchNameBytes) { w.overflow = true; if (w.waiters != 0) w.owes_ack = true; continue; }
    for (uint32_t i = 0; i < count; ++i) {
      WatchRecord& r = w.records[(w.record_head + w.record_count) % kMaxWatchRecords];
      r.action = i == 0 ? first : 5 /* RENAMED_NEW_NAME */;
      uint32_t n = 0;
      for (uint64_t k = 0; k < names[i].size(); ++k) r.name[n++] = names[i][k] == '/' ? '\\' : names[i][k];
      r.name_size = n;
      ++w.record_count;
    }
    if (w.waiters != 0) w.owes_ack = true;
  }
  if (any) sync_.notify_all();
}

void ObjectManager::watch_set_want_data(uint32_t id, bool want_data) {
  Locked hold(sync_);
  Object* o = at(id);
  if (o && o->kind == ObjectKind::ChangeNotification && o->watch >= 0 && watches_) watches_[o->watch].want_data = want_data;
}

bool ObjectManager::take_watch_state_locked(uint32_t id, bool* overflow, bool* want_data, bool clear) {
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::ChangeNotification || o->watch < 0 || !watches_) return false;
  Watch& w = watches_[o->watch];
  *overflow = w.overflow;
  *want_data = w.want_data;
  if (clear) w.overflow = false;
  return true;
}

void ObjectManager::watch_wait_begin(uint32_t id) {
  Locked hold(sync_);
  Object* o = at(id);
  if (o && o->kind == ObjectKind::ChangeNotification && o->watch >= 0 && watches_) ++watches_[o->watch].waiters;
}

void ObjectManager::watch_ack(uint32_t id) {
  Locked hold(sync_);
  Object* o = at(id);
  if (!o || o->kind != ObjectKind::ChangeNotification || o->watch < 0 || !watches_) return;
  Watch& w = watches_[o->watch];
  if (w.waiters != 0) --w.waiters;
  if (w.waiters == 0) w.owes_ack = false;
}

bool ObjectManager::watches_delivered_locked() {
  if (!watches_) return true;
  for (uint32_t i = 0; i < kMaxWatches; ++i) if (watches_[i].used && watches_[i].owes_ack) return false;
  return true;
}

void ObjectManager::watches_forgive_locked() {
  if (!watches_) return;
  for (uint32_t i = 0; i < kMaxWatches; ++i) watches_[i].owes_ack = false;
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

uint32_t ObjectManager::mailslot_message_count_locked(uint32_t id) {
  Mailslot* m = mailslot_of(id);
  return m ? m->message_count : 0;
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

// The key under which the blobs of an object are kept: "\x01<tag>:<id>".
uint32_t ObjectManager::object_key(char tag, uint32_t id, char (&out)[16]) {
  uint32_t n = 0;
  out[n++] = '\x01';
  out[n++] = tag;
  out[n++] = ':';
  char digits[10];
  uint32_t d = 0;
  do { digits[d++] = char('0' + id % 10); id /= 10; } while (id);
  while (d) out[n++] = digits[--d];
  out[n] = 0;
  return n;
}

void ObjectManager::forget_object_security_locked(uint32_t id) {
  if (!security_) return;
  for (const char tag : {'t', 's'}) {
    char key[16];
    const uint32_t n = object_key(tag, id, key);
    for (uint32_t i = 0; i < kMaxSecurity; ++i) if (security_[i].used && same_name(security_[i].path, security_[i].path_size, StringView(key, n))) security_erase_locked(security_[i]);
  }
}

}  // namespace process

// ---- I/O requests and completion ports

namespace process {

io::IoRequestTable* ObjectManager::request_table() {
  if (!requests_ && allocator_) {
    void* memory = allocator_->allocate(sizeof(io::IoRequestTable));
    if (memory) requests_ = new (memory) io::IoRequestTable(*allocator_);
  }
  return requests_;
}

void ObjectManager::free_port_locked(Object& o) {
  if (o.kind != ObjectKind::IoCompletion || o.port < 0 || !ports_) return;
  PortState& port = ports_[o.port];
  if (port.queue) { port.queue->~CompletionQueue(); if (allocator_) allocator_->deallocate(port.queue); }
  port = PortState();
  o.port = -1;
}

io::CompletionQueue* ObjectManager::port_queue_locked(uint32_t port_object) {
  Object* o = at(port_object);
  return o && o->kind == ObjectKind::IoCompletion && o->port >= 0 && ports_ ? ports_[o->port].queue : nullptr;
}

bool ObjectManager::is_port(uint32_t id) {
  Locked hold(sync_);
  return port_queue_locked(id) != nullptr;
}

bool ObjectManager::port_post_locked(uint32_t port_object, const io::Packet& packet) {
  io::CompletionQueue* queue = port_queue_locked(port_object);
  if (!queue || !queue->post(packet)) return false;
  sync_.notify_all();
  return true;
}

bool ObjectManager::port_post(uint32_t port_object, const io::Packet& packet) {
  Locked hold(sync_);
  return port_post_locked(port_object, packet);
}

bool ObjectManager::port_remove_locked(uint32_t port_object, io::Packet* out) {
  io::CompletionQueue* queue = port_queue_locked(port_object);
  return queue && queue->remove(out);
}

uint64_t ObjectManager::port_depth_locked(uint32_t port_object) {
  io::CompletionQueue* queue = port_queue_locked(port_object);
  return queue ? queue->depth() : 0;
}

io::IoRequest* ObjectManager::io_add_locked(const io::IoRequest& request) {
  io::IoRequestTable* table = request_table();
  if (!table) return nullptr;
  io::IoRequest* made = table->add(request);
  if (!made) return nullptr;
  if (made->event_object != 0) { if (Object* e = at(made->event_object)) ++e->references; else made->event_object = 0; }
  if (made->port_object != 0) { if (Object* e = at(made->port_object)) ++e->references; else made->port_object = 0; }
  return made;
}

io::IoRequest* ObjectManager::io_find_locked(uint32_t id) { return requests_ ? requests_->find(id) : nullptr; }

bool ObjectManager::io_complete_locked(uint32_t id, uint32_t status, uint64_t information, ByteView data) {
  if (!requests_ || !requests_->complete(id, status, information, data)) return false;
  sync_.notify_all();
  return true;
}

uint32_t ObjectManager::io_ready() const { return requests_ ? requests_->ready() : 0; }

uint32_t ObjectManager::io_take_completed(uint64_t owner, io::IoRequest* out, uint32_t capacity) {
  Locked hold(sync_);
  return requests_ ? requests_->take_completed(owner, out, capacity) : 0;
}

void ObjectManager::io_dispose(io::IoRequest& request) {
  Locked hold(sync_);
  if (request.event_object != 0) { release_locked(request.event_object); request.event_object = 0; }
  if (request.port_object != 0) { release_locked(request.port_object); request.port_object = 0; }
  if (allocator_) release_view(*allocator_, request.data);
}

uint32_t ObjectManager::io_cancel_locked(uint32_t source, uint32_t end, uint64_t owner, uint64_t thread, uint64_t status_block, uint32_t status, bool closing) {
  if (!requests_) return 0;
  uint32_t tags[io::IoRequestTable::kMaxRequests > 256 ? 256 : io::IoRequestTable::kMaxRequests];
  uint32_t count = 0;
  requests_->for_each([&](io::IoRequest& r) {
    if (r.completed || r.source != source || r.source_end != end || r.source_tag == 0 || count >= sizeof(tags) / sizeof(tags[0])) return;
    if ((owner != 0 && r.owner != owner) || (thread != 0 && r.thread != thread) || (status_block != 0 && r.status_block != status_block)) return;
    r.on_close = closing;
    tags[count++] = r.source_tag;
  });
  Pipe* p = pipe_of(source);
  uint32_t cancelled = 0;
  for (uint32_t i = 0; p && i < count; ++i) {
    PipeOp* op = pipe_find_op_locked(*p, tags[i]);
    if (!op || op->state != 1 || op->end != end || op->request == 0) continue;
    pipe_cancel_op_locked(*p, *op, status);
    ++cancelled;
  }
  return cancelled;
}

uint32_t ObjectManager::io_cancel_thread(uint64_t thread) {
  Locked hold(sync_);
  if (!requests_ || !pipes_) return 0;
  uint32_t cancelled = 0;
  for (uint32_t i = 0; i < kMaxPipes; ++i) {
    Pipe& p = pipes_[i];
    if (!p.used) continue;
    for (PipeOp& op : p.ops) {
      if (op.state != 1 || op.request == 0) continue;
      io::IoRequest* r = requests_->find(op.request);
      if (!r || r->thread != thread) continue;
      pipe_cancel_op_locked(p, op, kStatusCancelled);
      ++cancelled;
    }
  }
  return cancelled;
}

void ObjectManager::io_owner_gone(uint64_t pid) {
  Locked hold(sync_);
  if (!requests_) return;
  // The calls still pending are ended on the objects they wait on, with no answer for a process that is gone; every request is then dropped, and a port that is tied to the
  // call and has a context for it hears the outcome (cancelled for a call that was not over), as NT reports a call that ends with the process.
  if (pipes_) {
    for (uint32_t i = 0; i < kMaxPipes; ++i) {
      Pipe& p = pipes_[i];
      if (!p.used) continue;
      for (PipeOp& op : p.ops) {
        if (op.state != 1 || op.request == 0) continue;
        io::IoRequest* r = requests_->find(op.request);
        if (!r || r->owner != pid) continue;
        pipe_cancel_op_locked(p, op, kStatusCancelled);
      }
    }
  }
  io::IoRequest gone[16];
  for (;;) {
    const uint32_t n = requests_->remove_owner(pid, gone, 16);
    if (n == 0) break;
    for (uint32_t k = 0; k < n; ++k) {
      io::IoRequest& r = gone[k];
      if (r.port_object != 0 && r.port_context != 0) {
        io::Packet packet;
        packet.key = r.port_key; packet.context = r.port_context; packet.information = r.completed ? r.information : 0; packet.status = r.completed ? r.status : kStatusCancelled;
        port_post_locked(r.port_object, packet);
      }
      if (r.event_object != 0) release_locked(r.event_object);
      if (r.port_object != 0) release_locked(r.port_object);
    }
  }
}

bool ObjectManager::io_signal_source(uint32_t source, uint32_t end) {
  Locked hold(sync_);
  Pipe* p = pipe_of(source);
  if (!p || end > 1 || (p->port_flags[end] & 2)) return false;
  p->file_signaled[end] = true;
  sync_.notify_all();
  return true;
}

}  // namespace process
