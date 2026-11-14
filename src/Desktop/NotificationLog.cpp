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

#include "Desktop/NotificationLog.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace desktop {

using namespace gui;

NotificationLog::NotificationLog(allocators::IAllocator& allocator)
    : Frame(allocator), heading_(allocator), grid_(allocator), buttons_(allocator), close_button_(allocator) {
  set_title("Notifications");
  set_bounds({170, 100, 520, 300});
  set_padding(Insets::all(8));
  set_minimize_enabled(false);
  set_maximize_enabled(false);
  set_in_taskbar(false);
  set_escape_closes(true);

  heading_.set_dock(DockStyle::Top);
  heading_.set_size(0, 22);

  buttons_.set_style(PanelStyle::Transparent);
  buttons_.set_dock(DockStyle::Bottom);
  buttons_.set_size(0, 30);
  close_button_.set_text("Close");
  close_button_.set_dock(DockStyle::Right);
  close_button_.set_size(90, 0);
  close_button_.set_default(true);
  close_button_.on_click(Handler::immediate(&NotificationLog::on_close, this));
  buttons_.add(&close_button_);

  grid_.add_column("Notification", 150);
  grid_.add_column("Message", 250);
  grid_.add_column("Kind", 90);
  grid_.set_dock(DockStyle::Fill);
  add(&heading_);   // (the label comes directly before the list it names)
  add(&grid_);
  add(&buttons_);
}

StringView NotificationLog::kind_name(NoticeKind kind) {
  return kind == NoticeKind::Warning ? "Warning" : kind == NoticeKind::Error ? "Problem" : "Information";
}

void NotificationLog::refresh(const ToastCenter& toasts) {
  UiGuard lock(this);
  serial_ = toasts.missed_serial();
  grid_.clear_rows();
  const uint32_t count = toasts.missed_count();
  for (uint32_t i = count; i > 0; --i) {   // (the newest first)
    String message(allocator());
    for (const char c : toasts.missed_message(i - 1)) message.push_back(c == '\n' ? ' ' : c);   // (the toast wrapped it over lines; a row has one)
    const StringView cells[3] = {toasts.missed_title(i - 1), message.view(), kind_name(toasts.missed_kind(i - 1))};
    grid_.add_row(cells, 3);
  }
  heading_.set_text(count ? "Notifications that went away before you clicked them, newest first:" : "You have not missed any notifications.");
}

// Enter is the Close button, the default one (the list has nothing to do on Enter).
bool NotificationLog::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kEnter) {
    close();
    return true;
  }
  return Frame::on_key(args);
}

void NotificationLog::on_close(void* self, Control&) { static_cast<NotificationLog*>(self)->close(); }

}  // namespace desktop
