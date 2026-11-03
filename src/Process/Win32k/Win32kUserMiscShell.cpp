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

// Shell and process level services: the shell, Program Manager and task manager windows, shell hook registration, the default layout direction of a process, shutdown block
// reasons, window display affinity, the system beep and the time of the last input.
namespace process::win32k::user_misc {

namespace {

constexpr uint32_t kErrorRequestAborted = 1235;
constexpr uint32_t kMaxReasonChars = 256;

struct BlockReason {
  uint32_t hwnd;
  WideString reason;
  BlockReason(allocators::IAllocator& a, uint32_t window) : hwnd(window), reason(a) {}
};
struct Affinity { uint32_t hwnd, value; };

struct ShellState {
  uint32_t shell = 0, list_view = 0, progman = 0, taskman = 0;
  List<uint32_t> hook_windows;
  Hive<BlockReason> reasons;
  List<Affinity> affinities;
  uint64_t last_input_ms = 0;
  uint32_t beep_count = 0, last_beep = 0;
  explicit ShellState(allocators::IAllocator& a) : hook_windows(a), reasons(a), affinities(a) {}
};
UniquePointer<ShellState> g_shell;
ShellState* st() { return lazy_state(g_shell); }

BlockReason* find_reason(ShellState& s, uint32_t hwnd) {
  for (BlockReason& r : s.reasons) if (r.hwnd == hwnd) return &r;
  return nullptr;
}

uint64_t svc_set_shell_window_ex(Call& c) {
  ShellState* s = st();
  const uint32_t shell = c.arg32(0), list_view = c.arg32(1);
  if (!s) return 0;
  if (!user::is_window(shell) || (list_view != 0 && !user::is_window(list_view))) return c.fail(kErrorInvalidWindowHandle);
  if (user::window_process(shell) != c.pid()) return c.fail(kErrorAccessDenied);
  if (s->shell != 0 && user::is_window(s->shell)) return c.fail(kErrorAccessDenied);      // there is a shell already
  s->shell = shell;
  s->list_view = list_view;
  return 1;
}

uint64_t set_special_window(Call& c, uint32_t ShellState::*slot) {
  ShellState* s = st();
  const uint32_t hwnd = c.arg32(0);
  if (!s) return 0;
  if (hwnd != 0 && !user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  s->*slot = hwnd;
  return hwnd;
}
uint64_t svc_set_progman_window(Call& c) { return set_special_window(c, &ShellState::progman); }
uint64_t svc_set_taskman_window(Call& c) { return set_special_window(c, &ShellState::taskman); }

uint64_t svc_register_shell_hook_window(Call& c) {
  ShellState* s = st();
  const uint32_t hwnd = c.arg32(0);
  if (!s) return 0;
  if (!user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  if (user::window_process(hwnd) != c.pid()) return c.fail(kErrorAccessDenied);
  for (uint32_t w : s->hook_windows.view()) if (w == hwnd) return 1;
  return s->hook_windows.push_back(hwnd) ? 1 : c.fail(kErrorNotEnoughMemory);
}

uint64_t svc_deregister_shell_hook_window(Call& c) {
  ShellState* s = st();
  const uint32_t hwnd = c.arg32(0);
  if (!s) return 0;
  for (uint64_t i = 0; i < s->hook_windows.count(); ++i) if (s->hook_windows[i] == hwnd) { s->hook_windows.remove_at(i); return 1; }
  return c.fail(kErrorInvalidParameter);
}

uint64_t svc_get_process_default_layout(Call& c) {
  ProcessRecord* p = find_process_record(c.pid());
  return c.write_u32(c.arg(0), p ? p->default_layout : 0) ? 1 : c.fail(kErrorNoAccess);
}

uint64_t svc_set_process_default_layout(Call& c) {
  const uint32_t layout = c.arg32(0);
  if ((layout & ~0xFu) != 0) return c.fail(kErrorInvalidParameter);
  ProcessRecord* p = process_record(c.pid());
  if (!p) return 0;
  p->default_layout = layout;
  return 1;
}

uint64_t svc_shutdown_block_reason_create(Call& c) {
  ShellState* s = st();
  const uint32_t hwnd = c.arg32(0);
  if (!s) return 0;
  if (!user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  if (user::window_process(hwnd) != c.pid()) return c.fail(kErrorAccessDenied);
  WideString text(*c.allocator());
  if (!c.read_wide_z(c.arg(1), kMaxReasonChars, &text)) return c.fail(c.arg(1) == 0 ? kErrorInvalidParameter : kErrorNoAccess);
  if (text.empty()) return c.fail(kErrorInvalidParameter);
  BlockReason* r = find_reason(*s, hwnd);
  if (!r) r = s->reasons.emplace(*c.allocator(), hwnd);
  if (!r) return c.fail(kErrorNotEnoughMemory);
  r->reason.clear();
  r->reason.append(text.view());
  return 1;
}

uint64_t svc_shutdown_block_reason_query(Call& c) {
  ShellState* s = st();
  const uint32_t hwnd = c.arg32(0);
  if (!s) return 0;
  BlockReason* r = find_reason(*s, hwnd);
  if (!r) return c.fail(kErrorRequestAborted);
  uint32_t capacity = 0;
  if (!c.read_u32(c.arg(2), &capacity)) return c.fail(kErrorInvalidParameter);
  const uint32_t need = uint32_t(r->reason.size()) + 1;
  if (c.arg(1) == 0 || capacity < need) { c.write_u32(c.arg(2), need); return c.fail(kErrorInsufficientBuffer); }
  if (c.write_wide(c.arg(1), r->reason.view(), capacity) < 0 || !c.write_u32(c.arg(2), need)) return c.fail(kErrorNoAccess);
  return 1;
}

uint64_t svc_shutdown_reason_destroy(Call& c) {
  ShellState* s = st();
  BlockReason* r = s ? find_reason(*s, c.arg32(0)) : nullptr;
  if (!r) return c.fail(kErrorRequestAborted);
  s->reasons.erase(r);
  return 1;
}

uint64_t svc_set_window_display_affinity(Call& c) {
  ShellState* s = st();
  const uint32_t hwnd = c.arg32(0), value = c.arg32(1);
  if (!s) return 0;
  if (!user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  if (value != 0 && value != 1 && value != 0x11) return c.fail(kErrorInvalidParameter);
  if (user::window_process(hwnd) != c.pid()) return c.fail(kErrorAccessDenied);
  for (Affinity& a : s->affinities.view()) if (a.hwnd == hwnd) { a.value = value; return 1; }
  return s->affinities.push_back(Affinity{hwnd, value}) ? 1 : c.fail(kErrorNotEnoughMemory);
}

uint64_t svc_get_window_display_affinity(Call& c) {
  ShellState* s = st();
  const uint32_t hwnd = c.arg32(0);
  if (!s) return 0;
  if (!user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  uint32_t value = 0;
  for (const Affinity& a : s->affinities.view()) if (a.hwnd == hwnd) value = a.value;
  return c.write_u32(c.arg(1), value) ? 1 : c.fail(kErrorNoAccess);
}

uint64_t svc_message_beep(Call& c) {
  ShellState* s = st();
  if (!s) return 0;
  ++s->beep_count;      // (the sound itself belongs to the audio layer: the request is recorded for it)
  s->last_beep = c.arg32(0);
  return 1;
}

}  // namespace

void note_last_input(uint64_t now_ms) {
  if (ShellState* s = st()) s->last_input_ms = now_ms;
}

uint64_t last_input_ms(Call& c) {
  ShellState* s = st();
  if (!s) return 0;
  if (s->last_input_ms == 0) s->last_input_ms = now_ns(c.process()) / 1000000;      // no input seen yet: the system just started being observed
  return s->last_input_ms;
}

uint32_t shell_window() { return g_shell ? g_shell->shell : 0; }
uint32_t progman_window() { return g_shell ? g_shell->progman : 0; }
uint32_t taskman_window() { return g_shell ? g_shell->taskman : 0; }

void shell_hook_windows(List<uint32_t>* out) {
  if (!g_shell) return;
  for (uint32_t w : g_shell->hook_windows.view()) out->push_back(w);
}

void note_beep(uint32_t* count, uint32_t* last_type) {
  ShellState* s = st();
  *count = s ? s->beep_count : 0;
  *last_type = s ? s->last_beep : 0;
}

void shell_window_destroyed(uint32_t hwnd) {
  ShellState* s = g_shell.get();
  if (!s) return;
  if (s->shell == hwnd) { s->shell = 0; s->list_view = 0; }
  if (s->list_view == hwnd) s->list_view = 0;
  if (s->progman == hwnd) s->progman = 0;
  if (s->taskman == hwnd) s->taskman = 0;
  for (uint64_t i = 0; i < s->hook_windows.count();) { if (s->hook_windows[i] == hwnd) s->hook_windows.remove_at(i); else ++i; }
  for (uint64_t i = 0; i < s->affinities.count();) { if (s->affinities[i].hwnd == hwnd) s->affinities.remove_at(i); else ++i; }
  if (BlockReason* r = find_reason(*s, hwnd)) s->reasons.erase(r);
}

void shell_process_exited(uint64_t) {}

void install_shell() {
  using N = WindowsSyscallNumber;
  register_handler(N::NtUserSetShellWindowEx, svc_set_shell_window_ex);
  register_handler(N::NtUserSetProgmanWindow, svc_set_progman_window);
  register_handler(N::NtUserSetTaskmanWindow, svc_set_taskman_window);
  register_handler(N::NtUserRegisterShellHookWindow, svc_register_shell_hook_window);
  register_handler(N::NtUserDeregisterShellHookWindow, svc_deregister_shell_hook_window);
  register_handler(N::NtUserGetProcessDefaultLayout, svc_get_process_default_layout);
  register_handler(N::NtUserSetProcessDefaultLayout, svc_set_process_default_layout);
  register_handler(N::NtUserShutdownBlockReasonCreate, svc_shutdown_block_reason_create);
  register_handler(N::NtUserShutdownBlockReasonQuery, svc_shutdown_block_reason_query);
  register_handler(N::NtUserShutdownReasonDestroy, svc_shutdown_reason_destroy);
  register_handler(N::NtUserSetWindowDisplayAffinity, svc_set_window_display_affinity);
  register_handler(N::NtUserGetWindowDisplayAffinity, svc_get_window_display_affinity);
  register_handler(N::NtUserMessageBeep, svc_message_beep);
}

}  // namespace process::win32k::user_misc
