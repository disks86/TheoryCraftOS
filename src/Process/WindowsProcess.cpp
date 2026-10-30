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
#include "Process/AddressSpace.hpp"
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
constexpr uint32_t kMaxEnvironmentEntries = 512, kCreateSuspended = 1, kInheritHandles = 2, kUseStdHandles = 4, kBreakawayFromJob = 8;

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

}  // namespace

bool read_program_file(Process& p, StringView path, ByteBuffer* out, uint32_t* status) {
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

namespace {

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

bool names_own_process(Process& p, uint64_t handle) {
  if (handle == ~uint64_t{0}) return true;
  bool exited = false;
  uint32_t code = 0;
  uint64_t pid = 0;
  return process_handle_info(p, handle, &exited, &code, &pid) && pid == p.id();
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
  uint64_t std_handles[3] = {}, startup_at = 0;
  if (!argument(p, r, 4, &cwd_at) || !argument(p, r, 5, &environment_at) || !argument(p, r, 6, &flags_arg) || !argument(p, r, 7, &pid_out) || !argument(p, r, 8, &tid_out) ||
      !argument(p, r, 9, &std_handles[0]) || !argument(p, r, 10, &std_handles[1]) || !argument(p, r, 11, &std_handles[2]) || !argument(p, r, 12, &startup_at))
    return kAccessViolation;
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
  if (!read_program_file(p, image_path.view(), &image, &status)) return status;
  StringView entries[kMaxEnvironmentEntries];
  uint32_t found = 0;
  for (uint64_t at = 0; at < environment.size() && found < entry_count;) {
    const StringView rest = environment.view().without_prefix(at);
    const uint64_t length = rest.find('\0');
    entries[found++] = rest.first(length);
    at += length + 1;
  }
  // The startup block: ULONG size, flags, x, y, xsize, ysize, xchars, ychars, fill, show, console flags, reserved; HANDLE console; PUNICODE_STRING title, desktop.
  String startup_title(allocator), startup_desktop(allocator);
  uint32_t startup_words[12] = {};
  uint64_t startup_console = 0;
  if (startup_at != 0) {
    uint8_t block[48 + 24];
    if (!p.read_user(startup_at, ByteView(block, sizeof(block)))) return kAccessViolation;
    for (uint32_t i = 0; i < 12; ++i) startup_words[i] = block[4 * i] | (uint32_t{block[4 * i + 1]} << 8) | (uint32_t{block[4 * i + 2]} << 16) | (uint32_t{block[4 * i + 3]} << 24);
    uint64_t pointers[3] = {};
    for (uint32_t k = 0; k < 3; ++k) for (uint32_t i = 0; i < 8; ++i) pointers[k] |= uint64_t{block[48 + 8 * k + i]} << (8 * i);
    startup_console = pointers[0];
    status = read_text(p, pointers[1], &startup_title);
    if (status == kSuccess) status = read_text(p, pointers[2], &startup_desktop);
    if (status != kSuccess) return status;
  }
  ChildSpec spec;
  spec.startup.flags = startup_words[1]; spec.startup.x = startup_words[2]; spec.startup.y = startup_words[3]; spec.startup.x_size = startup_words[4]; spec.startup.y_size = startup_words[5];
  spec.startup.x_chars = startup_words[6]; spec.startup.y_chars = startup_words[7]; spec.startup.fill_attribute = startup_words[8]; spec.startup.show_window = startup_words[9];
  spec.startup.console_flags = startup_words[10]; spec.startup.console_handle = startup_console;
  spec.startup.title = startup_title.view();
  spec.startup.desktop = startup_desktop.view();
  spec.pid = runtime->allocate_process_id();
  if (spec.pid == 0) return kNoResources;
  spec.image = image.view();
  spec.image_path = image_path.view();
  spec.command_line = command_line.view();
  spec.working_directory = world_directory.view();
  spec.environment = ArrayView<const StringView>(entries, found);
  spec.suspended = (flags_arg & kCreateSuspended) != 0;
  spec.inherit_handles = (flags_arg & kInheritHandles) != 0;
  spec.use_std_handles = (flags_arg & kUseStdHandles) != 0;
  if (spec.use_std_handles) {
    for (uint32_t i = 0; i < 3; ++i) {
      uint64_t slot = 0;
      if (!slot_of(std_handles[i], &slot) || p.files().kind(slot) == DescriptorKind::Closed) {
        if (std_handles[i] == 0 || std_handles[i] == ~uint64_t{0}) { spec.std_handles[i] = std_handles[i]; continue; }      // none
        return kInvalidHandle;
      }
      if (slot > 2 && (!spec.inherit_handles || !p.files().inheritable(slot))) return kInvalidHandle;      // (the child only has what it inherits)
      spec.std_handles[i] = std_handles[i];
    }
  }
  const ObjectManager::Created created = objects->create_process(spec.pid);
  if (created.id == 0) return created.status;
  const uint32_t joined = objects->job_inherit(p.id(), spec.pid, (flags_arg & kBreakawayFromJob) != 0);      // the child belongs to the job of the one that starts it
  if (joined != kSuccess) { objects->release(created.id); return joined; }
  objects->add_ref(created.id);      // (one reference for each of the two handles)
  status = runtime->spawn_process(spec);
  if (status != kSuccess) {
    objects->job_forget(spec.pid);
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

namespace {

constexpr uint32_t kInvalidInfoClass = 0xC0000003u, kInfoLengthMismatch = 0xC0000004u, kPortNotSet = 0xC0000353u;
constexpr uint64_t kCurrentProcessHandle = ~uint64_t{0};

// The Win32 priority class constants and NT's numbers for them (PROCESS_PRIORITY_CLASS.PriorityClass).
uint32_t nt_priority_class(uint32_t win32) {
  switch (win32) {
    case 0x40: return 1;       // IDLE
    case 0x4000: return 5;     // BELOW_NORMAL
    case 0x8000: return 6;     // ABOVE_NORMAL
    case 0x80: return 3;       // HIGH
    case 0x100: return 4;      // REALTIME
    default: return 2;         // NORMAL
  }
}
uint32_t win32_priority_class(uint32_t nt) {
  switch (nt) {
    case 1: return 0x40;
    case 5: return 0x4000;
    case 6: return 0x8000;
    case 3: return 0x80;
    case 4: return 0x100;
    default: return 0x20;
  }
}

}  // namespace

// The base level of each priority class (the kernel scheduler's absolute priority a THREAD_PRIORITY_NORMAL thread of that class runs at), and how a relative thread
// priority offsets it; at the two extremes (idle, time-critical) real Windows saturates to an absolute level instead of adding an offset, which this keeps.
int32_t absolute_thread_priority(uint32_t win32_class, int32_t relative) {
  const bool realtime = win32_class == 0x100;
  uint32_t base = 8;                          // NORMAL_PRIORITY_CLASS
  switch (win32_class) {
    case 0x40: base = 4; break;                // IDLE_PRIORITY_CLASS
    case 0x4000: base = 6; break;               // BELOW_NORMAL_PRIORITY_CLASS
    case 0x8000: base = 10; break;              // ABOVE_NORMAL_PRIORITY_CLASS
    case 0x80: base = 13; break;                // HIGH_PRIORITY_CLASS
    case 0x100: base = 24; break;               // REALTIME_PRIORITY_CLASS
    default: break;
  }
  if (relative <= -15) return realtime ? 16 : 1;      // THREAD_PRIORITY_IDLE
  if (relative >= 15) return realtime ? 31 : 15;      // THREAD_PRIORITY_TIME_CRITICAL
  int32_t level = int32_t(base) + relative;
  if (level < 1) level = 1;
  if (level > 31) level = 31;
  return level;
}

void repriority_threads(Process& p) {
  IProcessRuntime* runtime = p.runtime();
  if (!runtime) return;
  for (uint32_t i = 0; i < Process::kMaxThreads; ++i) {
    Process::ThreadRecord* record = p.thread_at(i);
    if (record && record->used && !record->finished) runtime->set_priority(record->id, absolute_thread_priority(p.priority_class, record->priority));
  }
}

namespace {

struct Buffer {
  uint8_t bytes[128] = {};
  void put(uint32_t at, uint64_t value, uint32_t size) { for (uint32_t i = 0; i < size; ++i) bytes[at + i] = uint8_t(value >> (8 * i)); }
};

}  // namespace

uint32_t nt_query_information_process(Process& p, const SyscallRequest& r) {
  uint64_t returned = 0;
  if (!argument(p, r, 4, &returned)) return kAccessViolation;
  const uint32_t info_class = uint32_t(r.args[1]), length = uint32_t(r.args[3]);      // (ULONGs: the upper halves of the arguments are not defined)
  bool child_exited = false;
  uint32_t child_code = 0;
  uint64_t child_pid = 0;
  bool self = r.args[0] == kCurrentProcessHandle;
  if (!self) {
    if (!process_handle_info(p, r.args[0], &child_exited, &child_code, &child_pid)) return kInvalidHandle;
    self = child_pid == p.id();      // (OpenProcess on the own id gives a handle to this very process)
  }
  uint64_t remote_peb = 0;
  uint8_t remote_image[64] = {};
  if (!self && !child_exited && p.runtime() && (info_class == 0 || info_class == 37)) {      // (another program's PEB and image are not ours to look into: it tells)
    IProcessRuntime::RemoteOp op;
    op.kind = IProcessRuntime::RemoteOp::ProcessInfo;
    if (p.runtime()->remote_operation(child_pid, op) == kSuccess) {
      remote_peb = op.address;
      for (uint32_t i = 0; i < 64; ++i) remote_image[i] = op.image[i];
    }
  }
  Buffer b;
  uint32_t size = 0;
  IProcessRuntime* runtime = p.runtime();
  if (info_class == 0) {      // PROCESS_BASIC_INFORMATION
    uint64_t peb = 0;
    if (self) p.read_user_u64(p.gs_base() + 0x60, &peb);      // (another process's address space is not ours to look into)
    b.put(0, self ? 259 : (child_exited ? child_code : 259u), 8);      // ExitStatus (STILL_ACTIVE)
    b.put(8, self ? peb : remote_peb, 8);                               // PebBaseAddress
    b.put(16, runtime ? runtime->affinity(0) : 1, 8);                   // AffinityMask
    b.put(24, 8, 8);                                                    // BasePriority
    b.put(32, self ? p.id() : child_pid, 8);                            // UniqueProcessId
    b.put(40, self ? p.parent_id() : p.id(), 8);                        // InheritedFromUniqueProcessId
    size = 48;
  } else if (info_class == 37) {      // ProcessImageInformation: SECTION_IMAGE_INFORMATION
    const uint8_t(&image)[64] = self ? p.image_information() : remote_image;
    for (uint32_t i = 0; i < 64; ++i) b.bytes[i] = image[i];
    size = 64;
  } else if (!self) {
    return kInvalidInfoClass;
  } else {
    switch (info_class) {
      case 2: size = 48; break;      // IO_COUNTERS (nothing is counted)
      case 3: {      // VM_COUNTERS_EX
        const uint64_t mapped = p.address_space() ? p.address_space()->mapped_bytes() : 0;
        b.put(0, mapped, 8);         // PeakVirtualSize
        b.put(8, mapped, 8);         // VirtualSize
        b.put(24, mapped, 8);        // PeakWorkingSetSize
        b.put(32, mapped, 8);        // WorkingSetSize
        b.put(72, mapped, 8);        // PagefileUsage
        b.put(80, mapped, 8);        // PeakPagefileUsage
        b.put(88, mapped, 8);        // PrivateUsage
        size = length >= 96 ? 96 : 88;
        break;
      }
      case 4: {      // KERNEL_USER_TIMES
        uint64_t used = 0;
        if (runtime) runtime->cpu_time(0, &used);
        b.put(0, p.create_time, 8);
        b.put(24, used, 8);          // UserTime (the time is not split into kernel and user)
        size = 32;
        break;
      }
      case 7: size = 8; break;                                          // ProcessDebugPort: none
      case 12: b.put(0, p.error_mode, 4); size = 4; break;
      case 18: b.put(1, nt_priority_class(p.priority_class), 1); size = 2; break;
      case 20: {      // ProcessHandleCount
        uint32_t open = 0;
        for (uint64_t slot = 0; slot < ProcessFiles::kMaxDescriptors; ++slot) if (p.files().kind(slot) != DescriptorKind::Closed) ++open;
        b.put(0, open, 4);
        size = 4;
        break;
      }
      case 21: b.put(0, runtime ? runtime->affinity(0) : 1, 8); size = 8; break;
      case 24: size = 4; break;                                         // ProcessSessionInformation: session 0
      case 26: size = 8; break;                                         // ProcessWow64Information: not a 32-bit process
      case 27: {      // ProcessImageFileName: a UNICODE_STRING followed by its characters (read from the process parameters)
        uint64_t peb = 0, parameters = 0, image_length = 0, image_buffer = 0;
        uint8_t name[16] = {};
        p.read_user_u64(p.gs_base() + 0x60, &peb);
        if (peb == 0 || !p.read_user_u64(peb + 0x20, &parameters) || !p.read_user(parameters + 0x60, ByteView(name, 16))) return kAccessViolation;
        image_length = name[0] | (uint64_t{name[1]} << 8);
        for (uint32_t i = 0; i < 8; ++i) image_buffer |= uint64_t{name[8 + i]} << (8 * i);
        const uint32_t need = uint32_t(16 + image_length + 2);
        if (returned != 0 && !p.write_user_u32(returned, need)) return kAccessViolation;
        if (length < need) return kInfoLengthMismatch;
        uint8_t text[2 * 520] = {};
        if (image_length > sizeof(text) - 2 || !p.read_user(image_buffer, ByteView(text, image_length))) return kInfoLengthMismatch;
        uint8_t out[16] = {};
        out[0] = uint8_t(image_length); out[1] = uint8_t(image_length >> 8);
        out[2] = uint8_t(image_length + 2); out[3] = uint8_t((image_length + 2) >> 8);
        const uint64_t text_at = r.args[2] + 16;
        for (uint32_t i = 0; i < 8; ++i) out[8 + i] = uint8_t(text_at >> (8 * i));
        return p.write_user(r.args[2], ConstByteView(out, 16)) && p.write_user(text_at, ConstByteView(text, image_length + 2)) ? kSuccess : kAccessViolation;
      }
      case 30: return kPortNotSet;                                      // ProcessDebugObjectHandle: not being debugged
      case 31: b.put(0, 1, 4); size = 4; break;                         // ProcessDebugFlags: inheriting
      case 36: b.put(0, uint32_t(p.id() * 2654435761u) | 1u, 4); size = 4; break;      // ProcessCookie
      default: return kInvalidInfoClass;
    }
  }
  if (returned != 0 && !p.write_user_u32(returned, size)) return kAccessViolation;
  if (length < size) return kInfoLengthMismatch;
  return p.write_user(r.args[2], ConstByteView(b.bytes, size)) ? kSuccess : kAccessViolation;
}

// NtOpenProcess(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PCLIENT_ID): a handle to a running program by its process id. It can be waited for and asked about (the exit status);
// TerminateProcess works on the programs this one started.
uint32_t nt_open_process(Process& p, const SyscallRequest& r) {
  uint8_t id[16];
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  if (r.args[3] == 0 || !p.read_user(r.args[3], ByteView(id, 16))) return kAccessViolation;
  uint64_t pid = 0;
  for (uint32_t i = 0; i < 8; ++i) pid |= uint64_t{id[i]} << (8 * i);
  const bool exists = pid == p.id() || (p.runtime() && p.runtime()->process_exists(pid));
  const ObjectManager::Created c = objects->open_process(pid, exists);
  if (c.id == 0) return c.status == 0xC0000034u ? 0xC000000Bu : c.status;      // STATUS_INVALID_CID
  uint8_t attributes[4] = {};
  if (r.args[2] != 0) p.read_user(r.args[2] + 24, ByteView(attributes, 4));
  uint64_t slot = 0;
  const uint32_t status = give_handle(p, *objects, c.id, r.args[0], &slot);
  if (status == kSuccess) p.files().set_handle_flags(slot, (attributes[0] & 2) != 0, false);
  return status;
}

uint32_t nt_set_information_process(Process& p, const SyscallRequest& r) {
  if (r.args[0] != kCurrentProcessHandle) {
    bool exited = false;
    uint32_t code = 0;
    uint64_t pid = 0;
    if (!process_handle_info(p, r.args[0], &exited, &code, &pid)) return kInvalidHandle;
    if (pid != p.id()) {      // another program: its scheduling settings are accepted, not applied
      if (exited && uint32_t(r.args[1]) == 21) return 0xC000010Au;      // STATUS_PROCESS_IS_TERMINATING (the affinity of a program that ended)
      return uint32_t(r.args[1]) == 21 || uint32_t(r.args[1]) == 18 || uint32_t(r.args[1]) == 12 ? kSuccess : kInvalidInfoClass;
    }
  }
  const uint32_t info_class = uint32_t(r.args[1]), length = uint32_t(r.args[3]);
  uint8_t in[8] = {};
  switch (info_class) {
    case 12:      // ProcessDefaultHardErrorMode
      if (length != 4 || !p.read_user(r.args[2], ByteView(in, 4))) return length != 4 ? kInfoLengthMismatch : kAccessViolation;
      p.error_mode = in[0] | (uint32_t{in[1]} << 8) | (uint32_t{in[2]} << 16) | (uint32_t{in[3]} << 24);
      return kSuccess;
    case 18:      // ProcessPriorityClass { Foreground, PriorityClass }
      if (length != 2 || !p.read_user(r.args[2], ByteView(in, 2))) return length != 2 ? kInfoLengthMismatch : kAccessViolation;
      if (in[1] < 1 || in[1] > 6) return kInvalidParameter;
      p.priority_class = win32_priority_class(in[1]);
      repriority_threads(p);
      return kSuccess;
    case 21: {    // ProcessAffinityMask
      if (length != 8 || !p.read_user(r.args[2], ByteView(in, 8))) return length != 8 ? kInfoLengthMismatch : kAccessViolation;
      uint64_t mask = 0;
      for (uint32_t i = 0; i < 8; ++i) mask |= uint64_t{in[i]} << (8 * i);
      return p.runtime() && p.runtime()->set_affinity(0, mask) ? kSuccess : kInvalidParameter;
    }
    default: return kInvalidInfoClass;
  }
}

}  // namespace process::windowsabi
