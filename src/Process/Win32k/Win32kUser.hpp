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
#include "ObjectContainer/List.hpp"
#include "Process/Win32k/Win32k.hpp"
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
bool cursor_confined();                                          // a program keeps the cursor in a rectangle (ClipCursor); takes the lock
uint32_t mouse_capture_window();                                 // the window that has the mouse captured (SetCapture), 0: none; takes the lock                  // SetCursorPos / ClipCursor: the cursor moves (WM_MOUSEMOVE), but it is not input (no raw input); takes the lock
// Times and timers: the window manager's timers (SetTimer) and the delayed things (tooltips, double-click time) are due by the monotonic clock of the process hosts. The kernel
// calls this now and then (the desktop pump), and every message wait calls it; it posts WM_TIMER / WM_PAINT-like pending work. Takes the lock itself.
void pump(uint64_t now_ns);

// AdjustWindowRectEx: grows `rect` (a client rectangle) to the window rectangle for the style (the caption, frame and menu bar sizes come from user_misc::system_metrics).
bool adjust_window_rect(gdi::Rect* rect, uint32_t style, uint32_t ex_style, bool has_menu, uint32_t dpi);

// --- Redirection: the kernel's desktop shows each top-level window as one of its own windows (Win32kDesktopBridge). Each function takes the system lock.
// The observer is told (with the lock held: it must only record it) when a top-level window was made, destroyed, shown, hidden, moved, sized, retitled, activated or
// changed its style.
class IWindowObserver {
 public:
  virtual void top_level_changed(uint32_t hwnd) = 0;
  // The program asked for the user to move (edge 9) or size (edge 1 .. 8: WMSZ_LEFT .. WMSZ_BOTTOMRIGHT) the window with the mouse (SC_MOVE / SC_SIZE, a press on a
  // caption or border hit code): the desktop drags the window's frame until the button is let go.
  virtual void drag_requested(uint32_t hwnd, uint32_t edge) = 0;

 protected:
  IWindowObserver() = default;
  ~IWindowObserver() = default;
};
void set_redirection(bool on, IWindowObserver* observer, int32_t frame_border, int32_t frame_title);
struct TopLevelInfo {
  bool exists = false, visible = false, minimized = false, maximized = false, active = false;
  bool caption = false, sizable = false, minimize_box = false, maximize_box = false, tool = false, topmost = false;
  bool popup = false, no_activate = false;      // WS_POPUP, WS_EX_NOACTIVATE
  gdi::Rect window;          // screen coordinates (= the desktop's)
  gdi::Rect surface;         // the part the window surface covers (inside the desktop's frame)
  uint32_t surface_id = 0, owner = 0;
  uint32_t icon = 0;         // the small icon to show: WM_SETICON's small or big one, else the class's
  uint64_t pid = 0;
};
bool top_level_info(uint32_t hwnd, TopLevelInfo* out, WideString* title);
void top_levels(List<uint32_t>* out);                                      // the visible redirected top-levels, topmost first
// What the desktop's window does to its top-level: the user moved or sized it (window rectangle, screen coordinates), activated it, asked to close / minimize /
// maximize / restore it (WM_SYSCOMMAND to the program, as a click on its own caption would).
bool move_top_level(uint32_t hwnd, const gdi::Rect& window);
bool activate_top_level(uint32_t hwnd);                                     // 0: no Windows window is active (the desktop's own is)
uint32_t top_level_surface_locked(uint32_t hwnd);                         // the window surface (gdi::window_surface_pixels) of a top-level; the caller holds the lock
bool command_top_level(uint32_t hwnd, uint32_t sc);
// The cursor the desktop shows over the Windows programs' windows (takes the lock): hidden (ShowCursor's counter below 0, or a program set no cursor), else `cursor`
// (a cursor handle for cursor_picture; 0 with `system_id` for one of the system's shapes: OCR_NORMAL 32512, OCR_IBEAM 32513 ...).
struct PointerCursor {
  bool hidden = false;
  uint32_t cursor = 0, system_id = 32512;
};
void pointer_cursor(PointerCursor* out);
// The picture of a cursor (takes the lock): see user_misc::cursor_picture.
bool cursor_picture(uint32_t cursor, uint32_t* system_id, List<uint32_t>* argb, uint32_t* width, uint32_t* height, int32_t* hot_x, int32_t* hot_y);
// A GPU present into a window (a flip of a swapchain made for it): the picture (32-bit pixels, rows `pitch` bytes apart, B, G, R, A bytes or R, G, B, A when `rgba`)
// goes into the window surface over the client area, stretched to it (nearest pixel) when the sizes differ. False when the window is not a redirected one (the caller shows it
// on the display then). The caller holds the lock.
// Files dropped by the desktop (WM_DROPFILES). drop_target: the window under the screen point (x, y) that takes files (WS_EX_ACCEPTFILES, it or the nearest
// ancestor) inside the top-level `top_level`, with the point in its client coordinates; false when none does. drop_files: queues WM_DROPFILES there with
// the names (Windows paths, each NUL-terminated, a NUL after the last); its wParam is a ticket and its lParam kDropTicketMark, which the program's
// user32 turns into an HDROP when it takes the message (take_drop). Both take the lock.
struct DropTarget {
  uint32_t hwnd = 0;
  int32_t x = 0, y = 0;
  bool non_client = false;
};
inline constexpr uint64_t kDropTicketMark = 0x54434450;      // 'TCDP'
bool drop_target(uint32_t top_level, int32_t x, int32_t y, DropTarget* out);
bool drop_files(const DropTarget& target, ArrayView<const char16_t> names);
// The DROPFILES block (x64: pFiles, pt, fNC, fWide, then the wide names) of process `pid`'s drop `ticket`: its size, and when `out` is not empty
// (exactly that size), the block in it, after which the drop is gone. 0: no such drop (or `out` has the wrong size). The caller holds the lock.
uint64_t take_drop(uint64_t pid, uint32_t ticket, ByteView out);
bool window_redirected_locked(uint32_t hwnd);
bool present_to_window_locked(uint32_t hwnd, ConstByteView pixels, uint32_t width, uint32_t height, uint32_t pitch, bool rgba);                       // SC_CLOSE, SC_MINIMIZE, SC_MAXIMIZE, SC_RESTORE

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
// The cursor the desktop shows (the caller holds the lock): the ShowCursor counter, the cursor a thread set since the pointer came into its window (false: none, the
// class cursor shows), the pointer coming into a window of the thread, and the picture of a cursor: a system cursor's OCR_ id (32512 ...; nothing to draw), or else
// frame 0 as 0xAARRGGBB pixels (top row first) with its hot spot.
int32_t cursor_show_count();
bool thread_cursor(uint64_t tid, uint32_t* cursor);
void cursor_entered_window(uint64_t tid);
bool cursor_picture(uint32_t cursor, uint32_t* system_id, List<uint32_t>* argb, uint32_t* width, uint32_t* height, int32_t* hot_x, int32_t* hot_y);

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// (the user-misc module's own declarations follow)

}  // namespace process::win32k::user_misc
