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

#pragma once
#include "Gui/Control.hpp"
#include "Gui/Controls/ContextMenu.hpp"
#include "Utility/String.hpp"

namespace gui {

// A single-line text editor: typing inserts at the caret, Backspace/Delete/Left/Right/Home/End edit and
// move it, a click places it. Long text scrolls horizontally. Shift with a movement key, a drag with the mouse or Ctrl+A
// select text, and typing replaces the selection. Ctrl+C / Ctrl+X / Ctrl+V (also Ctrl+Insert, Shift+Delete, Shift+Insert)
// copy, cut and paste through the clipboard; line breaks in pasted text become spaces.
class TextBox : public Control {
 public:
  explicit TextBox(allocators::IAllocator& allocator);

  StringView text() const { return text_.view(); }
  // Replaces the text (the caret goes to the end). False if memory ran out.
  bool set_text(StringView text);
  void set_read_only(bool read_only) { UiGuard lock(this); read_only_ = read_only; }
  void set_max_length(uint32_t max) { UiGuard lock(this); max_length_ = max; }
  uint32_t caret() const { return caret_; }
  bool has_selection() const { return caret_ != anchor_; }
  uint32_t selection_start() const { return caret_ < anchor_ ? caret_ : anchor_; }
  uint32_t selection_end() const { return caret_ < anchor_ ? anchor_ : caret_; }
  void select_all();
  // The clipboard commands: what the keys and the context menu (Cut, Copy, Paste, Delete, Select All) do. False if there was nothing to do.
  bool copy_selection();
  bool cut_selection();
  bool delete_selection() { UiGuard lock(this); return has_selection() && replace_selection(StringView()); }
  void paste();
  StringView selected_text() const { return text_.view().substr(selection_start(), selection_end() - selection_start()); }
  void on_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override { dragging_ = false; }
  void prepare_context_menu(int32_t x, int32_t y) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {160, 24}; }

 private:
  void scroll_to_caret();
  uint32_t index_at(int32_t x) const;
  void move_caret(uint32_t to, bool extend);
  bool replace_selection(StringView with);
  uint32_t visible_chars() const;
  void changed();

  static void opening(void* self, Control&);
  static void run_cut(void* self, Control&);
  static void run_copy(void* self, Control&);
  static void run_paste(void* self, Control&);
  static void run_delete(void* self, Control&);
  static void run_select_all(void* self, Control&);

  ContextMenu context_;   // Cut, Copy, Paste, Delete, Select All; built the first time it is wanted
  MenuItem *cut_item_ = nullptr, *copy_item_ = nullptr, *paste_item_ = nullptr, *delete_item_ = nullptr, *select_item_ = nullptr;
  String text_, fallback_;  // fallback_: the clipboard when the window has none
  Handler changed_;
  uint32_t caret_ = 0, anchor_ = 0;  // between characters: 0 .. text length; the selection runs between them
  uint32_t scroll_ = 0;  // first visible character
  uint32_t max_length_ = 0;  // 0 = unlimited
  bool read_only_ = false, dragging_ = false;
};

}  // namespace gui
