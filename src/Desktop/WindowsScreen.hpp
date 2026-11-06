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

// The window that shows the screen of the Windows programs (win32k's one monitor) on the desktop and hands them the keyboard and the mouse.
//
// The screen and the input are the kernel's (IWindowsSession; the kernel's side is src/Process/Win32k/Win32kDesktopBridge.cpp). The window opens by itself when the
// screen first changes (a Windows program showed a window) and can be opened from the Start menu. What it hands over:
//   keys       every press and release while the window is the active one, as USB HID usages (the session makes virtual keys and characters of them);
//   the mouse  while the pointer is over the picture, or a button pressed there is held: a relative mouse's motion as it came (a game's mouse look reads it whole),
//              an absolute one's position, the buttons and the wheel. The pointer then follows the Windows cursor, so a program that confines it (ClipCursor) or moves
//              it (SetCursorPos) keeps it in the picture; Ctrl+Alt lets it go until the next click on the picture.
// Threads: the screen changes are pulled by the desktop's pump (the session only notes them, under the kernel's lock); the input arrives on the input pump's thread
// through the taps (Desktop forwards them); neither holds the UI lock while it calls the session.
#pragma once
#include "Desktop/WindowsSession.hpp"
#include "Gui/Control.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Gui/Windows/WindowManager.hpp"
#include "Hardware/Keyboard/IKeyboard.hpp"
#include "Hardware/Mouse/IMouse.hpp"

namespace desktop {

class WindowsScreenView : public gui::Control {
 public:
  explicit WindowsScreenView(allocators::IAllocator& allocator) : Control(allocator) {}
  ~WindowsScreenView();
  void paint(gui::Painter& painter) override;
  bool focusable() const override { return true; }
  bool on_key(const gui::KeyArgs&) override { return true; }      // (the keys went to the Windows programs through the tap)
  // Takes the picture (under the UI lock): false when there is no memory for it.
  bool set_picture(ArrayView<const uint32_t> pixels, uint32_t width, uint32_t height);
  uint32_t picture_width() const { return width_; }
  uint32_t picture_height() const { return height_; }

 private:
  ArrayView<uint32_t> pixels_;
  uint32_t width_ = 0, height_ = 0;
};

class WindowsScreen : public gui::Frame {
 public:
  static constexpr int32_t kFrameWidth = 2 * gui::Frame::kBorder, kFrameHeight = 2 * gui::Frame::kBorder + gui::Frame::kTitleHeight;

  explicit WindowsScreen(allocators::IAllocator& allocator);
  ~WindowsScreen();
  void set_session(IWindowsSession* session) { session_ = session; }
  IWindowsSession* session() const { return session_; }
  // Opens the window (sized to the screen) or brings it to the front.
  bool open(gui::WindowManager& manager);
  bool showing() const { return manager() != nullptr; }
  // The desktop's pump: takes the screen when it changed, and opens the window the first time it does. Not under the UI lock.
  void pump(gui::WindowManager& manager);
  // The input taps (see gui::InputPump::set_taps), forwarded by the desktop. `x`, `y`: where the pointer is about to be (desktop window coordinates).
  void tap_key(const hardware::KeyEvent& event, uint8_t modifiers);
  void tap_mouse(const hardware::MouseEvent& event, int32_t* x, int32_t* y);
  WindowsScreenView& view() { return view_; }
  bool grabbed() const { return grabbed_; }

 private:
  bool picture_area(gui::Rect2D* area) const;      // the picture in desktop window coordinates (false when the window is not showing)
  void release_grab();

  WindowsScreenView view_;
  IWindowsSession* session_ = nullptr;
  ArrayView<uint32_t> staging_;      // the screen as copied from the session (outside the UI lock)
  bool opened_once_ = false;
  bool sized_ = false;               // the screen was made to fit the desktop's work area (once, at the first pump)
  bool refresh_ = false;             // take the picture at the next pump even if nothing changed (the window was opened by hand)
  bool grabbed_ = false;             // the pointer is the Windows programs' (it entered the picture or a button went down there)
  bool released_ = false;            // Ctrl+Alt let the pointer go: nothing is handed over until the next click on the picture
  uint8_t buttons_ = 0;              // the buttons held as the Windows programs know them (hardware::mouse_button bits)
  uint8_t keys_down_[32] = {};       // the keys whose press was handed over (their release is too, wherever the focus went since)
};

}  // namespace desktop
