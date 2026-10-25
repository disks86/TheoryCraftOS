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

// A word cloud: words set in lines that are centred in the control, each at a size that follows its weight (the font is 8x16, drawn at 1x to 4x
// by whole pixels: the lightest word is 1x, the heaviest 4x, the rest in between in proportion; if all weights are equal every word is 2x). Words
// that do not fit on a line go to the next; lines beyond the bottom are cut off. A word without a colour of its own takes the next colour of the
// built-in palette. A click on a word selects it (drawn with the selection colours); Left/Right move the selection. No floating point is used.
#pragma once
#include "Gui/Control.hpp"
#include "Gui/Controls/StringList.hpp"
#include "ObjectContainer/List.hpp"

namespace gui {

class WordCloud : public Control {
 public:
  explicit WordCloud(allocators::IAllocator& allocator)
      : Control(allocator), words_(allocator), weights_(allocator), colors_(allocator), rects_(allocator) {}

  // Appends a word and returns its index, or -1 if memory ran out. A colour of 0 takes the next colour of the built-in palette.
  int32_t add_word(StringView text, uint64_t weight, uint32_t color = 0);
  void clear_words();
  int32_t word_count() const { return words_.count(); }
  StringView word_text(int32_t index) const { return words_.at(index); }
  uint64_t word_weight(int32_t index) const { const uint64_t* w = index >= 0 ? weights_.get(static_cast<uint64_t>(index)) : nullptr; return w ? *w : 0; }
  uint32_t word_color(int32_t index) const { const uint32_t* c = index >= 0 ? colors_.get(static_cast<uint64_t>(index)) : nullptr; return c ? *c : 0; }
  // Out of range indexes are ignored.
  void set_word_weight(int32_t index, uint64_t weight);

  // The size factor (1..kMaxScale) of a word.
  int32_t word_scale(int32_t index) const;
  // Where a word is set, in this control's coordinates; an empty rectangle if it is out of range or the layout failed (out of memory).
  Rect2D word_rect(int32_t index) const;
  // The word at a point of this control, or -1.
  int32_t word_at(int32_t x, int32_t y) const;

  int32_t selected_index() const { return selected_; }  // -1 = none
  void set_selected_index(int32_t index);
  void on_selection_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

  static constexpr int32_t kMargin = 6, kMaxScale = 4, kWordGap = 10, kLineGap = 2;

 private:
  void select(int32_t index);
  void ensure_layout() const;
  void touch() { UiGuard lock(this); layout_valid_ = false; invalidate(); }

  StringList words_;
  List<uint64_t> weights_;
  List<uint32_t> colors_;
  mutable List<Rect2D> rects_;  // the layout, rebuilt when the words or the width change
  mutable bool layout_valid_ = false;
  mutable uint32_t laid_width_ = 0;
  int32_t selected_ = -1;
  Handler changed_;
};

}  // namespace gui
