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

#include "Gui/Controls/DrawingCanvas.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Kernel/Clipboard.hpp"

namespace gui {

namespace {

constexpr uint32_t kZoomSteps[] = {1, 2, 3, 4, 6, 8, 12, 16};
constexpr uint32_t kZoomStepCount = sizeof kZoomSteps / sizeof kZoomSteps[0];

int64_t floor_half(int64_t v) { return v >= 0 ? v / 2 : -((-v + 1) / 2); }

uint64_t integer_sqrt(uint64_t n) {
  if (n == 0) return 0;
  uint64_t x = n, y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + n / x) / 2;
  }
  return x;
}

// Drawing on a bitmap of 0xAARRGGBB pixels; everything is clipped to it.
struct Raster {
  uint32_t* px;
  int32_t w, h;

  void plot(int32_t x, int32_t y, uint32_t c) const {
    if (static_cast<uint32_t>(x) < static_cast<uint32_t>(w) && static_cast<uint32_t>(y) < static_cast<uint32_t>(h)) px[static_cast<int64_t>(y) * w + x] = c;
  }

  // A dot `size` pixels across centered on (x, y): round, or square.
  void stamp(int32_t x, int32_t y, int32_t size, bool square, uint32_t c) const {
    if (size <= 1) {
      plot(x, y, c);
      return;
    }
    const int32_t left = x - (size - 1) / 2, top = y - (size - 1) / 2;
    for (int32_t oy = 0; oy < size; ++oy) {
      for (int32_t ox = 0; ox < size; ++ox) {
        if (!square) {
          const int64_t dx2 = 2 * ox - (size - 1), dy2 = 2 * oy - (size - 1);
          if (dx2 * dx2 + dy2 * dy2 > static_cast<int64_t>(size) * size) continue;
        }
        plot(left + ox, top + oy, c);
      }
    }
  }

  void line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t size, bool square, uint32_t c) const {
    const int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;  // dy <= 0
    const int32_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (;;) {
      stamp(x0, y0, size, square, c);
      if (x0 == x1 && y0 == y1) break;
      const int32_t e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }

  void fill_rect(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c) const {
    if (x0 > x1) { const int32_t t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { const int32_t t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (y1 >= h) y1 = h - 1;
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x) px[static_cast<int64_t>(y) * w + x] = c;
  }

  // A rectangle between two corners: the inside in `fill` (if `filled`), the outline `size` wide in `outline`.
  void rectangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t size, uint32_t outline, bool filled, uint32_t fill) const {
    if (filled) fill_rect(x0, y0, x1, y1, fill);
    line(x0, y0, x1, y0, size, true, outline);
    line(x1, y0, x1, y1, size, true, outline);
    line(x1, y1, x0, y1, size, true, outline);
    line(x0, y1, x0, y0, size, true, outline);
  }

  // An ellipse inside the box between two corners.
  void ellipse(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t size, uint32_t outline, bool filled, uint32_t fill) const {
    if (x0 > x1) { const int32_t t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { const int32_t t = y0; y0 = y1; y1 = t; }
    const int64_t w2 = x1 - x0, h2 = y1 - y0, cx2 = int64_t{x0} + x1, cy2 = int64_t{y0} + y1;  // doubled: centers fall on half pixels
    if (w2 == 0 || h2 == 0) {
      line(x0, y0, x1, y1, size, false, outline);
      return;
    }
    if (filled) {
      for (int32_t y = y0; y <= y1; ++y) {
        const int64_t dy2 = 2 * int64_t{y} - cy2;
        const int64_t dx2 = static_cast<int64_t>(integer_sqrt(static_cast<uint64_t>(w2 * w2 * (h2 * h2 - dy2 * dy2) / (h2 * h2))));
        fill_rect(static_cast<int32_t>(floor_half(cx2 - dx2)), y, static_cast<int32_t>(floor_half(cx2 + dx2 + 1)), y, fill);
      }
    }
    for (int32_t y = y0; y <= y1; ++y) {  // the ends of every row ...
      const int64_t dy2 = 2 * int64_t{y} - cy2;
      const int64_t dx2 = static_cast<int64_t>(integer_sqrt(static_cast<uint64_t>(w2 * w2 * (h2 * h2 - dy2 * dy2) / (h2 * h2))));
      stamp(static_cast<int32_t>(floor_half(cx2 - dx2)), y, size, false, outline);
      stamp(static_cast<int32_t>(floor_half(cx2 + dx2 + 1)), y, size, false, outline);
    }
    for (int32_t x = x0; x <= x1; ++x) {  // ... and of every column, so steep and flat parts have no gaps
      const int64_t dx2 = 2 * int64_t{x} - cx2;
      const int64_t dy2 = static_cast<int64_t>(integer_sqrt(static_cast<uint64_t>(h2 * h2 * (w2 * w2 - dx2 * dx2) / (w2 * w2))));
      stamp(x, static_cast<int32_t>(floor_half(cy2 - dy2)), size, false, outline);
      stamp(x, static_cast<int32_t>(floor_half(cy2 + dy2 + 1)), size, false, outline);
    }
  }

  // Replaces the area of one color around (x, y). False if memory ran out part way.
  bool flood(int32_t x, int32_t y, uint32_t color, List<uint32_t>* stack) const {
    if (static_cast<uint32_t>(x) >= static_cast<uint32_t>(w) || static_cast<uint32_t>(y) >= static_cast<uint32_t>(h)) return true;
    const uint32_t target = px[static_cast<int64_t>(y) * w + x];
    if (target == color) return true;
    stack->clear();
    if (!stack->push_back(static_cast<uint32_t>(y) * w + x)) return false;
    while (stack->count() > 0) {
      const uint32_t at = (*stack)[stack->count() - 1];
      stack->remove_at(stack->count() - 1);
      const int32_t sx = static_cast<int32_t>(at % w), sy = static_cast<int32_t>(at / w);
      uint32_t* row = px + static_cast<int64_t>(sy) * w;
      if (row[sx] != target) continue;
      int32_t l = sx, r = sx;
      while (l > 0 && row[l - 1] == target) --l;
      while (r < w - 1 && row[r + 1] == target) ++r;
      for (int32_t i = l; i <= r; ++i) row[i] = color;
      for (int32_t ny = sy - 1; ny <= sy + 1; ny += 2) {  // seed the rows above and below, once per run
        if (ny < 0 || ny >= h) continue;
        const uint32_t* other = px + static_cast<int64_t>(ny) * w;
        bool in_run = false;
        for (int32_t i = l; i <= r; ++i) {
          if (other[i] == target) {
            if (!in_run && !stack->push_back(static_cast<uint32_t>(ny) * w + i)) return false;
            in_run = true;
          } else {
            in_run = false;
          }
        }
      }
    }
    return true;
  }
};

uint32_t over_white(uint32_t argb) {
  const uint32_t a = argb >> 24;
  if (a == 255) return argb | 0xFF000000u;
  auto mix = [&](uint32_t c) { return (c * a + 255 * (255 - a) + 127) / 255; };
  return 0xFF000000u | (mix((argb >> 16) & 255) << 16) | (mix((argb >> 8) & 255) << 8) | mix(argb & 255);
}

}  // namespace

DrawingCanvas::DrawingCanvas(allocators::IAllocator& allocator)
    : Control(allocator),
      undo_(allocator),
      redo_(allocator),
      vscroll_(allocator, Orientation::Vertical),
      hscroll_(allocator, Orientation::Horizontal) {
  vscroll_.on_value_changed(Handler::immediate(&DrawingCanvas::vertical_changed, this));
  hscroll_.on_value_changed(Handler::immediate(&DrawingCanvas::horizontal_changed, this));
  vscroll_.set_step(16);
  hscroll_.set_step(16);
  add(&vscroll_);
  add(&hscroll_);
  vscroll_.set_visible(false);
  hscroll_.set_visible(false);
}

DrawingCanvas::~DrawingCanvas() {
  release_floating();
  drop_history(&undo_);
  drop_history(&redo_);
  release_view(allocator(), pixels_);
  release_view(allocator(), cache_);
}

void DrawingCanvas::vertical_changed(void* self, Control&) { static_cast<DrawingCanvas*>(self)->on_scroll_changed(); }
void DrawingCanvas::horizontal_changed(void* self, Control&) { static_cast<DrawingCanvas*>(self)->on_scroll_changed(); }

void DrawingCanvas::on_scroll_changed() {
  if (syncing_) return;
  scroll_x_ = hscroll_.value();
  scroll_y_ = vscroll_.value();
  cache_dirty_ = true;
  invalidate();
}

// --- The bitmap ---------------------------------------------------------------------

graphics::Surface DrawingCanvas::surface() const {
  graphics::Surface s;
  s.pixels = pixels_;
  s.width = width_;
  s.height = height_;
  s.pitch = width_;
  return s;
}

void DrawingCanvas::after_pixels_changed() {
  cache_dirty_ = true;
  update_scroll_bars();
  invalidate();
}

bool DrawingCanvas::new_image(uint32_t width, uint32_t height, uint32_t fill) {
  UiGuard lock(this);
  if (width == 0 || height == 0 || width > kMaxSide || height > kMaxSide) return false;
  ArrayView<uint32_t> fresh = allocate_view<uint32_t>(allocator(), uint64_t{width} * height);
  if (fresh.empty()) return false;
  for (uint32_t& p : fresh) p = fill | 0xFF000000u;
  cancel_stroke();
  release_floating();
  sel_valid_ = selecting_ = false;
  drop_history(&undo_);
  drop_history(&redo_);
  release_view(allocator(), pixels_);
  pixels_ = fresh;
  width_ = width;
  height_ = height;
  modified_ = false;
  scroll_x_ = scroll_y_ = 0;
  after_pixels_changed();
  fire(changed_);
  return true;
}

bool DrawingCanvas::set_pixels(const uint32_t* argb, uint32_t width, uint32_t height) {
  UiGuard lock(this);
  if (!argb || !new_image(width, height)) return false;
  for (uint64_t i = 0; i < uint64_t{width} * height; ++i) pixels_[i] = over_white(argb[i]);
  after_pixels_changed();
  return true;
}

// --- History ---------------------------------------------------------------------------

void DrawingCanvas::drop_history(List<ArrayView<uint32_t>>* list) {
  for (ArrayView<uint32_t>& v : *list) {
    history_bytes_ -= v.size() * sizeof(uint32_t);
    release_view(allocator(), v);
  }
  list->clear();
}

void DrawingCanvas::trim_history() {
  while (undo_.count() > 0 && (undo_.count() > kMaxHistoryEntries || history_bytes_ > kMaxHistoryBytes)) {
    history_bytes_ -= undo_[0].size() * sizeof(uint32_t);
    release_view(allocator(), undo_[0]);
    undo_.remove_at(0);
  }
}

// Saves the bitmap as it is before a change. False if there is no room for it.
bool DrawingCanvas::push_history() {
  if (pixels_.empty()) return false;
  ArrayView<uint32_t> copy = allocate_view<uint32_t>(allocator(), pixels_.size());
  if (copy.empty()) return false;
  copy.copy_from(ArrayView<const uint32_t>(pixels_.data(), pixels_.size()));
  if (!undo_.push_back(copy)) {
    release_view(allocator(), copy);
    return false;
  }
  history_bytes_ += copy.size() * sizeof(uint32_t);
  drop_history(&redo_);
  trim_history();
  return true;
}

// Exchanges the bitmap with the newest entry of `from`, which moves to `to` holding what the bitmap was.
void DrawingCanvas::swap_with(List<ArrayView<uint32_t>>* from, List<ArrayView<uint32_t>>* to) {
  if (from->count() == 0) return;
  ArrayView<uint32_t> saved = (*from)[from->count() - 1];
  ArrayView<uint32_t> current = allocate_view<uint32_t>(allocator(), pixels_.size());
  if (current.empty() || !to->push_back(current)) {
    release_view(allocator(), current);
    return;
  }
  history_bytes_ += current.size() * sizeof(uint32_t);
  current.copy_from(ArrayView<const uint32_t>(pixels_.data(), pixels_.size()));
  pixels_.copy_from(ArrayView<const uint32_t>(saved.data(), saved.size()));
  history_bytes_ -= saved.size() * sizeof(uint32_t);
  release_view(allocator(), saved);
  from->remove_at(from->count() - 1);
}

void DrawingCanvas::finish_change() {
  modified_ = true;
  cache_dirty_ = true;
  invalidate();
  fire(changed_);
}

void DrawingCanvas::undo() {
  UiGuard lock(this);
  if (drawing_) {
    cancel_stroke();
    return;
  }
  if (floating_) {
    cancel_floating();
    return;
  }
  swap_with(&undo_, &redo_);
  finish_change();
}

void DrawingCanvas::redo() {
  UiGuard lock(this);
  if (drawing_ || floating_ || redo_.count() == 0) return;
  swap_with(&redo_, &undo_);
  finish_change();
}

// --- Tools -----------------------------------------------------------------------------------

void DrawingCanvas::set_tool(PaintTool tool) {
  UiGuard lock(this);
  if (tool != PaintTool::Select) commit_floating();
  tool_ = tool;
}

void DrawingCanvas::set_tool_quietly(PaintTool tool) { tool_ = tool; }

void DrawingCanvas::set_brush_size(uint32_t size) {
  UiGuard lock(this);
  brush_size_ = size < 1 ? 1 : size > 64 ? 64 : size;
}

bool DrawingCanvas::pointer_to_image(const MouseArgs& args, int32_t* x, int32_t* y) const {
  const Rect2D v = viewport();
  const int64_t vx = int64_t{args.x} - v.x + scroll_x_, vy = int64_t{args.y} - v.y + scroll_y_;
  const int64_t z = zoom_;
  *x = static_cast<int32_t>(vx >= 0 ? vx / z : -((-vx + z - 1) / z));  // floor: points left of / above the bitmap are negative
  *y = static_cast<int32_t>(vy >= 0 ? vy / z : -((-vy + z - 1) / z));
  return *x >= 0 && *y >= 0 && *x < static_cast<int32_t>(width_) && *y < static_cast<int32_t>(height_);
}

void DrawingCanvas::begin_stroke(const MouseArgs& args) {
  if (pixels_.empty()) return;
  int32_t x, y;
  const bool inside = pointer_to_image(args, &x, &y);
  const bool left = args.button == MouseButton::Left;
  const uint32_t main_color = left ? primary_ : secondary_;
  if (tool_ == PaintTool::Select) {
    if (!left) return;
    if (floating_) {
      if (contains(selection(), x, y)) {  // grab the floating picture
        moving_ = true;
        grab_dx_ = x - sel_x_;
        grab_dy_ = y - sel_y_;
        return;
      }
      commit_floating();
    }
    if (sel_valid_ && contains(selection(), x, y)) {  // lift what is selected and carry it
      lift_selection();
      if (floating_) {
        moving_ = true;
        grab_dx_ = x - sel_x_;
        grab_dy_ = y - sel_y_;
      }
      return;
    }
    begin_selection(x, y);
    return;
  }
  commit_floating();
  if (tool_ == PaintTool::Picker) {
    if (!inside) return;
    const uint32_t c = pixels_[uint64_t{static_cast<uint32_t>(y)} * width_ + static_cast<uint32_t>(x)];
    if (left) primary_ = c;
    else secondary_ = c;
    fire(picked_);
    return;
  }
  if (pixels_.empty() || !push_history()) return;
  const Raster r{pixels_.data(), static_cast<int32_t>(width_), static_cast<int32_t>(height_)};
  if (tool_ == PaintTool::Fill) {
    List<uint32_t> stack(allocator());
    if (inside) r.flood(x, y, main_color, &stack);
    finish_change();
    return;
  }
  drawing_ = true;
  button_ = args.button;
  start_x_ = last_x_ = x;
  start_y_ = last_y_ = y;
  draw_to(x, y);
}

// Draws the stroke up to the pointer at (x, y).
void DrawingCanvas::draw_to(int32_t x, int32_t y) {
  const Raster r{pixels_.data(), static_cast<int32_t>(width_), static_cast<int32_t>(height_)};
  const bool left = button_ == MouseButton::Left;
  const uint32_t main_color = left ? primary_ : secondary_, other = left ? secondary_ : primary_;
  const int32_t size = static_cast<int32_t>(brush_size_);
  switch (tool_) {
    case PaintTool::Pencil:
      r.line(last_x_, last_y_, x, y, size, false, main_color);
      break;
    case PaintTool::Eraser:
      r.line(last_x_, last_y_, x, y, size < 3 ? 3 : size, true, secondary_);
      break;
    default: {  // shapes: start again from the bitmap as it was, and draw the shape as it is now
      pixels_.copy_from(ArrayView<const uint32_t>(undo_[undo_.count() - 1].data(), pixels_.size()));
      if (tool_ == PaintTool::Line) r.line(start_x_, start_y_, x, y, size, false, main_color);
      else if (tool_ == PaintTool::Rectangle) r.rectangle(start_x_, start_y_, x, y, size, main_color, filled_, other);
      else if (tool_ == PaintTool::Ellipse) r.ellipse(start_x_, start_y_, x, y, size, main_color, filled_, other);
      break;
    }
  }
  last_x_ = x;
  last_y_ = y;
  cache_dirty_ = true;
  invalidate();
}

void DrawingCanvas::cancel_stroke() {
  if (!drawing_) return;
  drawing_ = false;
  if (undo_.count() > 0) {  // put back what the stroke started from
    ArrayView<uint32_t> saved = undo_[undo_.count() - 1];
    pixels_.copy_from(ArrayView<const uint32_t>(saved.data(), saved.size()));
    history_bytes_ -= saved.size() * sizeof(uint32_t);
    release_view(allocator(), saved);
    undo_.remove_at(undo_.count() - 1);
  }
  cache_dirty_ = true;
  invalidate();
}

// --- Selection and clipboard ------------------------------------------------------------------

void DrawingCanvas::release_floating() {
  release_view(allocator(), float_);
  release_view(allocator(), base_);
  floating_ = moving_ = false;
}

// Puts the floating picture over the bitmap as it was without it.
void DrawingCanvas::render_floating() {
  if (!floating_) return;
  pixels_.copy_from(ArrayView<const uint32_t>(base_.data(), base_.size()));
  for (uint32_t y = 0; y < sel_h_; ++y) {
    const int64_t dy = int64_t{sel_y_} + y;
    if (dy < 0 || dy >= height_) continue;
    for (uint32_t x = 0; x < sel_w_; ++x) {
      const int64_t dx = int64_t{sel_x_} + x;
      if (dx < 0 || dx >= width_) continue;
      const uint32_t src = float_[uint64_t{y} * sel_w_ + x], a = src >> 24;
      uint32_t& dst = pixels_[static_cast<uint64_t>(dy) * width_ + static_cast<uint64_t>(dx)];
      if (a == 255) {
        dst = src;
      } else if (a != 0) {  // translucent: blend over what is there
        auto mix = [&](uint32_t shift) { return (((src >> shift) & 255) * a + ((dst >> shift) & 255) * (255 - a) + 127) / 255; };
        dst = 0xFF000000u | (mix(16) << 16) | (mix(8) << 8) | mix(0);
      }
    }
  }
  cache_dirty_ = true;
  invalidate();
}

// Sets the floating picture down where it is; the selection stays, cut to the bitmap.
void DrawingCanvas::commit_floating() {
  UiGuard lock(this);
  if (!floating_) return;
  release_floating();
  const int64_t x0 = sel_x_ < 0 ? 0 : sel_x_, y0 = sel_y_ < 0 ? 0 : sel_y_;
  const int64_t x1 = int64_t{sel_x_} + sel_w_ < width_ ? int64_t{sel_x_} + sel_w_ : width_;
  const int64_t y1 = int64_t{sel_y_} + sel_h_ < height_ ? int64_t{sel_y_} + sel_h_ : height_;
  if (x1 > x0 && y1 > y0) {
    sel_x_ = static_cast<int32_t>(x0);
    sel_y_ = static_cast<int32_t>(y0);
    sel_w_ = static_cast<uint32_t>(x1 - x0);
    sel_h_ = static_cast<uint32_t>(y1 - y0);
  } else {
    sel_valid_ = false;
  }
  finish_change();
}

// Takes the floating picture back: the bitmap returns to how it was before it was lifted or pasted.
void DrawingCanvas::cancel_floating() {
  if (!floating_) return;
  if (undo_.count() > 0) {
    ArrayView<uint32_t> saved = undo_[undo_.count() - 1];
    pixels_.copy_from(ArrayView<const uint32_t>(saved.data(), saved.size()));
    history_bytes_ -= saved.size() * sizeof(uint32_t);
    release_view(allocator(), saved);
    undo_.remove_at(undo_.count() - 1);
  }
  release_floating();
  sel_valid_ = false;
  finish_change();
}

void DrawingCanvas::lift_selection() {
  if (!sel_valid_ || floating_ || pixels_.empty()) return;
  ArrayView<uint32_t> base = allocate_view<uint32_t>(allocator(), pixels_.size());
  ArrayView<uint32_t> picture = allocate_view<uint32_t>(allocator(), uint64_t{sel_w_} * sel_h_);
  if (base.empty() || picture.empty() || !push_history()) {
    release_view(allocator(), base);
    release_view(allocator(), picture);
    return;
  }
  base.copy_from(ArrayView<const uint32_t>(pixels_.data(), pixels_.size()));
  for (uint32_t y = 0; y < sel_h_; ++y)
    for (uint32_t x = 0; x < sel_w_; ++x) {
      const uint64_t at = uint64_t{static_cast<uint32_t>(sel_y_) + y} * width_ + static_cast<uint32_t>(sel_x_) + x;
      picture[uint64_t{y} * sel_w_ + x] = pixels_[at];
      base[at] = secondary_;  // the hole it leaves
    }
  base_ = base;
  float_ = picture;
  floating_ = true;
  render_floating();
}

void DrawingCanvas::begin_selection(int32_t x, int32_t y) {
  x = x < 0 ? 0 : x >= static_cast<int32_t>(width_) ? static_cast<int32_t>(width_) - 1 : x;
  y = y < 0 ? 0 : y >= static_cast<int32_t>(height_) ? static_cast<int32_t>(height_) - 1 : y;
  start_x_ = x;
  start_y_ = y;
  sel_x_ = x;
  sel_y_ = y;
  sel_w_ = sel_h_ = 1;
  sel_valid_ = selecting_ = true;
  invalidate();
}

void DrawingCanvas::drag_selection(int32_t x, int32_t y) {
  x = x < 0 ? 0 : x >= static_cast<int32_t>(width_) ? static_cast<int32_t>(width_) - 1 : x;
  y = y < 0 ? 0 : y >= static_cast<int32_t>(height_) ? static_cast<int32_t>(height_) - 1 : y;
  sel_x_ = x < start_x_ ? x : start_x_;
  sel_y_ = y < start_y_ ? y : start_y_;
  sel_w_ = static_cast<uint32_t>((x < start_x_ ? start_x_ - x : x - start_x_) + 1);
  sel_h_ = static_cast<uint32_t>((y < start_y_ ? start_y_ - y : y - start_y_) + 1);
  invalidate();
}

void DrawingCanvas::select_all() {
  UiGuard lock(this);
  commit_floating();
  if (pixels_.empty()) return;
  sel_x_ = sel_y_ = 0;
  sel_w_ = width_;
  sel_h_ = height_;
  sel_valid_ = true;
  if (tool_ != PaintTool::Select) {  // so the selection can be dragged at once
    tool_ = PaintTool::Select;
    fire(tool_changed_);
  }
  invalidate();
}

void DrawingCanvas::select_rect(int32_t x, int32_t y, uint32_t width, uint32_t height) {
  UiGuard lock(this);
  commit_floating();
  const int64_t x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  const int64_t x1 = int64_t{x} + width < width_ ? int64_t{x} + width : width_, y1 = int64_t{y} + height < height_ ? int64_t{y} + height : height_;
  sel_valid_ = x1 > x0 && y1 > y0;
  if (sel_valid_) {
    sel_x_ = static_cast<int32_t>(x0);
    sel_y_ = static_cast<int32_t>(y0);
    sel_w_ = static_cast<uint32_t>(x1 - x0);
    sel_h_ = static_cast<uint32_t>(y1 - y0);
  }
  invalidate();
}

void DrawingCanvas::clear_selection() {
  UiGuard lock(this);
  commit_floating();
  sel_valid_ = selecting_ = false;
  invalidate();
}

bool DrawingCanvas::copy_selection() {
  UiGuard lock(this);
  kernel::Clipboard* clipboard = this->clipboard();
  if (!clipboard || pixels_.empty()) return false;
  if (floating_) return clipboard->set_image(float_.data(), sel_w_, sel_h_);
  if (!sel_valid_) return clipboard->set_image(pixels_.data(), width_, height_);  // nothing selected: the whole picture
  ArrayView<uint32_t> part = allocate_view<uint32_t>(allocator(), uint64_t{sel_w_} * sel_h_);
  if (part.empty()) return false;
  for (uint32_t y = 0; y < sel_h_; ++y)
    for (uint32_t x = 0; x < sel_w_; ++x) part[uint64_t{y} * sel_w_ + x] = pixels_[uint64_t{static_cast<uint32_t>(sel_y_) + y} * width_ + static_cast<uint32_t>(sel_x_) + x];
  const bool ok = clipboard->set_image(part.data(), sel_w_, sel_h_);
  release_view(allocator(), part);
  return ok;
}

bool DrawingCanvas::cut_selection() {
  UiGuard lock(this);
  if (!sel_valid_ || !copy_selection()) return false;
  delete_selection();
  return true;
}

void DrawingCanvas::delete_selection() {
  UiGuard lock(this);
  if (!sel_valid_) return;
  if (floating_) {  // the floating picture is dropped; what was under it stays as the lift left it
    pixels_.copy_from(ArrayView<const uint32_t>(base_.data(), base_.size()));
    release_floating();
    sel_valid_ = false;
    finish_change();
    return;
  }
  if (!push_history()) return;
  Raster{pixels_.data(), static_cast<int32_t>(width_), static_cast<int32_t>(height_)}
      .fill_rect(sel_x_, sel_y_, sel_x_ + static_cast<int32_t>(sel_w_) - 1, sel_y_ + static_cast<int32_t>(sel_h_) - 1, secondary_);
  finish_change();
}

bool DrawingCanvas::paste() {
  UiGuard lock(this);
  kernel::Clipboard* clipboard = this->clipboard();
  uint32_t w, h;
  if (!clipboard || pixels_.empty() || !clipboard->image_size(&w, &h) || w > kMaxSide || h > kMaxSide) return false;
  commit_floating();
  ArrayView<uint32_t> picture = allocate_view<uint32_t>(allocator(), uint64_t{w} * h);
  ArrayView<uint32_t> base = allocate_view<uint32_t>(allocator(), pixels_.size());
  if (picture.empty() || base.empty() || !clipboard->get_image(picture.data(), picture.size()) || !push_history()) {
    release_view(allocator(), picture);
    release_view(allocator(), base);
    return false;
  }
  base.copy_from(ArrayView<const uint32_t>(pixels_.data(), pixels_.size()));
  base_ = base;
  float_ = picture;
  floating_ = true;
  sel_valid_ = true;
  sel_w_ = w;
  sel_h_ = h;
  // It appears in the top left corner of what is in view, kept inside the bitmap if it fits.
  int64_t x = scroll_x_ / static_cast<int32_t>(zoom_), y = scroll_y_ / static_cast<int32_t>(zoom_);
  if (x + w > width_) x = width_ > w ? width_ - w : 0;
  if (y + h > height_) y = height_ > h ? height_ - h : 0;
  sel_x_ = static_cast<int32_t>(x);
  sel_y_ = static_cast<int32_t>(y);
  set_tool_quietly(PaintTool::Select);
  render_floating();
  finish_change();
  fire(tool_changed_);
  return true;
}

// --- View ---------------------------------------------------------------------------------------

void DrawingCanvas::set_zoom(uint32_t zoom) {
  UiGuard lock(this);
  uint32_t best = 0;
  for (uint32_t i = 0; i < kZoomStepCount; ++i)
    if ((kZoomSteps[i] > zoom ? kZoomSteps[i] - zoom : zoom - kZoomSteps[i]) < (kZoomSteps[best] > zoom ? kZoomSteps[best] - zoom : zoom - kZoomSteps[best])) best = i;
  if (kZoomSteps[best] == zoom_) return;
  // Keep the middle of the view where it is.
  const Rect2D v = viewport();
  const int64_t cx = (int64_t{scroll_x_} + v.width / 2) / zoom_, cy = (int64_t{scroll_y_} + v.height / 2) / zoom_;
  zoom_ = kZoomSteps[best];
  scroll_x_ = static_cast<int32_t>(cx * zoom_ - v.width / 2);
  scroll_y_ = static_cast<int32_t>(cy * zoom_ - v.height / 2);
  if (scroll_x_ < 0) scroll_x_ = 0;
  if (scroll_y_ < 0) scroll_y_ = 0;
  after_pixels_changed();
  fire(zoom_changed_);
}

void DrawingCanvas::zoom_in() {
  for (uint32_t i = 0; i + 1 < kZoomStepCount; ++i)
    if (kZoomSteps[i] == zoom_) {
      set_zoom(kZoomSteps[i + 1]);
      return;
    }
}

void DrawingCanvas::zoom_out() {
  for (uint32_t i = 1; i < kZoomStepCount; ++i)
    if (kZoomSteps[i] == zoom_) {
      set_zoom(kZoomSteps[i - 1]);
      return;
    }
}

void DrawingCanvas::set_grid(bool grid) {
  UiGuard lock(this);
  grid_ = grid;
  cache_dirty_ = true;
  invalidate();
}

Rect2D DrawingCanvas::viewport() const {
  const int32_t w = static_cast<int32_t>(bounds_.width) - 2 * kBorder - (show_v_ ? ScrollBar::kThickness : 0);
  const int32_t h = static_cast<int32_t>(bounds_.height) - 2 * kBorder - (show_h_ ? ScrollBar::kThickness : 0);
  return {kBorder, kBorder, static_cast<uint32_t>(w > 0 ? w : 0), static_cast<uint32_t>(h > 0 ? h : 0)};
}

Point DrawingCanvas::image_to_view(int32_t x, int32_t y) const {
  const Rect2D v = viewport();
  return {v.x + x * static_cast<int32_t>(zoom_) - scroll_x_, v.y + y * static_cast<int32_t>(zoom_) - scroll_y_};
}

bool DrawingCanvas::view_to_image(int32_t x, int32_t y, int32_t* image_x, int32_t* image_y) const {
  MouseArgs a;
  a.x = x;
  a.y = y;
  return pointer_to_image(a, image_x, image_y);
}

bool DrawingCanvas::pointer_position(int32_t* x, int32_t* y) const {
  *x = pointer_x_;
  *y = pointer_y_;
  return pointer_x_ >= 0;
}

CursorShape DrawingCanvas::cursor_shape(int32_t x, int32_t y) const {
  int32_t ix, iy;
  return view_to_image(x, y, &ix, &iy) || drawing_ ? CursorShape::Crosshair : CursorShape::Arrow;
}

void DrawingCanvas::update_scroll_bars() {
  const int32_t full_w = static_cast<int32_t>(bounds_.width) - 2 * kBorder, full_h = static_cast<int32_t>(bounds_.height) - 2 * kBorder;
  const int32_t thick = ScrollBar::kThickness;
  const int32_t content_w = static_cast<int32_t>(width_ * zoom_), content_h = static_cast<int32_t>(height_ * zoom_);
  bool v = false, h = false;
  for (int pass = 0; pass < 3; ++pass) {
    const bool need_v = content_h > full_h - (h ? thick : 0), need_h = content_w > full_w - (v ? thick : 0);
    if (need_v == v && need_h == h) break;
    v = need_v;
    h = need_h;
  }
  show_v_ = v;
  show_h_ = h;
  const int32_t vw = full_w - (v ? thick : 0), vh = full_h - (h ? thick : 0);
  const int32_t max_x = content_w > vw ? content_w - vw : 0, max_y = content_h > vh ? content_h - vh : 0;
  if (scroll_x_ > max_x) scroll_x_ = max_x;
  if (scroll_y_ > max_y) scroll_y_ = max_y;
  if (scroll_x_ < 0) scroll_x_ = 0;
  if (scroll_y_ < 0) scroll_y_ = 0;
  syncing_ = true;
  vscroll_.set_metrics(content_h, vh);
  vscroll_.set_value(scroll_y_);
  hscroll_.set_metrics(content_w, vw);
  hscroll_.set_value(scroll_x_);
  syncing_ = false;
  vscroll_.set_visible(v);
  hscroll_.set_visible(h);
}

void DrawingCanvas::layout() {
  update_scroll_bars();
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  const int32_t thick = ScrollBar::kThickness;
  const int32_t vh = h - 2 * kBorder - (show_h_ ? thick : 0), hw = w - 2 * kBorder - (show_v_ ? thick : 0);
  vscroll_.set_bounds({w - kBorder - thick, kBorder, static_cast<uint32_t>(thick), static_cast<uint32_t>(vh > 0 ? vh : 0)});
  hscroll_.set_bounds({kBorder, h - kBorder - thick, static_cast<uint32_t>(hw > 0 ? hw : 0), static_cast<uint32_t>(thick)});
  cache_dirty_ = true;
  Control::layout();
}

// Draws the visible part of the bitmap, at the current zoom, into the cache that paint() copies to the screen.
void DrawingCanvas::rebuild_cache() {
  const Rect2D v = viewport();
  if (v.width != cache_w_ || v.height != cache_h_) {
    release_view(allocator(), cache_);
    cache_ = allocate_view<uint32_t>(allocator(), uint64_t{v.width} * v.height);
    cache_w_ = cache_.empty() ? 0 : v.width;
    cache_h_ = cache_.empty() ? 0 : v.height;
  }
  if (cache_.empty()) return;
  const bool lines = grid_ && zoom_ >= 6;
  for (uint32_t y = 0; y < cache_h_; ++y) {
    uint32_t* row = cache_.data() + uint64_t{y} * cache_w_;
    const uint32_t vy = y + static_cast<uint32_t>(scroll_y_), iy = vy / zoom_;
    for (uint32_t x = 0; x < cache_w_; ++x) {
      const uint32_t vx = x + static_cast<uint32_t>(scroll_x_), ix = vx / zoom_;
      uint32_t c = ix < width_ && iy < height_ ? pixels_[uint64_t{iy} * width_ + ix] : kBackground;
      if (lines && ix < width_ && iy < height_ && (vx % zoom_ == 0 || vy % zoom_ == 0)) c = ((c >> 1) & 0x7F7F7F7Fu) | 0xFF404040u;
      row[x] = c;
    }
  }
}

void DrawingCanvas::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, kBackground);
  painter.bevel(all, true);
  if (cache_dirty_ || cache_.empty()) {
    rebuild_cache();
    cache_dirty_ = false;
  }
  if (!cache_.empty()) {
    graphics::Surface s;
    s.pixels = cache_;
    s.width = cache_w_;
    s.height = cache_h_;
    s.pitch = cache_w_;
    const Rect2D v = viewport();
    painter.image(s, {0, 0, cache_w_, cache_h_}, v.x, v.y);
  }
  if (sel_valid_ && sel_w_ > 0 && sel_h_ > 0) {  // the selection: a dashed outline
    const Rect2D v = viewport();
    painter.push_frame(v);
    const int32_t x0 = (sel_x_ * static_cast<int32_t>(zoom_) - scroll_x_), y0 = (sel_y_ * static_cast<int32_t>(zoom_) - scroll_y_);
    const int32_t x1 = x0 + static_cast<int32_t>(sel_w_ * zoom_) - 1, y1 = y0 + static_cast<int32_t>(sel_h_ * zoom_) - 1;
    auto dot = [&](int32_t x, int32_t y, int32_t i) { painter.fill({x, y, 1, 1}, (i / 3) % 2 ? 0xFFFFFFFF : 0xFF000000); };
    const int32_t span_x = x1 - x0 < 4096 ? x1 - x0 : 4096, span_y = y1 - y0 < 4096 ? y1 - y0 : 4096;
    for (int32_t i = 0; i <= span_x; ++i) {
      dot(x0 + i, y0, i);
      dot(x0 + i, y1, i);
    }
    for (int32_t i = 0; i <= span_y; ++i) {
      dot(x0, y0 + i, i);
      dot(x1, y0 + i, i);
    }
    painter.pop_frame();
  }
  if (show_v_ && show_h_)
    painter.fill({static_cast<int32_t>(bounds_.width) - kBorder - ScrollBar::kThickness, static_cast<int32_t>(bounds_.height) - kBorder - ScrollBar::kThickness,
                  static_cast<uint32_t>(ScrollBar::kThickness), static_cast<uint32_t>(ScrollBar::kThickness)}, t.face);
}

// --- Input ----------------------------------------------------------------------------------------

void DrawingCanvas::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left && args.button != MouseButton::Right) return;
  if (drawing_ || moving_ || selecting_) return;  // the other button went down mid-stroke
  begin_stroke(args);
}

void DrawingCanvas::on_mouse_move(const MouseArgs& args) {
  int32_t x, y;
  const bool inside = pointer_to_image(args, &x, &y);
  const int32_t px = inside ? x : -1, py = inside ? y : -1;
  if (px != pointer_x_ || py != pointer_y_) {
    pointer_x_ = px;
    pointer_y_ = py;
    fire(pointer_moved_);
  }
  if (drawing_ && (x != last_x_ || y != last_y_)) draw_to(x, y);
  if (moving_) {
    sel_x_ = x - grab_dx_;
    sel_y_ = y - grab_dy_;
    render_floating();
  } else if (selecting_) {
    drag_selection(x, y);
  }
}

void DrawingCanvas::on_mouse_up(const MouseArgs& args) {
  if (moving_) {
    moving_ = false;
    return;
  }
  if (selecting_) {
    selecting_ = false;
    if (sel_w_ <= 1 && sel_h_ <= 1) sel_valid_ = false;  // a click, not a drag
    invalidate();
    return;
  }
  if (!drawing_ || args.button != button_) return;
  int32_t x, y;
  pointer_to_image(args, &x, &y);
  if (x != last_x_ || y != last_y_) draw_to(x, y);
  drawing_ = false;
  finish_change();
}

void DrawingCanvas::on_mouse_leave() {
  if (pointer_x_ >= 0) {
    pointer_x_ = pointer_y_ = -1;
    fire(pointer_moved_);
  }
}

void DrawingCanvas::on_mouse_wheel(const MouseArgs& args) {
  scroll_y_ += args.wheel > 0 ? -48 : args.wheel < 0 ? 48 : 0;
  update_scroll_bars();
  cache_dirty_ = true;
  invalidate();
}

bool DrawingCanvas::on_key(const KeyArgs& args) {
  using namespace hardware;
  if (args.usage == key::kEscape) {
    if (drawing_) cancel_stroke();
    else if (floating_) cancel_floating();
    else if (sel_valid_) clear_selection();
    else return false;
    return true;
  }
  if (args.usage == key::kEnter && floating_) {
    commit_floating();
    return true;
  }
  if (args.usage == 0x4C && !drawing_) {  // Delete; with Shift: cut
    if (!sel_valid_) return false;
    if ((args.modifiers & modifier::kShift) && copy_selection()) delete_selection();
    else if (!(args.modifiers & modifier::kShift)) delete_selection();
    return true;
  }
  if (args.usage == 0x49 && !drawing_) {  // Insert: Ctrl copies, Shift pastes
    if (args.modifiers & modifier::kControl) copy_selection();
    else if (args.modifiers & modifier::kShift) paste();
    else return false;
    return true;
  }
  if ((args.modifiers & modifier::kControl) && !drawing_) {
    if (args.usage == key::kA) { select_all(); return true; }
    if (args.usage == key::kA + 2) { copy_selection(); return true; }          // C
    if (args.usage == key::kA + 23) { cut_selection(); return true; }          // X
    if (args.usage == key::kA + 21) { paste(); return true; }                  // V
  }
  if (args.modifiers & modifier::kControl) {
    if (args.usage == key::kA + 25 && !(args.modifiers & modifier::kShift)) { undo(); return true; }  // Z
    if (args.usage == key::kA + 24 || (args.usage == key::kA + 25)) { redo(); return true; }         // Y, Shift+Z
  }
  return false;
}

}  // namespace gui
