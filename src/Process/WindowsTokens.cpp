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
#include <initializer_list>
#include "Process/WindowsTokens.hpp"
#include "Process/Objects.hpp"
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsProcess.hpp"
#include "Process/WindowsThreads.hpp"
#include "Windows/tcos_token.h"

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kAccessDenied = 0xC0000022u,
                   kNoResources = 0xC000009Au, kNotImplemented = 0xC0000002u, kNoToken = 0xC000007Cu, kBadTokenType = 0xC00000A8u, kBufferTooSmall = 0xC0000023u,
                   kObjectTypeMismatch = 0xC0000024u, kInvalidInfoClass = 0xC0000003u, kUnsuccessful = 0xC0000001u;
constexpr uint64_t kCurrentProcess = ~uint64_t{0}, kCurrentThread = ~uint64_t{1};
constexpr uint32_t kTokenAllAccess = 0xF01FF, kTokenImpersonate = 4, kTokenDuplicate = 2;
constexpr uint32_t kMaximumAllowed = 0x02000000;
const security::GenericMapping kTokenMapping = {0x20008 | 0x10, 0x20000 | 0xE0, 0x20000, 0xF01FF};

// The tokens of processes and the impersonation tokens of threads: the token object each holds (a reference). Guarded by the system lock.
constexpr uint32_t kMaxTokenOwners = 256;
struct Owner { uint64_t id = 0; uint32_t object = 0; };
Owner g_process_tokens[kMaxTokenOwners];
Owner g_thread_tokens[kMaxTokenOwners];
uint64_t g_next_token_id = 0x20000;

// ---- the default token -------------------------------------------------------------------------------------------------------------------------------

// The S-1-<authority>-<subs...> SID as bytes.
struct SidBuf {
  uint8_t bytes[8 + 4 * 15] = {};
  uint32_t size = 0;
};
SidBuf make_sid(uint8_t authority, std::initializer_list<uint32_t> subs) {
  SidBuf s;
  s.bytes[0] = 1;
  s.bytes[1] = uint8_t(subs.size());
  s.bytes[7] = authority;
  uint32_t at = 8;
  for (uint32_t v : subs) { for (int i = 0; i < 4; ++i) s.bytes[at + i] = uint8_t(v >> (8 * i)); at += 4; }
  s.size = at;
  return s;
}

ConstByteView bytes_of(const TcosTokenBlob& header) {
  // The one place where the blob header is looked at as bytes: a packed plain struct, so its bytes are all there is to it.
  return ConstByteView(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
}

// Builds the token of the system's user: an administrator in a session of its own, elevated.
bool make_default_blob(ByteBuffer& out, uint64_t token_id) {
  const SidBuf user = make_sid(5, {21, 0, 0, 0, 1000});
  const SidBuf group = make_sid(5, {21, 0, 0, 0, 513});
  struct G { SidBuf sid; uint32_t attrs; };
  const G groups[] = {
      {make_sid(1, {0}), 7}, {make_sid(2, {0}), 7}, {make_sid(2, {1}), 7}, {make_sid(5, {32, 544}), 0xF}, {make_sid(5, {32, 545}), 7}, {make_sid(5, {4}), 7}, {make_sid(5, {11}), 7},
      {make_sid(5, {15}), 7}, {make_sid(5, {64, 10}), 7}, {make_sid(5, {5, 0, 0x3E7}), 0xC0000007u}, {make_sid(16, {12288}), 0x60},
  };
  struct P { uint32_t low, attrs; };
  // The privileges a standard user's token holds, all disabled to start with (what the program enables, it asks for).
  const P privileges[] = {{19, 1}, {23, 1}, {25, 1}, {33, 1}, {34, 1}, {35, 1}};
  const SidBuf admins = make_sid(5, {32, 544}), system_sid = make_sid(5, {18});
  constexpr uint32_t kGroupCount = sizeof(groups) / sizeof(groups[0]), kPrivilegeCount = sizeof(privileges) / sizeof(privileges[0]);
  // Layout: header, user, owner (the user is the owner), primary group, groups (the table, then the SIDs), privileges, default DACL.
  ByteBuffer tail(*system_objects()->allocator());
  TcosTokenBlob h = {};
  h.magic = TCOS_TOKEN_MAGIC;
  h.type = 1;
  h.level = 0;
  h.token_id = token_id;
  h.auth_id = 0x1A2B3;
  h.modified_id = token_id;
  h.expiration = 0x7FFFFFFFFFFFFFFFull;
  h.session = 1;
  h.elevation_type = 1;
  h.flags = TCOS_TOKEN_FLAG_VIRT_ALLOWED;
  h.mandatory_policy = 1;
  for (int i = 0; i < 8; ++i) h.source_name[i] = uint8_t("User32  "[i]);
  h.source_id = 0x1000;
  uint32_t at = sizeof(h);
  auto place = [&](const SidBuf& sid) { const uint32_t off = at; tail.append(ConstByteView(sid.bytes, sid.size)); at += sid.size; return off; };
  h.user_off = place(user);
  h.pgroup_off = place(group);
  uint32_t offsets[kGroupCount];
  for (uint32_t i = 0; i < kGroupCount; ++i) offsets[i] = place(groups[i].sid);
  const uint32_t admins_off = place(admins), system_off = place(system_sid);
  h.owner_off = admins_off;      // (the owner of what an administrator makes is the group of administrators)
  h.group_count = kGroupCount;
  h.groups_off = at;
  for (uint32_t i = 0; i < kGroupCount; ++i) { tail.put32(offsets[i]); tail.put32(groups[i].attrs); at += 8; }
  h.privilege_count = kPrivilegeCount;
  h.privileges_off = at;
  for (const P& p : privileges) { tail.put32(p.low); tail.put32(0); tail.put32(p.attrs); at += 12; }
  // the default DACL: GENERIC_ALL for the administrators and for SYSTEM
  h.dacl_off = at;
  const uint32_t ace_size = 8 + admins.size, ace_size2 = 8 + system_sid.size, acl_size = 8 + ace_size + ace_size2;
  tail.put8(2); tail.put8(0); tail.put16(acl_size); tail.put16(2); tail.put16(0);
  tail.put8(0); tail.put8(0); tail.put16(ace_size); tail.put32(0x10000000u); tail.append(ConstByteView(admins.bytes, admins.size));
  tail.put8(0); tail.put8(0); tail.put16(ace_size2); tail.put32(0x10000000u); tail.append(ConstByteView(system_sid.bytes, system_sid.size));
  at += acl_size;
  (void)admins_off; (void)system_off;
  h.size = at;
  out.clear();
  if (!out.append(bytes_of(h)) || !out.append(tail.view())) return false;
  return out.size() == at;
}

// ---- handles and tables --------------------------------------------------------------------------------------------------------------------------------

bool slot_of(uint64_t handle, uint64_t* slot) {
  handle &= ~uint64_t{3};
  if (handle == 0 || handle / 4 > Process::kMaxDescriptors) return false;
  *slot = handle / 4 - 1;
  return true;
}

uint32_t token_object_of(Process& p, uint64_t handle, uint64_t* slot) {
  if (!slot_of(handle, slot) || p.files().kind(*slot) != DescriptorKind::Token) return 0;
  return uint32_t(p.files().token(*slot));
}

// Does the handle hold all the rights? (A handle made with fewer carries the missing ones in its denied mask.)
bool holds(Process& p, uint64_t slot, uint32_t rights) { return (p.files().denied_access(slot) & rights) == 0; }

bool load_blob(uint32_t object, ByteBuffer& out) {
  ObjectManager* objects = system_objects();
  char key[16];
  const uint32_t n = ObjectManager::object_key('t', object, key);
  const uint32_t size = objects->security_size(StringView(key, n));
  if (size == 0) return false;
  out.clear();
  if (!out.put_zeros(size)) return false;
  return objects->security_get(StringView(key, n), out.writable_view()) == size;
}
bool store_blob(uint32_t object, ConstByteView blob) {
  char key[16];
  const uint32_t n = ObjectManager::object_key('t', object, key);
  return system_objects()->security_set(StringView(key, n), blob);
}

// A new token object holding the blob; its id, 0 if there is no room.
uint32_t new_token_object(ConstByteView blob) {
  ObjectManager* objects = system_objects();
  const ObjectManager::Created c = objects->create(ObjectKind::Token, StringView(), false, false, 0, 0, 0);
  if (c.id == 0) return 0;
  if (!store_blob(c.id, blob)) { objects->release(c.id); return 0; }
  return c.id;
}

Owner* find_owner(Owner* table, uint64_t id) {
  for (uint32_t i = 0; i < kMaxTokenOwners; ++i) if (table[i].object != 0 && table[i].id == id) return &table[i];
  return nullptr;
}
// Sets the token object of an owner (taking a reference to it; the previous one is let go). The system lock is not held.
bool set_owner(Owner* table, uint64_t id, uint32_t object) {
  ObjectManager* objects = system_objects();
  uint32_t previous = 0;
  bool ok = true;
  objects->sync().lock();
  Owner* o = find_owner(table, id);
  if (object == 0) {
    if (o) { previous = o->object; *o = Owner(); }
  } else {
    if (!o) for (uint32_t i = 0; i < kMaxTokenOwners && !o; ++i) if (table[i].object == 0) o = &table[i];
    if (!o) ok = false;
    else { previous = o->object; o->id = id; o->object = object; }
  }
  objects->sync().unlock();
  if (ok && object != 0) objects->add_ref(object);
  if (previous) objects->release(previous);
  return ok;
}
uint32_t owner_object(Owner* table, uint64_t id) {
  ObjectManager* objects = system_objects();
  objects->sync().lock();
  Owner* o = find_owner(table, id);
  const uint32_t object = o ? o->object : 0;
  objects->sync().unlock();
  return object;
}

// The token object of the process (made from the default token if it has none).
uint32_t process_token_object(uint64_t pid) {
  if (uint32_t object = owner_object(g_process_tokens, pid)) return object;
  ObjectManager* objects = system_objects();
  ByteBuffer blob(*objects->allocator());
  if (!make_default_blob(blob, __atomic_fetch_add(&g_next_token_id, 1, __ATOMIC_RELAXED))) return 0;
  const uint32_t object = new_token_object(blob.view());
  if (object == 0) return 0;
  if (!set_owner(g_process_tokens, pid, object)) { objects->release(object); return 0; }
  objects->release(object);      // (the table holds it now)
  return owner_object(g_process_tokens, pid);
}

// A handle to the token object in the table of the process; granted: the rights it holds.
uint32_t give_token_handle(Process& p, uint32_t object, uint32_t granted, bool inherit, uint64_t handle_at) {
  ObjectManager* objects = system_objects();
  objects->add_ref(object);
  const int64_t slot = p.files().open_token(DescriptorKind::Token, object);
  if (slot < 0) { objects->release(object); return kNoResources; }
  p.files().set_handle_flags(uint64_t(slot), inherit, false);
  p.files().set_denied_access(uint64_t(slot), ~granted);
  if (!p.write_user_u64(handle_at, (uint64_t(slot) + 1) * 4)) { p.close_descriptor(uint64_t(slot)); return kAccessViolation; }
  return kSuccess;
}

// The rights asked for in a token open or create: mapped, and within what a token has. kMaximumAllowed: all.
uint32_t token_rights(uint32_t desired, bool* valid) {
  uint32_t rights = kTokenMapping.map(desired);
  if (rights & kMaximumAllowed) rights = kTokenAllAccess;
  *valid = (rights & ~kTokenAllAccess) == 0;
  return rights;
}

// May the caller have `rights` to the token object (its own descriptor, if one was set)?
uint32_t check_token_access(Process& p, uint32_t object, uint32_t rights) {
  ObjectManager* objects = system_objects();
  char key[16];
  const uint32_t n = ObjectManager::object_key('s', object, key);
  const uint32_t size = objects->security_size(StringView(key, n));
  if (size == 0) return kSuccess;
  ByteBuffer sd(*objects->allocator());
  if (!sd.put_zeros(size) || objects->security_get(StringView(key, n), sd.writable_view()) != size) return kNoResources;
  uint32_t granted = 0, status = 0;
  return caller_may(p, sd.view(), rights, kTokenMapping, &granted, &status) ? kSuccess : status;
}

// The id of the process or thread a handle names.
bool process_of(Process& p, uint64_t handle, uint64_t* pid) {
  if (handle == kCurrentProcess) { *pid = p.id(); return true; }
  bool exited = false;
  uint32_t code = 0;
  return process_handle_info(p, handle, &exited, &code, pid);
}
bool thread_of(Process& p, uint64_t handle, uint64_t* tid) {
  if (handle == kCurrentThread || handle == 0) { *tid = p.runtime() ? p.runtime()->current_thread_id() : p.id(); return true; }
  if (remote_thread_handle(p, handle, tid)) return true;
  uint64_t slot = 0;
  if (!slot_of(handle, &slot) || p.files().kind(slot) != DescriptorKind::Thread) return false;
  *tid = p.files().token(slot);
  return true;
}

}  // namespace

bool process_token_blob(uint64_t pid, ByteBuffer& out) {
  const uint32_t object = process_token_object(pid);
  return object != 0 && load_blob(object, out);
}

bool caller_token_blob(Process& p, ByteBuffer& out) {
  const uint64_t tid = p.runtime() ? p.runtime()->current_thread_id() : p.id();
  if (const uint32_t object = owner_object(g_thread_tokens, tid)) if (load_blob(object, out)) return true;
  return process_token_blob(p.id(), out);
}

void token_inherit(uint64_t parent_pid, uint64_t child_pid) {
  const uint32_t parent = owner_object(g_process_tokens, parent_pid);
  if (parent == 0) return;
  ObjectManager* objects = system_objects();
  ByteBuffer blob(*objects->allocator());
  if (!load_blob(parent, blob)) return;
  const uint32_t copy = new_token_object(blob.view());
  if (copy == 0) return;
  if (set_owner(g_process_tokens, child_pid, copy)) { objects->release(copy); return; }
  objects->release(copy);
}

void token_process_exited(uint64_t pid) {
  set_owner(g_process_tokens, pid, 0);
  ObjectManager* objects = system_objects();
  if (!objects) return;
  for (uint32_t i = 0; i < kMaxTokenOwners; ++i) {      // the impersonation tokens of its threads (a thread id holds the process id in its low 20 bits)
    objects->sync().lock();
    const uint64_t id = g_thread_tokens[i].id;
    const bool mine = g_thread_tokens[i].object != 0 && (id & 0xFFFFF) == (pid & 0xFFFFF);
    objects->sync().unlock();
    if (mine) set_owner(g_thread_tokens, id, 0);
  }
}

uint32_t open_with_descriptor(Process& p, char tag, uint64_t id, uint32_t desired, const security::GenericMapping& mapping, uint32_t* granted) {
  ObjectManager* objects = system_objects();
  uint32_t status = 0;
  *granted = mapping.map(desired & ~kMaximumAllowed);
  if (desired & kMaximumAllowed) *granted = mapping.all;
  if (!objects || !objects->allocator()) return kSuccess;
  char key[16];
  const uint32_t n = ObjectManager::object_key(tag, uint32_t(id), key);
  const uint32_t size = objects->security_size(StringView(key, n));
  if (size == 0) return kSuccess;
  ByteBuffer sd(*objects->allocator());
  if (!sd.put_zeros(size) || objects->security_get(StringView(key, n), sd.writable_view()) != size) return kAccessDenied;
  if (!caller_may(p, sd.view(), desired, mapping, granted, &status)) return status ? status : kAccessDenied;
  return kSuccess;
}

bool caller_may(Process& p, ConstByteView descriptor, uint32_t desired, const security::GenericMapping& mapping, uint32_t* granted, uint32_t* status, bool backup_intent) {
  *granted = mapping.map(desired & ~kMaximumAllowed);
  *status = kSuccess;
  if (descriptor.empty()) return true;
  ObjectManager* objects = system_objects();
  ByteBuffer blob(*objects->allocator());
  if (!caller_token_blob(p, blob)) return true;      // (no token can be had: nothing to hold against the caller)
  security::TokenView token;
  if (!token.parse(blob.view())) { *status = kAccessDenied; return false; }
  const security::CheckResult r = security::access_check(descriptor, token, desired, mapping, backup_intent ? security::kBackupIntent : 0);
  *status = r.status;
  *granted = r.granted;
  return r.status == kSuccess;
}

uint32_t nt_tcos_token(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  if (!objects || !objects->allocator()) return kNotImplemented;
  uint64_t a[6] = {r.args[0], r.args[1], r.args[2], r.args[3], 0, 0};
  for (uint32_t i = 4; i < 6; ++i) if (!argument(p, r, i, &a[i])) return kAccessViolation;
  const uint32_t op = uint32_t(a[0]);
  uint64_t slot = 0;
  switch (op) {
    case TCOS_TOKEN_CREATE: {      // (op, blob, length, access, inherit, &handle)
      const uint64_t length = a[2];
      if (length < sizeof(TcosTokenBlob) || length > TCOS_TOKEN_MAX_BYTES) return kInvalidParameter;
      ByteBuffer blob(*objects->allocator());
      if (!blob.put_zeros(length) || !p.read_user(a[1], blob.writable_view())) return kAccessViolation;
      security::TokenView view;
      if (!view.parse(blob.view())) return kInvalidParameter;
      bool valid = false;
      const uint32_t rights = token_rights(uint32_t(a[3]), &valid);
      if (!valid) return kAccessDenied;
      const uint32_t object = new_token_object(blob.view());
      if (object == 0) return kNoResources;
      const uint32_t status = give_token_handle(p, object, rights, (a[4] & 1) != 0, a[5]);
      objects->release(object);
      return status;
    }
    case TCOS_TOKEN_READ: {      // (op, handle, required, buffer, capacity, &needed)
      const uint32_t object = token_object_of(p, a[1], &slot);
      if (object == 0) return kInvalidHandle;
      if (!holds(p, slot, uint32_t(a[2]))) return kAccessDenied;
      ByteBuffer blob(*objects->allocator());
      if (!load_blob(object, blob)) return kInvalidHandle;
      if (a[5] != 0 && !p.write_user_u32(a[5], uint32_t(blob.size()))) return kAccessViolation;
      if (a[4] < blob.size()) return kBufferTooSmall;
      return p.write_user(a[3], blob.view()) ? kSuccess : kAccessViolation;
    }
    case TCOS_TOKEN_WRITE: {      // (op, handle, required, blob, length)
      const uint32_t object = token_object_of(p, a[1], &slot);
      if (object == 0) return kInvalidHandle;
      if (!holds(p, slot, uint32_t(a[2]))) return kAccessDenied;
      const uint64_t length = a[4];
      if (length < sizeof(TcosTokenBlob) || length > TCOS_TOKEN_MAX_BYTES) return kInvalidParameter;
      ByteBuffer blob(*objects->allocator());
      if (!blob.put_zeros(length) || !p.read_user(a[3], blob.writable_view())) return kAccessViolation;
      security::TokenView view;
      if (!view.parse(blob.view())) return kInvalidParameter;
      return store_blob(object, blob.view()) ? kSuccess : kNoResources;
    }
    case TCOS_TOKEN_OPEN_PROCESS: {      // (op, process, access, &handle, inherit)
      uint64_t pid = 0;
      if (!process_of(p, a[1], &pid)) return kInvalidHandle;
      bool valid = false;
      const uint32_t rights = token_rights(uint32_t(a[2]), &valid);
      if (!valid) return kAccessDenied;
      const uint32_t object = process_token_object(pid);
      if (object == 0) return kNoResources;
      if (const uint32_t status = check_token_access(p, object, rights)) return status;
      return give_token_handle(p, object, rights, (a[4] & 1) != 0, a[3]);
    }
    case TCOS_TOKEN_OPEN_THREAD: {      // (op, thread, access, &handle, inherit)
      uint64_t tid = 0;
      if (!thread_of(p, a[1], &tid)) return kInvalidHandle;
      bool valid = false;
      const uint32_t rights = token_rights(uint32_t(a[2]), &valid);
      if (!valid) return kAccessDenied;
      const uint32_t object = owner_object(g_thread_tokens, tid);
      if (object == 0) return kNoToken;
      {      // the token of an anonymous impersonation cannot be opened
        ByteBuffer blob(*objects->allocator());
        security::TokenView view;
        if (load_blob(object, blob) && view.parse(blob.view()) && view.is_impersonation() && view.level() == 0) return 0xC00000A6u;      // STATUS_CANT_OPEN_ANONYMOUS
      }
      if (const uint32_t status = check_token_access(p, object, rights)) return status;
      return give_token_handle(p, object, rights, (a[4] & 1) != 0, a[3]);
    }
    case TCOS_TOKEN_SET_THREAD: {      // (op, thread, token handle or 0)
      uint64_t tid = 0;
      if (!thread_of(p, a[1], &tid)) return kInvalidHandle;
      uint32_t object = 0;
      if (a[2] != 0) {
        object = token_object_of(p, a[2], &slot);
        if (object == 0) return kInvalidHandle;
        if (!holds(p, slot, kTokenImpersonate)) return kAccessDenied;
        ByteBuffer blob(*objects->allocator());
        security::TokenView view;
        if (!load_blob(object, blob) || !view.parse(blob.view())) return kInvalidHandle;
        if (!view.is_impersonation()) return kBadTokenType;
      }
      return set_owner(g_thread_tokens, tid, object) ? kSuccess : kNoResources;
    }
    case TCOS_TOKEN_SET_PROCESS: {      // (op, process, token handle)
      uint64_t pid = 0;
      if (!process_of(p, a[1], &pid)) return kInvalidHandle;
      const uint32_t object = token_object_of(p, a[2], &slot);
      if (object == 0) return kInvalidHandle;
      if (!holds(p, slot, 1 /* TOKEN_ASSIGN_PRIMARY */)) return kAccessDenied;
      ByteBuffer blob(*objects->allocator());
      security::TokenView view;
      if (!load_blob(object, blob) || !view.parse(blob.view())) return kInvalidHandle;
      if (view.is_impersonation()) return kBadTokenType;
      return set_owner(g_process_tokens, pid, object) ? kSuccess : kNoResources;
    }
    case TCOS_TOKEN_CALLER: {      // (op, &handle)
      ByteBuffer blob(*objects->allocator());
      const uint64_t tid = p.runtime() ? p.runtime()->current_thread_id() : p.id();
      uint32_t object = owner_object(g_thread_tokens, tid);
      if (object == 0) object = process_token_object(p.id());
      if (object == 0) return kNoResources;
      return give_token_handle(p, object, kTokenAllAccess, false, a[1]);
    }
    case TCOS_FILE_PARENT_SD: {      // (op, handle, buffer, capacity, &needed)
      if (!slot_of(a[1], &slot) || (p.files().kind(slot) != DescriptorKind::File && p.files().kind(slot) != DescriptorKind::Directory)) return kInvalidHandle;
      OpenObject* o = p.files().object(slot);
      if (!o) return kInvalidHandle;
      const StringView full = o->path.view();
      const uint64_t cut = full.rfind('/');
      const StringView parent = cut == StringView::npos || cut == 0 ? StringView("/") : full.first(cut);
      const uint32_t size = objects->security_size(parent);
      if (a[4] != 0 && !p.write_user_u32(a[4], size)) return kAccessViolation;
      if (size == 0) return kSuccess;
      if (a[3] < size) return kBufferTooSmall;
      ByteBuffer sd(*objects->allocator());
      if (!sd.put_zeros(size) || objects->security_get(parent, sd.writable_view()) != size) return kNoResources;
      return p.write_user(a[2], sd.view()) ? kSuccess : kAccessViolation;
    }
    case TCOS_OBJECT_SD_GET: case TCOS_OBJECT_SD_SET: {      // (op, handle, required, buffer, length-or-capacity, &needed)
      char key[300];
      uint32_t key_size = 0;
      DescriptorKind kind = DescriptorKind::Closed;
      uint64_t token = 0;
      uint32_t granted_mask = 0xFFFFFFFFu;
      if (a[1] == kCurrentProcess || a[1] == kCurrentThread) {      // the pseudo handles of the process and the thread that ask
        kind = a[1] == kCurrentProcess ? DescriptorKind::Process : DescriptorKind::Thread;
        token = a[1] == kCurrentProcess ? p.id() : (p.runtime() ? p.runtime()->current_thread_id() : p.id());
        char k[16];
        const uint32_t n = ObjectManager::object_key(a[1] == kCurrentProcess ? 'p' : 'h', uint32_t(token), k);
        for (uint32_t i = 0; i < n; ++i) key[i] = k[i];
        key_size = n;
        slot = ProcessFiles::kMaxDescriptors;      // (no table entry)
      } else {
      if (!slot_of(a[1], &slot)) return kInvalidHandle;
      kind = p.files().kind(slot);
      if (kind == DescriptorKind::Closed) return kInvalidHandle;
      token = p.files().token(slot);
      granted_mask = ~p.files().denied_access(slot);
      switch (kind) {
        case DescriptorKind::File: case DescriptorKind::Directory: {
          OpenObject* o = p.files().object(slot);
          if (!o || o->path.size() >= 260) return kInvalidHandle;
          for (uint64_t i = 0; i < o->path.size(); ++i) key[i] = o->path.view()[i];
          key_size = uint32_t(o->path.size());
          granted_mask = o->granted_access;
          break;
        }
        case DescriptorKind::Process: {
          bool exited = false;
          uint32_t code = 0;
          uint64_t pid = 0;
          if (!objects->query_process(uint32_t(token), &exited, &code, &pid)) return kInvalidHandle;
          char k[16];
          const uint32_t n = ObjectManager::object_key(pid >= (uint64_t{1} << 20) ? 'h' : 'p', uint32_t(pid), k);
          for (uint32_t i = 0; i < n; ++i) key[i] = k[i];
          key_size = n;
          break;
        }
        case DescriptorKind::Thread: {
          char k[16];
          const uint32_t n = ObjectManager::object_key('h', uint32_t(token), k);
          for (uint32_t i = 0; i < n; ++i) key[i] = k[i];
          key_size = n;
          break;
        }
        case DescriptorKind::Key: case DescriptorKind::Pipe: {
          char k[16];
          const uint32_t n = ObjectManager::object_key(kind == DescriptorKind::Key ? 'k' : 'n', uint32_t(token), k);
          for (uint32_t i = 0; i < n; ++i) key[i] = k[i];
          key_size = n;
          break;
        }
        case DescriptorKind::Event: case DescriptorKind::Mutex: case DescriptorKind::Semaphore: case DescriptorKind::Timer: case DescriptorKind::Section: case DescriptorKind::Job:
        case DescriptorKind::DebugObject: case DescriptorKind::ChangeNotification: case DescriptorKind::Token: {
          char k[16];
          const uint32_t n = ObjectManager::object_key('s', uint32_t(token), k);
          for (uint32_t i = 0; i < n; ++i) key[i] = k[i];
          key_size = n;
          break;
        }
        default: return kObjectTypeMismatch;
      }
      }
      const uint32_t required = uint32_t(a[2]);
      if (required & ~granted_mask) return kAccessDenied;
      if (op == TCOS_OBJECT_SD_GET) {
        const uint32_t size = objects->security_size(StringView(key, key_size));
        if (a[5] != 0 && !p.write_user_u32(a[5], size)) return kAccessViolation;
        if (size == 0) return kSuccess;
        if (a[4] < size) return kBufferTooSmall;
        ByteBuffer sd(*objects->allocator());
        if (!sd.put_zeros(size) || objects->security_get(StringView(key, key_size), sd.writable_view()) != size) return kNoResources;
        return p.write_user(a[3], sd.view()) ? kSuccess : kAccessViolation;
      }
      const uint64_t length = a[4];
      if (length > ObjectManager::kMaxSecurityBlob) return kInvalidParameter;
      ByteBuffer sd(*objects->allocator());
      if (length != 0 && (!sd.put_zeros(length) || !p.read_user(a[3], sd.writable_view()))) return kAccessViolation;
      if (length != 0 && !security::valid_descriptor(sd.view())) return 0xC0000079u;      // STATUS_INVALID_SECURITY_DESCR
      return objects->security_set(StringView(key, key_size), sd.view()) ? kSuccess : kNoResources;
    }
    default: return kInvalidParameter;
  }
}

// NtTcosAccessCheck(PVOID arguments). The arguments, 8 numbers of 8 bytes: descriptor, descriptor length, token handle (0: the caller's), desired access,
// the generic mapping (a pointer to four 32 bit numbers), pointer to the granted access (32 bits), pointer to the status (32 bits), pointer to a 64 bit set of privileges used.
uint32_t nt_tcos_access_check(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  if (!objects || !objects->allocator()) return kNotImplemented;
  uint64_t a[8];
  for (uint32_t i = 0; i < 8; ++i) if (!p.read_user_u64(r.args[0] + 8 * i, &a[i])) return kAccessViolation;
  if (a[1] == 0 || a[1] > ObjectManager::kMaxSecurityBlob) return kInvalidParameter;
  ByteBuffer sd(*objects->allocator()), blob(*objects->allocator());
  if (!sd.put_zeros(a[1]) || !p.read_user(a[0], sd.writable_view())) return kAccessViolation;
  uint8_t m[16];
  if (!p.read_user(a[4], ByteView(m, 16))) return kAccessViolation;
  auto le = [&](uint32_t at) { return uint32_t(m[at]) | uint32_t(m[at + 1]) << 8 | uint32_t(m[at + 2]) << 16 | uint32_t(m[at + 3]) << 24; };
  const security::GenericMapping mapping = {le(0), le(4), le(8), le(12)};
  if (a[2] == 0) { if (!caller_token_blob(p, blob)) return kNoToken; }
  else {
    uint64_t slot = 0;
    const uint32_t object = token_object_of(p, a[2], &slot);
    if (object == 0) return kInvalidHandle;
    if (!holds(p, slot, 8 /* TOKEN_QUERY */)) return kAccessDenied;
    if (!load_blob(object, blob)) return kInvalidHandle;
  }
  security::TokenView token;
  if (!token.parse(blob.view())) return kInvalidParameter;
  const security::CheckResult result = security::access_check(sd.view(), token, uint32_t(a[3]), mapping);
  if (result.status == security::kStatusInvalidSecurityDescriptor || result.status == security::kStatusBadImpersonationLevel) return result.status;
  if (!p.write_user_u32(a[5], result.granted) || !p.write_user_u32(a[6], result.status) || (a[7] != 0 && !p.write_user_u64(a[7], result.privileges_used))) return kAccessViolation;
  return kSuccess;
}

}  // namespace process::windowsabi
