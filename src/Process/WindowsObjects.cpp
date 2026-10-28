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
#include "Process/WindowsObjects.hpp"
#include "Process/WindowsMemory.hpp"
#include "Process/ModuleLoader.hpp"
#include "Executable/PeFile.hpp"
#include "Process/Objects.hpp"
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsMailslots.hpp"
#include "Process/WindowsRegistry.hpp"
#include "Process/WindowsPipes.hpp"
#include "Process/WindowsSockets.hpp"
#include "Process/WindowsProcess.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kUserApc = 0xC0, kSuccess = 0, kTimeout = 0x102, kAlerted = 0x101, kAbandonedWait0 = 0x80, kNotImplemented = 0xC0000002u, kInvalidHandle = 0xC0000008u,
                   kInvalidParameter = 0xC000000Du, kAccessViolation = 0xC0000005u, kTypeMismatch = 0xC0000024u, kNoResources = 0xC000009Au,
                   kNameInvalid = 0xC0000033u, kNameTooLong = 0xC0000106u;
constexpr uint64_t kUnixToNt = 11644473600ull * 10000000ull;
constexpr uint64_t kCurrentProcess = ~uint64_t{0};
constexpr uint32_t kMaxWait = 64;

bool is_object_kind(DescriptorKind k) { return k == DescriptorKind::Event || k == DescriptorKind::Mutex || k == DescriptorKind::Semaphore || k == DescriptorKind::Process || k == DescriptorKind::Timer || k == DescriptorKind::Section || k == DescriptorKind::Job || k == DescriptorKind::ChangeNotification; }
DescriptorKind descriptor_kind_of(ObjectKind k) {
  switch (k) {
    case ObjectKind::Event: return DescriptorKind::Event;
    case ObjectKind::Mutex: return DescriptorKind::Mutex;
    case ObjectKind::Process: return DescriptorKind::Process;
    case ObjectKind::Timer: return DescriptorKind::Timer;
    case ObjectKind::Section: return DescriptorKind::Section;
    case ObjectKind::Job: return DescriptorKind::Job;
    case ObjectKind::ChangeNotification: return DescriptorKind::ChangeNotification;
    default: return DescriptorKind::Semaphore;
  }
}

bool slot_of(uint64_t handle, uint64_t* slot) {
  handle &= ~uint64_t{3};      // (the low two bits of a handle are free for the program's own use, as on Windows)
  if (handle == 0 || handle / 4 > Process::kMaxDescriptors) return false;
  *slot = handle / 4 - 1;
  return true;
}

// The name in an OBJECT_ATTRIBUTES, as UTF-8 without the "\BaseNamedObjects\" in front. An object without a name has an empty one.
uint32_t read_name(Process& p, uint64_t attributes, char* out, uint32_t capacity, uint32_t* size) {
  *size = 0;
  if (attributes == 0) return kSuccess;
  uint64_t name_at = 0;
  if (!p.read_user_u64(attributes + 16, &name_at)) return kAccessViolation;
  if (name_at == 0) return kSuccess;
  uint8_t header[16];
  if (!p.read_user(name_at, ByteView(header, sizeof(header)))) return kAccessViolation;
  const uint32_t bytes = uint32_t(header[0]) | uint32_t(header[1]) << 8;
  uint64_t buffer = 0;
  for (int i = 0; i < 8; ++i) buffer |= uint64_t{header[8 + i]} << (8 * i);
  if (bytes == 0) return kSuccess;
  if (bytes % 2 != 0 || bytes > 2 * 256) return kNameTooLong;
  uint8_t units[2 * 256];
  if (!p.read_user(buffer, ByteView(units, bytes))) return kAccessViolation;
  uint32_t n = 0;
  for (uint32_t i = 0; i < bytes / 2; ++i) {
    uint32_t cp = units[2 * i] | uint32_t(units[2 * i + 1]) << 8;
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < bytes / 2) {      // a surrogate pair
      const uint32_t low = units[2 * i + 2] | uint32_t(units[2 * i + 3]) << 8;
      if (low >= 0xDC00 && low < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00); ++i; }
    }
    char encoded[4];
    uint32_t len;
    if (cp < 0x80) { encoded[0] = char(cp); len = 1; }
    else if (cp < 0x800) { encoded[0] = char(0xC0 | (cp >> 6)); encoded[1] = char(0x80 | (cp & 0x3F)); len = 2; }
    else if (cp < 0x10000) { encoded[0] = char(0xE0 | (cp >> 12)); encoded[1] = char(0x80 | ((cp >> 6) & 0x3F)); encoded[2] = char(0x80 | (cp & 0x3F)); len = 3; }
    else { encoded[0] = char(0xF0 | (cp >> 18)); encoded[1] = char(0x80 | ((cp >> 12) & 0x3F)); encoded[2] = char(0x80 | ((cp >> 6) & 0x3F)); encoded[3] = char(0x80 | (cp & 0x3F)); len = 4; }
    if (n + len >= capacity) return kNameTooLong;
    for (uint32_t k = 0; k < len; ++k) out[n++] = encoded[k];
  }
  // Names are in the object directory of the session: "\BaseNamedObjects\Name" is "Name".
  static const char prefix[] = "\\BaseNamedObjects\\";
  const uint32_t plen = sizeof(prefix) - 1;
  bool has_prefix = n >= plen;
  for (uint32_t i = 0; has_prefix && i < plen; ++i) {
    const char a = out[i] >= 'A' && out[i] <= 'Z' ? char(out[i] + 32) : out[i];
    const char b = prefix[i] >= 'A' && prefix[i] <= 'Z' ? char(prefix[i] + 32) : prefix[i];
    has_prefix = a == b;
  }
  if (has_prefix) {
    for (uint32_t i = plen; i < n; ++i) out[i - plen] = out[i];
    n -= plen;
  }
  for (uint32_t i = 0; i < n; ++i) if (out[i] == '\\' ) return kNameInvalid;      // (a name has no folders)
  *size = n;
  return kSuccess;
}

// Puts a handle for the object in the process's table and writes it to *handle_at.
bool wants_inherit(Process& p, uint64_t object_attributes) {      // OBJ_INHERIT in the OBJECT_ATTRIBUTES
  uint8_t raw[4];
  return object_attributes != 0 && p.read_user(object_attributes + 24, ByteView(raw, 4)) && (raw[0] & 2) != 0;
}
uint32_t give_handle(Process& p, ObjectManager& objects, ObjectKind kind, uint32_t id, uint64_t handle_at, uint32_t status, bool inherit) {
  const int64_t slot = p.files().open_token(descriptor_kind_of(kind), id);
  if (slot < 0) { objects.release(id); return kNoResources; }
  p.files().set_handle_flags(uint64_t(slot), inherit, false);
  if (!p.write_user_u64(handle_at, (uint64_t(slot) + 1) * 4)) {
    p.close_descriptor(uint64_t(slot));      // (releases the object)
    return kAccessViolation;
  }
  return status;
}

uint32_t create_object(Process& p, const SyscallRequest& r, ObjectKind kind, bool manual_reset, bool state, int32_t initial, int32_t maximum) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  char name[ObjectManager::kNameBytes + 1];
  uint32_t size = 0;
  const uint32_t status = read_name(p, r.args[2], name, ObjectManager::kNameBytes, &size);
  if (status != kSuccess) return status;
  const uint64_t owner = p.runtime() ? p.runtime()->current_thread_id() : p.id();
  const ObjectManager::Created c = objects->create(kind, StringView(name, size), manual_reset, state, initial, maximum, owner);
  if (c.id == 0) return c.status;
  return give_handle(p, *objects, kind, c.id, r.args[0], c.status, wants_inherit(p, r.args[2]));
}

uint32_t open_object(Process& p, const SyscallRequest& r, ObjectKind kind) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  char name[ObjectManager::kNameBytes + 1];
  uint32_t size = 0;
  const uint32_t status = read_name(p, r.args[2], name, ObjectManager::kNameBytes, &size);
  if (status != kSuccess) return status;
  if (size == 0) return kInvalidParameter;
  const ObjectManager::Created c = objects->open(kind, StringView(name, size));
  if (c.id == 0) return c.status;
  return give_handle(p, *objects, kind, c.id, r.args[0], kSuccess, wants_inherit(p, r.args[2]));
}

// The object a handle names, of the kind asked for. 0 if it is none.
uint32_t object_of(Process& p, uint64_t handle, DescriptorKind kind) {
  uint64_t slot = 0;
  if (!slot_of(handle, &slot) || p.files().kind(slot) != kind) return 0;
  return uint32_t(p.files().token(slot));
}

uint32_t put_previous(Process& p, uint64_t address, int32_t value) {
  if (address == 0) return kSuccess;
  const uint8_t raw[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
  return p.write_user(address, ConstByteView(raw, 4)) ? kSuccess : kAccessViolation;
}

// What can be waited for.
struct Waitable {
  enum Type { Invalid, Thread, Object } type = Invalid;
  uint64_t thread = 0;
  uint32_t object = 0;
};

bool resolve(Process& p, uint64_t handle, Waitable* w) {
  uint64_t slot = 0;
  if (!slot_of(handle, &slot)) return false;
  const DescriptorKind k = p.files().kind(slot);
  if (k == DescriptorKind::Thread) { w->type = Waitable::Thread; w->thread = p.files().token(slot); return true; }
  if (is_object_kind(k)) { w->type = Waitable::Object; w->object = uint32_t(p.files().token(slot)); return true; }
  return false;
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

// Waits until one (any) or all of the items are ready, or the time is up. Returns the NTSTATUS of the wait: the index of the item that ended it (all: 0), plus 0x80 if it
// was an abandoned mutex; STATUS_TIMEOUT; STATUS_ALERTED when the process is told to stop.
uint32_t wait_for(Process& p, const Waitable* items, uint32_t count, bool all, uint64_t timeout_ns, bool alertable = false) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  ISystemSync& sync = objects->sync();
  IProcessRuntime* runtime = p.runtime();
  const uint64_t waiter = runtime ? runtime->current_thread_id() : p.id();
  const uint64_t started = p.host().monotonic_ns();
  auto ready = [&](uint32_t i) {
    if (items[i].type == Waitable::Thread) {
      Process::ThreadRecord* t = p.find_thread(items[i].thread);
      return !t || t->finished;
    }
    return objects->ready_locked(items[i].object, waiter);
  };
  sync.lock();
  for (;;) {
    if (alertable && runtime) {      // an APC queued for this thread (or a timer of its that fired) ends the wait; the caller's library runs it
      ObjectManager::TimerApc fired[8];
      uint32_t fired_count = 0;
      objects->take_timer_apcs_locked(waiter, fired, 8, &fired_count);
      Process::ThreadRecord* me = p.find_thread(waiter);
      for (uint32_t i = 0; me && i < fired_count; ++i) Process::queue_apc(*me, Process::ThreadRecord::Apc{fired[i].routine, fired[i].context, fired[i].time & 0xFFFFFFFFu, fired[i].time >> 32});
      if (me && me->apc_count > 0) { sync.unlock(); return kUserApc; }
    }
    int64_t chosen = -1;
    if (all) {
      bool every = true;
      for (uint32_t i = 0; i < count && every; ++i) every = ready(i);
      if (every) chosen = 0;
    } else {
      for (uint32_t i = 0; i < count; ++i) if (ready(i)) { chosen = i; break; }
    }
    if (chosen >= 0) {
      uint32_t status = kSuccess + uint32_t(chosen);
      for (uint32_t i = all ? 0 : uint32_t(chosen); i < (all ? count : uint32_t(chosen) + 1); ++i) {
        if (items[i].type != Waitable::Object) continue;
        bool abandoned = false;
        objects->acquire_locked(items[i].object, waiter, &abandoned);
        if (abandoned && status < kAbandonedWait0) status = kAbandonedWait0 + i;
      }
      sync.unlock();
      return status;
    }
    if (timeout_ns == 0) { sync.unlock(); return kTimeout; }
    uint64_t left = ~uint64_t{0};
    if (timeout_ns != ~uint64_t{0}) {
      const uint64_t used = p.host().monotonic_ns() - started;
      if (used >= timeout_ns) { sync.unlock(); return kTimeout; }
      left = timeout_ns - used;
    }
    if (p.exited() || (runtime && runtime->interrupted())) { sync.unlock(); return kAlerted; }
    {      // wake up when a timer that is waited for (or one that will queue an APC) is due
      const uint64_t now = sync.now_ns();
      uint64_t due = alertable ? objects->thread_timer_deadline_locked(waiter) : ~uint64_t{0};
      for (uint32_t i = 0; i < count; ++i) if (items[i].type == Waitable::Object) { const uint64_t d = objects->timer_deadline_locked(items[i].object); if (d < due) due = d; }
      if (due != ~uint64_t{0}) {
        const uint64_t until = due > now ? due - now : 0;
        if (until < left) left = until;
      }
    }
    for (uint32_t i = 0; i < count; ++i) if (items[i].type == Waitable::Object) objects->waiting_locked(items[i].object, 1);
    if (runtime) runtime->unlock();      // the process's other threads go on while this one waits (the system lock stays held until the wait begins)
    else p.host().yield();
    sync.wait(left);
    for (uint32_t i = 0; i < count; ++i) if (items[i].type == Waitable::Object) objects->waiting_locked(items[i].object, -1);
    sync.unlock();      // (never wait for the process's lock with the system's held)
    if (runtime) runtime->lock();
    sync.lock();
  }
}

}  // namespace

// NtCreateEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, EVENT_TYPE (0: notification = manual reset, 1: synchronization = auto reset), BOOLEAN InitialState)
uint32_t nt_create_event(Process& p, const SyscallRequest& r) {
  uint64_t initial = 0;
  if (!argument(p, r, 4, &initial)) return kAccessViolation;
  const uint32_t type = uint32_t(r.args[3]);
  if (type > 1) return kInvalidParameter;
  return create_object(p, r, ObjectKind::Event, type == 0, (initial & 0xFF) != 0, 0, 0);
}
uint32_t nt_open_event(Process& p, const SyscallRequest& r) { return open_object(p, r, ObjectKind::Event); }

uint32_t nt_set_event(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  const uint32_t id = object_of(p, r.args[0], DescriptorKind::Event);
  if (!objects || !id) return objects ? kInvalidHandle : kNotImplemented;
  int32_t previous = 0;
  const uint32_t status = objects->set_event(id, &previous);
  return status != kSuccess ? status : put_previous(p, r.args[1], previous);
}
uint32_t nt_reset_event(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  // FindNextChangeNotification is this same call on a ChangeNotification handle (see ObjectManager::reset_event): both are "stays signalled until told
  // otherwise" handles with nothing else to resetting them.
  uint32_t id = object_of(p, r.args[0], DescriptorKind::Event);
  if (!id) id = object_of(p, r.args[0], DescriptorKind::ChangeNotification);
  if (!id) return kInvalidHandle;
  int32_t previous = 0;
  const uint32_t status = objects->reset_event(id, &previous);
  return status != kSuccess ? status : put_previous(p, r.args[1], previous);
}
uint32_t nt_pulse_event(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  const uint32_t id = object_of(p, r.args[0], DescriptorKind::Event);
  if (!objects || !id) return objects ? kInvalidHandle : kNotImplemented;
  int32_t previous = 0;
  const uint32_t status = objects->pulse_event(id, &previous);
  return status != kSuccess ? status : put_previous(p, r.args[1], previous);
}

// NtCreateMutant(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, BOOLEAN InitialOwner)
uint32_t nt_create_mutant(Process& p, const SyscallRequest& r) { return create_object(p, r, ObjectKind::Mutex, false, (r.args[3] & 0xFF) != 0, 0, 0); }
uint32_t nt_open_mutant(Process& p, const SyscallRequest& r) { return open_object(p, r, ObjectKind::Mutex); }
uint32_t nt_release_mutant(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  const uint32_t id = object_of(p, r.args[0], DescriptorKind::Mutex);
  if (!objects || !id) return objects ? kInvalidHandle : kNotImplemented;
  int32_t previous = 0;
  const uint32_t status = objects->release_mutex(id, p.runtime() ? p.runtime()->current_thread_id() : p.id(), &previous);
  return status != kSuccess ? status : put_previous(p, r.args[1], previous);
}

// NtCreateSemaphore(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, LONG InitialCount, LONG MaximumCount)
uint32_t nt_create_semaphore(Process& p, const SyscallRequest& r) {
  uint64_t maximum = 0;
  if (!argument(p, r, 4, &maximum)) return kAccessViolation;
  return create_object(p, r, ObjectKind::Semaphore, false, false, int32_t(uint32_t(r.args[3])), int32_t(uint32_t(maximum)));
}
uint32_t nt_open_semaphore(Process& p, const SyscallRequest& r) { return open_object(p, r, ObjectKind::Semaphore); }
uint32_t nt_release_semaphore(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  const uint32_t id = object_of(p, r.args[0], DescriptorKind::Semaphore);
  if (!objects || !id) return objects ? kInvalidHandle : kNotImplemented;
  int32_t previous = 0;
  const uint32_t status = objects->release_semaphore(id, int32_t(uint32_t(r.args[1])), &previous);
  return status != kSuccess ? status : put_previous(p, r.args[2], previous);
}

uint32_t nt_wait_for_single_object(Process& p, const SyscallRequest& r) {
  Waitable w;
  if (!resolve(p, r.args[0], &w)) {
    uint64_t slot = 0;
    return slot_of(r.args[0], &slot) && p.files().kind(slot) != DescriptorKind::Closed ? kTypeMismatch : kInvalidHandle;
  }
  bool fault = false;
  const uint64_t timeout = timeout_ns_of(p, r.args[2], &fault);
  if (fault) return kAccessViolation;
  return wait_for(p, &w, 1, false, timeout, (r.args[1] & 0xFF) != 0);
}

uint32_t nt_wait_for_multiple_objects(Process& p, const SyscallRequest& r) {
  uint64_t timeout_at = 0;
  if (!argument(p, r, 4, &timeout_at)) return kAccessViolation;
  const uint32_t count = uint32_t(r.args[0]);
  const uint32_t type = uint32_t(r.args[2]);
  if (count == 0 || count > kMaxWait || type > 1) return kInvalidParameter;
  Waitable items[kMaxWait];
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t handle = 0;
    if (!p.read_user_u64(r.args[1] + 8 * uint64_t{i}, &handle)) return kAccessViolation;
    if (!resolve(p, handle, &items[i])) return kInvalidHandle;
    for (uint32_t j = 0; j < i; ++j) {
      if (items[j].type == items[i].type && items[j].thread == items[i].thread && items[j].object == items[i].object) return kInvalidParameter;      // (the same object twice)
    }
  }
  bool fault = false;
  const uint64_t timeout = timeout_ns_of(p, timeout_at, &fault);
  if (fault) return kAccessViolation;
  return wait_for(p, items, count, type == 0, timeout, (r.args[3] & 0xFF) != 0);
}

// The handle in source_slot of this process, given to another running program (a process object handle names it).
uint32_t give_object_handle_in(Process& target, uint32_t kind, uint64_t token, uint32_t denied, bool inherit, uint64_t* handle) {
  const int64_t copy = target.files().open_token(DescriptorKind(kind), token);
  if (copy < 0) return kNoResources;
  target.files().set_denied_access(uint64_t(copy), denied);
  target.files().set_handle_flags(uint64_t(copy), inherit, false);
  *handle = (uint64_t(copy) + 1) * 4;
  return kSuccess;
}

namespace {

// NtDuplicateObject into a program other than the caller (only the kinds of the object manager, which count their references).
uint32_t duplicate_to_other(Process& p, const SyscallRequest& r, uint64_t target_at, uint64_t desired_access, uint64_t attributes, uint64_t options) {
  bool exited = false;
  uint32_t code = 0;
  uint64_t pid = 0;
  if (!process_handle_info(p, r.args[2], &exited, &code, &pid)) return kInvalidHandle;
  if (exited) return 0xC000010Au;      // STATUS_PROCESS_IS_TERMINATING
  if (!p.runtime()) return kNotImplemented;
  uint64_t slot = 0;
  if (!slot_of(r.args[1], &slot) || p.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  const DescriptorKind kind = p.files().kind(slot);
  ObjectManager* objects = system_objects();
  if (!is_object_kind(kind) || !objects) return kNotImplemented;
  const uint32_t id = uint32_t(p.files().token(slot));
  const uint32_t original = p.files().denied_access(slot);
  IProcessRuntime::RemoteOp op;
  op.kind = IProcessRuntime::RemoteOp::Duplicate;
  op.type = uint32_t(kind);
  op.argument = id;
  op.protect = (options & 2) ? original : (original | ~uint32_t(desired_access));
  op.suspended = (options & 4) ? p.files().inheritable(slot) : (attributes & 2) != 0;
  objects->add_ref(id);
  const uint32_t status = p.runtime()->remote_operation(pid, op);
  if (status != kSuccess) { objects->release(id); return status; }
  if (target_at != 0 && !p.write_user_u64(target_at, op.address)) return kAccessViolation;      // (the copy stays, as the handle was already made)
  if (options & 1) p.close_descriptor(slot);      // DUPLICATE_CLOSE_SOURCE
  return kSuccess;
}

}  // namespace

uint32_t nt_duplicate_object(Process& p, const SyscallRequest& r) {
  uint64_t target_at = r.args[3], options = 0, attributes = 0;
  uint64_t desired_access = 0;
  if (!argument(p, r, 4, &desired_access) || !argument(p, r, 5, &attributes) || !argument(p, r, 6, &options)) return kAccessViolation;
  const bool source_here = names_own_process(p, r.args[0]);
  const bool target_here = names_own_process(p, r.args[2]);
  if (!source_here) return kNotImplemented;      // (a handle of another program cannot be read from here)
  if (!target_here) return duplicate_to_other(p, r, target_at, desired_access, attributes, options);
  if (r.args[1] == ~uint64_t{1}) {      // the pseudo handle of the calling thread becomes a real handle to it
    const uint64_t self = p.runtime() ? p.runtime()->current_thread_id() : p.id();
    if (!p.find_thread(self)) return kInvalidHandle;
    const int64_t slot_for_thread = p.files().open_token(DescriptorKind::Thread, self);
    if (slot_for_thread < 0) return kNoResources;
    if (target_at != 0 && !p.write_user_u64(target_at, (uint64_t(slot_for_thread) + 1) * 4)) {
      p.close_descriptor(uint64_t(slot_for_thread));
      return kAccessViolation;
    }
    return kSuccess;
  }
  if (r.args[1] == ~uint64_t{0}) {      // the pseudo handle of this process becomes a real handle to it
    ObjectManager* objects = system_objects();
    if (!objects) return kNotImplemented;
    const ObjectManager::Created c = objects->open_process(p.id(), true);
    if (c.id == 0) return c.status;
    const int64_t slot_for_process = p.files().open_token(DescriptorKind::Process, c.id);
    if (slot_for_process < 0) { objects->release(c.id); return kNoResources; }
    if (target_at != 0 && !p.write_user_u64(target_at, (uint64_t(slot_for_process) + 1) * 4)) {
      p.close_descriptor(uint64_t(slot_for_process));
      return kAccessViolation;
    }
    p.files().set_handle_flags(uint64_t(slot_for_process), (attributes & 2) != 0, false);
    return kSuccess;
  }
  uint64_t slot = 0;
  if (!slot_of(r.args[1], &slot) || p.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  const int64_t copy = p.files().duplicate(slot, 0);
  if (copy < 0) return kNoResources;
  if (is_object_kind(p.files().kind(slot))) if (ObjectManager* objects = system_objects()) objects->add_ref(uint32_t(p.files().token(slot)));
  if (p.files().kind(slot) == DescriptorKind::Pipe) pipe_token_add(p.files().token(slot));
  if (p.files().kind(slot) == DescriptorKind::Socket) socket_token_add(p.files().token(slot));
  if (p.files().kind(slot) == DescriptorKind::Mailslot) mailslot_token_add(p.files().token(slot));
  if (p.files().kind(slot) == DescriptorKind::Key) key_token_add(p.files().token(slot));
  if (target_at != 0 && !p.write_user_u64(target_at, (uint64_t(copy) + 1) * 4)) {
    p.close_descriptor(uint64_t(copy));
    return kAccessViolation;
  }
  {      // DUPLICATE_SAME_ACCESS keeps the rights; otherwise the copy has the ones asked for (the rights of a section are the ones MapViewOfFile checks)
    const uint32_t original = p.files().denied_access(slot);
    p.files().set_denied_access(uint64_t(copy), (options & 2) ? original : (original | ~uint32_t(desired_access)));
  }
  const bool same = (options & 4) != 0;      // DUPLICATE_SAME_ATTRIBUTES; otherwise the handle is inheritable if the attributes say OBJ_INHERIT
  p.files().set_handle_flags(uint64_t(copy), same ? p.files().inheritable(slot) : (attributes & 2) != 0, false);
  if (options & 1) p.close_descriptor(slot);      // DUPLICATE_CLOSE_SOURCE
  return kSuccess;
}

// A sleep that an APC can end: the NtDelayExecution of a thread that asked to be alertable.
uint32_t alertable_delay(Process& p, uint64_t nanoseconds) {
  const uint32_t status = wait_for(p, nullptr, 0, false, nanoseconds, true);
  return status == kTimeout ? kSuccess : status;
}

// NtSignalAndWaitForSingleObject(SignalHandle, WaitHandle, Alertable, Timeout): sets the event / releases the mutex or semaphore, then waits (one step to the others).
uint32_t nt_signal_and_wait(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  uint64_t slot = 0;
  if (!slot_of(r.args[0], &slot) || p.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  const uint32_t id = uint32_t(p.files().token(slot));
  int32_t previous = 0;
  uint32_t status = kInvalidHandle;
  switch (p.files().kind(slot)) {
    case DescriptorKind::Event: status = objects->set_event(id, &previous); break;
    case DescriptorKind::Mutex: status = objects->release_mutex(id, p.runtime() ? p.runtime()->current_thread_id() : p.id(), &previous); break;
    case DescriptorKind::Semaphore: status = objects->release_semaphore(id, 1, &previous); break;
    default: return kTypeMismatch;
  }
  if (status != kSuccess) return status;
  Waitable w;
  if (!resolve(p, r.args[1], &w)) return kInvalidHandle;
  bool fault = false;
  const uint64_t timeout = timeout_ns_of(p, r.args[3], &fault);
  if (fault) return kAccessViolation;
  return wait_for(p, &w, 1, false, timeout, (r.args[2] & 0xFF) != 0);
}

// NtCreateTimer(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, TIMER_TYPE (0: notification, 1: synchronization)) and NtOpenTimer(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES).
uint32_t nt_create_timer(Process& p, const SyscallRequest& r) {
  const uint32_t type = uint32_t(r.args[3]);
  if (type > 1) return kInvalidParameter;
  return create_object(p, r, ObjectKind::Timer, type == 0, false, 0, 0);
}
uint32_t nt_open_timer(Process& p, const SyscallRequest& r) { return open_object(p, r, ObjectKind::Timer); }

// NtSetTimer(Handle, PLARGE_INTEGER DueTime (negative: relative, 100 ns), PTIMER_APC_ROUTINE, PVOID Context, BOOLEAN ResumeTimer, LONG Period (ms), PBOOLEAN PreviousState)
uint32_t nt_set_timer(Process& p, const SyscallRequest& r) {
  uint64_t resume = 0, period = 0, previous_at = 0;
  if (!argument(p, r, 4, &resume) || !argument(p, r, 5, &period) || !argument(p, r, 6, &previous_at)) return kAccessViolation;
  ObjectManager* objects = system_objects();
  const uint32_t id = object_of(p, r.args[0], DescriptorKind::Timer);
  if (!objects || !id) return objects ? kInvalidHandle : kNotImplemented;
  uint64_t raw = 0;
  if (r.args[1] == 0 || !p.read_user_u64(r.args[1], &raw)) return kAccessViolation;
  const int32_t period_ms = int32_t(uint32_t(period));
  if (period_ms < 0) return kInvalidParameter;
  uint64_t delay_ns = 0;
  if (int64_t(raw) < 0) {
    const uint64_t units = uint64_t(0) - raw;
    delay_ns = units > ~uint64_t{0} / 100 ? ~uint64_t{0} / 2 : units * 100;
  } else {
    const uint64_t now = p.host().realtime_ns() / 100 + kUnixToNt;
    delay_ns = raw > now ? (raw - now) * 100 : 0;
  }
  const uint64_t due = objects->sync().now_ns() + delay_ns;
  bool was = false;
  const uint32_t status = objects->set_timer(id, due, uint64_t(period_ms) * 1000000ull, p.runtime() ? p.runtime()->current_thread_id() : p.id(), r.args[2], r.args[3], &was);
  if (status != kSuccess) return status;
  if (previous_at != 0) { const uint8_t v = was ? 1 : 0; if (!p.write_user(previous_at, ConstByteView(&v, 1))) return kAccessViolation; }
  return kSuccess;
}
uint32_t nt_cancel_timer(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  const uint32_t id = object_of(p, r.args[0], DescriptorKind::Timer);
  if (!objects || !id) return objects ? kInvalidHandle : kNotImplemented;
  bool was = false;
  const uint32_t status = objects->cancel_timer(id, &was);
  if (status != kSuccess) return status;
  if (r.args[1] != 0) { const uint8_t v = was ? 1 : 0; if (!p.write_user(r.args[1], ConstByteView(&v, 1))) return kAccessViolation; }
  return kSuccess;
}
// NtQueryTimer(Handle, TimerBasicInformation = 0, Buffer, Length, ReturnLength): { LARGE_INTEGER RemainingTime; BOOLEAN TimerState }
uint32_t nt_query_timer(Process& p, const SyscallRequest& r) {
  uint64_t returned = 0;
  if (!argument(p, r, 4, &returned)) return kAccessViolation;
  ObjectManager* objects = system_objects();
  const uint32_t id = object_of(p, r.args[0], DescriptorKind::Timer);
  if (!objects || !id) return objects ? kInvalidHandle : kNotImplemented;
  if (uint32_t(r.args[1]) != 0) return 0xC0000003u;
  if (uint32_t(r.args[3]) < 16) return 0xC0000004u;
  uint64_t remaining = 0;
  bool signaled = false;
  const uint32_t status = objects->query_timer(id, &remaining, &signaled);
  if (status != kSuccess) return status;
  uint8_t out[16] = {};
  const uint64_t units = remaining / 100;
  for (uint32_t i = 0; i < 8; ++i) out[i] = uint8_t(units >> (8 * i));
  out[8] = signaled ? 1 : 0;
  if (!p.write_user(r.args[2], ConstByteView(out, 16))) return kAccessViolation;
  if (returned != 0 && !p.write_user_u32(returned, 16)) return kAccessViolation;
  return kSuccess;
}

// ---- sections (CreateFileMapping, MapViewOfFile)

namespace {

constexpr uint32_t kPageReadOnly = 0x02, kPageReadWrite = 0x04, kPageWriteCopy = 0x08, kPageExecute = 0x10, kPageExecuteRead = 0x20, kPageExecuteReadWrite = 0x40,
                   kPageExecuteWriteCopy = 0x80;
constexpr uint32_t kStatusSectionProtection = 0xC000004Eu, kStatusConflictingAddresses = 0xC0000018u, kStatusNotMappedView = 0xC0000019u;

// The permissions of a PAGE_* constant; `valid` is false for a constant that is none of them. PAGE_NOACCESS is valid (a view that cannot be touched).
uint8_t protection_of_page(uint32_t page, bool* private_copy, bool* valid = nullptr) {
  if (valid) *valid = page == 0x01 || page == kPageReadOnly || page == kPageReadWrite || page == kPageWriteCopy || page == kPageExecute || page == kPageExecuteRead || page == kPageExecuteReadWrite || page == kPageExecuteWriteCopy;
  *private_copy = page == kPageWriteCopy || page == kPageExecuteWriteCopy;
  uint8_t prot = 0;
  if (page == kPageReadOnly || page == kPageExecuteRead || page == kPageReadWrite || page == kPageWriteCopy || page == kPageExecuteReadWrite || page == kPageExecuteWriteCopy) prot |= kProtRead;
  if (page == kPageReadWrite || page == kPageExecuteReadWrite) prot |= kProtWrite;      // (a copy-on-write page is written to only after AddressSpace::break_copy_on_write)
  if (page == kPageExecute || page == kPageExecuteRead || page == kPageExecuteReadWrite || page == kPageExecuteWriteCopy) prot |= kProtExecute;
  return prot;
}

}  // namespace

// What NtCreateSection says about a file that is no image this OS can map: the status the headers call for (0 if the headers are fine).
uint32_t image_header_status(ConstByteView file) {
  auto le16 = [&](uint64_t at) -> uint32_t { return at <= file.size() && file.size() - at >= 2 ? uint32_t(file[at]) | (uint32_t(file[at + 1]) << 8) : 0; };
  auto le32 = [&](uint64_t at) -> uint64_t { return at <= file.size() && file.size() - at >= 4 ? uint64_t(le16(at)) | (uint64_t(le16(at + 2)) << 16) : ~uint64_t{0}; };
  if (file.size() < 0x40 || file[0] != 'M' || file[1] != 'Z') return 0xC000012Fu;      // STATUS_INVALID_IMAGE_NOT_MZ
  const uint64_t nt = le32(0x3C);
  if (nt > file.size() || file.size() - nt < 4) return 0xC0000130u;      // STATUS_INVALID_IMAGE_PROTECT
  if (le16(nt) == 0x454E) {      // "NE": a 16-bit program
    if (nt + 54 >= file.size()) return 0xC000011Bu;
    const uint8_t type = file[nt + 54];      // ne_exetyp
    return type == 2 ? 0xC0000131u : type == 5 ? 0xC0000130u : 0xC000011Bu;      // WIN_16, PROTECT (DOS 4), NE_FORMAT
  }
  if (le32(nt) != 0x4550) return 0xC0000130u;
  return 0;
}

// NtCreateSection(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PLARGE_INTEGER MaximumSize, ULONG PageProtection, ULONG AllocationAttributes, HANDLE FileHandle): memory to map into
// address spaces, shared by name; with a file handle it is the file's contents (written back by FlushViewOfFile, when the last view goes and when the section does).
uint32_t nt_create_section(Process& p, const SyscallRequest& r) {
  uint64_t page_protection = 0, attributes = 0, file_handle = 0;
  if (!argument(p, r, 4, &page_protection) || !argument(p, r, 5, &attributes) || !argument(p, r, 6, &file_handle)) return kAccessViolation;
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  const uint32_t page = uint32_t(page_protection);
  bool copy = false;
  if (protection_of_page(page, &copy) == 0 || page == kPageExecute) return kInvalidParameter;      // (no access and execute-only sections do not exist)
  char name[ObjectManager::kNameBytes + 1];
  uint32_t name_size = 0;
  uint32_t status = read_name(p, r.args[2], name, ObjectManager::kNameBytes, &name_size);
  if (status != kSuccess) return status;
  uint64_t size = 0;
  if (r.args[3] != 0 && !p.read_user_u64(r.args[3], &size)) return kAccessViolation;
  filesystem::IFileSystem* fs = nullptr;
  String host(p.files().allocator());
  if (file_handle != 0 && file_handle != ~uint64_t{0}) {
    uint64_t slot = 0;
    if (!slot_of(file_handle, &slot) || p.files().kind(slot) != DescriptorKind::File) return kInvalidHandle;
    OpenObject* file = p.files().object(slot);
    if (!file || !p.files().host_path_of(file->path.view(), &host)) return kInvalidHandle;
    fs = p.files().file_system();
    if (!file->readable) return 0xC0000022u;      // a file that cannot be read cannot be mapped
    if (!file->writable && (page == kPageReadWrite || page == kPageExecuteReadWrite || page == kPageExecuteRead || page == kPageExecuteWriteCopy)) return 0xC0000022u;      // STATUS_ACCESS_DENIED
    if (size > p.files().size_of(*file) && !file->writable && !(uint32_t(attributes) & 0x1000000u)) return 0xC000004Eu;
  } else if (size == 0) {
    return kInvalidParameter;
  }
  constexpr uint32_t kSecFile = 0x800000, kSecImage = 0x1000000, kSecReserve = 0x4000000, kSecCommit = 0x8000000, kSecNoCache = 0x10000000, kSecWriteCombine = 0x40000000, kSecLargePages = 0x80000000u;
  const uint32_t flags = uint32_t(attributes);
  if ((flags & kSecLargePages) || ((flags & kSecReserve) && (flags & kSecCommit))) return kInvalidParameter;
  if (flags & kSecImage) {      // an image section is a PE file; it takes none of the other flags
    if (flags & (kSecReserve | kSecCommit | kSecFile | kSecWriteCombine)) return kInvalidParameter;
    if (!fs) return 0xC000007Bu;      // STATUS_INVALID_IMAGE_FORMAT: there is no file
  }
  if ((flags & (kSecNoCache | kSecWriteCombine)) && !(flags & (kSecReserve | kSecCommit))) return kInvalidParameter;
  if (flags & kSecFile) return kInvalidParameter;
  uint32_t reported = flags & (kSecNoCache | kSecWriteCombine | kSecImage);
  if (fs) reported |= kSecFile;
  else reported |= (flags & kSecReserve) ? kSecReserve : kSecCommit;
  const ObjectManager::Created c = objects->create_section(StringView(name, name_size), (flags & kSecImage) ? 0 : size, page, reported, fs, host.view());
  if (c.id == 0) return c.status;
  if (flags & kSecImage) {      // the file has to be a PE image this OS can run
    uint8_t* image = nullptr;
    uint64_t image_size = 0;
    SharedMemory* owner = nullptr;
    exe::PeFile pe(p.files().allocator());
    ObjectManager::SectionInfo info;
    if (!objects->section_memory(c.id, &image, &image_size, &owner) || !objects->section_info(c.id, &info)) { objects->release(c.id); return 0xC000007Bu; }
    uint32_t header = image_header_status(ConstByteView(image, info.size));
    if (header == 0) {
      const bool parsed = pe.parse(ConstByteView(image, info.size));
      const bool good_pair = parsed && ((pe.is_pe32_plus() && pe.machine() == exe::kPeMachineAmd64) || (!pe.is_pe32_plus() && pe.machine() == 0x14C));      // PE32+ for x86-64, PE32 for i386
      if (!good_pair) header = 0xC000007Bu;
    }
    if (header != 0) {
      objects->release(c.id);
      return header;
    }
    uint64_t image_rounded = 0;
    if (!AddressSpace::round_up(pe.size_of_image(), &image_rounded) || size > image_rounded) { objects->release(c.id); return 0xC0000040u; }      // STATUS_SECTION_TOO_BIG
    objects->section_set_reported_size(c.id, size != 0 ? size : image_rounded);
  }
  return give_handle(p, *objects, ObjectKind::Section, c.id, r.args[0], c.status, wants_inherit(p, r.args[2]));
}
// The rights an access mask asks for, with the generic ones spelled out (a section: query 1, write 2, read 4, execute 8, extend size 0x10).
uint32_t section_rights(uint32_t access) {
  if (access & 0x02000000u) return 0xF001Fu;      // MAXIMUM_ALLOWED
  if (access & 0x80000000u) access |= 0x20005u;   // GENERIC_READ
  if (access & 0x40000000u) access |= 0x20002u;   // GENERIC_WRITE
  if (access & 0x20000000u) access |= 0x20008u;   // GENERIC_EXECUTE
  if (access & 0x10000000u) access |= 0xF001Fu;   // GENERIC_ALL
  return access;
}
uint32_t nt_open_section(Process& p, const SyscallRequest& r) {
  const uint32_t status = open_object(p, r, ObjectKind::Section);
  uint64_t handle = 0, slot = 0;
  if (status == kSuccess && p.read_user_u64(r.args[0], &handle) && slot_of(handle, &slot)) p.files().set_denied_access(slot, ~section_rights(uint32_t(r.args[1])));
  return status;
}

namespace {
// SECTION_IMAGE_INFORMATION (64 bytes) of an image section: what the PE headers say, as NtQuerySection answers it.
uint32_t query_image_information(Process& p, const SyscallRequest& r, uint32_t id, const ObjectManager::SectionInfo& info, uint64_t length, uint64_t returned_at) {
  if (!(info.attributes & 0x1000000u)) return 0xC000004Fu;      // STATUS_SECTION_NOT_IMAGE
  if (length < 64) return 0xC0000004u;
  ObjectManager* objects = system_objects();
  uint8_t* storage = nullptr;
  uint64_t rounded = 0;
  SharedMemory* owner = nullptr;
  if (!objects->section_memory(id, &storage, &rounded, &owner)) return kInvalidHandle;
  exe::PeFile pe(p.files().allocator());
  if (!pe.parse(ConstByteView(storage, info.size))) return 0xC000007Bu;
  uint8_t out[64];
  describe_pe_image(pe, info.size, out, ConstByteView(storage, info.size));
  if (!p.write_user(r.args[2], ConstByteView(out, 64))) return kAccessViolation;
  if (returned_at != 0 && !p.write_user_u64(returned_at, 64)) return kAccessViolation;
  return kSuccess;
}
}  // namespace

// NtQuerySection(Section, SECTION_INFORMATION_CLASS, PVOID Buffer, SIZE_T Length, PSIZE_T ReturnLength): class 0, SECTION_BASIC_INFORMATION { BaseAddress; Attributes; Size }.
uint32_t nt_query_section(Process& p, const SyscallRequest& r) {
  uint64_t length = 0, returned_at = 0;
  if (!argument(p, r, 3, &length) || !argument(p, r, 4, &returned_at)) return kAccessViolation;
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  const uint32_t id = object_of(p, r.args[0], DescriptorKind::Section);
  if (!id) return kInvalidHandle;
  uint64_t slot = 0;
  if (slot_of(r.args[0], &slot) && (p.files().denied_access(slot) & 1)) return 0xC0000022u;      // SECTION_QUERY
  const uint32_t information_class = uint32_t(r.args[1]);
  if (information_class > 1) return 0xC0000003u;      // STATUS_INVALID_INFO_CLASS
  ObjectManager::SectionInfo info;
  if (!objects->section_info(id, &info)) return kInvalidHandle;
  if (information_class == 1) return query_image_information(p, r, id, info, length, returned_at);      // SectionImageInformation
  if (length < 24) return 0xC0000004u;      // STATUS_INFO_LENGTH_MISMATCH
  uint8_t out[24] = {};
  const uint64_t size = info.reported_size != 0 ? info.reported_size : (info.size + 4095) & ~uint64_t{4095};
  for (uint32_t i = 0; i < 4; ++i) out[8 + i] = uint8_t(info.attributes >> (8 * i));
  for (uint32_t i = 0; i < 8; ++i) out[16 + i] = uint8_t(size >> (8 * i));
  if (!p.write_user(r.args[2], ConstByteView(out, 24))) return kAccessViolation;
  if (returned_at != 0 && !p.write_user_u64(returned_at, 24)) return kAccessViolation;
  return kSuccess;
}
uint32_t nt_create_job_object(Process& p, const SyscallRequest& r) { return create_object(p, r, ObjectKind::Job, false, false, 0, 0); }
uint32_t nt_open_job_object(Process& p, const SyscallRequest& r) { return open_object(p, r, ObjectKind::Job); }

namespace {

// A view of an image section: the PE file laid out as the loader would, at its preferred base if that is free, else somewhere else with its relocations applied (then the answer
// is STATUS_IMAGE_NOT_AT_BASE, a success). Every view has pages of its own, copy-on-write where the sections are writable.
uint32_t map_image_view(Process& p, const SyscallRequest& r, uint32_t section_id, uint64_t size_at, const uint8_t* file_bytes, uint64_t file_size) {
  AddressSpace* space = p.address_space();
  exe::PeFile pe(p.files().allocator());
  if (!pe.parse(ConstByteView(file_bytes, file_size)) || !pe.is_pe32_plus()) return 0xC000007Bu;
  uint64_t base = 0, rounded = 0;
  if (size_at == 0 || !p.read_user_u64(r.args[2], &base)) return kAccessViolation;
  if (!AddressSpace::round_up(pe.size_of_image(), &rounded) || rounded == 0) return 0xC000007Bu;
  uint64_t chosen = base;
  bool moved = false;
  if (base != 0) {
    if (base % 65536 != 0) return kInvalidParameter;
    moved = base != pe.image_base();
  } else {
    chosen = pe.image_base();
    if (chosen == 0 || chosen % 65536 != 0 || chosen >= AddressSpace::kUserTop || !space->find_free(rounded, chosen, &chosen) || chosen != pe.image_base()) {
      uint64_t hint = 0x00007FF500000000ull, at = 0;
      for (uint32_t attempt = 0; attempt < 64; ++attempt) {      // a place that is free and on a 64 KiB boundary
        if (!space->find_free(rounded, hint, &at)) return kNoResources;
        const uint64_t aligned = (at + 0xFFFF) & ~uint64_t{0xFFFF};
        if (aligned == at) break;
        hint = aligned;
      }
      if (at % 65536 != 0) return kNoResources;
      chosen = at;
      moved = true;
    }
  }
  if (moved && (!pe.relocations_ok() || (pe.relocations().count() == 0 && !(pe.dll_characteristics() & 0x40)))) return kStatusConflictingAddresses;      // it cannot be moved
  LoadedImage image;
  const LoadStatus loaded = map_pe_image(pe, ConstByteView(file_bytes, file_size), *space, chosen, false, &image);
  if (loaded != LoadStatus::Ok) {
    space->unmap_allocation(chosen);
    return loaded == LoadStatus::OutOfMemory ? kNoResources : (loaded == LoadStatus::Malformed ? kStatusConflictingAddresses : 0xC000007Bu);
  }
  if (moved && !apply_relocations(*space, pe, chosen, chosen - pe.image_base())) { space->unmap(chosen, rounded); return 0xC000007Bu; }
  describe_image_pages(*space, pe, chosen);
  if (moved && (pe.dll_characteristics() & 0x40)) record_image_base(*space, chosen);
  if (!p.write_user_u64(r.args[2], chosen) || !p.write_user_u64(size_at, rounded)) { space->unmap_allocation(chosen); return kAccessViolation; }
  // The first view of an image that may be moved (dynamic base) is simply a success wherever it lands; any other that is not at the preferred base says so.
  const bool seen_before = system_objects()->section_note_image_view(section_id);
  return moved && (seen_before || !(pe.dll_characteristics() & 0x40)) ? 0x40000003u : kSuccess;      // STATUS_IMAGE_NOT_AT_BASE
}

}  // namespace

// NtMapViewOfSection(Section, Process, PVOID* BaseAddress, ULONG_PTR ZeroBits, SIZE_T CommitSize, PLARGE_INTEGER SectionOffset, PSIZE_T ViewSize, SECTION_INHERIT, ULONG AllocationType,
// ULONG Protect)
uint32_t nt_map_view_of_section(Process& p, const SyscallRequest& r) {
  uint64_t commit = 0, offset_at = 0, size_at = 0, inherit = 0, type = 0, protect = 0;
  if (!argument(p, r, 4, &commit) || !argument(p, r, 5, &offset_at) || !argument(p, r, 6, &size_at) || !argument(p, r, 7, &inherit) || !argument(p, r, 8, &type) || !argument(p, r, 9, &protect))
    return kAccessViolation;
  ObjectManager* objects = system_objects();
  AddressSpace* space = p.address_space();
  if (!objects || !space) return kNotImplemented;
  bool remote = false;
  uint64_t pid = 0;
  if (!names_own_process(p, r.args[1])) {      // a process this one started (or another running program)
    bool exited = false;
    uint32_t code = 0;
    if (!process_handle_info(p, r.args[1], &exited, &code, &pid)) return kInvalidHandle;
    if (exited) return 0xC000010Au;      // STATUS_PROCESS_IS_TERMINATING
    if (!p.runtime()) return kNotImplemented;
    remote = true;
  }
  const uint32_t id = object_of(p, r.args[0], DescriptorKind::Section);
  if (!id) return kInvalidHandle;
  ObjectManager::SectionInfo info;
  uint8_t* storage = nullptr;
  uint64_t rounded = 0;
  SharedMemory* owner = nullptr;
  if (!objects->section_info(id, &info) || !objects->section_memory(id, &storage, &rounded, &owner)) return kInvalidHandle;
  bool copy = false, valid = false;
  const uint32_t view_page = uint32_t(protect) ? uint32_t(protect) : kPageReadWrite;
  const uint8_t prot = protection_of_page(view_page, &copy, &valid);
  if (!valid) return kInvalidParameter;
  {      // what the view may do must be allowed by the section; a view that cannot be touched needs read access to the section
    const uint8_t wanted = page_access(view_page) ? page_access(view_page) : uint8_t{1};
    if (wanted & ~page_access(info.page ? info.page : kPageReadWrite)) return 0xC0000022u;      // STATUS_ACCESS_DENIED: the section was not opened for that
    uint64_t handle_slot = 0;
    const uint32_t denied = slot_of(r.args[0], &handle_slot) ? p.files().denied_access(handle_slot) : 0;
    constexpr uint32_t kSectionMapWrite = 2, kSectionMapRead = 4, kSectionMapExecute = 8;
    if (((wanted & 1) && !(wanted & 2) && (denied & kSectionMapRead)) || ((wanted & 2) && (denied & kSectionMapWrite)) || ((wanted & 4) && (denied & kSectionMapExecute) && (denied & 0x20))) return 0xC0000022u;
  }
  if (info.attributes & 0x1000000u) return remote ? kNotImplemented : map_image_view(p, r, id, size_at, storage, info.size);
  uint64_t offset = 0, view = 0, base = 0;
  if (offset_at != 0 && !p.read_user_u64(offset_at, &offset)) return kAccessViolation;
  if (size_at == 0 || !p.read_user_u64(size_at, &view) || !p.read_user_u64(r.args[2], &base)) return kAccessViolation;
  uint32_t status;
  if (remote) {
    IProcessRuntime::RemoteOp op;
    op.kind = IProcessRuntime::RemoteOp::MapView;
    op.address = base; op.size = view; op.argument = offset; op.thread_id = id; op.protect = view_page;
    status = p.runtime()->remote_operation(pid, op);
    base = op.address; view = op.size;
  } else {
    status = map_view_in(p, id, offset, &base, &view, view_page);
  }
  if (status != kSuccess) return status;
  if (p.write_user_u64(r.args[2], base) && p.write_user_u64(size_at, view)) return kSuccess;
  if (!remote) if (AddressSpace* own = p.address_space()) own->unmap(base, view);
  return kAccessViolation;
}

// A view of the (not image) section `id` in `target`: *base 0 lets the system choose; *view 0 means up to the end of the section. Both come back as they are.
uint32_t map_view_in(Process& target, uint32_t id, uint64_t offset, uint64_t* base_in_out, uint64_t* view_in_out, uint32_t view_page) {
  ObjectManager* objects = system_objects();
  AddressSpace* space = target.address_space();
  if (!objects || !space) return kNotImplemented;
  ObjectManager::SectionInfo info;
  uint8_t* storage = nullptr;
  uint64_t rounded = 0;
  SharedMemory* owner = nullptr;
  if (!objects->section_info(id, &info) || !objects->section_memory(id, &storage, &rounded, &owner)) return kInvalidHandle;
  bool copy = false, valid = false;
  const uint8_t prot = protection_of_page(view_page, &copy, &valid);
  if (!valid) return kInvalidParameter;
  uint64_t base = *base_in_out, view = *view_in_out;
  if (offset % 4096 != 0 || offset >= info.size) return kInvalidParameter;
  if (view == 0 || view > info.size - offset) view = info.size - offset;
  uint64_t bytes = 0;
  if (!AddressSpace::round_up(view, &bytes)) return kInvalidParameter;
  if (base == 0) {
    if (!space->find_free(bytes, 0x00007FF500000000ull, &base)) return kNoResources;
  } else if (base % 4096 != 0) {
    return kInvalidParameter;
  }
  // A copy-on-write view shares the section's memory until a page is written to (AddressSpace::break_copy_on_write gives that page a copy of its own).
  if (!space->map_shared(base, bytes, prot, storage + offset, owner)) return kStatusConflictingAddresses;
  PageAttributes attributes;
  attributes.type = PageAttributes::Mapped;
  attributes.protect = attributes.allocation_protect = view_page;
  attributes.file_view = info.file_backed;
  space->set_attributes(base, bytes, attributes);
  *base_in_out = base;
  *view_in_out = bytes;
  return kSuccess;
}

// The whole view that starts at `address` in `target`'s address space.
uint32_t unmap_view_in(Process& target, uint64_t address) {
  AddressSpace* space = target.address_space();
  if (!space) return kNotImplemented;
  uint64_t base = 0, size = 0;
  PageAttributes attributes;
  if (space->attributes_at(address, &attributes) && attributes.type == PageAttributes::Image) {      // an image goes with all its pieces
    return space->allocation_base(address, &base) && space->unmap_allocation(base) ? kSuccess : kStatusNotMappedView;      // (any address of an image unmaps all of it)
  }
  if (!space->allocation_extent(address, &base, &size) || base != address) return kStatusNotMappedView;
  return space->unmap(base, size) ? kSuccess : kStatusNotMappedView;
}

// NtUnmapViewOfSection(Process, BaseAddress): the whole view that starts at BaseAddress (in this process or in one it started).
uint32_t nt_unmap_view_of_section(Process& p, const SyscallRequest& r) {
  if (names_own_process(p, r.args[0])) return unmap_view_in(p, r.args[1]);
  bool exited = false;
  uint32_t code = 0;
  uint64_t pid = 0;
  if (!process_handle_info(p, r.args[0], &exited, &code, &pid)) return kInvalidHandle;
  if (exited) return 0xC000010Au;      // STATUS_PROCESS_IS_TERMINATING
  if (!p.runtime()) return kNotImplemented;
  IProcessRuntime::RemoteOp op;
  op.kind = IProcessRuntime::RemoteOp::Unmap;
  op.address = r.args[1];
  return p.runtime()->remote_operation(pid, op);
}

// NtAreMappedFilesTheSame(Address1, Address2): both have to lie in a view; the answer is yes for two addresses of one view and NOT_SAME_DEVICE for any other pair.
uint32_t nt_are_mapped_files_the_same(Process& p, const SyscallRequest& r) {
  constexpr uint32_t kInvalidAddress = 0xC0000141u, kConflictingAddresses = 0xC0000018u, kNotSameDevice = 0xC00000D4u;
  AddressSpace* space = p.address_space();
  if (!space) return kNotImplemented;
  if (r.args[0] == 0 || r.args[1] == 0) return kInvalidAddress;
  uint64_t base[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    PageAttributes attributes;
    if (!space->attributes_at(AddressSpace::round_down(r.args[i]), &attributes) || (attributes.type != PageAttributes::Mapped && attributes.type != PageAttributes::Image)) return kConflictingAddresses;
    if (!space->allocation_base(AddressSpace::round_down(r.args[i]), &base[i])) return kConflictingAddresses;
  }
  return base[0] == base[1] ? kSuccess : kNotSameDevice;
}

// NtFlushVirtualMemory(Process, PVOID* BaseAddress, PSIZE_T RegionSize, PIO_STATUS_BLOCK): a view of a file goes back to the file.
uint32_t nt_flush_virtual_memory(Process& p, const SyscallRequest& r) {
  AddressSpace* space = p.address_space();
  if (!space || !names_own_process(p, r.args[0])) return kNotImplemented;
  uint64_t base = 0;
  if (r.args[1] == 0 || !p.read_user_u64(r.args[1], &base)) return kAccessViolation;
  if (SharedMemory* shared = space->shared_at(base)) shared->flush();
  return kSuccess;
}

// NtQueryObject(Handle, Class, Buffer, Length, ReturnLength): basic information (0), the type's name (2) and the handle flags (4).
uint32_t nt_query_object(Process& p, const SyscallRequest& r) {
  uint64_t returned = 0;
  if (!argument(p, r, 4, &returned)) return kAccessViolation;
  uint64_t slot = 0;
  if (!slot_of(r.args[0], &slot) || p.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  const uint32_t info_class = uint32_t(r.args[1]), length = uint32_t(r.args[3]);
  uint8_t out[128] = {};
  auto put32 = [&](uint32_t at, uint32_t v) { for (uint32_t i = 0; i < 4; ++i) out[at + i] = uint8_t(v >> (8 * i)); };
  uint32_t size = 0;
  switch (info_class) {
    case 0:      // OBJECT_BASIC_INFORMATION
      put32(0, (p.files().inheritable(slot) ? 2u : 0u) | (p.files().protected_from_close(slot) ? 1u : 0u));      // Attributes (OBJ_INHERIT, a protected handle)
      put32(4, 0x001F0FFF);      // GrantedAccess
      put32(8, 1);               // HandleCount
      put32(12, 1);              // PointerCount
      size = 56;
      break;
    case 2: {    // OBJECT_TYPE_INFORMATION (the name only: a UNICODE_STRING and its characters)
      const char* type = "File";
      switch (p.files().kind(slot)) {
        case DescriptorKind::Thread: type = "Thread"; break;
        case DescriptorKind::Process: type = "Process"; break;
        case DescriptorKind::Event: type = "Event"; break;
        case DescriptorKind::Mutex: type = "Mutant"; break;
        case DescriptorKind::Semaphore: type = "Semaphore"; break;
        case DescriptorKind::Timer: type = "Timer"; break;
        case DescriptorKind::Section: type = "Section"; break;
        case DescriptorKind::Job: type = "Job"; break;
        case DescriptorKind::ChangeNotification: type = "ChangeNotification"; break;
        case DescriptorKind::Mailslot: type = "Mailslot"; break;
        case DescriptorKind::Key: type = "Key"; break;
        default: break;
      }
      uint32_t n = 0;
      while (type[n]) ++n;
      uint8_t text[32] = {};
      for (uint32_t i = 0; i < n; ++i) text[2 * i] = uint8_t(type[i]);
      const uint32_t bytes = 2 * n;
      out[0] = uint8_t(bytes); out[2] = uint8_t(bytes + 2);
      const uint32_t need = 104 + bytes + 2;      // (the structure is 104 bytes; the characters follow)
      if (returned != 0 && !p.write_user_u32(returned, need)) return kAccessViolation;
      if (length < need) return 0xC0000004u;
      uint8_t full[160] = {};
      for (uint32_t i = 0; i < 16; ++i) full[i] = out[i];
      const uint64_t text_at = r.args[2] + 104;
      for (uint32_t i = 0; i < 8; ++i) full[8 + i] = uint8_t(text_at >> (8 * i));
      for (uint32_t i = 0; i < bytes; ++i) full[104 + i] = text[i];
      return p.write_user(r.args[2], ConstByteView(full, 104 + bytes + 2)) ? kSuccess : kAccessViolation;
    }
    case 4:      // OBJECT_HANDLE_FLAG_INFORMATION { BOOLEAN Inherit, ProtectFromClose }
      out[0] = p.files().inheritable(slot) ? 1 : 0;
      out[1] = p.files().protected_from_close(slot) ? 1 : 0;
      size = 2;
      break;
    default: return 0xC0000003u;
  }
  if (returned != 0 && !p.write_user_u32(returned, size)) return kAccessViolation;
  if (length < size) return 0xC0000004u;
  return p.write_user(r.args[2], ConstByteView(out, size)) ? kSuccess : kAccessViolation;
}

// NtSetInformationObject(Handle, Class = 4, Buffer, Length): the handle flags.
uint32_t nt_set_information_object(Process& p, const SyscallRequest& r) {
  uint64_t slot = 0;
  if (!slot_of(r.args[0], &slot) || p.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  if (uint32_t(r.args[1]) != 4) return 0xC0000003u;
  if (uint32_t(r.args[3]) < 2) return 0xC0000004u;
  uint8_t flags[2];
  if (!p.read_user(r.args[2], ByteView(flags, 2))) return kAccessViolation;
  p.files().set_handle_flags(slot, flags[0] != 0, flags[1] != 0);
  return kSuccess;
}

}  // namespace process::windowsabi
