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
#include "Process/Win32k/Win32kUserMisc.hpp"

namespace process::win32k {

namespace user_misc {

namespace {

struct Records {
  Hive<ProcessRecord> processes;
  Hive<ThreadRecord> threads;
  uint32_t dpi = 96;
  explicit Records(allocators::IAllocator& allocator) : processes(allocator), threads(allocator) {}
};
UniquePointer<Records> g_records;

}  // namespace

ProcessRecord* find_process_record(uint64_t pid) {
  Records* r = g_records.get();
  if (!r) return nullptr;
  for (ProcessRecord& p : r->processes) if (p.pid == pid) return &p;
  return nullptr;
}

ProcessRecord* process_record(uint64_t pid) {
  if (ProcessRecord* p = find_process_record(pid)) return p;
  Records* r = lazy_state(g_records);
  if (!r) return nullptr;
  ProcessRecord* p = r->processes.emplace();
  if (p) p->pid = pid;
  return p;
}

ThreadRecord* find_thread_record(uint64_t tid) {
  Records* r = g_records.get();
  if (!r) return nullptr;
  for (ThreadRecord& t : r->threads) if (t.tid == tid) return &t;
  return nullptr;
}

ThreadRecord* thread_record(uint64_t tid, uint64_t pid) {
  if (ThreadRecord* t = find_thread_record(tid)) return t;
  Records* r = lazy_state(g_records);
  if (!r) return nullptr;
  ThreadRecord* t = r->threads.emplace();
  if (t) { t->tid = tid; t->pid = pid; }
  return t;
}

void thread_forget(uint64_t tid) {
  Records* r = g_records.get();
  if (!r) return;
  ObjectCursor cursor;
  for (bool more = r->threads.first(&cursor); more;) {
    if (r->threads.element(cursor)->tid == tid) more = r->threads.erase(&cursor);
    else more = r->threads.next(&cursor);
  }
}

uint32_t system_dpi() { return g_records ? g_records->dpi : 96; }
void set_system_dpi(uint32_t dpi) {
  if (Records* r = lazy_state(g_records)) r->dpi = dpi < 48 ? 48 : dpi > 960 ? 960 : dpi;
}

uint32_t thread_dpi(Call& call) {
  uint32_t context = 0;
  if (ThreadRecord* t = find_thread_record(call.tid())) context = t->dpi_context;
  if (context == 0) if (ProcessRecord* p = find_process_record(call.pid())) context = p->dpi_context;
  return (context & 0x0F) == 0 ? 96 : system_dpi();      // unaware (also: never set) threads see 96 dpi
}

namespace {

void process_exited_hook(uint64_t pid) {
  menus_process_exited(pid);
  cursors_process_exited(pid);
  accelerators_process_exited(pid);
  clipboard_process_exited(pid);
  settings_process_exited(pid);
  keyboard_process_exited(pid);
  input_process_exited(pid);
  shell_process_exited(pid);
  dwm_process_exited(pid);
  Records* r = g_records.get();
  if (!r) return;
  ObjectCursor cursor;
  for (bool more = r->threads.first(&cursor); more;) {
    if (r->threads.element(cursor)->pid == pid) more = r->threads.erase(&cursor);
    else more = r->threads.next(&cursor);
  }
  for (bool more = r->processes.first(&cursor); more;) {
    if (r->processes.element(cursor)->pid == pid) more = r->processes.erase(&cursor);
    else more = r->processes.next(&cursor);
  }
}

}  // namespace

// The window manager calls this when it destroys a window.
void window_destroyed(uint32_t hwnd) {
  clipboard_window_destroyed(hwnd);
  keyboard_window_destroyed(hwnd);
  shell_window_destroyed(hwnd);
  dwm_window_destroyed(hwnd);
  menu_window_destroyed(hwnd);
}

}  // namespace user_misc

void install_user_misc() {
  register_process_exit_hook(user_misc::process_exited_hook);
  user_misc::install_calls();
  user_misc::install_menus();
  user_misc::install_cursors();
  user_misc::install_accelerators();
  user_misc::install_clipboard();
  user_misc::install_settings();
  user_misc::install_monitors();
  user_misc::install_keyboard();
  user_misc::install_input();
  user_misc::install_shell();
  user_misc::install_dwm();
}

}  // namespace process::win32k
