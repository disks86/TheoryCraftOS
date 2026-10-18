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

// A tree of items: nodes with text, an icon and a number the program can put in them (the tag), nested to any depth. A node with children has a
// small box before it that shows "+" when it is closed and "-" when it is open; clicking the box (or Right / Left, Enter or Space on the selected node)
// opens and closes it. One node is selected: click it, or use Up / Down (through the nodes that are shown), Left (close it, or go to its parent),
// Right (open it, or go to its first child), Home, End, Page Up and Page Down. The mouse wheel scrolls, and a scroll bar shows when the tree is
// taller than the control. A right click selects the node under the pointer and opens the control's context menu - but only for nodes marked with
// set_node_menu(): other nodes have no menu, so one tree can carry different commands for different places.
#pragma once
#include "Gui/Control.hpp"
#include "Gui/Controls/ListView.hpp"
#include "Gui/Controls/ScrollBar.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace gui {

class TreeView : public Control {
 public:
  static constexpr int32_t kBorder = 2, kRowHeight = 18, kIndent = 18, kBox = 9, kIconSize = 16;

  explicit TreeView(allocators::IAllocator& allocator);

  // --- Building the tree. Nodes are numbered from 0 in the order they were added; a node keeps its number until clear().
  void clear();
  // Adds a node as the last child of `parent` (-1: at the top). Returns its number, or -1 if `parent` does not exist or memory ran out.
  int32_t add_node(int32_t parent, StringView text, uint32_t tag = 0, ItemIcon icon = ItemIcon::File);
  uint32_t node_count() const { return static_cast<uint32_t>(nodes_.count()); }
  StringView node_text(int32_t node) const { return valid(node) ? nodes_[static_cast<uint64_t>(node)].text.view() : StringView(); }
  void set_node_text(int32_t node, StringView text);
  uint32_t node_tag(int32_t node) const { return valid(node) ? nodes_[static_cast<uint64_t>(node)].tag : 0; }
  int32_t parent_of(int32_t node) const { return valid(node) ? nodes_[static_cast<uint64_t>(node)].parent : -1; }
  // The number of children of a node (-1: of the top level) and the child at `index`.
  uint32_t child_count(int32_t node) const;
  int32_t child_at(int32_t node, uint32_t index) const;

  // --- Opening and closing
  bool expanded(int32_t node) const { return valid(node) && nodes_[static_cast<uint64_t>(node)].expanded; }
  void set_expanded(int32_t node, bool expanded);
  void expand_all();

  // --- Selection (-1: none). select() opens the parents of the node and scrolls to it; the handler fires when the selection changes.
  int32_t selected() const { return selected_; }
  void select(int32_t node);
  void on_selection_changed(Handler handler) { UiGuard lock(this); selection_changed_ = handler; }

  // --- Context menu
  void set_node_menu(int32_t node, bool has_menu);
  bool accepts_context_menu(int32_t x, int32_t y) const override;
  void prepare_context_menu(int32_t x, int32_t y) override;

  // --- Geometry (for painting and tests): the nodes that are shown, from the top.
  uint32_t visible_count() const { return static_cast<uint32_t>(rows_.count()); }
  int32_t node_at_row(uint32_t row) const { return row < rows_.count() ? rows_[row] : -1; }
  // Where a node is (this control's coordinates); false if it is not shown (inside a closed node, or scrolled out) or does not exist.
  bool node_rect(int32_t node, Rect2D* out) const;
  // The box that opens and closes a node (false: it has no children or is not shown).
  bool box_rect(int32_t node, Rect2D* out) const;
  int32_t node_at(int32_t x, int32_t y) const;   // -1 for none

  bool focusable() const override { return true; }
  void layout() override;
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_wheel(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 private:
  struct Node {
    explicit Node(allocators::IAllocator& allocator) : text(allocator) {}
    String text;
    int32_t parent = -1;
    uint32_t tag = 0;
    bool expanded = false, menu = false;
    ItemIcon icon = ItemIcon::File;
    int32_t depth = 0;
    int32_t first_child = -1, last_child = -1, next_sibling = -1;   // the children are a chain
    uint32_t child_total = 0;
  };

  bool valid(int32_t node) const { return node >= 0 && static_cast<uint64_t>(node) < nodes_.count(); }
  void rebuild_rows();
  void add_rows(int32_t node);
  int32_t row_of(int32_t node) const;
  int32_t page_rows() const;
  void update_scroll_bar();
  void ensure_visible(int32_t node);
  void set_selected(int32_t node);
  static void scroll_changed(void* self, Control&);

  List<Node> nodes_;
  int32_t first_root_ = -1, last_root_ = -1;
  uint32_t root_total_ = 0;
  List<int32_t> rows_;     // the nodes shown, top to bottom
  int32_t selected_ = -1;
  int32_t top_row_ = 0;
  bool syncing_ = false;
  ScrollBar scroll_bar_;
  Handler selection_changed_;
};

}  // namespace gui
