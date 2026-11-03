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
#include "Process/Win32k/Win32kGdi.hpp"

// What the other win32k modules (and the kernel, which feeds the machine's input in) use of the window manager (Win32kUser*.cpp). The window manager implements everything
// declared here; it may add more below the marked line. Guarded by the system lock like all win32k state: the functions are called with it held (the inject_* functions and
// cursor_position take it themselves when called from outside a handler - see the comment on each).
namespace process::win32k::user {

// --- Windows.
bool is_window(uint32_t hwnd);
uint64_t window_thread(uint32_t hwnd);       // the thread that made the window (0 if there is no such window)
uint64_t window_process(uint32_t hwnd);
bool window_screen_rect(uint32_t hwnd, gdi::Rect* rect);        // GetWindowRect
bool window_client_screen_rect(uint32_t hwnd, gdi::Rect* rect); // the client area, in screen coordinates
bool window_visible(uint32_t hwnd);                              // IsWindowVisible (it and all its ancestors have WS_VISIBLE)
uint32_t desktop_window();                                       // the root of the window tree
uint32_t foreground_window();
uint32_t focus_window();                                          // the focus window of the foreground queue
uint32_t window_from_screen_point(int32_t x, int32_t y);
// Puts a message in the queue of the window's thread (PostMessage). False if the window does not exist.
bool post_message(uint32_t hwnd, uint32_t message, uint64_t wparam, uint64_t lparam);
// Puts a message in the queue of a thread (PostThreadMessage), hwnd 0.
bool post_thread_message(uint64_t thread, uint32_t message, uint64_t wparam, uint64_t lparam);

// --- The machine's input, fed by the kernel (the keyboard and mouse drivers / the desktop's input pump) or by tests. They take the system lock themselves, update the key state, the
// cursor and the focus/capture/hover windows, and queue the hardware messages (WM_KEYDOWN, WM_CHAR, WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_MOUSEWHEEL, ...) for the right threads.
void inject_key(uint32_t virtual_key, uint32_t scan_code, bool down, bool extended);
void inject_char(char32_t character);                            // text typed (the layout is not this module's business): WM_CHAR to the focus window
void inject_mouse_move(int32_t screen_x, int32_t screen_y);
void inject_mouse_button(uint32_t button, bool down);            // 1 left, 2 right, 3 middle, 4 X1, 5 X2
void inject_mouse_wheel(int32_t delta, bool horizontal);
void cursor_position(int32_t* x, int32_t* y);                    // takes the lock itself
// Times and timers: the window manager's timers (SetTimer) and the delayed things (tooltips, double-click time) are due by the monotonic clock of the process hosts. The kernel
// calls this now and then (the desktop pump), and every message wait calls it; it posts WM_TIMER / WM_PAINT-like pending work. Takes the lock itself.
void pump(uint64_t now_ns);

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// (the window manager's own declarations follow)

}  // namespace process::win32k::user
