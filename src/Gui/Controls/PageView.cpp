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
#include "Paint/DisplayPainter.hpp"

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

PageView::~PageView() {}

void PageView::set_picture(UniquePointer<browser::PagePicture> picture, bool keep_scroll) {
  UiGuard lock(this);
  picture_ = util::move(picture);
  document_ = UniquePointer<browser::HtmlDocument>();
  picture_mode_ = true;
  if (!fonts_) {
    fonts_ = UniquePointer<font::FontSet>::make(allocator(), allocator());
    if (fonts_) fonts_->add_bundled_fonts();
  }
  if (!keep_scroll) scroll_y_ = 0;
  hover_ = -1;
  hovered_link_node_ = 0;
  hovered_link_.clear();
  // (the same page drawn again - the press itself changed how it looks, as :active does - keeps the press: its node serials still mean the same, and the click is
  // decided when the button comes up)
  if (!keep_scroll) press_link_ = -1, press_node_ = 0;
  measure();
  sync_scroll_bar();
  invalidate();
}

void PageView::scroll_to_y(int32_t y) {
  UiGuard lock(this);
  const int32_t content = picture_ ? picture_->height() : 0;
  const int32_t max_y = content > viewport_h_ ? content - viewport_h_ : 0;
  scroll_y_ = y < 0 ? 0 : y > max_y ? max_y : y;
  sync_scroll_bar();
  invalidate();
}

void PageView::scroll_by_y(int32_t dy) { scroll_to_y(scroll_y_ + dy); }

bool PageView::hit(int32_t x, int32_t y, paint::HitResult* result) const {
  if (!picture_ || x < 0 || x >= viewport_w_ || y < 0) return false;
  return paint::hit_test(picture_->list, picture_->document, x, y + scroll_y_, result);
}

void PageView::set_hover_node(uint32_t link_node) {
  if (link_node == hovered_link_node_) return;
  hovered_link_node_ = link_node;
  hovered_link_.clear();
  if (link_node && picture_ && picture_->document) {
    const dom::Node* n = picture_->document->by_serial(link_node);
    StringView href;
    if (n && dom::get_attribute(n, "href", &href)) hovered_link_.assign(href);
  }
  fire(hover_handler_);
  invalidate();
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
  {
    const int32_t w = static_cast<int32_t>(bounds_.width) - ScrollBar::kThickness, h = static_cast<int32_t>(bounds_.height);
    viewport_w_ = w > 1 ? w : 1;
    viewport_h_ = h > 1 ? h : 1;
  }
  if (picture_mode_) {
    const int32_t content = picture_ ? picture_->height() : 0;
    const int32_t max_y = content > viewport_h_ ? content - viewport_h_ : 0;
    if (scroll_y_ > max_y) scroll_y_ = max_y;
    if (scroll_y_ < 0) scroll_y_ = 0;
    return;
  }
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
  if (picture_mode_) {
    scroll_bar_.set_step(40);
    scroll_bar_.set_metrics(picture_ ? picture_->height() : 0, viewport_h_);
    scroll_bar_.set_value(scroll_y_);
    syncing_ = false;
    return;
  }
  scroll_bar_.set_metrics(static_cast<int32_t>(layout_.line_count()), static_cast<int32_t>(rows_));
  scroll_bar_.set_value(static_cast<int32_t>(top_));
  syncing_ = false;
}

void PageView::layout() {
  measure();
  sync_scroll_bar();
  if (picture_mode_) {
    if (viewport_w_ != notified_w_ || viewport_h_ != notified_h_) {
      notified_w_ = viewport_w_;
      notified_h_ = viewport_h_;
      fire(resize_handler_);
    }
  } else if (layout_.columns() != notified_columns_ || rows_ != notified_rows_) {
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
  if (v->picture_mode_) {
    v->scroll_y_ = v->scroll_bar_.value();
    v->invalidate();
    return;
  }
  v->top_ = static_cast<uint32_t>(v->scroll_bar_.value());
  v->invalidate();
}

void PageView::scroll_to_line(uint32_t line) {
  if (picture_mode_) {
    scroll_to_y(static_cast<int32_t>(line) * kLineHeight);
    return;
  }
  UiGuard lock(this);
  const uint32_t lines = layout_.line_count();
  const uint32_t max_top = lines > rows_ ? lines - rows_ : 0;
  top_ = line < max_top ? line : max_top;
  sync_scroll_bar();
  invalidate();
}

void PageView::scroll_by(int32_t lines) {
  if (picture_mode_) {
    scroll_by_y(lines * kLineHeight);
    return;
  }
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
  if (picture_mode_) {
    paint::HitResult r;
    return hit(x, y, &r) && r.link ? static_cast<int32_t>(r.link) : -1;
  }
  uint32_t line, column;
  if (!document_ || !locate(x, y, &line, &column)) return -1;
  return layout_.link_at(*document_, line, column);
}

uint32_t PageView::node_at(int32_t x, int32_t y) const {
  if (picture_mode_) {
    paint::HitResult r;
    return hit(x, y, &r) ? r.node : 0;
  }
  uint32_t line, column;
  if (!document_ || !locate(x, y, &line, &column)) return 0;
  const int64_t piece = layout_.piece_at(line, column);
  return piece < 0 ? 0 : document_->run(layout_.piece(static_cast<uint32_t>(piece)).run).node;
}

bool PageView::object_at(int32_t x, int32_t y, uint32_t* piece_index, int32_t* ox, int32_t* oy) const {
  uint32_t line, column;
  if (!document_ || !locate(x, y, &line, &column)) return false;
  const int64_t p = layout_.piece_at(line, column);
  if (p < 0) return false;
  const browser::Piece& piece = layout_.piece(static_cast<uint32_t>(p));
  if (document_->run(piece.run).object == 0) return false;
  *piece_index = static_cast<uint32_t>(p);
  *ox = kMargin + static_cast<int32_t>(piece.column) * Painter::kGlyphWidth;
  *oy = (static_cast<int32_t>(line) - static_cast<int32_t>(piece.from) - static_cast<int32_t>(top_)) * kLineHeight;
  return true;
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

void PageView::set_pointer(uint32_t node, bool pressed) {
  if (node == pointer_node_ && pressed == pointer_pressed_) return;
  pointer_node_ = node;
  pointer_pressed_ = pressed;
  if (pointer_handler_.function) fire(pointer_handler_);
}

void PageView::on_mouse_down(const MouseArgs& args) {
  UiGuard lock(this);
  if (args.button == MouseButton::Left) set_pointer(pointer_node_ ? pointer_node_ : node_at(args.x, args.y), true);
  press_link_ = args.button == MouseButton::Left ? link_index_at(args.x, args.y) : -1;
  press_node_ = args.button == MouseButton::Left && node_handler_.function ? node_at(args.x, args.y) : 0;
}

void PageView::on_mouse_move(const MouseArgs& args) {
  UiGuard lock(this);
  if (picture_mode_) {
    paint::HitResult r;
    const bool found = hit(args.x, args.y, &r);
    set_hover_node(found ? r.link : 0);
    set_pointer(found ? r.node : 0, pointer_pressed_);
    return;
  }
  set_hover(link_index_at(args.x, args.y));
  set_pointer(node_at(args.x, args.y), pointer_pressed_);
}

void PageView::on_mouse_up(const MouseArgs& args) {
  UiGuard lock(this);
  const int32_t pressed = press_link_;
  const uint32_t pressed_node = press_node_;
  set_pointer(pointer_node_, false);
  press_link_ = -1;
  press_node_ = 0;
  if (pressed_node != 0 && args.button == MouseButton::Left && node_at(args.x, args.y) == pressed_node) {
    clicked_node_ = pressed_node;
    clicked_x_ = picture_mode_ ? args.x : args.x - kMargin;
    clicked_y_ = picture_mode_ ? args.y + scroll_y_ : args.y + static_cast<int32_t>(top_) * kLineHeight;
    clicked_object_ = 0;
    uint32_t piece_index;
    int32_t ox, oy;
    paint::HitResult found;
    if (picture_mode_) {
      if (hit(args.x, args.y, &found) && found.object) {
        clicked_object_ = 1;
        clicked_local_x_ = args.x - found.rect.x;
        clicked_local_y_ = args.y + scroll_y_ - found.rect.y;
        clicked_w_ = static_cast<uint32_t>(found.rect.width);
        clicked_h_ = static_cast<uint32_t>(found.rect.height);
      }
    } else if (object_at(args.x, args.y, &piece_index, &ox, &oy)) {
      const browser::Piece& piece = layout_.piece(piece_index);
      clicked_object_ = document_->run(piece.run).object;
      clicked_local_x_ = args.x - ox;
      clicked_local_y_ = args.y - oy;
      clicked_w_ = piece.px_w;
      clicked_h_ = piece.px_h;
    }
    fire(node_handler_);
    return;
  }
  if (pressed < 0 || args.button != MouseButton::Left || link_index_at(args.x, args.y) != pressed || !document_) return;
  clicked_link_.assign(document_->link(static_cast<uint32_t>(pressed)));
  fire(link_handler_);
}

void PageView::on_mouse_leave() {
  UiGuard lock(this);
  if (picture_mode_) set_hover_node(0);
  set_hover(-1);
  set_pointer(0, false);
}

bool PageView::on_key(const KeyArgs& args) {
  {
    UiGuard lock(this);
    const bool held = args.modifiers & (hardware::modifier::kControl | hardware::modifier::kLeftAlt | hardware::modifier::kRightAlt);
    // (Control with A, C, V or X is the page's too: select all and the clipboard in a field)
    const bool edit_shortcut = (args.modifiers & hardware::modifier::kControl) && !(args.modifiers & (hardware::modifier::kLeftAlt | hardware::modifier::kRightAlt)) &&
                               (args.usage == hardware::key::kA || args.usage == hardware::key::kA + 2 || args.usage == hardware::key::kA + 21 || args.usage == hardware::key::kA + 23);
    const bool wanted = capture_ == kCaptureAll ? (!held || edit_shortcut) : capture_ == kCaptureTab && args.usage == hardware::key::kTab;
    if (wanted && key_handler_.function && queued_ < 16) {
      queue_[queued_++] = args;
      fire(key_handler_);
      return true;
    }
  }
  return scroll_key(args);
}

bool PageView::take_key(KeyArgs* out) {
  UiGuard lock(this);
  if (queued_ == 0) return false;
  *out = queue_[0];
  for (uint8_t i = 1; i < queued_; ++i) queue_[i - 1] = queue_[i];
  --queued_;
  return true;
}

bool PageView::scroll_key(const KeyArgs& args) {
  const bool shift = args.modifiers & hardware::modifier::kShift;
  if (picture_mode_) {
    const int32_t step = 40, screen = viewport_h_ > 80 ? viewport_h_ - 40 : viewport_h_;
    switch (args.usage) {
      case kUp: scroll_by_y(-step); return true;
      case kDown: scroll_by_y(step); return true;
      case kPageUp: scroll_by_y(-screen); return true;
      case kPageDown: scroll_by_y(screen); return true;
      case kSpace: scroll_by_y(shift ? -screen : screen); return true;
      case kHome: scroll_to_y(0); return true;
      case kEnd: scroll_to_y(picture_ ? picture_->height() : 0); return true;
      default: return false;
    }
  }
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
  if (picture_mode_) {
    const uint32_t paper = picture_ && picture_->list.background() ? picture_->list.background() : kPaper;
    painter.fill({0, 0, bounds_.width, bounds_.height}, paper);
    measure();
    sync_scroll_bar();
    if (!picture_ || !fonts_ || usable <= 0) return;
    // the page is drawn in the area left of the scroll bar, clipped to what is visible of it
    const graphics::Rect2D clip = painter.clip_rect();
    const int32_t left = painter.origin_x(), top = painter.origin_y();
    int32_t x0 = left > static_cast<int32_t>(clip.x) ? left : static_cast<int32_t>(clip.x);
    int32_t y0 = top > static_cast<int32_t>(clip.y) ? top : static_cast<int32_t>(clip.y);
    int32_t x1 = static_cast<int32_t>(clip.x + clip.width), y1 = static_cast<int32_t>(clip.y + clip.height);
    if (x1 > left + usable) x1 = left + usable;
    if (y1 > top + static_cast<int32_t>(bounds_.height)) y1 = top + static_cast<int32_t>(bounds_.height);
    if (x1 <= x0 || y1 <= y0) return;
    graphics::Rect2D view{x0, y0, static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)};
    paint::DisplayPainter page_painter(allocator(), *fonts_, objects_);
    page_painter.paint(painter.renderer(), picture_->list, left, top - scroll_y_, view);
    return;
  }
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
      if (run.object) {
        // an object is drawn once, from its first row - or from the first row on the screen when its top has scrolled off
        if (objects_ && (piece.from == 0 || line == top_)) {
          const int32_t top = (static_cast<int32_t>(line) - static_cast<int32_t>(piece.from) - static_cast<int32_t>(top_)) * kLineHeight;
          objects_->paint_object(painter.renderer(), run.node, static_cast<browser::ObjectKind>(run.object), painter.origin_x() + x, painter.origin_y() + top, piece.px_w, piece.px_h,
                                 painter.clip_rect());
        }
        continue;
      }
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
