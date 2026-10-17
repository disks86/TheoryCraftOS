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

// A multi-line text editor. The text is plain bytes with '\n' between lines (a tab is shown as spaces up to the
// next multiple of 4 columns). Typing inserts at the caret (Enter keeps the line's indentation, Tab inserts a tab),
// Backspace/Delete/arrows/Home/End/Page Up/Page Down edit and move it, Ctrl+Home/End jump to the ends of the text.
// Shift with a movement key, a drag with the mouse or Ctrl+A select text; typing replaces the selection, and
// Ctrl+C / Ctrl+X / Ctrl+V (also Ctrl+Insert, Shift+Delete, Shift+Insert) copy, cut and paste through the window's clipboard. Long text scrolls
// (scroll bars, the mouse wheel, and the caret staying in view). No undo. Keys it does not use (Ctrl+S ...) go to the
// parent.
#pragma once
#include "Gui/Control.hpp"
#include "Gui/Controls/ContextMenu.hpp"
#include "Gui/Controls/ScrollBar.hpp"
#include "Utility/String.hpp"

namespace gui {

class TextEditor : public Control {
 public:
  static constexpr int32_t kBorder = 2, kPad = 3, kLineHeight = Painter::kGlyphHeight, kTabWidth = 4;
  static constexpr uint32_t kDefaultMaxLength = 1u << 20;

  explicit TextEditor(allocators::IAllocator& allocator);

  // --- Text. Offsets are bytes from the start; lines and columns count from 0.
  StringView text() const { return text_.view(); }
  // Replaces everything (the caret goes to the start, the text counts as unmodified). Text over the maximum length is
  // refused. False if it was or memory ran out.
  bool set_text(StringView text);
  // Replaces the selection (or inserts at the caret). False if read-only, too long or out of memory.
  bool insert_text(StringView text);
  bool modified() const { return modified_; }
  void mark_saved() { UiGuard lock(this); modified_ = false; fire(moved_); }
  void set_read_only(bool read_only) { UiGuard lock(this); read_only_ = read_only; }
  void set_max_length(uint32_t max) { UiGuard lock(this); max_length_ = max; }
  // Fired when the text changes / when the caret or selection moves (also after a change).
  void on_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }
  void on_caret_moved(Handler handler) { UiGuard lock(this); moved_ = handler; }

  // --- Caret and selection
  uint32_t caret() const { return caret_; }
  // Moves the caret (clamped); with `extend` the selection grows from where it was.
  void set_caret(uint32_t offset, bool extend = false);
  bool has_selection() const { return caret_ != anchor_; }
  uint32_t selection_start() const { return caret_ < anchor_ ? caret_ : anchor_; }
  uint32_t selection_end() const { return caret_ < anchor_ ? anchor_ : caret_; }
  void select_all();
  uint32_t caret_line() const { return line_of(caret_); }
  uint32_t caret_column() const { return column_at(line_of(caret_), caret_); }

  // --- Clipboard (the keys do the same). Copy and cut need a selection; false if there is none or it cannot be done.
  bool copy_selection();
  bool cut_selection();
  void paste();
  void delete_selection() { UiGuard lock(this); if (has_selection()) replace(selection_start(), selection_end(), StringView()); }

  // --- Lines
  uint32_t line_count() const { return static_cast<uint32_t>(starts_.count()); }
  uint32_t line_start(uint32_t line) const { return line < starts_.count() ? starts_[line] : static_cast<uint32_t>(text_.size()); }
  // Without the line break.
  uint32_t line_length(uint32_t line) const;
  uint32_t line_of(uint32_t offset) const;
  // The display column of `offset` on `line` (tabs expanded) and the offset nearest to a column.
  uint32_t column_at(uint32_t line, uint32_t offset) const;
  uint32_t offset_at(uint32_t line, uint32_t column) const;

  // --- Scrolling and geometry (for painting and tests)
  uint32_t top_line() const { return top_line_; }
  uint32_t left_column() const { return left_column_; }
  uint32_t visible_lines() const;
  uint32_t visible_columns() const;
  Rect2D body_rect() const;
  ScrollBar& vertical_scroll_bar() { return vscroll_; }
  ScrollBar& horizontal_scroll_bar() { return hscroll_; }
  // Where a character cell is, in this control's coordinates.
  Point cell_origin(uint32_t line, uint32_t column) const;

  bool focusable() const override { return true; }
  void layout() override;
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  void prepare_context_menu(int32_t x, int32_t y) override;
  void on_mouse_wheel(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {320, 200}; }

 private:
  bool replace(uint32_t from, uint32_t to, StringView with);
  void rebuild_lines();
  void update_scroll_bars();
  void ensure_caret_visible();
  void move_caret(uint32_t offset, bool extend, bool keep_column = false);
  uint32_t offset_at_point(int32_t x, int32_t y) const;
  void expand_line(uint32_t line, String* out) const;
  void on_scroll_changed();
  static void vertical_changed(void* self, Control&);
  static void horizontal_changed(void* self, Control&);

  static void opening(void* self, Control&);
  static void run_cut(void* self, Control&);
  static void run_copy(void* self, Control&);
  static void run_paste(void* self, Control&);
  static void run_delete(void* self, Control&);
  static void run_select_all(void* self, Control&);

  ContextMenu context_;   // Cut, Copy, Paste, Delete, Select All; built the first time it is wanted
  MenuItem *cut_item_ = nullptr, *copy_item_ = nullptr, *paste_item_ = nullptr, *delete_item_ = nullptr, *select_item_ = nullptr;
  String text_, clipboard_, scratch_;
  List<uint32_t> starts_;  // offset of each line's first byte
  uint32_t max_columns_ = 0;
  uint32_t caret_ = 0, anchor_ = 0;
  int32_t wanted_column_ = -1;  // the column Up/Down aim for
  uint32_t top_line_ = 0, left_column_ = 0;
  uint32_t max_length_ = kDefaultMaxLength;
  bool read_only_ = false, modified_ = false, dragging_ = false;
  bool show_v_ = false, show_h_ = false, syncing_ = false;
  ScrollBar vscroll_, hscroll_;
  Handler changed_, moved_;
};

}  // namespace gui
