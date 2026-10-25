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

// A pie chart: a disc divided into slices whose angles are in proportion to their values, starting at 12 o'clock and running clockwise, with an
// optional legend (a colour swatch, the label and the share in percent) beside it. A click on a slice, or on its legend row, selects it (the selected
// slice is drawn lighter); a click elsewhere clears the selection. Arrow keys Up/Down move the selection. No floating point is used.
#pragma once
#include "Gui/Control.hpp"
#include "Gui/Controls/StringList.hpp"
#include "ObjectContainer/List.hpp"

namespace gui {

class PieChart : public Control {
 public:
  explicit PieChart(allocators::IAllocator& allocator) : Control(allocator), labels_(allocator), values_(allocator), colors_(allocator) {}

  // Appends a slice and returns its index, or -1 if memory ran out. A colour of 0 takes the next colour of the built-in palette.
  int32_t add_slice(StringView label, uint64_t value, uint32_t color = 0);
  void clear_slices();
  int32_t slice_count() const { return labels_.count(); }
  StringView slice_label(int32_t index) const { return labels_.at(index); }
  uint64_t slice_value(int32_t index) const { const uint64_t* v = values_.get(static_cast<uint64_t>(index)); return v ? *v : 0; }
  uint32_t slice_color(int32_t index) const { const uint32_t* c = colors_.get(static_cast<uint64_t>(index)); return c ? *c : 0; }
  // Out of range indexes are ignored.
  void set_slice_value(int32_t index, uint64_t value);
  // The sum of all values (saturating).
  uint64_t total() const;
  // The share of a slice in tenths of a percent (0..1000); 0 while the total is 0.
  uint32_t permille(int32_t index) const;

  int32_t selected_index() const { return selected_; }  // -1 = none
  void set_selected_index(int32_t index);
  void on_selection_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }
  void set_legend(bool shown) { UiGuard lock(this); legend_ = shown; invalidate(); }
  bool legend() const { return legend_; }

  // Where the angle `turn` (0..65535, a full circle being 65536, clockwise from 12 o'clock) falls: the slice, or -1 if there is none.
  int32_t slice_at_angle(uint32_t turn) const;
  // The slice under a point of this control, or -1 (outside the disc, or no slices with a value).
  int32_t slice_at(int32_t x, int32_t y) const;
  // The disc's bounding square in this control's coordinates (width 0 if there is no room).
  Rect2D pie_rect() const;
  // The legend row at a point, or -1.
  int32_t legend_row_at(int32_t x, int32_t y) const;

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

  static constexpr int32_t kMargin = 4, kRowHeight = 18, kSwatch = 10;

 protected:
  Size default_size() const override { return {240, 140}; }

 private:
  void select(int32_t index);
  int32_t legend_width() const;
  void legend_text(int32_t index, String& out) const;

  StringList labels_;
  List<uint64_t> values_;
  List<uint32_t> colors_;
  int32_t selected_ = -1;
  bool legend_ = true;
  Handler changed_;
};

}  // namespace gui
