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

// A small modal question: a message of one or two lines and two buttons (the default one first,
// e.g. "Shut Down" / "Cancel"). Open it with show(); when it is dismissed - either button, the
// close button, or Escape - the result handler runs and accepted() says whether the default button was
// chosen. Enter chooses the default button. Minimize and maximize are disabled.
#pragma once
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Gui/Windows/WindowManager.hpp"

namespace gui {

class MessageDialog : public Frame {
 public:
  static constexpr int32_t kWidth = 380, kHeight = 150;

  explicit MessageDialog(allocators::IAllocator& allocator);

  // Texts; call before show().
  void set_texts(StringView title, StringView line1, StringView line2, StringView accept, StringView decline);
  // Runs after the dialog has closed and been removed from the manager.
  void on_result(Handler handler) { result_ = handler; }
  // True if the default (first) button was chosen.
  bool accepted() const { return accepted_; }
  bool showing() const { return manager() != nullptr; }
  Button& accept_button() { return accept_; }
  Button& decline_button() { return decline_; }

  // Opens it modally in the middle of the manager's area and focuses the default button.
  bool show(WindowManager& manager);

  bool on_key(const KeyArgs& args) override;

 private:
  static void on_accept(void* self, Control&);
  static void on_decline(void* self, Control&);
  static void on_closed_thunk(void* self, Control&);

  Label line1_, line2_;
  Panel buttons_, gap_;
  Button accept_, decline_;
  Handler result_;
  bool accepted_ = false;
};

}  // namespace gui
