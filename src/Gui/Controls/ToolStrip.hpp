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

// A palette of tools: square buttons with a drawn icon, laid out in a grid of `columns` buttons per row, of which one
// is selected (it looks pressed). Click a tool or use the arrow keys while the strip has the focus. The strip also
// reports which tool the pointer is over, so the program can show its name (on_hover_changed, hovered_tool()).
// An icon is a function that draws into the button's area with the ink color it is given.
#pragma once
#include "Gui/Control.hpp"
#include "Utility/String.hpp"

namespace gui {

using ToolIcon = void (*)(Painter& painter, const Rect2D& area, uint32_t ink);

class ToolStrip : public Control {
 public:
  static constexpr int32_t kButton = 28, kPad = 3;

  explicit ToolStrip(allocators::IAllocator& allocator) : Control(allocator), tools_(allocator) {}

  // Adds a tool after the others; the first one added is selected. Returns its index or -1.
  int32_t add_tool(StringView name, ToolIcon icon);
  uint32_t tool_count() const { return static_cast<uint32_t>(tools_.count()); }
  StringView tool_name(uint32_t index) const { return index < tools_.count() ? tools_[index].name.view() : StringView(); }
  void set_columns(uint32_t columns);
  uint32_t columns() const { return columns_; }

  int32_t selected_tool() const { return selected_; }  // -1 while there are no tools
  // The handler fires when the selection changes.
  void select_tool(int32_t index);
  void on_selection_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }
  // -1 when the pointer is not over a tool.
  int32_t hovered_tool() const { return hover_; }
  void on_hover_changed(Handler handler) { UiGuard lock(this); hover_changed_ = handler; }

  // Where a tool's button is, and which one is at a point (-1: none), in this control's coordinates.
  Rect2D tool_rect(uint32_t index) const;
  int32_t tool_at(int32_t x, int32_t y) const;
  // The size that holds all the tools.
  Size content_size() const;

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_leave() override;
  void on_mouse_down(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return content_size(); }

 private:
  struct Tool {
    String name;
    ToolIcon icon;
  };
  void set_hover(int32_t index);

  List<Tool> tools_;
  uint32_t columns_ = 1;
  int32_t selected_ = -1, hover_ = -1;
  Handler changed_, hover_changed_;
};

}  // namespace gui
