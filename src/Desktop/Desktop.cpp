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

#include "Desktop/Desktop.hpp"

namespace desktop {

using namespace gui;

Desktop::Desktop(allocators::IAllocator& allocator, graphics::IRenderer2D& renderer, hardware::IKeyboard* keyboard,
                 hardware::IMouse* mouse)
    : window_(allocator, renderer, theme_, renderer.extent()),
      input_(window_, keyboard, mouse),
      render_loop_(window_),
      surface_(allocator),
      taskbar_(allocator),
      start_(allocator),
      gap_left_(allocator),
      gap_right_(allocator),
      windows_(allocator),
      switcher_(allocator, windows_),
      tray_(allocator),
      clock_(allocator),
      shutdown_dialog_(allocator),
      explorer_(allocator),
      task_manager_(allocator),
      notepad_(allocator),
      about_(allocator),
      about_lines_{Label(allocator), Label(allocator), Label(allocator)} {
  // Taskbar: Start at the left, the clock in a sunken tray at the right, the window switcher in between.
  taskbar_.set_style(PanelStyle::Raised);
  taskbar_.set_dock(DockStyle::Bottom);
  taskbar_.set_size(0, kTaskbarHeight);
  taskbar_.set_padding(Insets::all(4));

  start_.set_text("Start");
  start_.set_dock(DockStyle::Left);
  start_.set_size(76, 0);
  start_.set_direction(MenuButton::Direction::Up);
  start_.set_menu_min_width(200);
  start_.menu().add_item("File Explorer", {&Desktop::on_explorer, this});
  start_.menu().add_item("Task Manager", {&Desktop::on_task_manager, this});
  start_.menu().add_item("Text Editor", {&Desktop::on_text_editor, this});
  start_.menu().add_item("About TheoryCraftOS...", {&Desktop::on_about, this});
  start_.menu().add_separator();
  start_.menu().add_item("Shut Down...", {&Desktop::on_shut_down, this});

  gap_left_.set_style(PanelStyle::Transparent);
  gap_left_.set_dock(DockStyle::Left);
  gap_left_.set_size(6, 0);
  gap_right_.set_style(PanelStyle::Transparent);
  gap_right_.set_dock(DockStyle::Right);
  gap_right_.set_size(6, 0);

  tray_.set_style(PanelStyle::Sunken);
  tray_.set_dock(DockStyle::Right);
  tray_.set_size(180, 0);
  clock_.set_dock(DockStyle::Fill);
  clock_.set_text("--/--/---- --:-- --");
  tray_.add(&clock_);

  switcher_.set_dock(DockStyle::Fill);

  taskbar_.add(&start_);
  taskbar_.add(&gap_left_);
  taskbar_.add(&tray_);
  taskbar_.add(&gap_right_);
  taskbar_.add(&switcher_);  // Fill goes last

  // The window manager fills the desktop surface: windows are stacked above the desktop, below the taskbar.
  windows_.set_dock(DockStyle::Fill);
  surface_.set_dock(DockStyle::Fill);
  surface_.add(&windows_);

  shutdown_dialog_.set_texts("Shut Down", "Shut down TheoryCraftOS now?", "Running services will be stopped.", "Shut Down", "Cancel");
  shutdown_dialog_.on_result({&Desktop::on_shutdown_answer, this});

  explorer_.set_clock(&now_us_);
  explorer_.on_file_opened({&Desktop::on_explorer_file, this});

  about_.set_title("About TheoryCraftOS");
  about_.set_bounds({140, 90, 400, 170});
  about_.set_padding(Insets::all(12));
  about_.set_minimize_enabled(true);
  static const char* const kAbout[3] = {"TheoryCraftOS", "A minimal UEFI x86_64 operating system.",
                                        "Move, resize, minimize or maximize windows."};
  for (int i = 0; i < 3; ++i) {
    about_lines_[i].set_text(kAbout[i]);
    about_lines_[i].set_dock(DockStyle::Top);
    about_lines_[i].set_size(0, 22);
    about_.add(&about_lines_[i]);
  }

  // Docking order: the strip first, then what fills the rest.
  surface_.set_dock(DockStyle::Fill);
  window_.add(&taskbar_);
  window_.add(&surface_);
  window_.show_cursor(mouse != nullptr);
  window_.layout();
}

void Desktop::open_explorer() {
  if (explorer_.manager()) explorer_.activate();
  else windows_.add_window(&explorer_);
}

void Desktop::open_task_manager() {
  if (task_manager_.manager()) {
    task_manager_.activate();
  } else {
    task_manager_.update(now_us_);
    windows_.add_window(&task_manager_);
  }
}

void Desktop::open_text_editor() {
  if (notepad_.manager()) notepad_.activate();
  else windows_.add_window(&notepad_);
}

void Desktop::open_about() {
  if (about_.manager()) about_.activate();
  else windows_.add_window(&about_);
}

void Desktop::ask_shut_down() { shutdown_dialog_.show(windows_); }

void Desktop::on_task_manager(void* self, Control&) { static_cast<Desktop*>(self)->open_task_manager(); }
void Desktop::on_text_editor(void* self, Control&) { static_cast<Desktop*>(self)->open_text_editor(); }
void Desktop::on_explorer_file(void* self, Control&) {
  auto* d = static_cast<Desktop*>(self);
  d->notepad_.open_file(d->explorer_.opened_file());
  d->open_text_editor();
}
void Desktop::on_explorer(void* self, Control&) { static_cast<Desktop*>(self)->open_explorer(); }
void Desktop::on_about(void* self, Control&) { static_cast<Desktop*>(self)->open_about(); }
void Desktop::on_shut_down(void* self, Control&) { static_cast<Desktop*>(self)->ask_shut_down(); }

void Desktop::on_shutdown_answer(void* self, Control&) {
  auto* d = static_cast<Desktop*>(self);
  if (d->shutdown_dialog_.accepted()) d->request_shutdown();
}

uint32_t format_date_time(const DateTime& t, char* out) {
  auto two = [&](uint32_t at, uint32_t v) {
    out[at] = static_cast<char>('0' + (v / 10) % 10);
    out[at + 1] = static_cast<char>('0' + v % 10);
  };
  uint32_t hour12 = t.hour % 12;
  if (hour12 == 0) hour12 = 12;
  two(0, t.month);
  out[2] = '/';
  two(3, t.day);
  out[5] = '/';
  two(6, t.year / 100);
  two(8, t.year % 100);
  out[10] = ' ';
  two(11, hour12);
  out[13] = ':';
  two(14, t.minute);
  out[16] = ' ';
  out[17] = t.hour < 12 ? 'A' : 'P';
  out[18] = 'M';
  out[19] = 0;
  return 19;
}

bool Desktop::start() {
  const bool handlers = window_.start_handler_thread();
  const bool rendering = render_loop_.start();
  if (handlers && rendering) return true;
  stop();
  return false;
}

void Desktop::stop() {
  render_loop_.stop();
  window_.stop_handler_thread();
}

bool Desktop::step(uint64_t uptime_us) {
  pump(uptime_us);
  if (!window_.dirty()) return false;
  window_.render();
  return true;
}

void Desktop::pump(uint64_t uptime_us) {
  __atomic_store_n(&now_us_, uptime_us, __ATOMIC_RELAXED);
  input_.pump();
  // Watchdog: a changed window that the render thread has not redrawn for seconds means that thread (or
  // whatever it is waiting for - the GPU, the UI lock) is stuck. Say so where it can be read afterwards.
  if (render_loop_.running() && diagnostic_) {
    const uint32_t frames = render_loop_.frames();
    if (!watching_ || frames != last_frames_ || !window_.dirty()) {
      watching_ = true;
      last_frames_ = frames;
      stall_since_us_ = uptime_us;
    } else if (uptime_us - stall_since_us_ > 3000000 && uptime_us - stall_reported_us_ > 5000000) {
      stall_reported_us_ = uptime_us;
      char text[64] = "desktop: no frame drawn for    s although the window changed\n";
      const uint64_t seconds = (uptime_us - stall_since_us_) / 1000000;
      text[26] = seconds >= 100 ? static_cast<char>('0' + (seconds / 100) % 10) : ' ';
      text[27] = seconds >= 10 ? static_cast<char>('0' + (seconds / 10) % 10) : ' ';
      text[28] = static_cast<char>('0' + seconds % 10);
      diagnostic_(text);
    }
  }
  const uint64_t second = uptime_us / 1000000;
  if (second != polled_second_) {
    polled_second_ = second;
    if (task_manager_.manager()) task_manager_.update(uptime_us);
    DateTime now;
    const bool ok = clock_source_ && clock_source_(clock_context_, &now);
    // The label only changes when the minute does; ~0 stands for "no time".
    const uint64_t packed = ok ? ((((uint64_t{now.year} * 13 + now.month) * 32 + now.day) * 24 + now.hour) * 60 + now.minute) : ~0ull - 1;
    if (packed != shown_) {
      shown_ = packed;
      char text[24];
      if (ok) {
        clock_.set_text(StringView(text, format_date_time(now, text)));
      } else {
        clock_.set_text("--/--/---- --:-- --");
      }
    }
  }
}

}  // namespace desktop
