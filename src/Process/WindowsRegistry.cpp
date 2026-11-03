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
#include "Process/WindowsRegistry.hpp"
#include "Process/Objects.hpp"
#include "Process/RegistryTree.hpp"
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsIo.hpp"
#include "Process/WindowsSyscalls.hpp"
#include "Process/WindowsTokens.hpp"
#include "Process/WindowsWait.hpp"
#include "Utility/Utf16.hpp"
#include <new>

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kBufferTooSmall = 0xC0000023u,
                   kBufferOverflow = 0x80000005u, kNoMemory = 0xC0000017u, kNoResources = 0xC000009Au, kObjectTypeMismatch = 0xC0000024u, kNoMoreEntries = 0x8000001Au;
constexpr uint64_t kUnixToNt = 11644473600ull * 10000000ull;

// The tree lives in static storage and is never destroyed (a global with a destructor would need atexit, which the kernel does not have).
alignas(RegistryTree) uint8_t g_storage[sizeof(RegistryTree)];
RegistryTree* g_tree = nullptr;

// The registry is shared by all the programs: one lock (the object manager's) covers it.
struct Guard {
  ISystemSync* sync;
  Guard() : sync(system_objects() ? &system_objects()->sync() : nullptr) { if (sync) sync->lock(); }
  ~Guard() { if (sync) sync->unlock(); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};

uint64_t now_filetime(Process& p) { return p.host().realtime_ns() / 100 + kUnixToNt; }

// With the lock held.
RegistryTree* tree(Process& p) {
  if (!g_tree) {
    allocators::IAllocator& allocator = p.files().allocator();
    g_tree = new (static_cast<void*>(g_storage)) RegistryTree(allocator, now_filetime(p));
  }
  return g_tree;
}

// A change to the registry reaches the watches of NtNotifyChangeKey (Objects.hpp: create_key_watch) under a name of the object manager's own: "\\REGISTRY\\MACHINE\\X" is
// "/:registry/MACHINE/X". Called with the registry lock released (the watches take the same lock).
constexpr uint32_t kNotifyName = 1, kNotifyAttributes = 2, kNotifyLastSet = 4, kNotifySecurity = 8;
void registry_watch_path(StringView full_name, bool value_leaf, String* out) {
  out->assign("/:registry");
  const uint64_t first = full_name.find('\\', 1);
  if (first != StringView::npos) {
    for (uint64_t i = first; i < full_name.size(); ++i) out->append(full_name[i] == '\\' ? StringView("/") : full_name.substr(i, 1));
  }
  if (value_leaf) out->append("/\x01");
}
void notify_key_changed(Process& p, uint32_t id, bool value_leaf, uint32_t filter) {
  ObjectManager* objects = system_objects();
  if (!objects) return;
  String full(p.files().allocator()), path(p.files().allocator());
  {
    Guard guard;
    RegistryTree* t = tree(p);
    if (!t || !t->full_name(id, &full)) return;
  }
  registry_watch_path(full.view(), value_leaf, &path);
  objects->notify_entry_changed(path.view(), value_leaf ? 3 : 1, filter);
}

bool key_of(Process& p, uint64_t handle, uint32_t* id) {
  if (handle == 0 || (handle & 3) != 0) return false;
  const uint64_t slot = handle / 4 - 1;
  if (slot >= ProcessFiles::kMaxDescriptors || p.files().kind(slot) != DescriptorKind::Key) return false;
  *id = static_cast<uint32_t>(p.files().token(slot));
  return true;
}

// OBJECT_ATTRIBUTES: Length (4), RootDirectory (8, at 8), ObjectName (8, at 16), Attributes (4, at 24).
uint32_t read_attributes(Process& p, uint64_t attributes, uint32_t* base, String* path, bool* inherit) {
  *base = RegistryTree::kRoot;
  *inherit = false;
  if (attributes == 0) return kAccessViolation;
  uint8_t raw[32];
  if (!p.read_user(attributes, ByteView(raw, 32))) return kAccessViolation;
  if (raw[0] != 48 || raw[1] != 0 || raw[2] != 0 || raw[3] != 0) return kInvalidParameter;      // Length is sizeof(OBJECT_ATTRIBUTES)
  uint64_t root = 0, name_at = 0;
  for (int i = 0; i < 8; ++i) { root |= uint64_t{raw[8 + i]} << (8 * i); name_at |= uint64_t{raw[16 + i]} << (8 * i); }
  *inherit = (raw[24] & 2) != 0;
  if (root != 0 && !key_of(p, root, base)) return kInvalidHandle;
  if (name_at == 0) return kSuccess;
  const uint32_t status = read_unicode_string_argument(p, name_at, path);
  if (status != kSuccess) return status;
  if (root == 0 && !path->view().starts_with('\\') && !path->empty()) return kRegPathSyntax;      // without a parent key the name has to be a full path
  if (root != 0 && path->view().starts_with('\\')) return kRegPathSyntax;
  return kSuccess;
}

uint32_t give_handle(Process& p, uint32_t id, bool inherit, uint64_t* handle) {
  const int64_t slot = p.files().open_token(DescriptorKind::Key, id);
  if (slot < 0) return kNoResources;
  p.files().set_handle_flags(uint64_t(slot), inherit, false);
  *handle = (uint64_t(slot) + 1) * 4;
  return kSuccess;
}

uint32_t open_or_create(Process& p, const SyscallRequest& r, bool create, uint32_t* disposition) {
  if (r.args[0] == 0) return kAccessViolation;
  if (r.args[2] == 0) return kAccessViolation;
  if ((r.args[1] & 0xFFFFFFFFu) == 0) { p.write_user_u64(r.args[0], 0); return 0xC0000022u; }      // no access asked for: STATUS_ACCESS_DENIED
  uint32_t base = 0;
  bool inherit = false;
  String path(p.files().allocator());
  uint32_t status = read_attributes(p, r.args[2], &base, &path, &inherit);
  if (status != kSuccess) return status;
  Guard guard;
  RegistryTree* t = tree(p);
  if (!t) return kNoMemory;
  uint32_t id = 0;
  bool created = false;
  status = t->resolve(base, path.view(), create, now_filetime(p), &id, &created);
  if (status != kSuccess) return status;
  uint64_t handle = 0;
  status = give_handle(p, id, inherit, &handle);
  if (status != kSuccess) return status;
  t->add_handle(id);
  if (!p.write_user_u64(r.args[0], handle)) { p.close_descriptor(handle / 4 - 1); return kAccessViolation; }
  if (disposition) *disposition = created ? 1u : 2u;      // REG_CREATED_NEW_KEY / REG_OPENED_EXISTING_KEY
  return kSuccess;
}

void put32(ByteBuffer& b, uint32_t v) { b.put32(v); }
void put64(ByteBuffer& b, uint64_t v) { b.put32(static_cast<uint32_t>(v)); b.put32(static_cast<uint32_t>(v >> 32)); }

// Copies a finished record to the caller: nothing and STATUS_BUFFER_TOO_SMALL if even the fixed part does not fit, the part that fits and STATUS_BUFFER_OVERFLOW if the
// variable part is cut, else all of it. *result is always the size needed.
uint32_t deliver(Process& p, uint64_t buffer, uint32_t length, uint64_t result, const ByteBuffer& record, uint32_t fixed) {
  const uint64_t size = record.size();
  if (result != 0 && !p.write_user_u32(result, static_cast<uint32_t>(size))) return kAccessViolation;
  if (length < fixed) return kBufferTooSmall;
  const uint64_t n = size < length ? size : length;
  if (n != 0 && !p.write_user(buffer, record.view().first(n))) return kAccessViolation;
  return size > length ? kBufferOverflow : kSuccess;
}

bool utf16_of(ByteBuffer& out, StringView text) { return util::append_utf16le(&out, text); }

}  // namespace

void key_token_add(uint64_t token) {
  Guard guard;
  if (g_tree) g_tree->add_handle(static_cast<uint32_t>(token));
}
void key_token_release(uint64_t token) {
  Guard guard;
  if (g_tree) g_tree->release_handle(static_cast<uint32_t>(token));
}

uint32_t nt_create_key(Process& p, const SyscallRequest& r) {
  if ((r.args[1] & 0x01000000u) && !caller_privilege_enabled(p, 8 /* SeSecurityPrivilege */)) return 0xC0000061u;
  // NtCreateKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG TitleIndex, PUNICODE_STRING Class, ULONG CreateOptions, PULONG Disposition)
  uint64_t disposition_at = 0;
  if (!argument(p, r, 6, &disposition_at)) return kAccessViolation;
  uint32_t disposition = 0;
  const uint32_t status = open_or_create(p, r, true, &disposition);
  if (status == kSuccess && disposition_at != 0 && !p.write_user_u32(disposition_at, disposition)) return kAccessViolation;
  uint64_t handle = 0;
  if (status == kSuccess && disposition == 1 && p.read_user_u64(r.args[0], &handle) && handle != 0) { uint32_t made = 0; if (key_of(p, handle, &made)) notify_key_changed(p, made, false, kNotifyName); }
  if (status == kSuccess && p.read_user_u64(r.args[0], &handle) && handle != 0) {      // the handle holds the rights asked for
    static const security::GenericMapping kKeyMapping = {0x20019, 0x20006, 0x20019, 0xF003F};
    uint32_t want = uint32_t(r.args[1]);
    uint32_t rights = (want & 0x02000000u) ? kKeyMapping.all : kKeyMapping.map(want);
    p.files().set_denied_access(handle / 4 - 1, ~rights);
  }
  return status;
}
// A key that was opened is held to its descriptor (if it has one): the rights asked for must be allowed to the calling thread; if not, the handle is closed again.
uint32_t open_key_checked(Process& p, const SyscallRequest& r) {
  if (const uint32_t gate = impersonation_gate(p)) return gate;
  if ((r.args[1] & 0x01000000u) && !caller_privilege_enabled(p, 8 /* SeSecurityPrivilege */)) return 0xC0000061u;      // STATUS_PRIVILEGE_NOT_HELD
  uint32_t status = open_or_create(p, r, false, nullptr);
  uint64_t handle = 0;
  if (status != kSuccess || !p.read_user_u64(r.args[0], &handle) || handle == 0) return status;
  const uint64_t slot = handle / 4 - 1;
  uint32_t granted = 0;
  static const security::GenericMapping kKeyMapping = {0x20019, 0x20006, 0x20019, 0xF003F};
  if (const uint32_t refused = open_with_descriptor(p, 'k', p.files().token(slot), uint32_t(r.args[1]), kKeyMapping, &granted)) {
    p.close_descriptor(slot);
    p.write_user_u64(r.args[0], 0);
    return refused;
  }
  p.files().set_denied_access(slot, ~granted);
  return kSuccess;
}
uint32_t nt_open_key(Process& p, const SyscallRequest& r) { return open_key_checked(p, r); }
uint32_t nt_open_key_ex(Process& p, const SyscallRequest& r) { return open_key_checked(p, r); }

bool key_full_name(Process& p, uint64_t token, String* out) {
  Guard guard;
  RegistryTree* t = tree(p);
  return t && t->full_name(static_cast<uint32_t>(token), out);
}

uint32_t nt_query_key(Process& p, const SyscallRequest& r) {
  // NtQueryKey(HANDLE, KEY_INFORMATION_CLASS, PVOID, ULONG Length, PULONG ResultLength)
  uint64_t result = 0;
  if (!argument(p, r, 4, &result)) return kAccessViolation;
  uint32_t id = 0;
  if (!key_of(p, r.args[0], &id)) return kInvalidHandle;
  const uint32_t info_class = static_cast<uint32_t>(r.args[1]), length = static_cast<uint32_t>(r.args[3]);
  Guard guard;
  RegistryTree* t = tree(p);
  if (!t) return kNoMemory;
  const RegKey* k = t->key(id);
  if (!k) return kInvalidHandle;
  if (k->deleted) return kRegKeyDeleted;
  const RegistryTree::Statistics s = t->statistics(id);
  const uint32_t name_bytes = utf16_units(k->name.view()) * 2;
  ByteBuffer record(p.files().allocator());
  uint32_t fixed = 0;
  switch (info_class) {
    case 0:      // KeyBasicInformation
      put64(record, k->write_time); put32(record, 0); put32(record, name_bytes);
      fixed = 16;
      if (!utf16_of(record, k->name.view())) return kNoMemory;
      break;
    case 1:      // KeyNodeInformation
      put64(record, k->write_time); put32(record, 0); put32(record, 20 + name_bytes); put32(record, 0); put32(record, name_bytes);
      fixed = 20;
      if (!utf16_of(record, k->name.view())) return kNoMemory;
      break;
    case 2:      // KeyFullInformation
      put64(record, k->write_time); put32(record, 0); put32(record, 44); put32(record, 0); put32(record, s.subkeys); put32(record, s.max_subkey_name_bytes);
      put32(record, 0); put32(record, s.values); put32(record, s.max_value_name_bytes); put32(record, s.max_value_data_bytes);
      fixed = 44;
      break;
    case 3: {    // KeyNameInformation: the whole path
      String full(p.files().allocator());
      if (!t->full_name(id, &full)) return kNoMemory;
      put32(record, utf16_units(full.view()) * 2);
      fixed = 4;
      if (!utf16_of(record, full.view())) return kNoMemory;
      break;
    }
    case 4:      // KeyCachedInformation
      put64(record, k->write_time); put32(record, 0); put32(record, s.subkeys); put32(record, s.max_subkey_name_bytes); put32(record, s.values); put32(record, s.max_value_name_bytes);
      put32(record, s.max_value_data_bytes); put32(record, name_bytes);
      fixed = 40;
      if (!utf16_of(record, k->name.view())) return kNoMemory;
      break;
    case 5:      // KeyFlagsInformation
      put32(record, 0); put32(record, k->volatile_key ? 1 : 0); put32(record, 0);
      fixed = 12;
      break;
    default: return kInvalidParameter;
  }
  return deliver(p, r.args[2], length, result, record, fixed);
}

uint32_t nt_enumerate_key(Process& p, const SyscallRequest& r) {
  // NtEnumerateKey(HANDLE, ULONG Index, KEY_INFORMATION_CLASS, PVOID, ULONG Length, PULONG ResultLength)
  uint64_t length_arg = 0, result = 0;
  if (!argument(p, r, 4, &length_arg) || !argument(p, r, 5, &result)) return kAccessViolation;
  uint32_t id = 0;
  if (!key_of(p, r.args[0], &id)) return kInvalidHandle;
  const uint32_t index = static_cast<uint32_t>(r.args[1]), info_class = static_cast<uint32_t>(r.args[2]), length = static_cast<uint32_t>(length_arg);
  if (info_class > 3) return kInvalidParameter;
  Guard guard;
  RegistryTree* t = tree(p);
  if (!t) return kNoMemory;
  const RegKey* k = t->key(id);
  if (!k) return kInvalidHandle;
  if (k->deleted) return kRegKeyDeleted;
  uint32_t seen = 0;
  const RegKey* child = nullptr;
  for (uint64_t i = 0; i < k->subkeys.count(); ++i) {
    const RegKey* c = t->key(k->subkeys[i]);
    if (!c || c->deleted) continue;
    if (seen++ == index) { child = c; break; }
  }
  if (!child) return kNoMoreEntries;
  const uint32_t name_bytes = utf16_units(child->name.view()) * 2;
  ByteBuffer record(p.files().allocator());
  uint32_t fixed = 0;
  switch (info_class) {
    case 0: put64(record, child->write_time); put32(record, 0); put32(record, name_bytes); fixed = 16; break;
    case 1: put64(record, child->write_time); put32(record, 0); put32(record, 20 + name_bytes); put32(record, 0); put32(record, name_bytes); fixed = 20; break;
    case 2: return kInvalidParameter;
    default: put32(record, name_bytes); fixed = 4; break;      // KeyNameInformation
  }
  if (!utf16_of(record, child->name.view())) return kNoMemory;
  return deliver(p, r.args[3], length, result, record, fixed);
}

namespace {
// KEY_VALUE_*_INFORMATION for a value: 0 basic, 1 full, 2 partial (3 and 4 are the 64-bit aligned forms of 1 and 2).
uint32_t value_record(Process& p, const RegValue& v, uint32_t info_class, ByteBuffer& record, uint32_t* fixed) {
  const uint32_t name_bytes = utf16_units(v.name.view()) * 2, data_bytes = static_cast<uint32_t>(v.data.size());
  switch (info_class) {
    case 0:
      put32(record, 0); put32(record, v.type); put32(record, name_bytes);
      *fixed = 12;
      if (!utf16_of(record, v.name.view())) return kNoMemory;
      return kSuccess;
    case 1: case 3: {
      const uint32_t offset = (20 + name_bytes + 7) & ~7u;
      put32(record, 0); put32(record, v.type); put32(record, offset); put32(record, data_bytes); put32(record, name_bytes);
      *fixed = 20;
      if (!utf16_of(record, v.name.view())) return kNoMemory;
      if (!record.put_zeros(offset - record.size()) || !record.append(v.data.view())) return kNoMemory;
      return kSuccess;
    }
    case 2: case 4:
      put32(record, 0); put32(record, v.type); put32(record, data_bytes);
      *fixed = 12;
      if (info_class == 4 && !record.put_zeros(4)) return kNoMemory;
      if (!record.append(v.data.view())) return kNoMemory;
      return kSuccess;
    default: return kInvalidParameter;
  }
  (void)p;
}
}  // namespace

uint32_t nt_enumerate_value_key(Process& p, const SyscallRequest& r) {
  // NtEnumerateValueKey(HANDLE, ULONG Index, KEY_VALUE_INFORMATION_CLASS, PVOID, ULONG Length, PULONG ResultLength)
  uint64_t length_arg = 0, result = 0;
  if (!argument(p, r, 4, &length_arg) || !argument(p, r, 5, &result)) return kAccessViolation;
  uint32_t id = 0;
  if (!key_of(p, r.args[0], &id)) return kInvalidHandle;
  const uint32_t index = static_cast<uint32_t>(r.args[1]), info_class = static_cast<uint32_t>(r.args[2]), length = static_cast<uint32_t>(length_arg);
  if (info_class > 4) return kInvalidParameter;
  Guard guard;
  RegistryTree* t = tree(p);
  if (!t) return kNoMemory;
  const RegKey* k = t->key(id);
  if (!k) return kInvalidHandle;
  if (k->deleted) return kRegKeyDeleted;
  if (index >= k->values.count() || !k->values[index]) return kNoMoreEntries;
  ByteBuffer record(p.files().allocator());
  uint32_t fixed = 0;
  const uint32_t status = value_record(p, *k->values[index], info_class, record, &fixed);
  if (status != kSuccess) return status;
  return deliver(p, r.args[3], length, result, record, fixed);
}

uint32_t nt_query_value_key(Process& p, const SyscallRequest& r) {
  // NtQueryValueKey(HANDLE, PUNICODE_STRING ValueName, KEY_VALUE_INFORMATION_CLASS, PVOID, ULONG Length, PULONG ResultLength)
  uint64_t length_arg = 0, result = 0;
  if (!argument(p, r, 4, &length_arg) || !argument(p, r, 5, &result)) return kAccessViolation;
  uint32_t id = 0;
  if (!key_of(p, r.args[0], &id)) return kInvalidHandle;
  const uint32_t info_class = static_cast<uint32_t>(r.args[2]), length = static_cast<uint32_t>(length_arg);
  if (info_class > 4) return kInvalidParameter;
  String name(p.files().allocator());
  if (r.args[1] != 0) { const uint32_t s = read_unicode_string_argument(p, r.args[1], &name); if (s != kSuccess) return s; }
  Guard guard;
  RegistryTree* t = tree(p);
  if (!t) return kNoMemory;
  const RegKey* k = t->key(id);
  if (!k) return kInvalidHandle;
  if (k->deleted) return kRegKeyDeleted;
  const RegValue* v = t->find_value(id, name.view());
  if (!v) return kRegNameNotFound;
  ByteBuffer record(p.files().allocator());
  uint32_t fixed = 0;
  const uint32_t status = value_record(p, *v, info_class, record, &fixed);
  if (status != kSuccess) return status;
  return deliver(p, r.args[3], length, result, record, fixed);
}

uint32_t nt_set_value_key(Process& p, const SyscallRequest& r) {
  // NtSetValueKey(HANDLE, PUNICODE_STRING ValueName, ULONG TitleIndex, ULONG Type, PVOID Data, ULONG DataSize)
  uint64_t type = 0, data_at = 0, size = 0;
  if (!argument(p, r, 3, &type) || !argument(p, r, 4, &data_at) || !argument(p, r, 5, &size)) return kAccessViolation;
  uint32_t id = 0;
  if (!key_of(p, r.args[0], &id)) return kInvalidHandle;
  size &= 0xFFFFFFFFu;
  if (size > kRegMaxValueBytes) return kInvalidParameter;
  String name(p.files().allocator());
  if (r.args[1] != 0) { const uint32_t s = read_unicode_string_argument(p, r.args[1], &name); if (s != kSuccess) return s; }
  ByteBuffer data(p.files().allocator());
  if (size != 0) {
    if (!data.put_zeros(size)) return kNoMemory;
    if (!p.read_user(data_at, data.writable_view())) return kAccessViolation;
  }
  uint32_t status;
  {
    Guard guard;
    RegistryTree* t = tree(p);
    if (!t) return kNoMemory;
    status = t->set_value(id, name.view(), static_cast<uint32_t>(type), data.view(), now_filetime(p));
  }
  if (status == kSuccess) notify_key_changed(p, id, true, kNotifyLastSet);
  return status;
}

uint32_t nt_delete_key(Process& p, const SyscallRequest& r) {
  uint32_t id = 0;
  if (!key_of(p, r.args[0], &id)) return kInvalidHandle;
  uint32_t status;
  String gone(p.files().allocator());
  {
    Guard guard;
    RegistryTree* t = tree(p);
    if (t) t->full_name(id, &gone);
    status = t ? t->delete_key(id) : kNoMemory;
  }
  if (status == kSuccess && !gone.empty()) if (ObjectManager* objects = system_objects()) {      // the watches of the key above hear of it
    String path(p.files().allocator());
    registry_watch_path(gone.view(), false, &path);
    objects->notify_entry_changed(path.view(), 2, kNotifyName);
  }
  if (status == kSuccess) if (ObjectManager* objects = system_objects()) {      // the descriptor kept for the key goes with it
    char key[16];
    objects->security_forget(StringView(key, ObjectManager::object_key('k', id, key)));
  }
  return status;
}

uint32_t nt_delete_value_key(Process& p, const SyscallRequest& r) {
  uint32_t id = 0;
  if (!key_of(p, r.args[0], &id)) return kInvalidHandle;
  String name(p.files().allocator());
  if (r.args[1] != 0) { const uint32_t s = read_unicode_string_argument(p, r.args[1], &name); if (s != kSuccess) return s; }
  uint32_t status;
  {
    Guard guard;
    RegistryTree* t = tree(p);
    status = t ? t->delete_value(id, name.view(), now_filetime(p)) : kNoMemory;
  }
  if (status == kSuccess) notify_key_changed(p, id, true, kNotifyLastSet);
  return status;
}

uint32_t nt_flush_key(Process& p, const SyscallRequest& r);


// ---- renaming, several values at once, information, notifications, hive files ---------------------------------------------------------------------------------------

namespace {
constexpr uint32_t kPrivilegeNotHeld = 0xC0000061u, kAccessDenied = 0xC0000022u, kPending = 0x103u, kObjectNameCollision = 0xC0000035u, kObjectNameNotFound = 0xC0000034u;
constexpr uint32_t kSeBackupPrivilege = 17, kSeRestorePrivilege = 18;
constexpr uint64_t kMaxHiveBytes = 64ull << 20;

// IO_STATUS_BLOCK { Status (4), Information (8, at 8) }.
void set_io_status(Process& p, uint64_t address, uint32_t status) {
  if (address != 0) { p.write_user_u32(address, status); p.write_user_u64(address + 8, 0); }
}

// The file behind a handle that was opened for the hive services; null if it is not a file.
OpenObject* file_of(Process& p, uint64_t handle) {
  if (handle == 0 || (handle & 3) != 0) return nullptr;
  const uint64_t slot = handle / 4 - 1;
  if (slot >= ProcessFiles::kMaxDescriptors || p.files().kind(slot) != DescriptorKind::File) return nullptr;
  OpenObject* o = p.files().object(slot);
  return o && o->file ? o : nullptr;
}

// Reads a whole file into `out` (at most kMaxHiveBytes).
uint32_t read_all(Process& p, OpenObject& file, ByteBuffer* out) {
  const uint64_t size = p.files().size_of(file);
  if (size > kMaxHiveBytes) return kInvalidParameter;
  if (size == 0) return kSuccess;
  if (!out->put_zeros(size)) return kNoMemory;
  uint64_t got = 0;
  if (p.files().read(file, 0, out->writable_view(), &got) != filesystem::Status::Ok || got != size) return 0xC0000185u;      // STATUS_IO_DEVICE_ERROR
  return kSuccess;
}

// The file an OBJECT_ATTRIBUTES names, read whole. `path` (if not null) gets the cleaned virtual path.
uint32_t read_file_by_attributes(Process& p, uint64_t attributes, ByteBuffer* out, String* path) {
  String name(p.files().allocator()), virtual_path(p.files().allocator());
  if (attributes == 0) return kAccessViolation;
  uint8_t raw[32];
  if (!p.read_user(attributes, ByteView(raw, 32))) return kAccessViolation;
  uint64_t name_at = 0;
  for (int i = 0; i < 8; ++i) name_at |= uint64_t{raw[16 + i]} << (8 * i);
  if (name_at == 0) return kInvalidParameter;
  uint32_t status = read_unicode_string_argument(p, name_at, &name);
  if (status != kSuccess) return status;
  if (p.files().resolve(name.view(), &virtual_path) != PathResult::Ok) return 0xC0000033u;      // STATUS_OBJECT_NAME_INVALID
  OpenFlags flags;
  flags.read = true;
  flags.must_not_be_directory = true;
  uint32_t slot = 0;
  const filesystem::Status opened = p.files().open(virtual_path.view(), flags, &slot);
  if (opened != filesystem::Status::Ok) return opened == filesystem::Status::NotFound ? kObjectNameNotFound : 0xC0000001u;
  OpenObject* file = p.files().object(slot);
  status = file ? read_all(p, *file, out) : kInvalidHandle;
  p.files().close(slot);
  if (status == kSuccess && path) path->assign(virtual_path.view());
  return status;
}

// Writes `image` as the whole content of the file at a virtual path (made if it is not there).
uint32_t write_file_at(Process& p, StringView virtual_path, ConstByteView image) {
  OpenFlags flags;
  flags.write = true;
  flags.create = true;
  flags.truncate = true;
  uint32_t slot = 0;
  if (p.files().open(virtual_path, flags, &slot) != filesystem::Status::Ok) return kAccessDenied;
  OpenObject* file = p.files().object(slot);
  uint64_t done = 0;
  const bool ok = file && p.files().write(*file, 0, image, &done) == filesystem::Status::Ok && done == image.size();
  p.files().close(slot);
  return ok ? kSuccess : 0xC0000185u;
}

uint32_t save_image(Process& p, uint32_t id, ByteBuffer* image) {
  Guard guard;
  RegistryTree* t = tree(p);
  if (!t) return kNoMemory;
  if (!t->alive(id)) return kRegKeyDeleted;
  return t->save(id, image) ? kSuccess : kNoMemory;
}
}  // namespace

uint32_t nt_rename_key(Process& p, const SyscallRequest& r) {
  // NtRenameKey(HANDLE KeyHandle, PUNICODE_STRING NewName)
  uint32_t id = 0;
  if (!key_of(p, r.args[0], &id)) return kInvalidHandle;
  if (r.args[1] == 0) return kAccessViolation;
  String name(p.files().allocator());
  uint32_t status = read_unicode_string_argument(p, r.args[1], &name);
  if (status != kSuccess) return status;
  String old_name(p.files().allocator());
  {
    Guard guard;
    RegistryTree* t = tree(p);
    if (!t) return kNoMemory;
    t->full_name(id, &old_name);
    status = t->rename_key(id, name.view(), now_filetime(p));
  }
  if (status == kSuccess) {
    notify_key_changed(p, id, false, kNotifyName);
    if (ObjectManager* objects = system_objects()) if (!old_name.empty()) {
      String path(p.files().allocator());
      registry_watch_path(old_name.view(), false, &path);
      objects->notify_entry_changed(path.view(), 2, kNotifyName);
    }
  }
  return status;
}

uint32_t nt_query_multiple_value_key(Process& p, const SyscallRequest& r) {
  // NtQueryMultipleValueKey(HANDLE, PKEY_VALUE_ENTRY ValueEntries, ULONG EntryCount, PVOID ValueBuffer, PULONG BufferLength, PULONG RequiredBufferLength)
  // KEY_VALUE_ENTRY { PUNICODE_STRING ValueName (0); ULONG DataLength (8); ULONG DataOffset (12); ULONG Type (16); } = 24 bytes
  uint64_t length_at = 0, required_at = 0;
  if (!argument(p, r, 4, &length_at) || !argument(p, r, 5, &required_at)) return kAccessViolation;
  uint32_t id = 0;
  if (!key_of(p, r.args[0], &id)) return kInvalidHandle;
  const uint32_t count = static_cast<uint32_t>(r.args[2]);
  if (count == 0 || count > 1024 || length_at == 0) return kInvalidParameter;
  uint8_t four[4];
  if (!p.read_user(length_at, ByteView(four, 4))) return kAccessViolation;
  const uint64_t capacity = four[0] | (four[1] << 8) | (four[2] << 16) | (uint64_t{four[3]} << 24);
  ByteBuffer entries(p.files().allocator());
  if (!entries.put_zeros(uint64_t{count} * 24) || !p.read_user(r.args[1], entries.writable_view())) return kAccessViolation;
  ByteBuffer data(p.files().allocator());
  ByteBuffer meta(p.files().allocator());      // per entry: data length, offset, type
  uint32_t status = kSuccess;
  {
    Guard guard;
    RegistryTree* t = tree(p);
    if (!t) return kNoMemory;
    if (!t->alive(id)) return kRegKeyDeleted;
    for (uint32_t i = 0; i < count && status == kSuccess; ++i) {
      uint64_t name_at = 0;
      for (int b = 0; b < 8; ++b) name_at |= uint64_t{entries.view()[uint64_t{i} * 24 + b]} << (8 * b);
      String name(p.files().allocator());
      if (name_at != 0) { status = read_unicode_string_argument(p, name_at, &name); if (status != kSuccess) break; }
      const RegValue* v = t->find_value(id, name.view());
      if (!v) { status = kRegNameNotFound; break; }
      if (!data.put_zeros((8 - (data.size() & 7)) & 7)) { status = kNoMemory; break; }
      const uint64_t offset = data.size();
      if (!data.append(v->data.view()) || !meta.put32(static_cast<uint32_t>(v->data.size())) || !meta.put32(static_cast<uint32_t>(offset)) || !meta.put32(v->type)) status = kNoMemory;
    }
  }
  if (status != kSuccess) return status;
  if (required_at != 0 && !p.write_user_u32(required_at, static_cast<uint32_t>(data.size()))) return kAccessViolation;
  for (uint32_t i = 0; i < count; ++i) {      // the entries tell where each value goes, also when the buffer is too small for them
    uint8_t out[12];
    for (int b = 0; b < 12; ++b) out[b] = meta.view()[uint64_t{i} * 12 + b];
    if (!p.write_user(r.args[1] + uint64_t{i} * 24 + 8, ConstByteView(out, 12))) return kAccessViolation;
  }
  if (data.size() > capacity) return kBufferOverflow;
  if (data.size() != 0 && !p.write_user(r.args[3], data.view())) return kAccessViolation;
  if (!p.write_user_u32(length_at, static_cast<uint32_t>(data.size()))) return kAccessViolation;
  return kSuccess;
}

uint32_t nt_set_information_key(Process& p, const SyscallRequest& r) {
  // NtSetInformationKey(HANDLE, KEY_SET_INFORMATION_CLASS, PVOID, ULONG Length): 0 write time; 1 Wow64 flags, 2 control flags, 3 virtualization, 4 debug, 5 handle tags (checked, not kept)
  uint32_t id = 0;
  if (!key_of(p, r.args[0], &id)) return kInvalidHandle;
  const uint32_t info_class = static_cast<uint32_t>(r.args[1]), length = static_cast<uint32_t>(r.args[3]);
  if (info_class > 5) return kInvalidParameter;
  if (length < (info_class == 0 ? 8u : 4u)) return 0xC0000004u;      // STATUS_INFO_LENGTH_MISMATCH
  if (info_class == 0) {
    uint64_t when = 0;
    if (!p.read_user_u64(r.args[2], &when)) return kAccessViolation;
    Guard guard;
    RegistryTree* t = tree(p);
    RegKey* k = t ? t->key(id) : nullptr;
    if (!k) return kInvalidHandle;
    if (k->deleted) return kRegKeyDeleted;
    k->write_time = when;
  }
  return kSuccess;
}

namespace {
// NtNotifyChangeKey and ..MultipleKeys. `event`: a handle or 0. With `asynchronous` and an event the watch belongs to the kernel, which signals the event at the first change;
// STATUS_PENDING comes back. Without, the call waits for a change, and the IO_STATUS_BLOCK says STATUS_SUCCESS when it comes.
uint32_t notify_change(Process& p, uint64_t key_handle, uint64_t event, uint64_t status_block, uint32_t filter, bool subtree, bool asynchronous) {
  uint32_t id = 0;
  if (!key_of(p, key_handle, &id)) return kInvalidHandle;
  if (filter == 0 || (filter & ~0xFu) != 0) return kInvalidParameter;
  ObjectManager* objects = system_objects();
  if (!objects) return 0xC0000002u;
  uint32_t event_object = 0;
  if (event != 0) {
    if ((event & 3) != 0 || event / 4 - 1 >= ProcessFiles::kMaxDescriptors || p.files().kind(event / 4 - 1) != DescriptorKind::Event) return kObjectTypeMismatch;
    event_object = static_cast<uint32_t>(p.files().token(event / 4 - 1));
  }
  String full(p.files().allocator()), path(p.files().allocator());
  {
    Guard guard;
    RegistryTree* t = tree(p);
    if (!t) return kNoMemory;
    if (!t->alive(id)) return kRegKeyDeleted;
    t->full_name(id, &full);
  }
  registry_watch_path(full.view(), false, &path);
  const bool kernel_owned = asynchronous && event_object != 0;
  const ObjectManager::Created c = objects->create_key_watch(path.view(), subtree, filter, kernel_owned ? event_object : 0, p.id());
  if (c.id == 0) return c.status != 0 ? c.status : kNoMemory;
  if (asynchronous) {
    if (!kernel_owned) objects->release(c.id);      // (nobody could learn of the change)
    return kPending;
  }
  const uint32_t watch = c.id;
  const uint32_t status = wait_until(p, ~uint64_t{0}, [&]() -> uint64_t { return objects->key_watch_fired_locked(watch) ? uint64_t{kSuccess} : kWaitPending; });
  objects->release(watch);
  if (status == kSuccess && event_object != 0) objects->set_event(event_object, nullptr);
  set_io_status(p, status_block, status);
  return status;
}
}  // namespace

uint32_t nt_notify_change_key(Process& p, const SyscallRequest& r) {
  // NtNotifyChangeKey(HANDLE KeyHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, ULONG CompletionFilter, BOOLEAN WatchTree,
  //                   PVOID Buffer, ULONG BufferSize, BOOLEAN Asynchronous)
  uint64_t filter = 0, tree_flag = 0, async = 0;
  if (!argument(p, r, 5, &filter) || !argument(p, r, 6, &tree_flag) || !argument(p, r, 9, &async)) return kAccessViolation;
  return notify_change(p, r.args[0], r.args[1], r.args[4], static_cast<uint32_t>(filter), (tree_flag & 0xFF) != 0, (async & 0xFF) != 0);
}

uint32_t nt_notify_change_multiple_keys(Process& p, const SyscallRequest& r) {
  // NtNotifyChangeMultipleKeys(HANDLE MasterKeyHandle, ULONG Count, OBJECT_ATTRIBUTES SlaveObjects[], HANDLE Event, PIO_APC_ROUTINE, PVOID ApcContext, PIO_STATUS_BLOCK,
  //                            ULONG CompletionFilter, BOOLEAN WatchTree, PVOID Buffer, ULONG BufferSize, BOOLEAN Asynchronous)
  uint64_t status_block = 0, filter = 0, tree_flag = 0, async = 0;
  if (!argument(p, r, 6, &status_block) || !argument(p, r, 7, &filter) || !argument(p, r, 8, &tree_flag) || !argument(p, r, 11, &async)) return kAccessViolation;
  if ((r.args[1] & 0xFFFFFFFFu) != 0) return kInvalidParameter;      // (only the master key can be watched here)
  return notify_change(p, r.args[0], r.args[3], status_block, static_cast<uint32_t>(filter), (tree_flag & 0xFF) != 0, (async & 0xFF) != 0);
}

namespace {
uint32_t save_to_handle(Process& p, uint64_t key_handle, uint64_t file_handle) {
  uint32_t id = 0;
  if (!key_of(p, key_handle, &id)) return kInvalidHandle;
  if (!caller_privilege_enabled(p, kSeBackupPrivilege)) return kPrivilegeNotHeld;
  OpenObject* file = file_of(p, file_handle);
  if (!file) return kInvalidHandle;
  if (!file->writable) return kAccessDenied;
  ByteBuffer image(p.files().allocator());
  const uint32_t status = save_image(p, id, &image);
  if (status != kSuccess) return status;
  uint64_t done = 0;
  if (p.files().resize(*file, 0) != filesystem::Status::Ok || p.files().write(*file, 0, image.view(), &done) != filesystem::Status::Ok || done != image.size()) return 0xC0000185u;
  return kSuccess;
}
}  // namespace

uint32_t nt_save_key(Process& p, const SyscallRequest& r) { return save_to_handle(p, r.args[0], r.args[1]); }      // NtSaveKey(HANDLE KeyHandle, HANDLE FileHandle)
uint32_t nt_save_key_ex(Process& p, const SyscallRequest& r) {      // NtSaveKeyEx(HANDLE KeyHandle, HANDLE FileHandle, ULONG Format): 1 standard, 2 latest, 3 no compression; one format here
  const uint32_t format = static_cast<uint32_t>(r.args[2]);
  if (format < 1 || format > 3) return kInvalidParameter;
  return save_to_handle(p, r.args[0], r.args[1]);
}

namespace {
uint32_t restore_image(Process& p, uint32_t id, const ByteBuffer& image, bool force) {
  Guard guard;
  RegistryTree* t = tree(p);
  if (!t) return kNoMemory;
  if (!t->alive(id)) return kRegKeyDeleted;
  if (!force && t->count_open_keys(id) > 1) return kAccessDenied;      // (the caller's own handle is the one)
  return t->restore(id, image.view(), true, now_filetime(p));
}
}  // namespace

uint32_t nt_restore_key(Process& p, const SyscallRequest& r) {
  // NtRestoreKey(HANDLE KeyHandle, HANDLE FileHandle, ULONG Flags): 1 whole hive volatile, 2 refresh hive, 4 no lazy flush, 8 force restore
  uint32_t id = 0;
  if (!key_of(p, r.args[0], &id)) return kInvalidHandle;
  if (!caller_privilege_enabled(p, kSeRestorePrivilege)) return kPrivilegeNotHeld;
  OpenObject* file = file_of(p, r.args[1]);
  if (!file) return kInvalidHandle;
  if (!file->readable) return kAccessDenied;
  if ((r.args[2] & 0xFFFFFFFFu & ~0xFu) != 0) return kInvalidParameter;
  ByteBuffer image(p.files().allocator());
  uint32_t status = read_all(p, *file, &image);
  if (status != kSuccess) return status;
  status = restore_image(p, id, image, (r.args[2] & 8) != 0);
  if (status == kSuccess) { notify_key_changed(p, id, false, kNotifyName | kNotifyLastSet); notify_key_changed(p, id, true, kNotifyLastSet); }
  return status;
}

uint32_t nt_replace_key(Process& p, const SyscallRequest& r) {
  // NtReplaceKey(POBJECT_ATTRIBUTES NewFile, HANDLE TargetHandle, POBJECT_ATTRIBUTES BackupFile): the key becomes what the new file holds, now (a real one waits for the next
  // boot); what it held is kept in the backup file.
  uint32_t id = 0;
  if (!key_of(p, r.args[1], &id)) return kInvalidHandle;
  if (!caller_privilege_enabled(p, kSeRestorePrivilege)) return kPrivilegeNotHeld;
  ByteBuffer image(p.files().allocator());
  uint32_t status = read_file_by_attributes(p, r.args[0], &image, nullptr);
  if (status != kSuccess) return status;
  if (!RegistryTree::valid_image(image.view())) return kInvalidParameter;
  if (r.args[2] != 0) {
    uint8_t raw[32];
    uint64_t name_at = 0;
    if (!p.read_user(r.args[2], ByteView(raw, 32))) return kAccessViolation;
    for (int i = 0; i < 8; ++i) name_at |= uint64_t{raw[16 + i]} << (8 * i);
    String name(p.files().allocator()), virtual_path(p.files().allocator());
    if (name_at == 0) return kInvalidParameter;
    status = read_unicode_string_argument(p, name_at, &name);
    if (status != kSuccess) return status;
    if (p.files().resolve(name.view(), &virtual_path) != PathResult::Ok) return 0xC0000033u;
    ByteBuffer old(p.files().allocator());
    status = save_image(p, id, &old);
    if (status == kSuccess) status = write_file_at(p, virtual_path.view(), old.view());
    if (status != kSuccess) return status;
  }
  status = restore_image(p, id, image, true);
  if (status == kSuccess) notify_key_changed(p, id, false, kNotifyName | kNotifyLastSet);
  return status;
}

namespace {
// The key a hive is loaded as: a new key directly below \Registry\Machine or \Registry\User.
uint32_t load_hive(Process& p, uint64_t target_attributes, uint64_t source_attributes) {
  if (!caller_privilege_enabled(p, kSeRestorePrivilege)) return kPrivilegeNotHeld;
  uint32_t base = 0;
  bool inherit = false;
  String path(p.files().allocator());
  uint32_t status = read_attributes(p, target_attributes, &base, &path, &inherit);
  if (status != kSuccess) return status;
  ByteBuffer image(p.files().allocator());
  String file_path(p.files().allocator());
  status = read_file_by_attributes(p, source_attributes, &image, &file_path);
  if (status != kSuccess) return status;
  if (!RegistryTree::valid_image(image.view())) return kInvalidParameter;
  uint32_t id = 0;
  bool created = false;
  Guard guard;
  RegistryTree* t = tree(p);
  if (!t) return kNoMemory;
  // The parent must be a hive container: split off the last component and look at what is above it.
  const uint64_t cut = path.view().rfind('\\');
  if (cut == StringView::npos) return kInvalidParameter;
  uint32_t parent = 0;
  status = t->resolve(base, path.view().first(cut), false, now_filetime(p), &parent, &created);
  if (status != kSuccess) return status;
  String parent_name(p.files().allocator());
  t->full_name(parent, &parent_name);
  if (!RegistryTree::names_equal(parent_name.view(), "\\REGISTRY\\MACHINE") && !RegistryTree::names_equal(parent_name.view(), "\\REGISTRY\\USER")) return kInvalidParameter;
  status = t->resolve(base, path.view(), true, now_filetime(p), &id, &created);
  if (status != kSuccess) return status;
  if (!created) return kObjectNameCollision;
  status = t->restore(id, image.view(), true, now_filetime(p));
  if (status != kSuccess) { t->delete_tree(id); return status; }
  RegKey* k = t->key(id);
  k->loaded_hive = true;
  k->hive_file.assign(file_path.view());
  return kSuccess;
}
}  // namespace

uint32_t nt_load_key(Process& p, const SyscallRequest& r) { return load_hive(p, r.args[0], r.args[1]); }      // NtLoadKey(POBJECT_ATTRIBUTES TargetKey, POBJECT_ATTRIBUTES SourceFile [, ULONG Flags])
uint32_t nt_load_key_ex(Process& p, const SyscallRequest& r) { return load_hive(p, r.args[0], r.args[1]); }   // NtLoadKeyEx(TargetKey, SourceFile, ULONG Flags, HANDLE TrustClassKey, ...): the rest is not used

uint32_t nt_unload_key(Process& p, const SyscallRequest& r) {
  // NtUnloadKey(POBJECT_ATTRIBUTES TargetKey): the hive is written back to its file, then goes. Only a loaded hive, and not while a handle is open in it.
  if (!caller_privilege_enabled(p, kSeRestorePrivilege)) return kPrivilegeNotHeld;
  uint32_t base = 0;
  bool inherit = false;
  String path(p.files().allocator());
  uint32_t status = read_attributes(p, r.args[0], &base, &path, &inherit);
  if (status != kSuccess) return status;
  ByteBuffer image(p.files().allocator());
  String file(p.files().allocator());
  {
    Guard guard;
    RegistryTree* t = tree(p);
    if (!t) return kNoMemory;
    uint32_t id = 0;
    bool created = false;
    status = t->resolve(base, path.view(), false, now_filetime(p), &id, &created);
    if (status != kSuccess) return status;
    const RegKey* k = t->key(id);
    if (!k || !k->loaded_hive) return kInvalidParameter;
    if (t->has_open_handles(id)) return kAccessDenied;
    file.assign(k->hive_file.view());
    if (!t->save(id, &image)) return kNoMemory;
    status = t->delete_tree(id);
    if (status != kSuccess) return status;
  }
  if (!file.empty()) status = write_file_at(p, file.view(), image.view());
  return status;
}

uint32_t nt_query_open_sub_keys(Process& p, const SyscallRequest& r) {
  // NtQueryOpenSubKeys(POBJECT_ATTRIBUTES TargetKey, PULONG HandleCount)
  uint32_t base = 0;
  bool inherit = false;
  String path(p.files().allocator());
  uint32_t status = read_attributes(p, r.args[0], &base, &path, &inherit);
  if (status != kSuccess) return status;
  uint32_t count = 0;
  {
    Guard guard;
    RegistryTree* t = tree(p);
    if (!t) return kNoMemory;
    uint32_t id = 0;
    bool created = false;
    status = t->resolve(base, path.view(), false, now_filetime(p), &id, &created);
    if (status != kSuccess) return status;
    count = t->count_open_keys(id);
  }
  return p.write_user_u32(r.args[1], count) ? kSuccess : kAccessViolation;
}

uint32_t nt_compact_keys(Process& p, const SyscallRequest& r) {
  // NtCompactKeys(ULONG Count, HANDLE KeyArray[]): nothing is kept apart that could be packed
  const uint32_t count = static_cast<uint32_t>(r.args[0]);
  if (count == 0 || count > 1024) return kInvalidParameter;
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t handle = 0;
    uint32_t id = 0;
    if (!p.read_user_u64(r.args[1] + uint64_t{i} * 8, &handle)) return kAccessViolation;
    if (!key_of(p, handle, &id)) return kInvalidHandle;
  }
  return kSuccess;
}

uint32_t nt_lock_registry_key(Process& p, const SyscallRequest& r) {
  uint32_t id = 0;
  return key_of(p, r.args[0], &id) ? kSuccess : kInvalidHandle;      // NtLockRegistryKey(HANDLE): (nothing here changes a key behind a program's back)
}

uint32_t nt_create_key_transacted(Process& p, const SyscallRequest& r) {
  // NtCreateKeyTransacted(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG TitleIndex, PUNICODE_STRING Class, ULONG CreateOptions, HANDLE TransactionHandle, PULONG Disposition):
  // every change is made at once, so the transaction only has to be a handle.
  uint64_t transaction = 0, disposition_at = 0;
  if (!argument(p, r, 6, &transaction) || !argument(p, r, 7, &disposition_at)) return kAccessViolation;
  if (transaction == 0) return kInvalidHandle;
  uint32_t disposition = 0;
  const uint32_t status = open_or_create(p, r, true, &disposition);
  if (status == kSuccess && disposition_at != 0 && !p.write_user_u32(disposition_at, disposition)) return kAccessViolation;
  uint64_t handle = 0;
  if (status == kSuccess && disposition == 1 && p.read_user_u64(r.args[0], &handle) && handle != 0) { uint32_t made = 0; if (key_of(p, handle, &made)) notify_key_changed(p, made, false, kNotifyName); }
  return status;
}

uint32_t nt_open_key_transacted(Process& p, const SyscallRequest& r) {
  // NtOpenKeyTransacted(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE TransactionHandle); NtOpenKeyTransactedEx(..., ULONG OpenOptions, HANDLE TransactionHandle)
  uint64_t transaction = r.args[3], last = 0;
  if (r.number == static_cast<uint64_t>(WindowsSyscallNumber::NtOpenKeyTransactedEx)) { if (!argument(p, r, 4, &last)) return kAccessViolation; transaction = last; }
  if (transaction == 0) return kInvalidHandle;
  return open_key_checked(p, r);
}


uint32_t nt_flush_key(Process& p, const SyscallRequest& r) {
  uint32_t id = 0;
  if (!key_of(p, r.args[0], &id)) return kInvalidHandle;
  ByteBuffer image(p.files().allocator());
  String file(p.files().allocator());
  {
    Guard guard;
    RegistryTree* t = tree(p);
    const RegKey* k = t ? t->key(id) : nullptr;
    if (!k || !k->loaded_hive || k->hive_file.empty()) return kSuccess;      // (the built-in hives are in memory only)
    file.assign(k->hive_file.view());
    if (!t->save(id, &image)) return kNoMemory;
  }
  return write_file_at(p, file.view(), image.view());
}

}  // namespace process::windowsabi
