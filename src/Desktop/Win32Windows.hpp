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

// The windows of the Windows programs as windows of the desktop: each visible top-level window of win32k is a gui::Frame in the desktop's window manager, stacked,
// activated, moved, sized, minimized, maximized and closed like any other, with its taskbar button and Alt+Tab place.
// The X server's windows go through the same code (a second instance over x11::Server, which implements IWindowsSession too: docs/X11.md), so the two kinds
// of foreign window look and behave alike.
//
// One coordinate space: win32k's screen is the window manager's area, so a window's rectangle is its frame's bounds. The desktop draws the title bar and frame of a
// window with a caption (win32k's non-client sizes are the frame's); the program's picture of the inside (its window surface) is drawn by the view straight from the
// kernel's memory (IWindowsSession::draw_window), so a frame costs the same one copy as a native window. A window without a caption is an undecorated frame of the
// program's picture alone; a menu, tooltip or drop-down (WindowsWindowInfo::popup) is never activated by the desktop and has no taskbar button.
//
// Both ways:
//   the program   shows, hides, moves, sizes, retitles, restyles, restacks, activates or destroys a window -> the session notes it -> pump() makes the frame follow
//                 (the frames of the programs' windows keep the programs' stacking order among themselves, an owned window above its owner; the desktop's own
//                 windows keep their places between them);
//   the user      drags or sizes the frame, presses its buttons, clicks it -> pump() sees the frame's change -> the session asks the program (WM_SYSCOMMAND / activation),
//                 which answers as it would on Windows; what it does comes back as a change. The frames' changes are taken first, and a frame the user moved,
//                 sized, minimized or maximized is not put back by an older report of the program's while it has not caught up (kHoldPumps).
// A window without a caption (or one that draws its own) is moved and sized by the user when the program asks (a press it reports as on its caption or a border:
// SC_MOVE / SC_SIZE): its frame follows the pointer until the left button is let go, and the program hears of it as for any move.
// Input (the taps, on the input pump's thread): the mouse goes to the programs while the pointer is over the picture of one of their windows, a button pressed there
// is held, a program has the mouse captured, or a program confines the cursor (ClipCursor) while one of its windows is active; then the pointer follows the Windows
// cursor. Ctrl+Alt lets a confined pointer go until the next click on a Windows window. Keys go to the programs while one of their windows is the active one.
// Locks: the frames are changed under the UI lock; the session takes the kernel's lock itself (UI lock -> system lock is the order, so painting may call it).
#pragma once
#include "Desktop/WindowsSession.hpp"
#include "Gui/Control.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Gui/Windows/WindowManager.hpp"
#include "Hardware/Keyboard/IKeyboard.hpp"
#include "Hardware/Mouse/IMouse.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace desktop {

class Win32Windows;

// The program's picture of a window: drawn from the kernel's window surface as it is. The pointer over it is the cursor the program shows.
class Win32WindowView : public gui::Control {
 public:
  Win32WindowView(allocators::IAllocator& allocator, IWindowsSession& session, uint32_t window, const Win32Windows& owner)
      : Control(allocator), session_(session), window_(window), owner_(owner) {}
  void paint(gui::Painter& painter) override;
  gui::CursorShape cursor_shape(int32_t x, int32_t y) const override;
  bool cursor_image(graphics::Surface* image, gui::Point* hot) const override;
  bool focusable() const override { return true; }
  bool on_key(const gui::KeyArgs&) override { return true; }      // (the keys went to the program through the tap)

 private:
  IWindowsSession& session_;
  uint32_t window_;
  const Win32Windows& owner_;
};

class Win32Window : public gui::Frame {
 public:
  Win32Window(allocators::IAllocator& allocator, IWindowsSession& session, uint32_t window, const Win32Windows& owner);
  uint32_t window() const { return window_; }
  Win32WindowView& view() { return view_; }
  // Where the program's picture is, in the window manager's coordinates (= win32k's screen; false while the frame is not showing).
  bool picture_area(gui::Rect2D* area) const;

 protected:
  bool close_requested() override;      // the program is asked (SC_CLOSE); it destroys the window if it agrees

 private:
  friend class Win32Windows;
  IWindowsSession& session_;
  uint32_t window_;
  Win32WindowView view_;
  gui::Rect2D known_;                   // the bounds the frame and the program last agreed on
  gui::Rect2D sent_;                    // the bounds last asked of the program
  uint32_t hold_ = 0;                   // pumps left during which the program's older rectangle does not move the frame back
  State known_state_ = State::Normal;   // the state the program last reported (or was asked for)
  State sent_state_ = State::Normal;    // the state last asked of the program
  uint32_t state_hold_ = 0;             // pumps left during which the program's older state does not undo the user's
  bool known_active_ = false;
  uint32_t icon_ = 0;                   // the icon handle the frame shows
};

class Win32Windows {
 public:
  static constexpr uint32_t kHoldPumps = 30;

  explicit Win32Windows(allocators::IAllocator& allocator) : allocator_(allocator), hosts_(allocator), changed_(allocator), repainted_(allocator), order_(allocator), pointer_pixels_(allocator) {}
  ~Win32Windows();
  // Null: none (the frames are taken down).
  void set_session(IWindowsSession* session, gui::WindowManager* manager);
  IWindowsSession* session() const { return session_; }
  // The desktop's pump: attaches the session (once the window manager has its size), makes the frames follow the programs and the programs follow the frames.
  // Not under the UI lock.
  void pump(gui::WindowManager& manager);
  // The desktop's monitors (desktop window coordinates; their work areas), the primary first; none: one, the window manager's area. Under the UI lock; the session
  // hears of it at the next pump.
  void set_monitors(ArrayView<const gui::Rect2D> regions, ArrayView<const gui::Rect2D> areas);
  // The input taps (see gui::InputPump::set_taps), forwarded by the desktop. `x`, `y`: where the pointer is about to be (desktop window coordinates).
  void tap_key(const hardware::KeyEvent& event, uint8_t modifiers);
  void tap_mouse(const hardware::MouseEvent& event, int32_t* x, int32_t* y);

  uint32_t count() const { return static_cast<uint32_t>(hosts_.count()); }
  // The cursor the programs show (pump() keeps it; read under the UI lock).
  gui::CursorShape pointer_shape() const { return pointer_shape_; }
  bool pointer_image(graphics::Surface* image, gui::Point* hot) const;
  Win32Window* host(uint32_t window) const;
  Win32Window* host_at(uint32_t index) const { return index < hosts_.count() ? hosts_[index].get() : nullptr; }
  bool grabbed() const { return grabbed_; }

 private:
  // (under the UI lock) the frame of a window that is shown follows what the program reports; one that is gone or hidden is taken down
  void apply(gui::WindowManager& manager, uint32_t window, const WindowsWindowInfo& info, const String& title);
  void remove(gui::WindowManager& manager, uint32_t window);
  void follow_frames(gui::WindowManager& manager);                // what the user did to the frames, to the programs
  void restack(gui::WindowManager& manager);                      // the frames in the programs' stacking order
  Win32Window* host_under(gui::WindowManager& manager, int32_t x, int32_t y, gui::Rect2D* area) const;      // manager coordinates
  void release_buttons();
  void drag_to(int32_t x, int32_t y);      // (UI lock held)

  allocators::IAllocator& allocator_;
  IWindowsSession* session_ = nullptr;
  gui::WindowManager* manager_ = nullptr;
  List<UniquePointer<Win32Window>> hosts_;
  List<uint32_t> changed_;
  List<IWindowsSession::Repaint> repainted_;
  List<uint32_t> order_;                   // the programs' stacking order (bottom first), to restack the frames by
  bool attached_ = false;
  // The monitors as set_monitors had them, and whether the session has them (under the UI lock).
  gui::Rect2D regions_[8], areas_[8];
  uint32_t monitor_count_ = 0;
  bool display_sent_ = false;
  gui::Rect2D sent_bounds_;
  gui::Frame* last_active_ = nullptr;
  // A drag a program asked for (a custom title bar or border: IWindowsSession::take_drag): the frame follows the pointer until the left button is let go. Under the UI lock.
  Win32Window* drag_ = nullptr;
  uint32_t drag_edge_ = 0;
  int32_t drag_x_ = 0, drag_y_ = 0;        // where the pointer was when it began (desktop window coordinates)
  gui::Rect2D drag_from_;
  int32_t pointer_x_ = 0, pointer_y_ = 0;  // the pointer as last seen by the tap
  // The programs' cursor (under the UI lock): its shape, and for a picture the pixels of `pointer_cursor_`.
  gui::CursorShape pointer_shape_ = gui::CursorShape::Arrow;
  uint32_t pointer_cursor_ = 0, pointer_w_ = 0, pointer_h_ = 0;
  gui::Point pointer_hot_;
  mutable List<uint32_t> pointer_pixels_;      // (mutable: handed to the renderer as a Surface, which only reads it)
  void follow_pointer(gui::WindowManager& manager);      // (not under the UI lock)      // the window manager's active frame as last seen (a change since is the user's)
  // input (the input pump's thread)
  bool grabbed_ = false;                   // the pointer is the Windows programs'
  bool released_ = false;                  // Ctrl+Alt let a confined pointer go: nothing is handed over until the next click on a Windows window
  uint8_t buttons_ = 0;                    // the buttons held as the programs know them (hardware::mouse_button bits)
  uint8_t keys_down_[32] = {};             // the keys whose press was handed over (their release is too)
};

}  // namespace desktop
