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

// "Sign in": asks for a user name and a password for a server (a network share that wants a login), in a modal dialog. show() opens it; when it
// is dismissed - OK (or Enter) or Cancel (or Escape, or the close button) - the result handler runs and accepted() says which. The password is
// kept only until clear() (call it when done with it) or the next show().
#pragma once
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Gui/Windows/WindowManager.hpp"

namespace desktop {

class CredentialsDialog : public gui::Frame {
 public:
  static constexpr int32_t kWidth = 400, kHeight = 200;

  explicit CredentialsDialog(allocators::IAllocator& allocator);

  // Opens it modally in the middle of the manager's area. `server` is named in the heading; `note` (may be empty) is a line under it, e.g. why
  // the dialog is back. The user name box starts with `user` (what was typed last time). `result` runs once the dialog is gone.
  // Style::UnixIds (NFS): the two boxes are a user id and a group id (user() and password() are the numbers as typed; `password` starts the second box,
  // else 65534), shown in plain text.
  enum class Style { Login, UnixIds };
  bool show(gui::WindowManager& manager, StringView server, StringView note, StringView user, gui::Handler result, Style style = Style::Login,
            StringView password = StringView());
  bool showing() const { return manager() != nullptr; }
  bool accepted() const { return __atomic_load_n(&accepted_, __ATOMIC_ACQUIRE); }
  StringView server() const { return server_.view(); }
  StringView user() const { return user_.text(); }
  StringView password() const { return password_.text(); }
  // Forgets the password (and the user name).
  void clear();

  gui::TextBox& user_box() { return user_; }
  gui::TextBox& password_box() { return password_; }
  gui::Button& ok_button() { return ok_; }
  gui::Button& cancel_button() { return cancel_; }
  gui::Label& heading_label() { return heading_; }
  gui::Label& note_label() { return note_; }

  bool on_key(const gui::KeyArgs& args) override;

 private:
  static void on_ok(void* self, gui::Control&);
  static void on_cancel(void* self, gui::Control&);

  gui::Label heading_, note_, user_label_, password_label_;
  gui::Panel user_row_, password_row_, buttons_, gap_;
  gui::TextBox user_, password_;
  gui::Button ok_, cancel_;
  String server_;
  bool accepted_ = false;
};

}  // namespace desktop
