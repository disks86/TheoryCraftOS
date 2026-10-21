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

// Shows a browser::HtmlDocument: the page laid out in the width of the control, on a white background, with headings and bold text drawn heavy,
// links blue and underlined (red while the pointer is over one), code on a shaded background and rules as lines. The mouse wheel, the scroll bar,
// the arrow keys, Page Up / Page Down, Home / End and Space scroll; a click on a link fires the link handler (clicked_link() is where it leads, as
// written in the page). The view owns the document it shows; set_document() hands it a new one (and returns to the top).
#pragma once
#include "Browser/HtmlDocument.hpp"
#include "Browser/PageLayout.hpp"
#include "Gui/Control.hpp"
#include "Gui/Controls/ScrollBar.hpp"
#include "Utility/UniquePointer.hpp"

namespace gui {

class PageView : public Control {
 public:
  static constexpr int32_t kMargin = 8, kLineHeight = 16;

  explicit PageView(allocators::IAllocator& allocator);

  // Shows `document` (nullptr: a blank page). The view goes back to the top unless `keep_scroll` (the same page drawn again).
  void set_document(UniquePointer<browser::HtmlDocument> document, bool keep_scroll = false);
  const browser::HtmlDocument* document() const { return document_.get(); }

  // --- Links
  void on_link_clicked(Handler handler) { UiGuard lock(this); link_handler_ = handler; }
  StringView clicked_link() const { return clicked_link_.view(); }
  // Fires when the pointer moves on to or off a link; hovered_link() is where it leads ("" off a link).
  void on_hover_changed(Handler handler) { UiGuard lock(this); hover_handler_ = handler; }
  StringView hovered_link() const { return hovered_link_.view(); }
  // Where the link at a point of this control leads (false if there is none there).
  bool link_at(int32_t x, int32_t y, String* target) const;

  // --- Clicks on the page itself
  // Fires when the pointer is pressed and released on the same piece of text that came from a node of the page (Run::node); clicked_node() is that node's
  // serial and clicked_x() / clicked_y() are where, in the page (columns * 8, lines * 16). With such a handler, links are followed by whoever handles it.
  void on_node_clicked(Handler handler) { UiGuard lock(this); node_handler_ = handler; }
  uint32_t clicked_node() const { return clicked_node_; }
  int32_t clicked_x() const { return clicked_x_; }
  int32_t clicked_y() const { return clicked_y_; }
  // Fires when the number of columns or rows that fit changes (the window was resized), also the first time it is known.
  void on_resized(Handler handler) { UiGuard lock(this); resize_handler_ = handler; }

  // --- View
  uint32_t line_count() const { return layout_.line_count(); }
  uint32_t visible_lines() const { return rows_; }
  uint32_t columns() const { return layout_.columns(); }
  uint32_t scroll_line() const { return top_; }
  void scroll_to_line(uint32_t line);
  void scroll_by(int32_t lines);

  bool focusable() const override { return true; }
  void layout() override;
  void paint(Painter& painter) override;
  void on_mouse_wheel(const MouseArgs& args) override { scroll_by(args.wheel > 0 ? -3 : args.wheel < 0 ? 3 : 0); }
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  void on_mouse_leave() override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {320, 200}; }

 private:
  void measure();       // columns_, rows_ and the scroll bar from the size, and the layout when it is stale
  void sync_scroll_bar();
  bool locate(int32_t x, int32_t y, uint32_t* line, uint32_t* column) const;
  int32_t link_index_at(int32_t x, int32_t y) const;
  uint32_t node_at(int32_t x, int32_t y) const;
  void set_hover(int32_t link);
  static void scroll_changed(void* self, Control&);

  UniquePointer<browser::HtmlDocument> document_;
  browser::PageLayout layout_;
  ScrollBar scroll_bar_;
  uint32_t rows_ = 1, top_ = 0;
  bool stale_ = true, syncing_ = false;
  int32_t hover_ = -1, press_link_ = -1;
  String clicked_link_, hovered_link_;
  Handler link_handler_, hover_handler_, node_handler_, resize_handler_;
  uint32_t press_node_ = 0, clicked_node_ = 0, notified_columns_ = 0, notified_rows_ = 0;
  int32_t clicked_x_ = 0, clicked_y_ = 0;
};

}  // namespace gui
