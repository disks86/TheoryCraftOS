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
#include "Process/WindowsProcess.hpp"
#include "Process/Objects.hpp"
#include "Process/WindowsFiles.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/Utf16.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kNotImplemented = 0xC0000002u, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du,
                   kNoMemory = 0xC0000017u, kObjectNameNotFound = 0xC0000034u, kInvalidImageFormat = 0xC000007Bu, kNoResources = 0xC000009Au,
                   kNameTooLong = 0xC0000106u;
constexpr uint64_t kMaxImageBytes = 64ull << 20, kMaxEnvironmentBytes = 64 * 1024, kMaxStringBytes = 2 * 8192;
constexpr uint32_t kMaxEnvironmentEntries = 512, kCreateSuspended = 1;

bool slot_of(uint64_t handle, uint64_t* slot) {
  if (handle == 0 || handle % 4 != 0 || handle / 4 > Process::kMaxDescriptors) return false;
  *slot = handle / 4 - 1;
  return true;
}

// Reads a UNICODE_STRING into UTF-8. An absent string (address 0) is empty.
uint32_t read_text(Process& p, uint64_t address, String* out) {
  out->clear();
  if (address == 0) return kSuccess;
  uint8_t header[16];
  if (!p.read_user(address, ByteView(header, sizeof(header)))) return kAccessViolation;
  const uint64_t length = header[0] | (uint64_t{header[1]} << 8);
  uint64_t buffer = 0;
  for (uint32_t i = 0; i < 8; ++i) buffer |= uint64_t{header[8 + i]} << (8 * i);
  if (length % 2 != 0 || length > kMaxStringBytes) return kNameTooLong;
  if (length == 0) return kSuccess;
  uint8_t units[kMaxStringBytes];
  if (!p.read_user(buffer, ByteView(units, length))) return kAccessViolation;
  return util::append_utf8_from_utf16le(out, ConstByteView(units, length)) ? kSuccess : kNoMemory;
}

// The environment block (UTF-16 strings, an empty one at the end) as UTF-8 text with one NUL after each entry; `entries` counts them.
uint32_t read_environment(Process& p, uint64_t address, String* text, uint32_t* entries) {
  text->clear();
  *entries = 0;
  if (address == 0) return kSuccess;
  if (address % 2 != 0) return kInvalidParameter;
  String entry(p.files().allocator());
  uint8_t chunk[4096];
  uint8_t pending[2 * 4096];      // the UTF-16 bytes of the entry being read
  uint64_t pending_size = 0, total = 0;
  for (;;) {
    const uint64_t room = 4096 - (address & 4095);      // (up to the end of the page: the block may end right before unmapped memory)
    if (!p.read_user(address, ByteView(chunk, room))) return kAccessViolation;
    for (uint64_t i = 0; i + 1 < room; i += 2) {
      if (chunk[i] != 0 || chunk[i + 1] != 0) {
        if (pending_size + 2 > sizeof(pending)) return kNameTooLong;
        pending[pending_size++] = chunk[i];
        pending[pending_size++] = chunk[i + 1];
        continue;
      }
      if (pending_size == 0) return kSuccess;      // the empty string: the end of the block
      entry.clear();
      if (!util::append_utf8_from_utf16le(&entry, ConstByteView(pending, pending_size))) return kNoMemory;
      pending_size = 0;
      if (*entries >= kMaxEnvironmentEntries) return kNameTooLong;
      if (!text->append(entry.view()) || !text->push_back('\0')) return kNoMemory;
      ++*entries;
    }
    total += room;
    if (total > kMaxEnvironmentBytes) return kNameTooLong;
    address += room;
  }
}

bool read_image(Process& p, StringView path, ByteBuffer* out, uint32_t* status) {
  String virtual_path(p.files().allocator());
  if (p.files().resolve(path, &virtual_path) != PathResult::Ok) { *status = kObjectNameNotFound; return false; }
  OpenFlags flags;
  flags.read = true;
  flags.must_not_be_directory = true;
  uint32_t slot = 0;
  if (p.files().open(virtual_path.view(), flags, &slot) != filesystem::Status::Ok) { *status = kObjectNameNotFound; return false; }
  OpenObject* file = p.files().object(slot);
  bool ok = false;
  const uint64_t size = file ? p.files().size_of(*file) : 0;
  if (size == 0 || size > kMaxImageBytes) {
    *status = size == 0 ? kInvalidImageFormat : kNoMemory;
  } else if (!out->put_zeros(size)) {
    *status = kNoMemory;
  } else {
    ByteView bytes = out->writable_view();
    ok = true;
    for (uint64_t done = 0; done < size && ok;) {
      uint64_t got = 0;
      const uint64_t want = size - done < (1u << 20) ? size - done : (1u << 20);
      if (p.files().read(*file, done, bytes.subview(done, want), &got) != filesystem::Status::Ok || got == 0) { *status = kInvalidImageFormat; ok = false; break; }
      done += got;
    }
  }
  p.files().close(slot);
  return ok;
}

// A handle for the process object in the table, written to `at`. Consumes one reference of the object whatever happens.
uint32_t give_handle(Process& p, ObjectManager& objects, uint32_t object, uint64_t at, uint64_t* slot_out) {
  const int64_t slot = p.files().open_token(DescriptorKind::Process, object);
  if (slot < 0) { objects.release(object); return kNoResources; }
  *slot_out = uint64_t(slot);
  if (!p.write_user_u64(at, (uint64_t(slot) + 1) * 4)) {
    p.close_descriptor(uint64_t(slot));      // (releases the object)
    return kAccessViolation;
  }
  return kSuccess;
}

}  // namespace

bool process_handle_info(Process& p, uint64_t handle, bool* exited, uint32_t* code, uint64_t* pid) {
  uint64_t slot = 0;
  ObjectManager* objects = system_objects();
  if (!objects || !slot_of(handle, &slot) || p.files().kind(slot) != DescriptorKind::Process) return false;
  return objects->query_process(uint32_t(p.files().token(slot)), exited, code, pid);
}

uint32_t terminate_process_handle(Process& p, uint64_t handle, uint32_t code, bool* handled) {
  bool exited = false;
  uint32_t old_code = 0;
  uint64_t pid = 0;
  *handled = process_handle_info(p, handle, &exited, &old_code, &pid);
  if (!*handled) return kInvalidHandle;
  if (exited) return 0xC0000022u;      // STATUS_ACCESS_DENIED: it has ended
  if (pid == p.id()) { p.terminate(code); return kSuccess; }
  return p.runtime() && p.runtime()->terminate_process(pid, code) ? kSuccess : 0xC0000022u;
}

uint32_t resume_process_handle(Process& p, uint64_t handle, uint32_t* previous, bool* handled) {
  bool exited = false;
  uint32_t code = 0;
  uint64_t pid = 0;
  *handled = process_handle_info(p, handle, &exited, &code, &pid);
  if (!*handled) return kInvalidHandle;
  *previous = 1;      // (it was suspended, or its count is already 0)
  if (exited || !p.runtime()) return kSuccess;
  if (p.runtime()->resume_process(pid)) return kSuccess;
  *previous = 0;
  return kSuccess;
}

uint32_t nt_create_user_process(Process& p, const SyscallRequest& r) {
  uint64_t cwd_at = 0, environment_at = 0, flags_arg = 0, pid_out = 0, tid_out = 0;
  if (!argument(p, r, 4, &cwd_at) || !argument(p, r, 5, &environment_at) || !argument(p, r, 6, &flags_arg) || !argument(p, r, 7, &pid_out) || !argument(p, r, 8, &tid_out)) return kAccessViolation;
  ObjectManager* objects = system_objects();
  IProcessRuntime* runtime = p.runtime();
  if (!objects || !runtime || !p.files().attached()) return kNotImplemented;
  if (r.args[0] == 0 || r.args[1] == 0) return kInvalidParameter;
  allocators::IAllocator& allocator = p.files().allocator();
  String image_path(allocator), command_line(allocator), directory(allocator), environment(allocator), world_directory(allocator);
  uint32_t status = read_text(p, r.args[2], &image_path);
  if (status == kSuccess) status = read_text(p, r.args[3], &command_line);
  if (status == kSuccess) status = read_text(p, cwd_at, &directory);
  uint32_t entry_count = 0;
  if (status == kSuccess) status = read_environment(p, environment_at, &environment, &entry_count);
  if (status != kSuccess) return status;
  if (image_path.empty()) return kInvalidParameter;
  if (directory.empty() ? !world_directory.assign(p.files().cwd()) : p.files().resolve(directory.view(), &world_directory) != PathResult::Ok) return kObjectNameNotFound;
  ByteBuffer image(allocator);
  if (!read_image(p, image_path.view(), &image, &status)) return status;
  StringView entries[kMaxEnvironmentEntries];
  uint32_t found = 0;
  for (uint64_t at = 0; at < environment.size() && found < entry_count;) {
    const StringView rest = environment.view().without_prefix(at);
    const uint64_t length = rest.find('\0');
    entries[found++] = rest.first(length);
    at += length + 1;
  }
  ChildSpec spec;
  spec.pid = runtime->allocate_process_id();
  if (spec.pid == 0) return kNoResources;
  spec.image = image.view();
  spec.image_path = image_path.view();
  spec.command_line = command_line.view();
  spec.working_directory = world_directory.view();
  spec.environment = ArrayView<const StringView>(entries, found);
  spec.suspended = (flags_arg & kCreateSuspended) != 0;
  const ObjectManager::Created created = objects->create_process(spec.pid);
  if (created.id == 0) return created.status;
  objects->add_ref(created.id);      // (one reference for each of the two handles)
  status = runtime->spawn_process(spec);
  if (status != kSuccess) {
    objects->release(created.id);
    objects->release(created.id);
    return status;
  }
  uint64_t process_slot = 0, thread_slot = 0;
  const uint32_t first = give_handle(p, *objects, created.id, r.args[0], &process_slot);
  const uint32_t second = give_handle(p, *objects, created.id, r.args[1], &thread_slot);
  if (first != kSuccess || second != kSuccess) {      // (it has been started already: it is stopped again)
    if (first == kSuccess) p.close_descriptor(process_slot);
    if (second == kSuccess) p.close_descriptor(thread_slot);
    runtime->terminate_process(spec.pid, 1);
    return first != kSuccess ? first : second;
  }
  const uint8_t id[4] = {uint8_t(spec.pid), uint8_t(spec.pid >> 8), uint8_t(spec.pid >> 16), uint8_t(spec.pid >> 24)};
  if (pid_out != 0 && !p.write_user(pid_out, ConstByteView(id, 4))) return kAccessViolation;
  if (tid_out != 0 && !p.write_user(tid_out, ConstByteView(id, 4))) return kAccessViolation;
  return kSuccess;
}

}  // namespace process::windowsabi
