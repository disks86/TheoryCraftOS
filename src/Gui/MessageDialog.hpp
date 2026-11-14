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
// chosen. Enter chooses the default button, which has the heavier outline. Minimize and maximize are disabled.
//
// A question with two ways to go on has a third button between the two (set_alternate): closing a document with unsaved changes asks
// "Save" / "Don't Save" / "Cancel". choice() then tells the three apart. Name the buttons after what they do; the default one is the safe choice,
// and the last one (Escape, the close button) leaves everything as it was.
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

  enum class Choice : uint8_t { Decline, Accept, Alternate };

  explicit MessageDialog(allocators::IAllocator& allocator);

  // Texts; call before show(). The title names the document or the program the question is about. (Takes the third button away again.)
  void set_texts(StringView title, StringView line1, StringView line2, StringView accept, StringView decline);
  // Which button Enter presses, has the heavier outline and starts with the focus: the first one unless told otherwise. A question whose first
  // button destroys something (Delete, Replace) makes the last one, which leaves everything as it was, the default: set_default_choice(Choice::Decline).
  // set_texts() makes the first button the default again.
  void set_default_choice(Choice choice);
  Choice default_choice() const { return default_; }
  // The third button, between the other two (empty: none); call after set_texts().
  void set_alternate(StringView text);
  // The question asked before a document with unsaved changes goes away: "Save" (the default), "Don't Save", "Cancel". `document` is its name
  // as the title bar shows it.
  void set_save_changes(StringView document);
  // Runs after the dialog has closed and been removed from the manager.
  void on_result(Handler handler) { on_closed(handler); }
  // True if the default (first) button was chosen.
  bool accepted() const { return choice() == Choice::Accept; }
  // Which button it was (Decline also for Escape and the close button).
  Choice choice() const { return static_cast<Choice>(__atomic_load_n(&choice_, __ATOMIC_ACQUIRE)); }
  bool showing() const { return manager() != nullptr; }
  Button& accept_button() { return accept_; }
  Button& decline_button() { return decline_; }
  Button& alternate_button() { return alternate_; }

  // Opens it modally in the middle of the manager's area and focuses the default button.
  bool show(WindowManager& manager);

  bool on_key(const KeyArgs& args) override;

 private:
  static void on_accept(void* self, Control&);
  static void on_decline(void* self, Control&);
  static void on_alternate(void* self, Control&);
  void finish(Choice choice);

  Label line1_, line2_;
  Panel buttons_, gap_, gap2_;
  Button accept_, decline_, alternate_;
  Button& default_button() { return default_ == Choice::Accept ? accept_ : default_ == Choice::Alternate ? alternate_ : decline_; }
  uint8_t choice_ = 0;
  Choice default_ = Choice::Accept;
};

}  // namespace gui
