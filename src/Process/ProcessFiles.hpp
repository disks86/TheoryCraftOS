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
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

namespace process {

// A program sees one tree of its own. The OS keeps the trees of the two worlds side by side in its file system: "/Linux" is what a Linux program calls "/",
// and "/Windows" is what a Windows program calls "C:\". A path a program writes is first made absolute against its current directory, then cleaned ("." and
// ".." removed, ".." never climbing above the program's root), and only then joined to the root: a program cannot name anything outside its tree.
enum class PathStyle : uint8_t {
  Posix,      // "/", names are bytes (UTF-8), case counts as the file system says
  Windows,    // "C:\dir\file", "\\??\\C:\dir\file" (NT), either slash, drive C only, no < > : " | ? * in names
};
enum class PathResult : uint8_t { Ok, Empty, TooLong, Invalid };
constexpr uint64_t kMaxPath = 4096;

// Makes `path` absolute against `cwd` (both in the cleaned form this function produces: "/" or "/a/b") and cleans it. `*out` is the cleaned virtual path.
PathResult resolve_path(PathStyle style, StringView cwd, StringView path, String* out);

// What a descriptor (Linux file descriptor, Windows handle) refers to.
enum class DescriptorKind : uint8_t { Closed, ConsoleInput, ConsoleOutput, ConsoleError, File, Directory, Null, Zero, Random, Thread, Event, Mutex, Semaphore, Process, Pipe, Timer, Section, Job, Socket, ChangeNotification, Mailslot, Key };

// How open() treats the path. (The Linux O_* and Windows dispositions both reduce to these.)
struct OpenFlags {
  bool read = false, write = false;
  bool create = false;              // make the file if it is missing
  bool exclusive = false;           // with create: fail if it exists
  bool truncate = false;            // empty it
  bool append = false;              // every write goes to the end
  bool must_be_directory = false;   // fail on a file (and create a directory instead of a file)
  bool must_not_be_directory = false;
};

// An open file or directory, shared by the descriptors that were duplicated from each other (they share the position too).
struct OpenObject {
  explicit OpenObject(allocators::IAllocator& allocator) : path(allocator), pattern(allocator) {}
  UniquePointer<filesystem::IFile> file;
  UniquePointer<filesystem::IDirectory> directory;
  String path;                       // the cleaned virtual path it was opened by
  String pattern;                    // directory listing: only names matching this (Windows wildcards); empty = all
  uint64_t position = 0;
  bool readable = false, writable = false, append = false;
  uint32_t granted_access = 0xFFFFFFFFu;      // the access mask it was opened with (Windows; a handle made some other way has every right)
  bool delete_on_close = false;
  // Directory listing: 0 = "." next, 1 = ".." next, 2 = the real entries.
  uint32_t directory_phase = 0;
  uint64_t directory_index = 0;      // entries handed out so far (a cookie for getdents)
  filesystem::DirectoryEntry pending;
  bool has_pending = false;          // an entry that was read but did not fit and waits for the next call
  uint32_t change_watch = 0;         // ReadDirectoryChangesW's own change-notification object for this directory handle, made on its first call; 0 = none yet
};

// The descriptor table and file operations of one process. Console descriptors 0..2 exist from the start; the file system is attached once.
class ProcessFiles {
 public:
  static constexpr uint32_t kMaxDescriptors = 256;

  explicit ProcessFiles(PathStyle style);
  ProcessFiles(const ProcessFiles&) = delete;
  ProcessFiles& operator=(const ProcessFiles&) = delete;

  // `root_prefix` is the folder of `fs` that is this process's root ("/Linux"). The file system must outlive the process's open files.
  void attach(allocators::IAllocator& allocator, filesystem::IFileSystem& fs, StringView root_prefix);
  bool attached() const { return fs_ != nullptr; }
  filesystem::IFileSystem* file_system() const { return fs_; }
  // The allocator given to attach() (for the temporary strings of the callers); only valid when attached().
  allocators::IAllocator& allocator() const { return *allocator_; }
  PathStyle style() const { return style_; }

  // --- descriptors
  DescriptorKind kind(uint64_t slot) const { return slot < kMaxDescriptors ? slots_[slot].kind : DescriptorKind::Closed; }
  OpenObject* object(uint64_t slot) const { return slot < kMaxDescriptors ? slots_[slot].object.get() : nullptr; }
  // Closes it; a file marked delete-on-close is removed with its last descriptor. False if it was not open.
  bool close(uint64_t slot);
  // A new descriptor for the same object, the lowest free one at or above `minimum`; -1 if the table is full.
  int64_t duplicate(uint64_t slot, uint64_t minimum);
  // Makes `target` refer to what `slot` does (closing what it had). False if either is invalid.
  bool duplicate_to(uint64_t slot, uint64_t target);
  bool close_on_exec(uint64_t slot) const { return slot < kMaxDescriptors && slots_[slot].close_on_exec; }
  void set_close_on_exec(uint64_t slot, bool value) { if (slot < kMaxDescriptors) slots_[slot].close_on_exec = value; }
  uint32_t denied_access(uint64_t slot) const { return slot < kMaxDescriptors ? slots_[slot].denied : 0; }
  void set_denied_access(uint64_t slot, uint32_t denied) { if (slot < kMaxDescriptors) slots_[slot].denied = denied; }
  bool inheritable(uint64_t slot) const { return slot < kMaxDescriptors && slots_[slot].inherit; }
  bool protected_from_close(uint64_t slot) const { return slot < kMaxDescriptors && slots_[slot].protect; }
  void set_handle_flags(uint64_t slot, bool inherit, bool protect) { if (slot < kMaxDescriptors) { slots_[slot].inherit = inherit; slots_[slot].protect = protect; } }
  // A new process's table gets the handles of `parent` that are inheritable, at the same numbers (what a Windows child receives). Console descriptors stay as they are.
  // `copy_token` is called for each token-carrying kind so the owner can count the extra use; returns how many were copied.
  uint32_t inherit_from(const ProcessFiles& parent, void (*copy_token)(DescriptorKind kind, uint64_t token));
  // Opens a device node as a descriptor (the Null/Zero/Random kinds).
  int64_t open_device(DescriptorKind kind);
  // A descriptor for something the process keeps track of itself (a thread): `token` says which one. -1 if the table is full.
  int64_t open_token(DescriptorKind kind, uint64_t token);
  void set_token(uint64_t slot, uint64_t value) { if (slot < kMaxDescriptors) slots_[slot].token = value; }
  uint64_t token(uint64_t slot) const { return slot < kMaxDescriptors ? slots_[slot].token : 0; }
  // The lowest free descriptor at or above `minimum`; -1 if none.
  int64_t free_slot(uint64_t minimum) const;

  // --- names
  StringView cwd() const { return StringView(cwd_, cwd_size_); }
  PathResult resolve(StringView path, String* virtual_path) const { return resolve_path(style_, cwd(), path, virtual_path); }
  PathResult resolve_from(StringView base, StringView path, String* virtual_path) const { return resolve_path(style_, base, path, virtual_path); }
  // The device node a cleaned Linux path names (/dev/null, /dev/zero, /dev/urandom, /dev/random, /dev/tty), or Closed.
  DescriptorKind device_at(StringView virtual_path) const;

  // --- operations on cleaned virtual paths
  filesystem::Status open(StringView virtual_path, const OpenFlags& flags, uint32_t* slot, bool* created = nullptr);
  filesystem::Status stat(StringView virtual_path, filesystem::DirectoryEntry* entry);
  filesystem::Status make_directory(StringView virtual_path);
  filesystem::Status set_metadata(StringView virtual_path, const filesystem::MetadataChange& change);
  filesystem::Status remove(StringView virtual_path);
  filesystem::Status rename(StringView from, StringView to);
  filesystem::Status change_directory(StringView virtual_path);
  // Hard and symbolic links (CreateHardLink, CreateSymbolicLink): Unsupported if the mounted file system has no way to make one.
  filesystem::Status create_hard_link(StringView existing, StringView link);
  filesystem::Status create_symbolic_link(StringView link, StringView target, bool target_is_directory);

  // --- operations on open files (the object must be of the right kind)
  filesystem::Status read(OpenObject& object, uint64_t offset, ByteView out, uint64_t* got);
  filesystem::Status write(OpenObject& object, uint64_t offset, ConstByteView data, uint64_t* done);
  filesystem::Status resize(OpenObject& object, uint64_t size);
  filesystem::Status flush(OpenObject& object);
  uint64_t size_of(const OpenObject& object) const { return object.file ? object.file->size() : 0; }
  // The metadata of an open file or directory.
  filesystem::Status stat_object(const OpenObject& object, filesystem::DirectoryEntry* entry);
  // Next entry of a directory, "." and ".." first. Ok, EndOfDirectory, or an error. `pending` entries are kept by the caller via the object.
  filesystem::Status next_entry(OpenObject& object, filesystem::DirectoryEntry* entry);
  void rewind(OpenObject& object);

 private:
  struct Slot {
    DescriptorKind kind = DescriptorKind::Closed;
    SharedPointer<OpenObject> object;
    uint64_t token = 0;
    bool close_on_exec = false;
    uint32_t denied = 0;      // access rights the handle lacks (a duplicate made with fewer rights than the original); 0 for a handle with all its object's rights
    bool inherit = false, protect = false;      // Windows handle flags: a child process gets the handle (CreateProcess with bInheritHandles); it cannot be closed
  };
  bool host_path(StringView virtual_path, String* out) const;
  bool fold_case(String* path, uint64_t prefix_size) const;

 public:
  // The path in the OS file system of a path inside this process's tree (what the file system underneath is asked for).
  bool host_path_of(StringView virtual_path, String* out) const { return host_path(virtual_path, out); }

 private:
  PathStyle style_;
  Slot slots_[kMaxDescriptors];
  allocators::IAllocator* allocator_ = nullptr;
  filesystem::IFileSystem* fs_ = nullptr;
  StringView root_prefix_;
  char cwd_[kMaxPath + 1] = {'/'};      // the cleaned virtual path of the current directory
  uint64_t cwd_size_ = 1;
};

// Makes sure the two trees exist in the OS's file system, each with the folders programs expect: "/Linux" (bin etc home root tmp usr var) and "/Windows"
// (Windows, Windows/System32, Users, Program Files, Temp). Missing folders are created, existing ones kept. False if a folder could not be made (a file system
// that cannot be written, say); programs then see whatever is there.
bool prepare_program_folders(filesystem::IFileSystem& fs);

}  // namespace process
