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
#include "Process/WindowsDebug.hpp"
#include "Process/Objects.hpp"
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsProcess.hpp"
#include "Process/WindowsWait.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kAccessDenied = 0xC0000022u,
                   kNotImplemented = 0xC0000002u, kNoResources = 0xC000009Au, kPortAlreadySet = 0xC0000048u, kPortNotSet = 0xC0000353u, kInfoLengthMismatch = 0xC0000004u,
                   kInvalidInfoClass = 0xC0000003u, kObjectTypeMismatch = 0xC0000024u, kProcessIsTerminating = 0xC000010Au;
constexpr uint32_t kDbgContinue = 0x00010002u, kDbgNotHandled = 0x80010001u;
constexpr uint64_t kPending = kWaitPending;

// DBG_STATE
enum : uint32_t { kStateCreateThread = 2, kStateCreateProcess = 3, kStateExitThread = 4, kStateExitProcess = 5, kStateException = 6, kStateLoadDll = 9, kStateUnloadDll = 10 };

constexpr uint32_t kMaxObjects = 8, kMaxEvents = 48, kMaxAttached = 16, kInfoBytes = 0xA8;      // the state-specific part of a DBGUI_WAIT_STATE_CHANGE (0x18 .. 0xC0)

struct Event {
  bool used = false, delivered = false, continued = false, blocking = false, orphan = false;
  uint32_t state = 0, status = kDbgNotHandled;
  uint64_t pid = 0, tid = 0, base = 0, start = 0;      // base/start: the image base and start address of a process or thread, the base of a library
  uint8_t info[kInfoBytes] = {};
};
struct DebugObject {
  bool used = false;
  uint32_t object = 0, flags = 0;
  Event events[kMaxEvents];
};
struct Attached { bool used = false; uint64_t pid = 0; uint32_t object = 0; bool children = false; };

DebugObject g_objects[kMaxObjects];
Attached g_attached[kMaxAttached];

void put(uint8_t* at, uint64_t value, uint32_t bytes) { for (uint32_t i = 0; i < bytes; ++i) at[i] = uint8_t(value >> (8 * i)); }

bool slot_of(uint64_t handle, uint64_t* slot) {
  handle &= ~uint64_t{3};
  if (handle == 0 || handle / 4 > Process::kMaxDescriptors) return false;
  *slot = handle / 4 - 1;
  return true;
}
// The debug object of a handle (with the system lock not held).
DebugObject* object_of_handle(Process& p, uint64_t handle, uint32_t* status) {
  uint64_t slot = 0;
  if (!slot_of(handle, &slot) || p.files().kind(slot) == DescriptorKind::Closed) { *status = kInvalidHandle; return nullptr; }
  if (p.files().kind(slot) != DescriptorKind::DebugObject) { *status = kObjectTypeMismatch; return nullptr; }
  const uint32_t id = uint32_t(p.files().token(slot));
  for (DebugObject& o : g_objects) if (o.used && o.object == id) return &o;
  *status = kInvalidHandle;
  return nullptr;
}
Attached* attachment_of(uint64_t pid) {
  for (Attached& a : g_attached) if (a.used && a.pid == pid) return &a;
  return nullptr;
}
DebugObject* object_by_id(uint32_t id) {
  for (DebugObject& o : g_objects) if (o.used && o.object == id) return &o;
  return nullptr;
}

// Queues an event (the system lock held). Returns it, or null if the queue is full.
Event* post_locked(DebugObject& o, uint32_t state, uint64_t pid, uint64_t tid, bool blocking) {
  for (Event& e : o.events) {
    if (e.used) continue;
    e = Event();
    e.used = true;
    e.state = state;
    e.pid = pid;
    e.tid = tid;
    e.blocking = blocking;
    return &e;
  }
  return nullptr;
}

// A handle in the calling process to a process or thread (by its id) for an event.
uint64_t give_object_handle(Process& p, uint64_t id) {
  ObjectManager* objects = system_objects();
  if (!objects || id == 0) return 0;
  const bool exists = id == p.id() || (p.runtime() && p.runtime()->process_exists(id & 0xFFFFF) );
  ObjectManager::Created c = objects->open_process(id, exists);
  if (c.id == 0) c = objects->create_process(id);
  if (c.id == 0) return 0;
  const int64_t slot = p.files().open_token(DescriptorKind::Process, c.id);
  if (slot < 0) { objects->release(c.id); return 0; }
  return (uint64_t(slot) + 1) * 4;
}

// Writes the event as a DBGUI_WAIT_STATE_CHANGE; the handles in it are made for the caller.
bool write_state_change(Process& p, uint64_t at, const Event& e) {
  uint8_t out[0x18 + kInfoBytes] = {};
  put(out + 0, e.state, 4);
  put(out + 8, e.pid, 8);
  put(out + 16, e.tid, 8);
  for (uint32_t i = 0; i < kInfoBytes; ++i) out[0x18 + i] = e.info[i];
  switch (e.state) {
    case kStateCreateProcess:
      put(out + 0x18, give_object_handle(p, e.pid), 8);      // HandleToProcess
      put(out + 0x20, give_object_handle(p, e.tid), 8);      // HandleToThread
      put(out + 0x38, e.base, 8);                            // BaseOfImage
      put(out + 0x50, e.start, 8);                           // InitialThread.StartAddress
      break;
    case kStateCreateThread:
      put(out + 0x18, give_object_handle(p, e.tid), 8);
      put(out + 0x28, e.start, 8);
      break;
    case kStateLoadDll: put(out + 0x20, e.base, 8); break;
    case kStateUnloadDll: put(out + 0x18, e.base, 8); break;
    default: break;
  }
  return p.write_user(at, ConstByteView(out, sizeof(out)));
}

uint64_t timeout_ns_of(Process& p, uint64_t at, bool* bad) {
  if (at == 0) return ~uint64_t{0};
  uint64_t raw = 0;
  if (!p.read_user_u64(at, &raw)) { *bad = true; return 0; }
  uint64_t units = 0;
  if (int64_t(raw) < 0) units = uint64_t(0) - raw;
  else return 0;      // (an absolute time is taken as: no wait)
  return units > ~uint64_t{0} / 100 ? ~uint64_t{0} : units * 100;
}

// The events that tell a debugger what a program already has when it is attached: the program, its libraries and its threads.
struct Snapshot {
  uint64_t base = 0, start = 0, main_thread = 0;
  IProcessRuntime::ThreadEntry threads[64];
  uint32_t thread_count = 0;
  IProcessRuntime::ModuleEntry modules[64];
  uint32_t module_count = 0;
};
bool take_snapshot(Process& p, uint64_t pid, Snapshot* s) {
  IProcessRuntime* runtime = p.runtime();
  if (!runtime || !runtime->process_exists(pid)) return false;
  IProcessRuntime::RemoteOp op;
  op.kind = IProcessRuntime::RemoteOp::ProcessInfo;
  if (runtime->remote_operation(pid, op) != kSuccess) return false;
  const uint64_t peb = op.address;
  s->start = 0;
  for (uint32_t i = 0; i < 8; ++i) s->start |= uint64_t{op.image[i]} << (8 * i);      // TransferAddress
  uint8_t base[8] = {};
  IProcessRuntime::RemoteOp read;
  read.kind = IProcessRuntime::RemoteOp::Read;
  read.address = peb + 0x10;
  read.size = 8;
  read.out = ByteView(base, 8);
  if (runtime->remote_operation(pid, read) == kSuccess) for (uint32_t i = 0; i < 8; ++i) s->base |= uint64_t{base[i]} << (8 * i);
  s->thread_count = runtime->list_threads(pid, s->threads, 64);
  s->module_count = runtime->list_modules(pid, s->modules, 64);
  s->main_thread = pid;
  // BeingDebugged
  uint8_t one = 1;
  IProcessRuntime::RemoteOp write;
  write.kind = IProcessRuntime::RemoteOp::Write;
  write.address = peb + 2;
  write.in = ConstByteView(&one, 1);
  runtime->remote_operation(pid, write);
  return true;
}

void clear_being_debugged(Process& p, uint64_t pid) {
  IProcessRuntime* runtime = p.runtime();
  if (!runtime) return;
  IProcessRuntime::RemoteOp op;
  op.kind = IProcessRuntime::RemoteOp::ProcessInfo;
  if (runtime->remote_operation(pid, op) != kSuccess) return;
  uint8_t zero = 0;
  IProcessRuntime::RemoteOp write;
  write.kind = IProcessRuntime::RemoteOp::Write;
  write.address = op.address + 2;
  write.in = ConstByteView(&zero, 1);
  runtime->remote_operation(pid, write);
}

uint32_t attach(Process& p, DebugObject* debug_object_unused, uint32_t object_id, uint64_t pid, bool children) {
  (void)debug_object_unused;
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  Snapshot* snapshot = static_cast<Snapshot*>(p.files().allocator().allocate(sizeof(Snapshot)));
  if (!snapshot) return kNoResources;
  new (snapshot) Snapshot();
  if (!take_snapshot(p, pid, snapshot)) { p.files().allocator().deallocate(snapshot); return kProcessIsTerminating; }
  uint32_t status = kSuccess;
  ISystemSync& sync = objects->sync();
  sync.lock();
  DebugObject* o = object_by_id(object_id);
  Attached* slot = nullptr;
  if (!o) status = kInvalidHandle;
  else if (attachment_of(pid)) status = kPortAlreadySet;
  else for (Attached& a : g_attached) if (!a.used) { slot = &a; break; }
  if (status == kSuccess && !slot) status = kNoResources;
  if (status == kSuccess) {
    slot->used = true; slot->pid = pid; slot->object = object_id; slot->children = children;
    if (Event* e = post_locked(*o, kStateCreateProcess, pid, snapshot->main_thread, false)) { e->base = snapshot->base; e->start = snapshot->start; }
    // ntdll first, then the threads, then the other libraries
    for (uint32_t i = 0; i < snapshot->module_count; ++i) {
      const IProcessRuntime::ModuleEntry& m = snapshot->modules[i];
      if (m.program || !(m.name[0] == 'n' && m.name[1] == 't' && m.name[2] == 'd' && m.name[3] == 'l' && m.name[4] == 'l')) continue;
      if (Event* e = post_locked(*o, kStateLoadDll, pid, snapshot->main_thread, false)) e->base = m.base;
    }
    for (uint32_t i = 0; i < snapshot->thread_count; ++i) {
      const IProcessRuntime::ThreadEntry& t = snapshot->threads[i];
      if (t.id == snapshot->main_thread) continue;
      if (Event* e = post_locked(*o, kStateCreateThread, pid, t.id, false)) e->start = t.start_address;
    }
    for (uint32_t i = 0; i < snapshot->module_count; ++i) {
      const IProcessRuntime::ModuleEntry& m = snapshot->modules[i];
      if (m.program || (m.name[0] == 'n' && m.name[1] == 't' && m.name[2] == 'd' && m.name[3] == 'l' && m.name[4] == 'l')) continue;
      if (Event* e = post_locked(*o, kStateLoadDll, pid, snapshot->main_thread, false)) e->base = m.base;
    }
    sync.notify_all();
  }
  sync.unlock();
  p.files().allocator().deallocate(snapshot);
  return status;
}

}  // namespace

void debug_note_created(uint32_t object, uint32_t flags) {
  ObjectManager* objects = system_objects();
  if (!objects) return;
  objects->sync().lock();
  for (DebugObject& o : g_objects) {
    if (o.used) continue;
    o = DebugObject();
    o.used = true;
    o.object = object;
    o.flags = flags;
    break;
  }
  objects->sync().unlock();
}

// (the system lock is held) The last handle to the debug object is gone: what it debugged goes on by itself, and whoever waited for an answer gets one.
void debug_object_freed_locked(uint32_t object) {
  for (Attached& a : g_attached) if (a.used && a.object == object) a = Attached();
  for (DebugObject& o : g_objects) {
    if (!o.used || o.object != object) continue;
    for (Event& e : o.events) {
      if (!e.used) continue;
      if (e.blocking && !e.orphan) { e.continued = true; e.status = kDbgContinue; e.delivered = true; }      // (the program that waits frees it)
      else e = Event();
    }
    o.used = false;
  }
}

uint32_t debug_object_of(uint64_t pid) {
  ObjectManager* objects = system_objects();
  if (!objects) return 0;
  objects->sync().lock();
  const Attached* a = attachment_of(pid);
  const uint32_t id = a ? a->object : 0;
  objects->sync().unlock();
  return id;
}

bool debug_wants_child(uint64_t parent_pid) {
  ObjectManager* objects = system_objects();
  if (!objects) return false;
  objects->sync().lock();
  const Attached* a = attachment_of(parent_pid);
  const bool wants = a && a->children;
  objects->sync().unlock();
  return wants;
}
void debug_set_inherit(uint64_t pid, bool inherit) {
  ObjectManager* objects = system_objects();
  if (!objects) return;
  objects->sync().lock();
  if (Attached* a = attachment_of(pid)) a->children = inherit;
  objects->sync().unlock();
}
uint32_t debug_attach_child(Process& parent, uint64_t child_pid) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  objects->sync().lock();
  const Attached* a = attachment_of(parent.id());
  const uint32_t object = a ? a->object : 0;
  objects->sync().unlock();
  return object ? attach(parent, nullptr, object, child_pid, true) : kSuccess;
}

// A blocking event of the calling program: queued, then waited for until the debugger has answered. Returns the answer.
static uint32_t post_and_wait(Process& p, uint32_t state, uint64_t tid, const uint8_t* info, uint32_t info_bytes) {
  ObjectManager* objects = system_objects();
  if (!objects) return kDbgNotHandled;
  ISystemSync& sync = objects->sync();
  sync.lock();
  const Attached* a = attachment_of(p.id());
  DebugObject* o = a ? object_by_id(a->object) : nullptr;
  Event* e = o ? post_locked(*o, state, p.id(), tid, true) : nullptr;
  if (!e) { sync.unlock(); return kDbgNotHandled; }
  for (uint32_t i = 0; i < info_bytes && i < kInfoBytes; ++i) e->info[i] = info[i];
  sync.notify_all();
  sync.unlock();
  uint32_t answer = kDbgNotHandled;
  const uint32_t waited = wait_until(p, ~uint64_t{0}, [&]() -> uint64_t {
    if (!e->used) return kSuccess;
    if (e->continued) { answer = e->status; *e = Event(); return kSuccess; }
    return kPending;
  });
  if (waited != kSuccess) {      // (the program ends): the event stays for the debugger if it was delivered, else it goes
    sync.lock();
    if (e->used && e->state == state && e->tid == tid && !e->continued) { e->orphan = true; if (!e->delivered) *e = Event(); }
    sync.unlock();
  }
  return answer;
}

static void post_plain(uint64_t pid, uint32_t state, uint64_t tid, uint64_t base, uint64_t start, uint32_t status) {
  ObjectManager* objects = system_objects();
  if (!objects) return;
  ISystemSync& sync = objects->sync();
  sync.lock();
  const Attached* a = attachment_of(pid);
  DebugObject* o = a ? object_by_id(a->object) : nullptr;
  if (o) {
    if (Event* e = post_locked(*o, state, pid, tid, false)) {
      e->base = base;
      e->start = start;
      if (state == kStateExitThread || state == kStateExitProcess) put(e->info, status, 4);      // ExitStatus is at 0x18
      sync.notify_all();
    }
  }
  sync.unlock();
}

void debug_thread_created(Process& p, uint64_t thread_id, uint64_t start) { if (debug_object_of(p.id())) post_plain(p.id(), kStateCreateThread, thread_id, 0, start, 0); }
void debug_thread_exited(Process& p, uint64_t thread_id, uint32_t code) { if (debug_object_of(p.id())) post_plain(p.id(), kStateExitThread, thread_id, 0, 0, code); }
void debug_module_loaded(Process& p, uint64_t base) { if (debug_object_of(p.id())) post_plain(p.id(), kStateLoadDll, p.runtime() ? p.runtime()->current_thread_id() : p.id(), base, 0, 0); }
void debug_module_unloaded(Process& p, uint64_t base) { if (debug_object_of(p.id())) post_plain(p.id(), kStateUnloadDll, p.runtime() ? p.runtime()->current_thread_id() : p.id(), base, 0, 0); }

void debug_process_exited(uint64_t pid, uint32_t code) {
  ObjectManager* objects = system_objects();
  if (!objects) return;
  ISystemSync& sync = objects->sync();
  sync.lock();
  Attached* a = attachment_of(pid);
  DebugObject* o = a ? object_by_id(a->object) : nullptr;
  if (o) {
    for (Event& e : o->events) if (e.used && e.pid == pid && e.blocking) { e.orphan = true; if (!e.delivered) e = Event(); }
    if (Event* e = post_locked(*o, kStateExitProcess, pid, pid, false)) put(e->info, code, 4);
    sync.notify_all();
  }
  if (a) *a = Attached();
  sync.unlock();
}

uint32_t nt_debug_active_process(Process& p, const SyscallRequest& r) {
  bool exited = false;
  uint32_t code = 0;
  uint64_t pid = 0;
  if (!process_handle_info(p, r.args[0], &exited, &code, &pid)) return kInvalidHandle;
  uint32_t status = kSuccess;
  DebugObject* o = object_of_handle(p, r.args[1], &status);
  if (!o) return status;
  if (exited) return kProcessIsTerminating;
  if (pid == p.id()) return kAccessDenied;
  uint64_t slot = 0;
  slot_of(r.args[1], &slot);
  return attach(p, o, uint32_t(p.files().token(slot)), pid, false);
}

uint32_t nt_remove_process_debug(Process& p, const SyscallRequest& r) {
  bool exited = false;
  uint32_t code = 0;
  uint64_t pid = 0;
  if (!process_handle_info(p, r.args[0], &exited, &code, &pid)) return kInvalidHandle;
  uint32_t status = kSuccess;
  DebugObject* o = object_of_handle(p, r.args[1], &status);
  if (!o) return status;
  ObjectManager* objects = system_objects();
  ISystemSync& sync = objects->sync();
  sync.lock();
  Attached* a = attachment_of(pid);
  if (!a || a->object != o->object) { sync.unlock(); return kPortNotSet; }
  for (Event& e : o->events) {      // what waits for an answer gets one; the rest of this program's events are dropped
    if (!e.used || e.pid != pid) continue;
    if (e.blocking && !e.orphan) { e.continued = true; e.status = kDbgContinue; e.delivered = true; }
    else e = Event();
  }
  *a = Attached();
  sync.notify_all();
  sync.unlock();
  if (!exited) clear_being_debugged(p, pid);
  return kSuccess;
}

uint32_t nt_wait_for_debug_event(Process& p, const SyscallRequest& r) {
  uint64_t state_at = 0;
  if (!argument(p, r, 3, &state_at)) return kAccessViolation;
  uint32_t status = kSuccess;
  DebugObject* o = object_of_handle(p, r.args[0], &status);
  if (!o) return status;
  bool bad = false;
  const uint64_t timeout = timeout_ns_of(p, r.args[2], &bad);
  if (bad) return kAccessViolation;
  Event copy;
  const uint32_t waited = wait_until(p, timeout, [&]() -> uint64_t {
    if (!o->used) return kInvalidHandle;
    for (Event& e : o->events) if (e.used && e.delivered && !e.continued) return kPending;      // (the next one is not reported until this one is continued)
    for (Event& e : o->events) {
      if (!e.used || e.delivered || e.orphan) continue;
      e.delivered = true;
      copy = e;
      return kSuccess;
    }
    return kPending;
  });
  if (waited != kSuccess) return waited == 0x101 ? 0x101u : waited;
  return write_state_change(p, state_at, copy) ? kSuccess : kAccessViolation;
}

uint32_t nt_debug_continue(Process& p, const SyscallRequest& r) {
  uint8_t id[16];
  if (r.args[1] == 0 || !p.read_user(r.args[1], ByteView(id, 16))) return kAccessViolation;
  uint64_t pid = 0, tid = 0;
  for (uint32_t i = 0; i < 8; ++i) { pid |= uint64_t{id[i]} << (8 * i); tid |= uint64_t{id[8 + i]} << (8 * i); }
  uint32_t status = kSuccess;
  DebugObject* o = object_of_handle(p, r.args[0], &status);
  if (!o) return status;
  const uint32_t answer = uint32_t(r.args[2]);
  if (answer != kDbgContinue && answer != kDbgNotHandled && answer != 0x40010001u && answer != 0x40010002u && answer != 0x00010001u && answer != 0xC0000005u && true) {
    // (other statuses are passed on as they are: the debuggee sees them as the answer)
  }
  ObjectManager* objects = system_objects();
  ISystemSync& sync = objects->sync();
  sync.lock();
  status = kInvalidParameter;
  for (Event& e : o->events) {
    if (!e.used || !e.delivered || e.continued || e.pid != pid || e.tid != tid) continue;
    if (answer == 0x40010001u) e.delivered = false;      // DBG_REPLY_LATER: the debugger gets the event again
    else if (e.blocking && !e.orphan) { e.continued = true; e.status = answer; }
    else e = Event();
    status = kSuccess;
    break;
  }
  sync.notify_all();
  sync.unlock();
  return status;
}

uint32_t nt_set_information_debug_object(Process& p, const SyscallRequest& r) {
  uint32_t status = kSuccess;
  DebugObject* o = object_of_handle(p, r.args[0], &status);
  if (!o) return status;
  uint64_t returned = 0;
  if (!argument(p, r, 4, &returned)) return kAccessViolation;
  if (uint32_t(r.args[1]) != 1) return kInvalidInfoClass;
  if ((r.args[3] & 0xFFFFFFFFu) != 4) return kInfoLengthMismatch;
  uint8_t raw[4] = {};
  if (!p.read_user(r.args[2], ByteView(raw, 4))) return kAccessViolation;
  const uint32_t flags = raw[0] | (uint32_t{raw[1]} << 8) | (uint32_t{raw[2]} << 16) | (uint32_t{raw[3]} << 24);
  if (flags & ~uint32_t{1}) return kInvalidParameter;
  ObjectManager* objects = system_objects();
  objects->sync().lock();
  o->flags = (o->flags & ~uint32_t{1}) | flags;
  objects->sync().unlock();
  if (returned != 0 && !p.write_user_u32(returned, 0)) return kAccessViolation;
  return kSuccess;
}

uint32_t nt_tcos_debug_exception(Process& p, const SyscallRequest& r) {
  if (!debug_object_of(p.id())) return kDbgNotHandled;
  uint8_t record[0x98];
  if (!p.read_user(r.args[0], ByteView(record, sizeof(record)))) return kDbgNotHandled;
  uint8_t info[0xA8] = {};
  for (uint32_t i = 0; i < sizeof(record); ++i) info[i] = record[i];
  put(info + 0x98, (r.args[1] & 0xFF) ? 1 : 0, 4);      // FirstChance
  return post_and_wait(p, kStateException, p.runtime() ? p.runtime()->current_thread_id() : p.id(), info, sizeof(info));
}

}  // namespace process::windowsabi
