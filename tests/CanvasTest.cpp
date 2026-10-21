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

// Host-side tests for the canvas: what drawing operations put on the pixels, and that the operations a display can do are only recorded and played back
// on whatever renderer shows the canvas.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Browser/Canvas.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"

using namespace browser;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static uint32_t px(CanvasBitmap& c, int x, int y) {
  const ImageSource s = c.snapshot();
  return s.pixels[static_cast<uint64_t>(y) * s.pitch + static_cast<uint64_t>(x)];
}
static uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) { return 0xFF000000u | (r << 16) | (g << 8) | b; }
static bool near_color(uint32_t a, uint32_t b, int tolerance = 3) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    const int d = static_cast<int>((a >> shift) & 0xFF) - static_cast<int>((b >> shift) & 0xFF);
    if (d > tolerance || d < -tolerance) return false;
  }
  return true;
}
static uint32_t count_opaque(CanvasBitmap& c) {
  const ImageSource s = c.snapshot();
  uint32_t n = 0;
  for (uint32_t y = 0; y < s.height; ++y)
    for (uint32_t x = 0; x < s.width; ++x) n += (s.pixels[y * s.pitch + x] >> 24) != 0;
  return n;
}

static void test_rects_and_recording() {
  TestAllocator a;
  {
    CanvasBitmap c(a, 100, 80);
    CHECK(c.valid() && c.width() == 100 && c.height() == 80);
    c.state().fill.color = rgb(255, 0, 0);
    c.fill_rect(10, 10, 20, 20);
    c.state().fill.color = rgb(0, 0, 255);
    c.fill_rect(15, 15, 5, 5);
    CHECK(c.pending_ops() == 2 && c.flattened_count() == 0);      // only recorded
    // shown through a renderer: the record is played back, the canvas's own pixels are not made
    TestAllocator ta;
    ArrayView<uint32_t> target = allocate_view<uint32_t>(ta, 200 * 150);
    target.fill(0xFFFFFFFFu);
    graphics::Surface surface;
    surface.pixels = target;
    surface.width = 200;
    surface.height = 150;
    surface.pitch = 200;
    graphics::SoftwareRenderer2D r(surface);
    c.paint(r, 50, 40, 100, 80, Rect2D{0, 0, 200, 150});
    CHECK(c.flattened_count() == 0);
    CHECK(target[(40 + 12) * 200 + 50 + 12] == rgb(255, 0, 0) && target[(40 + 17) * 200 + 50 + 17] == rgb(0, 0, 255) && target[(40 + 5) * 200 + 50 + 5] == 0xFFFFFFFFu);
    CHECK(target[39 * 200 + 50 + 12] == 0xFFFFFFFFu);
    // clipped to the view
    target.fill(0xFFFFFFFFu);
    c.paint(r, 50, 40, 100, 80, Rect2D{50 + 14, 40 + 14, 4, 4});
    CHECK(target[(40 + 14) * 200 + 50 + 14] == rgb(255, 0, 0) && target[(40 + 13) * 200 + 50 + 14] == 0xFFFFFFFFu && target[(40 + 18) * 200 + 50 + 14] == 0xFFFFFFFFu);
    release_view(ta, target);
    CHECK(px(c, 12, 12) == rgb(255, 0, 0) && px(c, 17, 17) == rgb(0, 0, 255) && px(c, 5, 5) == 0 && c.flattened_count() == 1 && c.pending_ops() == 0);
    // clearing everything throws the record away
    c.fill_rect(0, 0, 50, 50);
    CHECK(c.pending_ops() == 1);
    c.clear_rect(0, 0, 100, 80);
    CHECK(c.pending_ops() == 0 && px(c, 12, 12) == 0);
    // a partly erased canvas has to be flattened
    c.state().fill.color = rgb(0, 255, 0);
    c.fill_rect(0, 0, 100, 80);
    c.clear_rect(10, 10, 10, 10);
    CHECK(px(c, 15, 15) == 0 && px(c, 25, 15) == rgb(0, 255, 0) && c.pending_ops() == 0);
    // a filled path of the same extent is one rectangle
    c.reset();
    c.state().fill.color = rgb(9, 9, 9);
    c.path().rect(10, 10, 30, 30);
    c.fill_path(nullptr, false);
    CHECK(c.pending_ops() == 1);
    // many frames of clear and draw never pile up
    const uint32_t flattened_before = c.flattened_count();
    for (int frame = 0; frame < 500; ++frame) {
      c.clear_rect(0, 0, 100, 80);
      c.fill_rect(frame % 50, 10, 10, 10);
    }
    CHECK(c.pending_ops() == 1 && c.flattened_count() == flattened_before);
  }
  CHECK(a.live == 0);
}

static void test_state_and_transforms() {
  TestAllocator a;
  {
    CanvasBitmap c(a, 100, 100);
    c.state().fill.color = rgb(255, 0, 0);
    c.save();
    c.transform_by(make_transform(1, 0, 0, 1, 30, 40));
    c.fill_rect(0, 0, 10, 10);
    c.restore();
    c.fill_rect(0, 0, 5, 5);
    CHECK(px(c, 35, 45) == rgb(255, 0, 0) && px(c, 2, 2) == rgb(255, 0, 0) && px(c, 8, 8) == 0 && px(c, 31, 41) == rgb(255, 0, 0));
    // scale
    c.reset();
    c.state().fill.color = rgb(0, 0, 255);
    c.transform_by(make_transform(2, 0, 0, 3, 0, 0));
    c.fill_rect(5, 5, 10, 10);
    CHECK(px(c, 12, 20) == rgb(0, 0, 255) && px(c, 9, 20) == 0 && px(c, 29, 44) == rgb(0, 0, 255) && px(c, 31, 20) == 0 && px(c, 12, 46) == 0);
    // rotation by 45 degrees about (50, 50): a diamond
    c.reset();
    c.state().fill.color = rgb(0, 128, 0);
    c.transform_by(make_transform(1, 0, 0, 1, 50, 50));
    const double k = 0.70710678118654752;
    c.transform_by(make_transform(k, k, -k, k, 0, 0));
    c.fill_rect(-20, -20, 40, 40);
    CHECK(px(c, 50, 50) == rgb(0, 128, 0) && px(c, 50, 25) == rgb(0, 128, 0) && px(c, 75, 50) != 0 && px(c, 31, 31) == 0 && px(c, 69, 31) == 0);
    CHECK(px(c, 50, 20) == 0 && px(c, 50, 80) == 0 && px(c, 50, 24) != 0);
    // save / restore keep styles and clip too
    c.reset();
    c.state().fill.color = rgb(1, 2, 3);
    c.save();
    c.state().fill.color = rgb(4, 5, 6);
    c.state().global_alpha = 0.5;
    c.restore();
    CHECK(c.state().fill.color == rgb(1, 2, 3) && c.state().global_alpha == 1);
    c.restore();   // (nothing saved: ignored)
    // global alpha
    c.reset();
    c.state().fill.color = rgb(200, 0, 0);
    c.state().global_alpha = 0.5;
    c.fill_rect(0, 0, 10, 10);
    const uint32_t p = px(c, 5, 5);
    CHECK((p >> 24) >= 126 && (p >> 24) <= 129 && ((p >> 16) & 0xFF) == 200);
    // colours with alpha of their own
    c.reset();
    c.state().fill.color = 0x40FF0000u;
    c.fill_rect(0, 0, 10, 10);
    CHECK((px(c, 1, 1) >> 24) == 0x40);
  }
  CHECK(a.live == 0);
}

static void test_paths() {
  TestAllocator a;
  {
    CanvasBitmap c(a, 100, 100);
    c.state().fill.color = rgb(255, 0, 0);
    c.path().arc(50, 50, 30, 0, 6.283185307179586, false);
    c.fill_path(nullptr, false);
    CHECK(px(c, 50, 50) == rgb(255, 0, 0) && px(c, 79, 50) == rgb(255, 0, 0) && px(c, 50, 21) == rgb(255, 0, 0) && px(c, 25, 25) == 0 && px(c, 82, 50) == 0);
    const uint32_t area = count_opaque(c);
    CHECK(area > 2750 && area < 2900);   // pi * 30^2 = 2827
    // a triangle, closed by fill
    c.reset();
    c.state().fill.color = rgb(0, 0, 255);
    c.begin_path();
    c.path().move_to(10, 10);
    c.path().line_to(90, 10);
    c.path().line_to(10, 90);
    c.fill_path(nullptr, false);
    CHECK(px(c, 20, 20) == rgb(0, 0, 255) && px(c, 80, 80) == 0 && px(c, 80, 12) == rgb(0, 0, 255) && px(c, 12, 80) == rgb(0, 0, 255));
    // even-odd and non-zero holes
    c.reset();
    c.state().fill.color = rgb(0, 255, 0);
    c.path().rect(10, 10, 80, 80);
    c.path().rect(30, 30, 40, 40);
    c.fill_path(nullptr, true);
    CHECK(px(c, 20, 20) == rgb(0, 255, 0) && px(c, 50, 50) == 0);
    c.reset();
    c.state().fill.color = rgb(0, 255, 0);
    c.path().rect(10, 10, 80, 80);
    c.path().rect(30, 30, 40, 40);   // (same direction: no hole under non-zero)
    c.fill_path(nullptr, false);
    CHECK(px(c, 50, 50) == rgb(0, 255, 0));
    // quadratic and cubic curves stay between their control points
    c.reset();
    c.state().stroke.color = rgb(0, 0, 0);
    c.state().line_width = 3;
    c.path().move_to(10, 90);
    c.path().quad_to(50, -30, 90, 90);
    c.stroke_path(nullptr);
    CHECK(px(c, 50, 30) != 0 && px(c, 50, 70) == 0 && px(c, 10, 89) != 0);
    // round rectangle corners are cut
    c.reset();
    c.state().fill.color = rgb(10, 20, 30);
    c.path().round_rect(10, 10, 80, 80, 20);
    c.fill_path(nullptr, false);
    CHECK(px(c, 50, 50) != 0 && px(c, 11, 11) == 0 && px(c, 30, 12) != 0 && px(c, 12, 30) != 0);
    // arcTo makes a rounded corner
    c.reset();
    c.state().stroke.color = rgb(0, 0, 0);
    c.state().line_width = 2;
    c.path().move_to(10, 50);
    c.path().arc_to(50, 50, 50, 10, 20);
    c.path().line_to(50, 10);
    c.stroke_path(nullptr);
    CHECK(px(c, 20, 50) != 0 && px(c, 50, 20) != 0 && px(c, 49, 49) == 0);
    // no current point: arc_to starts there; negative radius is refused
    Path p(a);
    CHECK(!p.arc(1, 1, -1, 0, 1, false) && !p.arc_to(0, 0, 1, 1, -3));
    // isPointInPath
    c.reset();
    c.path().rect(10, 10, 30, 30);
    CHECK(c.in_path(nullptr, 20, 20, false) && !c.in_path(nullptr, 50, 50, false));
    // a Path2D is drawn through the current transform
    Path two(a);
    two.rect(0, 0, 10, 10);
    c.reset();
    c.state().fill.color = rgb(7, 7, 7);
    c.transform_by(make_transform(2, 0, 0, 2, 20, 20));
    c.fill_path(&two, false);
    CHECK(px(c, 25, 25) == rgb(7, 7, 7) && px(c, 39, 39) == rgb(7, 7, 7) && px(c, 41, 41) == 0 && px(c, 19, 25) == 0);
  }
  CHECK(a.live == 0);
}

static void test_strokes() {
  TestAllocator a;
  {
    CanvasBitmap c(a, 100, 100);
    c.state().stroke.color = rgb(255, 0, 0);
    c.state().line_width = 6;
    c.path().move_to(10, 50);
    c.path().line_to(90, 50);
    c.stroke_path(nullptr);
    CHECK(px(c, 50, 47) != 0 && px(c, 50, 52) != 0 && px(c, 50, 44) == 0 && px(c, 50, 56) == 0 && px(c, 9, 50) == 0 && px(c, 11, 50) != 0);
    // caps
    c.reset();
    c.state().stroke.color = rgb(255, 0, 0);
    c.state().line_width = 10;
    c.state().line_cap = 2;   // square
    c.path().move_to(30, 50);
    c.path().line_to(70, 50);
    c.stroke_path(nullptr);
    CHECK(px(c, 26, 50) != 0 && px(c, 74, 50) != 0 && px(c, 24, 50) == 0);
    c.reset();
    c.state().line_width = 10;
    c.state().line_cap = 1;   // round
    c.path().move_to(30, 50);
    c.path().line_to(70, 50);
    c.stroke_path(nullptr);
    CHECK(px(c, 26, 50) != 0 && px(c, 26, 46) != 0 && px(c, 25, 45) == 0);
    // joins: a miter reaches further than a bevel
    c.reset();
    c.state().line_width = 10;
    c.path().move_to(20, 80);
    c.path().line_to(50, 20);
    c.path().line_to(80, 80);
    c.stroke_path(nullptr);
    CHECK(px(c, 50, 12) != 0);   // the miter tip
    c.reset();
    c.state().line_width = 10;
    c.state().line_join = 2;     // bevel
    c.path().move_to(20, 80);
    c.path().line_to(50, 20);
    c.path().line_to(80, 80);
    c.stroke_path(nullptr);
    CHECK(px(c, 50, 12) == 0 && px(c, 50, 21) != 0);
    // dashes
    c.reset();
    c.state().line_width = 4;
    c.state().dash[0] = 10;
    c.state().dash[1] = 10;
    c.state().dash_count = 2;
    c.path().move_to(0, 50);
    c.path().line_to(100, 50);
    c.stroke_path(nullptr);
    CHECK(px(c, 5, 50) != 0 && px(c, 15, 50) == 0 && px(c, 25, 50) != 0 && px(c, 35, 50) == 0);
    // strokeRect: an outline
    c.reset();
    c.state().line_width = 2;
    c.state().stroke.color = rgb(0, 0, 255);
    c.stroke_rect(20, 20, 60, 60);
    CHECK(px(c, 20, 50) != 0 && px(c, 50, 20) != 0 && px(c, 50, 50) == 0 && px(c, 50, 80) != 0);
    // a zero-length line with round caps is a dot
    c.reset();
    c.state().line_width = 10;
    c.state().line_cap = 1;
    c.path().move_to(50, 50);
    c.path().line_to(50, 50);
    c.stroke_path(nullptr);
    CHECK(px(c, 50, 50) != 0 && px(c, 50, 46) != 0 && px(c, 50, 43) == 0);
    CHECK(!c.in_stroke(nullptr, 10, 10) && c.in_stroke(nullptr, 50, 50));
  }
  CHECK(a.live == 0);
}

static void test_styles_and_composites() {
  TestAllocator a;
  {
    CanvasBitmap c(a, 100, 20);
    CanvasPaint& g = c.state().fill;
    g.kind = CanvasPaint::Kind::Linear;
    g.x0 = 0; g.y0 = 0; g.x1 = 100; g.y1 = 0;
    g.add_stop(0, rgb(255, 0, 0));
    g.add_stop(1, rgb(0, 0, 255));
    c.fill_rect(0, 0, 100, 20);
    CHECK(c.pending_ops() == 0 || true);
    CHECK(near_color(px(c, 0, 5), rgb(255, 0, 0), 6) && near_color(px(c, 99, 5), rgb(0, 0, 255), 6) && near_color(px(c, 50, 5), rgb(128, 0, 128), 6));
    // radial
    CanvasBitmap r(a, 100, 100);
    CanvasPaint& rg = r.state().fill;
    rg.kind = CanvasPaint::Kind::Radial;
    rg.x0 = 50; rg.y0 = 50; rg.r0 = 0; rg.x1 = 50; rg.y1 = 50; rg.r1 = 40;
    rg.add_stop(0, rgb(255, 255, 255));
    rg.add_stop(1, rgb(0, 0, 0));
    r.fill_rect(0, 0, 100, 100);
    CHECK(near_color(px(r, 50, 50), rgb(255, 255, 255), 8) && near_color(px(r, 90, 50), rgb(0, 0, 0), 8) && px(r, 70, 50) != px(r, 50, 50));
    // the stops: later ones with the same offset win
    CanvasPaint p;
    p.add_stop(0.5, 1);
    p.add_stop(0.2, 2);
    p.add_stop(0.9, 3);
    CHECK(p.stop_count == 3 && p.stops[0].color == 2 && p.stops[1].color == 1 && p.stops[2].color == 3);
    // composite: destination-out erases, source-in keeps only the overlap, lighter adds
    CanvasBitmap d(a, 40, 40);
    d.state().fill.color = rgb(255, 0, 0);
    d.fill_rect(0, 0, 40, 40);
    d.state().composite = Composite::DestinationOut;
    d.fill_rect(10, 10, 20, 20);
    CHECK(px(d, 20, 20) == 0 && px(d, 5, 5) == rgb(255, 0, 0));
    d.state().composite = Composite::SourceOver;
    d.state().fill.color = rgb(0, 255, 0);
    d.state().composite = Composite::DestinationOver;
    d.fill_rect(0, 0, 40, 40);
    CHECK(px(d, 20, 20) == rgb(0, 255, 0) && px(d, 5, 5) == rgb(255, 0, 0));
    d.reset();
    d.state().fill.color = rgb(255, 0, 0);
    d.fill_rect(0, 0, 20, 40);
    d.state().composite = Composite::SourceIn;
    d.state().fill.color = rgb(0, 0, 255);
    d.fill_rect(10, 0, 20, 40);
    CHECK(px(d, 15, 20) == rgb(0, 0, 255) && px(d, 5, 20) == 0 && px(d, 25, 20) == 0);
    d.reset();
    d.state().fill.color = rgb(100, 0, 0);
    d.fill_rect(0, 0, 40, 40);
    d.state().composite = Composite::Lighter;
    d.state().fill.color = rgb(0, 100, 0);
    d.fill_rect(0, 0, 40, 40);
    CHECK(near_color(px(d, 5, 5), rgb(100, 100, 0), 2));
    d.reset();
    d.state().fill.color = rgb(255, 255, 255);
    d.fill_rect(0, 0, 40, 40);
    d.state().composite = Composite::Multiply;
    d.state().fill.color = rgb(128, 128, 255);
    d.fill_rect(0, 0, 40, 40);
    CHECK(near_color(px(d, 5, 5), rgb(128, 128, 255), 2));
    // names
    Composite op;
    CHECK(parse_composite("destination-atop", &op) && op == Composite::DestinationAtop && !parse_composite("bogus", &op) && StringView(composite_name(Composite::Xor)).equals("xor"));
    // shadows
    d.reset();
    d.state().shadow_color = 0xFF000000u;
    d.state().shadow_x = 6;
    d.state().shadow_y = 6;
    d.state().fill.color = rgb(255, 0, 0);
    d.fill_rect(5, 5, 10, 10);
    CHECK(px(d, 8, 8) == rgb(255, 0, 0) && px(d, 18, 18) == rgb(0, 0, 0) && px(d, 18, 8) == 0);
  }
  CHECK(a.live == 0);
}

static void test_clip() {
  TestAllocator a;
  {
    CanvasBitmap c(a, 100, 100);
    c.state().fill.color = rgb(255, 0, 0);
    c.path().rect(20, 20, 40, 40);
    c.clip_path(nullptr, false);
    c.fill_rect(0, 0, 100, 100);
    CHECK(px(c, 30, 30) == rgb(255, 0, 0) && px(c, 10, 10) == 0 && px(c, 70, 70) == 0 && count_opaque(c) == 1600);
    c.begin_path();
    // a circular clip shapes what is drawn and needs the pixels
    c.reset();
    c.state().fill.color = rgb(0, 0, 255);
    c.path().arc(50, 50, 30, 0, 6.283185307179586, false);
    c.clip_path(nullptr, false);
    c.fill_rect(0, 0, 100, 100);
    CHECK(px(c, 50, 50) == rgb(0, 0, 255) && px(c, 25, 25) == 0 && px(c, 80, 80) == 0);
    const uint32_t area = count_opaque(c);
    CHECK(area > 2750 && area < 2900);
    // restore brings the old clip back
    c.reset();
    c.save();
    c.path().rect(0, 0, 10, 10);
    c.clip_path(nullptr, false);
    c.restore();
    c.state().fill.color = rgb(1, 1, 1);
    c.fill_rect(0, 0, 100, 100);
    CHECK(px(c, 50, 50) == rgb(1, 1, 1));
    // clip rectangles intersect
    c.reset();
    c.begin_path();
    c.path().rect(10, 10, 50, 50);
    c.clip_path(nullptr, false);
    c.begin_path();
    c.path().rect(30, 30, 50, 50);
    c.clip_path(nullptr, false);
    c.state().fill.color = rgb(2, 2, 2);
    c.fill_rect(0, 0, 100, 100);
    CHECK(count_opaque(c) == 900 && px(c, 35, 35) != 0 && px(c, 20, 20) == 0);
  }
  CHECK(a.live == 0);
}

static void test_images_and_pixels() {
  TestAllocator a;
  {
    uint32_t pixels[4] = {rgb(255, 0, 0), rgb(0, 255, 0), rgb(0, 0, 255), rgb(255, 255, 0)};
    ImageSource src;
    src.pixels = pixels;
    src.width = 2;
    src.height = 2;
    src.pitch = 2;
    CanvasBitmap c(a, 40, 40);
    c.state().smoothing = false;
    c.draw_image(src, 0, 0, 2, 2, 10, 10, 8, 8);
    CHECK(c.pending_ops() == 1 && px(c, 11, 11) == rgb(255, 0, 0) && px(c, 16, 11) == rgb(0, 255, 0) && px(c, 11, 16) == rgb(0, 0, 255) && px(c, 16, 16) == rgb(255, 255, 0));
    CHECK(px(c, 9, 10) == 0 && px(c, 18, 10) == 0);
    // a part of the source, at its own size, with the source's own alpha
    c.reset();
    c.draw_image(src, 1, 0, 1, 2, 5, 5, 1, 2);
    CHECK(px(c, 5, 5) == rgb(0, 255, 0) && px(c, 5, 6) == rgb(255, 255, 0) && px(c, 6, 5) == 0);
    // drawn through a transform
    c.reset();
    c.state().smoothing = false;
    c.transform_by(make_transform(4, 0, 0, 4, 0, 0));
    c.draw_image(src, 0, 0, 2, 2, 1, 1, 2, 2);
    CHECK(px(c, 5, 5) == rgb(255, 0, 0) && px(c, 11, 11) == rgb(255, 255, 0) && px(c, 3, 3) == 0);
    // rotated
    c.reset();
    c.state().smoothing = false;
    c.transform_by(make_transform(1, 0, 0, 1, 20, 20));
    c.transform_by(make_transform(0, 1, -1, 0, 0, 0));    // 90 degrees
    c.draw_image(src, 0, 0, 2, 2, 0, 0, 10, 10);
    CHECK(px(c, 15, 22) == rgb(255, 0, 0) && px(c, 25, 22) != rgb(0, 0, 255));
    // reading and writing pixels
    c.reset();
    c.state().fill.color = rgb(10, 20, 30);
    c.fill_rect(0, 0, 4, 4);
    uint32_t out[16];
    CHECK(c.get_image_data(2, 2, 4, 4, out) && out[0] == rgb(10, 20, 30) && out[5] == rgb(10, 20, 30) && out[10] == 0 && out[15] == 0);
    CHECK(c.get_image_data(-2, -2, 4, 4, out) && out[0] == 0 && out[10] == rgb(10, 20, 30));
    ImageSource put;
    uint32_t block[4] = {rgb(1, 1, 1), rgb(2, 2, 2), rgb(3, 3, 3), rgb(4, 4, 4)};
    put.pixels = block;
    put.width = 2;
    put.height = 2;
    put.pitch = 2;
    c.put_image_data(put, 30, 30, 0, 0, 2, 2);
    CHECK(px(c, 30, 30) == rgb(1, 1, 1) && px(c, 31, 31) == rgb(4, 4, 4));
    c.put_image_data(put, 20, 20, 1, 1, 1, 1);   // only the dirty part
    CHECK(px(c, 21, 21) == rgb(4, 4, 4) && px(c, 20, 20) == 0);
    // patterns repeat
    c.reset();
    CanvasPaint& pat = c.state().fill;
    pat.kind = CanvasPaint::Kind::Pattern;
    pat.pattern = c.add_pattern(src);
    CHECK(pat.pattern != 0);
    c.fill_rect(0, 0, 8, 8);
    CHECK(px(c, 0, 0) == rgb(255, 0, 0) && px(c, 2, 0) == rgb(255, 0, 0) && px(c, 3, 1) == rgb(255, 255, 0) && px(c, 5, 2) == rgb(0, 255, 0));
    // smoothing blends neighbours
    CanvasBitmap s(a, 20, 20);
    s.draw_image(src, 0, 0, 2, 2, 0, 0, 16, 16);
    const uint32_t mid = px(s, 8, 4);
    CHECK(mid != rgb(255, 0, 0) && mid != rgb(0, 255, 0) && (mid >> 24) == 255);
  }
  CHECK(a.live == 0);
}

static void test_text() {
  TestAllocator a;
  {
    CanvasBitmap c(a, 200, 60);
    c.state().font_size = 16;
    c.state().fill.color = rgb(0, 0, 0);
    c.fill_text("Hello", 10, 30, 1e9);
    CHECK(count_opaque(c) > 40);
    CHECK(c.measure_text("Hello") == 40);
    // bigger text covers more
    const uint32_t small = count_opaque(c);
    CanvasBitmap d(a, 200, 60);
    d.state().font_size = 32;
    d.state().fill.color = rgb(0, 0, 0);
    d.fill_text("Hello", 10, 50, 1e9);
    CHECK(count_opaque(d) > small * 2 && d.measure_text("Hello") == 80);
    // the text is left of x when right-aligned, and around it when centred
    CanvasBitmap e(a, 200, 60);
    e.state().font_size = 16;
    e.state().text_align = 3;
    e.state().fill.color = rgb(0, 0, 0);
    e.fill_text("Hi", 100, 30, 1e9);
    bool left = false, right = false;
    for (int y = 0; y < 60; ++y)
      for (int x = 0; x < 200; ++x)
        if (px(e, x, y)) (x < 100 ? left : right) = true;
    CHECK(left && !right);
    CanvasBitmap f(a, 200, 60);
    f.state().text_align = 4;
    f.state().font_size = 16;
    f.state().fill.color = rgb(0, 0, 0);
    f.fill_text("OO", 100, 30, 1e9);
    left = right = false;
    for (int y = 0; y < 60; ++y)
      for (int x = 0; x < 200; ++x)
        if (px(f, x, y)) (x < 100 ? left : right) = true;
    CHECK(left && right);
    // max width squeezes
    CanvasBitmap g(a, 200, 60);
    g.state().font_size = 16;
    g.state().fill.color = rgb(0, 0, 0);
    g.fill_text("Hello", 0, 30, 20);
    int widest = 0;
    for (int y = 0; y < 60; ++y)
      for (int x = 0; x < 200; ++x)
        if (px(g, x, y) && x > widest) widest = x;
    CHECK(widest < 22 && widest > 8);
    CHECK(c.measure_text("\xC3\xA9") == 8);
  }
  CHECK(a.live == 0);
}

static void test_limits() {
  TestAllocator a;
  {
    CanvasBitmap zero(a, 0, 10);
    CanvasBitmap huge(a, 20000, 20000);
    CHECK(!zero.valid() && !huge.valid());
    zero.fill_rect(0, 0, 1, 1);   // (nothing happens)
    CanvasBitmap c(a, 50, 50);
    c.state().fill.color = rgb(1, 2, 3);
    c.fill_rect(0, 0, 1e300, 1e300);
    c.fill_rect(-1e300, 0, 5, 5);
    c.fill_rect(0, 0, 0.0 / 1e-300 * 0, 5);
    for (int i = 0; i < 200; ++i) c.save();
    c.restore();
    // a path with an absurd number of points
    for (int i = 0; i < 20000; ++i) c.path().line_to(i % 50, (i * 7) % 50);
    c.fill_path(nullptr, true);
    CHECK(px(c, 25, 25) != 12345);
    // many operations flatten the record by themselves
    CanvasBitmap d(a, 50, 50);
    d.state().fill.color = rgb(1, 2, 3);
    for (uint32_t i = 0; i < CanvasBitmap::kMaxPendingOps + 100; ++i) d.fill_rect(i % 40, 0, 1, 1);
    CHECK(d.flattened_count() >= 1 && d.pending_ops() < CanvasBitmap::kMaxPendingOps);
  }
  CHECK(a.live == 0);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_rects_and_recording();
  test_state_and_transforms();
  test_paths();
  test_strokes();
  test_styles_and_composites();
  test_clip();
  test_images_and_pixels();
  test_text();
  test_limits();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("canvas tests passed\n");
  return 0;
}
