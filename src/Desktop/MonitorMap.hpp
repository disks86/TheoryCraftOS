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

// A picture of how the monitors are arranged: every monitor is a rectangle in proportion to its place and size on the desktop, numbered and named,
// with a dark bar along the bottom of the primary one (that is where the taskbar is). Click a rectangle to select it; drag it to another place
// and let go: the handler is told where its top left would be on the desktop (the layout then puts it next to the others). Left and Right choose the
// previous and the next monitor. The picture is scaled to fit and leaves room around the monitors to drop one beside the others.
#pragma once
#include "Gui/Control.hpp"
#include "Hardware/Monitor/MonitorTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace desktop {

class MonitorMap : public gui::Control {
 public:
  static constexpr int32_t kMargin = 8;
  static constexpr uint32_t kNone = ~0u;

  explicit MonitorMap(allocators::IAllocator& allocator) : Control(allocator), boxes_(allocator) {}

  void clear();
  // Adds a monitor (the id is the program's number for it) with its rectangle on the desktop. False if memory ran out.
  bool add_box(uint32_t id, const hardware::MonitorRect& rect, StringView name, bool primary);
  uint32_t box_count() const { return static_cast<uint32_t>(boxes_.count()); }
  uint32_t box_id(uint32_t index) const { return boxes_[index].id; }

  // The selected monitor's id (kNone: none). select() fires the selection handler when it changes.
  uint32_t selected() const { return selected_; }
  void select(uint32_t id);
  void on_selection_changed(gui::Handler handler) { gui::UiGuard lock(this); selection_changed_ = handler; }
  // Fired when a dragged monitor is dropped: moved_id() and where its top left is wanted on the desktop.
  void on_moved(gui::Handler handler) { gui::UiGuard lock(this); moved_ = handler; }
  uint32_t moved_id() const { return moved_id_; }
  int32_t moved_x() const { return moved_x_; }
  int32_t moved_y() const { return moved_y_; }

  bool dragging() const { return dragging_ != kNone; }
  // Where a monitor's rectangle is drawn (this control's coordinates); false if there is no such monitor.
  bool box_rect(uint32_t id, gui::Rect2D* out) const;

  bool focusable() const override { return true; }
  void paint(gui::Painter& painter) override;
  void on_mouse_down(const gui::MouseArgs& args) override;
  void on_mouse_move(const gui::MouseArgs& args) override;
  void on_mouse_up(const gui::MouseArgs& args) override;
  bool on_key(const gui::KeyArgs& args) override;

 protected:
  gui::Size default_size() const override { return {320, 160}; }

 private:
  struct Box {
    explicit Box(allocators::IAllocator& allocator) : name(allocator) {}
    uint32_t id = 0;
    hardware::MonitorRect rect;
    bool primary = false;
    String name;
  };
  // desktop -> picture: screen = origin + ((desktop - base) * scale) >> 16
  struct View {
    int32_t base_x = 0, base_y = 0, origin_x = 0, origin_y = 0;
    int64_t scale = 65536;
  };
  View compute_view() const;
  gui::Rect2D screen_rect(const View& view, const hardware::MonitorRect& r) const;
  int32_t box_at(int32_t x, int32_t y, const View& view) const;
  int32_t index_of(uint32_t id) const;

  List<Box> boxes_;
  uint32_t selected_ = kNone;
  gui::Handler selection_changed_, moved_;
  uint32_t moved_id_ = kNone;
  int32_t moved_x_ = 0, moved_y_ = 0;
  // A press on a monitor: where, and (once the pointer moved far enough) the drag.
  uint32_t pressed_ = kNone, dragging_ = kNone;
  int32_t press_x_ = 0, press_y_ = 0, grab_dx_ = 0, grab_dy_ = 0, pointer_x_ = 0, pointer_y_ = 0;
  View frozen_;   // the picture as it was when the press began (it must not move under a drag)
};

}  // namespace desktop
