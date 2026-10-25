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

// A bar chart: one vertical bar per value, side by side, with a value axis on the left (gridlines at every quarter, labelled 0 and the top of the
// scale) and the label of each bar underneath, cut off to the width of its bar. The scale tops out at the largest value unless set_maximum() fixes
// it (a bar above a fixed maximum is drawn full height). A click on a bar's column selects it (drawn lighter); Left/Right move the selection.
// Values are not negative. No floating point is used.
#pragma once
#include "Gui/Control.hpp"
#include "Gui/Controls/StringList.hpp"
#include "ObjectContainer/List.hpp"

namespace gui {

class BarChart : public Control {
 public:
  explicit BarChart(allocators::IAllocator& allocator) : Control(allocator), labels_(allocator), values_(allocator), colors_(allocator) {}

  // Appends a bar and returns its index, or -1 if memory ran out. A colour of 0 takes the next colour of the built-in palette.
  int32_t add_bar(StringView label, uint64_t value, uint32_t color = 0);
  void clear_bars();
  int32_t bar_count() const { return labels_.count(); }
  StringView bar_label(int32_t index) const { return labels_.at(index); }
  uint64_t bar_value(int32_t index) const { const uint64_t* v = index >= 0 ? values_.get(static_cast<uint64_t>(index)) : nullptr; return v ? *v : 0; }
  uint32_t bar_color(int32_t index) const { const uint32_t* c = index >= 0 ? colors_.get(static_cast<uint64_t>(index)) : nullptr; return c ? *c : 0; }
  // Out of range indexes are ignored.
  void set_bar_value(int32_t index, uint64_t value);
  // The top of the scale: 0 (the default) means the largest value.
  void set_maximum(uint64_t maximum) { UiGuard lock(this); maximum_ = maximum; invalidate(); }
  uint64_t maximum() const { return maximum_; }
  // The top of the scale in use (never 0).
  uint64_t scale_top() const;

  int32_t selected_index() const { return selected_; }  // -1 = none
  void set_selected_index(int32_t index);
  void on_selection_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }

  // The plot area (inside the axes), in this control's coordinates; width 0 if there is no room.
  Rect2D plot_rect() const;
  // The bar column (full height of the plot) of bar `index`; the bar is drawn in the lower part of it.
  Rect2D column_rect(int32_t index) const;
  // The height in pixels of the bar for a value (0 for a value of 0, at least 1 for any other).
  uint32_t bar_height(uint64_t value) const;
  // The bar whose column (or label) is at a point of this control, or -1.
  int32_t bar_at(int32_t x, int32_t y) const;

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

  static constexpr int32_t kMargin = 4, kLabelHeight = 18, kBarGap = 4;

 protected:
  Size default_size() const override { return {260, 160}; }

 private:
  void select(int32_t index);
  int32_t axis_width() const;

  StringList labels_;
  List<uint64_t> values_;
  List<uint32_t> colors_;
  uint64_t maximum_ = 0;
  int32_t selected_ = -1;
  Handler changed_;
};

}  // namespace gui
