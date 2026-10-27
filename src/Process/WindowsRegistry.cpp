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
  // NtCreateKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG TitleIndex, PUNICODE_STRING Class, ULONG CreateOptions, PULONG Disposition)
  uint64_t disposition_at = 0;
  if (!argument(p, r, 6, &disposition_at)) return kAccessViolation;
  uint32_t disposition = 0;
  const uint32_t status = open_or_create(p, r, true, &disposition);
  if (status == kSuccess && disposition_at != 0 && !p.write_user_u32(disposition_at, disposition)) return kAccessViolation;
  return status;
}
uint32_t nt_open_key(Process& p, const SyscallRequest& r) { return open_or_create(p, r, false, nullptr); }
uint32_t nt_open_key_ex(Process& p, const SyscallRequest& r) { return open_or_create(p, r, false, nullptr); }

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
  Guard guard;
  RegistryTree* t = tree(p);
  if (!t) return kNoMemory;
  return t->set_value(id, name.view(), static_cast<uint32_t>(type), data.view(), now_filetime(p));
}

uint32_t nt_delete_key(Process& p, const SyscallRequest& r) {
  uint32_t id = 0;
  if (!key_of(p, r.args[0], &id)) return kInvalidHandle;
  Guard guard;
  RegistryTree* t = tree(p);
  return t ? t->delete_key(id) : kNoMemory;
}

uint32_t nt_delete_value_key(Process& p, const SyscallRequest& r) {
  uint32_t id = 0;
  if (!key_of(p, r.args[0], &id)) return kInvalidHandle;
  String name(p.files().allocator());
  if (r.args[1] != 0) { const uint32_t s = read_unicode_string_argument(p, r.args[1], &name); if (s != kSuccess) return s; }
  Guard guard;
  RegistryTree* t = tree(p);
  return t ? t->delete_value(id, name.view(), now_filetime(p)) : kNoMemory;
}

uint32_t nt_flush_key(Process& p, const SyscallRequest& r) {
  uint32_t id = 0;
  return key_of(p, r.args[0], &id) ? kSuccess : kInvalidHandle;      // (nothing is kept on disk)
}

}  // namespace process::windowsabi
