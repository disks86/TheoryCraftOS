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

#include "Gui/Controls/TreeView.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

TreeView::TreeView(allocators::IAllocator& allocator)
    : Control(allocator), nodes_(allocator), rows_(allocator), scroll_bar_(allocator, Orientation::Vertical) {
  scroll_bar_.on_value_changed(Handler::immediate(&TreeView::scroll_changed, this));
  scroll_bar_.set_visible(false);
  add(&scroll_bar_);
}

// --- Building ---------------------------------------------------------------------------------------

void TreeView::clear() {
  UiGuard lock(this);
  nodes_.clear();
  first_root_ = last_root_ = -1;
  root_total_ = 0;
  rows_.clear();
  selected_ = -1;
  top_row_ = 0;
  update_scroll_bar();
  invalidate();
}

int32_t TreeView::add_node(int32_t parent, StringView text, uint32_t tag, ItemIcon icon) {
  UiGuard lock(this);
  if (parent != -1 && !valid(parent)) return -1;
  Node node(allocator());
  node.text.assign(text);
  node.parent = parent;
  node.tag = tag;
  node.icon = icon;
  node.depth = parent < 0 ? 0 : nodes_[static_cast<uint64_t>(parent)].depth + 1;
  const int32_t index = static_cast<int32_t>(nodes_.count());
  if (!nodes_.push_back(util::move(node))) return -1;
  // Linked in as the last of its brothers and sisters.
  int32_t& first = parent < 0 ? first_root_ : nodes_[static_cast<uint64_t>(parent)].first_child;
  int32_t& last = parent < 0 ? last_root_ : nodes_[static_cast<uint64_t>(parent)].last_child;
  if (last >= 0) nodes_[static_cast<uint64_t>(last)].next_sibling = index;
  else first = index;
  last = index;
  if (parent < 0) ++root_total_;
  else ++nodes_[static_cast<uint64_t>(parent)].child_total;
  rebuild_rows();
  return index;
}

void TreeView::set_node_text(int32_t node, StringView text) {
  UiGuard lock(this);
  if (!valid(node)) return;
  nodes_[static_cast<uint64_t>(node)].text.assign(text);
  invalidate();
}

uint32_t TreeView::child_count(int32_t node) const {
  if (node == -1) return root_total_;
  return valid(node) ? nodes_[static_cast<uint64_t>(node)].child_total : 0;
}

int32_t TreeView::child_at(int32_t node, uint32_t index) const {
  if (node != -1 && !valid(node)) return -1;
  int32_t child = node == -1 ? first_root_ : nodes_[static_cast<uint64_t>(node)].first_child;
  for (; child >= 0 && index > 0; --index) child = nodes_[static_cast<uint64_t>(child)].next_sibling;
  return child;
}

void TreeView::set_node_menu(int32_t node, bool has_menu) {
  UiGuard lock(this);
  if (valid(node)) nodes_[static_cast<uint64_t>(node)].menu = has_menu;
}

// --- Rows: the nodes that are shown --------------------------------------------------------------------

void TreeView::add_rows(int32_t node) {
  rows_.push_back(node);
  const Node& n = nodes_[static_cast<uint64_t>(node)];
  if (!n.expanded) return;
  for (int32_t child = n.first_child; child >= 0; child = nodes_[static_cast<uint64_t>(child)].next_sibling) add_rows(child);
}

void TreeView::rebuild_rows() {
  rows_.clear();
  for (int32_t root = first_root_; root >= 0; root = nodes_[static_cast<uint64_t>(root)].next_sibling) add_rows(root);
  // A selected node that is no longer shown (a parent was closed) hands the selection to its nearest shown ancestor.
  if (selected_ >= 0 && row_of(selected_) < 0) {
    int32_t up = nodes_[static_cast<uint64_t>(selected_)].parent;
    while (up >= 0 && row_of(up) < 0) up = nodes_[static_cast<uint64_t>(up)].parent;
    set_selected(up);
  }
  update_scroll_bar();
  invalidate();
}

int32_t TreeView::row_of(int32_t node) const {
  for (uint64_t i = 0; i < rows_.count(); ++i)
    if (rows_[i] == node) return static_cast<int32_t>(i);
  return -1;
}

void TreeView::set_expanded(int32_t node, bool expanded) {
  UiGuard lock(this);
  if (!valid(node) || nodes_[static_cast<uint64_t>(node)].expanded == expanded) return;
  nodes_[static_cast<uint64_t>(node)].expanded = expanded;
  rebuild_rows();
}

void TreeView::expand_all() {
  UiGuard lock(this);
  for (uint64_t i = 0; i < nodes_.count(); ++i) nodes_[i].expanded = true;
  rebuild_rows();
}

// --- Selection and scrolling ---------------------------------------------------------------------------

void TreeView::set_selected(int32_t node) {
  if (node == selected_) return;
  selected_ = node;
  invalidate();
  fire(selection_changed_);
}

void TreeView::select(int32_t node) {
  UiGuard lock(this);
  if (node != -1 && !valid(node)) return;
  if (node >= 0) {   // (the parents are opened so that it is shown)
    bool changed = false;
    for (int32_t up = nodes_[static_cast<uint64_t>(node)].parent; up >= 0; up = nodes_[static_cast<uint64_t>(up)].parent)
      if (!nodes_[static_cast<uint64_t>(up)].expanded) {
        nodes_[static_cast<uint64_t>(up)].expanded = true;
        changed = true;
      }
    if (changed) rebuild_rows();
    ensure_visible(node);
  }
  set_selected(node);
}

int32_t TreeView::page_rows() const {
  const int32_t rows = (static_cast<int32_t>(bounds_.height) - 2 * kBorder) / kRowHeight;
  return rows > 0 ? rows : 1;
}

void TreeView::update_scroll_bar() {
  const int32_t total = static_cast<int32_t>(rows_.count()), page = page_rows();
  const int32_t max_top = total > page ? total - page : 0;
  if (top_row_ > max_top) top_row_ = max_top;
  syncing_ = true;
  scroll_bar_.set_metrics(total, page);
  scroll_bar_.set_value(top_row_);
  syncing_ = false;
  scroll_bar_.set_visible(total > page);
}

void TreeView::ensure_visible(int32_t node) {
  const int32_t row = row_of(node);
  if (row < 0) return;
  const int32_t page = page_rows();
  if (row < top_row_) top_row_ = row;
  else if (row >= top_row_ + page) top_row_ = row + 1 - page;
  update_scroll_bar();
  invalidate();
}

void TreeView::scroll_changed(void* self, Control&) {
  auto* t = static_cast<TreeView*>(self);
  if (t->syncing_) return;
  t->top_row_ = t->scroll_bar_.value();
  t->invalidate();
}

void TreeView::layout() {
  update_scroll_bar();
  const int32_t thick = ScrollBar::kThickness;
  const int32_t h = static_cast<int32_t>(bounds_.height) - 2 * kBorder;
  scroll_bar_.set_bounds({static_cast<int32_t>(bounds_.width) - kBorder - thick, kBorder, static_cast<uint32_t>(thick), static_cast<uint32_t>(h > 0 ? h : 0)});
  Control::layout();
}

// --- Geometry -----------------------------------------------------------------------------------------

bool TreeView::node_rect(int32_t node, Rect2D* out) const {
  const int32_t row = row_of(node);
  if (row < 0 || row < top_row_ || row >= top_row_ + page_rows() + 1) return false;
  const int32_t width = static_cast<int32_t>(bounds_.width) - 2 * kBorder - (scroll_bar_.visible() ? ScrollBar::kThickness : 0);
  *out = {kBorder, kBorder + (row - top_row_) * kRowHeight, static_cast<uint32_t>(width > 0 ? width : 0), static_cast<uint32_t>(kRowHeight)};
  return true;
}

bool TreeView::box_rect(int32_t node, Rect2D* out) const {
  Rect2D r;
  if (!node_rect(node, &r) || nodes_[static_cast<uint64_t>(node)].child_total == 0) return false;
  const int32_t x = r.x + 4 + nodes_[static_cast<uint64_t>(node)].depth * kIndent;
  *out = {x, r.y + (kRowHeight - kBox) / 2, static_cast<uint32_t>(kBox), static_cast<uint32_t>(kBox)};
  return true;
}

int32_t TreeView::node_at(int32_t x, int32_t y) const {
  if (x < kBorder || y < kBorder) return -1;
  const int32_t row = top_row_ + (y - kBorder) / kRowHeight;
  if (row < 0 || static_cast<uint64_t>(row) >= rows_.count()) return -1;
  if (x >= static_cast<int32_t>(bounds_.width) - kBorder - (scroll_bar_.visible() ? ScrollBar::kThickness : 0)) return -1;
  return rows_[static_cast<uint64_t>(row)];
}

// --- Painting -----------------------------------------------------------------------------------------

void TreeView::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.field);
  painter.bevel(all, true);
  const Rect2D body{kBorder, kBorder, bounds_.width > 2 * kBorder ? bounds_.width - 2 * kBorder : 0, bounds_.height > 2 * kBorder ? bounds_.height - 2 * kBorder : 0};
  painter.push_frame(body);
  const int32_t rows = page_rows() + 1;
  for (int32_t r = 0; r < rows && static_cast<uint64_t>(top_row_ + r) < rows_.count(); ++r) {
    const int32_t node = rows_[static_cast<uint64_t>(top_row_ + r)];
    const Node& n = nodes_[static_cast<uint64_t>(node)];
    const int32_t y = r * kRowHeight;
    const int32_t indent = 4 + n.depth * kIndent;
    if (n.child_total) {   // the box
      const Rect2D box{indent, y + (kRowHeight - kBox) / 2, static_cast<uint32_t>(kBox), static_cast<uint32_t>(kBox)};
      painter.fill(box, t.field);
      painter.outline(box, t.shadow);
      painter.fill({box.x + 2, box.y + kBox / 2, static_cast<uint32_t>(kBox - 4), 1}, t.text);
      if (!n.expanded) painter.fill({box.x + kBox / 2, box.y + 2, 1, static_cast<uint32_t>(kBox - 4)}, t.text);
    }
    const int32_t icon_x = indent + kBox + 4;
    const int32_t text_x = icon_x + kIconSize + 4;
    const bool selected = node == selected_;
    const int32_t text_w = Painter::text_width(n.text.view());
    if (selected) painter.fill({icon_x - 1, y, static_cast<uint32_t>(text_x - icon_x + text_w + 4), static_cast<uint32_t>(kRowHeight)}, focused() ? t.selection : t.shadow);
    draw_item_icon(painter, n.icon, icon_x, y + (kRowHeight - kIconSize) / 2, kIconSize);
    painter.text(text_x, y + (kRowHeight - Painter::kGlyphHeight) / 2, n.text.view(), selected ? t.selection_text : t.text);
  }
  painter.pop_frame();
}

// --- Mouse and keys ---------------------------------------------------------------------------------

void TreeView::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  const int32_t node = node_at(args.x, args.y);
  if (node < 0) return;
  Rect2D box;
  if (box_rect(node, &box) && args.x >= box.x - 2 && args.x < box.x + static_cast<int32_t>(box.width) + 2) {
    set_expanded(node, !nodes_[static_cast<uint64_t>(node)].expanded);
    return;
  }
  select(node);
}

void TreeView::on_mouse_wheel(const MouseArgs& args) {
  UiGuard lock(this);
  const int32_t total = static_cast<int32_t>(rows_.count()), page = page_rows();
  int32_t top = top_row_ + (args.wheel > 0 ? -3 : args.wheel < 0 ? 3 : 0);
  if (top > total - page) top = total - page;
  if (top < 0) top = 0;
  top_row_ = top;
  update_scroll_bar();
  invalidate();
}

bool TreeView::accepts_context_menu(int32_t x, int32_t y) const {
  const int32_t node = x < 0 ? selected_ : node_at(x, y);   // (from the keyboard: the selected node)
  return valid(node) && nodes_[static_cast<uint64_t>(node)].menu;
}

void TreeView::prepare_context_menu(int32_t x, int32_t y) {
  if (x < 0) return;
  const int32_t node = node_at(x, y);
  if (node >= 0) select(node);
}

bool TreeView::on_key(const KeyArgs& args) {
  using namespace hardware;
  UiGuard lock(this);
  const int32_t n = static_cast<int32_t>(rows_.count());
  const int32_t row = selected_ >= 0 ? row_of(selected_) : -1;
  auto go = [&](int32_t target) {
    if (n == 0) return;
    target = target < 0 ? 0 : target >= n ? n - 1 : target;
    select(rows_[static_cast<uint64_t>(target)]);
  };
  switch (args.usage) {
    case key::kDown: go(row < 0 ? 0 : row + 1); return true;
    case key::kUp: go(row < 0 ? 0 : row - 1); return true;
    case 0x4A: go(0); return true;       // Home
    case 0x4D: go(n - 1); return true;   // End
    case 0x4E: go((row < 0 ? 0 : row) + page_rows() - 1); return true;   // Page Down
    case 0x4B: go((row < 0 ? 0 : row) - page_rows() + 1); return true;   // Page Up
    case key::kRight:
      if (selected_ >= 0 && nodes_[static_cast<uint64_t>(selected_)].child_total) {
        if (!nodes_[static_cast<uint64_t>(selected_)].expanded) set_expanded(selected_, true);
        else select(nodes_[static_cast<uint64_t>(selected_)].first_child);
      }
      return true;
    case key::kLeft:
      if (selected_ >= 0) {
        const Node& node = nodes_[static_cast<uint64_t>(selected_)];
        if (node.child_total && node.expanded) set_expanded(selected_, false);
        else if (node.parent >= 0) select(node.parent);
      }
      return true;
    case key::kEnter:
    case key::kSpace:
      if (selected_ >= 0 && nodes_[static_cast<uint64_t>(selected_)].child_total) set_expanded(selected_, !nodes_[static_cast<uint64_t>(selected_)].expanded);
      return true;
    default:
      return false;
  }
}

}  // namespace gui
