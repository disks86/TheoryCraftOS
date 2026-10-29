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
#include "Process/WindowsMailslots.hpp"
#include "Process/Objects.hpp"
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsWait.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du,
                   kAccessDenied = 0xC0000022u, kObjectNameInvalid = 0xC0000033u, kObjectNameNotFound = 0xC0000034u, kObjectNameCollision = 0xC0000035u,
                   kBufferTooSmall = 0xC0000023u, kNotImplemented = 0xC0000002u, kInvalidInfoClass = 0xC0000003u, kInfoLengthMismatch = 0xC0000004u,
                   kNoResources = 0xC000009Au, kIoTimeout = 0xC00000B5u;
constexpr uint64_t kPending = kWaitPending;

// The token of a mailslot descriptor: bits 0..31 the object id, bit 32 set for a writer (CreateFile), clear for the reader (CreateMailslot).
constexpr uint64_t kWriterBit = 1ull << 32, kAsyncBit = 1ull << 33;      // (bit 33: the reader was made without FILE_SYNCHRONOUS_IO, so its reads may be pending)
uint64_t make_token(uint32_t id, bool is_writer) { return id | (is_writer ? kWriterBit : 0); }
uint32_t token_id(uint64_t token) { return uint32_t(token); }

// The reads that returned STATUS_PENDING and wait for a message (guarded by the system lock); one per mailslot handle.
struct PendingRead { bool used = false; uint64_t process = 0, slot = 0, status_block = 0, buffer = 0; uint32_t capacity = 0; };
constexpr uint32_t kMaxPending = 16;
PendingRead g_pending[kMaxPending];
PendingRead* find_pending(uint64_t process, uint64_t slot) {
  for (PendingRead& e : g_pending) if (e.used && e.process == process && e.slot == slot) return &e;
  return nullptr;
}
bool token_is_writer(uint64_t token) { return (token & kWriterBit) != 0; }

// The mailslot name in an OBJECT_ATTRIBUTES, when it is one: the part after "\??\mailslot\" or "\Device\Mailslot\" in UTF-8. False if the path is something else.
bool mailslot_name(Process& p, uint64_t attributes, String* name) {
  if (attributes == 0) return false;
  uint64_t name_at = 0;
  if (!p.read_user_u64(attributes + 16, &name_at) || name_at == 0) return false;
  String text(p.files().allocator());
  if (read_unicode_string_argument(p, name_at, &text) != kSuccess) return false;
  static const char* const prefixes[] = {"\\??\\mailslot\\", "\\Device\\Mailslot\\"};
  for (const char* prefix : prefixes) {
    uint64_t n = 0;
    while (prefix[n]) ++n;
    bool same = text.size() > n;      // a mailslot always has a name after the prefix (there is no "folder" handle, unlike pipes)
    for (uint64_t i = 0; same && i < n; ++i) {
      char a = text.view()[i], b = prefix[i];
      if (a >= 'A' && a <= 'Z') a = char(a + 32);
      if (b >= 'A' && b <= 'Z') b = char(b + 32);
      same = a == b;
    }
    if (!same) continue;
    name->assign(text.view().without_prefix(n));
    return true;
  }
  return false;
}

bool wants_inherit(Process& p, uint64_t attributes) {
  uint8_t raw[4];
  return attributes != 0 && p.read_user(attributes + 24, ByteView(raw, 4)) && (raw[0] & 2) != 0;
}

// IO_STATUS_BLOCK { NTSTATUS Status (8 bytes with padding); ULONG_PTR Information; }
bool store_io_status(Process& p, uint64_t address, uint32_t status, uint64_t information) {
  return address == 0 || (p.write_user_u64(address, status) && p.write_user_u64(address + 8, information));
}

uint32_t give_mailslot_handle(Process& p, uint64_t token, bool inherit, uint64_t* handle) {
  const int64_t slot = p.files().open_token(DescriptorKind::Mailslot, token);
  if (slot < 0) return kNoResources;
  p.files().set_handle_flags(uint64_t(slot), inherit, false);
  *handle = (uint64_t(slot) + 1) * 4;
  return kSuccess;
}

uint32_t result_status(ObjectManager::MailslotResult r) {
  switch (r) {
    case ObjectManager::MailslotResult::Ok: return kSuccess;
    case ObjectManager::MailslotResult::TooBig: return kInvalidParameter;
    case ObjectManager::MailslotResult::Full: return kNoResources;
    case ObjectManager::MailslotResult::Gone: return kObjectNameNotFound;
    default: return kSuccess;      // NoMessage / TooSmall: the caller decides (they need *got / *message_size too)
  }
}

}  // namespace

void mailslot_token_add(uint64_t token) { if (ObjectManager* objects = system_objects()) objects->add_ref(token_id(token)); }
void mailslot_token_release(uint64_t token) {
  ObjectManager* objects = system_objects();
  if (!objects) return;
  if (!token_is_writer(token)) { objects->sync().lock(); for (PendingRead& e : g_pending) if (e.used) e.used = false; objects->sync().unlock(); }      // (a pending read of a closed reader is dropped; at most a few exist)
  if (!token_is_writer(token)) objects->mailslot_close_reader(token_id(token));      // destroys it at once, like the real one
  objects->release(token_id(token));
}

// A read timeout as NtCreateMailslotFile gets it (a negative number is relative, in 100 ns; ~0 stands for forever) as it is kept: forever stays ~0, the rest is a plain count of 100 ns.
uint64_t normal_timeout(uint64_t raw) { return raw == ~uint64_t{0} ? raw : (int64_t(raw) < 0 ? uint64_t(-int64_t(raw)) : raw); }

uint32_t nt_create_mailslot_file(Process& p, const SyscallRequest& r) {
  uint64_t options = 0, quota = 0, max_message_size = 0, timeout_at = 0;
  if (!argument(p, r, 4, &options) || !argument(p, r, 5, &quota) || !argument(p, r, 6, &max_message_size) || !argument(p, r, 7, &timeout_at)) return kAccessViolation;
  (void)quota;
  String name(p.files().allocator());
  if (!mailslot_name(p, r.args[2], &name) || name.empty()) return kObjectNameInvalid;
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  uint64_t timeout_100ns = ~uint64_t{0};      // null ReadTimeout: wait forever, like every other timeout pointer in this codebase
  if (timeout_at != 0 && !p.read_user_u64(timeout_at, &timeout_100ns)) return kAccessViolation;
  timeout_100ns = normal_timeout(timeout_100ns);
  const ObjectManager::Created c = objects->create_mailslot(name.view(), uint32_t(max_message_size), timeout_100ns);
  if (c.id == 0) return c.status == 0x40000000u ? kObjectNameCollision : c.status;
  const bool inherit = wants_inherit(p, r.args[2]);
  uint64_t handle = 0;
  const uint32_t status = give_mailslot_handle(p, make_token(c.id, false) | ((options & 0x30) == 0 ? kAsyncBit : 0), inherit, &handle);
  if (status != kSuccess) { objects->release(c.id); return status; }
  if (!p.write_user_u64(r.args[0], handle)) { p.close_descriptor(handle / 4 - 1); return kAccessViolation; }
  return kSuccess;
}

bool mailslot_open_by_name(Process& p, uint64_t object_attributes, uint32_t access, uint32_t* status, uint64_t* handle) {
  (void)access;
  String name(p.files().allocator());
  if (!mailslot_name(p, object_attributes, &name) || name.empty()) return false;
  ObjectManager* objects = system_objects();
  if (!objects) { *status = kNotImplemented; return true; }
  const ObjectManager::Created c = objects->open_mailslot_writer(name.view());
  if (c.id == 0) { *status = c.status; return true; }
  const bool inherit = wants_inherit(p, object_attributes);
  *status = give_mailslot_handle(p, make_token(c.id, true), inherit, handle);
  if (*status != kSuccess) objects->release(c.id);
  return true;
}

uint32_t mailslot_io(Process& p, uint64_t slot, bool write, uint64_t status_block, uint64_t buffer, uint64_t length) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  const uint64_t token = p.files().token(slot);
  const uint32_t id = token_id(token);
  if (write) {
    if (!token_is_writer(token)) return (token & kAsyncBit) ? kInvalidParameter : kAccessDenied;      // the reader does not write (an asynchronous handle, as the library opens it, is told so as a bad parameter)
    length &= 0xFFFFFFFFu;
    constexpr uint64_t kMax = ObjectManager::kMailslotBufferBytes;
    if (length > kMax) return kInvalidParameter;
    uint8_t chunk[kMax];
    if (length != 0 && !p.read_user(buffer, ByteView(chunk, length))) return kAccessViolation;
    const ObjectManager::MailslotResult r = objects->mailslot_write_locked(id, ConstByteView(chunk, length));
    const uint32_t status = result_status(r);
    return store_io_status(p, status_block, status, status == kSuccess ? length : 0) ? status : kAccessViolation;
  }
  if (token_is_writer(token)) return kInvalidParameter;      // a writer does not read
  length &= 0xFFFFFFFFu;
  constexpr uint64_t kMax = ObjectManager::kMailslotBufferBytes;
  uint8_t chunk[kMax];
  const uint32_t capacity = uint32_t(length < kMax ? length : kMax);
  if (token & kAsyncBit) {      // an asynchronous handle: with nothing to read the request stays pending until the owner waits on the handle
    ISystemSync& sync = objects->sync();
    sync.lock();
    PendingRead* slot_entry = find_pending(p.id(), slot);
    const bool empty = objects->mailslot_message_count_locked(id) == 0;
    if (empty) {
      if (!slot_entry) for (PendingRead& e : g_pending) if (!e.used) { slot_entry = &e; break; }
      if (!slot_entry) { sync.unlock(); return kNoResources; }
      *slot_entry = PendingRead{true, p.id(), slot, status_block, buffer, capacity};
    }
    sync.unlock();
    if (empty) return 0x103u;      // STATUS_PENDING
  }
  // The read timeout is fetched once, outside wait_until's own lock (mailslot_info() takes the lock itself, and wait_until's attempt() runs with it already held -
  // calling back into a _locked-free, self-locking accessor from inside that lambda would try to lock twice). It shares wait_until's own ~0-means-forever sentinel.
  uint64_t read_timeout_100ns = ~uint64_t{0};
  { uint32_t cap = 0, next = 0, count = 0; objects->mailslot_info(id, &cap, &read_timeout_100ns, &next, &count); }
  uint32_t got = 0, message_size = 0;
  uint32_t status = wait_until(p, read_timeout_100ns == ~uint64_t{0} ? read_timeout_100ns : read_timeout_100ns * 100, [&]() -> uint64_t {
    const ObjectManager::MailslotResult r = objects->mailslot_read_locked(id, chunk, capacity, &got, &message_size);
    if (r == ObjectManager::MailslotResult::NoMessage) return kPending;
    if (r == ObjectManager::MailslotResult::TooSmall) return kBufferTooSmall;
    return result_status(r);
  });
  if (status == 0x102u) status = kIoTimeout;      // the read timeout elapsed with nothing to read (0x101/kAlerted, if the thread was interrupted, passes through as-is)
  if (status != kSuccess) return status;      // (a read that fails leaves the status block alone)
  if (got != 0 && !p.write_user(buffer, ConstByteView(chunk, got))) return kAccessViolation;
  return store_io_status(p, status_block, kSuccess, got) ? kSuccess : kAccessViolation;
}

bool mailslot_read_ready_locked(Process& p, uint64_t slot) {
  ObjectManager* objects = system_objects();
  const PendingRead* e = find_pending(p.id(), slot);
  return objects && e && objects->mailslot_message_count_locked(token_id(p.files().token(slot))) != 0;
}

uint32_t mailslot_complete_read(Process& p, uint64_t slot) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  ISystemSync& sync = objects->sync();
  sync.lock();
  PendingRead* e = find_pending(p.id(), slot);
  const PendingRead request = e ? *e : PendingRead{};
  if (e) e->used = false;
  sync.unlock();
  if (!request.used) return kSuccess;
  uint8_t chunk[ObjectManager::kMailslotBufferBytes];
  uint32_t got = 0, message_size = 0;
  sync.lock();
  const ObjectManager::MailslotResult r = objects->mailslot_read_locked(token_id(p.files().token(slot)), chunk, request.capacity, &got, &message_size);
  sync.unlock();
  uint32_t status = r == ObjectManager::MailslotResult::TooSmall ? kBufferTooSmall : result_status(r);
  if (r == ObjectManager::MailslotResult::NoMessage) status = kIoTimeout;
  if (status == kSuccess && got != 0 && !p.write_user(request.buffer, ConstByteView(chunk, got))) return kAccessViolation;
  return store_io_status(p, request.status_block, status, status == kSuccess ? got : 0) ? kSuccess : kAccessViolation;
}

uint32_t mailslot_query_information(Process& p, uint64_t slot, uint32_t info_class, uint64_t status_block, uint64_t buffer, uint32_t length) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  const uint32_t id = token_id(p.files().token(slot));
  if (info_class != 26) return kInvalidInfoClass;      // FileMailslotQueryInformation
  if (length < 24) return kInfoLengthMismatch;
  uint32_t max_message_size = 0, next_message_size = 0, message_count = 0;
  uint64_t read_timeout_100ns = 0;
  if (!objects->mailslot_info(id, &max_message_size, &read_timeout_100ns, &next_message_size, &message_count)) return kInvalidHandle;
  uint8_t out[24] = {};
  if (message_count == 0) next_message_size = 0xFFFFFFFFu;      // MAILSLOT_NO_MESSAGE
  for (int i = 0; i < 4; ++i) { out[i] = uint8_t(max_message_size >> (8 * i)); out[4 + i] = 0; out[8 + i] = uint8_t(next_message_size >> (8 * i)); out[12 + i] = uint8_t(message_count >> (8 * i)); }
  for (int i = 0; i < 8; ++i) out[16 + i] = uint8_t(read_timeout_100ns >> (8 * i));
  return p.write_user(buffer, ConstByteView(out, 24)) && store_io_status(p, status_block, kSuccess, 24) ? kSuccess : kAccessViolation;
}

uint32_t mailslot_set_information(Process& p, uint64_t slot, uint32_t info_class, uint64_t buffer, uint32_t length) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  const uint32_t id = token_id(p.files().token(slot));
  if (info_class != 27) return kInvalidInfoClass;      // FileMailslotSetInformation
  if (length < 8) return kInfoLengthMismatch;
  uint8_t in[8];
  if (!p.read_user(buffer, ByteView(in, 8))) return kAccessViolation;
  uint64_t timeout = 0;
  for (int i = 0; i < 8; ++i) timeout |= uint64_t{in[i]} << (8 * i);
  return objects->mailslot_set_timeout(id, normal_timeout(timeout)) ? kSuccess : kInvalidHandle;
}

}  // namespace process::windowsabi
