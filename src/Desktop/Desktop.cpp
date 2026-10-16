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
      surface_(allocator),
      taskbar_(allocator),
      start_(allocator),
      tray_(allocator),
      clock_(allocator) {
  // Taskbar: Start at the left, the clock in a sunken tray at the right.
  taskbar_.set_style(PanelStyle::Raised);
  taskbar_.set_dock(DockStyle::Bottom);
  taskbar_.set_size(0, kTaskbarHeight);
  taskbar_.set_padding(Insets::all(4));

  start_.set_text("Start");
  start_.set_dock(DockStyle::Left);
  start_.set_size(76, 0);
  start_.set_direction(MenuButton::Direction::Up);
  start_.set_menu_min_width(180);
  start_.menu().add_item("Shut Down", {&Desktop::on_shut_down, this});

  tray_.set_style(PanelStyle::Sunken);
  tray_.set_dock(DockStyle::Right);
  tray_.set_size(180, 0);
  clock_.set_dock(DockStyle::Fill);
  clock_.set_text("--/--/---- --:-- --");
  tray_.add(&clock_);

  taskbar_.add(&start_);
  taskbar_.add(&tray_);

  // Docking order: the strip first, then what fills the rest.
  surface_.set_dock(DockStyle::Fill);
  window_.add(&taskbar_);
  window_.add(&surface_);
  window_.show_cursor(mouse != nullptr);
  window_.layout();
}

void Desktop::on_shut_down(void* self, Control&) { static_cast<Desktop*>(self)->shutdown_requested_ = true; }

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

bool Desktop::step(uint64_t uptime_us) {
  input_.pump();
  const uint64_t second = uptime_us / 1000000;
  if (second != polled_second_) {
    polled_second_ = second;
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
  if (!window_.dirty()) return false;
  window_.render();
  return true;
}

}  // namespace desktop
