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
#include "Process/WindowsFiles.hpp"
#include "Process/DosWildcard.hpp"
#include "Process/Objects.hpp"
#include "Process/WindowsMailslots.hpp"
#include "Process/WindowsPipes.hpp"
#include "Process/WindowsWait.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/Utf16.hpp"

namespace process::windowsabi {

namespace {

using filesystem::DirectoryEntry;
using filesystem::NodeType;
using filesystem::Status;

constexpr uint32_t kSuccess = 0, kBufferOverflow = 0x80000005u, kNoMoreFiles = 0x80000006u, kInvalidInfoClass = 0xC0000003u, kInfoLengthMismatch = 0xC0000004u,
                   kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kEndOfFile = 0xC0000011u,
                   kNoMemory = 0xC0000017u, kAccessDenied = 0xC0000022u, kObjectNameInvalid = 0xC0000033u, kObjectNameNotFound = 0xC0000034u,
                   kObjectNameCollision = 0xC0000035u, kObjectPathNotFound = 0xC000003Au, kSharingViolation = 0xC0000043u, kDiskFull = 0xC000007Fu,
                   kFileIsADirectory = 0xC00000BAu, kMediaWriteProtected = 0xC00000A2u, kDirectoryNotEmpty = 0xC0000101u, kNotADirectory = 0xC0000103u,
                   kIoDeviceError = 0xC0000185u, kNotImplemented = 0xC0000002u, kInvalidDeviceRequest = 0xC0000010u, kTooManyOpenedFiles = 0xC000011Fu, kTooManyLinks = 0xC0000112u,
                   kNoResources = 0xC000009Au;
constexpr uint64_t kMaxIoBytes = 0x7FFFF000u, kChunk = 4096;
constexpr uint64_t kUnixToNtSeconds = 11644473600ull;
constexpr uint64_t kUseFilePointer = 0xFFFFFFFFFFFFFFFEull, kWriteToEnd = 0xFFFFFFFFFFFFFFFFull;
// NtCreateFile dispositions, options, access bits, Information results
constexpr uint32_t kSupersede = 0, kOpen = 1, kCreate = 2, kOpenIf = 3, kOverwrite = 4, kOverwriteIf = 5;
constexpr uint32_t kDirectoryFile = 1, kNonDirectoryFile = 0x40, kDeleteOnClose = 0x1000;
constexpr uint32_t kCannotDelete = 0xC0000121u, kUserMappedFile = 0xC0000243u;
constexpr uint32_t kGenericRead = 0x80000000u, kGenericWrite = 0x40000000u, kGenericAll = 0x10000000u, kFileReadData = 1, kFileWriteData = 2, kFileAppendData = 4;
constexpr uint64_t kFileSuperseded = 0, kFileOpened = 1, kFileCreated = 2, kFileOverwritten = 3;
constexpr uint32_t kAttributeReadOnly = 1, kAttributeDirectory = 0x10, kAttributeNormal = 0x80, kAttributeArchive = 0x20, kAttributeReparsePoint = 0x400;
// FILE_INFORMATION_CLASS
constexpr uint32_t kFileDirectoryInformation = 1, kFileFullDirectoryInformation = 2, kFileBothDirectoryInformation = 3, kFileBasicInformation = 4,
                   kFileStandardInformation = 5, kFileRenameInformation = 10, kFileNamesInformation = 12, kFileDispositionInformation = 13,
                   kFilePositionInformation = 14, kFileNameInformation = 9, kFileAllocationInformation = 19, kFileEndOfFileInformation = 20,
                   kFileNetworkOpenInformation = 34, kFileIdBothDirectoryInformation = 37, kFileLinkInformation = 11;

uint32_t nt_status(Status status) {
  switch (status) {
    case Status::Ok: return kSuccess;
    case Status::EndOfDirectory: return kNoMoreFiles;
    case Status::NotFound: return kObjectNameNotFound;
    case Status::AlreadyExists: return kObjectNameCollision;
    case Status::NotADirectory: return kNotADirectory;
    case Status::IsADirectory: return kFileIsADirectory;
    case Status::NotEmpty: return kDirectoryNotEmpty;
    case Status::Busy: return kSharingViolation;
    case Status::ReadOnly: return kMediaWriteProtected;
    case Status::NoSpace: return kDiskFull;
    case Status::InvalidPath: return kObjectNameInvalid;
    case Status::InvalidArgument: return kInvalidParameter;
    case Status::OutOfMemory: return kNoMemory;
    case Status::AccessDenied:
    case Status::LoginRequired:
    case Status::LoginFailed: return kAccessDenied;
    case Status::TooManyLinks: return kTooManyLinks;
    default: return kIoDeviceError;
  }
}

bool handle_slot(uint64_t handle, uint64_t* slot) {
  if (handle == 0 || handle % 4 != 0 || handle / 4 > Process::kMaxDescriptors) return false;
  *slot = handle / 4 - 1;
  return true;
}

// The directory part of a virtual path that files.resolve() already produced (so it has no "." or ".." components): "/Windows/sub/x.txt" -> "/Windows/sub", "/x.txt" -> "/".
StringView virtual_directory_of(StringView virtual_path) {
  uint64_t at = virtual_path.size();
  while (at > 0 && virtual_path[at - 1] != '/') --at;
  return at <= 1 ? StringView("/") : virtual_path.first(at - 1);
}

// The entry's own name, the part virtual_directory_of leaves out: "/Windows/sub/x.txt" -> "x.txt".
StringView virtual_leaf_of(StringView virtual_path) {
  uint64_t at = virtual_path.size();
  while (at > 0 && virtual_path[at - 1] != '/') --at;
  return virtual_path.without_prefix(at);
}

// After any successful change to a directory's contents (a file or subdirectory created, deleted, renamed or linked): wakes any FindFirstChangeNotification/
// ReadDirectoryChangesW handle watching `affected_directory` or an ancestor of it, and queues a FILE_ACTION_*/name record for the latter (see
// ObjectManager::notify_path_changed). `changed_path` is the changed entry's own full virtual path; `action` one of the FILE_ACTION_* constants below.
// A no-op if there is no ObjectManager yet (a host test that never made one; the real kernel always has).
constexpr uint32_t kFileActionAdded = 1, kFileActionRemoved = 2, kFileActionRenamedOldName = 4, kFileActionRenamedNewName = 5;
void notify_directory_changed(StringView changed_path, uint32_t action) {
  if (ObjectManager* objects = system_objects()) objects->notify_path_changed(virtual_directory_of(changed_path), action, virtual_leaf_of(changed_path));
}

// IO_STATUS_BLOCK { NTSTATUS Status (8 bytes with padding); ULONG_PTR Information; }
bool store_io_status(Process& p, uint64_t address, uint32_t status, uint64_t information) {
  return p.write_user_u64(address, status) && p.write_user_u64(address + 8, information);
}

uint64_t file_time(uint64_t unix_seconds) { return unix_seconds ? (unix_seconds + kUnixToNtSeconds) * 10000000ull : 0; }

void put(ByteView out, uint64_t at, uint64_t value, uint32_t bytes) {
  for (uint32_t i = 0; i < bytes; ++i) out[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

uint32_t attributes_of(const DirectoryEntry& e) {
  uint32_t a = e.type == NodeType::Directory ? kAttributeDirectory : 0;
  if (e.read_only) a |= kAttributeReadOnly;
  if (e.is_symlink) a |= kAttributeReparsePoint;
  if (e.type != NodeType::Directory && !e.is_symlink) a |= kAttributeArchive;      // (not kept: a file always counts as one that has not been backed up)
  return a ? a : kAttributeNormal;
}

// --- names

// Reads a UNICODE_STRING (Length, MaximumLength, Buffer) into UTF-8.
uint32_t read_unicode_string(Process& p, uint64_t address, String* out) {
  uint8_t header[16];
  if (address == 0 || !p.read_user(address, ByteView(header, 16))) return kAccessViolation;
  const uint64_t length = header[0] | (uint64_t{header[1]} << 8);
  uint64_t buffer = 0;
  for (uint32_t i = 0; i < 8; ++i) buffer |= uint64_t{header[8 + i]} << (8 * i);
  if (length % 2 != 0 || length > 2 * kMaxPath) return kObjectNameInvalid;
  out->clear();
  if (length == 0) return kSuccess;
  uint8_t units[2 * kMaxPath];
  if (!p.read_user(buffer, ByteView(units, length))) return kAccessViolation;
  return util::append_utf8_from_utf16le(out, ConstByteView(units, length)) ? kSuccess : kNoMemory;
}

// OBJECT_ATTRIBUTES { ULONG Length; HANDLE RootDirectory; PUNICODE_STRING ObjectName; ULONG Attributes; ... } -> the cleaned virtual path.
uint32_t object_path(Process& p, uint64_t attributes, String* virtual_path) {
  ProcessFiles& files = p.files();
  if (!files.attached()) return kObjectPathNotFound;
  uint8_t raw[0x20];
  if (attributes == 0 || !p.read_user(attributes, ByteView(raw, sizeof(raw)))) return kAccessViolation;
  const uint64_t length = raw[0] | (uint64_t{raw[1]} << 8) | (uint64_t{raw[2]} << 16) | (uint64_t{raw[3]} << 24);
  uint64_t root = 0, name = 0;
  for (uint32_t i = 0; i < 8; ++i) { root |= uint64_t{raw[8 + i]} << (8 * i); name |= uint64_t{raw[0x10 + i]} << (8 * i); }
  if (length < 0x30) return kInvalidParameter;
  String text(files.allocator());
  const uint32_t read = read_unicode_string(p, name, &text);
  if (read != kSuccess) return read;
  PathResult result;
  if (root != 0) {
    uint64_t slot = 0;
    if (!handle_slot(root, &slot) || files.kind(slot) != DescriptorKind::Directory) return kInvalidHandle;
    result = files.resolve_from(files.object(slot)->path.view(), text.view(), virtual_path);
  } else {
    result = files.resolve(text.view(), virtual_path);
  }
  switch (result) {
    case PathResult::Ok: return kSuccess;
    case PathResult::Empty: return root != 0 ? kSuccess : kObjectNameInvalid;
    case PathResult::Invalid: return kObjectNameInvalid;
    default: return kObjectNameInvalid;
  }
}

// The status for a missing name: the path is wrong if the folder above is missing too.
uint32_t not_found_status(ProcessFiles& files, StringView virtual_path) {
  const uint64_t cut = virtual_path.rfind('/');
  if (cut == StringView::npos || cut == 0) return kObjectNameNotFound;
  DirectoryEntry parent;
  return files.stat(virtual_path.first(cut), &parent) == Status::Ok ? kObjectNameNotFound : kObjectPathNotFound;
}

uint32_t status_for(ProcessFiles& files, Status status, StringView virtual_path) {
  return status == Status::NotFound ? not_found_status(files, virtual_path) : nt_status(status);
}

// Opens (or creates) what the OBJECT_ATTRIBUTES name. On success *handle and *information (FILE_OPENED, ...) are set.
// Is the file (by its virtual path) the file of a section: mapped, or held by a mapping handle?
// 0: no, 1: a data section, 2: an image section.
uint32_t file_is_mapped(ProcessFiles& files, StringView virtual_path) {
  ObjectManager* objects = system_objects();
  String host(files.allocator());
  return objects && files.host_path_of(virtual_path, &host) ? objects->file_has_section(host.view()) : 0;
}

uint32_t open_by_attributes(Process& p, uint64_t object_attributes, uint32_t access, uint32_t disposition, uint32_t options, uint64_t* information, uint64_t* handle, uint32_t share = 7, uint32_t file_attributes = 0) {
  ProcessFiles& files = p.files();
  {
    uint32_t pipe_status = 0;
    if (pipe_open_by_name(p, object_attributes, access, &pipe_status, handle)) {      // \\.\pipe\name
      if (pipe_status == kSuccess) *information = 1;      // FILE_OPENED
      return pipe_status;
    }
  }
  {
    uint32_t mailslot_status = 0;
    if (mailslot_open_by_name(p, object_attributes, access, &mailslot_status, handle)) {      // \\.\mailslot\name (a writer)
      if (mailslot_status == kSuccess) *information = 1;      // FILE_OPENED
      return mailslot_status;
    }
  }
  if (!files.attached()) return kObjectPathNotFound;
  {      // \\??\\NUL, the device that swallows what is written to it
    uint64_t name_at = 0;
    String text(files.allocator());
    if (object_attributes != 0 && p.read_user_u64(object_attributes + 16, &name_at) && name_at != 0 && read_unicode_string(p, name_at, &text) == kSuccess) {
      static const char* const names[] = {"\\??\\nul", "\\device\\null"};
      for (const char* device : names) {
        uint64_t n = 0;
        while (device[n]) ++n;
        if (text.size() != n) continue;
        bool same = true;
        for (uint64_t i = 0; same && i < n; ++i) same = (text.view()[i] >= 'A' && text.view()[i] <= 'Z' ? char(text.view()[i] + 32) : text.view()[i]) == device[i];
        if (!same) continue;
        const int64_t slot = files.open_device(DescriptorKind::Null);
        if (slot < 0) return kTooManyOpenedFiles;
        *handle = (uint64_t(slot) + 1) * 4;
        *information = kFileOpened;
        return kSuccess;
      }
    }
  }
  String virtual_path(files.allocator());
  const uint32_t named = object_path(p, object_attributes, &virtual_path);
  if (named != kSuccess) return named;
  if (disposition > kOverwriteIf) return kInvalidParameter;
  if ((options & kDirectoryFile) && (options & kNonDirectoryFile)) return kInvalidParameter;

  OpenFlags f;
  const bool all = access & kGenericAll;
  f.write = all || (access & (kGenericWrite | kFileWriteData | kFileAppendData));
  f.read = all || (access & (kGenericRead | kFileReadData)) || !f.write;
  f.create = disposition == kSupersede || disposition == kCreate || disposition == kOpenIf || disposition == kOverwriteIf;
  f.exclusive = disposition == kCreate;
  f.truncate = disposition == kSupersede || disposition == kOverwrite || disposition == kOverwriteIf;
  f.must_be_directory = options & kDirectoryFile;
  f.must_not_be_directory = options & kNonDirectoryFile;
  // Overwriting needs the file open for writing even if the program only asked to read.
  if (f.truncate) f.write = true;
  DirectoryEntry before;
  const bool existed = files.stat(virtual_path.view(), &before) == Status::Ok;
  if (existed && before.type == NodeType::Directory) {
    f.write = false;      // a directory is opened to be listed or queried
    f.truncate = false;
    if (f.must_not_be_directory) return kFileIsADirectory;
    if (disposition == kCreate) return kObjectNameCollision;
  }
  ObjectManager* sharing = system_objects();
  uint32_t wanted = 0;      // what the open does with the file, for the sharing rules
  if (access & (kGenericRead | kGenericAll | kFileReadData | 0x20 /* FILE_EXECUTE */ | 0x20000000u /* GENERIC_EXECUTE */)) wanted |= ObjectManager::kShareRead;
  if (access & (kGenericWrite | kGenericAll | kFileWriteData | kFileAppendData)) wanted |= ObjectManager::kShareWrite;
  if ((access & (kGenericAll | 0x10000 /* DELETE */)) || (options & kDeleteOnClose)) wanted |= ObjectManager::kShareDelete;
  if (f.truncate) wanted |= ObjectManager::kShareWrite;      // overwriting a file writes to it
  if (existed && before.type != NodeType::Directory) {
    if (f.must_be_directory) return kNotADirectory;
    if (before.read_only && f.write) return kAccessDenied;      // a read-only file cannot be opened for writing, nor overwritten
    if (options & kDeleteOnClose) {
      if (!(access & (kGenericAll | 0x10000))) return kAccessDenied;
      if (before.read_only) return kCannotDelete;
    }
    if (sharing && wanted && sharing->share_acquire(virtual_path.view(), nullptr, wanted, share) == ObjectManager::ShareResult::Violation) return kSharingViolation;
    if (f.truncate || (options & kDeleteOnClose)) {      // a mapped file cannot be overwritten; a mapped image cannot be deleted either
      const uint32_t mapped = file_is_mapped(files, virtual_path.view());
      if (mapped && f.truncate) return mapped == 2 ? kSharingViolation : kUserMappedFile;
      if (mapped == 2) return kAccessDenied;
    }
  } else if ((options & kDeleteOnClose) && !(access & (kGenericAll | 0x10000))) {
    return kAccessDenied;
  }
  uint32_t slot = 0;
  bool created = false;
  const Status status = files.open(virtual_path.view(), f, &slot, &created);
  if (status == Status::OutOfMemory && files.free_slot(0) < 0) return kTooManyOpenedFiles;
  if (status != Status::Ok) return status_for(files, status, virtual_path.view());
  if (created) notify_directory_changed(virtual_path.view(), kFileActionAdded);
  if (created && (file_attributes & kAttributeReadOnly)) {
    filesystem::MetadataChange change;
    change.set_read_only = true;
    change.read_only = true;
    files.set_metadata(virtual_path.view(), change);
  }
  if (files.kind(slot) == DescriptorKind::File) {
    if (OpenObject* opened = files.object(slot)) opened->no_data_access = !(access & (kGenericRead | kGenericWrite | kGenericAll | kFileReadData | kFileWriteData | kFileAppendData | 0x20 | 0x20000000u));
  }
  if (files.kind(slot) == DescriptorKind::File) {
    OpenObject* opened = files.object(slot);
    opened->granted_access = access;
    opened->writable = (access & (kGenericAll | kGenericWrite | kFileWriteData | kFileAppendData)) != 0;      // (an overwrite writes through the file system's own handle, not the program's)
  }
  if (sharing && wanted && files.kind(slot) == DescriptorKind::File) sharing->share_acquire(virtual_path.view(), files.object(slot), wanted, share);
  if (options & kDeleteOnClose) files.object(slot)->delete_on_close = true;
  uint32_t object_flags = 0;
  uint8_t flag_bytes[4];
  if (p.read_user(object_attributes + 24, ByteView(flag_bytes, 4))) object_flags = flag_bytes[0] | (uint32_t{flag_bytes[1]} << 8) | (uint32_t{flag_bytes[2]} << 16) | (uint32_t{flag_bytes[3]} << 24);
  files.set_handle_flags(slot, (object_flags & 2) != 0, false);      // OBJ_INHERIT
  *handle = (uint64_t{slot} + 1) * 4;
  if (created) *information = kFileCreated;
  else if (disposition == kSupersede) *information = kFileSuperseded;
  else if (disposition == kOverwrite || disposition == kOverwriteIf) *information = kFileOverwritten;
  else *information = kFileOpened;
  return kSuccess;
}

// The 1.. 64-bit unit position a Read/WriteFile uses: -2 = the file pointer, -1 = the end (writes only), anything else is absolute.
uint32_t read_offset(Process& p, uint64_t pointer, uint64_t* value) {
  if (pointer == 0) { *value = kUseFilePointer; return kSuccess; }
  return p.read_user_u64(pointer, value) ? kSuccess : kAccessViolation;
}

uint32_t file_io(Process& p, const SyscallRequest& r, bool write) {
  uint64_t apc = 0, status_block = 0, buffer = 0, length = 0, offset_pointer = 0;
  if (!argument(p, r, 2, &apc) || !argument(p, r, 4, &status_block) || !argument(p, r, 5, &buffer) || !argument(p, r, 6, &length) || !argument(p, r, 7, &offset_pointer))
    return kAccessViolation;
  uint64_t slot = 0;
  if (!handle_slot(r.args[0], &slot)) return kInvalidHandle;
  ProcessFiles& files = p.files();
  const DescriptorKind kind = files.kind(slot);
  if (kind == DescriptorKind::Closed) return kInvalidHandle;
  if (apc != 0) return kNotImplemented;      // asynchronous completion is not supported
  length &= 0xFFFFFFFFu;
  if (length > kMaxIoBytes) length = kMaxIoBytes;
  if (kind == DescriptorKind::Pipe) return pipe_io(p, slot, write, status_block, buffer, length);
  if (kind == DescriptorKind::Mailslot) return mailslot_io(p, slot, write, status_block, buffer, length);
  if (kind == DescriptorKind::Null) {      // the NUL device takes everything and has nothing to read
    const uint32_t status = write ? kSuccess : kEndOfFile;
    return store_io_status(p, status_block, status, write ? length : 0) ? status : kAccessViolation;
  }

  if (kind == DescriptorKind::ConsoleInput || kind == DescriptorKind::ConsoleOutput || kind == DescriptorKind::ConsoleError) {
    if (write) {
      if (kind == DescriptorKind::ConsoleInput) return kAccessDenied;
      bool faulted = false;
      const uint64_t done = copy_user_to_console(p, kind == DescriptorKind::ConsoleError ? 2 : 1, buffer, length, &faulted);
      if (faulted && done == 0) return kAccessViolation;
      return store_io_status(p, status_block, kSuccess, done) ? kSuccess : kAccessViolation;
    }
    if (kind != DescriptorKind::ConsoleInput) return kAccessDenied;
    bool faulted = false;
    const uint64_t got = copy_console_to_user(p, buffer, length, &faulted);
    if (faulted) return kAccessViolation;
    const uint32_t status = got == 0 && length != 0 ? kEndOfFile : kSuccess;
    return store_io_status(p, status_block, status, got) ? status : kAccessViolation;
  }
  if (kind == DescriptorKind::Directory) return kInvalidDeviceRequest;
  OpenObject& object = *files.object(slot);
  if (write ? !object.writable : !object.readable) return kAccessDenied;
  uint64_t requested = 0;
  uint32_t status = read_offset(p, offset_pointer, &requested);
  if (status != kSuccess) return status;
  uint64_t at = requested == kUseFilePointer ? object.position : requested;
  if (requested == kWriteToEnd) {
    if (!write) return kInvalidParameter;
    at = files.size_of(object);
  }
  uint64_t done = 0;
  uint8_t chunk[kChunk];
  Status io = Status::Ok;
  while (done < length && io == Status::Ok) {
    const uint64_t n = length - done < sizeof(chunk) ? length - done : sizeof(chunk);
    uint64_t moved = 0;
    if (write) {
      if (!p.read_user(buffer + done, ByteView(chunk, n))) { if (done == 0) return kAccessViolation; break; }
      io = files.write(object, at + done, ConstByteView(chunk, n), &moved);
    } else {
      io = files.read(object, at + done, ByteView(chunk, n), &moved);
      if (io == Status::Ok && moved && !p.write_user(buffer + done, ConstByteView(chunk, moved))) { if (done == 0) return kAccessViolation; break; }
    }
    done += moved;
    if (io == Status::Ok && moved < n) break;      // end of the file
  }
  if (io != Status::Ok && done == 0) return nt_status(io);
  object.position = at + done;
  status = kSuccess;
  if (!write && done == 0 && length != 0) status = kEndOfFile;
  return store_io_status(p, status_block, status, done) ? status : kAccessViolation;
}

// --- queries


void write_times(ByteView out, uint64_t at, const DirectoryEntry& e) {
  put(out, at, file_time(e.created ? e.created : e.modified), 8);      // CreationTime
  put(out, at + 8, file_time(e.modified), 8);                           // LastAccessTime
  put(out, at + 16, file_time(e.modified), 8);                          // LastWriteTime
  put(out, at + 24, file_time(e.modified), 8);                          // ChangeTime
}

uint64_t allocation_of(const DirectoryEntry& e) { return (e.size + 4095) & ~uint64_t{4095}; }

// FILE_BASIC_INFORMATION is 40 bytes.
bool write_basic(Process& p, uint64_t address, const DirectoryEntry& e) {
  uint8_t b[40] = {};
  ByteView v(b, sizeof(b));
  write_times(v, 0, e);
  put(v, 32, attributes_of(e), 4);
  return p.write_user(address, ConstByteView(b, sizeof(b)));
}

// UTF-8 to UTF-16LE without allocating; returns the number of bytes written (0 if `capacity` is too small or the text is malformed).
uint64_t to_utf16(StringView text, uint8_t* out, uint64_t capacity) {
  uint64_t i = 0, n = 0;
  auto emit = [&](uint32_t unit) {
    if (n + 2 > capacity) return false;
    out[n++] = static_cast<uint8_t>(unit);
    out[n++] = static_cast<uint8_t>(unit >> 8);
    return true;
  };
  while (i < text.size()) {
    const uint8_t b = static_cast<uint8_t>(text[i]);
    uint32_t cp, extra;
    if (b < 0x80) cp = b, extra = 0;
    else if ((b & 0xE0) == 0xC0) cp = b & 0x1F, extra = 1;
    else if ((b & 0xF0) == 0xE0) cp = b & 0x0F, extra = 2;
    else if ((b & 0xF8) == 0xF0) cp = b & 0x07, extra = 3;
    else return 0;
    if (i + extra >= text.size() && extra != 0) return 0;
    for (uint32_t k = 1; k <= extra; ++k) {
      const uint8_t c = static_cast<uint8_t>(text[i + k]);
      if ((c & 0xC0) != 0x80) return 0;
      cp = (cp << 6) | (c & 0x3F);
    }
    i += extra + 1;
    if (cp > 0x10FFFF) return 0;
    if (cp >= 0x10000) {
      const uint32_t v = cp - 0x10000;
      if (!emit(0xD800 + (v >> 10)) || !emit(0xDC00 + (v & 0x3FF))) return 0;
    } else if (!emit(cp)) {
      return 0;
    }
  }
  return n;
}

// One directory entry in the layout of `info_class`, with NextEntryOffset left 0. Returns its size (unpadded), or 0 if the class is unknown.
uint64_t build_directory_record(uint32_t info_class, const DirectoryEntry& e, uint64_t index, uint8_t* out, uint64_t capacity) {
  uint8_t name[2 * (filesystem::kMaxNameLength + 1)];
  const uint64_t name_bytes = to_utf16(e.name.view(), name, sizeof(name));
  if (name_bytes == 0 && !e.name.empty()) return 0;
  uint64_t name_at = 0, header = 0;
  switch (info_class) {
    case kFileDirectoryInformation: name_at = 64; break;
    case kFileFullDirectoryInformation: name_at = 68; break;
    case kFileBothDirectoryInformation: name_at = 94; break;
    case kFileIdBothDirectoryInformation: name_at = 104; break;
    case kFileNamesInformation: name_at = 12; break;
    default: return 0;
  }
  header = name_at;
  if (header + name_bytes > capacity) return 0;
  ByteView v(out, header + name_bytes);
  v.fill(0);
  put(v, 4, index, 4);                                 // FileIndex
  if (info_class == kFileNamesInformation) {
    put(v, 8, name_bytes, 4);
  } else {
    write_times(v, 8, e);
    put(v, 40, e.size, 8);                             // EndOfFile
    put(v, 48, allocation_of(e), 8);                   // AllocationSize
    put(v, 56, attributes_of(e), 4);
    put(v, 60, name_bytes, 4);
    if (info_class == kFileIdBothDirectoryInformation) put(v, 96, index + 1, 8);      // FileId
  }
  for (uint64_t i = 0; i < name_bytes; ++i) v[name_at + i] = name[i];
  return header + name_bytes;
}

bool classes_known(uint32_t c) {
  return c == kFileDirectoryInformation || c == kFileFullDirectoryInformation || c == kFileBothDirectoryInformation || c == kFileNamesInformation || c == kFileIdBothDirectoryInformation;
}

}  // namespace

uint32_t read_unicode_string_argument(Process& p, uint64_t address, String* out) { return read_unicode_string(p, address, out); }

bool argument(Process& p, const SyscallRequest& r, uint32_t index, uint64_t* out) {
  if (index < 4) { *out = r.args[index]; return true; }
  const uint64_t offset = 0x28 + uint64_t{index - 4} * 8;      // past the return address and the 32 bytes of home space
  if (r.user_stack > ~uint64_t{0} - offset - 7) return false;
  return p.read_user_u64(r.user_stack + offset, out);
}

uint32_t nt_create_file(Process& p, const SyscallRequest& r) {
  // NtCreateFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
  //              ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength)
  uint64_t status_block = 0, disposition = 0, options = 0, file_attributes = 0, share = 0;
  if (!argument(p, r, 3, &status_block) || !argument(p, r, 5, &file_attributes) || !argument(p, r, 6, &share) || !argument(p, r, 7, &disposition) || !argument(p, r, 8, &options)) return kAccessViolation;
  uint64_t information = 0, handle = 0;
  const uint32_t status = open_by_attributes(p, r.args[2], static_cast<uint32_t>(r.args[1]), static_cast<uint32_t>(disposition), static_cast<uint32_t>(options), &information, &handle, static_cast<uint32_t>(share) & 7, static_cast<uint32_t>(file_attributes));
  if (status != kSuccess) {
    store_io_status(p, status_block, status, 0);
    return status;
  }
  if (!p.write_user_u64(r.args[0], handle) || !store_io_status(p, status_block, kSuccess, information)) {
    p.close_descriptor(handle / 4 - 1);
    return kAccessViolation;
  }
  return kSuccess;
}

uint32_t nt_open_file(Process& p, const SyscallRequest& r) {
  // NtOpenFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG ShareAccess, ULONG OpenOptions)
  uint64_t status_block = 0, options = 0, share = 0;
  if (!argument(p, r, 3, &status_block) || !argument(p, r, 4, &share) || !argument(p, r, 5, &options)) return kAccessViolation;
  uint64_t information = 0, handle = 0;
  const uint32_t status = open_by_attributes(p, r.args[2], static_cast<uint32_t>(r.args[1]), kOpen, static_cast<uint32_t>(options), &information, &handle, static_cast<uint32_t>(share) & 7);
  if (status != kSuccess) {
    store_io_status(p, status_block, status, 0);
    return status;
  }
  if (!p.write_user_u64(r.args[0], handle) || !store_io_status(p, status_block, kSuccess, information)) {
    p.close_descriptor(handle / 4 - 1);
    return kAccessViolation;
  }
  return kSuccess;
}

uint32_t nt_read_file(Process& p, const SyscallRequest& r) { return file_io(p, r, false); }
uint32_t nt_write_file(Process& p, const SyscallRequest& r) { return file_io(p, r, true); }

uint32_t nt_close(Process& p, const SyscallRequest& r) {
  uint64_t slot = 0;
  if (!handle_slot(r.args[0], &slot)) return kInvalidHandle;
  if (p.files().protected_from_close(slot)) return 0xC0000235u;      // STATUS_HANDLE_NOT_CLOSABLE
  return p.close_descriptor(slot) ? kSuccess : kInvalidHandle;
}

uint32_t nt_query_information_file(Process& p, const SyscallRequest& r) {
  // NtQueryInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass)
  uint64_t info_class = 0;
  if (!argument(p, r, 4, &info_class)) return kAccessViolation;
  uint64_t slot = 0;
  if (!handle_slot(r.args[0], &slot)) return kInvalidHandle;
  ProcessFiles& files = p.files();
  const DescriptorKind kind = files.kind(slot);
  if (kind == DescriptorKind::Closed) return kInvalidHandle;
  if (kind == DescriptorKind::ConsoleInput && static_cast<uint32_t>(info_class) == 24) {      // FilePipeLocalInformation: what the console input could give at once
    bool at_end = false;
    const uint64_t available = p.host().console_input_available(&at_end);
    uint8_t b[40] = {};
    const uint32_t value = available > 0xFFFFFFFFu ? 0xFFFFFFFFu : uint32_t(available);
    for (int i = 0; i < 4; ++i) b[20 + i] = uint8_t(value >> (8 * i));      // ReadDataAvailable
    b[24] = at_end ? 1 : 0;      // (OutboundQuota's first byte: the end of input is there; a read would not wait)
    if ((r.args[3] & 0xFFFFFFFFu) < sizeof b) return kInfoLengthMismatch;
    if (!p.write_user(r.args[2], ConstByteView(b, sizeof b))) return kAccessViolation;
    return store_io_status(p, r.args[1], kSuccess, sizeof b) ? kSuccess : kAccessViolation;
  }
  if (kind == DescriptorKind::Pipe) {
    const uint32_t pipe_class = static_cast<uint32_t>(info_class);      // (a ULONG: the upper half of the stack slot is not defined)
    if (pipe_class == 23 || pipe_class == 24) return pipe_query_information(p, slot, pipe_class, r.args[1], r.args[2], uint32_t(r.args[3]));
  }
  if (kind == DescriptorKind::Mailslot) {
    const uint32_t mailslot_class = static_cast<uint32_t>(info_class);
    if (mailslot_class == 26) return mailslot_query_information(p, slot, mailslot_class, r.args[1], r.args[2], uint32_t(r.args[3]));
  }
  DirectoryEntry e;
  OpenObject* object = files.object(slot);
  if (object) {
    const Status status = files.stat_object(*object, &e);
    if (status != Status::Ok) return nt_status(status);
  } else {
    e.type = NodeType::File;      // the console
  }
  const uint64_t status_block = r.args[1], address = r.args[2], length = r.args[3] & 0xFFFFFFFFu;      // (a ULONG: the upper half of the register is not defined)
  uint8_t b[64] = {};
  ByteView v(b, sizeof(b));
  uint64_t size = 0;
  switch (static_cast<uint32_t>(info_class)) {
    case kFileBasicInformation: write_times(v, 0, e); put(v, 32, attributes_of(e), 4); size = 40; break;
    case kFileStandardInformation:
      put(v, 0, allocation_of(e), 8);
      put(v, 8, e.size, 8);
      put(v, 16, 1, 4);                                             // NumberOfLinks
      v[20] = object && object->delete_on_close ? 1 : 0;            // DeletePending
      v[21] = e.type == NodeType::Directory ? 1 : 0;                // Directory
      size = 24;
      break;
    case kFilePositionInformation: put(v, 0, object ? object->position : 0, 8); size = 8; break;
    case kFileNetworkOpenInformation:
      write_times(v, 0, e);
      put(v, 32, allocation_of(e), 8);
      put(v, 40, e.size, 8);
      put(v, 48, attributes_of(e), 4);
      size = 56;
      break;
    case kFileNameInformation: {
      // The path on the volume, "\dir\file" (the root is "\").
      if (!object) return kInvalidParameter;
      uint8_t name[2 * (kMaxPath + 1)];
      String windows_name(files.allocator());
      for (char c : object->path.view()) if (!windows_name.push_back(c == '/' ? '\\' : c)) return kNoMemory;
      const uint64_t bytes = to_utf16(windows_name.view(), name, sizeof(name));
      if (length < 4) return kInfoLengthMismatch;
      uint8_t out[4];
      ByteView ov(out, 4);
      put(ov, 0, bytes, 4);
      if (!p.write_user(address, ConstByteView(out, 4))) return kAccessViolation;
      const uint64_t room = length - 4 < bytes ? length - 4 : bytes;
      if (room && !p.write_user(address + 4, ConstByteView(name, room))) return kAccessViolation;
      const uint32_t status = room < bytes ? kBufferOverflow : kSuccess;
      return store_io_status(p, status_block, status, 4 + room) ? status : kAccessViolation;
    }
    default: return kInvalidInfoClass;
  }
  if (length < size) return kInfoLengthMismatch;
  if (!p.write_user(address, ConstByteView(b, size))) return kAccessViolation;
  return store_io_status(p, status_block, kSuccess, size) ? kSuccess : kAccessViolation;
}

uint32_t nt_set_information_file(Process& p, const SyscallRequest& r) {
  // NtSetInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass)
  uint64_t info_class = 0;
  if (!argument(p, r, 4, &info_class)) return kAccessViolation;
  uint64_t slot = 0;
  if (!handle_slot(r.args[0], &slot)) return kInvalidHandle;
  ProcessFiles& files = p.files();
  if (files.kind(slot) == DescriptorKind::Pipe) {
    if (static_cast<uint32_t>(info_class) != 23) return kInvalidInfoClass;
    const uint32_t pipe_status = pipe_set_information(p, slot, 23, r.args[2], uint32_t(r.args[3]));
    return store_io_status(p, r.args[1], pipe_status, 0) ? pipe_status : kAccessViolation;
  }
  if (files.kind(slot) == DescriptorKind::Mailslot) {
    if (static_cast<uint32_t>(info_class) != 27) return kInvalidInfoClass;
    const uint32_t mailslot_status = mailslot_set_information(p, slot, 27, r.args[2], uint32_t(r.args[3]));
    return store_io_status(p, r.args[1], mailslot_status, 0) ? mailslot_status : kAccessViolation;
  }
  OpenObject* object = files.object(slot);
  if (!object) return files.kind(slot) == DescriptorKind::Closed ? kInvalidHandle : kInvalidParameter;
  const uint64_t status_block = r.args[1], address = r.args[2], length = r.args[3] & 0xFFFFFFFFu;
  uint32_t status = kSuccess;
  uint64_t value = 0;
  switch (static_cast<uint32_t>(info_class)) {
    case kFilePositionInformation:
      if (length < 8) return kInfoLengthMismatch;
      if (!p.read_user_u64(address, &value)) return kAccessViolation;
      if (int64_t(value) < 0) return kInvalidParameter;
      object->position = value;
      break;
    case kFileEndOfFileInformation:
      if (length < 8) return kInfoLengthMismatch;
      if (!p.read_user_u64(address, &value)) return kAccessViolation;
      if (int64_t(value) < 0) return kInvalidParameter;
      if (value < files.size_of(*object) && file_is_mapped(files, object->path.view())) return kUserMappedFile;      // (a mapped file cannot be made shorter)
      status = nt_status(files.resize(*object, value));
      break;
    case kFileAllocationInformation: break;      // nothing to change: sizes are exact
    case kFileBasicInformation: {      // the times (0 or -1: leave as they are) and the read-only attribute (0: leave); the file system says if it keeps none of that
      if (length < 40) return kInfoLengthMismatch;
      if (!(object->granted_access & (0x100 /* FILE_WRITE_ATTRIBUTES */ | kGenericWrite | kGenericAll))) return kAccessDenied;
      uint8_t raw[40];
      if (!p.read_user(address, ByteView(raw, 40))) return kAccessViolation;
      auto le64 = [&](uint32_t at) { uint64_t v = 0; for (uint32_t i = 0; i < 8; ++i) v |= uint64_t{raw[at + i]} << (8 * i); return v; };
      auto unix_seconds = [&](uint64_t nt, uint64_t* out) {
        if (nt == 0 || nt == ~uint64_t{0}) return false;
        if (nt / 10000000ull < kUnixToNtSeconds) { *out = 0; return true; }
        *out = nt / 10000000ull - kUnixToNtSeconds;
        return true;
      };
      filesystem::MetadataChange change;
      change.set_created = unix_seconds(le64(0), &change.created);
      change.set_modified = unix_seconds(le64(16), &change.modified);
      const uint32_t attributes = raw[32] | (uint32_t{raw[33]} << 8) | (uint32_t{raw[34]} << 16) | (uint32_t{raw[35]} << 24);
      if (attributes != 0) { change.set_read_only = true; change.read_only = (attributes & kAttributeReadOnly) != 0; }
      if (change.set_created || change.set_modified || change.set_read_only) status = nt_status(files.set_metadata(object->path.view(), change));
      break;
    }
    case kFileDispositionInformation: {
      uint8_t flag = 0;
      if (length < 1 || !p.read_user(address, ByteView(&flag, 1))) return kAccessViolation;
      object->delete_on_close = flag != 0;
      break;
    }
    case kFileRenameInformation: {
      if (length < 20) return kInfoLengthMismatch;
      uint8_t header[20];
      if (!p.read_user(address, ByteView(header, 20))) return kAccessViolation;
      const uint64_t name_bytes = header[16] | (uint64_t{header[17]} << 8) | (uint64_t{header[18]} << 16) | (uint64_t{header[19]} << 24);
      if (name_bytes % 2 != 0 || name_bytes > 2 * kMaxPath || name_bytes > length - 20) return kInvalidParameter;
      uint8_t units[2 * kMaxPath];
      if (name_bytes && !p.read_user(address + 20, ByteView(units, name_bytes))) return kAccessViolation;
      String name(files.allocator()), target(files.allocator());
      if (!util::append_utf8_from_utf16le(&name, ConstByteView(units, name_bytes))) return kNoMemory;
      if (files.resolve(name.view(), &target) != PathResult::Ok) return kObjectNameInvalid;
      DirectoryEntry existing;
      if (file_is_mapped(files, object->path.view())) return kSharingViolation;      // (a mapped file stays where it is)
      auto same_ignoring_case = [](StringView a, StringView b) {
        if (a.size() != b.size()) return false;
        for (uint64_t i = 0; i < a.size(); ++i) {
          const char x = a[i] >= 'A' && a[i] <= 'Z' ? char(a[i] + 32) : a[i], y = b[i] >= 'A' && b[i] <= 'Z' ? char(b[i] + 32) : b[i];
          if (x != y) return false;
        }
        return true;
      };
      const bool respelled = same_ignoring_case(object->path.view(), target.view()) && object->path.view() != target.view();
      if (!respelled && files.stat(target.view(), &existing) == Status::Ok) {
        if (!header[0]) return kObjectNameCollision;
        status = nt_status(files.remove(target.view()));
      }
      if (status == kSuccess) {
        String old_path(files.allocator());
        old_path.assign(object->path.view());
        status = nt_status(files.rename(object->path.view(), target.view()));
        if (status == kSuccess) {
          notify_directory_changed(old_path.view(), kFileActionRenamedOldName);
          notify_directory_changed(target.view(), kFileActionRenamedNewName);
          object->path.assign(target.view());
        }
      }
      break;
    }
    case kFileLinkInformation: {      // CreateHardLink(new name = the name in the buffer, existing = the handle this call is on)
      if (length < 20) return kInfoLengthMismatch;
      uint8_t header[20];
      if (!p.read_user(address, ByteView(header, 20))) return kAccessViolation;
      const uint64_t name_bytes = header[16] | (uint64_t{header[17]} << 8) | (uint64_t{header[18]} << 16) | (uint64_t{header[19]} << 24);
      if (name_bytes % 2 != 0 || name_bytes > 2 * kMaxPath || name_bytes > length - 20) return kInvalidParameter;
      uint8_t units[2 * kMaxPath];
      if (name_bytes && !p.read_user(address + 20, ByteView(units, name_bytes))) return kAccessViolation;
      String name(files.allocator()), target(files.allocator());
      if (!util::append_utf8_from_utf16le(&name, ConstByteView(units, name_bytes))) return kNoMemory;
      if (files.resolve(name.view(), &target) != PathResult::Ok) return kObjectNameInvalid;
      DirectoryEntry existing;
      if (files.stat(target.view(), &existing) == Status::Ok) {
        if (!header[0]) return kObjectNameCollision;
        status = nt_status(files.remove(target.view()));
      }
      if (status == kSuccess) {
        status = nt_status(files.create_hard_link(object->path.view(), target.view()));
        if (status == kSuccess) notify_directory_changed(target.view(), kFileActionAdded);
      }
      break;
    }
    default: return kInvalidInfoClass;
  }
  return store_io_status(p, status_block, status, status == kSuccess ? length : 0) ? status : kAccessViolation;
}

uint32_t nt_query_directory_file(Process& p, const SyscallRequest& r) {
  // NtQueryDirectoryFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation, Length, FileInformationClass, ReturnSingleEntry,
  //                      FileName, RestartScan)
  uint64_t status_block = 0, buffer = 0, length = 0, info_class = 0, single = 0, pattern_pointer = 0, restart = 0;
  if (!argument(p, r, 4, &status_block) || !argument(p, r, 5, &buffer) || !argument(p, r, 6, &length) || !argument(p, r, 7, &info_class) ||
      !argument(p, r, 8, &single) || !argument(p, r, 9, &pattern_pointer) || !argument(p, r, 10, &restart))
    return kAccessViolation;
  uint64_t slot = 0;
  if (!handle_slot(r.args[0], &slot)) return kInvalidHandle;
  ProcessFiles& files = p.files();
  if (files.kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  if (files.kind(slot) != DescriptorKind::Directory) return kNotADirectory;
  if (!classes_known(static_cast<uint32_t>(info_class))) return kInvalidInfoClass;
  OpenObject& object = *files.object(slot);
  length &= 0xFFFFFFFFu;
  if (restart & 0xFF) files.rewind(object);
  if (pattern_pointer != 0 && (restart & 0xFF || object.pattern.empty())) {
    const uint32_t named = read_unicode_string(p, pattern_pointer, &object.pattern);
    if (named != kSuccess) return named;
  }
  const StringView pattern = object.pattern.view();
  uint64_t used = 0, last_header = 0;
  bool any = false;
  uint8_t record[2 * (filesystem::kMaxNameLength + 1) + 128];
  for (;;) {
    DirectoryEntry entry;
    Status status = files.next_entry(object, &entry);
    if (status == Status::EndOfDirectory) break;
    if (status != Status::Ok) return any ? kSuccess : nt_status(status);
    if (!pattern.empty() && !dos_wildcard_matches(pattern, entry.name.view())) continue;
    const uint64_t size = build_directory_record(static_cast<uint32_t>(info_class), entry, ++object.directory_index, record, sizeof(record));
    if (size == 0) return kInvalidInfoClass;
    const uint64_t padded = (size + 7) & ~uint64_t{7};
    if (used + size > length) {
      --object.directory_index;
      object.pending = entry;      // it did not fit: the next call starts with it
      object.has_pending = true;
      if (!any) {
        store_io_status(p, status_block, kBufferOverflow, 0);
        return kBufferOverflow;
      }
      break;
    }
    if (any) {
      uint8_t next[4];
      put(ByteView(next, 4), 0, used - last_header, 4);
      if (!p.write_user(buffer + last_header, ConstByteView(next, 4))) return kAccessViolation;      // NextEntryOffset of the one before
    }
    if (!p.write_user(buffer + used, ConstByteView(record, size))) return kAccessViolation;
    last_header = used;
    used += used + padded > length ? size : padded;
    any = true;
    if (single & 0xFF) break;
  }
  if (!any) {
    store_io_status(p, status_block, kNoMoreFiles, 0);
    return kNoMoreFiles;
  }
  return store_io_status(p, status_block, kSuccess, used) ? kSuccess : kAccessViolation;
}

uint32_t nt_query_attributes_file(Process& p, const SyscallRequest& r) {
  // NtQueryAttributesFile(POBJECT_ATTRIBUTES, PFILE_BASIC_INFORMATION)
  ProcessFiles& files = p.files();
  if (!files.attached()) return kObjectPathNotFound;
  String virtual_path(files.allocator());
  const uint32_t named = object_path(p, r.args[0], &virtual_path);
  if (named != kSuccess) return named;
  DirectoryEntry e;
  const Status status = files.stat(virtual_path.view(), &e);
  if (status != Status::Ok) return status_for(files, status, virtual_path.view());
  return write_basic(p, r.args[1], e) ? kSuccess : kAccessViolation;
}

uint32_t nt_create_symbolic_link(Process& p, const SyscallRequest& r) {
  ProcessFiles& files = p.files();
  if (!files.attached()) return kObjectPathNotFound;
  String link_virtual(files.allocator());
  const uint32_t named = object_path(p, r.args[0], &link_virtual);
  if (named != kSuccess) return named;
  if (link_virtual.view() == StringView("/")) return kAccessDenied;
  String raw_target(files.allocator());
  const uint32_t read = read_unicode_string(p, r.args[1], &raw_target);
  if (read != kSuccess) return read;
  if (raw_target.view().empty()) return kInvalidParameter;
  String target_virtual(files.allocator());
  // A relative target is the app's own idea of "next to the link", so it is resolved against the link's directory, not the process's current directory.
  if (files.resolve_from(virtual_directory_of(link_virtual.view()), raw_target.view(), &target_virtual) != PathResult::Ok) return kObjectNameInvalid;
  const bool target_is_directory = (r.args[2] & 0xFF) != 0;
  const Status status = files.create_symbolic_link(link_virtual.view(), target_virtual.view(), target_is_directory);
  if (status == Status::Ok) notify_directory_changed(link_virtual.view(), kFileActionAdded);
  return status_for(files, status, link_virtual.view());
}

uint32_t nt_delete_file(Process& p, const SyscallRequest& r) {
  ProcessFiles& files = p.files();
  if (!files.attached()) return kObjectPathNotFound;
  String virtual_path(files.allocator());
  const uint32_t named = object_path(p, r.args[0], &virtual_path);
  if (named != kSuccess) return named;
  if (virtual_path.view() == StringView("/")) return kAccessDenied;
  if (ObjectManager* sharing = system_objects()) {
    if (!sharing->share_allows(virtual_path.view(), ObjectManager::kShareDelete)) return kSharingViolation;
    if (file_is_mapped(files, virtual_path.view())) return kCannotDelete;
    // Files that are open (and share deletion) go away when the last of them is closed.
    const void* owners[8];
    const uint32_t open_count = sharing->share_owners(virtual_path.view(), owners, 8);
    if (open_count != 0 && open_count <= 8) {
      for (uint32_t i = 0; i < open_count; ++i) const_cast<OpenObject*>(static_cast<const OpenObject*>(owners[i]))->delete_on_close = true;
      return kSuccess;
    }
  }
  const Status status = files.remove(virtual_path.view());
  if (status == Status::Ok) notify_directory_changed(virtual_path.view(), kFileActionRemoved);
  return status_for(files, status, virtual_path.view());
}

uint32_t nt_create_change_notification(Process& p, const SyscallRequest& r) {
  // NtCreateChangeNotification(POBJECT_ATTRIBUTES DirectoryAttributes, ULONG WatchTree, ULONG NotifyFilter, PHANDLE Result)
  ProcessFiles& files = p.files();
  if (!files.attached()) return kObjectPathNotFound;
  String virtual_path(files.allocator());
  const uint32_t named = object_path(p, r.args[0], &virtual_path);
  if (named != kSuccess) return named;
  DirectoryEntry e;
  const Status status = files.stat(virtual_path.view(), &e);
  if (status != Status::Ok) return status_for(files, status, virtual_path.view());
  if (e.type != NodeType::Directory) return kNotADirectory;
  if (r.args[3] == 0) return kAccessViolation;
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  const ObjectManager::Created c = objects->create_change_notification(virtual_path.view(), (r.args[1] & 0xFF) != 0, static_cast<uint32_t>(r.args[2]));
  if (c.id == 0) return c.status;
  const int64_t slot = files.open_token(DescriptorKind::ChangeNotification, c.id);
  if (slot < 0) { objects->release(c.id); return kNoResources; }
  files.set_handle_flags(static_cast<uint64_t>(slot), false, false);
  if (!p.write_user_u64(r.args[3], (static_cast<uint64_t>(slot) + 1) * 4)) { p.close_descriptor(static_cast<uint64_t>(slot)); return kAccessViolation; }
  return kSuccess;
}

uint32_t nt_notify_change_directory_file(Process& p, const SyscallRequest& r) {
  // NtNotifyChangeDirectoryFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
  // ULONG CompletionFilter, BOOLEAN WatchTree)
  uint64_t status_block = 0, buffer = 0, length = 0, filter = 0, watch_tree = 0;
  if (!argument(p, r, 4, &status_block) || !argument(p, r, 5, &buffer) || !argument(p, r, 6, &length) || !argument(p, r, 7, &filter) ||
      !argument(p, r, 8, &watch_tree))
    return kAccessViolation;
  // Event/ApcRoutine/ApcContext (args 1-3) are kernel32's job: this call is always synchronous (there is no real asynchronous I/O in this kernel), so
  // ReadDirectoryChangesW sets the event and queues the completion routine itself once this returns, exactly like it already does for ReadFileEx.
  uint64_t slot = 0;
  if (!handle_slot(r.args[0], &slot)) return kInvalidHandle;
  ProcessFiles& files = p.files();
  if (files.kind(slot) != DescriptorKind::Directory) return kInvalidHandle;
  OpenObject* object = files.object(slot);
  if (!object) return kInvalidHandle;
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  if (object->change_watch == 0) {      // made on this handle's first call, and kept (not re-made) for the rest of its life
    const ObjectManager::Created c = objects->create_change_notification(object->path.view(), (watch_tree & 0xFF) != 0, static_cast<uint32_t>(filter));
    if (c.id == 0) return c.status;
    object->change_watch = c.id;
  }
  const uint32_t watch_id = object->change_watch;
  length &= 0xFFFFFFFFu;
  if (buffer == 0 || length == 0) return kInvalidParameter;
  ObjectManager::WatchRecord records[ObjectManager::kMaxWatchRecords];
  uint32_t taken = 0;
  const uint32_t status = wait_until(p, ~uint64_t{0}, [&]() -> uint64_t {
    if (!objects->take_watch_records(watch_id, records, ObjectManager::kMaxWatchRecords, &taken) || taken == 0) return kWaitPending;
    return kSuccess;
  });
  if (status != kSuccess) return store_io_status(p, status_block, status, 0) ? status : kAccessViolation;
  // FILE_NOTIFY_INFORMATION { ULONG NextEntryOffset; ULONG Action; ULONG FileNameLength (bytes); WCHAR FileName[]; }: as many whole records as fit in Length,
  // longword-aligned (like NtQueryDirectoryFile's own record chaining); a buffer too small to hold every record this call drained loses the rest (real Windows
  // instead keeps them for ERROR_MORE_DATA/the next overlapped completion - not implemented, acceptable given a bounded 32-pending-record cap per watch anyway).
  uint8_t record[12 + 2 * (ObjectManager::kWatchNameBytes)];
  uint64_t used = 0, last_header = 0;
  uint32_t written = 0;
  for (uint32_t i = 0; i < taken; ++i) {
    const uint64_t name_bytes = to_utf16(StringView(records[i].name, records[i].name_size), record + 12, sizeof(record) - 12);
    if (name_bytes == 0 && records[i].name_size != 0) continue;      // malformed name: skip it rather than fail the whole call
    const uint64_t size = 12 + name_bytes, padded = (size + 7) & ~uint64_t{7};
    if (used + size > length) break;
    put(ByteView(record, 12), 0, 0, 4);      // NextEntryOffset: 0 unless a later record patches it (the last one really is 0, like a real one)
    put(ByteView(record, 12), 4, records[i].action, 4);
    put(ByteView(record, 12), 8, name_bytes, 4);
    if (written > 0) {
      uint8_t next[4];
      put(ByteView(next, 4), 0, used - last_header, 4);
      if (!p.write_user(buffer + last_header, ConstByteView(next, 4))) return kAccessViolation;
    }
    if (!p.write_user(buffer + used, ConstByteView(record, size))) return kAccessViolation;
    last_header = used;
    used += used + padded > length ? size : padded;
    ++written;
  }
  return store_io_status(p, status_block, kSuccess, used) ? kSuccess : kAccessViolation;
}

uint32_t nt_flush_buffers_file(Process& p, const SyscallRequest& r) {
  uint64_t slot = 0;
  if (!handle_slot(r.args[0], &slot)) return kInvalidHandle;
  ProcessFiles& files = p.files();
  if (files.kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  if (files.kind(slot) == DescriptorKind::Pipe) {
    const uint32_t pipe_status = pipe_flush(p, slot);
    return store_io_status(p, r.args[1], pipe_status, 0) ? pipe_status : kAccessViolation;
  }
  OpenObject* object = files.object(slot);
  const uint32_t status = object ? nt_status(files.flush(*object)) : kSuccess;
  return store_io_status(p, r.args[1], status, 0) ? status : kAccessViolation;
}

// NtQueryVolumeInformationFile(FileHandle, IoStatusBlock, FsInformation, Length, FsInformationClass): only FileFsDeviceInformation, which is how GetFileType tells a
// console (FILE_DEVICE_CONSOLE) from a file (FILE_DEVICE_DISK).
uint32_t nt_query_volume_information_file(Process& p, const SyscallRequest& r) {
  uint64_t info_class = 0;
  if (!argument(p, r, 4, &info_class)) return kAccessViolation;
  uint64_t slot = 0;
  if (!handle_slot(r.args[0], &slot)) return kInvalidHandle;
  const DescriptorKind kind = p.files().kind(slot);
  if (kind == DescriptorKind::Closed) return kInvalidHandle;
  switch (kind) {      // an event, a section, a thread ... is not a file
    case DescriptorKind::Event: case DescriptorKind::Mutex: case DescriptorKind::Semaphore: case DescriptorKind::Process: case DescriptorKind::Thread: case DescriptorKind::Timer:
    case DescriptorKind::Section: case DescriptorKind::Job: case DescriptorKind::Key: case DescriptorKind::ChangeNotification: return 0xC0000024u;      // STATUS_OBJECT_TYPE_MISMATCH
    default: break;
  }
  const uint32_t volume_class = static_cast<uint32_t>(info_class);      // (a ULONG: the upper half of the argument is not defined)
  if (volume_class == 1 || volume_class == 3 || volume_class == 5 || volume_class == 7) {      // volume, size, attribute and full size information: what the file system underneath says
    if (!p.files().attached() || !p.files().file_system()) return kInvalidDeviceRequest;
    const filesystem::FileSystemInfo fs = p.files().file_system()->info();
    constexpr uint64_t kUnit = 4096, kSector = 512;
    const uint64_t total_units = fs.total_bytes ? fs.total_bytes / kUnit : (4ull << 30) / kUnit;      // (a file system without a limit is told to have 4 GiB)
    const uint64_t free_units = fs.total_bytes ? fs.free_bytes / kUnit : total_units;
    uint8_t out[160] = {};
    ByteView v(out, sizeof(out));
    uint64_t size = 0;
    auto utf16 = [&](StringView text, uint64_t at) { uint64_t n = 0; for (char c : text) { out[at + 2 * n] = uint8_t(c); ++n; } return n * 2; };
    if (volume_class == 1) {      // FILE_FS_VOLUME_INFORMATION { VolumeCreationTime, VolumeSerialNumber, VolumeLabelLength, SupportsObjects, VolumeLabel[] }
      const uint64_t bytes = utf16(fs.label.view(), 18);
      put(v, 8, 0x544F5343u, 4);
      put(v, 12, bytes, 4);
      size = 18 + bytes;
    } else if (volume_class == 3) {      // FILE_FS_SIZE_INFORMATION
      put(v, 0, total_units, 8); put(v, 8, free_units, 8); put(v, 16, kUnit / kSector, 4); put(v, 20, kSector, 4);
      size = 24;
    } else if (volume_class == 5) {      // FILE_FS_ATTRIBUTE_INFORMATION { FileSystemAttributes, MaximumComponentNameLength, FileSystemNameLength, FileSystemName[] }
      const uint64_t bytes = utf16(fs.type_name.view(), 12);
      put(v, 0, 0x3 | 0x4 | 0x40000 * 0 | (fs.read_only ? 0x80000 : 0), 4);      // case-sensitive search, case preserving names, (read-only volume)
      put(v, 4, 255, 4);
      put(v, 8, bytes, 4);
      size = 12 + bytes;
    } else {                  // FILE_FS_FULL_SIZE_INFORMATION
      put(v, 0, total_units, 8); put(v, 8, free_units, 8); put(v, 16, free_units, 8); put(v, 24, kUnit / kSector, 4); put(v, 28, kSector, 4);
      size = 32;
    }
    if ((r.args[3] & 0xFFFFFFFFu) < size) return kInfoLengthMismatch;
    if (!p.write_user(r.args[2], ConstByteView(out, size))) return kAccessViolation;
    return store_io_status(p, r.args[1], kSuccess, size) ? kSuccess : kAccessViolation;
  }
  if (volume_class != 4) return kInvalidInfoClass;      // FileFsDeviceInformation
  if ((r.args[3] & 0xFFFFFFFFu) < 8) return kInfoLengthMismatch;
  const bool console = kind == DescriptorKind::ConsoleInput || kind == DescriptorKind::ConsoleOutput || kind == DescriptorKind::ConsoleError;
  uint8_t b[8] = {};
  put(ByteView(b, 8), 0, console ? 0x50 : (kind == DescriptorKind::Pipe ? 0x11 : kind == DescriptorKind::Null ? 0x15 : kind == DescriptorKind::Mailslot ? 0x0c : 7), 4);      // DeviceType: FILE_DEVICE_CONSOLE / FILE_DEVICE_NAMED_PIPE / FILE_DEVICE_DISK
  if (!p.write_user(r.args[2], ConstByteView(b, 8))) return kAccessViolation;
  return store_io_status(p, r.args[1], kSuccess, 8) ? kSuccess : kAccessViolation;
}

}  // namespace process::windowsabi
