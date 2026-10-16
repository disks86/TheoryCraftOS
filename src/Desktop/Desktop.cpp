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
  tray_.set_size(110, 0);
  clock_.set_dock(DockStyle::Fill);
  clock_.set_text("Up 00:00:00");
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

bool Desktop::step(uint64_t uptime_us) {
  input_.pump();
  const uint64_t seconds = uptime_us / 1000000;
  if (seconds != shown_seconds_) {
    shown_seconds_ = seconds;
    // "Up HH:MM:SS"
    char text[16] = {'U', 'p', ' ', 0, 0, ':', 0, 0, ':', 0, 0, 0};
    const uint64_t h = (seconds / 3600) % 100, m = (seconds / 60) % 60, sec = seconds % 60;
    text[3] = static_cast<char>('0' + h / 10);
    text[4] = static_cast<char>('0' + h % 10);
    text[6] = static_cast<char>('0' + m / 10);
    text[7] = static_cast<char>('0' + m % 10);
    text[9] = static_cast<char>('0' + sec / 10);
    text[10] = static_cast<char>('0' + sec % 10);
    clock_.set_text(StringView(text, 11));
  }
  if (!window_.dirty()) return false;
  window_.render();
  return true;
}

}  // namespace desktop
