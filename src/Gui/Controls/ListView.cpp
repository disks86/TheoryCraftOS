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

#include "Gui/Controls/ListView.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Kernel/Clipboard.hpp"

namespace gui {

namespace {
constexpr uint32_t kFolderFill = 0xFFFFD866, kFolderEdge = 0xFF806000, kPageEdge = 0xFF404040, kPageLine = 0xFF8080A0;
constexpr uint32_t kDriveFill = 0xFFD0D0D0, kLed = 0xFF00C000;
}  // namespace

void draw_item_icon(Painter& painter, ItemIcon icon, int32_t x, int32_t y, int32_t size) {
  const int32_t s = size >= 16 ? size / 16 : 1;
  auto box = [&](int32_t bx, int32_t by, int32_t bw, int32_t bh) {
    return Rect2D{x + bx * s, y + by * s, static_cast<uint32_t>(bw * s), static_cast<uint32_t>(bh * s)};
  };
  switch (icon) {
    case ItemIcon::Folder:
      painter.fill(box(1, 3, 6, 3), kFolderFill);
      painter.outline(box(1, 3, 6, 3), kFolderEdge);
      painter.fill(box(1, 5, 14, 9), kFolderFill);
      painter.outline(box(1, 5, 14, 9), kFolderEdge);
      painter.line(x + 2 * s, y + 6 * s, x + 13 * s, y + 6 * s, painter.theme().highlight);
      break;
    case ItemIcon::Drive:
      painter.fill(box(1, 5, 14, 7), kDriveFill);
      painter.outline(box(1, 5, 14, 7), kPageEdge);
      painter.line(x + 2 * s, y + 7 * s, x + 13 * s, y + 7 * s, painter.theme().shadow);
      painter.fill(box(11, 9, 2, 1), kLed);
      break;
    default:
      painter.fill(box(3, 1, 10, 14), painter.theme().highlight);
      painter.outline(box(3, 1, 10, 14), kPageEdge);
      painter.fill(box(9, 1, 4, 4), painter.theme().face);
      painter.outline(box(9, 1, 4, 4), kPageEdge);
      for (int32_t row = 0; row < 4; ++row) painter.line(x + 5 * s, y + (7 + 2 * row) * s, x + 10 * s, y + (7 + 2 * row) * s, kPageLine);
      break;
  }
}

ListView::ListView(allocators::IAllocator& allocator)
    : Control(allocator),
      model_(allocator),
      icons_(allocator),
      columns_(allocator),
      vscroll_(allocator, Orientation::Vertical),
      hscroll_(allocator, Orientation::Horizontal),
      text_(allocator) {
  vscroll_.on_value_changed(Handler::immediate(&ListView::vertical_changed, this));
  hscroll_.on_value_changed(Handler::immediate(&ListView::horizontal_changed, this));
  vscroll_.set_step(kRowHeight);
  hscroll_.set_step(16);
  add(&vscroll_);
  add(&hscroll_);
  vscroll_.set_visible(false);
  hscroll_.set_visible(false);
}

void ListView::vertical_changed(void* self, Control&) { static_cast<ListView*>(self)->on_scroll_changed(); }
void ListView::horizontal_changed(void* self, Control&) { static_cast<ListView*>(self)->on_scroll_changed(); }

void ListView::on_scroll_changed() {
  if (syncing_) return;
  scroll_x_ = hscroll_.value();
  scroll_y_ = vscroll_.value();
  invalidate();
}

// --- Content ---------------------------------------------------------------------

void ListView::set_view_mode(ViewMode mode) {
  UiGuard lock(this);
  if (mode == mode_) return;
  mode_ = mode;
  scroll_x_ = scroll_y_ = 0;
  update_scroll_bars();
  if (selected_ >= 0) ensure_visible(static_cast<uint32_t>(selected_));
  invalidate();
}

int32_t ListView::add_column(StringView title, int32_t width, Align align) {
  UiGuard lock(this);
  if (model_.row_count() > 0) return -1;
  Column column{String(allocator()), width < kMinColumnWidth ? kMinColumnWidth : width, align};
  if (!column.title.assign(title) || !columns_.push_back(util::move(column))) return -1;
  model_.set_column_count(static_cast<uint32_t>(columns_.count()));
  update_scroll_bars();
  invalidate();
  return static_cast<int32_t>(columns_.count()) - 1;
}

void ListView::set_column_width(uint32_t column, int32_t width) {
  UiGuard lock(this);
  if (column >= columns_.count()) return;
  columns_[column].width = width < kMinColumnWidth ? kMinColumnWidth : width;
  update_scroll_bars();
  invalidate();
}

int32_t ListView::total_column_width() const {
  int32_t sum = 0;
  for (const Column& c : columns_) sum += c.width;
  return sum;
}

int32_t ListView::add_item(const StringView* cells, uint32_t count, ItemIcon icon) {
  UiGuard lock(this);
  if (columns_.count() == 0) return -1;
  const int32_t row = model_.add_row(cells, count);
  if (row < 0) return -1;
  if (!icons_.push_back(static_cast<uint8_t>(icon))) {
    model_.remove_row(static_cast<uint32_t>(row));
    return -1;
  }
  update_scroll_bars();
  invalidate();
  return row;
}

void ListView::clear_items() {
  UiGuard lock(this);
  model_.clear();
  icons_.clear();
  scroll_x_ = scroll_y_ = 0;
  last_click_item_ = -1;
  const bool had_selection = selected_ >= 0;
  selected_ = -1;
  update_scroll_bars();
  invalidate();
  if (had_selection) fire(selection_changed_);
}

// The widest name in the list view, with its icon, within limits.
int32_t ListView::list_cell_width() const {
  int32_t widest = 0;
  String name(allocator());
  for (uint32_t i = 0; i < model_.row_count(); ++i) {
    model_.cell_text(i, 0, &name);
    const int32_t w = Painter::text_width(name.view());
    if (w > widest) widest = w;
  }
  const int32_t cell = kSmallIcon + 12 + widest;
  return cell < kMinListColumnWidth ? kMinListColumnWidth : cell > kMaxListColumnWidth ? kMaxListColumnWidth : cell;
}

ListView::Metrics ListView::measure(int32_t vw, int32_t vh) const {
  Metrics m;
  const int32_t n = static_cast<int32_t>(model_.row_count());
  if (vw < 1) vw = 1;
  if (vh < 1) vh = 1;
  switch (mode_) {
    case ViewMode::Details:
      m.cell_w = total_column_width();
      m.cell_h = kRowHeight;
      m.per_line = 1;
      m.content_w = m.cell_w;
      m.content_h = n * kRowHeight;
      break;
    case ViewMode::List: {
      m.cell_w = list_cell_width();
      m.cell_h = kRowHeight;
      m.per_line = vh / kRowHeight > 0 ? vh / kRowHeight : 1;
      const int32_t columns = (n + m.per_line - 1) / m.per_line;
      m.content_w = columns * m.cell_w;
      m.content_h = (n < m.per_line ? n : m.per_line) * kRowHeight;
      break;
    }
    case ViewMode::Icons:
      m.cell_w = kIconCellWidth;
      m.cell_h = kIconCellHeight;
      m.per_line = vw / kIconCellWidth > 0 ? vw / kIconCellWidth : 1;
      m.content_w = (n < m.per_line ? n : m.per_line) * kIconCellWidth;
      m.content_h = ((n + m.per_line - 1) / m.per_line) * kIconCellHeight;
      break;
  }
  return m;
}

Rect2D ListView::header_rect() const {
  if (mode_ != ViewMode::Details) return {kBorder, kBorder, 0, 0};
  const int32_t w = static_cast<int32_t>(bounds_.width) - 2 * kBorder - (show_v_ ? ScrollBar::kThickness : 0);
  return {kBorder, kBorder, static_cast<uint32_t>(w > 0 ? w : 0), static_cast<uint32_t>(kHeaderHeight)};
}

Rect2D ListView::body_rect() const {
  const int32_t top = kBorder + (mode_ == ViewMode::Details ? kHeaderHeight : 0);
  const int32_t w = static_cast<int32_t>(bounds_.width) - 2 * kBorder - (show_v_ ? ScrollBar::kThickness : 0);
  const int32_t h = static_cast<int32_t>(bounds_.height) - top - kBorder - (show_h_ ? ScrollBar::kThickness : 0);
  return {kBorder, top, static_cast<uint32_t>(w > 0 ? w : 0), static_cast<uint32_t>(h > 0 ? h : 0)};
}

// Works out which scroll bars are needed (they take room from the items, which can in turn need the other
// bar), keeps the scroll position inside the content and tells the bars.
void ListView::update_scroll_bars() {
  const int32_t full_w = static_cast<int32_t>(bounds_.width) - 2 * kBorder;
  const int32_t full_h = static_cast<int32_t>(bounds_.height) - 2 * kBorder - (mode_ == ViewMode::Details ? kHeaderHeight : 0);
  const int32_t thick = ScrollBar::kThickness;
  bool v = false, h = false;
  Metrics m;
  for (int pass = 0; pass < 3; ++pass) {
    m = measure(full_w - (v ? thick : 0), full_h - (h ? thick : 0));
    const bool need_v = m.content_h > full_h - (h ? thick : 0), need_h = m.content_w > full_w - (v ? thick : 0);
    if (need_v == v && need_h == h) break;
    v = need_v;
    h = need_h;
  }
  show_v_ = v;
  show_h_ = h;
  metrics_ = m;
  const int32_t vw = full_w - (v ? thick : 0), vh = full_h - (h ? thick : 0);
  const int32_t max_x = m.content_w > vw ? m.content_w - vw : 0, max_y = m.content_h > vh ? m.content_h - vh : 0;
  if (scroll_x_ > max_x) scroll_x_ = max_x;
  if (scroll_y_ > max_y) scroll_y_ = max_y;
  if (scroll_x_ < 0) scroll_x_ = 0;
  if (scroll_y_ < 0) scroll_y_ = 0;
  syncing_ = true;
  vscroll_.set_metrics(m.content_h, vh);
  vscroll_.set_value(scroll_y_);
  hscroll_.set_metrics(m.content_w, vw);
  hscroll_.set_value(scroll_x_);
  syncing_ = false;
  vscroll_.set_visible(v);
  hscroll_.set_visible(h);
}

void ListView::layout() {
  update_scroll_bars();
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  const int32_t thick = ScrollBar::kThickness;
  const int32_t top = kBorder + (mode_ == ViewMode::Details ? kHeaderHeight : 0);
  const int32_t vh = h - top - kBorder - (show_h_ ? thick : 0);
  const int32_t hw = w - 2 * kBorder - (show_v_ ? thick : 0);
  vscroll_.set_bounds({w - kBorder - thick, top, static_cast<uint32_t>(thick), static_cast<uint32_t>(vh > 0 ? vh : 0)});
  hscroll_.set_bounds({kBorder, h - kBorder - thick, static_cast<uint32_t>(hw > 0 ? hw : 0), static_cast<uint32_t>(thick)});
  Control::layout();
}

// --- Items and selection -----------------------------------------------------------

bool ListView::item_rect(uint32_t item, Rect2D* out) const {
  if (item >= model_.row_count()) return false;
  const Rect2D body = body_rect();
  const int32_t i = static_cast<int32_t>(item), per = metrics_.per_line;
  int32_t x = 0, y = 0;
  switch (mode_) {
    case ViewMode::Details: y = i * metrics_.cell_h; break;
    case ViewMode::List: x = (i / per) * metrics_.cell_w; y = (i % per) * metrics_.cell_h; break;
    case ViewMode::Icons: x = (i % per) * metrics_.cell_w; y = (i / per) * metrics_.cell_h; break;
  }
  uint32_t width = static_cast<uint32_t>(metrics_.cell_w);
  if (mode_ == ViewMode::Details && body.width > width) width = body.width;  // the selection spans the whole row
  *out = {body.x + x - scroll_x_, body.y + y - scroll_y_, width, static_cast<uint32_t>(metrics_.cell_h)};
  return true;
}

int32_t ListView::item_at(int32_t x, int32_t y) const {
  const Rect2D body = body_rect();
  if (!contains(body, x, y)) return -1;
  const int32_t lx = x - body.x + scroll_x_, ly = y - body.y + scroll_y_;
  if (lx < 0 || ly < 0) return -1;
  int32_t index = -1;
  const int32_t per = metrics_.per_line;
  switch (mode_) {
    case ViewMode::Details: index = ly / metrics_.cell_h; break;
    case ViewMode::List: {
      const int32_t row = ly / metrics_.cell_h;
      if (row >= per) return -1;
      index = (lx / metrics_.cell_w) * per + row;
      break;
    }
    case ViewMode::Icons: {
      const int32_t column = lx / metrics_.cell_w;
      if (column >= per) return -1;
      index = (ly / metrics_.cell_h) * per + column;
      break;
    }
  }
  return index >= 0 && index < static_cast<int32_t>(model_.row_count()) ? index : -1;
}

void ListView::ensure_visible(uint32_t item) {
  Rect2D r;
  if (!item_rect(item, &r)) return;
  const Rect2D body = body_rect();
  if (r.x < body.x) scroll_x_ -= body.x - r.x;
  else if (r.x + static_cast<int32_t>(r.width) > body.x + static_cast<int32_t>(body.width) && mode_ != ViewMode::Details)
    scroll_x_ += r.x + static_cast<int32_t>(r.width) - (body.x + static_cast<int32_t>(body.width));
  if (r.y < body.y) scroll_y_ -= body.y - r.y;
  else if (r.y + static_cast<int32_t>(r.height) > body.y + static_cast<int32_t>(body.height))
    scroll_y_ += r.y + static_cast<int32_t>(r.height) - (body.y + static_cast<int32_t>(body.height));
  update_scroll_bars();
  invalidate();
}

void ListView::select_item(int32_t item) {
  UiGuard lock(this);
  if (item < -1 || item >= static_cast<int32_t>(model_.row_count())) return;
  if (item >= 0) ensure_visible(static_cast<uint32_t>(item));
  if (item == selected_) return;
  selected_ = item;
  invalidate();
  fire(selection_changed_);
}

void ListView::move_selection(int32_t delta) {
  const int32_t n = static_cast<int32_t>(model_.row_count());
  if (n == 0) return;
  int32_t target = selected_ < 0 ? (delta > 0 ? 0 : n - 1) : selected_ + delta;
  if (target < 0) target = 0;
  if (target >= n) target = n - 1;
  select_item(target);
}

void ListView::scroll_by(int32_t dx, int32_t dy) {
  scroll_x_ += dx;
  scroll_y_ += dy;
  update_scroll_bars();
  invalidate();
}

// --- Painting ------------------------------------------------------------------------

void ListView::paint_item_text(Painter& painter, const Rect2D& cell, uint32_t item, bool selected, uint32_t color) {
  const Theme& t = painter.theme();
  model_.cell_text(item, 0, &text_);
  if (mode_ == ViewMode::Icons) {
    const int32_t chars = (kIconCellWidth - 4) / Painter::kGlyphWidth;
    String shown(allocator());
    StringView name = text_.view();
    if (static_cast<int32_t>(name.size()) > chars) {
      shown.assign(name.first(static_cast<uint64_t>(chars - 3)));
      shown.append("...");
      name = shown.view();
    }
    const int32_t w = Painter::text_width(name) + 4;
    const Rect2D label{cell.x + (static_cast<int32_t>(cell.width) - w) / 2, cell.y + 4 + kLargeIcon + 4, static_cast<uint32_t>(w), 18};
    if (selected) painter.fill(label, focused() ? t.selection : t.shadow);
    painter.text_centered(label, name, selected ? t.selection_text : color);
    return;
  }
  const int32_t text_x = cell.x + kSmallIcon + 6;
  const int32_t w = Painter::text_width(text_.view()) + 4;
  const Rect2D label{text_x - 2, cell.y, static_cast<uint32_t>(w), static_cast<uint32_t>(kRowHeight)};
  if (selected) painter.fill(label, focused() ? t.selection : t.shadow);
  painter.text(text_x, cell.y + (kRowHeight - Painter::kGlyphHeight) / 2, text_.view(), selected ? t.selection_text : color);
}

void ListView::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.field);
  painter.bevel(all, true);
  const uint32_t color = enabled() ? t.text : t.disabled_text;

  if (mode_ == ViewMode::Details) {
    const Rect2D header = header_rect();
    painter.push_frame(header);
    int32_t x = -scroll_x_;
    for (uint32_t c = 0; c < columns_.count(); ++c) {
      const int32_t w = columns_[c].width;
      if (x + w >= 0 && x <= static_cast<int32_t>(header.width)) {
        const Rect2D cell{x, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(kHeaderHeight)};
        painter.fill(cell, t.face);
        painter.bevel(cell, false);
        painter.push_frame(cell);
        const StringView title = columns_[c].title.view();
        const int32_t room = w - 8, tw = Painter::text_width(title);
        int32_t tx = 4;
        if (columns_[c].align == Align::Right) tx = 4 + (room > tw ? room - tw : 0);
        else if (columns_[c].align == Align::Center) tx = 4 + (room > tw ? (room - tw) / 2 : 0);
        painter.text(tx, (kHeaderHeight - Painter::kGlyphHeight) / 2, title, color);
        painter.pop_frame();
      }
      x += w;
    }
    painter.pop_frame();
  }

  const Rect2D body = body_rect();
  painter.push_frame(body);
  const Rect2D view{0, 0, body.width, body.height};
  for (uint32_t i = 0; i < model_.row_count(); ++i) {
    Rect2D r;
    item_rect(i, &r);
    r.x -= body.x;
    r.y -= body.y;
    const Rect2D visible = intersect(r, view);
    if (visible.width == 0 || visible.height == 0) continue;
    const bool selected = static_cast<int32_t>(i) == selected_;
    const ItemIcon icon = item_icon(i);
    painter.push_frame(r);
    const Rect2D cell{0, 0, r.width, r.height};
    if (mode_ == ViewMode::Icons) {
      draw_item_icon(painter, icon, (static_cast<int32_t>(r.width) - kLargeIcon) / 2, 4, kLargeIcon);
      paint_item_text(painter, cell, i, selected, color);
    } else if (mode_ == ViewMode::List) {
      draw_item_icon(painter, icon, 2, (kRowHeight - kSmallIcon) / 2, kSmallIcon);
      paint_item_text(painter, cell, i, selected, color);
    } else {
      if (selected) painter.fill(cell, focused() ? t.selection : t.shadow);
      int32_t x = 0;
      for (uint32_t c = 0; c < columns_.count(); ++c) {
        const int32_t w = columns_[c].width;
        painter.push_frame({x, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(kRowHeight)});
        if (c == 0) {
          draw_item_icon(painter, icon, 2, (kRowHeight - kSmallIcon) / 2, kSmallIcon);
          model_.cell_text(i, 0, &text_);
          painter.text(kSmallIcon + 6, (kRowHeight - Painter::kGlyphHeight) / 2, text_.view(), selected ? t.selection_text : color);
        } else {
          model_.cell_text(i, c, &text_);
          const int32_t room = w - 8, tw = Painter::text_width(text_.view());
          int32_t tx = 4;
          if (columns_[c].align == Align::Right) tx = 4 + (room > tw ? room - tw : 0);
          else if (columns_[c].align == Align::Center) tx = 4 + (room > tw ? (room - tw) / 2 : 0);
          painter.text(tx, (kRowHeight - Painter::kGlyphHeight) / 2, text_.view(), selected ? t.selection_text : color);
        }
        painter.pop_frame();
        x += w;
      }
    }
    if (selected && focused() && mode_ != ViewMode::Icons) painter.focus_frame(cell);
    painter.pop_frame();
  }
  painter.pop_frame();

  if (show_v_ && show_h_)
    painter.fill({static_cast<int32_t>(bounds_.width) - kBorder - ScrollBar::kThickness, static_cast<int32_t>(bounds_.height) - kBorder - ScrollBar::kThickness,
                  static_cast<uint32_t>(ScrollBar::kThickness), static_cast<uint32_t>(ScrollBar::kThickness)}, t.face);
}

// --- Input -----------------------------------------------------------------------------

int32_t ListView::column_at(int32_t x, bool* on_border) const {
  *on_border = false;
  int32_t left = kBorder - scroll_x_;
  for (uint32_t c = 0; c < columns_.count(); ++c) {
    const int32_t right = left + columns_[c].width;
    if (x >= right - kResizeZone && x < right + kResizeZone) {
      *on_border = true;
      return static_cast<int32_t>(c);
    }
    if (x >= left && x < right) return static_cast<int32_t>(c);
    left = right;
  }
  return -1;
}

void ListView::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  if (mode_ == ViewMode::Details && contains(header_rect(), args.x, args.y)) {
    bool border = false;
    const int32_t column = column_at(args.x, &border);
    if (column >= 0 && border) {
      resizing_ = column;
      resize_origin_x_ = args.x;
      resize_origin_width_ = columns_[column].width;
    }
    return;
  }
  const int32_t item = item_at(args.x, args.y);
  if (item < 0) {
    if (contains(body_rect(), args.x, args.y)) select_item(-1);
    last_click_item_ = -1;
    return;
  }
  select_item(item);
  const uint64_t now = clock_ ? __atomic_load_n(clock_, __ATOMIC_RELAXED) : 0;
  if (clock_ && item == last_click_item_ && now - last_click_us_ <= kDoubleClickUs) {
    last_click_item_ = -1;
    fire(activated_);
  } else {
    last_click_item_ = item;
    last_click_us_ = now;
  }
}

void ListView::on_mouse_move(const MouseArgs& args) {
  if (resizing_ >= 0) set_column_width(static_cast<uint32_t>(resizing_), resize_origin_width_ + args.x - resize_origin_x_);
}

void ListView::on_mouse_up(const MouseArgs&) { resizing_ = -1; }

void ListView::on_mouse_wheel(const MouseArgs& args) {
  const int32_t direction = args.wheel > 0 ? -1 : args.wheel < 0 ? 1 : 0;
  if (mode_ == ViewMode::List) scroll_by(direction * 3 * 16, 0);
  else scroll_by(0, direction * 3 * (mode_ == ViewMode::Icons ? kIconCellHeight / 2 : kRowHeight));
}

bool ListView::copy_selected_item() {
  UiGuard lock(this);
  if (selected_ < 0 || !clipboard()) return false;
  String line(allocator()), cell(allocator());
  for (uint32_t c = 0; c < columns_.count(); ++c) {
    model_.cell_text(static_cast<uint32_t>(selected_), c, &cell);
    if ((c > 0 && !line.push_back('\t')) || !line.append(cell.view())) return false;
  }
  return clipboard()->set_text(line.view());
}

// A right click selects the item under the pointer (and no item on an empty place), as a left click does.
void ListView::prepare_context_menu(int32_t x, int32_t y) {
  if (x < 0 || y < 0) return;   // from the keyboard: the selection stays
  if (contains(header_rect(), x, y)) return;
  select_item(item_at(x, y));
}

bool ListView::on_key(const KeyArgs& args) {
  using namespace hardware;
  const int32_t n = static_cast<int32_t>(model_.row_count());
  const int32_t per = metrics_.per_line;
  if (copy_enabled_ && (args.modifiers & modifier::kControl) && args.usage == key::kA + 2) return copy_selected_item();   // Ctrl+C
  switch (args.usage) {
    case key::kEnter:
      if (selected_ >= 0) fire(activated_);
      return true;
    case 0x4A: if (n) select_item(0); return true;      // Home
    case 0x4D: if (n) select_item(n - 1); return true;  // End
    case key::kDown: move_selection(mode_ == ViewMode::Icons ? per : 1); return true;
    case key::kUp: move_selection(mode_ == ViewMode::Icons ? -per : -1); return true;
    case 0x4E: move_selection(mode_ == ViewMode::Details ? 10 : mode_ == ViewMode::Icons ? per * 3 : per); return true;    // Page Down
    case 0x4B: move_selection(mode_ == ViewMode::Details ? -10 : mode_ == ViewMode::Icons ? -per * 3 : -per); return true;  // Page Up
    case key::kRight:
      if (mode_ == ViewMode::List) move_selection(per);
      else if (mode_ == ViewMode::Icons) move_selection(1);
      else scroll_by(16, 0);
      return true;
    case key::kLeft:
      if (mode_ == ViewMode::List) move_selection(-per);
      else if (mode_ == ViewMode::Icons) move_selection(-1);
      else scroll_by(-16, 0);
      return true;
    default:
      return false;
  }
}

}  // namespace gui
