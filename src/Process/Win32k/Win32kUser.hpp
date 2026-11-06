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
void set_window_menu(uint32_t hwnd, uint32_t menu);              // the menu of a top-level window (SetMenu); the caller changes the frame
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
void inject_mouse_motion(int32_t dx, int32_t dy);                // a relative mouse's motion: raw input gets it whole, the cursor moves as far as the screen / ClipCursor lets it
void inject_mouse_button(uint32_t button, bool down);            // 1 left, 2 right, 3 middle, 4 X1, 5 X2
void inject_mouse_wheel(int32_t delta, bool horizontal);
void cursor_position(int32_t* x, int32_t* y);                    // takes the lock itself
void set_cursor_position(int32_t x, int32_t y);
bool cursor_confined();                                          // a program keeps the cursor in a rectangle (ClipCursor); takes the lock                  // SetCursorPos / ClipCursor: the cursor moves (WM_MOUSEMOVE), but it is not input (no raw input); takes the lock
// Times and timers: the window manager's timers (SetTimer) and the delayed things (tooltips, double-click time) are due by the monotonic clock of the process hosts. The kernel
// calls this now and then (the desktop pump), and every message wait calls it; it posts WM_TIMER / WM_PAINT-like pending work. Takes the lock itself.
void pump(uint64_t now_ns);

// AdjustWindowRectEx: grows `rect` (a client rectangle) to the window rectangle for the style (the caption, frame and menu bar sizes come from user_misc::system_metrics).
bool adjust_window_rect(gdi::Rect* rect, uint32_t style, uint32_t ex_style, bool has_menu, uint32_t dpi);

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// (the window manager's own declarations follow)

// For host tests: drops the whole state of the window manager (the next call makes a fresh one with the allocator of the system then current), and makes the calling host
// thread act as `tid` (0: ask the process runtime as usual). Not used by the kernel.
void reset_state();
void set_test_thread(uint64_t tid);
// The upcall protocol the user-mode half of win32u.dll uses (docs/win32k/User.md): the layout NtUserTcosGetUpcall fills in (all little endian, 8-byte aligned).
struct UpcallRecord {
  uint64_t id;
  uint32_t kind, flags;
  uint64_t proc;
  uint64_t hwnd;
  uint32_t msg, payload_len;
  uint64_t wparam, lparam;
  uint64_t extra[3];
  uint16_t fix_at, fix_to;
  uint32_t reserved;
  uint8_t payload[4096];
};
constexpr uint64_t kUpcallDueValue = 0x7FFFFFF1u, kDrainRetryValue = 0x7FFFFFF3u;

}  // namespace process::win32k::user

// What the window manager and the GDI module use of the rest of USER (Win32kUserMisc*.cpp: menus, cursors and icons, clipboard, monitors, settings, ...). Implemented by the
// user-misc module; it may add more below the marked line.
namespace process::win32k::user_misc {

int32_t system_metrics(uint32_t index, uint32_t dpi);               // GetSystemMetricsForDpi (dpi 0 or 96: the metrics at 96 dpi); the values of Windows 10 at 96 dpi
uint32_t system_color(uint32_t index);                              // GetSysColor: a COLORREF (0x00BBGGRR)
uint32_t monitor_from_rect(const gdi::Rect& rect, uint32_t flags);  // MonitorFromRect (flags: MONITOR_DEFAULTTONULL 0, TOPRIMARY 1, TONEAREST 2); there is one monitor
uint32_t monitor_from_point(int32_t x, int32_t y, uint32_t flags);
uint32_t dpi_for_monitor(uint32_t monitor);                         // 96 unless the display settings say otherwise
gdi::Rect monitor_rect(uint32_t monitor, bool work_area);           // the monitor's rectangle / its work area
bool is_menu(uint32_t handle);
// The input context associated with a window (AssociateInputContext): 0xFFFFFFFF for a window nothing was associated with, 0 for one with the IME switched off.
uint32_t window_input_context_raw(uint32_t hwnd);
// The menu bar of a window changed (SetMenu) or a window went: the menu module drops what depended on the window. The window manager calls this when it destroys a window.
void window_destroyed(uint32_t hwnd);
bool attach_window_menu(uint32_t hwnd, uint32_t menu);                  // CreateWindowEx with a menu: the window owns it from now on (false: no such menu)
// Raw input: the window manager reports each mouse (type 0) and keyboard (type 1) event of the machine and of SendInput, with the RAWMOUSE / RAWKEYBOARD bytes; the input module
// makes a record for each process registered for the device and posts WM_INPUT to its target window. `injected`: from SendInput (the record names no device, as on Windows).
void raw_input_event(uint32_t type, bool injected, ConstByteView data);
bool raw_input_no_legacy(uint64_t pid, uint32_t type);                  // RIDEV_NOLEGACY: the process wants no WM_MOUSEMOVE / WM_KEYDOWN ... of that device

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// (the user-misc module's own declarations follow)

}  // namespace process::win32k::user_misc
