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

// The desktop shell: a full-screen root window holding the desktop surface - where the window manager
// stacks application windows - and a taskbar along the bottom with the Start button (its menu opens
// upwards), the window switcher in the space in between, and the clock. The Start menu offers "File Explorer", "Task Manager", "Text Editor", "Paint"
// and "About" (ordinary windows) and "Shut Down...", which asks for confirmation in a modal dialog and then only
// requests the shutdown (shutdown_requested()): the boot code owns the hardware and does the orderly
// power-off once the desktop loop returns.
#pragma once
#include "Desktop/DesktopSurface.hpp"
#include "Desktop/FileDialog.hpp"
#include "Desktop/FileExplorer.hpp"
#include "Desktop/Notepad.hpp"
#include "Desktop/Paint.hpp"
#include "Desktop/TaskManager.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/MenuButton.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/InputPump.hpp"
#include "Gui/MessageDialog.hpp"
#include "Gui/RenderLoop.hpp"
#include "Gui/Window.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Gui/Windows/WindowManager.hpp"
#include "Gui/Windows/WindowSwitcher.hpp"

namespace desktop {

struct DateTime {
  uint16_t year = 0;
  uint8_t month = 0, day = 0;  // 1-based
  uint8_t hour = 0, minute = 0;  // 24-hour clock
};

// Where the taskbar clock gets the local time from; false if it cannot be read right now.
using ClockSource = bool (*)(void* context, DateTime* out);

// "MM/dd/yyyy hh:mm AM" - 12-hour clock, always 19 characters. `out` needs room for 20. Returns the length.
uint32_t format_date_time(const DateTime& time, char* out);

class Desktop {
 public:
  static constexpr int32_t kTaskbarHeight = 32;

  // The devices may be null (a machine without a mouse still shows the desktop).
  Desktop(allocators::IAllocator& allocator, graphics::IRenderer2D& renderer, hardware::IKeyboard* keyboard,
          hardware::IMouse* mouse);
  Desktop(const Desktop&) = delete;
  Desktop& operator=(const Desktop&) = delete;

  gui::Window& window() { return window_; }
  // The system clipboard that copy and paste in every window of the desktop use (nullptr: none).
  void set_clipboard(kernel::Clipboard* clipboard) { window_.set_clipboard(clipboard); }
  gui::MenuButton& start_button() { return start_; }
  gui::WindowManager& windows() { return windows_; }
  gui::WindowSwitcher& switcher() { return switcher_; }
  gui::MessageDialog& shutdown_dialog() { return shutdown_dialog_; }
  gui::Frame& about_window() { return about_; }
  gui::Frame& explorer_window() { return explorer_; }
  FileExplorer& explorer() { return explorer_; }
  // The filesystem the file explorer browses (nullptr: none). It must outlive the desktop.
  void set_file_system(filesystem::IFileSystem* fs) {
    explorer_.set_file_system(fs);
    notepad_.set_file_system(fs);
    paint_.set_file_system(fs);
    file_dialog_.set_file_system(fs);
  }
  Paint& paint() { return paint_; }
  // The Open and Save As dialog that the text editor and Paint share.
  FileDialog& file_dialog() { return file_dialog_; }
  // Opens the paint program, or brings it forward (restoring it if minimized).
  void open_paint();
  Notepad& notepad() { return notepad_; }
  // Opens the text editor, or brings it forward (restoring it if minimized).
  void open_text_editor();
  TaskManager& task_manager() { return task_manager_; }
  // The machine's figures for the task manager (nullptr: none). It must outlive the desktop.
  void set_system_monitor(ISystemMonitor* monitor) { task_manager_.set_monitor(monitor); }
  // Opens the task manager, or brings it forward (restoring it if minimized); it refreshes once a second meanwhile.
  void open_task_manager();
  // Opens the file explorer window, or brings it forward (restoring it if minimized).
  void open_explorer();
  // Opens the About window, or brings it forward (restoring it if minimized).
  void open_about();
  // Asks "shut down?" in a modal dialog; accepting it makes shutdown_requested() true.
  void ask_shut_down();
  StringView clock_text() const { return clock_.text(); }
  bool shutdown_requested() const { return __atomic_load_n(&shutdown_requested_, __ATOMIC_ACQUIRE); }
  void request_shutdown() { __atomic_store_n(&shutdown_requested_, true, __ATOMIC_RELEASE); }
  // The clock shows "--/--/---- --:-- --" until a source is set (and while it fails).
  void set_clock_source(ClockSource source, void* context) { clock_source_ = source; clock_context_ = context; shown_ = ~0ull; }

  // --- Running. The desktop uses three threads: the caller's loop (input and the clock: pump()), a render
  // thread (redraws whenever something changed) and the handler thread that runs what controls fire
  // (menu items, button clicks, dialog answers). A slow handler therefore cannot stop the screen
  // from updating or the mouse from moving; handlers and other threads may change controls freely
  // (every setter is thread-safe, see UiGuard).
  // Starts the render and handler threads; false if threads are unavailable (then use step()).
  bool start();
  // Ends both threads (what was queued still runs).
  void stop();
  // Where the desktop reports trouble it can see but not fix (no screen output: the screen may be what is stuck).
  void set_diagnostic_output(void (*write)(StringView text)) { diagnostic_ = write; }
  // Frames the render thread has drawn.
  uint32_t frames_drawn() const { return render_loop_.frames(); }
  // Delivers pending input and refreshes the clock (reads the source at most once per second of
  // `uptime_us`). Call in a loop from the thread that owns the input devices.
  void pump(uint64_t uptime_us);
  // Single-threaded alternative to start(): pump(), then draw if anything changed. Returns whether it drew.
  bool step(uint64_t uptime_us);

 private:
  static void on_paint(void* self, gui::Control&);
  static void on_text_editor(void* self, gui::Control&);
  static void on_explorer_file(void* self, gui::Control&);
  static void on_task_manager(void* self, gui::Control&);
  static void on_explorer(void* self, gui::Control&);
  static void on_about(void* self, gui::Control&);
  static void on_shut_down(void* self, gui::Control&);
  static void on_shutdown_answer(void* self, gui::Control&);

  gui::Theme theme_;
  gui::Window window_;
  gui::InputPump input_;
  gui::RenderLoop render_loop_;
  DesktopSurface surface_;
  gui::Panel taskbar_;
  gui::MenuButton start_;
  gui::Panel gap_left_, gap_right_;
  gui::WindowManager windows_;
  gui::WindowSwitcher switcher_;
  gui::Panel tray_;
  gui::Label clock_;
  gui::MessageDialog shutdown_dialog_;
  FileDialog file_dialog_;
  FileExplorer explorer_;
  TaskManager task_manager_;
  Notepad notepad_;
  Paint paint_;
  gui::Frame about_;
  gui::Label about_lines_[3];
  ClockSource clock_source_ = nullptr;
  void* clock_context_ = nullptr;
  void (*diagnostic_)(StringView) = nullptr;
  bool watching_ = false;  // the watchdog has its first sample
  uint32_t last_frames_ = 0;
  uint64_t stall_since_us_ = 0, stall_reported_us_ = 0;
  uint64_t polled_second_ = ~0ull;  // the uptime second the source was last read in
  uint64_t shown_ = ~0ull;          // what the label shows, packed (see step())
  uint64_t now_us_ = 0;             // the latest uptime pump() was given (the explorer measures double clicks with it)
  bool shutdown_requested_ = false;
};

}  // namespace desktop
