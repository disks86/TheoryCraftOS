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
#include "Process/Win32k/Win32kCompositionInternal.hpp"

// NtDComposition*: DirectComposition channels, connections, synchronization objects, shared resources, frames and the DWM channel (docs/win32k/Composition.md).
namespace process::win32k::composition {

namespace {

constexpr uint64_t kDefaultChannelBytes = 0x10000, kMaxChannelBytes = 4ull << 20;
constexpr uint32_t kMaxChannelsPerProcess = 256, kMaxResources = 4096, kMaxProperties = 16384, kMaxEdges = 16384, kMaxFrames = 128, kMaxBatchFrames = 256, kMaxConnections = 256;

// Commands in a channel's batch buffer: {u32 opcode, u32 length (the whole command, a multiple of 4)} then the payload.
enum Opcode : uint32_t { kOpNop = 0, kOpCreateResource = 1, kOpReleaseResource = 2, kOpReleaseAllResources = 3, kOpSetProperty = 4, kOpAddChild = 5, kOpRemoveChild = 6 };
constexpr uint32_t kMaxCommandBytes = 4096;

int64_t resource_lower_bound(const Channel& ch, uint32_t id) {
  uint64_t lo = 0, hi = ch.resources.count();
  while (lo < hi) {
    const uint64_t mid = lo + (hi - lo) / 2;
    if (ch.resources[mid].id < id) lo = mid + 1; else hi = mid;
  }
  return int64_t(lo);
}
Resource* find_resource(Channel& ch, uint32_t id) {
  const int64_t at = resource_lower_bound(ch, id);
  return uint64_t(at) < ch.resources.count() && ch.resources[uint64_t(at)].id == id ? &ch.resources[uint64_t(at)] : nullptr;
}

uint32_t rd32(ConstByteView v, uint64_t at) {
  uint32_t r = 0;
  for (uint32_t i = 0; i < 4; ++i) r |= uint32_t{v[at + i]} << (8 * i);
  return r;
}

// Applies one command. kStatusSuccess, or the status that stops the batch.
uint32_t apply_command(Channel& ch, uint32_t opcode, ConstByteView payload) {
  switch (opcode) {
    case kOpNop: return kStatusSuccess;
    case kOpCreateResource: {
      if (payload.size() != 8) return kStatusInvalidParameter;
      const uint32_t id = rd32(payload, 0), type = rd32(payload, 4);
      if (id == 0 || type == 0 || type > 0xFFFF || find_resource(ch, id) || ch.resources.count() >= kMaxResources) return kStatusInvalidParameter;
      Resource r;
      r.id = id; r.type = type;
      return ch.resources.insert(uint64_t(resource_lower_bound(ch, id)), r) ? kStatusSuccess : kStatusNoMemory;
    }
    case kOpReleaseResource: {
      if (payload.size() != 4) return kStatusInvalidParameter;
      Resource* r = find_resource(ch, rd32(payload, 0));
      if (!r || r->released) return kStatusInvalidParameter;
      r->released = true;
      return kStatusSuccess;
    }
    case kOpReleaseAllResources: {
      if (payload.size() != 0) return kStatusInvalidParameter;
      for (Resource& r : ch.resources.view()) { r.released = true; r.incoming = 0; }
      ch.edges.clear();
      return kStatusSuccess;
    }
    case kOpSetProperty: {
      if (payload.size() < 8 || payload.size() > 8 + 64) return kStatusInvalidParameter;
      const uint32_t id = rd32(payload, 0), property = rd32(payload, 4);
      Resource* r = find_resource(ch, id);
      if (!r || r->released) return kStatusInvalidParameter;
      uint64_t value = 0;
      for (uint64_t i = 0; i < 8 && 8 + i < payload.size(); ++i) value |= uint64_t{payload[8 + i]} << (8 * i);
      for (Property& p : ch.properties.view()) if (p.resource == id && p.property == property) { p.value = value; return kStatusSuccess; }
      if (ch.properties.count() >= kMaxProperties) return kStatusInvalidParameter;
      Property p;
      p.resource = id; p.property = property; p.value = value;
      return ch.properties.push_back(p) ? kStatusSuccess : kStatusNoMemory;
    }
    case kOpAddChild: case kOpRemoveChild: {
      if (payload.size() != 8) return kStatusInvalidParameter;
      const uint32_t parent = rd32(payload, 0), child = rd32(payload, 4);
      Resource* p = find_resource(ch, parent);
      Resource* c = find_resource(ch, child);
      if (!p || !c || parent == child || p->released || c->released) return kStatusInvalidParameter;
      for (uint64_t i = 0; i < ch.edges.count(); ++i) {
        if (ch.edges[i].parent != parent || ch.edges[i].child != child) continue;
        if (opcode == kOpAddChild) return kStatusInvalidParameter;
        ch.edges.remove_at(i);
        --c->incoming;
        return kStatusSuccess;
      }
      if (opcode == kOpRemoveChild || ch.edges.count() >= kMaxEdges) return kStatusInvalidParameter;
      Edge e;
      e.parent = parent; e.child = child;
      if (!ch.edges.push_back(e)) return kStatusNoMemory;
      ++c->incoming;
      return kStatusSuccess;
    }
    default: return kStatusInvalidParameter;
  }
}

// The compositor deletes what the client released once nothing refers to it any more, and the ids go to the deleted list.
void delete_released(Channel& ch) {
  bool again = true;
  while (again) {
    again = false;
    for (uint64_t i = 0; i < ch.resources.count();) {
      const Resource r = ch.resources[i];
      if (!r.released || r.incoming != 0) { ++i; continue; }
      for (uint64_t e = ch.edges.count(); e-- > 0;) {
        if (ch.edges[e].parent != r.id) continue;
        if (Resource* c = find_resource(ch, ch.edges[e].child)) if (c->incoming > 0) --c->incoming;
        ch.edges.remove_at(e);
      }
      for (uint64_t p = ch.properties.count(); p-- > 0;) if (ch.properties[p].resource == r.id) ch.properties.remove_at(p);
      ch.deleted.push_back(r.id);
      ch.resources.remove_at(i);
      again = true;
    }
  }
}

// Commits the commands processed so far as one batch: returns its id.
uint32_t commit(Ctx& x, Channel& ch) {
  State& s = x.st;
  const uint32_t batch = ch.next_batch++;
  if (ch.next_batch == 0) ch.next_batch = 1;
  ch.committed_batch = batch;
  ch.pending_commands = 0;
  delete_released(ch);
  const uint64_t frame = s.next_frame++;
  if (ch.frames.count() >= kMaxBatchFrames) ch.frames.remove_at(0);
  BatchFrame bf;
  bf.batch = batch; bf.frame = frame;
  ch.frames.push_back(bf);
  if (ch.notifications < 0xFFFF) ++ch.notifications;
  x.signal(ch.completion_event);
  if (ch.connection_id != 0) {
    List<HandleTable::Info> all(s.allocator);
    s.handles.list(Kind::Connection, &all);
    for (const HandleTable::Info& i : all.view()) { Connection* c = static_cast<Connection*>(i.object); if (c->id == ch.connection_id) ++c->batch; }
  }
  wake_all();
  return batch;
}

uint32_t create_channel(Ctx& x, bool dwm, uint64_t out_handle, uint64_t out_size, uint64_t out_base) {
  Call& c = x.call;
  State& s = x.st;
  if (dwm) {
    if (s.dwm_pid != 0 && s.dwm_pid != c.pid()) return kStatusAccessDenied;
  } else if (s.handles.kind_count(c.pid(), Kind::Channel) >= kMaxChannelsPerProcess) {
    return kStatusQuotaExceeded;
  }
  uint64_t size = 0;
  if (out_size != 0 && !c.read_u64(out_size, &size)) return kStatusAccessViolation;
  if (size == 0) size = kDefaultChannelBytes;
  if (size > kMaxChannelBytes) return kStatusInvalidParameter;
  size = (size + 4095) & ~uint64_t{4095};
  if (out_handle == 0) return kStatusInvalidParameter;
  SharedPointer<Channel> ch = SharedPointer<Channel>::make(s.allocator, s.allocator);
  if (!ch) return kStatusNoMemory;
  ch->memory = MappedMemory::create(s.allocator, size);
  if (!ch->memory) return kStatusNoMemory;
  if (!map_into_caller(c, *ch->memory, &ch->mapped_base)) return kStatusNotSupported;
  ch->owner = c.pid();
  ch->dwm = dwm;
  ch->serial = s.next_serial++;
  uint32_t status = kStatusSuccess;
  SharedPointer<Object> base = ch;
  const uint64_t handle = new_handle(x, base, kRightAll, &status);
  if (!handle) { unmap_from_caller(c, ch->mapped_base, size); return status; }
  if (!write_handle(c, out_handle, handle) || (out_size != 0 && !c.write_u64(out_size, size)) || (out_base != 0 && !c.write_u64(out_base, ch->mapped_base))) {
    unmap_from_caller(c, ch->mapped_base, size);
    s.handles.close(handle, c.pid());
    return kStatusAccessViolation;
  }
  if (dwm) s.dwm_pid = c.pid();
  return kStatusSuccess;
}

// NtDCompositionCreateChannel(PHANDLE channel, PSIZE_T section_size (in: wanted, 0 = default; out: actual), PVOID* mapped_buffer)
uint64_t svc_create_channel(Call& c, Ctx& x) { return create_channel(x, false, c.arg(0), c.arg(1), c.arg(2)); }
// NtDCompositionCreateDwmChannel: the same for the compositor process (only one process at a time may hold DWM channels)
uint64_t svc_create_dwm_channel(Call& c, Ctx& x) { return create_channel(x, true, c.arg(0), c.arg(1), c.arg(2)); }

// NtDCompositionDestroyChannel(HANDLE channel)
uint64_t svc_destroy_channel(Call& c, Ctx& x) {
  uint32_t status;
  Channel* ch = lookup<Channel>(x, c.arg(0), Kind::Channel, 0, &status);
  if (!ch) return status;
  if (ch->owner == c.pid() && ch->mapped_base != 0 && ch->memory) { unmap_from_caller(c, ch->mapped_base, ch->memory->size()); ch->mapped_base = 0; }
  if (ch->dwm && ch->owner == c.pid() && x.st.dwm_pid == c.pid()) {
    // the DWM leaves when its last DWM channel goes
    uint32_t others = 0;
    List<HandleTable::Info> all(x.st.allocator);
    x.st.handles.list(Kind::Channel, &all);
    for (const HandleTable::Info& i : all.view()) if (i.object != ch && static_cast<Channel*>(i.object)->dwm && i.pid == c.pid()) ++others;
    if (others == 0) x.st.dwm_pid = 0;
  }
  x.st.handles.close(c.arg(0), c.pid());
  wake_all();
  return kStatusSuccess;
}

// Runs the batch in the channel's buffer: `size` bytes. *consumed gets the bytes used, *commands how many commands ran.
uint32_t process_batch(Channel& ch, uint32_t size, uint32_t* consumed, uint32_t* commands) {
  *consumed = 0;
  *commands = 0;
  if (!ch.memory || size > ch.memory->size() || size % 4 != 0) return kStatusInvalidParameter;
  ConstByteView buffer(ch.memory->data(), size);
  uint64_t at = 0;
  while (at < size) {
    if (size - at < 8) return kStatusInvalidParameter;
    const uint32_t opcode = rd32(buffer, at), length = rd32(buffer, at + 4);
    if (length < 8 || length % 4 != 0 || length > size - at || length - 8 > kMaxCommandBytes) return kStatusInvalidParameter;
    const uint32_t status = apply_command(ch, opcode, buffer.subview(at + 8, length - 8));
    if (status != kStatusSuccess) return status;
    at += length;
    *consumed = uint32_t(at);
    ++*commands;
    ++ch.pending_commands;
  }
  ch.processed_bytes += at;
  return kStatusSuccess;
}

// NtDCompositionProcessChannelBatchBuffer(HANDLE channel, DWORD size, DWORD* consumed, DWORD* commands)
uint64_t svc_process_batch(Call& c, Ctx& x) {
  uint32_t status;
  Channel* ch = lookup<Channel>(x, c.arg(0), Kind::Channel, kRightModify, &status);
  if (!ch) return status;
  uint32_t consumed = 0, commands = 0;
  const uint32_t result = process_batch(*ch, c.arg32(1), &consumed, &commands);
  if (c.arg(2) != 0 && !c.write_u32(c.arg(2), consumed)) return kStatusAccessViolation;
  if (c.arg(3) != 0 && !c.write_u32(c.arg(3), commands)) return kStatusAccessViolation;
  return result;
}

// NtDCompositionCommitChannel(HANDLE channel, DWORD flags, HANDLE event (optional), DWORD* batch_id (optional))
uint64_t svc_commit_channel(Call& c, Ctx& x) {
  const uint32_t flags = c.arg32(1);
  if (flags > 1) return kStatusInvalidParameter;
  uint32_t event = 0, status = kStatusSuccess;
  if (c.arg(2) != 0) { event = take_event(x, c.arg(2), &status); if (!event) return status; }      // (first: it gives the lock up)
  Channel* ch = lookup<Channel>(x, c.arg(0), Kind::Channel, kRightModify, &status);
  if (!ch) { if (event) x.st.defer_event(event, false, true); return status; }
  const uint32_t batch = commit(x, *ch);
  if (event) x.st.defer_event(event, true, true);
  if (c.arg(3) != 0 && !c.write_u32(c.arg(3), batch)) return kStatusAccessViolation;
  return kStatusSuccess;
}

// NtDCompositionSubmitDWMBatch(HANDLE dwm_channel, DWORD size, DWORD* consumed, DWORD* commands): processes the DWM channel's buffer and commits it
uint64_t svc_submit_dwm_batch(Call& c, Ctx& x) {
  uint32_t status;
  Channel* ch = lookup<Channel>(x, c.arg(0), Kind::Channel, kRightModify, &status);
  if (!ch) return status;
  if (!ch->dwm) return kStatusAccessDenied;
  uint32_t consumed = 0, commands = 0;
  const uint32_t result = process_batch(*ch, c.arg32(1), &consumed, &commands);
  if (result == kStatusSuccess) commit(x, *ch);
  if (c.arg(2) != 0 && !c.write_u32(c.arg(2), consumed)) return kStatusAccessViolation;
  if (c.arg(3) != 0 && !c.write_u32(c.arg(3), commands)) return kStatusAccessViolation;
  return result;
}

// NtDCompositionGetBatchId(HANDLE channel, DWORD* batch_id): the last batch the channel committed (0 if none)
uint64_t svc_get_batch_id(Call& c, Ctx& x) {
  uint32_t status;
  Channel* ch = lookup<Channel>(x, c.arg(0), Kind::Channel, kRightQuery, &status);
  if (!ch) return status;
  return c.write_u32(c.arg(1), ch->committed_batch) ? kStatusSuccess : kStatusAccessViolation;
}

// NtDCompositionGetFrameId(UINT64* frame_id): the id of the last frame the compositor made
uint64_t svc_get_frame_id(Call& c, Ctx& x) { return c.write_u64(c.arg(0), x.st.next_frame - 1) ? kStatusSuccess : kStatusAccessViolation; }

// NtDCompositionGetFrameIdFromBatchId(HANDLE channel, DWORD batch_id, UINT64* frame_id)
uint64_t svc_get_frame_id_from_batch(Call& c, Ctx& x) {
  uint32_t status;
  Channel* ch = lookup<Channel>(x, c.arg(0), Kind::Channel, kRightQuery, &status);
  if (!ch) return status;
  for (const BatchFrame& bf : ch->frames.view()) if (bf.batch == c.arg32(1)) return c.write_u64(c.arg(2), bf.frame) ? kStatusSuccess : kStatusAccessViolation;
  return kStatusNotFound;
}

// NtDCompositionGetDeletedResources(HANDLE channel, DWORD* ids, DWORD* count (in: room, out: written)): STATUS_BUFFER_OVERFLOW when more are waiting
uint64_t svc_get_deleted_resources(Call& c, Ctx& x) {
  uint32_t status, room = 0;
  Channel* ch = lookup<Channel>(x, c.arg(0), Kind::Channel, kRightQuery | kRightModify, &status);
  if (!ch) return status;
  if (!c.read_u32(c.arg(2), &room)) return kStatusAccessViolation;
  const uint32_t n = uint32_t(ch->deleted.count() < room ? ch->deleted.count() : room);
  if (n != 0 && c.arg(1) == 0) return kStatusAccessViolation;
  for (uint32_t i = 0; i < n; ++i) if (!c.write_u32(c.arg(1) + uint64_t{i} * 4, ch->deleted[i])) return kStatusAccessViolation;
  for (uint32_t i = 0; i < n; ++i) ch->deleted.remove_at(0);
  if (!c.write_u32(c.arg(2), n)) return kStatusAccessViolation;
  return ch->deleted.count() != 0 ? kStatusBufferOverflow : kStatusSuccess;
}

// NtDCompositionReleaseAllResources(HANDLE channel)
uint64_t svc_release_all(Call& c, Ctx& x) {
  uint32_t status;
  Channel* ch = lookup<Channel>(x, c.arg(0), Kind::Channel, kRightModify, &status);
  if (!ch) return status;
  apply_command(*ch, kOpReleaseAllResources, ConstByteView());
  return kStatusSuccess;
}

// NtDCompositionSetChannelCommitCompletionEvent(HANDLE channel, HANDLE event (0: none)): the event is signalled when a batch commits
uint64_t svc_set_commit_event(Call& c, Ctx& x) {
  uint32_t status = kStatusSuccess, event = 0;
  if (c.arg(1) != 0) { event = take_event(x, c.arg(1), &status); if (!event) return status; }
  Channel* ch = lookup<Channel>(x, c.arg(0), Kind::Channel, kRightModify, &status);
  if (!ch) { if (event) x.st.defer_event(event, false, true); return status; }
  if (ch->completion_event) x.st.defer_event(ch->completion_event, false, true);
  ch->completion_event = event;
  return kStatusSuccess;
}

// NtDCompositionSetChannelConnectionId(HANDLE channel, UINT64 connection_id (0: none)): a connection of the caller
uint64_t svc_set_connection_id(Call& c, Ctx& x) {
  uint32_t status;
  Channel* ch = lookup<Channel>(x, c.arg(0), Kind::Channel, kRightModify, &status);
  if (!ch) return status;
  const uint64_t id = c.arg(1);
  if (id != 0) {
    bool found = false;
    List<HandleTable::Info> all(x.st.allocator);
    x.st.handles.list(Kind::Connection, &all);
    for (const HandleTable::Info& i : all.view()) if (i.pid == c.pid() && static_cast<Connection*>(i.object)->id == id) found = true;
    if (!found) return kStatusInvalidParameter;
  }
  ch->connection_id = id;
  return kStatusSuccess;
}

// NtDCompositionWaitForChannel(HANDLE channel, DWORD timeout_ms, DWORD flags): waits for a committed batch nobody has reported yet
uint64_t svc_wait_for_channel(Call& c, Ctx& x) {
  uint32_t status;
  SharedPointer<Object> pin = x.st.handles.share(c.arg(0), c.pid(), Kind::Channel, kRightQuery, &status);
  if (!pin) return status;
  if (c.arg32(2) > 1) return kStatusInvalidParameter;
  Channel* ch = static_cast<Channel*>(pin.get());
  const bool got = wait_until(c, timeout_ns_of_ms(c.arg32(1)), [&] { if (ch->notifications == 0) return false; --ch->notifications; return true; });
  if (got) return kStatusSuccess;
  return c.process().exited() ? kStatusThreadIsTerminating : kStatusTimeout;
}

// NtDCompositionSynchronize(HANDLE channel, DWORD flags, DWORD timeout_ms): returns when the compositor has taken every committed batch; it takes them at the commit
uint64_t svc_synchronize(Call& c, Ctx& x) {
  uint32_t status;
  Channel* ch = lookup<Channel>(x, c.arg(0), Kind::Channel, kRightQuery, &status);
  if (!ch) return status;
  return c.arg32(1) > 1 ? kStatusInvalidParameter : kStatusSuccess;
}

// NtDCompositionCreateSynchronizationObject(PHANDLE sync, DWORD flags)
uint64_t svc_create_sync(Call& c, Ctx& x) {
  if (c.arg32(1) > 1) return kStatusInvalidParameter;
  SharedPointer<SyncObject> o = make_object<SyncObject>(x);
  if (!o) return kStatusNoMemory;
  o->flags = c.arg32(1);
  uint32_t status;
  SharedPointer<Object> base = o;
  const uint64_t h = new_handle(x, base, kRightAll, &status);
  if (!h) return status;
  if (!write_handle(c, c.arg(0), h)) { x.st.handles.close(h, c.pid()); return kStatusAccessViolation; }
  return kStatusSuccess;
}

// NtDCompositionCommitSynchronizationObject(HANDLE channel, HANDLE sync, DWORD flags): the object is signalled once for the commit
uint64_t svc_commit_sync(Call& c, Ctx& x) {
  uint32_t status;
  Channel* ch = lookup<Channel>(x, c.arg(0), Kind::Channel, kRightModify, &status);
  if (!ch) return status;
  SyncObject* so = lookup<SyncObject>(x, c.arg(1), Kind::SyncObject, kRightModify, &status);
  if (!so) return status;
  if (c.arg32(2) != 0) return kStatusInvalidParameter;
  ++so->committed;
  wake_all();
  return kStatusSuccess;
}

// NtDCompositionSyncWait(HANDLE sync, DWORD timeout_ms): waits until the object was committed and takes one commit
uint64_t svc_sync_wait(Call& c, Ctx& x) {
  uint32_t status;
  SharedPointer<Object> pin = x.st.handles.share(c.arg(0), c.pid(), Kind::SyncObject, kRightQuery, &status);
  if (!pin) return status;
  SyncObject* so = static_cast<SyncObject*>(pin.get());
  const bool got = wait_until(c, timeout_ns_of_ms(c.arg32(1)), [&] { if (so->consumed >= so->committed) return false; ++so->consumed; return true; });
  if (got) return kStatusSuccess;
  return c.process().exited() ? kStatusThreadIsTerminating : kStatusTimeout;
}

// NtDCompositionCreateConnection(PHANDLE connection, UINT64* connection_id (optional))
uint64_t svc_create_connection(Call& c, Ctx& x) {
  if (x.st.handles.kind_count(c.pid(), Kind::Connection) >= kMaxConnections) return kStatusQuotaExceeded;
  SharedPointer<Connection> o = make_object<Connection>(x);
  if (!o) return kStatusNoMemory;
  o->owner = c.pid();
  o->id = (c.pid() << 24) ^ x.st.next_connection++;
  uint32_t status;
  SharedPointer<Object> base = o;
  const uint64_t h = new_handle(x, base, kRightAll, &status);
  if (!h) return status;
  if (!write_handle(c, c.arg(0), h) || (c.arg(1) != 0 && !c.write_u64(c.arg(1), o->id))) { x.st.handles.close(h, c.pid()); return kStatusAccessViolation; }
  return kStatusSuccess;
}
// NtDCompositionDestroyConnection(HANDLE connection)
uint64_t svc_destroy_connection(Call& c, Ctx& x) {
  uint32_t status;
  if (!lookup<Connection>(x, c.arg(0), Kind::Connection, 0, &status)) return status;
  x.st.handles.close(c.arg(0), c.pid());
  return kStatusSuccess;
}
// NtDCompositionGetConnectionBatch(HANDLE connection, DWORD* batch): how many batches the channels attached to the connection have committed
uint64_t svc_get_connection_batch(Call& c, Ctx& x) {
  uint32_t status;
  Connection* o = lookup<Connection>(x, c.arg(0), Kind::Connection, kRightQuery, &status);
  if (!o) return status;
  return c.write_u32(c.arg(1), o->batch) ? kStatusSuccess : kStatusAccessViolation;
}
// NtDCompositionConnectPipe: connects a connection to the compositor's pipe; this system has no compositor process to remote to.
uint64_t svc_connect_pipe(Call& c, Ctx& x) {
  uint32_t status;
  if (!lookup<Connection>(x, c.arg(0), Kind::Connection, kRightModify, &status)) return status;
  return kStatusNotSupported;
}

// NtDCompositionCreateSharedResourceHandle(PHANDLE shared, DWORD flags)
uint64_t svc_create_shared_resource(Call& c, Ctx& x) {
  if (c.arg32(1) > 3) return kStatusInvalidParameter;
  SharedPointer<SharedResource> o = make_object<SharedResource>(x);
  if (!o) return kStatusNoMemory;
  o->flags = c.arg32(1);
  uint32_t status;
  SharedPointer<Object> base = o;
  const uint64_t h = new_handle(x, base, kRightAll, &status);
  if (!h) return status;
  if (!write_handle(c, c.arg(0), h)) { x.st.handles.close(h, c.pid()); return kStatusAccessViolation; }
  return kStatusSuccess;
}
// NtDCompositionReferenceSharedResourceOnDwmChannel(HANDLE shared, HANDLE dwm_channel, DWORD resource_id): the shared resource becomes resource `id` of the DWM's channel
uint64_t svc_reference_shared_on_dwm(Call& c, Ctx& x) {
  uint32_t status;
  SharedResource* so = lookup<SharedResource>(x, c.arg(0), Kind::SharedResource, kRightQuery, &status);
  if (!so) return status;
  Channel* ch = lookup<Channel>(x, c.arg(1), Kind::Channel, kRightModify, &status);
  if (!ch) return status;
  if (!ch->dwm || ch->owner != c.pid()) return kStatusAccessDenied;
  const uint32_t id = c.arg32(2);
  if (id == 0 || so->dwm_channel_serial != 0 || find_resource(*ch, id) || ch->resources.count() >= kMaxResources) return kStatusInvalidParameter;
  Resource r;
  r.id = id; r.type = 0xFFFF;
  if (!ch->resources.insert(uint64_t(resource_lower_bound(*ch, id)), r)) return kStatusNoMemory;
  so->dwm_channel_serial = ch->serial;
  so->resource_id = id;
  return kStatusSuccess;
}

// NtDCompositionCreateAndBindSharedSection(HANDLE channel, DWORD resource_id, SIZE_T size, PHANDLE section): a section (an ordinary handle) bound to a resource of the channel
uint64_t svc_create_and_bind_section(Call& c, Ctx& x) {
  const uint32_t id = c.arg32(1);
  const uint64_t size = c.arg(2);
  if (id == 0 || size == 0 || size > (16ull << 20) || c.arg(3) == 0) return kStatusInvalidParameter;
  uint32_t status;
  Channel* ch = lookup<Channel>(x, c.arg(0), Kind::Channel, kRightModify, &status);
  if (!ch) return status;
  if (find_resource(*ch, id) || ch->resources.count() >= kMaxResources) return kStatusInvalidParameter;
  SharedPointer<Object> pin = x.st.handles.share(c.arg(0), c.pid(), Kind::Channel, kRightModify, &status);
  uint32_t section = 0;
  {
    Ctx::Unlocked u(x);
    const ObjectManager::Created made = x.st.objects->create_section(StringView(), size, 4, 0x8000000u, nullptr, StringView());
    section = made.id;
  }
  if (!section) return kStatusNoMemory;
  ch = static_cast<Channel*>(pin.get());
  uint64_t handle = 0;
  if (find_resource(*ch, id) || !give_descriptor(c, DescriptorKind::Section, section, &handle)) { x.st.defer_event(section, false, true); return kStatusInvalidParameter; }
  Resource r;
  r.id = id; r.type = 0xFFFE;
  if (!ch->resources.insert(uint64_t(resource_lower_bound(*ch, id)), r)) { drop_descriptor(x, handle); return kStatusNoMemory; }
  if (!c.write_u64(c.arg(3), handle)) {
    for (uint64_t i = 0; i < ch->resources.count(); ++i) if (ch->resources[i].id == id) { ch->resources.remove_at(i); break; }
    drop_descriptor(x, handle);
    return kStatusAccessViolation;
  }
  return kStatusSuccess;
}

// NtDCompositionDuplicateHandleToProcess(HANDLE handle, HANDLE process, PHANDLE out (a handle valid in that process), DWORD access, DWORD options (2: same access))
uint64_t svc_duplicate_to_process(Call& c, Ctx& x) {
  const uint64_t access_arg = c.arg(3), options = c.arg(4);
  if (c.bad_stack()) return kStatusAccessViolation;
  uint32_t status = kStatusSuccess, access = 0;
  const bool same = (options & 2) != 0;
  if ((options & ~uint64_t{2}) != 0) return kStatusInvalidParameter;
  if (!same && !map_access(uint32_t(access_arg), &access)) return kStatusInvalidParameter;
  uint64_t target = 0;
  status = process_of_handle(x, c.arg(1), &target);      // (it gives the lock up)
  if (status != kStatusSuccess) return status;
  const uint64_t out = x.st.handles.duplicate(c.arg(0), c.pid(), target, access, same, &status);
  if (!out) return status;
  if (!write_handle(c, c.arg(2), out)) { x.st.handles.close(out, target); return kStatusAccessViolation; }
  return kStatusSuccess;
}

// NtDCompositionDuplicateSwapchainHandleToDwm(HANDLE surface, PHANDLE out): the compositor process gets a handle to a composition surface
uint64_t svc_duplicate_swapchain_to_dwm(Call& c, Ctx& x) {
  uint32_t status;
  if (!lookup<Surface>(x, c.arg(0), Kind::Surface, kRightQuery, &status)) return status;
  if (x.st.dwm_pid == 0) return kStatusNotSupported;
  const uint64_t out = x.st.handles.duplicate(c.arg(0), c.pid(), x.st.dwm_pid, kRightQuery | kRightModify, false, &status);
  if (!out) return status;
  if (!write_handle(c, c.arg(1), out)) { x.st.handles.close(out, x.st.dwm_pid); return kStatusAccessViolation; }
  return kStatusSuccess;
}

// NtDCompositionGetChannels(HANDLE* out, DWORD* count (in: room, out: how many are handed out)): the compositor gets handles to the channels of the other processes
uint64_t svc_get_channels(Call& c, Ctx& x) {
  State& s = x.st;
  if (s.dwm_pid == 0 || s.dwm_pid != c.pid()) return kStatusAccessDenied;
  uint32_t room = 0;
  if (!c.read_u32(c.arg(1), &room)) return kStatusAccessViolation;
  List<HandleTable::Info> all(s.allocator), mine(s.allocator);
  s.handles.list(Kind::Channel, &all);
  uint32_t written = 0;
  for (const HandleTable::Info& i : all.view()) if (i.pid == c.pid()) mine.push_back(i);
  for (const HandleTable::Info& i : all.view()) {
    if (i.pid == c.pid() || static_cast<Channel*>(i.object)->dwm) continue;
    bool have = false;
    for (const HandleTable::Info& m : mine.view()) if (m.object == i.object) have = true;
    if (have) continue;
    if (written >= room) return kStatusBufferOverflow;
    uint32_t status;
    const uint64_t h = s.handles.duplicate(i.handle, i.pid, c.pid(), kRightQuery | kRightModify, false, &status);
    if (!h) return status;
    HandleTable::Info added;
    added.handle = h; added.pid = c.pid(); added.object = i.object;
    mine.push_back(added);
    if (!c.write_u64(c.arg(0) + uint64_t{written} * 8, h)) { s.handles.close(h, c.pid()); return kStatusAccessViolation; }
    ++written;
  }
  return c.write_u32(c.arg(1), written) ? kStatusSuccess : kStatusAccessViolation;
}

// --- frames (the compositor's own bookkeeping; only the DWM channel's owner may use them) ----------------------------------------------------------------------------------

Channel* dwm_channel(Ctx& x, uint64_t handle, uint32_t* status) {
  Channel* ch = lookup<Channel>(x, handle, Kind::Channel, kRightModify, status);
  if (!ch) return nullptr;
  if (!ch->dwm || ch->owner != x.call.pid()) { *status = kStatusAccessDenied; return nullptr; }
  return ch;
}
Frame* find_frame(State& s, uint64_t id) {
  for (Frame& f : s.frames.view()) if (f.id == id) return &f;
  return nullptr;
}

// NtDCompositionBeginFrame(HANDLE dwm_channel, UINT64* frame_id): starts a frame
uint64_t svc_begin_frame(Call& c, Ctx& x) {
  uint32_t status;
  if (!dwm_channel(x, c.arg(0), &status)) return status;
  State& s = x.st;
  if (s.frames.count() >= kMaxFrames) {
    bool dropped = false;
    for (uint64_t i = 0; i < s.frames.count() && !dropped; ++i) if (s.frames[i].state >= 2) { s.frames.remove_at(i); dropped = true; }
    if (!dropped) return kStatusQuotaExceeded;
  }
  Frame f;
  f.id = s.next_frame++;
  f.owner = c.pid();
  if (!c.write_u64(c.arg(1), f.id)) { --s.next_frame; return kStatusAccessViolation; }
  return s.frames.push_back(f) ? kStatusSuccess : kStatusNoMemory;
}
uint64_t move_frame(Call& c, Ctx& x, uint8_t from_min, uint8_t from_max, uint8_t to) {
  uint32_t status;
  if (!dwm_channel(x, c.arg(0), &status)) return status;
  Frame* f = find_frame(x.st, c.arg(1));
  if (!f) return kStatusNotFound;
  if (f->state < from_min || f->state > from_max) return kStatusInvalidDeviceState;
  f->state = to;
  return kStatusSuccess;
}
// NtDCompositionConfirmFrame / RetireFrame / DiscardFrame(HANDLE dwm_channel, UINT64 frame_id)
uint64_t svc_confirm_frame(Call& c, Ctx& x) { return move_frame(c, x, 0, 0, 1); }
uint64_t svc_retire_frame(Call& c, Ctx& x) { return move_frame(c, x, 1, 1, 2); }
uint64_t svc_discard_frame(Call& c, Ctx& x) { return move_frame(c, x, 0, 1, 3); }

// NtDCompositionGetFrameLegacyTokens(HANDLE dwm_channel, UINT64 frame_id, DWORD* count): legacy (redirected GDI) tokens; there are none
uint64_t svc_get_frame_legacy_tokens(Call& c, Ctx& x) {
  uint32_t status;
  if (!dwm_channel(x, c.arg(0), &status)) return status;
  if (!find_frame(x.st, c.arg(1))) return kStatusNotFound;
  return c.write_u32(c.arg(2), 0) ? kStatusSuccess : kStatusAccessViolation;
}
// NtDCompositionGetFrameSurfaceUpdates(HANDLE dwm_channel, void* updates, DWORD* count (in: room, out: written)): none pending
uint64_t svc_get_frame_surface_updates(Call& c, Ctx& x) {
  uint32_t status, room = 0;
  if (!dwm_channel(x, c.arg(0), &status)) return status;
  if (!c.read_u32(c.arg(2), &room)) return kStatusAccessViolation;
  return c.write_u32(c.arg(2), 0) ? kStatusSuccess : kStatusAccessViolation;
}

// --- statistics ---------------------------------------------------------------------------------------------------------------------------------------------------------

struct Statistics { uint32_t size, channels, resources, connections; uint64_t frames, batches; uint32_t dwm_present, reserved; };
struct FrameStatistics { uint32_t size, begun, confirmed, retired; uint32_t discarded, reserved; uint64_t last_frame; };

// NtDCompositionGetStatistics(Statistics* buffer, DWORD size)
uint64_t svc_get_statistics(Call& c, Ctx& x) {
  if (c.arg32(1) < sizeof(Statistics)) return kStatusInfoLengthMismatch;
  Statistics st = {};
  st.size = sizeof(Statistics);
  List<HandleTable::Info> all(x.st.allocator);
  x.st.handles.list(Kind::Channel, &all);
  st.channels = uint32_t(all.count());
  uint64_t batches = 0;
  for (const HandleTable::Info& i : all.view()) {
    const Channel* ch = static_cast<Channel*>(i.object);
    st.resources += uint32_t(ch->resources.count());
    batches += ch->committed_batch;
  }
  st.connections = x.st.handles.kind_count(c.pid(), Kind::Connection);
  st.frames = x.st.next_frame - 1;
  st.batches = batches;
  st.dwm_present = x.st.dwm_pid != 0;
  return c.write_struct(c.arg(0), st) ? kStatusSuccess : kStatusAccessViolation;
}
// NtDCompositionGetFrameStatistics(FrameStatistics* buffer, DWORD size)
uint64_t svc_get_frame_statistics(Call& c, Ctx& x) {
  if (c.arg32(1) < sizeof(FrameStatistics)) return kStatusInfoLengthMismatch;
  FrameStatistics st = {};
  st.size = sizeof(FrameStatistics);
  for (const Frame& f : x.st.frames.view()) {
    if (f.state == 0) ++st.begun; else if (f.state == 1) ++st.confirmed; else if (f.state == 2) ++st.retired; else ++st.discarded;
  }
  st.last_frame = x.st.next_frame - 1;
  return c.write_struct(c.arg(0), st) ? kStatusSuccess : kStatusAccessViolation;
}

// --- clock, flags, counters -----------------------------------------------------------------------------------------------------------------------------------------------

constexpr uint64_t kTickNs = 16666667ull;      // the compositor clock: 60 Hz

// NtDCompositionBoostCompositorClock(BOOLEAN boost)
uint64_t svc_boost_clock(Call& c, Ctx& x) { x.st.compositor_boosted = (c.arg(0) & 0xFF) != 0; return kStatusSuccess; }

// NtDCompositionWaitForCompositorClock(DWORD timeout_ms): returns at the next tick of the compositor clock (60 Hz), or STATUS_TIMEOUT when that is later than the timeout
uint64_t svc_wait_for_clock(Call& c, Ctx& x) {
  const uint64_t timeout = timeout_ns_of_ms(c.arg32(0));
  const uint64_t start = now_ns(c.process());
  const uint64_t tick = (start / kTickNs + 1) * kTickNs;
  if (timeout != ~uint64_t{0} && tick - start > timeout) {
    wait_until(c, timeout, [] { return false; });
    return kStatusTimeout;
  }
  struct Ctxt { Process* p; uint64_t tick; } context{&c.process(), tick};
  const bool ok = wait_loop(c, ~uint64_t{0}, [](void* v) { Ctxt* t = static_cast<Ctxt*>(v); return now_ns(*t->p) >= t->tick; }, &context, [](void* v) { return static_cast<Ctxt*>(v)->tick; });
  return ok ? kStatusSuccess : (c.process().exited() ? kStatusThreadIsTerminating : kStatusTimeout);
}

// NtDCompositionEnableMMCSS(HANDLE channel, BOOLEAN enable) / NtDCompositionSuspendAnimations(HANDLE channel, BOOLEAN suspend)
uint64_t svc_enable_mmcss(Call& c, Ctx& x) {
  uint32_t status;
  Channel* ch = lookup<Channel>(x, c.arg(0), Kind::Channel, kRightModify, &status);
  if (!ch) return status;
  ch->mmcss = (c.arg(1) & 0xFF) != 0;
  return kStatusSuccess;
}
uint64_t svc_suspend_animations(Call& c, Ctx& x) {
  uint32_t status;
  Channel* ch = lookup<Channel>(x, c.arg(0), Kind::Channel, kRightModify, &status);
  if (!ch) return status;
  ch->animations_suspended = (c.arg(1) & 0xFF) != 0;
  return kStatusSuccess;
}

// NtDCompositionSetDebugCounter(DWORD index, UINT64 value): only the compositor's process
uint64_t svc_set_debug_counter(Call& c, Ctx& x) {
  if (x.st.dwm_pid == 0 || x.st.dwm_pid != c.pid()) return kStatusAccessDenied;
  if (c.arg32(0) >= 32) return kStatusInvalidParameter;
  x.st.debug_counters[c.arg32(0)] = c.arg(1);
  return kStatusSuccess;
}

// NtDCompositionSetMaterialProperty(DWORD material, DWORD property, UINT64 value) / NtDCompositionGetMaterialProperty(DWORD material, DWORD property, UINT64* value)
uint64_t svc_set_material(Call& c, Ctx& x) {
  const uint32_t material = c.arg32(0), property = c.arg32(1);
  if (material == 0) return kStatusInvalidParameter;
  for (MaterialValue& m : x.st.materials.view()) if (m.owner == c.pid() && m.material == material && m.property == property) { m.value = c.arg(2); return kStatusSuccess; }
  if (x.st.materials.count() >= 4096) return kStatusQuotaExceeded;
  MaterialValue m;
  m.owner = c.pid(); m.material = material; m.property = property; m.value = c.arg(2);
  return x.st.materials.push_back(m) ? kStatusSuccess : kStatusNoMemory;
}
uint64_t svc_get_material(Call& c, Ctx& x) {
  for (const MaterialValue& m : x.st.materials.view())
    if (m.owner == c.pid() && m.material == c.arg32(0) && m.property == c.arg32(1)) return c.write_u64(c.arg(2), m.value) ? kStatusSuccess : kStatusAccessViolation;
  return kStatusNotFound;
}

// --- telemetry: recorded, nothing consumes it here ------------------------------------------------------------------------------------------------------------------------------

// NtDCompositionTelemetrySetApplicationId(DWORD id)
uint64_t svc_telemetry_app_id(Call& c, Ctx& x) { x.st.telemetry_app_id = c.arg32(0); return kStatusSuccess; }
// NtDCompositionTelemetryAnimationScenarioBegin(UINT64 scenario) / Reference(UINT64) / Unreference(UINT64): scenarios are counted per id
uint64_t scenario_adjust(Call& c, Ctx& x, int delta) {
  const uint64_t id = c.arg(0);
  if (id == 0) return kStatusInvalidParameter;
  List<uint64_t>& s = x.st.scenarios;
  for (uint64_t i = 0; i + 1 < s.count(); i += 2) {
    if (s[i] != id) continue;
    if (delta < 0 && s[i + 1] == 0) return kStatusInvalidDeviceState;
    s[i + 1] = uint64_t(int64_t(s[i + 1]) + delta);
    return kStatusSuccess;
  }
  if (delta < 0) return kStatusInvalidDeviceState;
  if (s.count() >= 1024) return kStatusQuotaExceeded;
  return s.push_back(id) && s.push_back(1) ? kStatusSuccess : kStatusNoMemory;
}
uint64_t svc_scenario_begin(Call& c, Ctx& x) { return scenario_adjust(c, x, 1); }
uint64_t svc_scenario_reference(Call& c, Ctx& x) { return scenario_adjust(c, x, 1); }
uint64_t svc_scenario_unreference(Call& c, Ctx& x) { return scenario_adjust(c, x, -1); }
// NtDCompositionTelemetryTouchInteractionBegin / Update / End(UINT64 interaction)
uint64_t svc_touch_begin(Call& c, Ctx& x) {
  const uint64_t id = c.arg(0);
  if (id == 0) return kStatusInvalidParameter;
  for (uint64_t v : x.st.touch_active.view()) if (v == id) return kStatusInvalidDeviceState;
  if (x.st.touch_active.count() >= 256) return kStatusQuotaExceeded;
  return x.st.touch_active.push_back(id) ? kStatusSuccess : kStatusNoMemory;
}
uint64_t svc_touch_update(Call& c, Ctx& x) {
  for (uint64_t v : x.st.touch_active.view()) if (v == c.arg(0)) return kStatusSuccess;
  return kStatusNotFound;
}
uint64_t svc_touch_end(Call& c, Ctx& x) {
  for (uint64_t i = 0; i < x.st.touch_active.count(); ++i) if (x.st.touch_active[i] == c.arg(0)) { x.st.touch_active.remove_at(i); return kStatusSuccess; }
  return kStatusNotFound;
}

}  // namespace

void install_dcomp() {
  COMPOSITION_REGISTER(NtDCompositionCreateChannel, svc_create_channel);
  COMPOSITION_REGISTER(NtDCompositionCreateDwmChannel, svc_create_dwm_channel);
  COMPOSITION_REGISTER(NtDCompositionDestroyChannel, svc_destroy_channel);
  COMPOSITION_REGISTER(NtDCompositionProcessChannelBatchBuffer, svc_process_batch);
  COMPOSITION_REGISTER(NtDCompositionCommitChannel, svc_commit_channel);
  COMPOSITION_REGISTER(NtDCompositionSubmitDWMBatch, svc_submit_dwm_batch);
  COMPOSITION_REGISTER(NtDCompositionGetBatchId, svc_get_batch_id);
  COMPOSITION_REGISTER(NtDCompositionGetFrameId, svc_get_frame_id);
  COMPOSITION_REGISTER(NtDCompositionGetFrameIdFromBatchId, svc_get_frame_id_from_batch);
  COMPOSITION_REGISTER(NtDCompositionGetDeletedResources, svc_get_deleted_resources);
  COMPOSITION_REGISTER(NtDCompositionReleaseAllResources, svc_release_all);
  COMPOSITION_REGISTER(NtDCompositionSetChannelCommitCompletionEvent, svc_set_commit_event);
  COMPOSITION_REGISTER(NtDCompositionSetChannelConnectionId, svc_set_connection_id);
  COMPOSITION_REGISTER(NtDCompositionWaitForChannel, svc_wait_for_channel);
  COMPOSITION_REGISTER(NtDCompositionSynchronize, svc_synchronize);
  COMPOSITION_REGISTER(NtDCompositionCreateSynchronizationObject, svc_create_sync);
  COMPOSITION_REGISTER(NtDCompositionCommitSynchronizationObject, svc_commit_sync);
  COMPOSITION_REGISTER(NtDCompositionSyncWait, svc_sync_wait);
  COMPOSITION_REGISTER(NtDCompositionCreateConnection, svc_create_connection);
  COMPOSITION_REGISTER(NtDCompositionDestroyConnection, svc_destroy_connection);
  COMPOSITION_REGISTER(NtDCompositionGetConnectionBatch, svc_get_connection_batch);
  COMPOSITION_REGISTER(NtDCompositionConnectPipe, svc_connect_pipe);
  COMPOSITION_REGISTER(NtDCompositionCreateSharedResourceHandle, svc_create_shared_resource);
  COMPOSITION_REGISTER(NtDCompositionReferenceSharedResourceOnDwmChannel, svc_reference_shared_on_dwm);
  COMPOSITION_REGISTER(NtDCompositionCreateAndBindSharedSection, svc_create_and_bind_section);
  COMPOSITION_REGISTER(NtDCompositionDuplicateHandleToProcess, svc_duplicate_to_process);
  COMPOSITION_REGISTER(NtDCompositionDuplicateSwapchainHandleToDwm, svc_duplicate_swapchain_to_dwm);
  COMPOSITION_REGISTER(NtDCompositionGetChannels, svc_get_channels);
  COMPOSITION_REGISTER(NtDCompositionBeginFrame, svc_begin_frame);
  COMPOSITION_REGISTER(NtDCompositionConfirmFrame, svc_confirm_frame);
  COMPOSITION_REGISTER(NtDCompositionRetireFrame, svc_retire_frame);
  COMPOSITION_REGISTER(NtDCompositionDiscardFrame, svc_discard_frame);
  COMPOSITION_REGISTER(NtDCompositionGetFrameLegacyTokens, svc_get_frame_legacy_tokens);
  COMPOSITION_REGISTER(NtDCompositionGetFrameSurfaceUpdates, svc_get_frame_surface_updates);
  COMPOSITION_REGISTER(NtDCompositionGetStatistics, svc_get_statistics);
  COMPOSITION_REGISTER(NtDCompositionGetFrameStatistics, svc_get_frame_statistics);
  COMPOSITION_REGISTER(NtDCompositionBoostCompositorClock, svc_boost_clock);
  COMPOSITION_REGISTER(NtDCompositionWaitForCompositorClock, svc_wait_for_clock);
  COMPOSITION_REGISTER(NtDCompositionEnableMMCSS, svc_enable_mmcss);
  COMPOSITION_REGISTER(NtDCompositionSuspendAnimations, svc_suspend_animations);
  COMPOSITION_REGISTER(NtDCompositionSetDebugCounter, svc_set_debug_counter);
  COMPOSITION_REGISTER(NtDCompositionSetMaterialProperty, svc_set_material);
  COMPOSITION_REGISTER(NtDCompositionGetMaterialProperty, svc_get_material);
  COMPOSITION_REGISTER(NtDCompositionTelemetrySetApplicationId, svc_telemetry_app_id);
  COMPOSITION_REGISTER(NtDCompositionTelemetryAnimationScenarioBegin, svc_scenario_begin);
  COMPOSITION_REGISTER(NtDCompositionTelemetryAnimationScenarioReference, svc_scenario_reference);
  COMPOSITION_REGISTER(NtDCompositionTelemetryAnimationScenarioUnreference, svc_scenario_unreference);
  COMPOSITION_REGISTER(NtDCompositionTelemetryTouchInteractionBegin, svc_touch_begin);
  COMPOSITION_REGISTER(NtDCompositionTelemetryTouchInteractionUpdate, svc_touch_update);
  COMPOSITION_REGISTER(NtDCompositionTelemetryTouchInteractionEnd, svc_touch_end);
}

}  // namespace process::win32k::composition
