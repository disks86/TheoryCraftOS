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

#include "Gui/Painter.hpp"

namespace gui {

Painter::Painter(graphics::IRenderer2D& renderer, const Theme& theme) : renderer_(renderer), theme_(theme) {
  const graphics::Extent2D e = renderer.extent();
  frames_[0] = {0, 0, {0, 0, e.width, e.height}};
  renderer_.reset_clip();
}

void Painter::apply_clip() { renderer_.set_clip(frames_[depth_].clip); }

bool Painter::clip_empty() const { return frames_[depth_].clip.width == 0 || frames_[depth_].clip.height == 0; }

Rect2D Painter::absolute(const Rect2D& r) const {
  return {r.x + frames_[depth_].origin_x, r.y + frames_[depth_].origin_y, r.width, r.height};
}

void Painter::push_frame(const Rect2D& bounds) {
  if (depth_ + 1 >= kMaxDepth) return;  // absurdly deep nesting: draw into the current frame
  const Frame& cur = frames_[depth_];
  Frame next;
  next.origin_x = cur.origin_x + bounds.x;
  next.origin_y = cur.origin_y + bounds.y;
  next.clip = intersect(cur.clip, {next.origin_x, next.origin_y, bounds.width, bounds.height});
  frames_[++depth_] = next;
  apply_clip();
}

void Painter::pop_frame() {
  if (depth_ == 0) return;
  --depth_;
  apply_clip();
}

void Painter::fill(const Rect2D& r, uint32_t color) { renderer_.fill_rect(absolute(r), color); }
void Painter::outline(const Rect2D& r, uint32_t color) { renderer_.draw_rect(absolute(r), color); }

void Painter::line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
  const Frame& f = frames_[depth_];
  renderer_.draw_line(x0 + f.origin_x, y0 + f.origin_y, x1 + f.origin_x, y1 + f.origin_y, color);
}

void Painter::bevel(const Rect2D& r, bool sunken) {
  if (r.width < 4 || r.height < 4) return;
  const uint32_t outer_tl = sunken ? theme_.shadow : theme_.highlight;
  const uint32_t outer_br = sunken ? theme_.highlight : theme_.dark;
  const uint32_t inner_tl = sunken ? theme_.dark : theme_.light;
  const uint32_t inner_br = sunken ? theme_.light : theme_.shadow;
  const int32_t w = static_cast<int32_t>(r.width), h = static_cast<int32_t>(r.height);
  auto edges = [&](int32_t inset, uint32_t tl, uint32_t br) {
    const int32_t x0 = r.x + inset, y0 = r.y + inset, x1 = r.x + w - 1 - inset, y1 = r.y + h - 1 - inset;
    line(x0, y0, x1, y0, tl);  // top
    line(x0, y0, x0, y1, tl);  // left
    line(x0, y1, x1, y1, br);  // bottom
    line(x1, y0, x1, y1, br);  // right
  };
  edges(0, outer_tl, outer_br);
  edges(1, inner_tl, inner_br);
}

void Painter::image(const graphics::Surface& source, const Rect2D& source_rect, int32_t x, int32_t y) {
  const Frame& f = frames_[depth_];
  renderer_.draw_image(source, source_rect, x + f.origin_x, y + f.origin_y);
}

bool Painter::device_image(graphics::IGraphicsDevice& owner, graphics::IImage& image, const Rect2D& source_rect, int32_t x, int32_t y) {
  const Frame& f = frames_[depth_];
  return renderer_.draw_device_image(owner, image, source_rect, x + f.origin_x, y + f.origin_y);
}

void Painter::text(int32_t x, int32_t y, StringView s, uint32_t color) {
  // draw_text wants a C string: copy in pieces so any length works.
  char buffer[65];
  uint64_t done = 0;
  while (done < s.size()) {
    uint64_t n = s.size() - done;
    if (n > 64) n = 64;
    for (uint64_t i = 0; i < n; ++i) buffer[i] = s[done + i];
    buffer[n] = 0;
    const Frame& f = frames_[depth_];
    renderer_.draw_text(x + f.origin_x + static_cast<int32_t>(done) * kGlyphWidth, y + f.origin_y, buffer, color);
    done += n;
  }
}

void Painter::text_centered(const Rect2D& r, StringView s, uint32_t color) {
  const int32_t x = r.x + (static_cast<int32_t>(r.width) - text_width(s)) / 2;
  const int32_t y = r.y + (static_cast<int32_t>(r.height) - kGlyphHeight) / 2;
  text(x, y, s, color);
}

void Painter::focus_frame(const Rect2D& r) {
  const int32_t x0 = r.x, y0 = r.y, x1 = r.x + static_cast<int32_t>(r.width) - 1,
                y1 = r.y + static_cast<int32_t>(r.height) - 1;
  for (int32_t x = x0; x <= x1; x += 2) {  // dotted: every other pixel
    renderer_.put_pixel(x + frames_[depth_].origin_x, y0 + frames_[depth_].origin_y, theme_.text);
    renderer_.put_pixel(x + frames_[depth_].origin_x, y1 + frames_[depth_].origin_y, theme_.text);
  }
  for (int32_t y = y0; y <= y1; y += 2) {
    renderer_.put_pixel(x0 + frames_[depth_].origin_x, y + frames_[depth_].origin_y, theme_.text);
    renderer_.put_pixel(x1 + frames_[depth_].origin_x, y + frames_[depth_].origin_y, theme_.text);
  }
}

}  // namespace gui
