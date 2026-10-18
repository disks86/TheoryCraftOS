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

// An address bar for paths made of '/'-separated names: every folder of the path is a button that goes back to that
// folder, with a chevron between them ("/ > docs > old"). A path too long for the bar loses its first folders
// (shown as "..."). Clicking the empty space after the last folder turns the bar into a text box for typing a path;
// Enter goes there, Escape or clicking elsewhere cancels. Either way the bar only reports where to go (on_navigate,
// requested_path()) - the program decides whether that works and then calls set_path().
#pragma once
#include "Gui/Control.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Utility/String.hpp"

namespace gui {

class BreadcrumbBar : public Control {
 public:
  static constexpr int32_t kPad = 4, kSeparatorWidth = 14, kEllipsisWidth = 32;

  // One clickable part of the bar, in this control's coordinates (for painting and tests).
  struct Segment {
    int32_t x = 0, width = 0;
    uint32_t path_length = 0;  // the segment goes to the first `path_length` bytes of the path ("" = the root)
    uint32_t label_start = 0, label_length = 0;
    bool ellipsis = false;
  };

  explicit BreadcrumbBar(allocators::IAllocator& allocator);

  // The path shown ("/" or "/a/b"; a missing leading slash is added, trailing slashes are dropped).
  void set_path(StringView path);
  StringView path() const { return path_.view(); }
  // Where the user asked to go (a clicked folder or a typed path, normalized as above).
  StringView requested_path() const { return requested_.view(); }
  // What was typed, as typed (not normalized), when the request came from the text box; empty after a click on a folder. For programs that accept
  // other kinds of address than a path ("smb://server/share").
  StringView typed_text() const { return typed_.view(); }
  void on_navigate(Handler handler) { UiGuard lock(this); navigate_ = handler; }

  // --- Typing a path
  bool editing() const { return editing_; }
  void begin_edit();
  // The same, with `text` in the box instead of the current path.
  void begin_edit(StringView text);
  void cancel_edit();
  TextBox& edit_box() { return edit_; }

  // --- Geometry (for painting and tests)
  uint32_t segment_count() const { return static_cast<uint32_t>(segments_.count()); }
  const Segment& segment(uint32_t index) const { return segments_[index]; }
  StringView segment_label(uint32_t index) const;
  int32_t hovered_segment() const { return hover_; }

  bool focusable() const override { return false; }
  void layout() override;
  void paint(Painter& painter) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_leave() override;
  void on_mouse_down(const MouseArgs& args) override;

 protected:
  Size default_size() const override { return {320, 24}; }

 private:
  // The text box shown while editing; hands Enter, Escape and losing the focus to the bar.
  class EditBox final : public TextBox {
   public:
    EditBox(allocators::IAllocator& allocator, BreadcrumbBar& owner) : TextBox(allocator), owner_(owner) {}
    bool on_key(const KeyArgs& args) override;
    void on_focus_changed(bool focused) override;

   private:
    BreadcrumbBar& owner_;
  };

  void rebuild();
  int32_t segment_at(int32_t x, int32_t y) const;
  void request(StringView path);
  void commit_edit();
  static bool normalize(StringView in, String* out);

  String path_, requested_, typed_;
  List<uint32_t> ends_;  // where each folder name ends in path_
  List<Segment> segments_;
  EditBox edit_;
  Handler navigate_;
  bool editing_ = false;
  int32_t hover_ = -1;
};

}  // namespace gui
