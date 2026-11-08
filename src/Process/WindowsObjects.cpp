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
#include "Process/WindowsDebug.hpp"
#include "Process/WindowsMemory.hpp"
#include "Process/ModuleLoader.hpp"
#include "Executable/PeFile.hpp"
#include "Process/Objects.hpp"
#include "Process/AccessCheck.hpp"
#include "Process/WindowsTokens.hpp"
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsIo.hpp"
#include "Process/WindowsWait.hpp"
#include "Process/WindowsMailslots.hpp"
#include "Process/WindowsRegistry.hpp"
#include "Process/WindowsPipes.hpp"
#include "Process/WindowsSockets.hpp"
#include "Process/WindowsProcess.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kUserApc = 0xC0, kSuccess = 0, kTimeout = 0x102, kAlerted = 0x101, kAbandonedWait0 = 0x80, kNotImplemented = 0xC0000002u, kInvalidHandle = 0xC0000008u,
                   kInvalidParameter = 0xC000000Du, kAccessViolation = 0xC0000005u, kTypeMismatch = 0xC0000024u, kNoResources = 0xC000009Au,
                   kNameInvalid = 0xC0000033u, kNameTooLong = 0xC0000106u, kNameExistsStatus = 0x40000000u;
constexpr uint64_t kUnixToNt = 11644473600ull * 10000000ull;
constexpr uint64_t kCurrentProcess = ~uint64_t{0};
constexpr uint32_t kMaxWait = 64;

bool is_object_kind(DescriptorKind k) { return k == DescriptorKind::Event || k == DescriptorKind::Mutex || k == DescriptorKind::Semaphore || k == DescriptorKind::Process || k == DescriptorKind::Timer || k == DescriptorKind::Section || k == DescriptorKind::Job || k == DescriptorKind::ChangeNotification || k == DescriptorKind::DebugObject || k == DescriptorKind::Token || k == DescriptorKind::KeyedEvent || k == DescriptorKind::IoCompletion || k == DescriptorKind::Reserve || k == DescriptorKind::ObjectDirectory || k == DescriptorKind::SymbolicLink || k == DescriptorKind::Port; }
DescriptorKind descriptor_kind_of(ObjectKind k) {
  switch (k) {
    case ObjectKind::Event: return DescriptorKind::Event;
    case ObjectKind::Mutex: return DescriptorKind::Mutex;
    case ObjectKind::Process: return DescriptorKind::Process;
    case ObjectKind::Timer: return DescriptorKind::Timer;
    case ObjectKind::Section: return DescriptorKind::Section;
    case ObjectKind::Job: return DescriptorKind::Job;
    case ObjectKind::ChangeNotification: return DescriptorKind::ChangeNotification;
    case ObjectKind::DebugObject: return DescriptorKind::DebugObject;
    case ObjectKind::Token: return DescriptorKind::Token;
    case ObjectKind::KeyedEvent: return DescriptorKind::KeyedEvent;
    case ObjectKind::IoCompletion: return DescriptorKind::IoCompletion;
    case ObjectKind::Reserve: return DescriptorKind::Reserve;
    case ObjectKind::Directory: return DescriptorKind::ObjectDirectory;
    case ObjectKind::SymbolicLink: return DescriptorKind::SymbolicLink;
    case ObjectKind::Port: return DescriptorKind::Port;
    default: return DescriptorKind::Semaphore;
  }
}

bool slot_of(uint64_t handle, uint64_t* slot) {
  handle &= ~uint64_t{3};      // (the low two bits of a handle are free for the program's own use, as on Windows)
  if (handle == 0 || handle / 4 > Process::kMaxDescriptors) return false;
  *slot = handle / 4 - 1;
  return true;
}

// The name in an UNICODE_STRING, as UTF-8. 0x80000002 (STATUS_DATATYPE_MISALIGNMENT) for characters at an odd address.
uint32_t read_unicode_string(Process& p, uint64_t string_at, char* out, uint32_t capacity, uint32_t* size) {
  *size = 0;
  uint8_t header[16];
  if (!p.read_user(string_at, ByteView(header, sizeof(header)))) return kAccessViolation;
  const uint32_t bytes = uint32_t(header[0]) | uint32_t(header[1]) << 8;
  uint64_t buffer = 0;
  for (int i = 0; i < 8; ++i) buffer |= uint64_t{header[8 + i]} << (8 * i);
  if (bytes == 0) return kSuccess;
  if (buffer % 2 != 0) return 0x80000002u;
  if (bytes % 2 != 0 || bytes >= 65534) return kNameInvalid;
  if (bytes > 2 * ObjectManager::kNameBytes) return kNameTooLong;
  uint8_t units[2 * ObjectManager::kNameBytes];
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
  *size = n;
  return kSuccess;
}

// UTF-8 to the UTF-16 units of a Windows string; the number of units (the text is cut at `capacity`).
uint32_t utf8_to_utf16(StringView text, uint16_t* out, uint32_t capacity) {
  uint32_t n = 0;
  for (uint64_t i = 0; i < text.size() && n < capacity;) {
    const uint8_t c = uint8_t(text[i]);
    uint32_t cp = c, extra = 0;
    if (c >= 0xF0) { cp = c & 7; extra = 3; }
    else if (c >= 0xE0) { cp = c & 15; extra = 2; }
    else if (c >= 0xC0) { cp = c & 31; extra = 1; }
    ++i;
    for (uint32_t k = 0; k < extra && i < text.size(); ++k, ++i) cp = (cp << 6) | (uint8_t(text[i]) & 0x3F);
    if (cp >= 0x10000 && n + 2 <= capacity) { cp -= 0x10000; out[n++] = uint16_t(0xD800 + (cp >> 10)); out[n++] = uint16_t(0xDC00 + (cp & 0x3FF)); }
    else if (cp < 0x10000) out[n++] = uint16_t(cp);
  }
  return n;
}

// The walk of the object manager's names (ObjectPath.hpp) with the objects of the system behind it.
class ManagerLookup final : public objpath::ILookup {
 public:
  explicit ManagerLookup(ObjectManager& objects) : objects_(objects) {}
  bool find(StringView flat, bool case_insensitive, objpath::Entry* out) override { return objects_.lookup_entry(flat, case_insensitive, out); }

 private:
  ObjectManager& objects_;
};

// What OBJECT_ATTRIBUTES (and the walk of its name) says.
struct Named {
  objpath::Resolved res;
  uint32_t same_object = 0;     // an empty name below a handle: the object of that handle itself (its object id), of the descriptor kind below
  DescriptorKind same_kind = DescriptorKind::Closed;
  bool anonymous = false;      // no name: an object nobody can find by name
  bool openif = false;         // OBJ_OPENIF
  StringView flat() const { return anonymous ? StringView() : StringView(res.flat, res.flat_size); }
  objpath::EntryKind exists() const { return anonymous ? objpath::EntryKind::None : res.entry.kind; }
};

constexpr uint32_t kObjOpenIf = 0x80, kObjOpenLink = 0x100, kObjCaseInsensitive = 0x40;

uint32_t resolve_name(Process& p, ObjectManager& objects, uint64_t attributes, bool creating, bool open_link, Named* out) {
  if (attributes == 0) {
    if (!creating) return kInvalidParameter;
    out->anonymous = true;
    return kSuccess;
  }
  uint8_t head[48];
  if (!p.read_user(attributes, ByteView(head, sizeof(head)))) return kAccessViolation;
  auto u32 = [&](int at) { return uint32_t(head[at]) | uint32_t(head[at + 1]) << 8 | uint32_t(head[at + 2]) << 16 | uint32_t(head[at + 3]) << 24; };
  auto u64 = [&](int at) { return uint64_t(u32(at)) | uint64_t(u32(at + 4)) << 32; };
  if (u32(0) != sizeof(head)) return kInvalidParameter;
  const uint64_t root = u64(8), name_at = u64(16);
  const uint32_t flags = u32(24);
  out->openif = (flags & kObjOpenIf) != 0;
  objpath::ResolveRequest request;
  request.case_insensitive = (flags & kObjCaseInsensitive) != 0;
  request.open_link = open_link || (flags & kObjOpenLink) != 0;
  char name[ObjectManager::kNameBytes + 1];
  uint32_t size = 0;
  if (name_at == 0) {
    if (root != 0) return kNameInvalid;
    if (!creating) return objpath::kStatusPathSyntaxBad;
    out->anonymous = true;
    return kSuccess;
  }
  if (const uint32_t status = read_unicode_string(p, name_at, name, ObjectManager::kNameBytes, &size)) return status;
  if (size == 0 && creating) { out->anonymous = true; return kSuccess; }      // (an empty name makes an object without a name, whatever the directory)
  char root_path[objpath::kMaxPath];
  if (root != 0) {
    uint64_t slot = 0;
    if (!slot_of(root, &slot) || p.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
    if (size == 0) {      // (the handle itself is what is opened)
      if (!is_object_kind(p.files().kind(slot))) return kTypeMismatch;
      out->same_object = uint32_t(p.files().token(slot));
      out->same_kind = p.files().kind(slot);
      return kSuccess;
    }
    if (p.files().kind(slot) != DescriptorKind::ObjectDirectory) return kTypeMismatch;
    const uint32_t n = objects.object_text(uint32_t(p.files().token(slot)), root_path, sizeof(root_path));
    if (n == 0) return objpath::kStatusPathNotFound;
    request.has_root = true;
    request.root_path = StringView(root_path, n);
  }
  {      // \Device\NamedPipe (and what is below it) belongs to the pipe file system, not to the object manager's directories: it is of the wrong type
    static const char device[] = "\\Device\\NamedPipe";
    bool same = root == 0 && size >= sizeof(device) - 1 && (size == sizeof(device) - 1 || name[sizeof(device) - 1] == '\\');
    for (uint32_t i = 0; same && i < sizeof(device) - 1; ++i) same = (name[i] | 0x20) == (device[i] | 0x20);
    if (same) return kTypeMismatch;
  }
  request.name_given = true;
  request.name = StringView(name, size);
  ManagerLookup lookup(objects);
  objpath::resolve(lookup, request, &out->res);
  return out->res.status;
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


// The rights a handle of each kind can hold, and what its generic rights stand for (the generic mapping of the object type); false for a kind that is not told apart here.
struct KindAccess { uint32_t valid; security::GenericMapping mapping; };
bool kind_access(DescriptorKind kind, KindAccess* out) {
  switch (kind) {
    case DescriptorKind::Event: *out = {0x1F0003u, {0x20001u, 0x20002u, 0x120000u, 0x1F0003u}}; return true;
    case DescriptorKind::Mutex: *out = {0x1F0001u, {0x20001u, 0x20000u, 0x120000u, 0x1F0001u}}; return true;
    case DescriptorKind::Semaphore: *out = {0x1F0003u, {0x20001u, 0x20002u, 0x120000u, 0x1F0003u}}; return true;
    case DescriptorKind::Timer: *out = {0x1F0003u, {0x20001u, 0x20002u, 0x120000u, 0x1F0003u}}; return true;
    case DescriptorKind::Section: *out = {0xF001Fu, {0x20005u, 0x20002u, 0x20008u, 0xF001Fu}}; return true;
    case DescriptorKind::Job: *out = {0x1F001Fu, {0x20004u, 0x20003u, 0x120000u, 0x1F001Fu}}; return true;
    case DescriptorKind::Process: *out = {0x1FFFFFu, {0x20410u, 0x20BEAu, 0x121001u, 0x1FFFFFu}}; return true;
    case DescriptorKind::Thread: *out = {0x1FFFFFu, {0x20048u, 0x20037u, 0x120000u, 0x1FFFFFu}}; return true;
    case DescriptorKind::Key: *out = {0xF003Fu, {0x20019u, 0x20006u, 0x20019u, 0xF003Fu}}; return true;
    case DescriptorKind::Token: *out = {0xF01FFu, {0x20018u, 0x200E0u, 0x20000u, 0xF01FFu}}; return true;
    case DescriptorKind::KeyedEvent: *out = {0x1F0003u, {0x20001u, 0x20002u, 0x120000u, 0x1F0003u}}; return true;
    case DescriptorKind::Reserve: *out = {0x1F0001u, {0x20001u, 0x20000u, 0x120000u, 0x1F0001u}}; return true;
    case DescriptorKind::Port: *out = {0x1F0001u, {0x20001u, 0x20001u, 0x120000u, 0x1F0001u}}; return true;
    case DescriptorKind::IoCompletion: *out = {0x1F0003u, {0x120001u, 0x120002u, 0x120000u, 0x1F0003u}}; return true;
    case DescriptorKind::DebugObject: *out = {0x1F000Fu, {0x20001u, 0x20002u, 0x120000u, 0x1F000Fu}}; return true;
    case DescriptorKind::File: case DescriptorKind::Directory: case DescriptorKind::Pipe: *out = {0x1F01FFu, {0x120089u, 0x120116u, 0x1200A0u, 0x1F01FFu}}; return true;
    default: return false;
  }
}

// The rights a handle to a synchronization object can have: the object's own ("query state", and "modify state" for all but mutexes), the standard ones and SYNCHRONIZE.
// Generic requests are mapped to them; what is asked for that is not among them is refused. `granted` gets what the handle will hold.
constexpr uint32_t kAccessDeniedStatus = 0xC0000022u, kPrivilegeNotHeld = 0xC0000061u, kSynchronize = 0x100000u, kMaximumAllowed = 0x2000000u, kAccessSystemSecurity = 0x1000000u;
uint32_t map_sync_access(ObjectKind kind, uint32_t desired, bool opening, uint32_t* granted) {
  const bool modifiable = kind != ObjectKind::Mutex;
  const uint32_t all = 0x1F0001u | (modifiable ? 2u : 0u);
  if (kind != ObjectKind::Event && kind != ObjectKind::Mutex && kind != ObjectKind::Semaphore && kind != ObjectKind::Timer && kind != ObjectKind::KeyedEvent) { *granted = all; return kSuccess; }
  uint32_t access = desired;
  if (access & 0x80000000u) access |= 0x20001u;      // GENERIC_READ
  if (access & 0x40000000u) access |= modifiable ? 0x20002u : 0x20000u;      // GENERIC_WRITE
  if (access & 0x20000000u) access |= 0x120000u;      // GENERIC_EXECUTE
  if (access & 0x10000000u) access |= all;      // GENERIC_ALL
  access &= ~0xF0000000u;
  if (access & kMaximumAllowed) access = all;
  if (access & kAccessSystemSecurity) return kPrivilegeNotHeld;
  if (opening && access == 0) return kAccessDeniedStatus;
  if (!opening && access == 0) access = all;      // (a creator that asks for nothing gets everything)
  if (access & ~all) return kAccessDeniedStatus;
  *granted = access;
  return kSuccess;
}
// Records on a new handle which of the rights it does not carry.
void restrict_handle(Process& p, uint64_t handle_at, uint32_t granted, ObjectKind kind) {
  uint64_t handle = 0, slot = 0;
  if (!p.read_user_u64(handle_at, &handle) || !slot_of(handle, &slot)) return;
  const bool modifiable = kind != ObjectKind::Mutex;
  (void)modifiable;
  p.files().set_denied_access(slot, ~granted);
}
// Does the handle (a slot) hold the rights?
bool handle_allows(Process& p, uint64_t handle, uint32_t rights) {
  uint64_t slot = 0;
  return !slot_of(handle, &slot) || (p.files().denied_access(slot) & rights) == 0;
}

// The rights asked for in a duplicate, with the generic ones as the specific ones of the kind (MAXIMUM_ALLOWED asks for all).
uint32_t mapped_access(DescriptorKind kind, uint32_t desired) {
  KindAccess a;
  if (!kind_access(kind, &a)) return desired;
  uint32_t mapped = a.mapping.map(desired);
  if (desired & 0x02000000u) mapped = a.valid;
  return mapped & ~uint32_t{0x02000000u};
}

// The descriptor kept for an object limits what the calling thread may have of it: *desired becomes what it gets (MAXIMUM_ALLOWED asks for all the descriptor allows). 0, or why not.
uint32_t restrict_by_descriptor(Process& p, uint32_t id, DescriptorKind kind, uint32_t* desired) {
  if (const uint32_t gate = impersonation_gate(p)) return gate;
  ObjectManager* objects = system_objects();
  KindAccess a;
  if (!objects || !objects->allocator() || !kind_access(kind, &a)) return kSuccess;
  char key[16];
  const uint32_t n = ObjectManager::object_key('s', id, key);
  const uint32_t size = objects->security_size(StringView(key, n));
  if (size == 0) return kSuccess;
  ByteBuffer sd(*objects->allocator());
  if (!sd.put_zeros(size) || objects->security_get(StringView(key, n), sd.writable_view()) != size) return kAccessDeniedStatus;
  uint32_t granted = 0, status = 0;
  if (!caller_may(p, sd.view(), *desired, a.mapping, &granted, &status)) return status ? status : kAccessDeniedStatus;
  if (*desired & kMaximumAllowed) *desired = granted;
  return kSuccess;
}

// The same for a program other than the caller: its own token (the one of the process) decides.
uint32_t object_access_for_process(uint64_t pid, uint32_t id, DescriptorKind kind, uint32_t desired, uint32_t* granted) {
  ObjectManager* objects = system_objects();
  KindAccess a;
  if (!objects || !objects->allocator() || !kind_access(kind, &a)) return kSuccess;
  char key[16];
  const uint32_t n = ObjectManager::object_key('s', id, key);
  const uint32_t size = objects->security_size(StringView(key, n));
  if (size == 0) return kSuccess;
  ByteBuffer sd(*objects->allocator()), blob(*objects->allocator());
  if (!sd.put_zeros(size) || objects->security_get(StringView(key, n), sd.writable_view()) != size) return kAccessDeniedStatus;
  security::TokenView token;
  if (!process_token_blob(pid, blob) || !token.parse(blob.view())) return kSuccess;
  const security::CheckResult result = security::access_check(sd.view(), token, desired, a.mapping);
  *granted = result.granted;
  return result.status == 0 ? kSuccess : (result.status ? result.status : kAccessDeniedStatus);
}

// Makes the object (or finds the one with the name). `text` is kept with it (a directory's path, a link's target).
uint32_t create_object(Process& p, const SyscallRequest& r, ObjectKind kind, bool manual_reset, bool state, int32_t initial, int32_t maximum, StringView text = StringView(), bool collide = true) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  if (!p.write_user_u64(r.args[0], 0)) return kAccessViolation;      // (no handle, until there is one)
  Named n;
  if (const uint32_t status = resolve_name(p, *objects, r.args[2], true, kind == ObjectKind::SymbolicLink, &n)) return status;
  const bool directory_or_link = n.exists() == objpath::EntryKind::Directory || n.exists() == objpath::EntryKind::Link;
  const bool same_kind = (kind == ObjectKind::Directory && n.exists() == objpath::EntryKind::Directory) || (kind == ObjectKind::SymbolicLink && n.exists() == objpath::EntryKind::Link);
  if (directory_or_link && !same_kind) return kTypeMismatch;
  if (n.exists() == objpath::EntryKind::Object && n.res.entry.object_kind != uint8_t(kind)) return kTypeMismatch;
  uint32_t granted = 0;
  if (const uint32_t denied = map_sync_access(kind, uint32_t(r.args[1]), false, &granted)) return denied;
  const uint64_t owner = p.runtime() ? p.runtime()->current_thread_id() : p.id();
  if (kind == ObjectKind::Directory && !n.anonymous) text = StringView(n.res.path, n.res.path_size);      // (a directory keeps its path: names below it are walked from there)
  ObjectManager::Created c;
  if (same_kind && n.res.entry.builtin) {      // a directory or link that is always there: a handle to it, as an object that has no name of its own
    if (collide && !n.openif) return objpath::kStatusNameCollision;
    c = objects->create(kind, StringView(), false, false, 0, 0, owner, StringView(n.res.entry.text, n.res.entry.text_size));
    if (c.id != 0) c.status = kNameExistsStatus;
  } else {
    c = objects->create(kind, n.flat(), manual_reset, state, initial, maximum, owner, text);
  }
  if (c.id == 0) return c.status;
  if (collide && c.status == kNameExistsStatus && !n.openif) { objects->release(c.id); return objpath::kStatusNameCollision; }
  if (kind == ObjectKind::SymbolicLink && c.status == kNameExistsStatus) c.status = kSuccess;      // (a link that was there is opened, and that is not told)
  const uint32_t given = give_handle(p, *objects, kind, c.id, r.args[0], c.status, wants_inherit(p, r.args[2]));
  if (given == kSuccess || given == c.status) restrict_handle(p, r.args[0], granted, kind);
  return given;
}

uint32_t open_object(Process& p, const SyscallRequest& r, ObjectKind kind) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  if (!p.write_user_u64(r.args[0], 0)) return kAccessViolation;
  Named n;
  if (const uint32_t status = resolve_name(p, *objects, r.args[2], false, kind == ObjectKind::SymbolicLink, &n)) return status;
  uint32_t granted = 0, wanted = uint32_t(r.args[1]);
  ObjectManager::Created c;
  if (n.same_object != 0) {      // an empty name below a handle: a new handle to the object of that handle, if it is of the kind asked for
    if ((kind != ObjectKind::Directory && kind != ObjectKind::SymbolicLink) || n.same_kind != descriptor_kind_of(kind)) return kTypeMismatch;      // (only these two are opened by an empty name below themselves)
    objects->add_ref(n.same_object);
    c.id = n.same_object;
  } else {
  const objpath::EntryKind want = kind == ObjectKind::Directory ? objpath::EntryKind::Directory : kind == ObjectKind::SymbolicLink ? objpath::EntryKind::Link : objpath::EntryKind::Object;
  if (n.res.entry.kind == objpath::EntryKind::None) return objpath::kStatusNameNotFound;
  if (n.res.entry.kind != want || (want == objpath::EntryKind::Object && n.res.entry.object_kind != uint8_t(kind))) return kTypeMismatch;
  if (n.res.entry.builtin) {      // (a handle to a place that is always there)
    const bool root_dir = kind == ObjectKind::Directory;
    c = objects->create(kind, StringView(), false, false, 0, 0, 0, root_dir ? StringView(n.res.path, n.res.path_size) : StringView(n.res.entry.text, n.res.entry.text_size));
  } else {
    c = objects->open(kind, n.flat());
  }
  }
  if (c.id == 0) return c.status;
  if (const uint32_t refused = restrict_by_descriptor(p, c.id, descriptor_kind_of(kind), &wanted)) { objects->release(c.id); return refused; }
  if (const uint32_t denied = map_sync_access(kind, wanted, true, &granted)) { objects->release(c.id); return denied; }
  const uint32_t given = give_handle(p, *objects, kind, c.id, r.args[0], kSuccess, wants_inherit(p, r.args[2]));
  if (given == kSuccess) restrict_handle(p, r.args[0], granted, kind);
  return given;
}

// The object a handle names, of the kind asked for. 0 if it is none.
uint32_t object_of(Process& p, uint64_t handle, DescriptorKind kind) {
  uint64_t slot = 0;
  if (!slot_of(handle, &slot) || p.files().kind(slot) != kind) return 0;
  return uint32_t(p.files().token(slot));
}

// Why a handle is no use for a call: it names an object of another kind (STATUS_OBJECT_TYPE_MISMATCH) or no object (STATUS_INVALID_HANDLE).
uint32_t bad_object_status(Process& p, uint64_t handle) {
  uint64_t slot = 0;
  if (handle == ~uint64_t{0} || handle == ~uint64_t{1}) return kTypeMismatch;
  return slot_of(handle, &slot) && p.files().kind(slot) != DescriptorKind::Closed ? kTypeMismatch : kInvalidHandle;
}

uint32_t put_previous(Process& p, uint64_t address, int32_t value) {
  if (address == 0) return kSuccess;
  const uint8_t raw[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
  return p.write_user(address, ConstByteView(raw, 4)) ? kSuccess : kAccessViolation;
}

// What can be waited for.
struct Waitable {
  enum Type { Invalid, Thread, Object, Mailslot, Never, Pipe, Always } type = Invalid;
  uint64_t thread = 0;      // (Mailslot: the descriptor slot)
  uint32_t object = 0;
};

bool resolve(Process& p, uint64_t handle, Waitable* w) {
  uint64_t slot = 0;
  if (!slot_of(handle, &slot)) return false;
  const DescriptorKind k = p.files().kind(slot);
  if (k == DescriptorKind::Thread) {
    if (p.files().denied_access(slot) & 0x100000u) return false;
    w->type = Waitable::Thread;
    w->thread = p.files().token(slot);
    return true;
  }
  if (k == DescriptorKind::Pipe) { w->type = Waitable::Pipe; w->thread = slot; return true; }      // (a pipe handle is signalled, as a file object with no call under way is)
  if (k == DescriptorKind::Mailslot) { w->type = Waitable::Mailslot; w->thread = slot; return true; }
  if (k == DescriptorKind::File || k == DescriptorKind::Directory || k == DescriptorKind::ConsoleInput || k == DescriptorKind::ConsoleOutput || k == DescriptorKind::ConsoleError ||
      k == DescriptorKind::Null || k == DescriptorKind::Zero || k == DescriptorKind::Random) {      // a file object is signalled when no call on it is under way
    w->type = Waitable::Always;
    return true;
  }
  if (is_object_kind(k)) {
    if (p.files().denied_access(slot) & 0x100000u) return false;      // (no SYNCHRONIZE right)
    w->type = Waitable::Object;
    w->object = uint32_t(p.files().token(slot));
    return true;
  }
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
    if (items[i].type == Waitable::Never) return false;
    if (items[i].type == Waitable::Always) return true;
    if (items[i].type == Waitable::Pipe) return !pipe_handle_gone_locked(p, items[i].thread);
    if (items[i].type == Waitable::Mailslot) return mailslot_read_ready_locked(p, items[i].thread);
    return objects->ready_locked(items[i].object, waiter);
  };
  sync.lock();
  for (;;) {
    if (objects->io_ready() != 0) { sync.unlock(); io_reap(p); sync.lock(); }      // (calls that finished while this one waited are applied to the program: a wait for their event ends)
    if (alertable && runtime) {      // an APC queued for this thread (or a timer of its that fired) ends the wait; the caller's library runs it
      ObjectManager::TimerApc fired[8];
      uint32_t fired_count = 0;
      objects->take_timer_apcs_locked(waiter, fired, 8, &fired_count);
      Process::ThreadRecord* me = p.find_thread(waiter);
      for (uint32_t i = 0; me && i < fired_count; ++i) Process::queue_apc(*me, Process::ThreadRecord::Apc{fired[i].routine, fired[i].context, fired[i].time & 0xFFFFFFFFu, fired[i].time >> 32});
      if (me && me->apc_count > 0) { sync.unlock(); return kUserApc; }
      if (me && me->alert_pending) { me->alert_pending = false; sync.unlock(); return kAlerted; }      // (NtAlertThread)
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
      for (uint32_t i = all ? 0 : uint32_t(chosen); i < (all ? count : uint32_t(chosen) + 1); ++i) {
        if (items[i].type == Waitable::Mailslot && mailslot_complete_read(p, items[i].thread) != kSuccess) return kAccessViolation;      // (the read the wait was for)
      }
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
    if (runtime && runtime->park_wanted()) {      // asked to suspend while it waits: it stops here (its registers can be had) and goes on waiting when it is resumed
      sync.unlock();      // (only then: dropping the lock here would let a wake-up fall between the look above and the wait below)
      const int parked = runtime->park_point();
      sync.lock();
      if (parked == 2) { sync.unlock(); return kAlerted; }
      continue;      // (look again: anything may have changed while the lock was let go)
    }
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

// Ports (WindowsPorts.cpp). A port is an object of the manager like the others; these three make one by its OBJECT_ATTRIBUTES, find one by a UNICODE_STRING name, and give
// the process a handle for an object it holds a reference of.
uint32_t port_create_object(Process& p, uint64_t handle_at, uint64_t attributes, uint32_t* id) {
  SyscallRequest forward;
  forward.args[0] = handle_at;
  forward.args[1] = 0x1F0001;
  forward.args[2] = attributes;
  if (const uint32_t status = create_object(p, forward, ObjectKind::Port, false, false, 0, 0, StringView(), true)) return status;
  uint64_t handle = 0, slot = 0;
  if (!p.read_user_u64(handle_at, &handle) || !slot_of(handle, &slot)) return kAccessViolation;
  *id = uint32_t(p.files().token(slot));
  return kSuccess;
}
uint32_t port_find_by_name(Process& p, uint64_t unicode_name, uint32_t* id) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  char name[ObjectManager::kNameBytes + 1];
  uint32_t size = 0;
  if (const uint32_t status = read_unicode_string(p, unicode_name, name, ObjectManager::kNameBytes, &size)) return status;
  if (size == 0) return kNameInvalid;
  objpath::ResolveRequest request;
  request.case_insensitive = true;
  request.name_given = true;
  request.name = StringView(name, size);
  ManagerLookup lookup(*objects);
  objpath::Resolved res;
  objpath::resolve(lookup, request, &res);
  if (res.status != kSuccess) return res.status;
  if (res.entry.kind == objpath::EntryKind::None) return objpath::kStatusNameNotFound;
  if (res.entry.kind != objpath::EntryKind::Object || res.entry.object_kind != uint8_t(ObjectKind::Port)) return kTypeMismatch;
  const ObjectManager::Created c = objects->open(ObjectKind::Port, StringView(res.flat, res.flat_size));
  if (c.id == 0) return c.status;
  *id = c.id;
  return kSuccess;
}
uint32_t port_give_handle(Process& p, uint32_t id, uint64_t handle_at) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  return give_handle(p, *objects, ObjectKind::Port, id, handle_at, kSuccess, false);
}
uint32_t port_object_of(Process& p, uint64_t handle, uint32_t* id) {
  uint64_t slot = 0;
  if (!slot_of(handle, &slot) || p.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  if (p.files().kind(slot) != DescriptorKind::Port) return kTypeMismatch;
  *id = uint32_t(p.files().token(slot));
  return kSuccess;
}
// A port object without a name, for the communication ports a connection makes; the caller holds the reference the call gives.
uint32_t port_make_anonymous(uint32_t* id) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  const ObjectManager::Created c = objects->create(ObjectKind::Port, StringView(), false, false, 0, 0, 0);
  if (c.id == 0) return c.status;
  *id = c.id;
  return kSuccess;
}

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
  if (!handle_allows(p, r.args[0], 2)) return kAccessDeniedStatus;
  const uint32_t id = object_of(p, r.args[0], DescriptorKind::Event);
  if (!objects || !id) return objects ? bad_object_status(p, r.args[0]) : kNotImplemented;
  int32_t previous = 0;
  const uint32_t status = objects->set_event(id, &previous);
  return status != kSuccess ? status : put_previous(p, r.args[1], previous);
}
uint32_t nt_reset_event(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  if (!handle_allows(p, r.args[0], 2)) return kAccessDeniedStatus;
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
  if (!handle_allows(p, r.args[0], 2)) return kAccessDeniedStatus;
  const uint32_t id = object_of(p, r.args[0], DescriptorKind::Event);
  if (!objects || !id) return objects ? bad_object_status(p, r.args[0]) : kNotImplemented;
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
  if (!objects || !id) return objects ? bad_object_status(p, r.args[0]) : kNotImplemented;
  int32_t previous = 0;
  const uint32_t status = objects->release_mutex(id, p.runtime() ? p.runtime()->current_thread_id() : p.id(), &previous);
  return status != kSuccess ? status : put_previous(p, r.args[1], 1 - previous);      // (the count of a mutant: 1 when free, one less for each time it is held)
}

// NtCreateSemaphore(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, LONG InitialCount, LONG MaximumCount)
uint32_t nt_create_semaphore(Process& p, const SyscallRequest& r) {
  uint64_t maximum = 0;
  if (!argument(p, r, 4, &maximum)) return kAccessViolation;
  return create_object(p, r, ObjectKind::Semaphore, false, false, int32_t(uint32_t(r.args[3])), int32_t(uint32_t(maximum)));
}
uint32_t nt_open_semaphore(Process& p, const SyscallRequest& r) { return open_object(p, r, ObjectKind::Semaphore); }
uint32_t nt_release_semaphore(Process& p, const SyscallRequest& r) {
  if (!handle_allows(p, r.args[0], 2)) return kAccessDeniedStatus;
  ObjectManager* objects = system_objects();
  const uint32_t id = object_of(p, r.args[0], DescriptorKind::Semaphore);
  if (!objects || !id) return objects ? bad_object_status(p, r.args[0]) : kNotImplemented;
  int32_t previous = 0;
  const uint32_t status = objects->release_semaphore(id, int32_t(uint32_t(r.args[1])), &previous);
  return status != kSuccess ? status : put_previous(p, r.args[2], previous);
}

uint32_t nt_wait_for_single_object(Process& p, const SyscallRequest& r) {
  Waitable w;
  if (r.args[0] == ~uint64_t{0} || r.args[0] == ~uint64_t{1}) w.type = Waitable::Never;      // the pseudo handles of the running process and thread: signalled when it ends, and it is running
  else if (!resolve(p, r.args[0], &w)) {
    uint64_t slot = 0;
    if (slot_of(r.args[0], &slot) && (is_object_kind(p.files().kind(slot)) || p.files().kind(slot) == DescriptorKind::Thread) && (p.files().denied_access(slot) & 0x100000u)) return kAccessDeniedStatus;      // (no SYNCHRONIZE right)
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

uint32_t take_object_handle_from(Process& source, IProcessRuntime::RemoteOp& op) {
  uint64_t slot = 0;
  if (!slot_of(op.address, &slot) || source.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  const DescriptorKind kind = source.files().kind(slot);
  const uint64_t token = source.files().token(slot);
  if (kind != DescriptorKind::Pipe && !is_object_kind(kind)) return kNotImplemented;      // (the kinds with a count in the object manager or the pipes' ends)
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  op.type = uint32_t(kind);
  op.argument = token;
  op.protect = source.files().denied_access(slot);
  if (kind == DescriptorKind::Pipe) pipe_token_add(token);
  else objects->add_ref(uint32_t(token));
  if (op.suspended && !source.files().protected_from_close(slot)) source.close_descriptor(slot);      // DUPLICATE_CLOSE_SOURCE
  return kSuccess;
}

namespace {

// NtDuplicateObject from a program other than the caller into the caller.
uint32_t duplicate_from_other(Process& p, const SyscallRequest& r, uint64_t target_at, uint64_t desired_access, uint64_t attributes, uint64_t options) {
  bool exited = false;
  uint32_t code = 0;
  uint64_t pid = 0;
  if (!process_handle_info(p, r.args[0], &exited, &code, &pid)) return kInvalidHandle;
  if (exited) return 0xC000010Au;      // STATUS_PROCESS_IS_TERMINATING
  if (!p.runtime()) return kNotImplemented;
  IProcessRuntime::RemoteOp op;
  op.kind = IProcessRuntime::RemoteOp::TakeHandle;
  op.address = r.args[1];
  op.suspended = (options & 1) != 0;
  const uint32_t status = p.runtime()->remote_operation(pid, op);
  if (status != kSuccess) return status;
  if (!(options & 2) && caller_reduced(p) && is_object_kind(DescriptorKind(op.type))) {      // a thread that impersonates is held to the descriptor of the object
    KindAccess a;
    if (kind_access(DescriptorKind(op.type), &a) && (mapped_access(DescriptorKind(op.type), uint32_t(desired_access)) & a.valid & op.protect)) {
      if (ObjectManager* objects = system_objects()) objects->release(uint32_t(op.argument));
      return kAccessDeniedStatus;
    }
  }
  const uint32_t denied = (options & 2) ? op.protect : (op.protect | ~mapped_access(DescriptorKind(op.type), uint32_t(desired_access)));
  uint64_t handle = 0;
  uint32_t given = give_object_handle_in(p, op.type, op.argument, denied, (options & 4) ? false : (attributes & 2) != 0, &handle);
  if (given == kSuccess && target_at != 0 && !p.write_user_u64(target_at, handle)) { p.close_descriptor(handle / 4 - 1); return kAccessViolation; }
  if (given != kSuccess) {      // (the count the other program made for the copy)
    if (DescriptorKind(op.type) == DescriptorKind::Pipe) pipe_token_release(op.argument);
    else if (ObjectManager* objects = system_objects()) objects->release(uint32_t(op.argument));
  }
  return given;
}

// NtDuplicateObject into a program other than the caller (only the kinds of the object manager, which count their references).
// A handle of one other program copied into a third (or the same other) program: taken out of the first, then put into the second.
uint32_t duplicate_between_others(Process& p, const SyscallRequest& r, uint64_t target_at, uint64_t desired_access, uint64_t attributes, uint64_t options) {
  bool exited = false;
  uint32_t code = 0;
  uint64_t source_pid = 0, target_pid = 0;
  if (!process_handle_info(p, r.args[0], &exited, &code, &source_pid) || exited) return kInvalidHandle;
  if (!process_handle_info(p, r.args[2], &exited, &code, &target_pid) || exited) return kInvalidHandle;
  if (!p.runtime()) return kNotImplemented;
  IProcessRuntime::RemoteOp take;
  take.kind = IProcessRuntime::RemoteOp::TakeHandle;
  take.address = r.args[1];
  take.suspended = (options & 1) != 0;
  if (const uint32_t status = p.runtime()->remote_operation(source_pid, take)) return status;
  ObjectManager* objects = system_objects();
  if (!objects || !is_object_kind(DescriptorKind(take.type))) return kNotImplemented;
  if (!(options & 2) && caller_is_impersonating(p) && caller_reduced(p)) {
    KindAccess a;
    if (kind_access(DescriptorKind(take.type), &a) && (mapped_access(DescriptorKind(take.type), uint32_t(desired_access)) & a.valid & take.protect)) { objects->release(uint32_t(take.argument)); return kAccessDeniedStatus; }
  }
  IProcessRuntime::RemoteOp put;
  put.kind = IProcessRuntime::RemoteOp::Duplicate;
  put.type = take.type;
  put.argument = take.argument;
  put.protect = (options & 2) ? take.protect : (take.protect | ~mapped_access(DescriptorKind(take.type), uint32_t(desired_access)));
  put.suspended = (options & 4) ? false : (attributes & 2) != 0;
  const uint32_t status = p.runtime()->remote_operation(target_pid, put);
  if (status != kSuccess) { objects->release(uint32_t(take.argument)); return status; }
  if (target_at != 0 && !p.write_user_u64(target_at, put.address)) return kAccessViolation;
  return kSuccess;
}

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
  const bool pipe = kind == DescriptorKind::Pipe;      // (a pipe handle is copied with its token: the other program holds the end too)
  if ((!is_object_kind(kind) && !pipe) || !objects) return kNotImplemented;
  const uint64_t token = p.files().token(slot);
  const uint32_t id = uint32_t(token);
  const uint32_t original = p.files().denied_access(slot);
  IProcessRuntime::RemoteOp op;
  op.kind = IProcessRuntime::RemoteOp::Duplicate;
  op.type = uint32_t(kind);
  op.argument = pipe ? token : id;
  op.protect = (options & 2) ? original : (original | ~mapped_access(kind, uint32_t(desired_access)));
  if (!(options & 2)) {      // a program that runs under a restricted token only gets the rights the handle holds
    ByteBuffer blob(*objects->allocator());
    security::TokenView token;
    KindAccess a;
    if (process_token_blob(pid, blob) && token.parse(blob.view()) && token.is_reduced() && kind_access(kind, &a) &&
        (mapped_access(kind, uint32_t(desired_access)) & a.valid & original)) return kAccessDeniedStatus;
  }
  op.suspended = (options & 4) ? p.files().inheritable(slot) : (attributes & 2) != 0;
  if (pipe) pipe_token_add(token); else objects->add_ref(id);
  const uint32_t status = p.runtime()->remote_operation(pid, op);
  if (status != kSuccess) { if (pipe) pipe_token_release(token); else objects->release(id); return status; }
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
  if (!source_here) return target_here ? duplicate_from_other(p, r, target_at, desired_access, attributes, options) : duplicate_between_others(p, r, target_at, desired_access, attributes, options);
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
    p.files().set_handle_flags(uint64_t(slot_for_process), (options & 4) ? false : (attributes & 2) != 0, false);      // (the pseudo handle has no attributes to keep)
    return kSuccess;
  }
  uint64_t slot = 0;
  if (!slot_of(r.args[1], &slot) || p.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  if (!(options & 2) && p.files().kind(slot) == DescriptorKind::File) {      // a file handle cannot be copied with rights the original does not have
    if (const OpenObject* opened = p.files().object(slot)) {
      auto map_file_access = [](uint32_t a) {
        if (a & 0x80000000u) a |= 0x120089u;      // GENERIC_READ
        if (a & 0x40000000u) a |= 0x120116u;      // GENERIC_WRITE
        if (a & 0x20000000u) a |= 0x1200A0u;      // GENERIC_EXECUTE
        if (a & 0x10000000u) a |= 0x1F01FFu;      // GENERIC_ALL
        return a & ~0xF0000000u;
      };
      if (map_file_access(uint32_t(desired_access)) & ~map_file_access(opened->granted_access)) return 0xC0000022u;      // STATUS_ACCESS_DENIED
    }
  }
  if ((options & 1) && !p.files().protected_from_close(slot)) {      // DUPLICATE_CLOSE_SOURCE into this very process: the source goes first, so the handle keeps its number
    if (target_at != 0 && !p.write_user_u64(target_at, r.args[1])) return kAccessViolation;
    const uint32_t original = p.files().denied_access(slot);
    p.files().set_denied_access(slot, (options & 2) ? original : (p.files().kind(slot) == DescriptorKind::File || p.files().kind(slot) == DescriptorKind::Directory ? original : 0u) | ~mapped_access(p.files().kind(slot), uint32_t(desired_access)));
    p.files().set_handle_flags(slot, (options & 4) ? p.files().inheritable(slot) : (attributes & 2) != 0, (options & 4) != 0 && p.files().protected_from_close(slot));
    return kSuccess;
  }
  if (!(options & 2) && caller_reduced(p)) {      // a thread that impersonates may not copy a handle with more rights than the handle holds
    KindAccess a;
    if (kind_access(p.files().kind(slot), &a) && (mapped_access(p.files().kind(slot), uint32_t(desired_access)) & a.valid & p.files().denied_access(slot))) return kAccessDeniedStatus;
  }
  if (!(options & 2) && p.files().kind(slot) == DescriptorKind::Process) {      // more rights to a process than its handle holds: what its descriptor allows
    bool exited = false;
    uint32_t code = 0;
    uint64_t pid = 0;
    ObjectManager* objects = system_objects();
    uint32_t granted = 0;
    if (objects && objects->query_process(uint32_t(p.files().token(slot)), &exited, &code, &pid) && pid < (uint64_t{1} << 20)) {
      KindAccess a;
      if (kind_access(DescriptorKind::Process, &a) && (mapped_access(DescriptorKind::Process, uint32_t(desired_access)) & a.valid & p.files().denied_access(slot)))
        if (const uint32_t refused = open_with_descriptor(p, 'p', pid, uint32_t(desired_access), kProcessMapping, &granted)) return refused;
    }
  }
  if (!(options & 2) && !(desired_access & kMaximumAllowed) && p.files().kind(slot) == DescriptorKind::Section) {
    // A section handle asked for with rights its source does not have: only what the section's descriptor grants (Windows checks the object then). Without one here the
    // rights stay those of the source: Chromium tells a read-only region from a writable one this way (its sections have an empty DACL, which grants nothing more).
    const uint32_t more = mapped_access(DescriptorKind::Section, uint32_t(desired_access)) & p.files().denied_access(slot) & ~uint32_t{0x02000000u};
    if (more) {
      uint32_t wanted = uint32_t(desired_access);
      char key[16];
      ObjectManager* objects = system_objects();
      const uint32_t n = ObjectManager::object_key('s', uint32_t(p.files().token(slot)), key);
      if (!objects || objects->security_size(StringView(key, n)) == 0) return kAccessDeniedStatus;
      if (const uint32_t refused = restrict_by_descriptor(p, uint32_t(p.files().token(slot)), DescriptorKind::Section, &wanted)) return refused;
    }
  }
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
    p.files().set_denied_access(uint64_t(copy), (options & 2) ? original : (p.files().kind(slot) == DescriptorKind::File || p.files().kind(slot) == DescriptorKind::Directory ? original : 0u) | ~mapped_access(p.files().kind(slot), uint32_t(desired_access)));
  }
  const bool same = (options & 4) != 0;      // DUPLICATE_SAME_ATTRIBUTES; otherwise the handle is inheritable if the attributes say OBJ_INHERIT
  p.files().set_handle_flags(uint64_t(copy), same ? p.files().inheritable(slot) : (attributes & 2) != 0, same && p.files().protected_from_close(slot));
  if ((options & 1) && !p.files().protected_from_close(slot)) p.close_descriptor(slot);      // DUPLICATE_CLOSE_SOURCE (a handle that cannot be closed stays)
  return kSuccess;
}

// A sleep that an APC can end: the NtDelayExecution of a thread that asked to be alertable.
uint32_t alertable_delay(Process& p, uint64_t nanoseconds) {
  const uint32_t status = wait_for(p, nullptr, 0, false, nanoseconds, true);
  return status == kTimeout ? kSuccess : status;
}

uint32_t nt_alert_thread_by_thread_id(Process& p, const SyscallRequest& r) {
  Process::ThreadRecord* record = p.find_thread(r.args[0]);
  if (!record || record->finished) return 0xC000000Bu;      // STATUS_INVALID_CID
  record->alerted = true;
  if (p.runtime()) p.runtime()->notify();
  if (ObjectManager* objects = system_objects()) objects->notify();
  return kSuccess;
}

uint32_t nt_wait_for_alert_by_thread_id(Process& p, const SyscallRequest& r) {
  bool fault = false;
  const uint64_t timeout = timeout_ns_of(p, r.args[1], &fault);
  if (fault) return kAccessViolation;
  Process::ThreadRecord* me = p.find_thread(p.runtime() ? p.runtime()->current_thread_id() : p.id());
  if (!me) return kInvalidHandle;
  return wait_until(p, timeout, [&]() -> uint64_t {
    if (!me->alerted) return kWaitPending;
    me->alerted = false;
    return 0x101u;      // STATUS_ALERTED
  });
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
  if (!handle_allows(p, r.args[0], 2)) return kAccessDeniedStatus;
  uint64_t resume = 0, period = 0, previous_at = 0;
  if (!argument(p, r, 4, &resume) || !argument(p, r, 5, &period) || !argument(p, r, 6, &previous_at)) return kAccessViolation;
  ObjectManager* objects = system_objects();
  const uint32_t id = object_of(p, r.args[0], DescriptorKind::Timer);
  if (!objects || !id) return objects ? bad_object_status(p, r.args[0]) : kNotImplemented;
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
  if (!objects || !id) return objects ? bad_object_status(p, r.args[0]) : kNotImplemented;
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
  if (!objects || !id) return objects ? bad_object_status(p, r.args[0]) : kNotImplemented;
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

// What NtCreateSection(SEC_IMAGE) says about the file: 0 for a PE32+ x86-64 or PE32 i386 image (the ones Windows maps on this CPU; `pe` is then parsed), else the NT status.
uint32_t image_section_status(ConstByteView file, exe::PeFile& pe) {
  if (const uint32_t header = image_header_status(file)) return header;
  if (!pe.parse(file)) return 0xC000007Bu;
  const bool good_pair = (pe.is_pe32_plus() && pe.machine() == exe::kPeMachineAmd64) || (!pe.is_pe32_plus() && pe.machine() == 0x14C);
  return good_pair ? 0 : 0xC000007Bu;
}

// NtCreateSection(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PLARGE_INTEGER MaximumSize, ULONG PageProtection, ULONG AllocationAttributes, HANDLE FileHandle): memory to map into
// address spaces, shared by name; with a file handle it is the file's contents (written back by FlushViewOfFile, when the last view goes and when the section does).
uint32_t section_rights(uint32_t access);
uint32_t nt_create_section(Process& p, const SyscallRequest& r) {
  uint64_t page_protection = 0, attributes = 0, file_handle = 0;
  if (!argument(p, r, 4, &page_protection) || !argument(p, r, 5, &attributes) || !argument(p, r, 6, &file_handle)) return kAccessViolation;
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  const uint32_t page = uint32_t(page_protection);
  bool copy = false;
  if (protection_of_page(page, &copy) == 0 || page == kPageExecute) return kInvalidParameter;      // (no access and execute-only sections do not exist)
  if (!p.write_user_u64(r.args[0], 0)) return kAccessViolation;
  Named named;
  if (const uint32_t status = resolve_name(p, *objects, r.args[2], true, false, &named)) return status;
  if (named.exists() == objpath::EntryKind::Directory || named.exists() == objpath::EntryKind::Link) return kTypeMismatch;
  uint64_t size = 0;
  if (r.args[3] != 0 && !p.read_user_u64(r.args[3], &size)) return kAccessViolation;
  filesystem::IFileSystem* fs = nullptr;
  String host(p.files().allocator());
  uint64_t file_size = 0;
  if (file_handle != 0 && file_handle != ~uint64_t{0}) {
    uint64_t slot = 0;
    if (!slot_of(file_handle, &slot) || p.files().kind(slot) != DescriptorKind::File) return kInvalidHandle;
    OpenObject* file = p.files().object(slot);
    if (!file || !p.files().host_path_of(file->path.view(), &host)) return kInvalidHandle;
    fs = p.files().file_system();
    if (!file->readable || file->no_data_access) return 0xC0000022u;      // a file that cannot be read cannot be mapped
    if (!file->writable && (page == kPageReadWrite || page == kPageExecuteReadWrite || page == kPageExecuteRead || page == kPageExecuteWriteCopy)) return 0xC0000022u;      // STATUS_ACCESS_DENIED
    file_size = p.files().size_of(*file);
    const bool section_writes = page == kPageReadWrite || page == kPageExecuteReadWrite;      // only a section that can be written grows the file
    if (size > file_size && !section_writes && !(uint32_t(attributes) & 0x1000000u)) return file->writable ? 0xC0000040u /* STATUS_SECTION_TOO_BIG */ : kStatusSectionProtection;
    if (size == 0 && file_size == 0) return (uint32_t(attributes) & 0x1000000u) ? 0xC0000020u /* STATUS_INVALID_FILE_FOR_SECTION */ : 0xC000011Eu /* STATUS_MAPPED_FILE_SIZE_ZERO */;
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
  const ObjectManager::Created c = objects->create_section(named.flat(), (flags & kSecImage) ? 0 : size, page, reported, fs, host.view());
  if (c.id != 0 && c.status == kNameExistsStatus && !named.openif) { objects->release(c.id); return objpath::kStatusNameCollision; }
  if (c.id == 0) return (fs && c.status == kNoResources) ? 0xC0000040u /* STATUS_SECTION_TOO_BIG: more than can be held */ : c.status;
  if (fs && !(flags & kSecImage)) objects->section_set_reported_size(c.id, size != 0 ? size : file_size);      // (a section of a file has the size it was asked for, not the pages')
  if (flags & kSecImage) {      // the file has to be a PE image this OS can run
    uint8_t* image = nullptr;
    uint64_t image_size = 0;
    SharedMemory* owner = nullptr;
    exe::PeFile pe(p.files().allocator());
    ObjectManager::SectionInfo info;
    if (!objects->section_memory(c.id, &image, &image_size, &owner) || !objects->section_info(c.id, &info)) { objects->release(c.id); return 0xC000007Bu; }
    uint32_t header = image_section_status(ConstByteView(image, info.size), pe);
    if (header != 0) {
      objects->release(c.id);
      return header;
    }
    uint64_t image_rounded = 0;
    if (!AddressSpace::round_up(pe.size_of_image(), &image_rounded) || size > image_rounded) { objects->release(c.id); return 0xC0000040u; }      // STATUS_SECTION_TOO_BIG
    objects->section_set_reported_size(c.id, size != 0 ? size : image_rounded);
  }
  const uint32_t given = give_handle(p, *objects, ObjectKind::Section, c.id, r.args[0], c.status, wants_inherit(p, r.args[2]));
  uint64_t handle = 0, slot = 0;
  if ((given == kSuccess || given == c.status) && p.read_user_u64(r.args[0], &handle) && slot_of(handle, &slot)) p.files().set_denied_access(slot, ~section_rights(uint32_t(r.args[1])));      // (the handle holds the rights it was made for)
  return given;
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
  if (length < 64) return 0xC0000004u;
  if (!(info.attributes & 0x1000000u)) return length > (uint64_t{1} << 32) ? kAccessViolation : 0xC0000049u;      // STATUS_SECTION_NOT_IMAGE
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
// NtCreateDebugObject(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG Flags): the flags are DEBUG_KILL_ON_CLOSE (1) only.
uint32_t nt_create_debug_object(Process& p, const SyscallRequest& r) {
  const uint32_t flags = uint32_t(r.args[3]);
  if (flags & ~uint32_t{1}) return kInvalidParameter;
  const uint32_t status = create_object(p, r, ObjectKind::DebugObject, false, false, 0, 0);
  if (status != kSuccess) return status;
  uint64_t handle = 0, slot = 0;
  if (p.read_user_u64(r.args[0], &handle) && slot_of(handle, &slot)) debug_note_created(uint32_t(p.files().token(slot)), flags);
  return status;
}
uint32_t nt_open_job_object(Process& p, const SyscallRequest& r) { return open_object(p, r, ObjectKind::Job); }
// NtAllocateReserveObject(PHANDLE, POBJECT_ATTRIBUTES, MEMORY_RESERVE_TYPE: 0 user APC, 1 I/O completion)
uint32_t nt_allocate_reserve_object(Process& p, const SyscallRequest& r) {
  if (r.args[2] > 1) return kInvalidParameter;
  SyscallRequest forward = r;
  forward.args[2] = r.args[1];      // (create_object takes the attributes from the third argument)
  return create_object(p, forward, ObjectKind::Reserve, false, false, 0, int32_t(r.args[2]));
}
// The reserve object a handle names, for an APC that uses it (kSuccess and *id; STATUS_INVALID_HANDLE; STATUS_OBJECT_TYPE_MISMATCH; STATUS_INVALID_PARAMETER_2 when it is in use).
uint32_t reserve_take_handle(Process& p, uint64_t handle, uint32_t* id) {
  uint64_t slot = 0;
  if (!slot_of(handle, &slot) || p.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  if (p.files().kind(slot) != DescriptorKind::Reserve) return 0xC0000024u;
  ObjectManager* objects = system_objects();
  if (!objects) return kInvalidHandle;
  *id = uint32_t(p.files().token(slot));
  return objects->reserve_take(*id);
}
void reserve_give_back(uint32_t id) { if (ObjectManager* objects = system_objects()) objects->reserve_give_back(id); }
// Keyed events and I/O completion ports are made and found by name here; the waiting and queueing of them is not done yet.
uint32_t nt_create_keyed_event(Process& p, const SyscallRequest& r) { return r.args[3] != 0 ? kInvalidParameter : create_object(p, r, ObjectKind::KeyedEvent, false, false, 0, 0); }      // (PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG Flags)
uint32_t nt_open_keyed_event(Process& p, const SyscallRequest& r) { return open_object(p, r, ObjectKind::KeyedEvent); }
uint32_t nt_create_io_completion(Process& p, const SyscallRequest& r) { return create_object(p, r, ObjectKind::IoCompletion, false, false, 0, 0); }      // (PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG Count)
uint32_t nt_open_io_completion(Process& p, const SyscallRequest& r) { return open_object(p, r, ObjectKind::IoCompletion); }

// --- directories and symbolic links of the object namespace (see ObjectPath.hpp)
uint32_t nt_create_directory_object(Process& p, const SyscallRequest& r) { return create_object(p, r, ObjectKind::Directory, false, false, 0, 0, StringView(), true); }      // (PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES)
uint32_t nt_open_directory_object(Process& p, const SyscallRequest& r) { return open_object(p, r, ObjectKind::Directory); }
uint32_t nt_open_symbolic_link_object(Process& p, const SyscallRequest& r) { return open_object(p, r, ObjectKind::SymbolicLink); }
// (PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PUNICODE_STRING LinkTarget)
uint32_t nt_create_symbolic_link_object(Process& p, const SyscallRequest& r) {
  uint8_t header[4];
  if (r.args[2] == 0 || r.args[3] == 0 || !p.read_user(r.args[3], ByteView(header, sizeof(header)))) return kAccessViolation;
  const uint32_t length = uint32_t(header[0]) | uint32_t(header[1]) << 8, maximum = uint32_t(header[2]) | uint32_t(header[3]) << 8;
  if (length == 0 || maximum < length) return kInvalidParameter;
  char target[ObjectManager::kNameBytes + 1];
  uint32_t size = 0;
  if (const uint32_t status = read_unicode_string(p, r.args[3], target, ObjectManager::kNameBytes, &size)) return status;
  return create_object(p, r, ObjectKind::SymbolicLink, false, false, 0, 0, StringView(target, size), true);
}

uint32_t typed_handle(Process& p, uint64_t handle, DescriptorKind kind, uint32_t* id) {
  uint64_t slot = 0;
  if (!slot_of(handle, &slot) || p.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  if (p.files().kind(slot) != kind) return kTypeMismatch;
  *id = uint32_t(p.files().token(slot));
  return kSuccess;
}

// NtQuerySymbolicLinkObject(Handle, PUNICODE_STRING LinkTarget, PULONG ReturnedLength): the target as the characters of the string (with an end mark the Length does not count).
uint32_t nt_query_symbolic_link_object(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  uint32_t id = 0;
  if (const uint32_t status = typed_handle(p, r.args[0], DescriptorKind::SymbolicLink, &id)) return status;
  char text[ObjectManager::kNameBytes];
  const uint32_t n = objects->object_text(id, text, sizeof(text));
  uint16_t units[ObjectManager::kNameBytes];
  const uint32_t count = utf8_to_utf16(StringView(text, n), units, ObjectManager::kNameBytes);
  const uint32_t full = 2 * count + 2;
  uint8_t header[16];
  if (!p.read_user(r.args[1], ByteView(header, sizeof(header)))) return kAccessViolation;
  const uint32_t maximum = uint32_t(header[2]) | uint32_t(header[3]) << 8;
  uint64_t buffer = 0;
  for (int i = 0; i < 8; ++i) buffer |= uint64_t{header[8 + i]} << (8 * i);
  if (r.args[2] != 0 && !p.write_user_u32(r.args[2], full)) return kAccessViolation;
  if (maximum < full) return 0xC0000023u;      // STATUS_BUFFER_TOO_SMALL
  uint8_t out[2 * ObjectManager::kNameBytes + 2] = {};
  for (uint32_t i = 0; i < count; ++i) { out[2 * i] = uint8_t(units[i]); out[2 * i + 1] = uint8_t(units[i] >> 8); }
  if (!p.write_user(buffer, ConstByteView(out, full))) return kAccessViolation;
  const uint8_t length[2] = {uint8_t(2 * count), uint8_t((2 * count) >> 8)};
  return p.write_user(r.args[1], ConstByteView(length, 2)) ? kSuccess : kAccessViolation;
}

const char* object_type_name(ObjectKind kind) {
  switch (kind) {
    case ObjectKind::Event: return "Event";
    case ObjectKind::Mutex: return "Mutant";
    case ObjectKind::Semaphore: return "Semaphore";
    case ObjectKind::Process: return "Process";
    case ObjectKind::Timer: return "Timer";
    case ObjectKind::Section: return "Section";
    case ObjectKind::Job: return "Job";
    case ObjectKind::ChangeNotification: return "ChangeNotification";
    case ObjectKind::Mailslot: case ObjectKind::Pipe: return "File";
    case ObjectKind::DebugObject: return "DebugObject";
    case ObjectKind::Token: return "Token";
    case ObjectKind::KeyedEvent: return "KeyedEvent";
    case ObjectKind::IoCompletion: return "IoCompletion";
    case ObjectKind::Reserve: return "UserApcReserve";
    case ObjectKind::Directory: return "Directory";
    case ObjectKind::SymbolicLink: return "SymbolicLink";
    case ObjectKind::Port: return "ALPC Port";
    default: return "Object";
  }
}

// NtQueryDirectoryObject(Handle, Buffer, Length, ReturnSingleEntry, RestartScan, Context, ReturnLength): DIRECTORY_BASIC_INFORMATION entries (name, type name), then an empty
// entry, then the characters. `*Context` is the number of the entry to give next.
uint32_t nt_query_directory_object(Process& p, const SyscallRequest& r) {
  constexpr uint32_t kEntry = 32, kNoMoreEntries = 0x8000001Au, kMoreEntries = 0x105u, kBufferTooSmall = 0xC0000023u, kLimit = 4096;
  uint64_t single_at = 0, restart_at = 0, context_at = 0, returned_at = 0;
  if (!argument(p, r, 3, &single_at) || !argument(p, r, 4, &restart_at) || !argument(p, r, 5, &context_at) || !argument(p, r, 6, &returned_at)) return kAccessViolation;
  const bool single = (single_at & 0xFF) != 0, restart = (restart_at & 0xFF) != 0;
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  uint32_t id = 0;
  if (const uint32_t status = typed_handle(p, r.args[0], DescriptorKind::ObjectDirectory, &id)) return status;
  uint8_t raw[4];
  if (context_at == 0 || !p.read_user(context_at, ByteView(raw, 4))) return kAccessViolation;
  const uint32_t context = uint32_t(raw[0]) | uint32_t(raw[1]) << 8 | uint32_t(raw[2]) << 16 | uint32_t(raw[3]) << 24;
  const uint32_t length = uint32_t(r.args[2]) < kLimit ? uint32_t(r.args[2]) : kLimit;
  const uint32_t start = restart ? 0 : context;
  char dir_text[objpath::kMaxPath], flat[objpath::kMaxPath];
  uint32_t flat_size = 0;
  bool have_dir = false;
  if (const uint32_t dir_size = objects->object_text(id, dir_text, sizeof(dir_text))) {
    objpath::Path path;
    have_dir = path.parse(StringView(dir_text, dir_size)) && path.to_flat(flat, sizeof(flat), &flat_size);
  }
  struct Item { char name[ObjectManager::kNameBytes]; uint32_t name_size = 0; ObjectKind kind = ObjectKind::None; uint16_t units[ObjectManager::kNameBytes]; uint32_t unit_count = 0; StringView type; };
  auto fetch = [&](uint32_t index, Item* item) {
    if (!have_dir || !objects->directory_child(StringView(flat, flat_size), index, item->name, sizeof(item->name), &item->name_size, &item->kind)) return false;
    item->unit_count = utf8_to_utf16(StringView(item->name, item->name_size), item->units, ObjectManager::kNameBytes);
    item->type = StringView(object_type_name(item->kind));
    return true;
  };
  auto strings_of = [](const Item& item) { return 2 * item.unit_count + 2 + 2 * uint32_t(item.type.size()) + 2; };
  // How many entries fit: the entries and the empty one are kEntry bytes each, the characters follow.
  Item item;
  uint32_t count = 0, string_bytes = 0, first_needed = 0;
  bool more = false;
  while (fetch(start + count, &item)) {
    if (count == 0) first_needed = 2 * kEntry + strings_of(item);
    if (uint64_t{kEntry} * (count + 2) + string_bytes + strings_of(item) > length) { more = true; break; }
    string_bytes += strings_of(item);
    ++count;
    if (single) break;
  }
  auto set_context = [&](uint32_t value) {
    const uint8_t ctx[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
    return p.write_user(context_at, ConstByteView(ctx, 4));
  };
  uint8_t out[kLimit] = {};
  if (count == 0) {      // nothing to give: the empty entry alone
    uint32_t status = kNoMoreEntries;
    if (more) {
      if (single) {
        if (returned_at != 0 && !p.write_user_u32(returned_at, first_needed)) return kAccessViolation;
        return kBufferTooSmall;
      }
      status = kMoreEntries;
    }
    if (length >= kEntry && !p.write_user(r.args[1], ConstByteView(out, kEntry))) return kAccessViolation;
    if (returned_at != 0 && !p.write_user_u32(returned_at, kEntry)) return kAccessViolation;
    if (status == kMoreEntries && !set_context(start)) return kAccessViolation;
    return status;
  }
  auto put16 = [&](uint32_t at, uint32_t v) { out[at] = uint8_t(v); out[at + 1] = uint8_t(v >> 8); };
  auto put64 = [&](uint32_t at, uint64_t v) { for (uint32_t i = 0; i < 8; ++i) out[at + i] = uint8_t(v >> (8 * i)); };
  uint32_t used = kEntry * (count + 1);
  for (uint32_t i = 0; i < count; ++i) {
    if (!fetch(start + i, &item)) return kBufferTooSmall;      // (the directory changed under us)
    const uint32_t name_bytes = 2 * item.unit_count, type_bytes = 2 * uint32_t(item.type.size());
    if (uint64_t{used} + name_bytes + type_bytes + 4 > kLimit) return kBufferTooSmall;
    put16(i * kEntry, name_bytes); put16(i * kEntry + 2, name_bytes + 2); put64(i * kEntry + 8, r.args[1] + used);
    for (uint32_t k = 0; k < item.unit_count; ++k) put16(used + 2 * k, item.units[k]);
    used += name_bytes + 2;
    put16(i * kEntry + 16, type_bytes); put16(i * kEntry + 18, type_bytes + 2); put64(i * kEntry + 24, r.args[1] + used);
    for (uint32_t k = 0; k < item.type.size(); ++k) put16(used + 2 * k, uint8_t(item.type[k]));
    used += type_bytes + 2;
  }
  if (!p.write_user(r.args[1], ConstByteView(out, used))) return kAccessViolation;
  if (returned_at != 0 && !p.write_user_u32(returned_at, used)) return kAccessViolation;
  if (!set_context(start + count)) return kAccessViolation;
  return more && !single ? kMoreEntries : kSuccess;
}

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
  // An image that is not linked for dynamic base is mapped where there is room without its relocations being applied (Windows leaves it as the file has it).
  if (moved && (pe.dll_characteristics() & 0x40) && !apply_relocations(*space, pe, chosen, chosen - pe.image_base())) { space->unmap(chosen, rounded); return 0xC000007Bu; }
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
  if (info.attributes & 0x1000000u) {
    if (remote) return kNotImplemented;
    const uint32_t mapped = map_image_view(p, r, id, size_at, storage, info.size);
    uint64_t at = 0;
    char file[40];
    if ((mapped == kSuccess || mapped == 0x40000003u) && objects->section_file_name(id, file) && p.read_user_u64(r.args[2], &at)) p.note_view_file(at, file);
    return mapped;
  }
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
  if (!remote) {
    char file[40];
    if (objects->section_file_name(id, file)) p.note_view_file(base, file);
  }
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
  const uint64_t limit = info.reported_size != 0 && info.reported_size < info.size ? info.reported_size : info.size;
  if (view > limit - (offset < limit ? offset : limit) && info.reported_size != 0) return 0xC000001Fu;      // STATUS_INVALID_VIEW_SIZE
  if (view == 0 || view > info.size - offset) view = info.size - offset;
  uint64_t bytes = 0;
  if (!AddressSpace::round_up(view, &bytes)) return kInvalidParameter;
  if (base == 0) {      // views begin on the allocation granularity (64 KiB), as on Windows: programs round a view's address down to it to unmap the view
    uint64_t hint = 0x00007FF500000000ull;
    for (uint32_t attempt = 0;; ++attempt) {
      if (attempt >= 256 || !space->find_free(bytes, hint, &base)) return kNoResources;
      const uint64_t aligned = (base + 0xFFFF) & ~uint64_t{0xFFFF};
      if (aligned == base) break;
      hint = aligned;
    }
  } else if (base % 4096 != 0) {
    return kInvalidParameter;
  }
  // A copy-on-write view shares the section's memory until a page is written to (AddressSpace::break_copy_on_write gives that page a copy of its own).
  if (!space->map_shared(base, bytes, prot, storage + offset, owner)) return kStatusConflictingAddresses;
  PageAttributes attributes;
  attributes.type = PageAttributes::Mapped;
  attributes.protect = attributes.allocation_protect = view_page | ((info.attributes & 0x10000000u) ? 0x200u : 0u);      // (SEC_NOCACHE shows as PAGE_NOCACHE)
  attributes.file_view = info.file_backed;
  space->set_attributes(base, bytes, attributes);
  if (objects->section_reserved(owner)) {      // pages nobody committed yet cannot be touched (the committed ones show in every view)
    PageAttributes reserved = attributes;
    reserved.protect = 0x01;      // PAGE_NOACCESS
    reserved.reserved = true;
    for (uint64_t at = 0; at < bytes;) {
      const bool committed = objects->section_page_committed(owner, offset + at);
      uint64_t end = at + 4096;
      while (end < bytes && objects->section_page_committed(owner, offset + end) == committed) end += 4096;
      if (!committed) {
        space->protect(base + at, end - at, 0);
        space->set_attributes(base + at, end - at, reserved);
      }
      at = end;
    }
  }
  *base_in_out = base;
  *view_in_out = bytes;
  return kSuccess;
}

uint32_t map_section_handle(Process& p, uint64_t handle, uint64_t offset, uint64_t bytes, uint64_t* view_base, uint64_t* address) {
  const uint32_t id = object_of(p, handle, DescriptorKind::Section);
  if (!id) return kInvalidHandle;
  if (!handle_allows(p, handle, 0x2 /* SECTION_MAP_WRITE */)) return kAccessDeniedStatus;
  ObjectManager* objects = system_objects();
  ObjectManager::SectionInfo info;
  if (!objects || !objects->section_info(id, &info)) return kInvalidHandle;
  const uint64_t page_offset = offset & ~uint64_t{4095}, inside = offset - page_offset;
  if (offset >= info.size || bytes > info.size - offset) return kInvalidParameter;      // (the bits have to fit in the section)
  uint64_t base = 0, view = inside + bytes;
  const uint32_t status = map_view_in(p, id, page_offset, &base, &view, kPageReadWrite);
  if (status != kSuccess) return status;
  *view_base = base;
  *address = base + inside;
  return kSuccess;
}

// The whole view that starts at `address` in `target`'s address space.
uint32_t unmap_view_in(Process& target, uint64_t address) {
  AddressSpace* space = target.address_space();
  if (!space) return kNotImplemented;
  uint64_t base = 0, size = 0;
  PageAttributes attributes;
  if (!space->attributes_at(address, &attributes) || attributes.type == PageAttributes::Private) return kStatusNotMappedView;      // (plain memory is not a view)
  if (space->attributes_at(address, &attributes) && attributes.type == PageAttributes::Image) {      // an image goes with all its pieces
    return space->allocation_base(address, &base) && space->unmap_allocation(base) ? kSuccess : kStatusNotMappedView;      // (any address of an image unmaps all of it)
  }
  if (!space->allocation_extent(address, &base, &size)) return kStatusNotMappedView;      // (any address inside the view names it)
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
  bool image[2] = {};
  char name[2][40] = {};
  bool named[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    PageAttributes attributes;
    if (!space->attributes_at(AddressSpace::round_down(r.args[i]), &attributes) || (attributes.type != PageAttributes::Mapped && attributes.type != PageAttributes::Image)) return kConflictingAddresses;
    if (!space->allocation_base(AddressSpace::round_down(r.args[i]), &base[i])) return kConflictingAddresses;
    image[i] = attributes.type == PageAttributes::Image;
    named[i] = p.view_file_name(base[i], name[i]) || (image[i] && p.module_host() && p.module_host()->module_name_at(r.args[i], name[i]));
  }
  if (base[0] == base[1]) return kSuccess;
  // An image can be compared with any view of a file; a view of data only with itself.
  if (image[0] && named[0] && named[1]) {
    uint32_t k = 0;
    while (name[0][k] != 0 && name[0][k] == name[1][k]) ++k;
    if (name[0][k] == 0 && name[1][k] == 0) return kSuccess;
  }
  return kNotSameDevice;
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
    case 0: {      // OBJECT_BASIC_INFORMATION
      put32(0, (p.files().inheritable(slot) ? 2u : 0u) | (p.files().protected_from_close(slot) ? 1u : 0u));      // Attributes (OBJ_INHERIT, a protected handle)
      uint32_t granted = 0x001F0FFF;
      KindAccess kind_rights;
      if (kind_access(p.files().kind(slot), &kind_rights)) granted = kind_rights.valid & ~p.files().denied_access(slot);      // what the handle holds of what the kind has
      if (p.files().kind(slot) == DescriptorKind::File || p.files().kind(slot) == DescriptorKind::Directory) if (const OpenObject* opened = p.files().object(slot)) if (opened->granted_access != 0xFFFFFFFFu) {      // a file or directory handle says what it was opened for
        granted = kind_rights.mapping.map(opened->granted_access & ~0x02000000u) & kind_rights.valid & ~p.files().denied_access(slot);
        if (opened->granted_access & 0x02000000u) granted = kind_rights.valid & ~p.files().denied_access(slot);
      }
      put32(4, granted);      // GrantedAccess
      put32(8, 1);               // HandleCount
      put32(12, 1);              // PointerCount
      size = 56;
      break;
    }
    case 1: {    // OBJECT_NAME_INFORMATION: a UNICODE_STRING and its characters; the absolute path of the name an object was made with
      ObjectManager* objects = system_objects();
      char text[objpath::kMaxPath];
      uint32_t chars = 0;
      const DescriptorKind kind = p.files().kind(slot);
      if (objects && kind == DescriptorKind::ObjectDirectory) {
        chars = objects->object_text(uint32_t(p.files().token(slot)), text, sizeof(text));
      } else if (kind == DescriptorKind::Key) {
        String full(p.files().allocator());
        if (key_full_name(p, p.files().token(slot), &full)) {
          const uint32_t n = full.size() < sizeof(text) ? uint32_t(full.size()) : uint32_t(sizeof(text));
          for (uint32_t i = 0; i < n; ++i) text[i] = full.view()[i];
          chars = n;
        }
      } else if (objects && kind == DescriptorKind::Pipe) {      // "\Device\NamedPipe\name"; an unnamed instance, the pipe device and its directory have no path
        char flat[ObjectManager::kNameBytes];
        const uint32_t flat_size = (p.files().token(slot) >> 63) ? 0 : objects->object_name(uint32_t(p.files().token(slot)), flat, sizeof(flat));
        if (flat_size == 0 || flat[0] == 1) return 0xC0000039u;      // STATUS_OBJECT_PATH_INVALID
        static const char prefix[] = "\\Device\\NamedPipe\\";
        for (uint32_t i = 0; prefix[i] && chars < sizeof(text); ++i) text[chars++] = prefix[i];
        for (uint32_t i = 0; i < flat_size && chars < sizeof(text); ++i) text[chars++] = flat[i];
      } else if (objects && is_object_kind(kind)) {
        char flat[ObjectManager::kNameBytes];
        const uint32_t flat_size = objects->object_name(uint32_t(p.files().token(slot)), flat, sizeof(flat));
        objpath::Path path;
        if (flat_size != 0 && objpath::flat_to_path(StringView(flat, flat_size), &path)) path.format(text, sizeof(text), &chars);
      }
      uint16_t units[objpath::kMaxPath];
      const uint32_t count = utf8_to_utf16(StringView(text, chars), units, objpath::kMaxPath);
      const uint32_t bytes = 2 * count, need = count == 0 ? 16 : 16 + bytes + 2;      // (an object without a name is an empty string with no characters)
      if (returned != 0 && !p.write_user_u32(returned, need)) return kAccessViolation;
      if (length < need) return 0xC0000004u;
      uint8_t full[16 + 2 * objpath::kMaxPath + 2] = {};
      if (count != 0) {
        full[0] = uint8_t(bytes); full[1] = uint8_t(bytes >> 8); full[2] = uint8_t(bytes + 2); full[3] = uint8_t((bytes + 2) >> 8);
        const uint64_t text_at = r.args[2] + 16;
        for (uint32_t i = 0; i < 8; ++i) full[8 + i] = uint8_t(text_at >> (8 * i));
      }
      for (uint32_t i = 0; i < count; ++i) { full[16 + 2 * i] = uint8_t(units[i]); full[16 + 2 * i + 1] = uint8_t(units[i] >> 8); }
      return p.write_user(r.args[2], ConstByteView(full, need)) ? kSuccess : kAccessViolation;
    }
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
        case DescriptorKind::DebugObject: type = "DebugObject"; break;
        case DescriptorKind::KeyedEvent: type = "KeyedEvent"; break;
        case DescriptorKind::IoCompletion: type = "IoCompletion"; break;
        case DescriptorKind::Reserve: type = "UserApcReserve"; break;
        case DescriptorKind::Token: type = "Token"; break;
        case DescriptorKind::ChangeNotification: type = "ChangeNotification"; break;
        case DescriptorKind::Mailslot: type = "Mailslot"; break;
        case DescriptorKind::Key: type = "Key"; break;
        case DescriptorKind::ObjectDirectory: type = "Directory"; break;
        case DescriptorKind::SymbolicLink: type = "SymbolicLink"; break;
        case DescriptorKind::Port: type = "ALPC Port"; break;
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

// NtCompareObjects(First, Second): handles of one object have the same kind and the same object behind them (the token of the object manager, the open file, the process or thread).
uint32_t nt_compare_objects(Process& p, const SyscallRequest& r) {
  uint64_t first = 0, second = 0;
  if (!slot_of(r.args[0], &first) || !slot_of(r.args[1], &second)) return kInvalidHandle;
  ProcessFiles& files = p.files();
  const DescriptorKind a = files.kind(first), b = files.kind(second);
  if (a == DescriptorKind::Closed || b == DescriptorKind::Closed) return kInvalidHandle;
  constexpr uint32_t kNotSameObject = 0xC00004D4u;
  if (a != b) return kNotSameObject;
  if (files.object(first) != files.object(second)) return kNotSameObject;
  if (files.token(first) != files.token(second)) return kNotSameObject;      // (the terminal descriptors a console starts with have no token; the ones made later each have one)
  return kSuccess;
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
