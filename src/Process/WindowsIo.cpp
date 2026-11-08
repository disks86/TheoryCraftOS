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
#include "Process/WindowsIo.hpp"
#include "Process/Objects.hpp"
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsPipes.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kTimeout = 0x102, kAlerted = 0x101, kUserApc = 0xC0, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kAccessViolation = 0xC0000005u,
                   kAccessDenied = 0xC0000022u, kTypeMismatch = 0xC0000024u, kNotFound = 0xC0000225u, kNotImplemented = 0xC0000002u, kInvalidInfoClass = 0xC0000003u,
                   kInfoLengthMismatch = 0xC0000004u, kCancelled = 0xC0000120u, kNoMemory = 0xC0000017u;
constexpr uint64_t kUnixToNt = 11644473600ull * 10000000ull;

bool slot_of(uint64_t handle, uint64_t* slot) {
  if (handle == 0 || handle % 4 != 0 || handle / 4 > Process::kMaxDescriptors) return false;
  *slot = handle / 4 - 1;
  return true;
}

uint64_t current_thread(Process& p) { return p.runtime() ? p.runtime()->current_thread_id() : p.id(); }

// A 16-byte IO_STATUS_BLOCK in the program's memory.
bool store_status_block(Process& p, uint64_t address, uint32_t status, uint64_t information) {
  if (address == 0) return true;
  uint8_t raw[16] = {};
  for (uint32_t i = 0; i < 4; ++i) raw[i] = uint8_t(status >> (8 * i));
  for (uint32_t i = 0; i < 8; ++i) raw[8 + i] = uint8_t(information >> (8 * i));
  return p.write_user(address, ConstByteView(raw, 16));
}

// An APC for the thread that made a call: the routine is called with (context, IO_STATUS_BLOCK, reserved) when the thread waits alertably.
void queue_io_apc(Process& p, uint64_t thread, uint64_t routine, uint64_t context, uint64_t status_block) {
  if (routine == 0) return;
  Process::ThreadRecord* t = p.find_thread(thread);
  if (!t || t->finished) return;
  if (!Process::queue_apc(*t, Process::ThreadRecord::Apc{routine, context, status_block, 0})) return;
  if (p.runtime()) p.runtime()->notify();
}

}  // namespace

uint32_t io_call_begin(Process& p, uint64_t slot, const SyscallRequest& r, IoCall* call) {
  (void)slot;
  *call = IoCall();
  uint64_t apc = 0, context = 0, status_block = 0;
  if (!argument(p, r, 2, &apc) || !argument(p, r, 3, &context) || !argument(p, r, 4, &status_block)) return kAccessViolation;
  call->event_handle = r.args[1];
  call->apc = apc;
  call->context = context;
  call->status_block = status_block;
  call->thread = current_thread(p);
  if (call->event_handle == 1) { call->event_handle = 0; call->wait = true; }      // (the library's: the call waits)
  {
    uint32_t port = 0, flags = 0;
    uint64_t key = 0;
    if (call->apc != 0 && io_handle_port(p, slot, &port, &key, &flags)) return kInvalidParameter;      // (a handle tied to a port reports there: it takes no APC)
  }
  if (call->event_handle != 0) {
    ObjectManager* objects = system_objects();
    if (!objects) return kNotImplemented;
    uint64_t event_slot = 0;
    if (!slot_of(call->event_handle, &event_slot) || p.files().kind(event_slot) != DescriptorKind::Event) return kInvalidHandle;
    if (p.files().denied_access(event_slot) & 2) return kAccessDenied;      // (EVENT_MODIFY_STATE)
    call->event_object = uint32_t(p.files().token(event_slot));
    int32_t previous = 0;
    const uint32_t status = objects->reset_event(call->event_object, &previous);
    if (status != kSuccess) return status;
  }
  if ((call->event_handle != 0 || call->apc != 0 || p.files().kind(slot) == DescriptorKind::Pipe) && call->status_block != 0) call->saved = p.read_user(call->status_block, ByteView(call->status_block_before, 16));
  return kSuccess;
}

uint32_t io_call_end(Process& p, uint64_t slot, const IoCall& call, uint32_t status, bool apc_allowed, bool completes_with_error) {
  if (status == 0x103) return status;
  if (((status >> 30) == 3 && !completes_with_error) || status == kUserApc) {      // an error: nothing completes
    if (call.saved) p.write_user(call.status_block, ConstByteView(call.status_block_before, 16));
    return status;
  }
  uint64_t information = 0;
  if (call.status_block != 0) p.read_user_u64(call.status_block + 8, &information);
  if (call.event_handle != 0 || call.apc != 0 || call.context != 0) {
    IoCall completed = call;
    if (!apc_allowed) completed.apc = 0;
    io_call_completed(p, slot, completed, status, information);
  }
  if (call.event_object == 0 && p.files().kind(slot) == DescriptorKind::Pipe) pipe_handle_signal(p, slot);      // (a call that went through without an event of its own signals the file object)
  return status;
}

bool io_handle_port(Process& p, uint64_t slot, uint32_t* port, uint64_t* key, uint32_t* flags) {
  switch (p.files().kind(slot)) {
    case DescriptorKind::Pipe: return pipe_handle_port(p, slot, port, key, flags);
    case DescriptorKind::File: case DescriptorKind::Directory: {
      OpenObject* o = p.files().object(slot);
      if (!o || o->io_port == 0) return false;
      *port = o->io_port; *key = o->io_key; *flags = o->io_flags;
      return true;
    }
    default: return false;
  }
}

void io_call_completed(Process& p, uint64_t slot, const IoCall& call, uint32_t status, uint64_t information) {
  ObjectManager* objects = system_objects();
  if (!objects) return;
  if (call.event_object != 0) objects->set_event(call.event_object, nullptr);
  uint32_t port = 0, flags = 0;
  uint64_t key = 0;
  const bool tied = io_handle_port(p, slot, &port, &key, &flags);
  if (tied) {
    if (call.context != 0 && int32_t(status) >= 0 && !(flags & 1)) {      // (a call that went through reports to the port, unless the handle skips that; a warning is not reported)
      io::Packet packet;
      packet.key = key; packet.context = call.context; packet.information = information; packet.status = status;
      objects->port_post(port, packet);
    }
  } else if (call.apc != 0) {
    queue_io_apc(p, call.thread, call.apc, call.context, call.status_block);
  }
}

io::IoRequest io_new_request(Process& p, uint64_t slot, const IoCall& call, uint64_t buffer, uint32_t buffer_length) {
  io::IoRequest r;
  r.owner = p.id();
  r.thread = call.thread;
  r.status_block = call.status_block;
  r.apc_routine = call.apc;
  r.apc_context = call.context;
  r.buffer = buffer;
  r.buffer_length = buffer_length;
  r.event_object = call.event_object;
  uint32_t port = 0, flags = 0;
  uint64_t key = 0;
  if (io_handle_port(p, slot, &port, &key, &flags)) {
    r.apc_routine = 0;      // (a handle tied to a port reports there, not with an APC)
    if (call.context != 0) { r.port_object = port; r.port_key = key; r.port_context = call.context; }
  }
  return r;
}

void io_reap(Process& p) {
  ObjectManager* objects = system_objects();
  if (!objects) return;
  io::IoRequest batch[16];
  bool applied = false;
  for (;;) {
    const uint32_t n = objects->io_take_completed(p.id(), batch, 16);
    if (n == 0) break;
    applied = true;
    for (uint32_t i = 0; i < n; ++i) {
      io::IoRequest& r = batch[i];
      if (!r.data.empty() && r.buffer != 0) {
        const uint64_t count = r.data.size() < r.buffer_length ? r.data.size() : r.buffer_length;
        if (count != 0) p.write_user(r.buffer, ConstByteView(r.data).first(count));
      }
      if (!r.on_close) store_status_block(p, r.status_block, r.status, r.information);
      if (!r.on_close) {
        if (r.event_object != 0) objects->set_event(r.event_object, nullptr);
        else objects->io_signal_source(r.source, r.source_end);
        if (r.apc_routine != 0) queue_io_apc(p, r.thread, r.apc_routine, r.apc_context, r.status_block);
      }
      if (r.port_object != 0 && r.port_context != 0 && !(r.on_close && r.event_object != 0)) {
        io::Packet packet;
        packet.key = r.port_key; packet.context = r.port_context; packet.information = r.information; packet.status = r.status;
        if (r.on_close) { packet.status_block = r.status_block; packet.owner = r.owner; }
        objects->port_post(r.port_object, packet);
      }
      objects->io_dispose(r);
    }
  }
  if (applied) objects->notify();
}

// ---- cancelling

uint32_t nt_cancel_io_file(Process& p, const SyscallRequest& r) {
  uint64_t slot = 0;
  if (r.args[1] == 0) return kAccessViolation;
  if (!slot_of(r.args[0], &slot) || p.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  uint32_t cancelled = 0;
  if (p.files().kind(slot) == DescriptorKind::Pipe) cancelled = pipe_handle_cancel(p, slot, current_thread(p), 0);
  (void)cancelled;
  io_reap(p);      // (the calls that ended have their answer when this returns)
  return store_status_block(p, r.args[1], kSuccess, 0) ? kSuccess : kAccessViolation;
}

uint32_t nt_cancel_io_file_ex(Process& p, const SyscallRequest& r) {
  uint64_t slot = 0;
  if (r.args[2] == 0) return kAccessViolation;
  if (!slot_of(r.args[0], &slot) || p.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  uint32_t cancelled = 0;
  if (p.files().kind(slot) == DescriptorKind::Pipe) cancelled = pipe_handle_cancel(p, slot, 0, r.args[1]);
  io_reap(p);
  const uint32_t status = cancelled ? kSuccess : kNotFound;
  return store_status_block(p, r.args[2], status, 0) ? status : kAccessViolation;
}

// ---- completion ports

namespace {

uint32_t port_of(Process& p, uint64_t handle, uint32_t* port) {
  uint64_t slot = 0;
  if (!slot_of(handle, &slot) || p.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  if (p.files().kind(slot) != DescriptorKind::IoCompletion) return kTypeMismatch;
  if (p.files().denied_access(slot) & 2) return kAccessDenied;      // (IO_COMPLETION_MODIFY_STATE)
  *port = uint32_t(p.files().token(slot));
  return kSuccess;
}

uint64_t timeout_ns_of(Process& p, uint64_t pointer, bool* fault) {
  *fault = false;
  if (pointer == 0) return ~uint64_t{0};
  uint64_t raw = 0;
  if (!p.read_user_u64(pointer, &raw)) { *fault = true; return 0; }
  if (int64_t(raw) < 0) {
    const uint64_t units = uint64_t(0) - raw;
    return units > ~uint64_t{0} / 100 ? ~uint64_t{0} : units * 100;
  }
  const uint64_t now = p.host().realtime_ns() / 100 + kUnixToNt;
  return raw > now ? (raw - now) * 100 : 0;
}

// Waits until the port has a packet (or up to `max` of them, taken in *packets / *count), the time is up, or - when alertable - an APC is queued for the thread.
uint32_t wait_port(Process& p, uint32_t port, uint64_t timeout_ns, bool alertable, io::Packet* packets, uint32_t max, uint32_t* count) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  ISystemSync& sync = objects->sync();
  IProcessRuntime* runtime = p.runtime();
  const uint64_t waiter = current_thread(p);
  const uint64_t started = p.host().monotonic_ns();
  *count = 0;
  sync.lock();
  for (;;) {
    if (objects->io_ready() != 0) { sync.unlock(); io_reap(p); sync.lock(); }
    Process::ThreadRecord* me = p.find_thread(waiter);
    if (alertable && runtime) {      // an APC queued for the thread goes before a packet - until the thread has taken a packet from this port: after that the packets go first
      ObjectManager::TimerApc fired[8];
      uint32_t fired_count = 0;
      objects->take_timer_apcs_locked(waiter, fired, 8, &fired_count);
      for (uint32_t i = 0; me && i < fired_count; ++i) Process::queue_apc(*me, Process::ThreadRecord::Apc{fired[i].routine, fired[i].context, fired[i].time & 0xFFFFFFFFu, fired[i].time >> 32});
      if (me && me->apc_count > 0 && me->completion_port != port) { sync.unlock(); return kUserApc; }
    }
    if (objects->port_depth_locked(port) != 0) {
      while (*count < max && objects->port_remove_locked(port, &packets[*count])) ++*count;
      if (me) me->completion_port = port;      // (the thread is associated with the port now)
      sync.unlock();
      return kSuccess;
    }
    if (alertable && runtime && me && me->apc_count > 0) { sync.unlock(); return kUserApc; }
    if (!objects->is_port_locked(port)) { sync.unlock(); return 0xC0000008u; }
    if (timeout_ns == 0) { sync.unlock(); return kTimeout; }
    uint64_t left = ~uint64_t{0};
    if (timeout_ns != ~uint64_t{0}) {
      const uint64_t used = p.host().monotonic_ns() - started;
      if (used >= timeout_ns) { sync.unlock(); return kTimeout; }
      left = timeout_ns - used;
    }
    if (p.exited() || (runtime && runtime->interrupted())) { sync.unlock(); return kAlerted; }
    if (runtime && runtime->park_wanted()) {
      sync.unlock();      // (only then: dropping the lock here would let a wake-up fall between the look above and the wait below)
      const int parked = runtime->park_point();
      sync.lock();
      if (parked == 2) { sync.unlock(); return kAlerted; }
      continue;      // (look again: anything may have changed while the lock was let go)
    }
    if (alertable) {
      const uint64_t now = sync.now_ns();
      const uint64_t due = objects->thread_timer_deadline_locked(waiter);
      if (due != ~uint64_t{0}) { const uint64_t until = due > now ? due - now : 0; if (until < left) left = until; }
    }
    if (runtime) runtime->unlock();
    else p.host().yield();
    sync.wait(left);
    sync.unlock();
    if (runtime) runtime->lock();
    sync.lock();
  }
}

}  // namespace

uint32_t nt_set_io_completion(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  uint32_t port = 0;
  if (const uint32_t status = port_of(p, r.args[0], &port)) return status;
  uint64_t information = 0;
  if (!argument(p, r, 4, &information)) return kAccessViolation;
  io::Packet packet;
  packet.key = r.args[1]; packet.context = r.args[2]; packet.status = uint32_t(r.args[3]); packet.information = information;
  return objects->port_post(port, packet) ? kSuccess : kNoMemory;
}

uint32_t nt_set_io_completion_ex(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  uint32_t port = 0;
  if (const uint32_t status = port_of(p, r.args[0], &port)) return status;
  uint64_t context = 0, status_value = 0, information = 0;
  if (!argument(p, r, 3, &context) || !argument(p, r, 4, &status_value) || !argument(p, r, 5, &information)) return kAccessViolation;
  io::Packet packet;
  packet.key = r.args[2]; packet.context = context; packet.status = uint32_t(status_value); packet.information = information;
  return objects->port_post(port, packet) ? kSuccess : kNoMemory;
}

uint32_t nt_remove_io_completion(Process& p, const SyscallRequest& r) {
  uint32_t port = 0;
  if (const uint32_t status = port_of(p, r.args[0], &port)) return status;
  uint64_t timeout_at = 0;
  if (!argument(p, r, 4, &timeout_at)) return kAccessViolation;
  bool fault = false;
  const uint64_t timeout = timeout_ns_of(p, timeout_at, &fault);
  if (fault) return kAccessViolation;
  io::Packet packet;
  uint32_t got = 0;
  const uint32_t status = wait_port(p, port, timeout, false, &packet, 1, &got);
  if (status != kSuccess) return status;
  if (packet.status_block != 0 && packet.owner == p.id()) store_status_block(p, packet.status_block, packet.status, packet.information);      // (a call cancelled by the close of a handle: its IO_STATUS_BLOCK is written now)
  if ((r.args[1] != 0 && !p.write_user_u64(r.args[1], packet.key)) || (r.args[2] != 0 && !p.write_user_u64(r.args[2], packet.context)) || !store_status_block(p, r.args[3], packet.status, packet.information)) return kAccessViolation;
  return kSuccess;
}

uint32_t nt_remove_io_completion_ex(Process& p, const SyscallRequest& r) {
  uint32_t port = 0;
  if (const uint32_t status = port_of(p, r.args[0], &port)) return status;
  uint64_t removed_at = 0, timeout_at = 0, alertable = 0;
  if (!argument(p, r, 3, &removed_at) || !argument(p, r, 4, &timeout_at) || !argument(p, r, 5, &alertable)) return kAccessViolation;
  uint32_t count = uint32_t(r.args[2]);
  if (count == 0) return kInvalidParameter;
  if (count > 64) count = 64;
  bool fault = false;
  const uint64_t timeout = timeout_ns_of(p, timeout_at, &fault);
  if (fault) return kAccessViolation;
  io::Packet packets[64];
  uint32_t got = 0;
  const uint32_t status = wait_port(p, port, timeout, (alertable & 0xFF) != 0, packets, count, &got);
  if (status != kSuccess) {
    if (status == kUserApc || status == kTimeout) { if (removed_at != 0) p.write_user_u32(removed_at, 0); }
    return status;
  }
  for (uint32_t i = 0; i < got; ++i) {      // FILE_IO_COMPLETION_INFORMATION { KeyContext, ApcContext, IoStatusBlock (Status, Information) }: 32 bytes
    const uint64_t at = r.args[1] + uint64_t{i} * 32;
    if (packets[i].status_block != 0 && packets[i].owner == p.id()) store_status_block(p, packets[i].status_block, packets[i].status, packets[i].information);
    if (!p.write_user_u64(at, packets[i].key) || !p.write_user_u64(at + 8, packets[i].context) || !store_status_block(p, at + 16, packets[i].status, packets[i].information)) return kAccessViolation;
  }
  if (removed_at != 0 && !p.write_user_u32(removed_at, got)) return kAccessViolation;
  return kSuccess;
}

uint32_t nt_query_io_completion(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  uint64_t slot = 0;
  if (!slot_of(r.args[0], &slot) || p.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  if (p.files().kind(slot) != DescriptorKind::IoCompletion) return kTypeMismatch;
  if (uint32_t(r.args[1]) != 0) return kInvalidInfoClass;      // IoCompletionBasicInformation { LONG Depth }
  if (uint32_t(r.args[3]) != 4) return kInfoLengthMismatch;
  uint64_t returned_at = 0;
  if (!argument(p, r, 4, &returned_at)) return kAccessViolation;
  objects->sync().lock();
  const uint64_t depth = objects->port_depth_locked(uint32_t(p.files().token(slot)));
  objects->sync().unlock();
  if (!p.write_user_u32(r.args[2], uint32_t(depth))) return kAccessViolation;
  if (returned_at != 0 && !p.write_user_u32(returned_at, 4)) return kAccessViolation;
  return kSuccess;
}

// ---- file information

bool io_set_file_information(Process& p, uint64_t slot, uint32_t info_class, uint64_t buffer, uint32_t length, uint32_t* status) {
  if (info_class != 30 && info_class != 41) return false;
  ObjectManager* objects = system_objects();
  if (!objects) { *status = kNotImplemented; return true; }
  const DescriptorKind kind = p.files().kind(slot);
  if (info_class == 30) {      // FileCompletionInformation { HANDLE Port; PVOID Key }
    if (length < 16) { *status = kInfoLengthMismatch; return true; }
    uint64_t port_handle = 0, key = 0, port_slot = 0;
    if (!p.read_user_u64(buffer, &port_handle) || !p.read_user_u64(buffer + 8, &key)) { *status = kAccessViolation; return true; }
    if (!slot_of(port_handle, &port_slot) || p.files().kind(port_slot) == DescriptorKind::Closed) { *status = kInvalidHandle; return true; }
    if (p.files().kind(port_slot) != DescriptorKind::IoCompletion) { *status = kTypeMismatch; return true; }
    const uint32_t port = uint32_t(p.files().token(port_slot));
    if (kind == DescriptorKind::Pipe) { *status = pipe_handle_set_port(p, slot, port, key); return true; }
    OpenObject* o = p.files().object(slot);
    if (!o || (kind != DescriptorKind::File && kind != DescriptorKind::Directory)) { *status = kInvalidParameter; return true; }
    if (o->io_port != 0) { *status = kInvalidParameter; return true; }      // (a file is tied once)
    objects->add_ref(port);
    o->io_port = port; o->io_key = key; o->io_flags = 0;
    *status = kSuccess;
    return true;
  }
  // FileIoCompletionNotificationInformation { ULONG Flags }
  if (length < 4) { *status = kInfoLengthMismatch; return true; }
  uint8_t raw[4];
  if (!p.read_user(buffer, ByteView(raw, 4))) { *status = kAccessViolation; return true; }
  const uint32_t flags = raw[0] | (uint32_t{raw[1]} << 8) | (uint32_t{raw[2]} << 16) | (uint32_t{raw[3]} << 24);
  if (flags & ~uint32_t{7}) { *status = kInvalidParameter; return true; }
  if (kind == DescriptorKind::Pipe) { *status = pipe_handle_set_flags(p, slot, flags); return true; }
  OpenObject* o = p.files().object(slot);
  if (!o) { *status = kInvalidParameter; return true; }
  o->io_flags = flags;
  *status = kSuccess;
  return true;
}

bool io_query_file_information(Process& p, uint64_t slot, uint32_t info_class, uint64_t buffer, uint32_t length, uint32_t* status, uint64_t* size) {
  if (info_class != 41) return false;
  if (length < 4) { *status = kInfoLengthMismatch; return true; }
  uint32_t port = 0, flags = 0;
  uint64_t key = 0;
  io_handle_port(p, slot, &port, &key, &flags);
  if (p.files().kind(slot) == DescriptorKind::Pipe) pipe_handle_flags(p, slot, &flags);
  else if (OpenObject* o = p.files().object(slot)) flags = o->io_flags;
  *status = p.write_user_u32(buffer, flags) ? kSuccess : kAccessViolation;
  *size = 4;
  return true;
}

}  // namespace process::windowsabi
