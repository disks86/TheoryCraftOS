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

#include "Gui/Controls/PageView.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

namespace {
constexpr uint32_t kPaper = 0xFFFFFFFFu, kInk = 0xFF000000u, kHeadingInk = 0xFF000080u, kLinkInk = 0xFF0000EEu, kHoverInk = 0xFFCC0000u;
constexpr uint32_t kCodeShade = 0xFFEAEAEAu, kCodeShadeDark = 0xFF303030u, kRuleInk = 0xFF808080u;
constexpr uint32_t kInkDark = 0xFFE8E8E8u, kLinkInkDark = 0xFF8AB4FFu, kHeadingInkDark = 0xFFB0B0FFu;
constexpr uint8_t kPageUp = 0x4B, kPageDown = 0x4E, kHome = 0x4A, kEnd = 0x4D, kSpace = hardware::key::kSpace, kUp = hardware::key::kUp, kDown = hardware::key::kDown;
}  // namespace

PageView::PageView(allocators::IAllocator& allocator)
    : Control(allocator), layout_(allocator), scroll_bar_(allocator, Orientation::Vertical), clicked_link_(allocator), hovered_link_(allocator) {
  scroll_bar_.on_value_changed(Handler::immediate(&PageView::scroll_changed, this));
  add(&scroll_bar_);
}

void PageView::set_document(UniquePointer<browser::HtmlDocument> document, bool keep_scroll) {
  UiGuard lock(this);
  document_ = util::move(document);
  stale_ = true;
  if (!keep_scroll) top_ = 0;
  hover_ = press_link_ = -1;
  press_node_ = 0;
  hovered_link_.clear();
  measure();
  sync_scroll_bar();
  invalidate();
}

void PageView::measure() {
  const int32_t w = static_cast<int32_t>(bounds_.width) - ScrollBar::kThickness - 2 * kMargin, h = static_cast<int32_t>(bounds_.height);
  const uint32_t columns = w >= Painter::kGlyphWidth ? static_cast<uint32_t>(w / Painter::kGlyphWidth) : 1;
  rows_ = h >= kLineHeight ? static_cast<uint32_t>(h / kLineHeight) : 1;
  if (stale_ || columns != layout_.columns()) {
    if (document_) layout_.build(*document_, columns);
    else layout_.clear();
    stale_ = false;
  }
  const uint32_t lines = layout_.line_count();
  if (top_ + rows_ > lines) top_ = lines > rows_ ? lines - rows_ : 0;
}

void PageView::sync_scroll_bar() {
  syncing_ = true;
  scroll_bar_.set_metrics(static_cast<int32_t>(layout_.line_count()), static_cast<int32_t>(rows_));
  scroll_bar_.set_value(static_cast<int32_t>(top_));
  syncing_ = false;
}

void PageView::layout() {
  measure();
  sync_scroll_bar();
  if (layout_.columns() != notified_columns_ || rows_ != notified_rows_) {
    notified_columns_ = layout_.columns();
    notified_rows_ = rows_;
    fire(resize_handler_);
  }
  const int32_t thick = ScrollBar::kThickness;
  scroll_bar_.set_bounds({static_cast<int32_t>(bounds_.width) - thick, 0, static_cast<uint32_t>(thick), bounds_.height});
  Control::layout();
}

void PageView::scroll_changed(void* self, Control&) {
  auto* v = static_cast<PageView*>(self);
  if (v->syncing_) return;
  v->top_ = static_cast<uint32_t>(v->scroll_bar_.value());
  v->invalidate();
}

void PageView::scroll_to_line(uint32_t line) {
  UiGuard lock(this);
  const uint32_t lines = layout_.line_count();
  const uint32_t max_top = lines > rows_ ? lines - rows_ : 0;
  top_ = line < max_top ? line : max_top;
  sync_scroll_bar();
  invalidate();
}

void PageView::scroll_by(int32_t lines) {
  UiGuard lock(this);
  int64_t to = static_cast<int64_t>(top_) + lines;
  if (to < 0) to = 0;
  scroll_to_line(static_cast<uint32_t>(to));
}

// --- Links ------------------------------------------------------------------------------------------

bool PageView::locate(int32_t x, int32_t y, uint32_t* line, uint32_t* column) const {
  const int32_t usable = static_cast<int32_t>(bounds_.width) - ScrollBar::kThickness;
  if (x < kMargin || x >= usable || y < 0) return false;
  const uint32_t l = top_ + static_cast<uint32_t>(y / kLineHeight);
  if (l >= layout_.line_count()) return false;
  *line = l;
  *column = static_cast<uint32_t>((x - kMargin) / Painter::kGlyphWidth);
  return true;
}

int32_t PageView::link_index_at(int32_t x, int32_t y) const {
  uint32_t line, column;
  if (!document_ || !locate(x, y, &line, &column)) return -1;
  return layout_.link_at(*document_, line, column);
}

uint32_t PageView::node_at(int32_t x, int32_t y) const {
  uint32_t line, column;
  if (!document_ || !locate(x, y, &line, &column)) return 0;
  const int64_t piece = layout_.piece_at(line, column);
  return piece < 0 ? 0 : document_->run(layout_.piece(static_cast<uint32_t>(piece)).run).node;
}

bool PageView::link_at(int32_t x, int32_t y, String* target) const {
  UiGuard lock(this);
  const int32_t link = link_index_at(x, y);
  if (link < 0) return false;
  return target->assign(document_->link(static_cast<uint32_t>(link)));
}

void PageView::set_hover(int32_t link) {
  if (link == hover_) return;
  hover_ = link;
  hovered_link_.assign(link >= 0 && document_ ? document_->link(static_cast<uint32_t>(link)) : StringView());
  invalidate();
  fire(hover_handler_);
}

void PageView::on_mouse_down(const MouseArgs& args) {
  UiGuard lock(this);
  press_link_ = args.button == MouseButton::Left ? link_index_at(args.x, args.y) : -1;
  press_node_ = args.button == MouseButton::Left && node_handler_.function ? node_at(args.x, args.y) : 0;
}

void PageView::on_mouse_move(const MouseArgs& args) {
  UiGuard lock(this);
  set_hover(link_index_at(args.x, args.y));
}

void PageView::on_mouse_up(const MouseArgs& args) {
  UiGuard lock(this);
  const int32_t pressed = press_link_;
  const uint32_t pressed_node = press_node_;
  press_link_ = -1;
  press_node_ = 0;
  if (pressed_node != 0 && args.button == MouseButton::Left && node_at(args.x, args.y) == pressed_node) {
    clicked_node_ = pressed_node;
    clicked_x_ = args.x - kMargin;
    clicked_y_ = args.y + static_cast<int32_t>(top_) * kLineHeight;
    fire(node_handler_);
    return;
  }
  if (pressed < 0 || args.button != MouseButton::Left || link_index_at(args.x, args.y) != pressed || !document_) return;
  clicked_link_.assign(document_->link(static_cast<uint32_t>(pressed)));
  fire(link_handler_);
}

void PageView::on_mouse_leave() {
  UiGuard lock(this);
  set_hover(-1);
}

bool PageView::on_key(const KeyArgs& args) {
  const bool shift = args.modifiers & hardware::modifier::kShift;
  const int32_t page = rows_ > 1 ? static_cast<int32_t>(rows_) - 1 : 1;
  switch (args.usage) {
    case kUp: scroll_by(-1); return true;
    case kDown: scroll_by(1); return true;
    case kPageUp: scroll_by(-page); return true;
    case kPageDown: scroll_by(page); return true;
    case kSpace: scroll_by(shift ? -page : page); return true;
    case kHome: scroll_to_line(0); return true;
    case kEnd: scroll_to_line(layout_.line_count()); return true;
    default: return false;
  }
}

// --- Painting ---------------------------------------------------------------------------------------

// Is the colour dark enough that text on it should be light?
static bool is_dark(uint32_t color) {
  const uint32_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
  return (r * 299 + g * 587 + b * 114) / 1000 < 128;
}

void PageView::paint(Painter& painter) {
  const int32_t usable = static_cast<int32_t>(bounds_.width) - ScrollBar::kThickness;
  const uint32_t paper = document_ && document_->page_background() ? document_->page_background() : kPaper;
  painter.fill({0, 0, bounds_.width, bounds_.height}, paper);
  measure();
  sync_scroll_bar();
  if (!document_) return;
  const uint32_t lines = layout_.line_count();
  for (uint32_t row = 0; row < rows_ && top_ + row < lines; ++row) {
    const uint32_t line = top_ + row;
    const int32_t y = static_cast<int32_t>(row) * kLineHeight;
    const uint32_t first = layout_.first_piece(line), end = layout_.end_piece(line);
    if (first < end) {   // a block with a background of its own is painted across the whole line
      const browser::Run& lead = document_->run(layout_.piece(first).run);
      if ((lead.flags & browser::kRunBlockBg) && lead.background) painter.fill({0, y, static_cast<uint32_t>(usable > 0 ? usable : 0), static_cast<uint32_t>(kLineHeight)}, lead.background);
    }
    for (uint32_t p = first; p < end; ++p) {
      const browser::Piece& piece = layout_.piece(p);
      const browser::Run& run = document_->run(piece.run);
      const int32_t x = kMargin + static_cast<int32_t>(piece.column) * Painter::kGlyphWidth;
      const int32_t width = static_cast<int32_t>(piece.length) * Painter::kGlyphWidth;
      const uint32_t ground = run.background ? run.background : paper;   // what the text is on
      const bool dark = is_dark(ground);
      if (run.flags & browser::kRunRule) {
        painter.fill({kMargin, y + kLineHeight / 2 - 1, static_cast<uint32_t>(usable > 2 * kMargin ? usable - 2 * kMargin : 1), 2}, run.color ? run.color : kRuleInk);
        continue;
      }
      if (run.background && !(run.flags & browser::kRunBlockBg)) painter.fill({x, y, static_cast<uint32_t>(width), static_cast<uint32_t>(kLineHeight)}, run.background);
      else if (!run.background && (run.flags & (browser::kRunCode | browser::kRunPre))) painter.fill({x, y, static_cast<uint32_t>(width), static_cast<uint32_t>(kLineHeight)}, dark ? kCodeShadeDark : kCodeShade);
      const bool link = run.link >= 0;
      uint32_t ink;
      if (run.color) ink = run.color;
      else if (link) ink = run.link == hover_ ? kHoverInk : dark ? kLinkInkDark : kLinkInk;
      else if (run.flags & browser::kRunHeading) ink = dark ? kHeadingInkDark : kHeadingInk;
      else ink = dark ? kInkDark : kInk;
      if (link && run.link == hover_) ink = kHoverInk;
      const StringView text = document_->run_text(piece.run).substr(piece.from, piece.length);
      painter.text(x, y, text, ink);
      if (run.flags & browser::kRunBold) painter.text(x + 1, y, text, ink);
      if (run.flags & browser::kRunUnderline) painter.fill({x, y + kLineHeight - 2, static_cast<uint32_t>(width), 1}, ink);
      if (run.flags & browser::kRunStrike) painter.fill({x, y + kLineHeight / 2, static_cast<uint32_t>(width), 1}, ink);
    }
  }
}

}  // namespace gui
