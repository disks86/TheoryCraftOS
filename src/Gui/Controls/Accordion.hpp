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

// An accordion: a vertical stack of sections, each a header bar (an arrow and a title) over a body that is shown only while the section is expanded.
// Bodies are ordinary controls (usually Panels) that the caller owns, like every control; add_section() puts one in the accordion and the accordion
// places it under its header, as wide as the accordion and as tall as asked for. By default only one section is open at a time (opening one closes
// the others); set_exclusive(false) lets any number be open. The first section added starts open. A click on a header, or Enter / Space with the focus
// on it, toggles the section; Up/Down move between headers and Home/End go to the first and last. Sections that do not fit the height are cut off.
#pragma once
#include "Gui/Control.hpp"
#include "Utility/String.hpp"

namespace gui {

class Accordion : public Control {
 public:
  static constexpr int32_t kHeaderHeight = 24, kBorder = 2, kDefaultBodyHeight = 100;

  explicit Accordion(allocators::IAllocator& allocator)
      : Control(allocator), titles_(allocator), bodies_(allocator), heights_(allocator), open_(allocator) {}

  // Adds a section after the others and returns its index, or -1 (no body, or memory ran out). `body_height` is how tall the body is while open; 0 takes
  // the body's own preferred height, or kDefaultBodyHeight if it has none.
  int32_t add_section(StringView title, Control* body, int32_t body_height = 0);
  int32_t section_count() const { return static_cast<int32_t>(titles_.count()); }
  StringView section_title(int32_t index) const { return index >= 0 && static_cast<uint64_t>(index) < titles_.count() ? titles_[static_cast<uint64_t>(index)].view() : StringView(); }
  Control* section_body(int32_t index) const { return index >= 0 && static_cast<uint64_t>(index) < bodies_.count() ? bodies_[static_cast<uint64_t>(index)] : nullptr; }

  bool expanded(int32_t index) const;
  // Opens or closes a section (out of range does nothing). Opening one in exclusive mode closes the others. The handler fires when anything changed.
  void set_expanded(int32_t index, bool expanded);
  void toggle(int32_t index) { set_expanded(index, !expanded(index)); }
  void expand_all();    // only with exclusive off
  void collapse_all();
  int32_t expanded_count() const;
  bool exclusive() const { return exclusive_; }
  // Turning it on closes all but the first open section.
  void set_exclusive(bool exclusive);
  // The section whose state changed last (-1 before any change); for the handler.
  int32_t last_changed() const { return last_changed_; }
  void on_expansion_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }

  // The header with the focus (the one Enter and Space toggle), -1 if there are no sections.
  int32_t focused_section() const { return focus_; }

  // Geometry (for painting and tests), in this control's coordinates; empty rectangles for an index out of range.
  Rect2D header_rect(int32_t index) const;
  Rect2D body_rect(int32_t index) const;   // where the body is while the section is open (empty while it is closed)
  int32_t header_at(int32_t x, int32_t y) const;
  // The height every section needs as things stand (headers and open bodies).
  int32_t content_height() const;

  bool focusable() const override { return true; }
  void layout() override;
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {240, 200}; }

 private:
  void apply_open(int32_t index, bool expanded);
  void move_focus(int32_t index);

  List<String> titles_;
  List<Control*> bodies_;
  List<int32_t> heights_;
  List<uint8_t> open_;
  bool exclusive_ = true;
  int32_t focus_ = -1;
  int32_t last_changed_ = -1;
  Handler changed_;
};

}  // namespace gui
