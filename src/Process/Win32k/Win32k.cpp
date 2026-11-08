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
#include "Process/Win32k/Win32k.hpp"
#include "Process/WindowsObjects.hpp"

namespace process {

// The hooks WindowsSyscalls.cpp calls into win32k through (null until install()).
extern uint64_t (*g_win32k_dispatch)(Process&, const SyscallRequest&, bool* handled);
extern void (*g_win32k_process_exited)(uint64_t pid);

namespace win32k {

namespace {

constexpr uint32_t kServices = kServiceEnd - kServiceBase;
constexpr uint32_t kMaxExitHooks = 16, kMaxDuplicates = 64;
constexpr uint32_t kTebLastError = 0x68;

struct Table {
  Handler handlers[kServices] = {};
  bool locked[kServices] = {};
  uint32_t registered = 0, duplicates = 0;
  uint32_t duplicate_numbers[kMaxDuplicates] = {};
  ProcessExitHook exit_hooks[kMaxExitHooks] = {};
  uint32_t exit_hook_count = 0;
  Handler dwm_handler = nullptr;
  bool installed = false;
  uint64_t thread_override = 0;
};
Table g_table;

ObjectManager* objects_or_null() { return system_objects(); }

uint64_t dispatch_hook(Process& process, const SyscallRequest& request, bool* handled) {
  uint64_t result = 0;
  *handled = dispatch(process, request, &result);
  return result;
}

}  // namespace

uint64_t Call::tid() {
  if (g_table.thread_override != 0) return g_table.thread_override;
  IProcessRuntime* runtime = process_.runtime();
  return runtime ? runtime->current_thread_id() : process_.id();
}

allocators::IAllocator* Call::allocator() const { return system_allocator(); }

uint64_t Call::arg(uint32_t index) {
  if (index < 4) return request_.args[index];
  const uint64_t offset = 0x28 + uint64_t{index - 4} * 8;      // past the return address and the 32 bytes of home space
  uint64_t value = 0;
  if (request_.user_stack > ~uint64_t{0} - offset - 7 || !process_.read_user_u64(request_.user_stack + offset, &value)) {
    bad_stack_ = true;
    return 0;
  }
  return value;
}

bool Call::read_u16(uint64_t address, uint16_t* out) {
  uint8_t raw[2];
  if (!read(address, ByteView(raw, 2))) return false;
  *out = uint16_t(raw[0] | raw[1] << 8);
  return true;
}
bool Call::read_u32(uint64_t address, uint32_t* out) {
  uint8_t raw[4];
  if (!read(address, ByteView(raw, 4))) return false;
  *out = uint32_t(raw[0]) | uint32_t(raw[1]) << 8 | uint32_t(raw[2]) << 16 | uint32_t(raw[3]) << 24;
  return true;
}
bool Call::read_u64(uint64_t address, uint64_t* out) {
  uint8_t raw[8];
  if (!read(address, ByteView(raw, 8))) return false;
  uint64_t value = 0;
  for (uint32_t i = 0; i < 8; ++i) value |= uint64_t{raw[i]} << (8 * i);
  *out = value;
  return true;
}
bool Call::write_u16(uint64_t address, uint16_t value) {
  const uint8_t raw[2] = {uint8_t(value), uint8_t(value >> 8)};
  return write(address, ConstByteView(raw, 2));
}
bool Call::write_u32(uint64_t address, uint32_t value) {
  uint8_t raw[4];
  for (uint32_t i = 0; i < 4; ++i) raw[i] = uint8_t(value >> (8 * i));
  return write(address, ConstByteView(raw, 4));
}
bool Call::write_u64(uint64_t address, uint64_t value) {
  uint8_t raw[8];
  for (uint32_t i = 0; i < 8; ++i) raw[i] = uint8_t(value >> (8 * i));
  return write(address, ConstByteView(raw, 8));
}

bool Call::read_wide(uint64_t address, uint32_t chars, WideString* out) {
  if (chars == 0) return true;
  if (address == 0 || chars > (1u << 20)) return false;
  uint8_t raw[256];
  uint32_t done = 0;
  while (done < chars) {
    const uint32_t n = chars - done < 128 ? chars - done : 128;
    if (!read(address + uint64_t{done} * 2, ByteView(raw, uint64_t{n} * 2))) return false;
    for (uint32_t i = 0; i < n; ++i) if (!out->append(char16_t(raw[2 * i] | raw[2 * i + 1] << 8))) return false;
    done += n;
  }
  return true;
}

bool Call::read_wide_z(uint64_t address, uint32_t max_chars, WideString* out) {
  if (address == 0) return false;
  for (uint32_t i = 0; i < max_chars; ++i) {
    uint16_t c = 0;
    if (!read_u16(address + uint64_t{i} * 2, &c)) return false;
    if (c == 0) return true;
    if (!out->append(char16_t(c))) return false;
  }
  return false;
}

bool Call::read_ansi_z(uint64_t address, uint32_t max_chars, WideString* out) {
  if (address == 0) return false;
  for (uint32_t i = 0; i < max_chars; ++i) {
    uint8_t c = 0;
    if (!read_u8(address + i, &c)) return false;
    if (c == 0) return true;
    if (!out->append(char16_t(c))) return false;
  }
  return false;
}

bool Call::read_unicode_string(uint64_t address, WideString* out) {
  uint16_t length = 0;
  uint64_t buffer = 0;
  if (!read_u16(address, &length) || !read_u64(address + 8, &buffer)) return false;
  return length == 0 || read_wide(buffer, length / 2, out);
}

int32_t Call::write_wide(uint64_t address, ArrayView<const char16_t> text, uint32_t capacity, bool terminate) {
  uint32_t n = text.size() < capacity ? uint32_t(text.size()) : capacity;
  uint8_t raw[256];
  uint32_t done = 0;
  while (done < n) {
    const uint32_t k = n - done < 128 ? n - done : 128;
    for (uint32_t i = 0; i < k; ++i) {
      const char16_t c = text[done + i];
      raw[2 * i] = uint8_t(c);
      raw[2 * i + 1] = uint8_t(c >> 8);
    }
    if (!write(address + uint64_t{done} * 2, ConstByteView(raw, uint64_t{k} * 2))) return -1;
    done += k;
  }
  if (terminate && n < capacity && !write_u16(address + uint64_t{n} * 2, 0)) return -1;
  return int32_t(n);
}

void Call::set_last_error(uint32_t error) {
  uint64_t teb = process_.gs_base();
  if (Process::ThreadRecord* thread = process_.find_thread(tid())) if (thread->teb != 0) teb = thread->teb;
  if (teb != 0) write_u32(teb + kTebLastError, error);
}

bool register_handler(WindowsSyscallNumber number, Handler handler, bool locked) {
  const uint32_t n = uint32_t(number);
  if (n < kServiceBase || n >= kServiceEnd || !handler) return false;
  const uint32_t index = n - kServiceBase;
  if (g_table.handlers[index]) { if (g_table.duplicates < kMaxDuplicates) g_table.duplicate_numbers[g_table.duplicates] = n; ++g_table.duplicates; return false; }
  g_table.handlers[index] = handler;
  g_table.locked[index] = locked;
  ++g_table.registered;
  return true;
}

bool register_dwm_handler(Handler handler) {
  if (!handler || g_table.dwm_handler) return false;
  g_table.dwm_handler = handler;
  return true;
}

Handler handler_of(uint32_t service) { return service >= kServiceBase && service < kServiceEnd ? g_table.handlers[service - kServiceBase] : nullptr; }
uint32_t registered_count() { return g_table.registered; }
uint32_t duplicate_registrations() { return g_table.duplicates; }
uint32_t duplicate_service(uint32_t index) { return index < g_table.duplicates && index < kMaxDuplicates ? g_table.duplicate_numbers[index] : 0; }

allocators::IAllocator* system_allocator() {
  ObjectManager* objects = objects_or_null();
  return objects ? objects->allocator() : nullptr;
}
void set_thread_override(uint64_t tid) { g_table.thread_override = tid; }

namespace {
struct LaterUnmap {
  Process* process = nullptr;
  uint64_t view = 0;
};
constexpr uint32_t kMaxLaterUnmaps = 64;
LaterUnmap g_later_unmaps[kMaxLaterUnmaps];      // (under the system lock)
uint32_t g_later_unmap_count = 0;

// The views the handler that just ran queued for `process` (the system lock held); the others stay queued for a call of theirs.
uint32_t take_later_unmaps(Process& process, ArrayView<uint64_t> out) {
  uint32_t taken = 0, kept = 0;
  for (uint32_t i = 0; i < g_later_unmap_count; ++i) {
    if (g_later_unmaps[i].process == &process && taken < out.size()) out[taken++] = g_later_unmaps[i].view;
    else g_later_unmaps[kept++] = g_later_unmaps[i];
  }
  g_later_unmap_count = kept;
  return taken;
}
}  // namespace

bool unmap_view_later(Process& process, uint64_t view) {
  if (g_later_unmap_count >= kMaxLaterUnmaps) return false;
  g_later_unmaps[g_later_unmap_count++] = LaterUnmap{&process, view};
  return true;
}

bool dispatch(Process& process, const SyscallRequest& request, uint64_t* result) {
  if (request.number == uint32_t(WindowsSyscallNumber::NtTcosDwm)) {
    ObjectManager* objects = objects_or_null();
    if (!g_table.dwm_handler || !objects) return false;
    Call call(process, request);
    ISystemSync& sync = objects->sync();
    sync.lock();
    *result = g_table.dwm_handler(call);
    sync.unlock();
    return true;
  }
  if (request.number < kServiceBase || request.number >= kServiceEnd) return false;
  const uint32_t index = uint32_t(request.number) - kServiceBase;
  const Handler handler = g_table.handlers[index];
  ObjectManager* objects = objects_or_null();
  if (!handler || !objects) return false;
  Call call(process, request);
  if (!g_table.locked[index]) {
    *result = handler(call);
    return true;
  }
  ISystemSync& sync = objects->sync();
  sync.lock();
  *result = handler(call);
  uint64_t views[kMaxLaterUnmaps];
  const uint32_t unmaps = g_later_unmap_count ? take_later_unmaps(process, ArrayView<uint64_t>(views, kMaxLaterUnmaps)) : 0;
  sync.unlock();
  for (uint32_t i = 0; i < unmaps; ++i) windowsabi::unmap_view_in(process, views[i]);
  return true;
}

bool register_process_exit_hook(ProcessExitHook hook) {
  if (!hook || g_table.exit_hook_count >= kMaxExitHooks) return false;
  g_table.exit_hooks[g_table.exit_hook_count++] = hook;
  return true;
}

void process_exited(uint64_t pid) {
  ObjectManager* objects = objects_or_null();
  if (!objects) return;
  ISystemSync& sync = objects->sync();
  sync.lock();
  for (uint32_t i = 0; i < g_table.exit_hook_count; ++i) g_table.exit_hooks[i](pid);
  sync.notify_all();
  sync.unlock();
}

void wake_all() {
  if (ObjectManager* objects = objects_or_null()) objects->sync().notify_all();
}

uint64_t now_ns(Process& process) { return process.host().monotonic_ns(); }

bool wait_loop(Call& call, uint64_t timeout_ns, bool (*attempt)(void* context), void* context, uint64_t (*next_event_ns)(void* context)) {
  ObjectManager* objects = objects_or_null();
  if (!objects) return false;
  ISystemSync& sync = objects->sync();
  Process& p = call.process();
  IProcessRuntime* runtime = p.runtime();
  const uint64_t started = p.host().monotonic_ns();
  for (;;) {
    if (attempt(context)) return true;
    uint64_t left = ~uint64_t{0};
    if (timeout_ns != ~uint64_t{0}) {
      const uint64_t used = p.host().monotonic_ns() - started;
      if (used >= timeout_ns) return false;
      left = timeout_ns - used;
    }
    if (next_event_ns) {
      const uint64_t at = next_event_ns(context), now = p.host().monotonic_ns();
      if (at != ~uint64_t{0}) {
        const uint64_t until = at > now ? at - now : 0;
        if (until < left) left = until;
        if (until == 0) continue;
      }
    }
    if (p.exited() || (runtime && runtime->interrupted())) return false;
    if (runtime && runtime->park_wanted()) {      // asked to suspend while it waits: it stops here and goes on waiting when resumed
      sync.unlock();      // (only then: dropping the lock here would let a wake-up fall between the look above and the wait below)
      const int parked = runtime->park_point();
      sync.lock();
      if (parked == 2) return false;
      continue;      // (look again: anything may have changed while the lock was let go)
    }
    if (runtime) runtime->unlock();
    else p.host().yield();
    sync.wait(left);
    sync.unlock();
    if (runtime) runtime->lock();
    sync.lock();
  }
}

void install() {
  if (g_table.installed) return;
  g_table.installed = true;
  install_gdi();
  install_user();
  install_user_misc();
  install_composition();
  g_win32k_dispatch = dispatch_hook;
  g_win32k_process_exited = process_exited;
}

}  // namespace win32k
}  // namespace process
