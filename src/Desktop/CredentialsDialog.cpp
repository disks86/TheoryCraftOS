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

#include "Desktop/CredentialsDialog.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace desktop {

using namespace gui;

CredentialsDialog::CredentialsDialog(allocators::IAllocator& allocator)
    : Frame(allocator), heading_(allocator), note_(allocator), user_label_(allocator), password_label_(allocator), user_row_(allocator), password_row_(allocator),
      buttons_(allocator), gap_(allocator), user_(allocator), password_(allocator), ok_(allocator), cancel_(allocator), server_(allocator) {
  set_title("Sign In");
  set_minimize_enabled(false);
  set_maximize_enabled(false);
  set_resizable(false);
  set_escape_closes(true);
  set_padding(Insets::all(10));

  heading_.set_dock(DockStyle::Top);
  heading_.set_size(0, 22);
  note_.set_dock(DockStyle::Top);
  note_.set_size(0, 22);

  user_label_.set_text("User name:");
  user_label_.set_dock(DockStyle::Left);
  user_label_.set_size(100, 0);
  user_.set_dock(DockStyle::Fill);
  user_.set_max_length(128);
  user_row_.set_style(PanelStyle::Transparent);
  user_row_.set_dock(DockStyle::Top);
  user_row_.set_size(0, 28);
  user_row_.add(&user_label_);
  user_row_.add(&user_);

  password_label_.set_text("Password:");
  password_label_.set_dock(DockStyle::Left);
  password_label_.set_size(100, 0);
  password_.set_dock(DockStyle::Fill);
  password_.set_password(true);
  password_.set_max_length(128);
  password_row_.set_style(PanelStyle::Transparent);
  password_row_.set_dock(DockStyle::Top);
  password_row_.set_size(0, 28);
  password_row_.add(&password_label_);
  password_row_.add(&password_);

  // Buttons along the bottom, right-aligned: Cancel at the far right, OK on its left.
  buttons_.set_style(PanelStyle::Transparent);
  buttons_.set_dock(DockStyle::Bottom);
  buttons_.set_size(0, 28);
  gap_.set_style(PanelStyle::Transparent);
  cancel_.set_text("Cancel");
  cancel_.set_dock(DockStyle::Right);
  cancel_.set_size(96, 0);
  gap_.set_dock(DockStyle::Right);
  gap_.set_size(10, 0);
  ok_.set_text("OK");
  ok_.set_dock(DockStyle::Right);
  ok_.set_size(96, 0);
  cancel_.on_click(Handler::immediate(&CredentialsDialog::on_cancel, this));
  ok_.on_click(Handler::immediate(&CredentialsDialog::on_ok, this));
  buttons_.add(&cancel_);
  buttons_.add(&gap_);
  buttons_.add(&ok_);

  add(&heading_);
  add(&note_);
  add(&user_row_);
  add(&password_row_);
  add(&buttons_);
}

void CredentialsDialog::clear() {
  UiGuard lock(this);
  user_.set_text(StringView());
  password_.set_text(StringView());
}

bool CredentialsDialog::show(WindowManager& manager, StringView server, StringView note, StringView user, Handler result, Style style, StringView password) {
  UiGuard lock(&manager);
  if (showing()) return false;
  __atomic_store_n(&accepted_, false, __ATOMIC_RELEASE);
  on_closed(result);
  server_.assign(server);
  const bool ids = style == Style::UnixIds;
  String heading(allocator());
  heading.assign(ids ? "Enter the user and group ids for " : "Enter the login for ");
  heading.append(server);
  heading_.set_text(heading.view());
  note_.set_text(note);
  user_label_.set_text(ids ? "User id:" : "User name:");
  password_label_.set_text(ids ? "Group id:" : "Password:");
  password_.set_password(!ids);
  user_.set_text(ids && user.empty() ? StringView("65534") : user);
  password_.set_text(ids ? (password.empty() ? StringView("65534") : password) : StringView());
  const int32_t area_w = static_cast<int32_t>(manager.bounds().width), area_h = static_cast<int32_t>(manager.bounds().height);
  set_bounds({(area_w - kWidth) / 2 > 0 ? (area_w - kWidth) / 2 : 0, (area_h - kHeight) / 3 > 0 ? (area_h - kHeight) / 3 : 0, static_cast<uint32_t>(kWidth),
              static_cast<uint32_t>(kHeight)});
  if (!manager.add_window(this, /*modal=*/true)) return false;
  if (Window* win = window()) {
    win->layout();
    win->set_focus(user.empty() ? static_cast<Control*>(&user_) : static_cast<Control*>(&password_));
  }
  return true;
}

void CredentialsDialog::on_ok(void* self, Control&) {
  auto* d = static_cast<CredentialsDialog*>(self);
  __atomic_store_n(&d->accepted_, true, __ATOMIC_RELEASE);
  d->close();
}

void CredentialsDialog::on_cancel(void* self, Control&) {
  auto* d = static_cast<CredentialsDialog*>(self);
  __atomic_store_n(&d->accepted_, false, __ATOMIC_RELEASE);
  d->close();
}

bool CredentialsDialog::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kEnter) {   // (in a text box too: Enter signs in)
    ok_.click();
    return true;
  }
  return Frame::on_key(args);
}

}  // namespace desktop
